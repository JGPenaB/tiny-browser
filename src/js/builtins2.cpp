// Standard library, part 2: String, JSON, Promise, Date, iterators, generators, Map/Set and friends.
#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "builtins.h"

namespace tb::js {

static void needFn2(Vm& vm, Value f) {
    if (!vm.isCallable(f)) vm.throwError(Err::TypeError, "%s is not a function", utf16to8(vm.inspect(f, 0)).c_str());
}
static double clampIndex(Vm& vm, Value v, double len, double def) {  // negative values count from the end
    if (v.isUndef()) return def;
    double d = vm.toInteger(v);
    if (d < 0) return std::max(0.0, d + len);
    return std::min(d, len);
}
static double clamp0(double d, double len) { return d < 0 ? 0 : d > len ? len : d; }

// ---------------------------------------------------------------------------- String
static JSString* thisStr(CallInfo& ci, const char* method) {
    Value t = ci.thisv;
    if (t.isString()) return t.asString();
    if (t.isNullish()) ci.vm.throwError(Err::TypeError, "String.prototype.%s called on null or undefined", method);
    if (t.isObject() && t.asObject()->cls == Cls::String) return ((JSPrimitive*)t.asObject())->prim.asString();
    return ci.vm.toString(t);
}

JSFN(stringCtor) {
    Vm& vm = ci.vm;
    Value v = ci.arg(0);
    JSString* s;
    if (ci.argc == 0) s = vm.a.empty;
    else if (v.isSymbol() && !ci.isConstruct()) s = vm.str(u"Symbol(" + (((Symbol*)v.asCell())->desc ? ((Symbol*)v.asCell())->desc->s : std::u16string()) + u")");
    else s = vm.toString(v);
    return ci.isConstruct() ? wrapPrimitive(ci, V(s), Cls::String, vm.stringProto) : V(s);
}
JSFN(strToString) { return V(thisStr(ci, "toString")); }
JSFN(strCharAt) {
    JSString* s = thisStr(ci, "charAt");
    double i = ci.vm.toInteger(ci.arg(0));
    if (i < 0 || i >= (double)s->s.size()) return V(ci.vm.a.empty);
    return ci.vm.strv(std::u16string(1, s->s[(size_t)i]));
}
JSFN(strCharCodeAt) {
    JSString* s = thisStr(ci, "charCodeAt");
    double i = ci.vm.toInteger(ci.arg(0));
    if (i < 0 || i >= (double)s->s.size()) return Value::dbl(NAN);
    return Value::i32(s->s[(size_t)i]);
}
JSFN(strCodePointAt) {
    JSString* s = thisStr(ci, "codePointAt");
    double d = ci.vm.toInteger(ci.arg(0));
    if (d < 0 || d >= (double)s->s.size()) return Value::undef();
    size_t i = (size_t)d;
    char16_t c = s->s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->s.size() && s->s[i + 1] >= 0xDC00 && s->s[i + 1] <= 0xDFFF)
        return Value::i32(0x10000 + ((c - 0xD800) << 10) + (s->s[i + 1] - 0xDC00));
    return Value::i32(c);
}
JSFN(strAt) {
    JSString* s = thisStr(ci, "at");
    double len = (double)s->s.size(), i = ci.vm.toInteger(ci.arg(0));
    if (i < 0) i += len;
    if (i < 0 || i >= len) return Value::undef();
    return ci.vm.strv(std::u16string(1, s->s[(size_t)i]));
}
JSFN(strIndexOf) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, "indexOf");
    JSString* pat = vm.toString(ci.arg(0));
    double from = clamp0(vm.toInteger(ci.arg(1)), (double)s->s.size());
    size_t r = s->s.find(pat->s, (size_t)from);
    return r == std::u16string::npos ? Value::i32(-1) : Value::num((double)r);
}
JSFN(strLastIndexOf) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, "lastIndexOf");
    JSString* pat = vm.toString(ci.arg(0));
    double n = vm.toNumber(ci.arg(1));
    size_t from = std::isnan(n) ? std::u16string::npos : (size_t)clamp0(std::trunc(n), (double)s->s.size());
    size_t r = s->s.rfind(pat->s, from);
    return r == std::u16string::npos ? Value::i32(-1) : Value::num((double)r);
}
JSFN(strIncludes) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, "includes");
    if (isRegExp(ci.arg(0))) vm.throwError(Err::TypeError, "First argument to String.prototype.includes must not be a regular expression");
    JSString* pat = vm.toString(ci.arg(0));
    double from = clamp0(vm.toInteger(ci.arg(1)), (double)s->s.size());
    return Value::boolean(s->s.find(pat->s, (size_t)from) != std::u16string::npos);
}
JSFN(strStartsWith) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, "startsWith");
    JSString* pat = vm.toString(ci.arg(0));
    size_t pos = (size_t)clamp0(vm.toInteger(ci.arg(1)), (double)s->s.size());
    return Value::boolean(pos + pat->s.size() <= s->s.size() && s->s.compare(pos, pat->s.size(), pat->s) == 0);
}
JSFN(strEndsWith) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, "endsWith");
    JSString* pat = vm.toString(ci.arg(0));
    size_t end = ci.arg(1).isUndef() ? s->s.size() : (size_t)clamp0(vm.toInteger(ci.arg(1)), (double)s->s.size());
    if (pat->s.size() > end) return Value::boolean(false);
    return Value::boolean(s->s.compare(end - pat->s.size(), pat->s.size(), pat->s) == 0);
}
JSFN(strSlice) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, "slice");
    double len = (double)s->s.size();
    size_t a = (size_t)clampIndex(vm, ci.arg(0), len, 0), b = (size_t)clampIndex(vm, ci.arg(1), len, len);
    if (a >= b) return V(vm.a.empty);
    if (a == 0 && b == s->s.size()) return V(s);
    return vm.strv(s->s.substr(a, b - a));
}
JSFN(strSubstring) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, "substring");
    double len = (double)s->s.size();
    size_t a = (size_t)clamp0(vm.toInteger(ci.arg(0)), len), b = ci.arg(1).isUndef() ? (size_t)len : (size_t)clamp0(vm.toInteger(ci.arg(1)), len);
    if (a > b) std::swap(a, b);
    return vm.strv(s->s.substr(a, b - a));
}
JSFN(strSubstr) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, "substr");
    double len = (double)s->s.size(), start = vm.toInteger(ci.arg(0));
    if (start < 0) start = std::max(0.0, len + start);
    start = std::min(start, len);
    double count = ci.arg(1).isUndef() ? len - start : clamp0(vm.toInteger(ci.arg(1)), len - start);
    return vm.strv(s->s.substr((size_t)start, (size_t)count));
}
static Value caseMap(CallInfo& ci, bool upper, const char* name) {
    JSString* s = thisStr(ci, name);
    const std::u16string& in = s->s;
    bool ascii = true, changes = false;
    for (char16_t c : in) {
        if (c > 127) {
            ascii = false;
            break;
        }
        if (upper ? (c >= 'a' && c <= 'z') : (c >= 'A' && c <= 'Z')) changes = true;
    }
    if (ascii) {
        if (!changes) return V(s);
        std::u16string out = in;
        for (auto& c : out) {
            if (upper && c >= 'a' && c <= 'z') c -= 32;
            else if (!upper && c >= 'A' && c <= 'Z') c += 32;
        }
        return ci.vm.strv(std::move(out));
    }
    DWORD flag = upper ? LCMAP_UPPERCASE : LCMAP_LOWERCASE;
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, flag, (LPCWSTR)in.data(), (int)in.size(), nullptr, 0, nullptr, nullptr, 0);
    std::u16string out(n > 0 ? n : 0, u'\0');
    if (n > 0) LCMapStringEx(LOCALE_NAME_INVARIANT, flag, (LPCWSTR)in.data(), (int)in.size(), (LPWSTR)out.data(), n, nullptr, nullptr, 0);
    return ci.vm.strv(std::move(out));
}
JSFN(strToLowerCase) { return caseMap(ci, false, "toLowerCase"); }
JSFN(strToUpperCase) { return caseMap(ci, true, "toUpperCase"); }
static Value trimImpl(CallInfo& ci, int mode, const char* name) {  // 1 start, 2 end, 3 both
    JSString* s = thisStr(ci, name);
    size_t a = 0, b = s->s.size();
    if (mode & 1)
        while (a < b && isJsSpace(s->s[a])) ++a;
    if (mode & 2)
        while (b > a && isJsSpace(s->s[b - 1])) --b;
    if (a == 0 && b == s->s.size()) return V(s);
    return ci.vm.strv(s->s.substr(a, b - a));
}
JSFN(strTrim) { return trimImpl(ci, 3, "trim"); }
JSFN(strTrimStart) { return trimImpl(ci, 1, "trimStart"); }
JSFN(strTrimEnd) { return trimImpl(ci, 2, "trimEnd"); }
static Value padImpl(CallInfo& ci, bool atStart, const char* name) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, name);
    double target = vm.toInteger(ci.arg(0));
    std::u16string fill = ci.arg(1).isUndef() ? std::u16string(u" ") : vm.toStr16(ci.arg(1));
    if (target <= (double)s->s.size() || fill.empty()) return V(s);
    if (target > 268435456.0) vm.throwError(Err::RangeError, "Invalid string length");
    size_t need = (size_t)target - s->s.size();
    std::u16string pad;
    while (pad.size() < need) pad += fill;
    pad.resize(need);
    return vm.strv(atStart ? pad + s->s : s->s + pad);
}
JSFN(strPadStart) { return padImpl(ci, true, "padStart"); }
JSFN(strPadEnd) { return padImpl(ci, false, "padEnd"); }
JSFN(strRepeat) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, "repeat");
    double n = vm.toInteger(ci.arg(0));
    if (n < 0 || !std::isfinite(n)) vm.throwError(Err::RangeError, "Invalid count value");
    if (n * (double)s->s.size() > 268435456.0) vm.throwError(Err::RangeError, "Invalid string length");
    std::u16string out;
    out.reserve((size_t)n * s->s.size());
    for (size_t i = 0; i < (size_t)n; ++i) out += s->s;
    return vm.strv(std::move(out));
}
JSFN(strConcat) {
    Vm& vm = ci.vm;
    std::u16string out = thisStr(ci, "concat")->s;
    for (int i = 0; i < ci.argc; ++i) out += vm.toStr16(ci.args[i]);
    return vm.strv(std::move(out));
}
JSFN(strSplit) {
    Vm& vm = ci.vm;
    JSString* s = thisStr(ci, "split");
    Value sep = ci.arg(0);
    uint32_t limit = ci.arg(1).isUndef() ? 0xFFFFFFFFu : vm.toUint32(ci.arg(1));
    if (isRegExp(sep)) return regexpSplit(vm, sep, s, limit);
    JSArray* out = vm.newArray();
    if (limit == 0) return V(out);
    if (sep.isUndef()) {
        out->elems.push_back(V(s));
        return V(out);
    }
    std::u16string d = vm.toStr16(sep);
    const std::u16string& str = s->s;
    if (d.empty()) {
        for (size_t i = 0; i < str.size() && out->elems.size() < limit; ++i) out->elems.push_back(vm.strv(std::u16string(1, str[i])));
        return V(out);
    }
    size_t from = 0;
    for (;;) {
        size_t p = str.find(d, from);
        if (p == std::u16string::npos) break;
        out->elems.push_back(vm.strv(str.substr(from, p - from)));
        if (out->elems.size() >= limit) return V(out);
        from = p + d.size();
    }
    out->elems.push_back(vm.strv(str.substr(from)));
    return V(out);
}

std::u16string expandReplacement(const std::u16string& tpl, const std::u16string& subject, size_t mStart, size_t mEnd,
                                 const std::vector<int>* caps, const std::vector<std::pair<std::u16string, int>>* names) {
    std::u16string out;
    size_t ngroups = caps ? caps->size() / 2 : 1;
    auto group = [&](size_t g) {
        int s = (*caps)[g * 2], e = (*caps)[g * 2 + 1];
        if (s >= 0 && e >= s) out.append(subject, (size_t)s, (size_t)(e - s));
    };
    for (size_t i = 0; i < tpl.size(); ++i) {
        char16_t c = tpl[i];
        if (c != '$' || i + 1 >= tpl.size()) {
            out += c;
            continue;
        }
        char16_t n = tpl[i + 1];
        if (n == '$') {
            out += u'$';
            ++i;
        } else if (n == '&') {
            out.append(subject, mStart, mEnd - mStart);
            ++i;
        } else if (n == '`') {
            out.append(subject, 0, mStart);
            ++i;
        } else if (n == '\'') {
            out.append(subject, mEnd, std::u16string::npos);
            ++i;
        } else if (n >= '0' && n <= '9') {
            size_t g = n - '0', used = 1;
            if (i + 2 < tpl.size() && tpl[i + 2] >= '0' && tpl[i + 2] <= '9') {
                size_t g2 = g * 10 + (tpl[i + 2] - '0');
                if (g2 >= 1 && g2 < ngroups) {
                    g = g2;
                    used = 2;
                }
            }
            if (g >= 1 && g < ngroups) {
                group(g);
                i += used;
            } else
                out += c;
        } else if (n == '<' && names && !names->empty()) {
            size_t close = tpl.find(u'>', i + 2);
            if (close == std::u16string::npos) {
                out += c;
                continue;
            }
            std::u16string name = tpl.substr(i + 2, close - i - 2);
            for (auto& p : *names)
                if (p.first == name) group((size_t)p.second);
            i = close;
        } else
            out += c;
    }
    return out;
}

static Value replaceImpl(CallInfo& ci, bool all) {
    Vm& vm = ci.vm;
    JSString* str = thisStr(ci, all ? "replaceAll" : "replace");
    Value pat = ci.arg(0), rep = ci.arg(1);
    if (isRegExp(pat)) return regexpReplace(vm, pat, str, rep, all);
    std::u16string pattern = vm.toStr16(pat);
    bool fn = vm.isCallable(rep);
    std::u16string repl = fn ? std::u16string() : vm.toStr16(rep);
    const std::u16string& s = str->s;
    std::u16string out;
    auto emit = [&](size_t pos) {
        if (fn) {
            Value args[3] = {vm.strv(pattern), Value::num((double)pos), V(str)};
            out += vm.toStr16(vm.call(rep, Value::undef(), args, 3));
        } else
            out += expandReplacement(repl, s, pos, pos + pattern.size(), nullptr, nullptr);
    };
    if (pattern.empty()) {
        emit(0);
        if (!all) return vm.strv(out + s);
        for (size_t i = 0; i < s.size(); ++i) {
            out += s[i];
            emit(i + 1);
        }
        return vm.strv(std::move(out));
    }
    size_t from = 0;
    for (;;) {
        size_t p = s.find(pattern, from);
        if (p == std::u16string::npos) break;
        out.append(s, from, p - from);
        emit(p);
        from = p + pattern.size();
        if (!all) break;
    }
    if (from == 0) return V(str);
    out.append(s, from, std::u16string::npos);
    return vm.strv(std::move(out));
}
JSFN(strReplace) { return replaceImpl(ci, false); }
JSFN(strReplaceAll) { return replaceImpl(ci, true); }

static Value toRegExp(Vm& vm, Value v, const char* flags) {
    if (isRegExp(v)) return v;
    Value args[2] = {v.isUndef() ? V(vm.a.empty) : V(vm.toString(v)), vm.strv(flags)};
    return vm.construct(vm.regexpCtor, args, 2, vm.regexpCtor);
}
JSFN(strMatch) {
    JSString* s = thisStr(ci, "match");
    return regexpMatch(ci.vm, toRegExp(ci.vm, ci.arg(0), ""), s);
}
JSFN(strMatchAll) {
    JSString* s = thisStr(ci, "matchAll");
    return regexpMatchAll(ci.vm, toRegExp(ci.vm, ci.arg(0), "g"), s);
}
JSFN(strSearch) {
    JSString* s = thisStr(ci, "search");
    return regexpSearch(ci.vm, toRegExp(ci.vm, ci.arg(0), ""), s);
}
JSFN(strLocaleCompare) {
    JSString* s = thisStr(ci, "localeCompare");
    JSString* o = ci.vm.toString(ci.arg(0));
    int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, 0, (LPCWSTR)s->s.data(), (int)s->s.size(), (LPCWSTR)o->s.data(), (int)o->s.size(), nullptr, nullptr, 0);
    if (r == 0) return Value::i32(s->s < o->s ? -1 : s->s > o->s ? 1 : 0);
    return Value::i32(r - 2);
}
JSFN(strNormalize) { return V(thisStr(ci, "normalize")); }
JSFN(strIterator) { return V(ci.vm.newIter(Cls::StringIter, V(thisStr(ci, "[Symbol.iterator]")), 0)); }
JSFN(strFromCharCode) {
    std::u16string out;
    for (int i = 0; i < ci.argc; ++i) out += (char16_t)(ci.vm.toUint32(ci.args[i]) & 0xFFFF);
    return ci.vm.strv(std::move(out));
}
JSFN(strFromCodePoint) {
    Vm& vm = ci.vm;
    std::u16string out;
    for (int i = 0; i < ci.argc; ++i) {
        double d = vm.toNumber(ci.args[i]);
        if (!(d >= 0 && d <= 0x10FFFF) || d != std::floor(d)) vm.throwError(Err::RangeError, "Invalid code point");
        uint32_t cp = (uint32_t)d;
        if (cp < 0x10000) {
            out += (char16_t)cp;
        } else {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        }
    }
    return vm.strv(std::move(out));
}
JSFN(strRaw) {
    Vm& vm = ci.vm;
    Value raw = vm.get(ci.arg(0), vm.a.raw);
    uint32_t n = vm.lengthOf(raw);
    std::u16string out;
    for (uint32_t i = 0; i < n; ++i) {
        out += vm.toStr16(vm.get(raw, PropKey::index(i)));
        if (i + 1 < n && (int)i + 1 < ci.argc) out += vm.toStr16(ci.args[i + 1]);
    }
    return vm.strv(std::move(out));
}

void setupString(Vm& vm) {
    JSNative* c = makeCtor(vm, "String", stringCtor, 1, vm.stringProto);
    vm.defFn(c, "fromCharCode", strFromCharCode, 1);
    vm.defFn(c, "fromCodePoint", strFromCodePoint, 1);
    vm.defFn(c, "raw", strRaw, 1);
    struct {
        const char* name;
        NativeFn fn;
        int len;
    } fns[] = {
        {"toString", strToString, 0}, {"valueOf", strToString, 0}, {"charAt", strCharAt, 1}, {"charCodeAt", strCharCodeAt, 1},
        {"codePointAt", strCodePointAt, 1}, {"at", strAt, 1}, {"indexOf", strIndexOf, 1}, {"lastIndexOf", strLastIndexOf, 1},
        {"includes", strIncludes, 1}, {"startsWith", strStartsWith, 1}, {"endsWith", strEndsWith, 1}, {"slice", strSlice, 2},
        {"substring", strSubstring, 2}, {"substr", strSubstr, 2}, {"toLowerCase", strToLowerCase, 0},
        {"toUpperCase", strToUpperCase, 0}, {"toLocaleLowerCase", strToLowerCase, 0}, {"toLocaleUpperCase", strToUpperCase, 0},
        {"trim", strTrim, 0}, {"trimStart", strTrimStart, 0}, {"trimEnd", strTrimEnd, 0}, {"trimLeft", strTrimStart, 0},
        {"trimRight", strTrimEnd, 0}, {"padStart", strPadStart, 2}, {"padEnd", strPadEnd, 2}, {"repeat", strRepeat, 1},
        {"concat", strConcat, 1}, {"split", strSplit, 2}, {"replace", strReplace, 2}, {"replaceAll", strReplaceAll, 2},
        {"match", strMatch, 1}, {"matchAll", strMatchAll, 1}, {"search", strSearch, 1}, {"localeCompare", strLocaleCompare, 1},
        {"normalize", strNormalize, 0},
    };
    for (auto& f : fns) vm.defFn(vm.stringProto, f.name, f.fn, f.len);
    defSym(vm, vm.stringProto, vm.symIterator, strIterator, "[Symbol.iterator]", 0);
}

// ---------------------------------------------------------------------------- JSON
struct JsonParser {
    Vm& vm;
    const std::u16string& s;
    size_t i = 0;
    int depth = 0;
    [[noreturn]] void fail() {
        if (i >= s.size()) vm.throwError(Err::SyntaxError, "Unexpected end of JSON input");
        vm.throwError(Err::SyntaxError, "Unexpected token '%s' in JSON at position %d", utf16to8(s.substr(i, 1)).c_str(), (int)i);
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool lit(std::u16string_view w) {
        if (s.compare(i, w.size(), w) != 0) return false;
        i += w.size();
        return true;
    }
    std::u16string string() {
        ++i;
        std::u16string out;
        for (;;) {
            if (i >= s.size()) fail();
            char16_t c = s[i++];
            if (c == '"') break;
            if (c < 0x20) {
                --i;
                fail();
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) fail();
            char16_t e = s[i++];
            switch (e) {
                case '"':
                case '\\':
                case '/': out += e; break;
                case 'b': out += u'\b'; break;
                case 'f': out += u'\f'; break;
                case 'n': out += u'\n'; break;
                case 'r': out += u'\r'; break;
                case 't': out += u'\t'; break;
                case 'u': {
                    unsigned v = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (i >= s.size()) fail();
                        char16_t h = s[i];
                        int d = h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : -1;
                        if (d < 0) fail();
                        v = v * 16 + d;
                        ++i;
                    }
                    out += (char16_t)v;
                    break;
                }
                default:
                    --i;
                    fail();
            }
        }
        return out;
    }
    Value number() {
        size_t st = i;
        auto digit = [&] { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
        if (s[i] == '-') ++i;
        if (i < s.size() && s[i] == '0') {
            ++i;
        } else if (digit()) {
            while (digit()) ++i;
        } else
            fail();
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (!digit()) fail();
            while (digit()) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (!digit()) fail();
            while (digit()) ++i;
        }
        if (i - st < 10) {  // short integers skip strtod
            bool neg = s[st] == '-', plain = true;
            int32_t v = 0;
            for (size_t k = st + (neg ? 1 : 0); k < i; ++k) {
                if (s[k] < '0' || s[k] > '9') {
                    plain = false;
                    break;
                }
                v = v * 10 + (s[k] - '0');
            }
            if (plain && !(neg && v == 0)) return Value::i32(neg ? -v : v);
        }
        std::string t;
        for (size_t k = st; k < i; ++k) t += (char)s[k];
        return Value::num(strtod(t.c_str(), nullptr));
    }
    Value value() {
        ws();
        if (i >= s.size()) fail();
        char16_t c = s[i];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return vm.strv(string());
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        if (lit(u"true")) return Value::boolean(true);
        if (lit(u"false")) return Value::boolean(false);
        if (lit(u"null")) return Value::null();
        fail();
    }
    Value array() {
        if (++depth > 1200) vm.throwError(Err::RangeError, "JSON nesting is too deep");
        ++i;
        JSArray* a = vm.newArray();
        ws();
        if (i < s.size() && s[i] == ']') {
            ++i;
        } else {
            for (;;) {
                a->elems.push_back(value());
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    break;
                }
                fail();
            }
        }
        --depth;
        return V(a);
    }
    Value object() {
        if (++depth > 1200) vm.throwError(Err::RangeError, "JSON nesting is too deep");
        ++i;
        JSObject* o = vm.newObject();
        ws();
        if (i < s.size() && s[i] == '}') {
            ++i;
        } else {
            for (;;) {
                ws();
                if (i >= s.size() || s[i] != '"') fail();
                std::u16string key = string();
                ws();
                if (i >= s.size() || s[i] != ':') fail();
                ++i;
                Value v = value();
                vm.defineOwn(o, vm.toKey(V(vm.atom(std::u16string_view(key)))), v, PF_ALL);
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    break;
                }
                fail();
            }
        }
        --depth;
        return V(o);
    }
};

static Value internalize(Vm& vm, Value holder, PropKey key, Value reviver) {
    Value val = vm.get(holder, key);
    if (val.isObject()) {
        JSObject* o = val.asObject();
        std::vector<PropKey> keys;
        vm.ownKeys(o, keys, true, false, true);
        for (PropKey k : keys) {
            Value nv = internalize(vm, val, k, reviver);
            if (nv.isUndef()) vm.del(o, k);
            else vm.defineOwn(o, k, nv, PF_ALL);
        }
    }
    Value args[2] = {vm.keyToValue(key), val};
    return vm.call(reviver, holder, args, 2);
}
JSFN(jsonParse) {
    Vm& vm = ci.vm;
    JSString* text = vm.toString(ci.arg(0));
    JsonParser p{vm, text->s};
    Value v = p.value();
    p.ws();
    if (p.i < text->s.size()) p.fail();
    if (vm.isCallable(ci.arg(1))) {
        JSObject* holder = vm.newObject();
        holder->add(PropKey::of(vm.a.empty), v, PF_ALL);
        return internalize(vm, V(holder), PropKey::of(vm.a.empty), ci.arg(1));
    }
    return v;
}

struct JsonWriter {
    Vm& vm;
    std::u16string out, gap;
    Value replacer;
    bool hasList = false;
    std::vector<PropKey> list;
    std::vector<JSObject*> stack;
    void quote(const std::u16string& s) {
        out += u'"';
        for (size_t i = 0; i < s.size(); ++i) {
            char16_t c = s[i];
            switch (c) {
                case '"': out += u"\\\""; break;
                case '\\': out += u"\\\\"; break;
                case '\b': out += u"\\b"; break;
                case '\f': out += u"\\f"; break;
                case '\n': out += u"\\n"; break;
                case '\r': out += u"\\r"; break;
                case '\t': out += u"\\t"; break;
                default: {
                    bool loneHigh = c >= 0xD800 && c <= 0xDBFF && !(i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF);
                    bool loneLow = c >= 0xDC00 && c <= 0xDFFF && !(i > 0 && s[i - 1] >= 0xD800 && s[i - 1] <= 0xDBFF);
                    if (c < 0x20 || loneHigh || loneLow) {
                        char b[8];
                        snprintf(b, sizeof b, "\\u%04x", (unsigned)c);
                        for (char* p = b; *p; ++p) out += (char16_t)*p;
                    } else
                        out += c;
                }
            }
        }
        out += u'"';
    }
    bool write(Value holder, PropKey key, Value v, const std::u16string& indent) {
        if (v.isObject()) {
            Value tj = vm.get(v, vm.a.toJSON);
            if (vm.isCallable(tj)) {
                Value k = vm.keyToValue(key);
                v = vm.call(tj, v, &k, 1);
            }
        }
        if (!replacer.isUndef()) {
            Value args[2] = {vm.keyToValue(key), v};
            v = vm.call(replacer, holder, args, 2);
        }
        if (v.isObject()) {
            JSObject* o = v.asObject();
            if (o->cls == Cls::Number) v = Value::num(vm.toNumber(v));
            else if (o->cls == Cls::String) v = V(vm.toString(v));
            else if (o->cls == Cls::Boolean) v = ((JSPrimitive*)o)->prim;
        }
        if (v.isNull()) {
            out += u"null";
            return true;
        }
        if (v.isBool()) {
            out += v.isTrue() ? u"true" : u"false";
            return true;
        }
        if (v.isNumber()) {
            double d = v.number();
            out += std::isfinite(d) ? vm.numberToString(d)->s : std::u16string(u"null");
            return true;
        }
        if (v.isString()) {
            quote(v.asString()->s);
            return true;
        }
        if (!v.isObject() || vm.isCallable(v)) return false;
        JSObject* o = v.asObject();
        for (JSObject* seen : stack)
            if (seen == o) vm.throwError(Err::TypeError, "Converting circular structure to JSON");
        if (stack.size() > 1200) vm.throwError(Err::RangeError, "JSON nesting is too deep");
        stack.push_back(o);
        std::u16string inner = indent + gap;
        if (vm.isArray(v)) {
            uint32_t n = vm.lengthOf(v);
            out += u'[';
            for (uint32_t i = 0; i < n; ++i) {
                if (i) out += u',';
                if (!gap.empty()) {
                    out += u'\n';
                    out += inner;
                }
                if (!write(v, PropKey::index(i), vm.get(v, PropKey::index(i)), inner)) out += u"null";
            }
            if (n && !gap.empty()) {
                out += u'\n';
                out += indent;
            }
            out += u']';
        } else {
            std::vector<PropKey> keys;
            if (hasList) keys = list;
            else vm.ownKeys(o, keys, true, false, true);
            out += u'{';
            bool any = false;
            for (PropKey k : keys) {
                Value pv = vm.getObj(o, k, v);
                size_t mark = out.size();
                if (any) out += u',';
                if (!gap.empty()) {
                    out += u'\n';
                    out += inner;
                }
                quote(vm.toStr16(vm.keyToValue(k)));
                out += gap.empty() ? u":" : u": ";
                if (write(v, k, pv, inner)) any = true;
                else out.resize(mark);
            }
            if (any && !gap.empty()) {
                out += u'\n';
                out += indent;
            }
            out += u'}';
        }
        stack.pop_back();
        return true;
    }
};
JSFN(jsonStringify) {
    Vm& vm = ci.vm;
    JsonWriter w{vm};
    Value rep = ci.arg(1), space = ci.arg(2);
    if (vm.isCallable(rep)) {
        w.replacer = rep;
    } else if (vm.isArray(rep)) {
        w.hasList = true;
        for (Value e : ((JSArray*)rep.asObject())->elems) {
            if (!e.isString() && !e.isNumber()) continue;
            PropKey k = vm.toKey(V(vm.toString(e)));
            if (std::find(w.list.begin(), w.list.end(), k) == w.list.end()) w.list.push_back(k);
        }
    }
    if (space.isObject() && (space.asObject()->cls == Cls::Number || space.asObject()->cls == Cls::String)) space = ((JSPrimitive*)space.asObject())->prim;
    if (space.isNumber()) w.gap.assign((size_t)std::min(10.0, std::max(0.0, std::trunc(space.number()))), u' ');
    else if (space.isString()) w.gap = space.asString()->s.substr(0, 10);
    JSObject* holder = vm.newObject();
    holder->add(PropKey::of(vm.a.empty), ci.arg(0), PF_ALL);
    if (!w.write(V(holder), PropKey::of(vm.a.empty), ci.arg(0), std::u16string())) return Value::undef();
    return vm.strv(std::move(w.out));
}
void setupJSON(Vm& vm) {
    JSObject* json = vm.newObject();
    vm.def(vm.global, "JSON", V(json));
    vm.defFn(json, "parse", jsonParse, 2);
    vm.defFn(json, "stringify", jsonStringify, 3);
    vm.defineOwn(json, PropKey::of(vm.symToStringTag), vm.strv("JSON"), PF_C);
}

// ---------------------------------------------------------------------------- Promise
static JSPromise* thisPromise(CallInfo& ci, const char* method) {
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != Cls::Promise)
        ci.vm.throwError(Err::TypeError, "Method Promise.prototype.%s called on incompatible receiver", method);
    return (JSPromise*)ci.thisv.asObject();
}
JSFN(promiseCtorFn) {
    Vm& vm = ci.vm;
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Promise constructor cannot be invoked without 'new'");
    Value ex = ci.arg(0);
    if (!vm.isCallable(ex)) vm.throwError(Err::TypeError, "Promise resolver %s is not a function", utf16to8(vm.inspect(ex, 0)).c_str());
    JSPromise* p = vm.newPromise();
    p->proto = vm.protoFromNewTarget(ci.newTarget, vm.promiseProto);
    Value fns[2];
    makeResolvingFunctions(vm, p, fns);
    try {
        vm.call(ex, Value::undef(), fns, 2);
    } catch (JSThrow&) {
        Value e = vm.pending;
        vm.pending = Value::undef();
        vm.call(fns[1], Value::undef(), &e, 1);
    }
    return V(p);
}
JSFN(promiseThenFn) { return V(ci.vm.promiseThen(thisPromise(ci, "then"), ci.arg(0), ci.arg(1), true)); }
JSFN(promiseCatchFn) {
    Value args[2] = {Value::undef(), ci.arg(0)};
    return ci.vm.invoke(ci.thisv, "then", args, 2);
}
JSFN(finallyValue) { return ci.callee->data; }
JSFN(finallyThrow) { ci.vm.throwValue(ci.callee->data); }
static Value finallyStep(CallInfo& ci, bool rejected) {
    Vm& vm = ci.vm;
    Value r = vm.call(ci.callee->data, Value::undef(), nullptr, 0);
    JSPromise* p = vm.promiseResolve(r);
    JSNative* k = vm.newNative(rejected ? finallyThrow : finallyValue, "", 0);
    k->data = ci.arg(0);
    return V(vm.promiseThen(p, V(k), Value::undef(), true));
}
JSFN(finallyOnFulfilled) { return finallyStep(ci, false); }
JSFN(finallyOnRejected) { return finallyStep(ci, true); }
JSFN(promiseFinallyFn) {
    Vm& vm = ci.vm;
    Value f = ci.arg(0);
    Value args[2] = {f, f};
    if (vm.isCallable(f)) {
        JSNative* onF = vm.newNative(finallyOnFulfilled, "", 1);
        onF->data = f;
        JSNative* onR = vm.newNative(finallyOnRejected, "", 1);
        onR->data = f;
        args[0] = V(onF);
        args[1] = V(onR);
    }
    return vm.invoke(ci.thisv, "then", args, 2);
}
JSFN(promiseResolveFn) { return V(ci.vm.promiseResolve(ci.arg(0))); }
JSFN(promiseRejectFn) {
    JSPromise* p = ci.vm.newPromise();
    ci.vm.rejectPromise(p, ci.arg(0));
    return V(p);
}
JSFN(promiseWithResolvers) {
    Vm& vm = ci.vm;
    JSPromise* p = vm.newPromise();
    Value fns[2];
    makeResolvingFunctions(vm, p, fns);
    JSObject* o = vm.newObject();
    vm.def(o, "promise", V(p), PF_ALL);
    vm.def(o, "resolve", fns[0], PF_ALL);
    vm.def(o, "reject", fns[1], PF_ALL);
    return V(o);
}

// Shared state of Promise.all / allSettled / any: [0] result values, [1] promises still pending, [2] the result promise.
static void comboDone(Vm& vm, JSObject* st, bool aggregateError) {
    Prop* left = st->find(PropKey::index(1));
    int n = left->val.asInt() - 1;
    left->val = Value::i32(n);
    if (n) return;
    JSPromise* result = (JSPromise*)st->find(PropKey::index(2))->val.asCell();
    Value values = st->find(PropKey::index(0))->val;
    if (!aggregateError) {
        vm.resolvePromise(result, values);
        return;
    }
    Value ctor = vm.get(V(vm.global), "AggregateError");
    Value args[2] = {values, vm.strv("All promises were rejected")};
    vm.rejectPromise(result, vm.construct(ctor, args, 2, ctor));
}
static Value comboElement(CallInfo& ci, int kind) {  // 0 all, 1 settled-fulfilled, 2 settled-rejected, 3 any-rejected
    Vm& vm = ci.vm;
    JSObject* st = ci.callee->data.asObject();
    uint32_t idx = (uint32_t)ci.callee->data2.asInt();
    JSArray* values = (JSArray*)st->find(PropKey::index(0))->val.asCell();
    Value v = ci.arg(0);
    if (kind == 1 || kind == 2) {
        JSObject* o = vm.newObject();
        vm.def(o, "status", vm.strv(kind == 1 ? "fulfilled" : "rejected"), PF_ALL);
        vm.def(o, kind == 1 ? "value" : "reason", v, PF_ALL);
        v = V(o);
    }
    if (idx < values->elems.size()) values->elems[idx] = v;
    comboDone(vm, st, kind == 3);
    return Value::undef();
}
JSFN(comboAllF) { return comboElement(ci, 0); }
JSFN(comboSettledF) { return comboElement(ci, 1); }
JSFN(comboSettledR) { return comboElement(ci, 2); }
JSFN(comboAnyR) { return comboElement(ci, 3); }

static Value promiseCombine(CallInfo& ci, int mode) {  // 0 all, 1 allSettled, 2 race, 3 any
    Vm& vm = ci.vm;
    JSPromise* result = vm.newPromise();
    Value fns[2];
    makeResolvingFunctions(vm, result, fns);
    JSArray* values = vm.newArray();
    JSObject* st = vm.newObject(nullptr);
    st->add(PropKey::index(0), V(values), PF_W);
    st->add(PropKey::index(1), Value::i32(1), PF_W);
    st->add(PropKey::index(2), V(result), PF_W);
    uint32_t idx = 0;
    try {
        vm.iterate(ci.arg(0), [&](Value item) {
            JSPromise* p = vm.promiseResolve(item);
            if (mode == 2) {
                vm.promiseThen(p, fns[0], fns[1], false);
                return true;
            }
            values->elems.push_back(Value::undef());
            Prop* left = st->find(PropKey::index(1));
            left->val = Value::i32(left->val.asInt() + 1);
            auto mk = [&](NativeFn f) {
                JSNative* n = vm.newNative(f, "", 1);
                n->data = V(st);
                n->data2 = Value::i32((int32_t)idx);
                return V(n);
            };
            Value onF = mode == 0 ? mk(comboAllF) : mode == 1 ? mk(comboSettledF) : fns[0];
            Value onR = mode == 0 ? fns[1] : mode == 1 ? mk(comboSettledR) : mk(comboAnyR);
            vm.promiseThen(p, onF, onR, false);
            ++idx;
            return true;
        });
    } catch (JSThrow&) {
        Value e = vm.pending;
        vm.pending = Value::undef();
        vm.call(fns[1], Value::undef(), &e, 1);
        return V(result);
    }
    if (mode != 2) comboDone(vm, st, mode == 3);
    return V(result);
}
JSFN(promiseAll) { return promiseCombine(ci, 0); }
JSFN(promiseAllSettled) { return promiseCombine(ci, 1); }
JSFN(promiseRace) { return promiseCombine(ci, 2); }
JSFN(promiseAny) { return promiseCombine(ci, 3); }

void setupPromise(Vm& vm) {
    vm.promiseProto = vm.newObject();
    JSNative* c = makeCtor(vm, "Promise", promiseCtorFn, 1, vm.promiseProto);
    vm.promiseCtor = V(c);
    vm.defFn(vm.promiseProto, "then", promiseThenFn, 2);
    vm.defFn(vm.promiseProto, "catch", promiseCatchFn, 1);
    vm.defFn(vm.promiseProto, "finally", promiseFinallyFn, 1);
    vm.defineOwn(vm.promiseProto, PropKey::of(vm.symToStringTag), vm.strv("Promise"), PF_C);
    vm.defFn(c, "resolve", promiseResolveFn, 1);
    vm.defFn(c, "reject", promiseRejectFn, 1);
    vm.defFn(c, "all", promiseAll, 1);
    vm.defFn(c, "allSettled", promiseAllSettled, 1);
    vm.defFn(c, "race", promiseRace, 1);
    vm.defFn(c, "any", promiseAny, 1);
    vm.defFn(c, "withResolvers", promiseWithResolvers, 0);
}

// ---------------------------------------------------------------------------- Date
static const double kMsPerDay = 86400000.0;
static const char* kDayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static double daysFromCivil(double y, int m, int d) {  // m: 1..12
    y -= m <= 2;
    double era = std::floor(y / 400);
    double yoe = y - era * 400;
    double doy = std::floor((153.0 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1;
    double doe = yoe * 365 + std::floor(yoe / 4) - std::floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}
static void civilFromDays(double z, double& y, int& m, int& d) {
    z += 719468;
    double era = std::floor(z / 146097);
    double doe = z - era * 146097;
    double yoe = std::floor((doe - std::floor(doe / 1460) + std::floor(doe / 36524) - std::floor(doe / 146096)) / 365);
    y = yoe + era * 400;
    double doy = doe - (365 * yoe + std::floor(yoe / 4) - std::floor(yoe / 100));
    double mp = std::floor((5 * doy + 2) / 153);
    d = (int)(doy - std::floor((153 * mp + 2) / 5) + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) y += 1;
}
struct DateParts {
    double year;
    int month, day, hour, min, sec, ms, wday;  // month: 0..11
};
static DateParts partsOf(double t) {
    DateParts p{};
    double days = std::floor(t / kMsPerDay), rem = t - days * kMsPerDay;
    int m = 1;
    civilFromDays(days, p.year, m, p.day);
    p.month = m - 1;
    p.hour = (int)(rem / 3600000);
    rem -= p.hour * 3600000.0;
    p.min = (int)(rem / 60000);
    rem -= p.min * 60000.0;
    p.sec = (int)(rem / 1000);
    p.ms = (int)(rem - p.sec * 1000.0);
    double wd = std::fmod(days + 4, 7);
    if (wd < 0) wd += 7;
    p.wday = (int)wd;
    return p;
}
static double makeTime(double y, double mon, double day, double h, double mi, double s, double ms) {
    if (!std::isfinite(y) || !std::isfinite(mon) || !std::isfinite(day) || !std::isfinite(h) || !std::isfinite(mi) || !std::isfinite(s) || !std::isfinite(ms)) return NAN;
    y = std::trunc(y), mon = std::trunc(mon), day = std::trunc(day), h = std::trunc(h), mi = std::trunc(mi), s = std::trunc(s), ms = std::trunc(ms);
    double ym = y + std::floor(mon / 12);
    int mn = (int)(mon - std::floor(mon / 12) * 12);
    if (std::fabs(ym) > 400000) return NAN;
    return (daysFromCivil(ym, mn + 1, 1) + (day - 1)) * kMsPerDay + h * 3600000.0 + mi * 60000.0 + s * 1000.0 + ms;
}
static double timeClip(double t) {
    if (!std::isfinite(t) || std::fabs(t) > 8.64e15) return NAN;
    return std::trunc(t) + 0.0;
}
static double nowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t v = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (double)((v - 116444736000000000ull) / 10000);
}
// Milliseconds to add to a UTC time to get local wall-clock time.
static double localOffset(double utcMs) {
    double ticks = (utcMs + 11644473600000.0) * 10000.0;
    if (std::isfinite(utcMs) && ticks > 0 && ticks < 9.0e18) {
        uint64_t v = (uint64_t)ticks;
        FILETIME ft{(DWORD)v, (DWORD)(v >> 32)}, lf;
        SYSTEMTIME utc, loc;
        if (FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc) && SystemTimeToFileTime(&loc, &lf)) {
            uint64_t lv = ((uint64_t)lf.dwHighDateTime << 32) | lf.dwLowDateTime;
            return std::round(((double)(int64_t)(lv - v)) / 600000000.0) * 60000.0;
        }
    }
    TIME_ZONE_INFORMATION tz;
    GetTimeZoneInformation(&tz);
    return -(double)tz.Bias * 60000.0;
}
static double localToUtc(double local) {
    if (!std::isfinite(local)) return NAN;
    double guess = local - localOffset(local);
    return local - localOffset(guess);
}

static std::string formatDate(double t, int kind) {  // 0 toString, 1 date, 2 time, 3 ISO, 4 UTC, 5 locale date, 6 locale time, 7 locale
    if (std::isnan(t)) return "Invalid Date";
    char buf[128];
    if (kind == 3 || kind == 4) {
        DateParts p = partsOf(t);
        if (kind == 4) {
            snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", kDayNames[p.wday], p.day, kMonthNames[p.month], (int)p.year, p.hour, p.min, p.sec);
        } else if (p.year >= 0 && p.year <= 9999) {
            snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", (int)p.year, p.month + 1, p.day, p.hour, p.min, p.sec, p.ms);
        } else {
            snprintf(buf, sizeof buf, "%c%06d-%02d-%02dT%02d:%02d:%02d.%03dZ", p.year < 0 ? '-' : '+', (int)std::fabs(p.year), p.month + 1, p.day, p.hour, p.min, p.sec, p.ms);
        }
        return buf;
    }
    double off = localOffset(t);
    DateParts p = partsOf(t + off);
    int om = (int)(std::fabs(off) / 60000);
    char zone[16];
    snprintf(zone, sizeof zone, "GMT%c%02d%02d", off < 0 ? '-' : '+', om / 60, om % 60);
    int h12 = p.hour % 12 == 0 ? 12 : p.hour % 12;
    switch (kind) {
        case 1: snprintf(buf, sizeof buf, "%s %s %02d %04d", kDayNames[p.wday], kMonthNames[p.month], p.day, (int)p.year); break;
        case 2: snprintf(buf, sizeof buf, "%02d:%02d:%02d %s", p.hour, p.min, p.sec, zone); break;
        case 5: snprintf(buf, sizeof buf, "%d/%d/%d", p.month + 1, p.day, (int)p.year); break;
        case 6: snprintf(buf, sizeof buf, "%d:%02d:%02d %s", h12, p.min, p.sec, p.hour < 12 ? "AM" : "PM"); break;
        case 7: snprintf(buf, sizeof buf, "%d/%d/%d, %d:%02d:%02d %s", p.month + 1, p.day, (int)p.year, h12, p.min, p.sec, p.hour < 12 ? "AM" : "PM"); break;
        default:
            snprintf(buf, sizeof buf, "%s %s %02d %04d %02d:%02d:%02d %s", kDayNames[p.wday], kMonthNames[p.month], p.day, (int)p.year, p.hour, p.min, p.sec, zone);
    }
    return buf;
}

static double parseDate(const std::u16string& in) {
    std::string s;
    for (char16_t c : in) s += c < 128 ? (char)c : ' ';
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t lead = 0;
    while (lead < s.size() && isspace((unsigned char)s[lead])) ++lead;
    s.erase(0, lead);
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    {  // ISO 8601: YYYY[-MM[-DD]][THH:mm[:ss[.sss]]][Z|+HH:MM]
        const char* p = s.c_str();
        int sign = 0;
        if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
        auto digits = [&](int count, int& out) {
            out = 0;
            for (int k = 0; k < count; ++k) {
                if (!isDigit(p[k])) return false;
                out = out * 10 + (p[k] - '0');
            }
            p += count;
            return true;
        };
        int y = 0, mo = 1, d = 1, h = 0, mi = 0, si = 0;
        double frac = 0, zone = 0;
        bool hasTime = false, hasZone = false;
        if (digits(sign ? 6 : 4, y)) {
            bool ok = true;
            if (*p == '-') {
                ++p;
                ok = digits(2, mo);
                if (ok && *p == '-') {
                    ++p;
                    ok = digits(2, d);
                }
            }
            if (ok && (*p == 'T' || *p == 't' || (*p == ' ' && isDigit(p[1]) && isDigit(p[2]) && p[3] == ':'))) {
                ++p;
                hasTime = true;
                ok = digits(2, h) && *p == ':';
                if (ok) {
                    ++p;
                    ok = digits(2, mi);
                }
                if (ok && *p == ':') {
                    ++p;
                    ok = digits(2, si);
                    if (ok && (*p == '.' || *p == ',')) {
                        ++p;
                        double scale = 100;
                        while (isDigit(*p)) {
                            frac += (*p - '0') * scale;
                            scale /= 10;
                            ++p;
                        }
                    }
                }
            }
            if (ok && (*p == 'Z' || *p == 'z')) {
                ++p;
                hasZone = true;
            } else if (ok && hasTime && (*p == '+' || *p == '-')) {
                int zs = *p++ == '-' ? -1 : 1, zh = 0, zm = 0;
                ok = digits(2, zh);
                if (ok && *p == ':') ++p;
                if (ok && isDigit(*p)) ok = digits(2, zm);
                zone = zs * (zh * 60 + zm) * 60000.0;
                hasZone = true;
            }
            if (ok && *p == 0 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h <= 24 && mi < 60 && si < 60) {
                double t = makeTime((sign < 0 ? -1.0 : 1.0) * y, mo - 1, d, h, mi, si, std::floor(frac));
                if (hasZone) t -= zone;
                else if (hasTime) t = localToUtc(t);  // date-only forms are UTC, date-times without a zone are local
                return timeClip(t);
            }
        }
    }
    // Free-form: "Nov 26, 2026 10:30:00 GMT+0100", "Thu, 26 Nov 2026 09:30:00 GMT", "11/26/2026 4:05 PM" ...
    int year = INT_MIN, month = -1, day = -1, hh = 0, mm = 0;
    double ss = 0, zone = 0;
    bool am = false, pm = false, hasZone = false, timeSeen = false;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (isalpha(c)) {
            std::string w;
            while (i < n && isalpha((unsigned char)s[i])) w += (char)tolower((unsigned char)s[i++]);
            static const struct {
                const char* name;
                int hours;
            } zones[] = {{"gmt", 0}, {"utc", 0}, {"ut", 0}, {"z", 0}, {"est", -5}, {"edt", -4}, {"cst", -6}, {"cdt", -5}, {"mst", -7}, {"mdt", -6}, {"pst", -8}, {"pdt", -7}};
            bool known = false;
            if (w == "am") am = known = true;
            else if (w == "pm") pm = known = true;
            for (auto& z : zones)
                if (!known && w == z.name) {
                    zone = z.hours * 3600000.0;
                    hasZone = known = true;
                }
            if (!known && w.size() >= 3)
                for (int k = 0; k < 12; ++k)
                    if (tolower(kMonthNames[k][0]) == w[0] && kMonthNames[k][1] == w[1] && kMonthNames[k][2] == w[2]) month = k;
            continue;
        }
        if (!isDigit((char)c)) {
            ++i;
            continue;
        }
        size_t st = i;
        double v = 0;
        while (i < n && isDigit(s[i])) v = v * 10 + (s[i++] - '0');
        size_t len = i - st;
        auto readInt = [&] {
            int r = 0;
            while (i < n && isDigit(s[i])) r = r * 10 + (s[i++] - '0');
            return r;
        };
        if (st > 0 && (s[st - 1] == '+' || s[st - 1] == '-') && timeSeen) {  // zone offset: +hhmm, +hh:mm or +hh
            int zh = (int)v, zm = 0;
            if (len == 4) {
                zh = (int)v / 100;
                zm = (int)v % 100;
            } else if (i < n && s[i] == ':') {
                ++i;
                zm = readInt();
            }
            zone += (s[st - 1] == '-' ? -1 : 1) * (zh * 60 + zm) * 60000.0;
            hasZone = true;
        } else if (i < n && s[i] == ':') {  // time of day
            hh = (int)v;
            ++i;
            mm = readInt();
            if (i < n && s[i] == ':') {
                ++i;
                ss = readInt();
                if (i < n && s[i] == '.') {
                    ++i;
                    double scale = 0.1;
                    while (i < n && isDigit(s[i])) {
                        ss += (s[i++] - '0') * scale;
                        scale /= 10;
                    }
                }
            }
            timeSeen = true;
        } else if (i < n && s[i] == '/' && month < 0) {  // month/day/year
            month = (int)v - 1;
            ++i;
            day = readInt();
            if (i < n && s[i] == '/') {
                ++i;
                size_t ys = i;
                year = readInt();
                if (i - ys <= 2) year += year < 50 ? 2000 : 1900;
            }
        } else if (len >= 3 || v > 31) {
            year = (int)v;
        } else if (day < 0) {
            day = (int)v;
        } else if (year == INT_MIN) {
            year = (int)v + ((int)v < 50 ? 2000 : 1900);
        }
    }
    if (year == INT_MIN || month < 0 || month > 11) return NAN;
    if (day < 0) day = 1;
    if (pm && hh < 12) hh += 12;
    if (am && hh == 12) hh = 0;
    double t = makeTime(year, month, day, hh, mm, std::floor(ss), std::floor((ss - std::floor(ss)) * 1000 + 0.5));
    return timeClip(hasZone ? t - zone : localToUtc(t));
}

static JSPrimitive* thisDate(CallInfo& ci) {
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != Cls::Date) ci.vm.throwError(Err::TypeError, "this is not a Date object.");
    return (JSPrimitive*)ci.thisv.asObject();
}
JSFN(dateCtor) {
    Vm& vm = ci.vm;
    if (!ci.isConstruct()) return vm.strv(formatDate(nowMs(), 0));
    double t;
    if (ci.argc == 0) {
        t = nowMs();
    } else if (ci.argc == 1) {
        Value v = ci.args[0];
        if (v.isObject() && v.asObject()->cls == Cls::Date) {
            t = ((JSPrimitive*)v.asObject())->prim.number();
        } else {
            v = vm.toPrimitive(v, 0);
            t = v.isString() ? parseDate(v.asString()->s) : timeClip(vm.toNumber(v));
        }
    } else {
        double f[7] = {0, 0, 1, 0, 0, 0, 0};
        for (int i = 0; i < 7 && i < ci.argc; ++i) f[i] = vm.toNumber(ci.args[i]);
        if (std::isfinite(f[0]) && std::trunc(f[0]) >= 0 && std::trunc(f[0]) <= 99) f[0] = 1900 + std::trunc(f[0]);
        t = timeClip(localToUtc(makeTime(f[0], f[1], f[2], f[3], f[4], f[5], f[6])));
    }
    return wrapPrimitive(ci, Value::num(t), Cls::Date, vm.dateProto);
}
JSFN(dateNow) { return Value::num(nowMs()); }
JSFN(dateParse) { return Value::num(parseDate(ci.vm.toStr16(ci.arg(0)))); }
JSFN(dateUTC) {
    double f[7] = {NAN, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < 7 && i < ci.argc; ++i) f[i] = ci.vm.toNumber(ci.args[i]);
    if (std::isfinite(f[0]) && std::trunc(f[0]) >= 0 && std::trunc(f[0]) <= 99) f[0] = 1900 + std::trunc(f[0]);
    return Value::num(timeClip(makeTime(f[0], f[1], f[2], f[3], f[4], f[5], f[6])));
}
JSFN(dateGet) {
    int code = ci.callee->data.asInt();
    double t = thisDate(ci)->prim.number();
    if (code == 100) return Value::num(t);
    if (std::isnan(t)) return Value::dbl(NAN);
    if (code == 101) return Value::num(-localOffset(t) / 60000.0);
    if (code < 16) t += localOffset(t);
    DateParts p = partsOf(t);
    switch (code & 15) {
        case 0: return Value::num(p.year);
        case 1: return Value::i32(p.month);
        case 2: return Value::i32(p.day);
        case 3: return Value::i32(p.wday);
        case 4: return Value::i32(p.hour);
        case 5: return Value::i32(p.min);
        case 6: return Value::i32(p.sec);
        case 7: return Value::i32(p.ms);
        default: return Value::num(p.year - 1900);
    }
}
JSFN(dateSet) {
    Vm& vm = ci.vm;
    int code = ci.callee->data.asInt();
    JSPrimitive* d = thisDate(ci);
    double t = d->prim.number();
    if (code == 100) {
        d->prim = Value::num(timeClip(vm.toNumber(ci.arg(0))));
        return d->prim;
    }
    bool utc = code >= 16;
    int field = code & 15;  // 0 year, 1 month, 2 day, 3 hours, 4 minutes, 5 seconds, 6 milliseconds
    static const int maxArgs[] = {3, 2, 1, 4, 3, 2, 1};
    double args[4] = {NAN, NAN, NAN, NAN};
    int given = std::min(ci.argc, maxArgs[field]);
    for (int i = 0; i < given; ++i) args[i] = vm.toNumber(ci.args[i]);
    if (std::isnan(t)) {
        if (field != 0) return Value::dbl(NAN);
        t = 0;
    }
    DateParts p = partsOf(utc ? t : t + localOffset(t));
    double f[7] = {p.year, (double)p.month, (double)p.day, (double)p.hour, (double)p.min, (double)p.sec, (double)p.ms};
    for (int i = 0; i < given; ++i) f[field + i] = args[i];
    if (given == 0) f[field] = NAN;
    double nt = makeTime(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    if (!utc) nt = localToUtc(nt);
    d->prim = Value::num(timeClip(nt));
    return d->prim;
}
JSFN(dateFormat) {
    Vm& vm = ci.vm;
    int kind = ci.callee->data.asInt();
    double t = thisDate(ci)->prim.number();
    if (kind == 3 && std::isnan(t)) vm.throwError(Err::RangeError, "Invalid time value");
    return vm.strv(formatDate(t, kind));
}
JSFN(dateToJSON) {
    Value tv = ci.vm.toPrimitive(ci.thisv, 1);
    if (tv.isNumber() && !std::isfinite(tv.number())) return Value::null();
    return ci.vm.invoke(ci.thisv, "toISOString");
}

void setupDate(Vm& vm) {
    JSPrimitive* proto = vm.alloc<JSPrimitive>();
    proto->proto = vm.objectProto;
    proto->prim = Value::dbl(NAN);
    vm.dateProto = proto;
    JSNative* c = makeCtor(vm, "Date", dateCtor, 7, proto);
    vm.defFn(c, "now", dateNow, 0);
    vm.defFn(c, "parse", dateParse, 1);
    vm.defFn(c, "UTC", dateUTC, 7);
    struct {
        const char* name;
        int code;
    } getters[] = {{"getTime", 100}, {"valueOf", 100}, {"getTimezoneOffset", 101}, {"getFullYear", 0}, {"getMonth", 1}, {"getDate", 2},
                   {"getDay", 3}, {"getHours", 4}, {"getMinutes", 5}, {"getSeconds", 6}, {"getMilliseconds", 7}, {"getYear", 8},
                   {"getUTCFullYear", 16}, {"getUTCMonth", 17}, {"getUTCDate", 18}, {"getUTCDay", 19}, {"getUTCHours", 20},
                   {"getUTCMinutes", 21}, {"getUTCSeconds", 22}, {"getUTCMilliseconds", 23}};
    for (auto& g : getters) vm.defFn(proto, g.name, dateGet, 0)->data = Value::i32(g.code);
    struct {
        const char* name;
        int code, len;
    } setters[] = {{"setTime", 100, 1}, {"setFullYear", 0, 3}, {"setMonth", 1, 2}, {"setDate", 2, 1}, {"setHours", 3, 4}, {"setMinutes", 4, 3},
                   {"setSeconds", 5, 2}, {"setMilliseconds", 6, 1}, {"setUTCFullYear", 16, 3}, {"setUTCMonth", 17, 2}, {"setUTCDate", 18, 1},
                   {"setUTCHours", 19, 4}, {"setUTCMinutes", 20, 3}, {"setUTCSeconds", 21, 2}, {"setUTCMilliseconds", 22, 1}};
    for (auto& s : setters) vm.defFn(proto, s.name, dateSet, s.len)->data = Value::i32(s.code);
    struct {
        const char* name;
        int kind;
    } formats[] = {{"toString", 0}, {"toDateString", 1}, {"toTimeString", 2}, {"toISOString", 3}, {"toUTCString", 4}, {"toGMTString", 4},
                   {"toLocaleDateString", 5}, {"toLocaleTimeString", 6}, {"toLocaleString", 7}};
    for (auto& f : formats) vm.defFn(proto, f.name, dateFormat, 0)->data = Value::i32(f.kind);
    vm.defFn(proto, "toJSON", dateToJSON, 1);
}

// ---------------------------------------------------------------------------- iterators and generators
JSFN(returnThis) { return ci.thisv; }
JSFN(builtinIterNext) {
    Vm& vm = ci.vm;
    Cls c = ci.thisv.isObject() ? ci.thisv.asObject()->cls : Cls::Object;
    if (c != Cls::ArrayIter && c != Cls::MapIter && c != Cls::SetIter && c != Cls::StringIter) vm.throwError(Err::TypeError, "next method called on incompatible receiver");
    Value out;
    if (vm.iterStep((JSIter*)ci.thisv.asObject(), out)) return vm.makeIterResult(out, false);
    return vm.makeIterResult(Value::undef(), true);
}
static Value generatorResume(CallInfo& ci, int mode) {
    Vm& vm = ci.vm;
    JSGenerator* g = ci.thisv.isObject() && ci.thisv.asObject()->cls == Cls::Generator ? (JSGenerator*)ci.thisv.asObject() : nullptr;
    if (!g || g->isAsync) vm.throwError(Err::TypeError, "Generator method called on incompatible receiver");
    bool done = false;
    Value v = vm.resumeGenerator(g, ci.arg(0), mode, done);
    if (!done && g->delegating) return v;  // yield* hands on the inner iterator's own result object
    return vm.makeIterResult(v, done);
}
JSFN(generatorNext) { return generatorResume(ci, 0); }
JSFN(generatorThrow) { return generatorResume(ci, 1); }
JSFN(generatorReturn) { return generatorResume(ci, 2); }

// ---------------------------------------------------------------------------- Map, Set, WeakMap, WeakSet
static Value normKey(Value k) { return (k.isDouble() && k.asDouble() == 0) ? Value::i32(0) : k; }
static void mapCompact(JSMap* m) {
    m->removed.clear();
    size_t w = 0;
    for (size_t r = 0; r < m->entries.size(); ++r) {
        if (m->entries[r].first.isEmpty()) {
            m->removed.push_back((uint32_t)r);
            continue;
        }
        if (w != r) m->entries[w] = m->entries[r];
        ++w;
    }
    m->entries.resize(w);
    m->lookup.clear();
    for (size_t i = 0; i < w; ++i) m->lookup[m->entries[i].first] = (uint32_t)i;
    m->epoch++;
}
static void mapPut(JSMap* m, Value k, Value v) {
    k = normKey(k);
    auto it = m->lookup.find(k);
    if (it != m->lookup.end()) {
        m->entries[it->second].second = v;
        return;
    }
    if (m->entries.size() > 32 && m->entries.size() > m->live * 2) mapCompact(m);
    m->lookup[k] = (uint32_t)m->entries.size();
    m->entries.push_back({k, v});
    m->live++;
}
static JSMap* thisMap(CallInfo& ci) {
    Cls cls = (Cls)ci.callee->data.asInt();
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != cls) ci.vm.throwError(Err::TypeError, "Method called on incompatible receiver");
    return (JSMap*)ci.thisv.asObject();
}
static void weakCheck(Vm& vm, Cls cls, Value key) {
    if ((cls == Cls::WeakMap || cls == Cls::WeakSet) && !key.isObject() && !key.isSymbol()) vm.throwError(Err::TypeError, "Invalid value used as weak collection key");
}
JSFN(collectionCtor) {
    Vm& vm = ci.vm;
    Cls cls = (Cls)ci.callee->data.asInt();
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Constructor requires 'new'");
    JSMap* m = vm.alloc<JSMap>();
    m->cls = cls;
    m->proto = vm.protoFromNewTarget(ci.newTarget, ci.callee->data2.asObject());
    Value src = ci.arg(0);
    if (!src.isNullish()) {
        bool keyed = cls == Cls::Map || cls == Cls::WeakMap;
        vm.iterate(src, [&](Value e) {
            if (keyed) {
                if (!e.isObject()) vm.throwError(Err::TypeError, "Iterator value %s is not an entry object", utf16to8(vm.inspect(e, 0)).c_str());
                Value k = vm.get(e, PropKey::index(0));
                weakCheck(vm, cls, k);
                mapPut(m, k, vm.get(e, PropKey::index(1)));
            } else {
                weakCheck(vm, cls, e);
                mapPut(m, e, e);
            }
            return true;
        });
    }
    return V(m);
}
JSFN(mapGet) {
    JSMap* m = thisMap(ci);
    auto it = m->lookup.find(normKey(ci.arg(0)));
    return it == m->lookup.end() ? Value::undef() : m->entries[it->second].second;
}
JSFN(mapSet) {
    JSMap* m = thisMap(ci);
    weakCheck(ci.vm, m->cls, ci.arg(0));
    mapPut(m, ci.arg(0), ci.arg(1));
    return ci.thisv;
}
JSFN(setAdd) {
    JSMap* m = thisMap(ci);
    weakCheck(ci.vm, m->cls, ci.arg(0));
    mapPut(m, ci.arg(0), ci.arg(0));
    return ci.thisv;
}
JSFN(mapHas) {
    JSMap* m = thisMap(ci);
    return Value::boolean(m->lookup.find(normKey(ci.arg(0))) != m->lookup.end());
}
JSFN(mapDelete) {
    JSMap* m = thisMap(ci);
    auto it = m->lookup.find(normKey(ci.arg(0)));
    if (it == m->lookup.end()) return Value::boolean(false);
    m->entries[it->second] = {Value::empty(), Value::undef()};
    m->lookup.erase(it);
    m->live--;
    return Value::boolean(true);
}
JSFN(mapClear) {
    JSMap* m = thisMap(ci);
    m->entries.clear();
    m->lookup.clear();
    m->removed.clear();
    m->live = 0;
    m->epoch += 2;
    return Value::undef();
}
JSFN(mapSize) { return Value::num((double)thisMap(ci)->live); }
JSFN(mapForEach) {
    Vm& vm = ci.vm;
    JSMap* m = thisMap(ci);
    needFn2(vm, ci.arg(0));
    JSIter* it = vm.newIter(m->cls == Cls::Map ? Cls::MapIter : Cls::SetIter, ci.thisv, 2);
    Value pair;
    while (vm.iterStep(it, pair)) {
        JSArray* p = (JSArray*)pair.asObject();
        Value args[3] = {p->elems[1], p->elems[0], ci.thisv};
        vm.call(ci.arg(0), ci.arg(1), args, 3);
    }
    return Value::undef();
}
JSFN(mapIterate) {
    JSMap* m = thisMap(ci);
    return V(ci.vm.newIter(m->cls == Cls::Map ? Cls::MapIter : Cls::SetIter, ci.thisv, (uint8_t)ci.callee->data2.asInt()));
}

JSFN(weakRefCtor) {
    Vm& vm = ci.vm;
    if (!ci.isConstruct() || !ci.arg(0).isObject()) vm.throwError(Err::TypeError, "WeakRef: target must be an object");
    JSObject* o = vm.newObject(vm.protoFromNewTarget(ci.newTarget, ci.callee->data2.asObject()));
    o->add(PropKey::of(ci.callee->data.asCell()), ci.arg(0), 0);
    return V(o);
}
JSFN(weakRefDeref) {
    if (!ci.thisv.isObject()) return Value::undef();
    Prop* p = ci.thisv.asObject()->find(PropKey::of(ci.callee->data.asCell()));
    return p ? p->val : Value::undef();
}
JSFN(finalizationRegistryCtor) {
    return V(ci.vm.newObject(ci.vm.protoFromNewTarget(ci.newTarget, ci.callee->data2.asObject())));
}
JSFN(noop) { return Value::undef(); }

void Vm::setupCollections() {
    Vm& vm = *this;
    // iterator prototypes
    iteratorProto = newObject();
    defSym(vm, iteratorProto, symIterator, returnThis, "[Symbol.iterator]", 0);
    auto iterProto = [&](const char* tag) {
        JSObject* p = newObject(iteratorProto);
        defFn(p, "next", builtinIterNext, 0);
        defineOwn(p, PropKey::of(symToStringTag), strv(tag), PF_C);
        return p;
    };
    arrayIterProto = iterProto("Array Iterator");
    arrayIterNextFn = get(V(arrayIterProto), a.next);
    mapIterProto = iterProto("Map Iterator");
    setIterProto = iterProto("Set Iterator");
    stringIterProto = iterProto("String Iterator");
    generatorProto = newObject(iteratorProto);
    defFn(generatorProto, "next", generatorNext, 1);
    defFn(generatorProto, "throw", generatorThrow, 1);
    defFn(generatorProto, "return", generatorReturn, 1);
    defineOwn(generatorProto, PropKey::of(symToStringTag), strv("Generator"), PF_C);

    // keyed collections
    struct {
        const char* name;
        Cls cls;
        JSObject** slot;
    } kinds[] = {{"Map", Cls::Map, &mapProto}, {"Set", Cls::Set, &setProto}, {"WeakMap", Cls::WeakMap, &weakMapProto}, {"WeakSet", Cls::WeakSet, &weakSetProto}};
    for (auto& k : kinds) {
        JSObject* proto = newObject();
        *k.slot = proto;
        JSNative* c = makeCtor(vm, k.name, collectionCtor, 0, proto);
        Value tag = Value::i32((int)k.cls);
        c->data = tag;
        c->data2 = V(proto);
        bool keyed = k.cls == Cls::Map || k.cls == Cls::WeakMap, weak = k.cls == Cls::WeakMap || k.cls == Cls::WeakSet;
        auto method = [&](const char* name, NativeFn fn, int len) {
            JSNative* n = defFn(proto, name, fn, len);
            n->data = tag;
            return n;
        };
        if (keyed) {
            method("get", mapGet, 1);
            method("set", mapSet, 2);
        } else
            method("add", setAdd, 1);
        method("has", mapHas, 1);
        method("delete", mapDelete, 1);
        defineOwn(proto, PropKey::of(symToStringTag), strv(k.name), PF_C);
        if (weak) continue;
        method("clear", mapClear, 0);
        method("forEach", mapForEach, 1);
        JSNative* size = newNative(mapSize, "size", 0);
        size->data = tag;
        defineAccessor(proto, PropKey::of(atom("size")), V(size), Value::undef(), PF_C);
        JSNative* entries = method("entries", mapIterate, 0);
        entries->data2 = Value::i32(2);
        JSNative* values = method("values", mapIterate, 0);
        values->data2 = Value::i32(0);
        JSNative* keys = method("keys", mapIterate, 0);
        keys->data2 = Value::i32(keyed ? 1 : 0);
        defineOwn(proto, PropKey::of(symIterator), V(keyed ? entries : values), PF_W | PF_C);
    }

    // WeakRef and FinalizationRegistry keep their targets alive for now (no weak semantics yet)
    {
        Symbol* slot = newSymbol(str("WeakRefTarget"));
        JSObject* proto = newObject();
        JSNative* c = makeCtor(vm, "WeakRef", weakRefCtor, 1, proto);
        c->data = V(slot);
        c->data2 = V(proto);
        defFn(proto, "deref", weakRefDeref, 0)->data = V(slot);
        JSObject* frProto = newObject();
        JSNative* fr = makeCtor(vm, "FinalizationRegistry", finalizationRegistryCtor, 1, frProto);
        fr->data2 = V(frProto);
        defFn(frProto, "register", noop, 2);
        defFn(frProto, "unregister", noop, 1);
    }
}

}  // namespace tb::js
