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

int runJsFile(const std::wstring& path, const std::wstring& outPath) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 2;
    std::string bytes;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) bytes.append(buf, n);
    CloseHandle(f);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) bytes.erase(0, 3);

    Runner r;
    g_runner = &r;
    r.out = outPath.empty() ? GetStdHandle(STD_OUTPUT_HANDLE)
                            : CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    Vm& vm = r.vm;
    r.roots.push_back(V(vm.newObject(nullptr)));
    RootScope keep(vm, r.roots);
    vm.onConsole = [&r](int level, const std::u16string& text) {
        if (level == 3 && text.rfind(u"Uncaught", 0) == 0) r.failed = true;
        r.write(text);
    };
    vm.defFn(vm.global, "setTimeout", setTimeoutFn, 2);
    vm.defFn(vm.global, "setInterval", setIntervalFn, 2);
    vm.defFn(vm.global, "clearTimeout", clearTimerFn, 1);
    vm.defFn(vm.global, "clearInterval", clearTimerFn, 1);
    vm.def(vm.global, "print", vm.get(vm.get(V(vm.global), "console"), "log"));

    std::string name = utf16to8(std::u16string(path.begin(), path.end()));
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    std::u16string err;
    if (!vm.run(utf8to16(bytes), name, nullptr, &err)) {
        r.write(err.rfind(u"Uncaught", 0) == 0 ? err : u"Uncaught " + err);
        r.failed = true;
    }

    while (!r.timers.empty()) {
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
    if (!outPath.empty() && r.out != INVALID_HANDLE_VALUE) CloseHandle(r.out);
    g_runner = nullptr;
    return r.failed ? 1 : 0;
}

}  // namespace tb::js
