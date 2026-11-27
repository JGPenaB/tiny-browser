// Command-line JavaScript runner: tinybrowser.exe --js file.js [output.txt]
// Gives scripts console output, timers and, for the conformance suite, other realms and other agents, so the
// engine can be tested without a page.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "builtins.h"

namespace tb::js {

namespace {
struct Timer {
    int id;
    double due, interval;  // interval < 0: fires once
};

// Agents, as test262's $262.agent describes them (INTERPRETING.md): other threads, each running a script in an
// engine of its own. The script that started them broadcasts SharedArrayBuffers to them, and they send text back.
struct Broadcast {
    std::shared_ptr<SharedBlock> block;
    double maxLen;
};
struct Agent {
    std::thread thread;
    Vm* vm = nullptr;             // while its engine exists
    std::deque<Broadcast> inbox;  // broadcasts it has not taken yet
    bool done = false;
};
struct AgentGroup {  // the agents one script started. `lock` guards everything in it, and the agents' fields.
    std::mutex lock;
    std::condition_variable changed;
    std::vector<std::unique_ptr<Agent>> agents;
    std::deque<std::u16string> reports;
    int untaken = 0;        // broadcasts still sitting in inboxes
    bool stopping = false;  // the script that started the agents has finished
    bool failed = false;    // an agent ended with an uncaught error
};

struct Runner {  // one running script: the main one, or an agent's
    Vm vm;
    std::vector<Timer> timers;
    std::vector<Value> roots;  // [0]: object mapping timer id -> [callback, args...]; [1]: an agent's broadcast callback
    RootScope keep{vm, roots};
    int nextId = 1;
    HANDLE out = INVALID_HANDLE_VALUE;
    bool failed = false;
    std::shared_ptr<AgentGroup> group;  // set once agents exist
    Agent* self = nullptr;              // the agent this script runs as; null for the main script
    bool leaving = false;               // an agent that has said it is done
    void write(const std::u16string& s) {
        std::string u = utf16to8(s);
        u += '\n';
        DWORD n = 0;
        if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, u.data(), (DWORD)u.size(), &n, nullptr);
    }
    JSObject* table() { return roots[0].asObject(); }
    void uncaught() {  // the pending exception goes to the output and fails the run
        Value e = vm.pending;
        vm.pending = Value::undef();
        if (vm.terminated) return;  // stopped from outside: whoever did that reports it
        std::u16string text = vm.describeError(e);
        write(text.rfind(u"Uncaught", 0) == 0 ? text : u"Uncaught " + text);
        failed = true;
    }
};
thread_local Runner* g_runner = nullptr;
// Milliseconds on a clock that never goes back and is fine enough to time a wait with.
double tick() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool pastDeadline(Vm& vm) {
    uint64_t deadline = vm.deadlineTick;
    return deadline && GetTickCount64() > deadline;
}

Value addTimer(CallInfo& ci, bool repeat) {
    Runner& r = *g_runner;
    Vm& vm = ci.vm;
    if (!vm.isCallable(ci.arg(0))) return Value::i32(0);
    double ms = ci.argc > 1 ? vm.toNumber(ci.args[1]) : 0;
    if (!(ms > 0)) ms = 0;
    JSArray* rec = vm.newArray();
    rec->elems.push_back(ci.arg(0));
    for (int i = 2; i < ci.argc; ++i) rec->elems.push_back(ci.args[i]);
    int id = r.nextId++;
    vm.defineOwn(r.table(), PropKey::index((uint32_t)id), V(rec), PF_ALL);
    r.timers.push_back({id, tick() + ms, repeat ? std::max(ms, 1.0) : -1.0});
    return Value::i32(id);
}
JSFN(setTimeoutFn) { return addTimer(ci, false); }
JSFN(setIntervalFn) { return addTimer(ci, true); }
JSFN(clearTimerFn) {
    Runner& r = *g_runner;
    int id = ci.vm.toInt32(ci.arg(0));
    r.timers.erase(std::remove_if(r.timers.begin(), r.timers.end(), [id](const Timer& t) { return t.id == id; }), r.timers.end());
    if (id > 0) ci.vm.del(r.table(), PropKey::index((uint32_t)id));
    return Value::undef();
}

// ---- the agent functions: __agentStart, __agentBroadcast and __agentGetReport for the script that starts agents,
// __agentReceive, __agentReport and __agentLeaving for the agents, __agentSleep and __agentNow for both
void agentMain(std::shared_ptr<AgentGroup> group, Agent* self, std::string source, HANDLE out, uint64_t deadline);
JSFN(agentStartFn) {  // (source): runs the script in a new agent, and returns once that agent's engine is up
    Runner& r = *g_runner;
    std::string source = utf16to8(ci.vm.toStr16(ci.arg(0)));
    if (!r.group) r.group = std::make_shared<AgentGroup>();
    AgentGroup& g = *r.group;
    std::unique_lock<std::mutex> lock(g.lock);
    g.agents.push_back(std::make_unique<Agent>());
    Agent* a = g.agents.back().get();
    a->thread = std::thread(agentMain, r.group, a, std::move(source), r.out, (uint64_t)ci.vm.deadlineTick);
    g.changed.wait(lock, [a] { return a->vm || a->done; });
    return Value::undef();
}
JSFN(agentBroadcastFn) {  // (sharedArrayBuffer): hands the buffer to every agent, and waits until each has taken it
    Vm& vm = ci.vm;
    Runner& r = *g_runner;
    Value v = ci.arg(0);
    JSArrayBuffer* b = v.isObject() && v.asObject()->cls == Cls::ArrayBuffer ? (JSArrayBuffer*)v.asObject() : nullptr;
    if (!b || !b->block) vm.throwError(Err::TypeError, "Only a SharedArrayBuffer can be broadcast");
    if (r.group) {
        AgentGroup& g = *r.group;
        std::unique_lock<std::mutex> lock(g.lock);
        for (auto& a : g.agents) {
            if (a->done || a.get() == r.self) continue;
            a->inbox.push_back({b->block, b->maxLen});
            ++g.untaken;
        }
        g.changed.notify_all();
        while (g.untaken > 0 && !pastDeadline(vm)) g.changed.wait_for(lock, std::chrono::milliseconds(20));
    }
    vm.pollInterrupt();
    return Value::undef();
}
JSFN(agentReceiveFn) {  // (callback): the function this agent's broadcasts are given to
    g_runner->roots[1] = ci.arg(0);
    return Value::undef();
}
JSFN(agentReportFn) {  // (value): text for the script that started the agents
    Runner& r = *g_runner;
    std::u16string text = ci.vm.toStr16(ci.arg(0));
    if (r.group) {
        std::lock_guard<std::mutex> lock(r.group->lock);
        r.group->reports.push_back(std::move(text));
    }
    return Value::undef();
}
JSFN(agentGetReportFn) {  // (): the oldest report nobody has read yet, or null
    Runner& r = *g_runner;
    std::u16string text;
    if (r.group) {
        std::lock_guard<std::mutex> lock(r.group->lock);
        if (r.group->reports.empty()) return Value::null();
        text = std::move(r.group->reports.front());
        r.group->reports.pop_front();
        return ci.vm.strv(std::move(text));
    }
    return Value::null();
}
JSFN(agentLeavingFn) {
    g_runner->leaving = true;
    return Value::undef();
}
JSFN(agentSleepFn) {  // (milliseconds)
    Vm& vm = ci.vm;
    double end = tick() + vm.toNumber(ci.arg(0));
    for (double left; (left = end - tick()) > 0 && !pastDeadline(vm);) Sleep((DWORD)std::min(left, 20.0));
    vm.pollInterrupt();
    return Value::undef();
}
JSFN(agentNowFn) { return Value::num(tick()); }
}  // namespace

static bool readFile(const std::wstring& path, std::string& bytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) bytes.append(buf, n);
    CloseHandle(f);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) bytes.erase(0, 3);
    return true;
}

static Value evalScriptFn(CallInfo& ci) {  // __evalScript(code): runs the text as a script of its own (ScriptEvaluation, 16.1.6)
    return ci.vm.eval(ci.vm.toStr16(ci.arg(0)), "<script>");
}
static Value htmlDdaFn(CallInfo& ci) {  // __htmldda(): an object with [[IsHTMLDDA]], as test262's $262.IsHTMLDDA
    JSNative* n = ci.vm.newNative([](CallInfo&) { return Value::null(); }, "", 0);
    n->htmlDda = true;
    return Value::cell(n);
}
static Value canBlockFn(CallInfo& ci) {  // __canBlock(flag): may Atomics.wait put this thread to sleep?
    ci.vm.canBlock = ci.vm.toBool(ci.arg(0));
    return Value::undef();
}
static Value readFileFn(CallInfo& ci) {  // __readFile(path): the text of a file, for loading modules from disk
    std::u16string wide = ci.vm.toStr16(ci.arg(0));
    std::string bytes;
    if (!readFile(std::wstring(wide.begin(), wide.end()), bytes)) ci.vm.throwError(Err::TypeError, "Cannot read %s", utf16to8(wide).c_str());
    return ci.vm.strv(utf8to16(bytes));
}
static Value createRealmFn(CallInfo& ci);
// What the shell adds to the global object of the running realm.
static void defineShellGlobals(Vm& vm) {
    vm.defFn(vm.global, "setTimeout", setTimeoutFn, 2);
    vm.defFn(vm.global, "setInterval", setIntervalFn, 2);
    vm.defFn(vm.global, "clearTimeout", clearTimerFn, 1);
    vm.defFn(vm.global, "clearInterval", clearTimerFn, 1);
    vm.def(vm.global, "print", vm.get(vm.get(V(vm.global), "console"), "log"));
    vm.defFn(vm.global, "__readFile", readFileFn, 1);
    vm.defFn(vm.global, "__canBlock", canBlockFn, 1);
    vm.defFn(vm.global, "__evalScript", evalScriptFn, 1);
    vm.defFn(vm.global, "__htmldda", htmlDdaFn, 0);
    vm.defFn(vm.global, "__createRealm", createRealmFn, 0);
    vm.defFn(vm.global, "__agentStart", agentStartFn, 1);
    vm.defFn(vm.global, "__agentBroadcast", agentBroadcastFn, 1);
    vm.defFn(vm.global, "__agentGetReport", agentGetReportFn, 0);
    vm.defFn(vm.global, "__agentReceive", agentReceiveFn, 1);
    vm.defFn(vm.global, "__agentReport", agentReportFn, 1);
    vm.defFn(vm.global, "__agentLeaving", agentLeavingFn, 0);
    vm.defFn(vm.global, "__agentSleep", agentSleepFn, 1);
    vm.defFn(vm.global, "__agentNow", agentNowFn, 0);
}
static Value createRealmFn(CallInfo& ci) {  // __createRealm(): the global object of a new realm, as test262's $262.createRealm needs
    Vm& vm = ci.vm;
    RealmScope scope(vm, vm.createRealm());
    defineShellGlobals(vm);
    return V(vm.global);
}

// Makes `r` the script that runs on this thread, with its console and the shell's functions.
static void prepare(Runner& r, HANDLE out) {
    g_runner = &r;
    r.out = out;
    Vm& vm = r.vm;
    r.roots.push_back(V(vm.newObject(nullptr)));
    r.roots.push_back(Value::undef());
    vm.onConsole = [&r](int level, const std::u16string& text) {
        // a rejection nobody handled is reported, but it does not make the script fail
        if (level == 3 && text.rfind(u"Uncaught", 0) == 0 && text.rfind(u"Uncaught (in promise)", 0) != 0) r.failed = true;
        r.write(text);
    };
    defineShellGlobals(vm);
    vm.canBlock = true;
}

// Is this script waiting for something that only another agent can bring about?
static bool expectsOthers(Runner& r) {
    if (!r.group) return false;
    std::lock_guard<std::mutex> lock(r.group->lock);
    if (r.group->stopping) return false;
    if (r.self) return !r.leaving;  // an agent stays for broadcasts until it says it is leaving
    if (r.vm.asyncWaiters.empty()) return false;
    for (auto& a : r.group->agents)
        if (!a->done) return true;  // someone who could still notify
    return false;
}
// Takes what other agents have sent this one: notifications for its waitAsync promises, and one broadcast.
static void takeFromOthers(Runner& r) {
    Vm& vm = r.vm;
    settleNotifiedWaiters(vm);
    if (r.self && vm.isCallable(r.roots[1])) {
        Broadcast message;
        {
            std::lock_guard<std::mutex> lock(r.group->lock);
            if (r.self->inbox.empty()) return;
            message = r.self->inbox.front();
            r.self->inbox.pop_front();
            --r.group->untaken;
        }
        r.group->changed.notify_all();
        JSArrayBuffer* buffer = vm.alloc<JSArrayBuffer>();
        buffer->proto = vm.sharedBufferProto;
        buffer->shared = true;
        buffer->block = message.block;
        buffer->maxLen = message.maxLen;
        Value arg = V(buffer);
        try {
            vm.call(r.roots[1], Value::undef(), &arg, 1);
        } catch (JSThrow&) {
            r.uncaught();
        }
    }
}
// The event loop of one script: its timers, and what other agents send it. Ends when nothing more can happen.
static void runEvents(Runner& r, double limitMs, double started) {
    Vm& vm = r.vm;
    while (!vm.terminated) {
        if (limitMs > 0 && tick() - started > limitMs) break;
        if (r.group) {
            takeFromOthers(r);
            vm.runMicrotasks();
        }
        bool expecting = expectsOthers(r);
        if (r.timers.empty()) {
            if (!expecting) break;
            Sleep(1);
            if (pastDeadline(vm)) vm.terminated = true;
            continue;
        }
        size_t best = 0;
        for (size_t i = 1; i < r.timers.size(); ++i)
            if (r.timers[i].due < r.timers[best].due) best = i;
        Timer t = r.timers[best];
        double now = tick();
        if (t.due > now) {
            Sleep((DWORD)std::min(t.due - now, expecting ? 1.0 : 50.0));
            continue;
        }
        if (t.interval < 0) r.timers.erase(r.timers.begin() + best);
        else r.timers[best].due = now + t.interval;
        Prop* p = r.table()->find(PropKey::index((uint32_t)t.id));
        if (!p) continue;
        JSArray* rec = (JSArray*)p->val.asCell();
        Value fn = rec->elems[0];
        std::vector<Value> args(rec->elems.begin() + 1, rec->elems.end());
        RootScope argRoots(vm, args);
        args.push_back(fn);
        if (t.interval < 0) vm.del(r.table(), PropKey::index((uint32_t)t.id));
        try {
            vm.call(fn, Value::undef(), args.data(), (int)args.size() - 1);
        } catch (JSThrow&) {
            r.uncaught();
        }
        vm.runMicrotasks();
        if (vm.gc.shouldCollect()) vm.gc.collect();
    }
}

namespace {
// The thread of one agent: its script, then its event loop until it leaves or the script that started it ends.
void agentMain(std::shared_ptr<AgentGroup> group, Agent* self, std::string source, HANDLE out, uint64_t deadline) {
    {
        Runner r;
        r.group = group;
        r.self = self;
        prepare(r, out);
        Vm& vm = r.vm;
        vm.deadlineTick = deadline;
        {
            std::lock_guard<std::mutex> lock(group->lock);
            self->vm = &vm;
        }
        group->changed.notify_all();
        std::u16string err;
        if (!vm.run(utf8to16(source), "agent", nullptr, &err) && !vm.terminated) {
            r.write(u"Uncaught (in an agent) " + err);
            r.failed = true;
        }
        runEvents(r, 0, 0);
        std::lock_guard<std::mutex> lock(group->lock);
        self->vm = nullptr;  // before the engine goes away: stopAgents reaches into it under this lock
        self->done = true;
        group->untaken -= (int)self->inbox.size();
        self->inbox.clear();
        if (r.failed) group->failed = true;
    }
    g_runner = nullptr;
    group->changed.notify_all();
}
}  // namespace
// Ends the agents a finished script started: whatever they are doing is stopped, and their threads are waited for.
static void stopAgents(Runner& r) {
    if (!r.group) return;
    AgentGroup& g = *r.group;
    {
        std::lock_guard<std::mutex> lock(g.lock);
        g.stopping = true;
        for (auto& a : g.agents)
            if (a->vm) a->vm->deadlineTick = 1;  // long past: its script stops at the next check
    }
    g.changed.notify_all();
    for (auto& a : g.agents)
        if (a->thread.joinable()) a->thread.join();
    if (g.failed) r.failed = true;
}

// Runs one script in a fresh engine, with its timers, writing console output to `out`. limitMs > 0 stops a
// script that runs too long. Returns true when it failed with an uncaught error.
static bool runOne(const std::string& bytes, const std::string& name, HANDLE out, double limitMs) {
    Runner r;
    prepare(r, out);
    Vm& vm = r.vm;
    double started = tick();
    if (limitMs > 0) vm.deadlineTick = GetTickCount64() + (ULONGLONG)limitMs;

    std::u16string err;
    if (!vm.run(utf8to16(bytes), name, nullptr, &err)) {
        r.write(err.rfind(u"Uncaught", 0) == 0 ? err : u"Uncaught " + err);
        r.failed = true;
    }
    runEvents(r, limitMs, started);
    stopAgents(r);
    if (vm.terminated) {
        r.write(u"Uncaught TimeLimit: the script ran too long");
        r.failed = true;
    }
    g_runner = nullptr;
    return r.failed;
}

int runJsFile(const std::wstring& path, const std::wstring& outPath) {
    std::string bytes;
    if (!readFile(path, bytes)) return 2;
    HANDLE out = outPath.empty() ? GetStdHandle(STD_OUTPUT_HANDLE)
                                 : CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    std::string name = utf16to8(std::u16string(path.begin(), path.end()));
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    bool failed = runOne(bytes, name, out, 0);
    if (!outPath.empty() && out != INVALID_HANDLE_VALUE) CloseHandle(out);
    return failed ? 1 : 0;
}

// tinybrowser.exe --js-batch <list.txt> <output.txt>: runs every script named in the list (one path per line),
// each in a fresh engine with a time limit. The output has a "### <path>" line before each script's console
// output and a "### end <0|1>" line after it, so a crash shows which script was running.
int runJsBatch(const std::wstring& listPath, const std::wstring& outPath) {
    std::string list;
    if (!readFile(listPath, list)) return 2;
    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) return 2;
    auto put = [&](const std::string& line) {
        DWORD n = 0;
        WriteFile(out, line.data(), (DWORD)line.size(), &n, nullptr);
    };
    size_t pos = 0;
    while (pos < list.size()) {
        size_t nl = list.find('\n', pos);
        if (nl == std::string::npos) nl = list.size();
        std::string path = list.substr(pos, nl - pos);
        pos = nl + 1;
        while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
        if (path.empty()) continue;
        // "file<TAB>name": run the file under another name, the one its relative imports are resolved against
        std::string name;
        size_t tab = path.find('\t');
        if (tab != std::string::npos) {
            name = path.substr(tab + 1);
            path.resize(tab);
        }
        put("### " + path + "\n");
        std::string bytes;
        std::u16string wide = utf8to16(path);
        bool failed = true;
        if (readFile(std::wstring(wide.begin(), wide.end()), bytes)) {
            size_t slash = path.find_last_of("/\\");
            if (name.empty()) name = slash == std::string::npos ? path : path.substr(slash + 1);
            failed = runOne(bytes, name, out, 10000);
        } else
            put("Uncaught could not read the file\n");
        put(failed ? "### end 1\n" : "### end 0\n");
    }
    CloseHandle(out);
    return 0;
}

}  // namespace tb::js
