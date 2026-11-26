// RegExp objects and the String methods that take a regular expression.
#include <algorithm>
#include <unordered_map>

#include "builtins.h"
#include "regex.h"

namespace tb::js {

namespace {

struct JSRegExp : JSObject {
    std::shared_ptr<RegexProgram> prog;
    JSString* source = nullptr;
    JSString* flagText = nullptr;
    RegexFlags flags;
    JSRegExp() { cls = Cls::RegExp; }
    void trace(Gc& g) override {
        JSObject::trace(g);
        g.mark(source);
        g.mark(flagText);
    }
};

// Compiled programs are shared between RegExp objects with the same source and flags (a literal inside a
// loop or a function is evaluated again and again).
std::unordered_map<std::u16string, std::shared_ptr<RegexProgram>>& programCache() {
    static std::unordered_map<std::u16string, std::shared_ptr<RegexProgram>> cache;
    return cache;
}

JSRegExp* makeRegExp(Vm& vm, const std::u16string& src, const std::u16string& flagStr, JSObject* proto) {
    RegexFlags f;
    bool unicodeSets = false;
    for (char16_t c : flagStr) {
        bool* slot = c == 'g' ? &f.global : c == 'i' ? &f.ignoreCase : c == 'm' ? &f.multiline : c == 's' ? &f.dotAll : c == 'u' ? &f.unicode
                     : c == 'y' ? &f.sticky : c == 'd' ? &f.hasIndices : c == 'v' ? &unicodeSets : nullptr;
        if (!slot || *slot) vm.throwError(Err::SyntaxError, u"Invalid flags supplied to RegExp constructor '" + flagStr + u"'");
        *slot = true;
    }
    if (unicodeSets) f.unicode = true;
    std::u16string key = flagStr + u"/" + src;
    auto& cache = programCache();
    std::shared_ptr<RegexProgram> prog;
    auto it = cache.find(key);
    if (it != cache.end()) {
        prog = it->second;
    } else {
        std::string error;
        prog = regexCompile(src, f, error);
        if (!prog) vm.throwError(Err::SyntaxError, u"Invalid regular expression: /" + src + u"/: " + utf8to16(error));
        if (cache.size() > 2000) cache.clear();
        cache.emplace(std::move(key), prog);
    }
    JSRegExp* r = vm.alloc<JSRegExp>();
    r->proto = proto;
    r->prog = prog;
    r->source = vm.str(src);
    std::u16string canonical;
    for (char16_t c : std::u16string(u"dgimsuvy"))
        if (flagStr.find(c) != std::u16string::npos) canonical += c;
    r->flagText = vm.str(canonical);
    r->flags = f;
    r->add(PropKey::of(vm.a.lastIndex), Value::i32(0), PF_W);
    return r;
}

JSRegExp* thisRegExp(CallInfo& ci) {
    if (!isRegExp(ci.thisv)) ci.vm.throwError(Err::TypeError, "Receiver is not a RegExp");
    return (JSRegExp*)ci.thisv.asObject();
}
void setLastIndex(Vm& vm, JSRegExp* rx, int v) {
    if (Prop* p = rx->find(PropKey::of(vm.a.lastIndex))) p->val = Value::i32(v);
}
size_t advance(const JSRegExp* rx, const std::u16string& s, size_t pos) {
    if (rx->flags.unicode && pos + 1 < s.size() && s[pos] >= 0xD800 && s[pos] <= 0xDBFF && s[pos + 1] >= 0xDC00 && s[pos + 1] <= 0xDFFF) return pos + 2;
    return pos + 1;
}

// exec() semantics: honours and updates lastIndex for global and sticky expressions.
bool execWithState(Vm& vm, JSRegExp* rx, JSString* str, std::vector<int>& caps) {
    bool stateful = rx->flags.global || rx->flags.sticky;
    double from = 0;
    if (stateful) {
        Prop* p = rx->find(PropKey::of(vm.a.lastIndex));
        from = p ? vm.toInteger(p->val) : 0;
        if (from < 0) from = 0;
        if (from > (double)str->s.size()) {
            setLastIndex(vm, rx, 0);
            return false;
        }
    }
    if (!regexExec(*rx->prog, str->s, (size_t)from, rx->flags.sticky, caps)) {
        if (stateful) setLastIndex(vm, rx, 0);
        return false;
    }
    if (stateful) setLastIndex(vm, rx, caps[1]);
    return true;
}

Value captureValue(Vm& vm, const std::u16string& s, const std::vector<int>& caps, size_t g) {
    int a = caps[g * 2], b = caps[g * 2 + 1];
    if (a < 0 || b < a) return Value::undef();
    return vm.strv(s.substr((size_t)a, (size_t)(b - a)));
}
Value groupsObject(Vm& vm, JSRegExp* rx, const std::u16string& s, const std::vector<int>& caps) {
    auto& names = regexGroupNames(*rx->prog);
    if (names.empty()) return Value::undef();
    JSObject* groups = vm.newObject(nullptr);
    for (auto& n : names) {
        Value v = captureValue(vm, s, caps, (size_t)n.second);
        PropKey k = PropKey::of(vm.atom(std::u16string_view(n.first)));
        if (Prop* p = groups->find(k)) {
            if (!v.isUndef()) p->val = v;
        } else
            groups->add(k, v, PF_ALL);
    }
    return V(groups);
}
Value matchArray(Vm& vm, JSRegExp* rx, JSString* str, const std::vector<int>& caps) {
    size_t n = caps.size() / 2;
    JSArray* arr = vm.newArray(n);
    for (size_t g = 0; g < n; ++g) arr->elems[g] = captureValue(vm, str->s, caps, g);
    arr->add(PropKey::of(vm.a.index), Value::i32(caps[0]), PF_ALL);
    arr->add(PropKey::of(vm.a.input), V(str), PF_ALL);
    arr->add(PropKey::of(vm.atom("groups")), groupsObject(vm, rx, str->s, caps), PF_ALL);
    return V(arr);
}

JSFN(regexpCtorFn) {
    Vm& vm = ci.vm;
    Value pat = ci.arg(0), fl = ci.arg(1);
    std::u16string src, flags;
    if (isRegExp(pat)) {
        JSRegExp* o = (JSRegExp*)pat.asObject();
        if (!ci.isConstruct() && fl.isUndef()) return pat;
        src = o->source->s;
        flags = fl.isUndef() ? o->flagText->s : vm.toStr16(fl);
    } else {
        if (!pat.isUndef()) src = vm.toStr16(pat);
        if (!fl.isUndef()) flags = vm.toStr16(fl);
    }
    return V(makeRegExp(vm, src, flags, vm.protoFromNewTarget(ci.newTarget, vm.regexpProto)));
}
JSFN(regexpExec) {
    Vm& vm = ci.vm;
    JSRegExp* rx = thisRegExp(ci);
    JSString* s = vm.toString(ci.arg(0));
    std::vector<int> caps;
    if (!execWithState(vm, rx, s, caps)) return Value::null();
    return matchArray(vm, rx, s, caps);
}
JSFN(regexpTest) {
    Vm& vm = ci.vm;
    JSRegExp* rx = thisRegExp(ci);
    JSString* s = vm.toString(ci.arg(0));
    std::vector<int> caps;
    return Value::boolean(execWithState(vm, rx, s, caps));
}
std::u16string sourceText(JSRegExp* rx) {
    if (rx->source->s.empty()) return u"(?:)";
    std::u16string out;
    bool inClass = false;
    const std::u16string& s = rx->source->s;
    for (size_t i = 0; i < s.size(); ++i) {
        char16_t c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            out += c;
            out += s[++i];
            continue;
        }
        if (c == '[') inClass = true;
        else if (c == ']') inClass = false;
        if (c == '/' && !inClass) out += u"\\/";
        else if (c == '\n') out += u"\\n";
        else if (c == '\r') out += u"\\r";
        else out += c;
    }
    return out;
}
JSFN(regexpToString) {
    JSRegExp* rx = thisRegExp(ci);
    return ci.vm.strv(u"/" + sourceText(rx) + u"/" + rx->flagText->s);
}
JSFN(regexpGetter) {
    Vm& vm = ci.vm;
    int which = ci.callee->data.asInt();
    if (!isRegExp(ci.thisv)) {
        if (ci.thisv.isObject() && ci.thisv.asObject() == vm.regexpProto) return which == 0 ? vm.strv("(?:)") : which == 1 ? V(vm.a.empty) : Value::undef();
        vm.throwError(Err::TypeError, "Receiver is not a RegExp");
    }
    JSRegExp* rx = (JSRegExp*)ci.thisv.asObject();
    switch (which) {
        case 0: return vm.strv(sourceText(rx));
        case 1: return V(rx->flagText);
        case 2: return Value::boolean(rx->flags.global);
        case 3: return Value::boolean(rx->flags.ignoreCase);
        case 4: return Value::boolean(rx->flags.multiline);
        case 5: return Value::boolean(rx->flags.dotAll);
        case 6: return Value::boolean(rx->flags.unicode);
        case 7: return Value::boolean(rx->flags.sticky);
        default: return Value::boolean(rx->flags.hasIndices);
    }
}
JSFN(regexpEscape) {
    const std::u16string& s = ci.vm.toStr16(ci.arg(0));
    std::u16string out;
    for (char16_t c : s) {
        if (c < 128 && c && strchr("\\^$.*+?()[]{}|/-", (char)c)) out += u'\\';
        out += c;
    }
    return ci.vm.strv(std::move(out));
}

}  // namespace

bool isRegExp(Value v) { return v.isObject() && v.asObject()->cls == Cls::RegExp; }

void setupRegExp(Vm& vm) {
    vm.regexpProto = vm.newObject();
    JSNative* c = makeCtor(vm, "RegExp", regexpCtorFn, 2, vm.regexpProto);
    vm.regexpCtor = V(c);
    vm.defFn(c, "escape", regexpEscape, 1);
    vm.defFn(vm.regexpProto, "exec", regexpExec, 1);
    vm.defFn(vm.regexpProto, "test", regexpTest, 1);
    vm.defFn(vm.regexpProto, "toString", regexpToString, 0);
    const char* getters[] = {"source", "flags", "global", "ignoreCase", "multiline", "dotAll", "unicode", "sticky", "hasIndices"};
    for (int i = 0; i < 9; ++i) {
        JSNative* g = vm.newNative(regexpGetter, getters[i], 0);
        g->data = Value::i32(i);
        vm.defineAccessor(vm.regexpProto, PropKey::of(vm.atom(getters[i])), V(g), Value::undef(), PF_C);
    }
}

Value regexpMatch(Vm& vm, Value rxv, JSString* s) {
    JSRegExp* rx = (JSRegExp*)rxv.asObject();
    std::vector<int> caps;
    if (!rx->flags.global) {
        if (!execWithState(vm, rx, s, caps)) return Value::null();
        return matchArray(vm, rx, s, caps);
    }
    JSArray* out = vm.newArray();
    const std::u16string& str = s->s;
    size_t pos = 0;
    while (pos <= str.size() && regexExec(*rx->prog, str, pos, rx->flags.sticky, caps)) {
        out->elems.push_back(vm.strv(str.substr((size_t)caps[0], (size_t)(caps[1] - caps[0]))));
        pos = caps[1] == caps[0] ? advance(rx, str, (size_t)caps[1]) : (size_t)caps[1];
    }
    setLastIndex(vm, rx, 0);
    return out->elems.empty() ? Value::null() : V(out);
}

Value regexpMatchAll(Vm& vm, Value rxv, JSString* s) {
    JSRegExp* rx = (JSRegExp*)rxv.asObject();
    if (!rx->flags.global) vm.throwError(Err::TypeError, "String.prototype.matchAll called with a non-global RegExp argument");
    JSArray* all = vm.newArray();
    const std::u16string& str = s->s;
    std::vector<int> caps;
    size_t pos = 0;
    while (pos <= str.size() && regexExec(*rx->prog, str, pos, rx->flags.sticky, caps)) {
        all->elems.push_back(matchArray(vm, rx, s, caps));
        pos = caps[1] == caps[0] ? advance(rx, str, (size_t)caps[1]) : (size_t)caps[1];
    }
    return V(vm.newIter(Cls::ArrayIter, V(all), 0));
}

Value regexpSearch(Vm& vm, Value rxv, JSString* s) {
    JSRegExp* rx = (JSRegExp*)rxv.asObject();
    std::vector<int> caps;
    (void)vm;
    return Value::i32(regexExec(*rx->prog, s->s, 0, rx->flags.sticky, caps) ? caps[0] : -1);
}

Value regexpReplace(Vm& vm, Value rxv, JSString* s, Value replacement, bool all) {
    JSRegExp* rx = (JSRegExp*)rxv.asObject();
    bool global = rx->flags.global;
    if (all && !global) vm.throwError(Err::TypeError, "replaceAll must be called with a global RegExp");
    bool fn = vm.isCallable(replacement);
    std::u16string tpl = fn ? std::u16string() : vm.toStr16(replacement);
    const std::u16string& str = s->s;
    auto& names = regexGroupNames(*rx->prog);
    std::u16string out;
    std::vector<int> caps;
    size_t last = 0, pos = 0;
    bool any = false;
    for (;;) {
        bool hit = global ? (pos <= str.size() && regexExec(*rx->prog, str, pos, rx->flags.sticky, caps)) : execWithState(vm, rx, s, caps);
        if (!hit) break;
        any = true;
        size_t ms = (size_t)caps[0], me = (size_t)caps[1];
        out.append(str, last, ms - last);
        if (fn) {
            std::vector<Value> args;
            RootScope roots(vm, args);
            for (size_t g = 0; g < caps.size() / 2; ++g) args.push_back(captureValue(vm, str, caps, g));
            args.push_back(Value::num((double)ms));
            args.push_back(V(s));
            if (!names.empty()) args.push_back(groupsObject(vm, rx, str, caps));
            std::vector<int> saved = caps;  // the callback may run other regular expressions
            out += vm.toStr16(vm.call(replacement, Value::undef(), args.data(), (int)args.size()));
            caps = saved;
        } else
            out += expandReplacement(tpl, str, ms, me, &caps, &names);
        last = me;
        if (!global) break;
        pos = me == ms ? advance(rx, str, me) : me;
    }
    if (global) setLastIndex(vm, rx, 0);
    if (!any) return V(s);
    if (last < str.size()) out.append(str, last, std::u16string::npos);
    return vm.strv(std::move(out));
}

Value regexpSplit(Vm& vm, Value rxv, JSString* s, uint32_t limit) {
    JSRegExp* rx = (JSRegExp*)rxv.asObject();
    JSArray* out = vm.newArray();
    if (limit == 0) return V(out);
    const std::u16string& str = s->s;
    std::vector<int> caps;
    if (str.empty()) {
        if (!regexExec(*rx->prog, str, 0, true, caps)) out->elems.push_back(V(s));
        return V(out);
    }
    size_t p = 0, q = 0;
    while (q < str.size()) {
        if (!regexExec(*rx->prog, str, q, false, caps)) break;
        size_t ms = (size_t)caps[0], me = (size_t)caps[1];
        if (ms >= str.size()) break;
        if (me == p || me == ms) {  // an empty match splits between characters, never at the current start
            if (me == ms && ms > p) {
                out->elems.push_back(vm.strv(str.substr(p, ms - p)));
                if (out->elems.size() >= limit) return V(out);
                p = ms;
                for (size_t g = 1; g < caps.size() / 2; ++g) {
                    out->elems.push_back(captureValue(vm, str, caps, g));
                    if (out->elems.size() >= limit) return V(out);
                }
            }
            q = advance(rx, str, ms);
            continue;
        }
        out->elems.push_back(vm.strv(str.substr(p, ms - p)));
        if (out->elems.size() >= limit) return V(out);
        p = me;
        for (size_t g = 1; g < caps.size() / 2; ++g) {
            out->elems.push_back(captureValue(vm, str, caps, g));
            if (out->elems.size() >= limit) return V(out);
        }
        q = p;
    }
    out->elems.push_back(vm.strv(str.substr(p)));
    return V(out);
}

}  // namespace tb::js
