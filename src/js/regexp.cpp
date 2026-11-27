// RegExp objects (ECMA-262 22.2) and the fast routes String's pattern methods take for an untouched RegExp.
// The generic algorithms behind Symbol.match, matchAll, replace, search and split are written in JavaScript
// (builtins_js.h); the functions at the end of this file do the same work directly, and may only be used when
// regexpIsPristine says that no script could tell the difference.
#include <algorithm>
#include <unordered_map>

#include "builtins.h"
#include "regex.h"

namespace tb::js {

namespace {

struct JSRegExp : JSObject {
    std::shared_ptr<RegexProgram> prog;
    JSString* source = nullptr;    // [[OriginalSource]]
    JSString* flagText = nullptr;  // [[OriginalFlags]], in the order d g i m s u v y
    RegexFlags flags;
    bool unicodeSets = false;
    bool legacy = true;  // made by RegExp itself, not by a subclass: takes part in RegExp.$1 and friends, can be recompiled
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
    static thread_local std::unordered_map<std::u16string, std::shared_ptr<RegexProgram>> cache;  // one per agent
    return cache;
}

// RegExpInitialize (22.2.3.4) without the final write to lastIndex: parses the pattern and stores the result
void initRegExp(Vm& vm, JSRegExp* r, const std::u16string& src, const std::u16string& flagStr) {
    RegexFlags f;
    bool unicodeSets = false;
    for (char16_t c : flagStr) {
        bool* slot = c == 'g' ? &f.global : c == 'i' ? &f.ignoreCase : c == 'm' ? &f.multiline : c == 's' ? &f.dotAll : c == 'u' ? &f.unicode
                     : c == 'y' ? &f.sticky : c == 'd' ? &f.hasIndices : c == 'v' ? &unicodeSets : nullptr;
        if (!slot || *slot) vm.throwError(Err::SyntaxError, u"Invalid flags supplied to RegExp constructor '" + flagStr + u"'");
        *slot = true;
    }
    if (unicodeSets && f.unicode) vm.throwError(Err::SyntaxError, u"Invalid flags supplied to RegExp constructor '" + flagStr + u"'");
    if (unicodeSets) f.unicode = f.unicodeSets = true;
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
    r->prog = prog;
    r->source = vm.str(src);
    std::u16string canonical;
    for (char16_t c : std::u16string(u"dgimsuvy"))
        if (flagStr.find(c) != std::u16string::npos) canonical += c;
    r->flagText = vm.str(canonical);
    r->flags = f;
    r->unicodeSets = unicodeSets;
}
JSRegExp* makeRegExp(Vm& vm, const std::u16string& src, const std::u16string& flagStr, JSObject* proto) {
    JSRegExp* r = vm.alloc<JSRegExp>();
    r->proto = proto;
    r->realm = vm.currentRealm;
    r->add(PropKey::of(vm.a.lastIndex), Value::i32(0), PF_W);
    r->source = r->flagText = vm.a.empty;
    initRegExp(vm, r, src, flagStr);
    return r;
}

JSRegExp* thisRegExp(CallInfo& ci) {
    if (!isRegExp(ci.thisv)) ci.vm.throwError(Err::TypeError, "Receiver is not a RegExp");
    return (JSRegExp*)ci.thisv.asObject();
}
// Set(R, "lastIndex", v, true): lastIndex is always an own data property, but it can be made read-only
void setLastIndex(Vm& vm, JSRegExp* rx, int v) {
    Prop* p = rx->find(PropKey::of(vm.a.lastIndex));
    if (!(p->flags & PF_W)) vm.throwError(Err::TypeError, "Cannot assign to read-only property 'lastIndex'");
    p->val = Value::i32(v);
}
size_t advance(const JSRegExp* rx, const std::u16string& s, size_t pos) {
    if (rx->flags.unicode && pos + 1 < s.size() && s[pos] >= 0xD800 && s[pos] <= 0xDBFF && s[pos + 1] >= 0xDC00 && s[pos + 1] <= 0xDFFF) return pos + 2;
    return pos + 1;
}

// UpdateLegacyRegExpStaticProperties / InvalidateLegacyRegExpStaticProperties (the legacy RegExp features proposal)
void remember(Vm& vm, const JSRegExp* rx, JSString* str, const std::vector<int>& caps) {
    if (rx->realm != vm.currentRealm) return;  // a RegExp of another realm leaves this realm's record alone
    vm.regexpLegacyValid = rx->legacy;
    if (!rx->legacy) return;
    vm.regexpLastInput = V(str);
    vm.regexpLastCaps.assign(caps.begin(), caps.begin() + std::min<size_t>(caps.size(), 20));
}

// RegExpBuiltinExec (22.2.7.2) up to the match itself: reads lastIndex, and writes it for global and sticky
// expressions. False when there is no match.
bool execWithState(Vm& vm, JSRegExp* rx, JSString* str, std::vector<int>& caps) {
    Value li = rx->find(PropKey::of(vm.a.lastIndex))->val;
    double from;
    if (li.isInt()) {
        from = std::max(li.asInt(), 0);
    } else {  // ToLength, which may run a script's valueOf
        from = vm.toInteger(li);
        from = from <= 0 ? 0 : std::min(from, 9007199254740991.0);
    }
    bool stateful = rx->flags.global || rx->flags.sticky;
    if (!stateful) from = 0;
    if (from > (double)str->s.size()) {
        if (stateful) setLastIndex(vm, rx, 0);
        return false;
    }
    if (!regexExec(*rx->prog, str->s, (size_t)from, rx->flags.sticky, caps)) {
        if (stateful) setLastIndex(vm, rx, 0);
        return false;
    }
    if (stateful) setLastIndex(vm, rx, caps[1]);
    remember(vm, rx, str, caps);
    return true;
}

Value captureValue(Vm& vm, const std::u16string& s, const std::vector<int>& caps, size_t g) {
    int a = caps[g * 2], b = caps[g * 2 + 1];
    if (a < 0 || b < a) return Value::undef();
    return vm.strv(s.substr((size_t)a, (size_t)(b - a)));
}
Value indexPair(Vm& vm, const std::vector<int>& caps, size_t g) {
    int a = caps[g * 2], b = caps[g * 2 + 1];
    if (a < 0 || b < a) return Value::undef();
    JSArray* pair = vm.newArray(2);
    pair->elems[0] = Value::i32(a);
    pair->elems[1] = Value::i32(b);
    return V(pair);
}
// The groups object of a match, or of its indices array (pairs): a later group of the same name only replaces an
// earlier one that took no part in the match.
Value groupsObject(Vm& vm, JSRegExp* rx, const std::u16string& s, const std::vector<int>& caps, bool pairs = false) {
    auto& names = regexGroupNames(*rx->prog);
    if (names.empty()) return Value::undef();
    JSObject* groups = vm.newObject(nullptr);
    for (auto& n : names) {
        Value v = pairs ? indexPair(vm, caps, (size_t)n.second) : captureValue(vm, s, caps, (size_t)n.second);
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
    if (rx->flags.hasIndices) {  // MakeMatchIndicesIndexPairArray
        JSArray* indices = vm.newArray(n);
        for (size_t g = 0; g < n; ++g) indices->elems[g] = indexPair(vm, caps, g);
        indices->add(PropKey::of(vm.atom("groups")), groupsObject(vm, rx, str->s, caps, true), PF_ALL);
        arr->add(PropKey::of(vm.atom("indices")), V(indices), PF_ALL);
    }
    return V(arr);
}
Value builtinExec(Vm& vm, JSRegExp* rx, JSString* s) {
    std::vector<int> caps;
    if (!execWithState(vm, rx, s, caps)) return Value::null();
    return matchArray(vm, rx, s, caps);
}
// RegExpExec (22.2.7.1): a script's own exec method comes first
Value genericExec(Vm& vm, Value rx, JSString* s) {
    Value exec = vm.get(rx, "exec");
    if (vm.isCallable(exec)) {
        Value arg = V(s);
        Value result = vm.call(exec, rx, &arg, 1);
        if (!result.isObject() && !result.isNull()) vm.throwError(Err::TypeError, "The exec method returned something that is neither an object nor null");
        return result;
    }
    if (!isRegExp(rx)) vm.throwError(Err::TypeError, "Receiver is not a RegExp");
    return builtinExec(vm, (JSRegExp*)rx.asObject(), s);
}

JSFN(regexpCtorFn) {  // 22.2.4.1
    Vm& vm = ci.vm;
    Value pattern = ci.arg(0), flags = ci.arg(1);
    bool patternIsRegExp = isRegExpLike(vm, pattern);
    Value newTarget = ci.newTarget;
    if (!ci.isConstruct()) {
        newTarget = V(ci.callee);
        if (patternIsRegExp && flags.isUndef() && vm.sameValue(newTarget, vm.get(pattern, vm.a.constructor))) return pattern;
    }
    Value source = pattern;
    if (isRegExp(pattern)) {
        JSRegExp* o = (JSRegExp*)pattern.asObject();
        source = V(o->source);
        if (flags.isUndef()) flags = V(o->flagText);
    } else if (patternIsRegExp) {
        source = vm.get(pattern, "source");
        if (flags.isUndef()) flags = vm.get(pattern, "flags");
    }
    JSObject* proto = vm.protoFromNewTarget(newTarget, vm.regexpProto);
    std::u16string src = source.isUndef() ? std::u16string() : vm.toStr16(source);
    std::u16string fl = flags.isUndef() ? std::u16string() : vm.toStr16(flags);
    JSRegExp* made = makeRegExp(vm, src, fl, proto);
    made->legacy = newTarget == vm.regexpCtor;
    return V(made);
}
JSFN(regexpCompile) {  // B.2.4.1
    Vm& vm = ci.vm;
    JSRegExp* rx = thisRegExp(ci);
    if (rx->realm != vm.currentRealm) vm.throwError(Err::TypeError, "RegExp.prototype.compile cannot be used on a RegExp of another realm");
    if (!rx->legacy) vm.throwError(Err::TypeError, "RegExp.prototype.compile cannot be used on an instance of a RegExp subclass");
    Value pattern = ci.arg(0), flags = ci.arg(1);
    std::u16string src, fl;
    if (isRegExp(pattern)) {
        if (!flags.isUndef()) vm.throwError(Err::TypeError, "Flags cannot be given when the pattern is a RegExp");
        src = ((JSRegExp*)pattern.asObject())->source->s;
        fl = ((JSRegExp*)pattern.asObject())->flagText->s;
    } else {
        if (!pattern.isUndef()) src = vm.toStr16(pattern);
        if (!flags.isUndef()) fl = vm.toStr16(flags);
    }
    initRegExp(vm, rx, src, fl);
    setLastIndex(vm, rx, 0);
    return ci.thisv;
}
// The legacy static accessors; data: 1 to 9 a capture, 10 input, 11 lastMatch, 12 lastParen, 13 leftContext, 14 rightContext
JSFN(regexpLegacyGet) {
    Vm& vm = ci.vm;
    if (ci.thisv != vm.regexpCtor) vm.throwError(Err::TypeError, "This property can only be read from the RegExp constructor itself");
    if (!vm.regexpLegacyValid) vm.throwError(Err::TypeError, "The last match was made by a RegExp subclass");
    int which = ci.callee->data.asInt();
    if (!vm.regexpLastInput.isString()) return V(vm.a.empty);
    if (which == 10) return vm.regexpLastInput;
    const std::u16string& s = vm.regexpLastInput.asString()->s;
    const std::vector<int>& caps = vm.regexpLastCaps;
    auto piece = [&](size_t g) {
        if (g * 2 + 1 >= caps.size() || caps[g * 2] < 0 || caps[g * 2 + 1] < caps[g * 2]) return V(vm.a.empty);
        return vm.strv(s.substr((size_t)caps[g * 2], (size_t)(caps[g * 2 + 1] - caps[g * 2])));
    };
    if (caps.size() < 2) return V(vm.a.empty);
    if (which <= 9) return piece((size_t)which);
    if (which == 11) return piece(0);
    if (which == 12) return caps.size() > 2 ? piece(caps.size() / 2 - 1) : V(vm.a.empty);
    if (which == 13) return vm.strv(s.substr(0, (size_t)caps[0]));
    return vm.strv(s.substr((size_t)caps[1]));
}
JSFN(regexpLegacySetInput) {
    Vm& vm = ci.vm;
    if (ci.thisv != vm.regexpCtor) vm.throwError(Err::TypeError, "This property can only be set on the RegExp constructor itself");
    vm.regexpLastInput = V(vm.toString(ci.arg(0)));
    return Value::undef();
}
JSFN(regexpExec) {
    Vm& vm = ci.vm;
    JSRegExp* rx = thisRegExp(ci);
    return builtinExec(vm, rx, vm.toString(ci.arg(0)));
}
JSFN(regexpTest) {
    Vm& vm = ci.vm;
    if (!ci.thisv.isObject()) vm.throwError(Err::TypeError, "RegExp.prototype.test called on a non-object");
    JSString* s = vm.toString(ci.arg(0));
    if (isRegExp(ci.thisv) && regexpIsPristine(vm, ci.thisv)) {
        std::vector<int> caps;
        return Value::boolean(execWithState(vm, (JSRegExp*)ci.thisv.asObject(), s, caps));
    }
    return Value::boolean(!genericExec(vm, ci.thisv, s).isNull());
}
// EscapeRegExpPattern (22.2.6.13.1)
std::u16string sourceText(JSRegExp* rx) {
    if (rx->source->s.empty()) return u"(?:)";
    std::u16string out;
    bool inClass = false;
    const std::u16string& s = rx->source->s;
    for (size_t i = 0; i < s.size(); ++i) {
        char16_t c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            out += c;
            c = s[++i];
            if (c == '\n') out += u'n';
            else if (c == '\r') out += u'r';
            else if (c == 0x2028) out += u"u2028";
            else if (c == 0x2029) out += u"u2029";
            else out += c;
            continue;
        }
        if (c == '[') inClass = true;
        else if (c == ']') inClass = false;
        if (c == '/' && !inClass) out += u"\\/";
        else if (c == '\n') out += u"\\n";
        else if (c == '\r') out += u"\\r";
        else if (c == 0x2028) out += u"\\u2028";
        else if (c == 0x2029) out += u"\\u2029";
        else out += c;
    }
    return out;
}
JSFN(regexpToString) {
    Vm& vm = ci.vm;
    if (!ci.thisv.isObject()) vm.throwError(Err::TypeError, "RegExp.prototype.toString called on a non-object");
    std::u16string pattern = vm.toStr16(vm.get(ci.thisv, "source"));
    std::u16string flags = vm.toStr16(vm.get(ci.thisv, "flags"));
    return vm.strv(u"/" + pattern + u"/" + flags);
}
const char* kFlagNames[] = {"hasIndices", "global", "ignoreCase", "multiline", "dotAll", "unicode", "unicodeSets", "sticky"};
const char16_t kFlagLetters[] = u"dgimsuvy";
JSFN(regexpFlagGetter) {  // RegExpHasFlag (22.2.6.4.1); data: index into kFlagNames
    Vm& vm = ci.vm;
    if (!isRegExp(ci.thisv)) {
        if (ci.thisv.isObject() && ci.thisv.asObject() == vm.regexpProto) return Value::undef();
        vm.throwError(Err::TypeError, "Receiver is not a RegExp");
    }
    JSRegExp* rx = (JSRegExp*)ci.thisv.asObject();
    return Value::boolean(rx->flagText->s.find(kFlagLetters[ci.callee->data.asInt()]) != std::u16string::npos);
}
JSFN(regexpFlagsGetter) {  // 22.2.6.4: built from the eight flag properties, whatever kind of object this is
    Vm& vm = ci.vm;
    if (!ci.thisv.isObject()) vm.throwError(Err::TypeError, "RegExp.prototype.flags getter called on a non-object");
    if (isRegExp(ci.thisv) && regexpIsPristine(vm, ci.thisv)) return V(((JSRegExp*)ci.thisv.asObject())->flagText);
    std::u16string out;
    for (int i = 0; i < 8; ++i)
        if (vm.toBool(vm.get(ci.thisv, kFlagNames[i]))) out += kFlagLetters[i];
    return vm.strv(std::move(out));
}
JSFN(regexpSourceGetter) {
    Vm& vm = ci.vm;
    if (!isRegExp(ci.thisv)) {
        if (ci.thisv.isObject() && ci.thisv.asObject() == vm.regexpProto) return vm.strv("(?:)");
        vm.throwError(Err::TypeError, "Receiver is not a RegExp");
    }
    return vm.strv(sourceText((JSRegExp*)ci.thisv.asObject()));
}
JSFN(regexpSpecies) { return ci.thisv; }
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

// IsRegExp (7.2.6): Symbol.match decides when it is present
bool isRegExpLike(Vm& vm, Value v) {
    if (!v.isObject()) return false;
    Value matcher = vm.get(v, PropKey::of(vm.symMatch));
    if (!matcher.isUndef()) return vm.toBool(matcher);
    return isRegExp(v);
}

// True when v (a RegExp object) and RegExp.prototype are exactly as the engine made them, apart from lastIndex
// holding a plain index: then the direct routes below cannot be told apart from the generic algorithms.
bool regexpIsPristine(Vm& vm, Value v) {
    JSObject* rx = v.asObject();
    if (rx->proto != vm.regexpProto || rx->props.size() != 1) return false;
    const Prop& li = rx->props[0];
    if (!(li.flags & PF_W) || !li.val.isInt() || li.val.asInt() < 0) return false;
    const std::vector<Prop>& props = vm.regexpProto->props;
    if (props.size() != vm.regexpWatch.size()) return false;
    for (size_t i = 0; i < props.size(); ++i) {
        const Vm::PropWatch& w = vm.regexpWatch[i];
        const Prop& p = props[i];
        if (!(p.key == w.key) || p.flags != w.flags) return false;
        uint64_t now = (p.flags & PF_ACC) ? ((Accessor*)p.val.asCell())->get.b : p.val.b;
        if (now != w.value) return false;
    }
    return true;
}
// Records what RegExp.prototype looks like once every built-in has been installed.
void regexpSnapshot(Vm& vm) {
    vm.regexpWatch.clear();
    for (const Prop& p : vm.regexpProto->props)
        vm.regexpWatch.push_back({p.key, (p.flags & PF_ACC) ? ((Accessor*)p.val.asCell())->get.b : p.val.b, p.flags});
}

Value regexpBuiltinExec(Vm& vm, Value rx, JSString* s) {
    if (!isRegExp(rx)) vm.throwError(Err::TypeError, "Receiver is not a RegExp");
    return builtinExec(vm, (JSRegExp*)rx.asObject(), s);
}

void setupRegExp(Vm& vm) {
    vm.regexpProto = vm.newObject();
    JSNative* c = makeCtor(vm, "RegExp", regexpCtorFn, 2, vm.regexpProto);
    vm.regexpCtor = V(c);
    vm.defFn(c, "escape", regexpEscape, 1);
    vm.defineAccessor(c, PropKey::of(vm.symSpecies), V(vm.newNative(regexpSpecies, "get [Symbol.species]", 0)), Value::undef(), PF_C);
    vm.defFn(vm.regexpProto, "exec", regexpExec, 1);
    vm.defFn(vm.regexpProto, "test", regexpTest, 1);
    vm.defFn(vm.regexpProto, "toString", regexpToString, 0);
    vm.defFn(vm.regexpProto, "compile", regexpCompile, 2);
    struct {
        const char* name;
        const char* alias;
        int which;
    } legacy[] = {{"input", "$_", 10}, {"lastMatch", "$&", 11}, {"lastParen", "$+", 12}, {"leftContext", "$`", 13}, {"rightContext", "$'", 14},
                  {"$1", nullptr, 1}, {"$2", nullptr, 2}, {"$3", nullptr, 3}, {"$4", nullptr, 4}, {"$5", nullptr, 5},
                  {"$6", nullptr, 6}, {"$7", nullptr, 7}, {"$8", nullptr, 8}, {"$9", nullptr, 9}};
    for (auto& l : legacy)
        for (const char* name : {l.name, l.alias}) {
            if (!name) continue;
            JSNative* get = vm.newNative(regexpLegacyGet, std::string("get ") + name, 0);
            get->data = Value::i32(l.which);
            Value set = l.which == 10 ? V(vm.newNative(regexpLegacySetInput, std::string("set ") + name, 1)) : Value::undef();
            vm.defineAccessor(c, PropKey::of(vm.atom(name)), V(get), set, PF_C);
        }
    auto getter = [&](const char* name, NativeFn fn, int data) {
        JSNative* g = vm.newNative(fn, std::string("get ") + name, 0);
        g->data = Value::i32(data);
        vm.defineAccessor(vm.regexpProto, PropKey::of(vm.atom(name)), V(g), Value::undef(), PF_C);
    };
    getter("source", regexpSourceGetter, 0);
    getter("flags", regexpFlagsGetter, 0);
    for (int i = 0; i < 8; ++i) getter(kFlagNames[i], regexpFlagGetter, i);
}

// ---------------------------------------------------------------------------- direct routes for an untouched RegExp
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
    std::vector<int> last;
    while (pos <= str.size() && regexExec(*rx->prog, str, pos, rx->flags.sticky, caps)) {
        out->elems.push_back(vm.strv(str.substr((size_t)caps[0], (size_t)(caps[1] - caps[0]))));
        pos = caps[1] == caps[0] ? advance(rx, str, (size_t)caps[1]) : (size_t)caps[1];
        last = caps;
    }
    setLastIndex(vm, rx, 0);
    if (!last.empty()) remember(vm, rx, s, last);
    return out->elems.empty() ? Value::null() : V(out);
}

Value regexpSearch(Vm& vm, Value rxv, JSString* s) {
    JSRegExp* rx = (JSRegExp*)rxv.asObject();
    std::vector<int> caps;
    if (!regexExec(*rx->prog, s->s, 0, rx->flags.sticky, caps)) return Value::i32(-1);
    remember(vm, rx, s, caps);
    return Value::i32(caps[0]);
}

Value regexpReplace(Vm& vm, Value rxv, JSString* s, Value replacement, bool all) {
    JSRegExp* rx = (JSRegExp*)rxv.asObject();
    bool global = rx->flags.global;
    if (all && !global) vm.throwError(Err::TypeError, "replaceAll must be called with a global RegExp");
    bool fn = vm.isCallable(replacement);
    std::u16string tpl = fn ? std::u16string() : vm.toStr16(replacement);
    const std::u16string& str = s->s;
    auto& names = regexGroupNames(*rx->prog);
    // every match is found before any replacement is worked out, since a replacement function may do anything
    std::vector<std::vector<int>> matches;
    std::vector<int> caps;
    if (global) {
        size_t pos = 0;
        while (pos <= str.size() && regexExec(*rx->prog, str, pos, rx->flags.sticky, caps)) {
            matches.push_back(caps);
            pos = caps[1] == caps[0] ? advance(rx, str, (size_t)caps[1]) : (size_t)caps[1];
        }
        setLastIndex(vm, rx, 0);
        if (!matches.empty()) remember(vm, rx, s, matches.back());
    } else if (execWithState(vm, rx, s, caps)) {
        matches.push_back(caps);
    }
    if (matches.empty()) return V(s);
    std::u16string out;
    size_t last = 0;
    for (const std::vector<int>& m : matches) {
        size_t ms = (size_t)m[0], me = (size_t)m[1];
        out.append(str, last, ms - last);
        if (fn) {
            std::vector<Value> args;
            RootScope roots(vm, args);
            for (size_t g = 0; g < m.size() / 2; ++g) args.push_back(captureValue(vm, str, m, g));
            args.push_back(Value::num((double)ms));
            args.push_back(V(s));
            if (!names.empty()) args.push_back(groupsObject(vm, rx, str, m));
            out += vm.toStr16(vm.call(replacement, Value::undef(), args.data(), (int)args.size()));
        } else
            out += expandReplacement(tpl, str, ms, me, &m, &names);
        last = me;
    }
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
