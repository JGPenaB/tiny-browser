// Command-line JavaScript runner: tinybrowser.exe --js file.js [output.txt]
// Gives scripts console output and timers so the engine can be tested without a page.
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "builtins.h"

namespace tb::js {

namespace {
struct Timer {
    int id;
    double due, interval;  // interval < 0: fires once
};
struct Runner {
    Vm vm;
    std::vector<Timer> timers;
    std::vector<Value> roots;  // [0]: object mapping timer id -> [callback, args...]
    int nextId = 1;
    HANDLE out = INVALID_HANDLE_VALUE;
    bool failed = false;
    void write(const std::u16string& s) {
        std::string u = utf16to8(s);
        u += '\n';
        DWORD n = 0;
        if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, u.data(), (DWORD)u.size(), &n, nullptr);
    }
    JSObject* table() { return roots[0].asObject(); }
};
Runner* g_runner = nullptr;
double tick() { return (double)GetTickCount64(); }

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

// Runs one script in a fresh engine, with its timers, writing console output to `out`. limitMs > 0 stops a
// script that runs too long. Returns true when it failed with an uncaught error.
static bool runOne(const std::string& bytes, const std::string& name, HANDLE out, double limitMs) {
    Runner r;
    g_runner = &r;
    r.out = out;
    Vm& vm = r.vm;
    r.roots.push_back(V(vm.newObject(nullptr)));
    RootScope keep(vm, r.roots);
    vm.onConsole = [&r](int level, const std::u16string& text) {
        // a rejection nobody handled is reported, but it does not make the script fail
        if (level == 3 && text.rfind(u"Uncaught", 0) == 0 && text.rfind(u"Uncaught (in promise)", 0) != 0) r.failed = true;
        r.write(text);
    };
    vm.defFn(vm.global, "setTimeout", setTimeoutFn, 2);
    vm.defFn(vm.global, "setInterval", setIntervalFn, 2);
    vm.defFn(vm.global, "clearTimeout", clearTimerFn, 1);
    vm.defFn(vm.global, "clearInterval", clearTimerFn, 1);
    vm.def(vm.global, "print", vm.get(vm.get(V(vm.global), "console"), "log"));
    vm.defFn(vm.global, "__readFile", readFileFn, 1);
    vm.defFn(vm.global, "__canBlock", canBlockFn, 1);
    vm.defFn(vm.global, "__evalScript", evalScriptFn, 1);
    double started = tick();
    if (limitMs > 0) vm.deadlineTick = GetTickCount64() + (ULONGLONG)limitMs;
    vm.canBlock = true;

    std::u16string err;
    if (!vm.run(utf8to16(bytes), name, nullptr, &err)) {
        r.write(err.rfind(u"Uncaught", 0) == 0 ? err : u"Uncaught " + err);
        r.failed = true;
    }

    while (!r.timers.empty() && !vm.terminated) {
        if (limitMs > 0 && tick() - started > limitMs) break;
        size_t best = 0;
        for (size_t i = 1; i < r.timers.size(); ++i)
            if (r.timers[i].due < r.timers[best].due) best = i;
        Timer t = r.timers[best];
        double now = tick();
        if (t.due > now) {
            Sleep((DWORD)std::min(t.due - now, 50.0));
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
            Value e = vm.pending;
            vm.pending = Value::undef();
            std::u16string text = vm.describeError(e);
            r.write(text.rfind(u"Uncaught", 0) == 0 ? text : u"Uncaught " + text);
            r.failed = true;
        }
        vm.runMicrotasks();
        if (vm.gc.shouldCollect()) vm.gc.collect();
    }
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
