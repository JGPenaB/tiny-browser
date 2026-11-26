// Virtual machine: heap and garbage collector, object model, conversions, and the bytecode interpreter.
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>

#include "builtins.h"
#include "bytecode.h"
#include "js.h"

namespace tb::js {

// ---------------------------------------------------------------------------- text helpers
std::u16string utf8to16(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::u16string w(n, u'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), (wchar_t*)w.data(), n);
    return w;
}
std::string utf16to8(std::u16string_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}
static std::u16string ascii16(std::string_view s) { return std::u16string(s.begin(), s.end()); }

// ---------------------------------------------------------------------------- garbage collector
static const size_t kBlock = 256 * 1024;
static const uint32_t kSizes[] = {32, 48, 64, 80, 96, 128, 160, 192, 256, 384, 512, 1024};
static const int kClasses = 12;

struct Gc::Block {
    uint8_t* mem;
    uint32_t cellSize, count;
    std::vector<uint8_t> used;
    std::vector<uint32_t> free;
};

void* Gc::allocate(size_t size) {
    int ci = 0;
    while (ci < kClasses && kSizes[ci] < size) ++ci;
    if (ci == kClasses) abort();
    sinceGc_ += kSizes[ci];
    auto& avail = classBlocks_[ci];
    while (!avail.empty() && avail.back()->free.empty()) avail.pop_back();
    if (avail.empty()) {
        Block* b = new Block();
        b->mem = (uint8_t*)_aligned_malloc(kBlock, kBlock);
        b->cellSize = kSizes[ci];
        b->count = (uint32_t)(kBlock / b->cellSize);
        b->used.assign(b->count, 0);
        b->free.reserve(b->count);
        for (uint32_t i = b->count; i-- > 0;) b->free.push_back(i);
        blocks_.push_back(b);
        blockMap_[(uintptr_t)b->mem] = b;
        avail.push_back(b);
    }
    Block* b = avail.back();
    uint32_t idx = b->free.back();
    b->free.pop_back();
    b->used[idx] = 1;
    return b->mem + (size_t)idx * b->cellSize;
}

Cell* Gc::findCell(uintptr_t p) {
    uintptr_t base = p & ~(uintptr_t)(kBlock - 1);
    auto it = blockMap_.find(base);
    if (it == blockMap_.end()) return nullptr;
    Block* b = it->second;
    size_t idx = (p - base) / b->cellSize;
    if (idx >= b->count || !b->used[idx]) return nullptr;
    return (Cell*)(base + idx * b->cellSize);
}

#pragma warning(push)
#pragma warning(disable : 4611)  // setjmp with C++ objects: only used to spill registers
void Gc::scanStack() {
    jmp_buf jb;
    setjmp(jb);
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    uintptr_t sp = (uintptr_t)&jb & ~(uintptr_t)7;
    for (uintptr_t* p = (uintptr_t*)sp; p < (uintptr_t*)hi; ++p) {
        uintptr_t w = *p;
        if ((w & Value::TAG_PTR) == Value::TAG_PTR) w &= Value::PAYLOAD;
        if (w < 0x10000) continue;
        if (Cell* c = findCell(w)) mark(c);
    }
}
#pragma warning(pop)

void Gc::sweep() {
    live_ = 0;
    for (auto& list : classBlocks_) list.clear();
    for (size_t bi = 0; bi < blocks_.size(); ++bi) {
        Block* b = blocks_[bi];
        b->free.clear();
        for (uint32_t i = b->count; i-- > 0;) {
            if (!b->used[i]) {
                b->free.push_back(i);
                continue;
            }
            Cell* c = (Cell*)(b->mem + (size_t)i * b->cellSize);
            if (c->mark) {
                c->mark = 0;
                live_ += b->cellSize;
            } else {
                c->~Cell();
                b->used[i] = 0;
                b->free.push_back(i);
            }
        }
        int ci = 0;
        while (kSizes[ci] != b->cellSize) ++ci;
        if (!b->free.empty()) classBlocks_[ci].push_back(b);
    }
}

void Gc::collect() {
    ++collections_;
    vm_.markRoots(*this);
    scanStack();
    while (!work_.empty()) {
        Cell* c = work_.back();
        work_.pop_back();
        c->trace(*this);
    }
    sweep();
    threshold_ = std::max<size_t>(8u << 20, live_ * 2);
    sinceGc_ = 0;
}

void Gc::destroyAll() {
    for (Block* b : blocks_) {
        for (uint32_t i = 0; i < b->count; ++i)
            if (b->used[i]) ((Cell*)(b->mem + (size_t)i * b->cellSize))->~Cell();
        _aligned_free(b->mem);
        delete b;
    }
    blocks_.clear();
    blockMap_.clear();
}
Gc::~Gc() { destroyAll(); }

void Symbol::trace(Gc& g) { g.mark(desc); }
void JSHost::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(slotA);
    g.mark(slotB);
}
void JSString::trace(Gc& g) {
    g.mark(left);
    g.mark(right);
}
// Writes the pieces of a rope into one buffer, right to left, without recursion (ropes can be very deep).
void flattenRope(JSString* root) {
    std::u16string out(root->ropeLen, u'\0');
    size_t pos = out.size();
    std::vector<JSString*> todo{root};
    while (!todo.empty()) {
        JSString* n = todo.back();
        todo.pop_back();
        if (n->left) {
            todo.push_back(n->left);
            todo.push_back(n->right);
        } else {
            pos -= n->s.size();
            std::copy(n->s.begin(), n->s.end(), out.begin() + pos);
        }
    }
    root->s = std::move(out);
    root->left = root->right = nullptr;
}
void Accessor::trace(Gc& g) {
    g.mark(get);
    g.mark(set);
}
void JSObject::trace(Gc& g) {
    g.mark(proto);
    for (auto& p : props) {
        if (!p.key.isIndex()) g.mark(p.key.cell());
        g.mark(p.val);
    }
}
void JSArray::trace(Gc& g) {
    JSObject::trace(g);
    for (Value v : elems) g.mark(v);
}
void JSPrimitive::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(prim);
    g.mark(extra);
}
void Context::trace(Gc& g) {
    g.mark(parent);
    for (Value v : slots) g.mark(v);
}
void FuncProto::trace(Gc& g) {
    for (auto& site : evalSites)
        for (auto& sc : site.scopes)
            for (auto& v : sc.vars) g.mark(v.name);
    for (Value v : consts) g.mark(v);
    for (FuncProto* p : inner) g.mark(p);
    g.mark(name);
}
void JSFunction::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(code);
    g.mark(ctx);
    g.mark(home);
    g.mark(fields);
    g.mark(privates);
}
void JSNative::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(name);
    g.mark(data);
    g.mark(data2);
}
void JSBound::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(target);
    g.mark(boundThis);
    for (Value v : args) g.mark(v);
}
void JSMap::trace(Gc& g) {
    JSObject::trace(g);
    for (auto& e : entries) {
        g.mark(e.first);
        g.mark(e.second);
    }
}
void JSIter::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(target);
    for (Value v : keys) g.mark(v);
}
void JSPromise::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(value);
    for (auto& r : reactions) {
        g.mark(r.onFulfilled);
        g.mark(r.onRejected);
        g.mark(r.resultPromise);
    }
}
void JSGenerator::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(fn);
    for (Value v : regs) g.mark(v);
    g.mark(ctx);
    g.mark(newTarget);
    g.mark(promise);
    g.mark(retVal);
    g.mark(args);
    for (auto& r : queue) {
        g.mark(r.value);
        g.mark(r.promise);
    }
}

// ---------------------------------------------------------------------------- property storage
Prop* JSObject::find(PropKey k) {
    if (index) {
        auto it = index->find(k.v);
        return it == index->end() ? nullptr : &props[it->second];
    }
    for (auto& p : props)
        if (p.key == k) return &p;
    return nullptr;
}
Prop& JSObject::add(PropKey k, Value v, uint8_t flags) {
    props.push_back({k, v, flags});
    if (index) {
        (*index)[k.v] = (uint32_t)props.size() - 1;
    } else if (props.size() > 12) {
        index = std::make_unique<std::unordered_map<uint64_t, uint32_t>>();
        for (size_t i = 0; i < props.size(); ++i) (*index)[props[i].key.v] = (uint32_t)i;
    }
    return props.back();
}
bool JSObject::remove(PropKey k) {
    for (size_t i = 0; i < props.size(); ++i)
        if (props[i].key == k) {
            props.erase(props.begin() + i);
            if (index) {
                index->clear();
                for (size_t j = 0; j < props.size(); ++j) (*index)[props[j].key.v] = (uint32_t)j;
            }
            return true;
        }
    return false;
}

size_t ValueHash::operator()(Value v) const {
    if (v.isString()) return std::hash<std::u16string>()(v.asString()->s);
    if (v.isInt()) return std::hash<double>()((double)v.asInt());
    if (v.isDouble()) return std::hash<double>()(v.asDouble());
    return std::hash<uint64_t>()(v.b);
}
bool ValueEq::operator()(Value a, Value b) const {
    if (a.b == b.b) return true;
    if (a.isString() && b.isString()) return a.asString()->s == b.asString()->s;
    if (a.isNumber() && b.isNumber()) {
        double x = a.number(), y = b.number();
        return x == y || (x != x && y != y);
    }
    return false;
}

// ---------------------------------------------------------------------------- Vm: setup and allocation
static const size_t kMaxFrames = 12000;
static const int kMaxDepth = 1800;

Vm::Vm() {
    stack.resize(1 << 19);
    stackTop = stack.data();
    stackEnd = stack.data() + stack.size() - 512;
    frames.reserve(kMaxFrames + 16);
    const char* names[] = {"length", "prototype", "constructor", "name", "message", "stack", "toString", "valueOf", "value", "done",
                           "next", "get", "set", "enumerable", "configurable", "writable", "undefined", "null", "true", "false",
                           "object", "function", "string", "number", "boolean", "symbol", "", "arguments", "callee", "return", "then",
                           "index", "input", "cause", "errors", "lastIndex", "__proto__", "default", "raw", "toJSON", "join", "source",
                           "flags", "global", "status", "reason"};
    JSString** slots = &a.length;
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) slots[i] = atom(names[i]);
    objectProto = alloc<JSObject>();
    functionProto = alloc<JSObject>();
    functionProto->proto = objectProto;
    global = newObject(objectProto, Cls::Global);
    globalLex = alloc<JSObject>();
    setupGlobals();
    setupCollections();
    setupTypedArrays(*this);
    setupAsyncIteration(*this);
    setupProxy(*this);
    setupModules(*this);
    setupJsBuiltins(*this);
}
Vm::~Vm() {}

JSString* Vm::str(std::u16string s) {
    JSString* j = alloc<JSString>();
    gc.noteExternal(s.size() * 2);
    j->s = std::move(s);
    return j;
}
JSString* Vm::str(std::string_view s) { return str(ascii16(s)); }
JSString* Vm::atom(std::u16string_view sv) {
    auto it = atomTable.find(sv);
    if (it != atomTable.end()) return it->second;
    JSString* j = alloc<JSString>();
    j->s = std::u16string(sv);
    j->isAtom = true;
    j->atom = j;
    atomTable.emplace(std::u16string_view(j->s), j);
    return j;
}
JSString* Vm::atom(std::string_view s) { return atom(std::u16string_view(ascii16(s))); }
JSString* Vm::atomOf(JSString* s) {
    if (!s->atom) s->atom = atom(std::u16string_view(s->s));
    return s->atom;
}
Symbol* Vm::newSymbol(JSString* desc) {
    Symbol* s = alloc<Symbol>();
    s->desc = desc;
    return s;
}
JSObject* Vm::newObject(JSObject* proto, Cls cls) {
    JSObject* o = alloc<JSObject>();
    o->proto = proto;
    o->cls = cls;
    return o;
}
JSArray* Vm::newArray(size_t n) {
    JSArray* a2 = alloc<JSArray>();
    a2->proto = arrayProto;
    if (n) a2->elems.assign(n, Value::undef());
    return a2;
}
JSNative* Vm::newNative(NativeFn fn, std::string_view name, int length) {
    JSNative* n = alloc<JSNative>();
    n->proto = functionProto;
    n->fn = fn;
    n->name = atom(name);
    n->length = length;
    return n;
}
JSIter* Vm::newIter(Cls cls, Value target, uint8_t mode) {
    JSIter* it = alloc<JSIter>();
    it->cls = cls;
    it->proto = cls == Cls::ArrayIter ? arrayIterProto : cls == Cls::MapIter ? mapIterProto : cls == Cls::SetIter ? setIterProto
                : cls == Cls::StringIter ? stringIterProto : nullptr;
    it->target = target;
    it->mode = mode;
    if ((cls == Cls::MapIter || cls == Cls::SetIter) && target.isObject()) it->epoch = ((JSMap*)target.asObject())->epoch;
    return it;
}
void Vm::def(JSObject* o, const char* name, Value v, uint8_t flags) { defineOwn(o, PropKey::of(atom(name)), v, flags); }
JSNative* Vm::defFn(JSObject* o, const char* name, NativeFn fn, int length, uint8_t flags) {
    JSNative* n = newNative(fn, name, length);
    defineOwn(o, PropKey::of(atom(name)), Value::cell(n), flags);
    return n;
}
void Vm::defGetter(JSObject* o, const char* name, NativeFn getter, NativeFn setter) {
    defineAccessor(o, PropKey::of(atom(name)), Value::cell(newNative(getter, name, 0)),
                   setter ? Value::cell(newNative(setter, name, 1)) : Value::undef(), PF_C);
}

void Vm::markRoots(Gc& g) {
    for (Value* p = stack.data(); p < stackTop; ++p) g.mark(*p);
    for (auto& f : frames) {
        g.mark(f.fn);
        g.mark(f.code);
        g.mark(f.ctx);
        g.mark(f.newTarget);
        g.mark(f.args);
        g.mark(f.gen);
    }
    g.mark(pending);
    g.mark(global);
    g.mark(globalLex);
    JSObject** protos = &objectProto;
    for (int i = 0; i < 22; ++i) g.mark(protos[i]);
    for (JSObject* p : errorProtos) g.mark(p);
    Symbol** syms = &symIterator;
    for (int i = 0; i < 6; ++i) g.mark(syms[i]);
    g.mark(regexpCtor);
    g.mark(promiseCtor);
    g.mark(freshError);
    for (auto& kv : atomTable) g.mark(kv.second);
    for (auto& kv : symbolRegistry) g.mark(kv.second);
    for (auto& j : jobQueue) {
        g.mark(j.a);
        g.mark(j.b);
        g.mark(j.c);
    }
    for (Value v : rejections) g.mark(v);
    for (auto* vec : rootVecs)
        for (Value v : *vec) g.mark(v);
    for (auto& tracer : rootTracers) tracer(g);
    for (JSObject* p : typedProtos) g.mark(p);
    g.mark(arrayBufferProto);
    g.mark(dataViewProto);
    g.mark(importHook);
    g.mark(evalFn);
    g.mark(arrayValuesFn);
    g.mark(arrayCtorValue);
    g.mark(arraySpeciesGetter);
    g.mark(arrayIterNextFn);
    g.mark(asyncIteratorProto);
    g.mark(asyncGeneratorProto);
    g.mark(asyncFromSyncProto);
    g.mark(genReturnSignal);
}

// ---------------------------------------------------------------------------- errors
std::u16string Vm::captureStack() {
    std::u16string s;
    int n = 0;
    for (size_t i = frames.size(); i-- > 0 && n < 14; ++n) {
        Frame& f = frames[i];
        size_t at = f.pc - f.code->code.data();
        if (at > 0) --at;
        uint32_t line = at < f.code->lines.size() ? f.code->lines[at] : 0;
        s += u"\n    at ";
        s += (f.code->name && !f.code->name->s.empty()) ? f.code->name->s : std::u16string(u"<anonymous>");
        s += u" (" + utf8to16(f.code->file) + u":" + ascii16(std::to_string(line)) + u")";
    }
    return s;
}

static const char* kErrNames[] = {"Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError"};

JSObject* Vm::newError(Err type, const std::u16string& msg) {
    JSObject* e = newObject(errorProtos[(int)type] ? errorProtos[(int)type] : objectProto, Cls::Error);
    e->add(PropKey::of(a.message), strv(msg), PF_W | PF_C);
    e->add(PropKey::of(a.stack), strv(ascii16(kErrNames[(int)type]) + u": " + msg + captureStack()), PF_W | PF_C);
    return e;
}
void Vm::throwValue(Value v) {
    pending = v;
    throw JSThrow{};
}
void Vm::throwError(Err type, const std::u16string& msg) {
    JSObject* e = newError(type, msg);
    if (traceThrows && onConsole && !terminated) onConsole(2, u"[throw] " + utf8to16(kErrNames[(int)type]) + u": " + msg + captureStack());
    freshError = e;
    throwValue(Value::cell(e));
}
void Vm::throwError(Err type, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    throwError(type, utf8to16(buf));
}

std::u16string Vm::describeError(Value e) {
    if (e.isObject() && e.asObject()->cls == Cls::Error) {
        try {
            Value st = get(e, a.stack);
            if (st.isString()) return st.asString()->s;
            return toStr16(get(e, a.name)) + u": " + toStr16(get(e, a.message));
        } catch (JSThrow&) {
            pending = Value::undef();
        }
    }
    try {
        return u"Uncaught " + inspect(e);
    } catch (JSThrow&) {
        pending = Value::undef();
        return u"Uncaught exception";
    }
}

// ---------------------------------------------------------------------------- conversions
bool Vm::toBool(Value v) {
    if (v.isBool()) return v.isTrue();
    if (v.isInt()) return v.asInt() != 0;
    if (v.isDouble()) {
        double d = v.asDouble();
        return d == d && d != 0;
    }
    if (v.isNullish() || v.isEmpty()) return false;
    if (v.isString()) return ((JSString*)v.asCell())->length() != 0;
    return true;
}

double Vm::stringToNumber(const std::u16string& s) {
    size_t i = 0, n = s.size();
    auto ws = [](char16_t c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0xA0 || c == 0xFEFF || c == 0x2028 || c == 0x2029; };
    while (i < n && ws(s[i])) ++i;
    while (n > i && ws(s[n - 1])) --n;
    if (i == n) return 0;
    std::string t;
    for (size_t k = i; k < n; ++k) {
        if (s[k] > 127) return NAN;
        t += (char)s[k];
    }
    if (t == "Infinity" || t == "+Infinity") return INFINITY;
    if (t == "-Infinity") return -INFINITY;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X' || t[1] == 'o' || t[1] == 'O' || t[1] == 'b' || t[1] == 'B')) {
        int radix = (t[1] | 0x20) == 'x' ? 16 : (t[1] | 0x20) == 'o' ? 8 : 2;
        double v = 0;
        for (size_t k = 2; k < t.size(); ++k) {
            char c = t[k];
            int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : 99;
            if (d >= radix) return NAN;
            v = v * radix + d;
        }
        return v;
    }
    for (char c : t)
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')) return NAN;
    char* end = nullptr;
    double v = strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size() || end == t.c_str()) return NAN;
    return v;
}

double Vm::toNumber(Value v) {
    if (v.isInt()) return v.asInt();
    if (v.isDouble()) return v.asDouble();
    if (v.isUndef() || v.isEmpty()) return NAN;
    if (v.isNull()) return 0;
    if (v.isBool()) return v.isTrue() ? 1 : 0;
    if (v.isString()) return stringToNumber(v.asString()->s);
    if (v.isSymbol()) throwError(Err::TypeError, "Cannot convert a Symbol value to a number");
    return toNumber(toPrimitive(v, 1));
}
int32_t Vm::toInt32(Value v) {
    if (v.isInt()) return v.asInt();
    double d = toNumber(v);
    if (!std::isfinite(d)) return 0;
    double m = std::fmod(std::trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}
double Vm::toInteger(Value v) {
    if (v.isInt()) return v.asInt();
    double d = toNumber(v);
    if (d != d) return 0;
    return std::trunc(d);
}

JSString* Vm::numberToString(double d, int radix) {
    if (d != d) return atom("NaN");
    if (d == INFINITY) return atom("Infinity");
    if (d == -INFINITY) return atom("-Infinity");
    if (d == 0) return atom("0");
    char buf[128];
    if (radix != 10) {
        bool neg = d < 0;
        if (neg) d = -d;
        double ip = std::floor(d), fp = d - ip;
        std::string s;
        if (ip == 0) s = "0";
        while (ip >= 1) {
            int dg = (int)std::fmod(ip, radix);
            s.insert(s.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[dg]);
            ip = std::floor(ip / radix);
        }
        if (fp > 0) {
            s += '.';
            for (int k = 0; k < 52 && fp > 0; ++k) {
                fp *= radix;
                int dg = (int)fp;
                s += "0123456789abcdefghijklmnopqrstuvwxyz"[dg];
                fp -= dg;
            }
        }
        if (neg) s.insert(s.begin(), '-');
        return str(s);
    }
    if (d == std::trunc(d) && std::fabs(d) < 1e15) {
        snprintf(buf, sizeof buf, "%.0f", d);
        return str(buf);
    }
    auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    bool neg = sci[0] == '-';
    if (neg) sci.erase(0, 1);
    size_t e = sci.find('e');
    std::string digits;
    for (char c : sci.substr(0, e))
        if (c != '.') digits += c;
    int exp10 = atoi(sci.c_str() + e + 1);
    int k = (int)digits.size(), n = exp10 + 1;
    std::string out;
    if (k <= n && n <= 21) {
        out = digits + std::string(n - k, '0');
    } else if (0 < n && n <= 21) {
        out = digits.substr(0, n) + "." + digits.substr(n);
    } else if (-6 < n && n <= 0) {
        out = "0." + std::string(-n, '0') + digits;
    } else {
        out = digits.substr(0, 1);
        if (k > 1) out += "." + digits.substr(1);
        out += "e";
        out += (n - 1 >= 0 ? "+" : "-");
        out += std::to_string(std::abs(n - 1));
    }
    if (neg) out.insert(out.begin(), '-');
    return str(out);
}

JSString* Vm::toString(Value v) {
    if (v.isString()) return v.asString();
    if (v.isInt()) {
        int i = v.asInt();
        if (i >= 0 && i < 10) {
            char16_t c = (char16_t)('0' + i);
            return atom(std::u16string_view(&c, 1));
        }
        return str(std::to_string(i));
    }
    if (v.isDouble()) return numberToString(v.asDouble());
    if (v.isUndef() || v.isEmpty()) return a.undefined;
    if (v.isNull()) return a.null_;
    if (v.isBool()) return v.isTrue() ? a.true_ : a.false_;
    if (v.isSymbol()) throwError(Err::TypeError, "Cannot convert a Symbol value to a string");
    return toString(toPrimitive(v, 2));
}

JSObject* Vm::toObject(Value v) {
    if (v.isObject()) return v.asObject();
    if (v.isNullish()) throwError(Err::TypeError, "Cannot convert undefined or null to object");
    JSPrimitive* p = alloc<JSPrimitive>();
    p->prim = v;
    if (v.isString()) {
        p->cls = Cls::String;
        p->proto = stringProto;
    } else if (v.isNumber()) {
        p->cls = Cls::Number;
        p->proto = numberProto;
    } else if (v.isBool()) {
        p->cls = Cls::Boolean;
        p->proto = booleanProto;
    } else {
        p->cls = Cls::SymbolObj;
        p->proto = symbolProto;
    }
    return p;
}

Value Vm::toPrimitive(Value v, int hint) {
    if (!v.isObject()) return v;
    JSObject* o = v.asObject();
    Value ex = getObj(o, PropKey::of(symToPrimitive), v);
    if (!ex.isNullish()) {
        Value h = Value::cell(atom(hint == 1 ? "number" : hint == 2 ? "string" : "default"));
        Value r = call(ex, v, &h, 1);
        if (r.isObject()) throwError(Err::TypeError, "Cannot convert object to primitive value");
        return r;
    }
    if (hint == 0 && o->cls == Cls::Date) hint = 2;
    JSString* order[2] = {hint == 2 ? a.toString : a.valueOf, hint == 2 ? a.valueOf : a.toString};
    for (JSString* m : order) {
        Value f = getObj(o, PropKey::of(m), v);
        if (isCallable(f)) {
            Value r = call(f, v, nullptr, 0);
            if (!r.isObject()) return r;
        }
    }
    throwError(Err::TypeError, "Cannot convert object to primitive value");
}

static bool arrayIndex(const std::u16string& s, uint32_t& out) {
    size_t n = s.size();
    if (n == 0 || n > 10 || s[0] < '0' || s[0] > '9') return false;
    if (n > 1 && s[0] == '0') return false;
    uint64_t v = 0;
    for (char16_t c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v >= 4294967295ull) return false;
    out = (uint32_t)v;
    return true;
}

PropKey Vm::toKey(Value v) {
    if (v.isInt()) {
        int i = v.asInt();
        if (i >= 0) return PropKey::index((uint32_t)i);
    } else if (v.isString()) {
        JSString* s = v.asString();
        uint32_t idx;
        if (arrayIndex(s->s, idx)) return PropKey::index(idx);
        return PropKey::of(atomOf(s));
    } else if (v.isSymbol()) {
        return PropKey::of(v.asCell());
    } else if (v.isDouble()) {
        double d = v.asDouble();
        if (d >= 0 && d < 4294967295.0 && d == std::floor(d)) return PropKey::index((uint32_t)d);
    } else if (v.isObject()) {
        return toKey(toPrimitive(v, 2));
    }
    return PropKey::of(atomOf(toString(v)));
}
Value Vm::keyToValue(PropKey k) {
    if (k.isIndex()) return strv(std::to_string(k.asIndex()));
    return Value::cell(k.cell());
}

JSString* Vm::typeOf(Value v) {
    if (v.isUndef() || v.isEmpty()) return a.undefined;
    if (v.isNull()) return a.object;
    if (v.isBool()) return a.boolean;
    if (v.isNumber()) return a.number;
    if (v.isString()) return a.string;
    if (v.isSymbol()) return a.symbol;
    return isCallable(v) ? a.function : a.object;
}
bool Vm::isCallable(Value v) {
    if (!v.isObject()) return false;
    Cls c = v.asObject()->cls;
    return c == Cls::Function || c == Cls::Native || c == Cls::Bound || (c == Cls::Proxy && ((JSProxy*)v.asObject())->callable);
}
bool Vm::isConstructor(Value v) {
    if (!v.isObject()) return false;
    JSObject* o = v.asObject();
    if (o->cls == Cls::Function) {
        FuncProto* p = ((JSFunction*)o)->code;
        return !p->isArrow && !p->isMethod && !p->isGenerator && !p->isAsync;
    }
    if (o->cls == Cls::Native) return (o->oflags & OF_CTOR) != 0;
    if (o->cls == Cls::Bound) return isConstructor(((JSBound*)o)->target);
    if (o->cls == Cls::Proxy) return ((JSProxy*)o)->constructible;
    return false;
}

bool Vm::strictEquals(Value x, Value y) {
    if (x.b == y.b) return !(x.isDouble() && x.asDouble() != x.asDouble());
    if (x.isNumber() && y.isNumber()) return x.number() == y.number();
    if (x.isString() && y.isString()) return x.asString()->s == y.asString()->s;
    return false;
}
bool Vm::sameValueZero(Value x, Value y) {
    if (x.b == y.b) return true;
    if (x.isNumber() && y.isNumber()) {
        double p = x.number(), q = y.number();
        return p == q || (p != p && q != q);
    }
    if (x.isString() && y.isString()) return x.asString()->s == y.asString()->s;
    return false;
}
bool Vm::sameValue(Value x, Value y) {
    if (x.isNumber() && y.isNumber()) {
        double p = x.number(), q = y.number();
        if (p != p && q != q) return true;
        return p == q && std::signbit(p) == std::signbit(q);
    }
    return sameValueZero(x, y);
}
bool Vm::looseEquals(Value x, Value y) {
    for (;;) {
        if (x.isNumber() && y.isNumber()) return x.number() == y.number();
        if (x.isString() && y.isString()) return x.asString()->s == y.asString()->s;
        if (x.isNullish() && y.isNullish()) return true;
        if (x.isNullish() || y.isNullish()) return false;
        if (x.isObject() && y.isObject()) return x.b == y.b;
        if (x.isBool() && y.isBool()) return x.b == y.b;
        if (x.isSymbol() || y.isSymbol()) return x.b == y.b;
        if (x.isBool()) x = Value::i32(x.isTrue() ? 1 : 0);
        else if (y.isBool()) y = Value::i32(y.isTrue() ? 1 : 0);
        else if (x.isObject()) x = toPrimitive(x, 0);
        else if (y.isObject()) y = toPrimitive(y, 0);
        else if (x.isString()) x = Value::num(stringToNumber(x.asString()->s));
        else if (y.isString()) y = Value::num(stringToNumber(y.asString()->s));
        else return false;
    }
}

Value Vm::concat(Value x, Value y) {
    JSString* p = (JSString*)x.asCell();
    JSString* q = (JSString*)y.asCell();
    size_t lp = p->length(), lq = q->length();
    if (lq == 0) return x;
    if (lp == 0) return y;
    if (lp + lq > 0x1FFFFFFF) throwError(Err::RangeError, "Invalid string length");
    if (lp + lq < 40) {
        const std::u16string& ps = x.asString()->s;
        const std::u16string& qs = y.asString()->s;
        std::u16string r;
        r.reserve(lp + lq);
        r += ps;
        r += qs;
        return Value::cell(str(std::move(r)));
    }
    JSString* r = alloc<JSString>();
    r->left = p;
    r->right = q;
    r->ropeLen = (uint32_t)(lp + lq);
    gc.noteExternal((lp + lq) / 8 + 32);
    return Value::cell(r);
}

Value Vm::add(Value x, Value y) {
    if (x.isString() && y.isString()) return concat(x, y);
    Value px = toPrimitive(x, 0), py = toPrimitive(y, 0);
    if (px.isString() || py.isString()) {
        if (!px.isString()) px = Value::cell(toString(px));
        if (!py.isString()) py = Value::cell(toString(py));
        return concat(px, py);
    }
    return Value::num(toNumber(px) + toNumber(py));
}

int Vm::compare(Value x, Value y) {
    if (!(x.isNumber() && y.isNumber())) {
        x = toPrimitive(x, 1);
        y = toPrimitive(y, 1);
        if (x.isString() && y.isString()) {
            int c = x.asString()->s.compare(y.asString()->s);
            return c < 0 ? -1 : c > 0 ? 1 : 0;
        }
    }
    double p = toNumber(x), q = toNumber(y);
    if (p != p || q != q) return 2;
    return p < q ? -1 : p > q ? 1 : 0;
}

// ---------------------------------------------------------------------------- objects
Value Vm::fnProtoProp(JSFunction* f) {
    PropKey k = PropKey::of(a.prototype);
    if (Prop* p = f->find(k)) return p->val;
    FuncProto* c = f->code;
    if (c->isArrow || c->isMethod || (c->isAsync && !c->isGenerator)) return Value::undef();
    JSObject* po = newObject(c->isGenerator ? (c->isAsync ? asyncGeneratorProto : generatorProto) : objectProto);
    if (!c->isGenerator) po->add(PropKey::of(a.constructor), Value::cell(f), PF_W | PF_C);
    f->add(k, Value::cell(po), PF_W);
    return Value::cell(po);
}

// Properties that exist without being stored: array elements and length, string characters, function metadata.
static bool virtualProp(Vm& vm, JSObject* o, PropKey k, Prop& out) {
    switch (o->cls) {
        case Cls::Array:
        case Cls::Arguments: {
            JSArray* arr = (JSArray*)o;
            if (k.isIndex()) {
                uint32_t i = k.asIndex();
                if (i < arr->elems.size() && !arr->elems[i].isEmpty()) {
                    out = {k, arr->elems[i], (uint8_t)((o->oflags & OF_FROZEN) ? PF_E : (o->oflags & OF_SEALED) ? (PF_W | PF_E) : PF_ALL)};
                    return true;
                }
            } else if (k.cell() == vm.a.length && o->cls == Cls::Array) {
                out = {k, Value::num((double)arrayLen(arr)), (uint8_t)((o->oflags & (OF_LENRO | OF_FROZEN)) ? 0 : PF_W)};
                return true;
            } else if (k.cell() == vm.a.length && !(o->oflags & OF_NO_LENGTH)) {  // arguments.length: an ordinary property
                out = {k, Value::num((double)arr->elems.size()), (uint8_t)(PF_W | PF_C)};
                return true;
            }
            return false;
        }
        case Cls::String: {
            const std::u16string& s = ((JSPrimitive*)o)->prim.asString()->s;
            if (k.isIndex()) {
                if (k.asIndex() < s.size()) {
                    out = {k, vm.strv(std::u16string(1, s[k.asIndex()])), PF_E};
                    return true;
                }
            } else if (k.cell() == vm.a.length) {
                out = {k, Value::i32((int32_t)s.size()), 0};
                return true;
            }
            return false;
        }
        case Cls::Function: {
            if (k.isIndex()) return false;
            JSFunction* f = (JSFunction*)o;
            if (k.cell() == vm.a.prototype) {
                Value v = vm.fnProtoProp(f);
                if (v.isUndef()) return false;
                out = {k, v, PF_W};
                return true;
            }
            if (k.cell() == vm.a.name && !(o->oflags & OF_NO_NAME)) {
                out = {k, Value::cell(f->code->name ? f->code->name : vm.a.empty), PF_C};
                return true;
            }
            if (k.cell() == vm.a.length && !(o->oflags & OF_NO_LENGTH)) {
                out = {k, Value::i32(f->code->length), PF_C};
                return true;
            }
            return false;
        }
        case Cls::Native: {
            if (k.isIndex()) return false;
            JSNative* n = (JSNative*)o;
            if (k.cell() == vm.a.name && !(o->oflags & OF_NO_NAME)) {
                out = {k, Value::cell(n->name ? n->name : vm.a.empty), PF_C};
                return true;
            }
            if (k.cell() == vm.a.length && !(o->oflags & OF_NO_LENGTH)) {
                out = {k, Value::i32(n->length), PF_C};
                return true;
            }
            return false;
        }
        case Cls::TypedArray: {
            if (!k.isIndex()) return false;
            JSTypedArray* t = (JSTypedArray*)o;
            if (k.asIndex() >= t->length) return false;
            out = {k, typedGet(t, k.asIndex()), (uint8_t)(PF_W | PF_E)};
            return true;
        }
        case Cls::Host: {
            JSHost* h = (JSHost*)o;
            if (!h->hooks || !h->hooks->get || k.isSymbol()) return false;
            Value v;
            if (!h->hooks->get(vm, o, k, v)) return false;
            out = {k, v, h->hooks->attrs};
            return true;
        }
        default: return false;
    }
}

bool Vm::getOwn(JSObject* o, PropKey k, Prop& out) {
    if (o->cls == Cls::Proxy) {
        PropDesc d;
        if (!proxyGetOwn(o, k, d)) return false;
        uint8_t flags = (uint8_t)((d.enumerable ? PF_E : 0) | (d.configurable ? PF_C : 0));
        if (d.hasGet || d.hasSet) {
            Accessor* acc = alloc<Accessor>();
            acc->get = d.get;
            acc->set = d.set;
            out = {k, Value::cell(acc), (uint8_t)(flags | PF_ACC)};
        } else
            out = {k, d.value, (uint8_t)(flags | (d.writable ? PF_W : 0))};
        return true;
    }
    if (Prop* p = o->find(k)) {
        out = *p;
        return true;
    }
    return virtualProp(*this, o, k, out);
}

Value Vm::getObj(JSObject* o, PropKey k, Value receiver) {
    for (JSObject* cur = o; cur; cur = cur->proto) {
        if (cur->cls == Cls::Proxy) return proxyGet(cur, k, receiver);
        if (k.isIndex() && cur->cls == Cls::TypedArray) {
            JSTypedArray* t = (JSTypedArray*)cur;
            return k.asIndex() < t->length ? typedGet(t, k.asIndex()) : Value::undef();
        }
        if (k.isIndex() && (cur->cls == Cls::Array || cur->cls == Cls::Arguments)) {
            JSArray* arr = (JSArray*)cur;
            uint32_t i = k.asIndex();
            if (i < arr->elems.size() && !arr->elems[i].isEmpty()) return arr->elems[i];
            if (!(cur->oflags & OF_SLOWIDX)) continue;
        }
        if (Prop* p = cur->find(k)) {
            if (p->flags & PF_ACC) {
                Accessor* acc = (Accessor*)p->val.asCell();
                if (acc->get.isUndef()) return Value::undef();
                return call(acc->get, receiver, nullptr, 0);
            }
            return p->val;
        }
        if (cur->cls != Cls::Object) {
            Prop tmp;
            if (virtualProp(*this, cur, k, tmp)) return tmp.val;
        }
    }
    return Value::undef();
}

JSObject* Vm::protoOf(Value v) {
    if (v.isObject()) return v.asObject()->proto;
    if (v.isString()) return stringProto;
    if (v.isNumber()) return numberProto;
    if (v.isBool()) return booleanProto;
    if (v.isSymbol()) return symbolProto;
    return nullptr;
}

static std::string keyName(Vm& vm, PropKey k) {
    if (k.isIndex()) return std::to_string(k.asIndex());
    if (k.isSymbol()) return "Symbol(" + utf16to8(((Symbol*)k.cell())->desc ? ((Symbol*)k.cell())->desc->s : u"") + ")";
    return utf16to8(k.str()->s);
}

Value Vm::get(Value target, PropKey k) {
    if (target.isObject()) return getObj(target.asObject(), k, target);
    if (target.isString()) {
        if (!k.isIndex() && k.cell() == a.length) return Value::i32((int32_t)((JSString*)target.asCell())->length());
        const std::u16string& s = target.asString()->s;
        if (k.isIndex()) {
            if (k.asIndex() < s.size()) return strv(std::u16string(1, s[k.asIndex()]));
            return Value::undef();
        }
        if (k.cell() == a.length) return Value::i32((int32_t)s.size());
        return getObj(stringProto, k, target);
    }
    if (target.isNullish() || target.isEmpty())
        throwError(Err::TypeError, "Cannot read properties of %s (reading '%s')", target.isNull() ? "null" : "undefined", keyName(*this, k).c_str());
    return getObj(protoOf(target), k, target);
}

bool Vm::setObj(JSObject* o, PropKey k, Value v, Value receiver) {
    if (o->cls == Cls::Proxy) return proxySet(o, k, v, receiver);
    if (o->cls == Cls::Host && !k.isSymbol()) {
        JSHost* h = (JSHost*)o;
        if (h->hooks && h->hooks->set && h->hooks->set(*this, o, k, v)) return true;
    }
    if (o->cls == Cls::TypedArray && k.isIndex()) {
        typedSet(*this, (JSTypedArray*)o, k.asIndex(), v);
        return true;
    }
    bool self = receiver.isObject() && receiver.asObject() == o;
    if (o->cls == Cls::Array || o->cls == Cls::Arguments) {
        JSArray* arr = (JSArray*)o;
        if (k.isIndex()) {
            uint32_t i = k.asIndex();
            size_t size = arr->elems.size();
            if (i < size && !arr->elems[i].isEmpty()) {  // an existing plain element
                if (o->oflags & OF_FROZEN) return false;
                if (self) {
                    arr->elems[i] = v;
                    return true;
                }
            } else if (self && !(o->oflags & (OF_SLOWIDX | OF_NOEXT | OF_LENRO)) && !protoIndexProps &&
                       (arr->proto == arrayProto || (o->cls == Cls::Arguments && arr->proto == objectProto)) && (i < size || i - size <= (1u << 22))) {
                // a hole or a slot past the end, and nothing on the prototypes that could intercept the write
                if (i >= size) arr->elems.resize((size_t)i + 1, Value::empty());
                arr->elems[i] = v;
                return true;
            }
        } else if (self && o->cls == Cls::Array && !k.isSymbol() && k.cell() == a.length) {
            PropDesc d;
            d.hasValue = true;
            d.value = v;
            return defineOwnDesc(o, k, d);
        }
    }
    // OrdinarySet (ECMA-262 10.1.9.2): find the property on the object or its prototypes
    bool ownMissing = false;
    for (JSObject* cur = o; cur; cur = cur->proto) {
        if (cur->cls == Cls::Proxy) return proxySet(cur, k, v, receiver);
        Prop* p = cur->find(k);
        if (!p && cur->cls != Cls::Object && cur->cls != Cls::Host) {
            // properties that are not stored: array elements, a function's name and length, a string's characters.
            // A function's prototype is created on first use and is then an ordinary property.
            Prop vp;
            if (virtualProp(*this, cur, k, vp)) {
                p = cur->find(k);
                if (!p) {
                    if (!(vp.flags & PF_W)) return false;
                    break;
                }
            }
        }
        if (!p) {
            if (cur == o) ownMissing = true;
            continue;
        }
        if (p->flags & PF_ACC) {
            Accessor* acc = (Accessor*)p->val.asCell();
            if (acc->set.isUndef()) return false;
            call(acc->set, receiver, &v, 1);
            return true;
        }
        if (!(p->flags & PF_W)) return false;
        if (cur == o && self) {
            p->val = v;
            return true;
        }
        break;
    }
    // the value goes on the receiver: an existing writable data property is updated, otherwise one is created
    if (!receiver.isObject()) return false;
    JSObject* r = receiver.asObject();
    if (r->cls != Cls::Host && !(self && ownMissing)) {
        Prop existing{};
        if (getOwn(r, k, existing)) {
            if ((existing.flags & PF_ACC) || !(existing.flags & PF_W)) return false;
            PropDesc d;
            d.hasValue = true;
            d.value = v;
            return defineOwnDesc(r, k, d);
        }
    }
    if (((r->cls == Cls::Array || r->cls == Cls::Arguments || r->cls == Cls::TypedArray) && k.isIndex()) || r->cls == Cls::Proxy) return defineElement(r, k, v, PF_ALL);
    if (r->oflags & (OF_NOEXT | OF_FROZEN)) return false;
    if (k.isIndex() && (r == arrayProto || r == objectProto)) protoIndexProps = true;
    r->add(k, v, (r->cls == Cls::Function && !k.isIndex() && k.cell() == a.prototype) ? PF_W : PF_ALL);
    return true;
}

bool Vm::set(Value target, PropKey k, Value v) {
    if (target.isObject()) return setObj(target.asObject(), k, v, target);
    if (target.isNullish() || target.isEmpty())
        throwError(Err::TypeError, "Cannot set properties of %s (setting '%s')", target.isNull() ? "null" : "undefined", keyName(*this, k).c_str());
    // primitives: only a setter on the prototype chain can take the value (ECMA-262 10.1.9.2, receiver not an object)
    for (JSObject* cur = protoOf(target); cur; cur = cur->proto)
        if (Prop* p = cur->find(k)) {
            if (p->flags & PF_ACC) {
                Accessor* acc = (Accessor*)p->val.asCell();
                if (!acc->set.isUndef()) {
                    call(acc->set, target, &v, 1);
                    return true;
                }
            }
            return false;
        }
    return false;
}

void Vm::failedSet(Value target, PropKey k) {
    std::string name = keyName(*this, k);
    if (!target.isObject()) throwError(Err::TypeError, "Cannot create property '%s' on %s", name.c_str(), utf16to8(inspect(target, 0)).c_str());
    JSObject* o = target.asObject();
    for (JSObject* cur = o; cur; cur = cur->proto)
        if (Prop* p = cur->find(k)) {
            if (p->flags & PF_ACC) throwError(Err::TypeError, "Cannot set property '%s', which has only a getter", name.c_str());
            throwError(Err::TypeError, "Cannot assign to read-only property '%s'", name.c_str());
        }
    Prop vp;
    if (getOwn(o, k, vp)) throwError(Err::TypeError, "Cannot assign to read-only property '%s'", name.c_str());
    throwError(Err::TypeError, "Cannot add property '%s': the object is not extensible", name.c_str());
}

bool Vm::defineOwn(JSObject* o, PropKey k, Value v, uint8_t flags) {
    if (o->cls == Cls::Proxy) return defineElement(o, k, v, flags);
    if (o->cls == Cls::TypedArray && k.isIndex()) {
        typedSet(*this, (JSTypedArray*)o, k.asIndex(), v);
        return true;
    }
    if ((o->cls == Cls::Array || o->cls == Cls::Arguments) && k.isIndex()) {
        JSArray* arr = (JSArray*)o;
        uint32_t i = k.asIndex();
        size_t size = arr->elems.size();
        if (flags != PF_ALL || (o->oflags & (OF_SLOWIDX | OF_NOEXT | OF_LENRO | OF_FROZEN | OF_SEALED)) || (i >= size && i - size > (1u << 22)))
            return defineElement(o, k, v, flags);
        if (i >= size) arr->elems.resize((size_t)i + 1, Value::empty());
        arr->elems[i] = v;
        return true;
    }
    if (o->cls == Cls::Array && !k.isIndex() && !k.isSymbol() && k.cell() == a.length) return setObj(o, k, v, Value::cell(o));
    if (k.isIndex() && (o == arrayProto || o == objectProto)) protoIndexProps = true;
    if (Prop* p = o->find(k)) {
        p->val = v;
        p->flags = flags;
        return true;
    }
    if (o->oflags & OF_NOEXT) return false;
    o->add(k, v, flags);
    return true;
}

void Vm::defineAccessor(JSObject* o, PropKey k, Value getter, Value setter, uint8_t flags) {
    if ((k.isIndex() && (o->cls == Cls::Array || o->cls == Cls::Arguments || o == arrayProto || o == objectProto)) || o->cls == Cls::Proxy) {
        PropDesc d;
        if (!getter.isEmpty()) d.hasGet = true, d.get = getter;
        if (!setter.isEmpty()) d.hasSet = true, d.set = setter;
        d.hasEnumerable = d.hasConfigurable = true;
        d.enumerable = (flags & PF_E) != 0;
        d.configurable = (flags & PF_C) != 0;
        defineOwnDesc(o, k, d);
        return;
    }
    Accessor* acc = nullptr;
    if (Prop* p = o->find(k)) {
        if (p->flags & PF_ACC) {
            acc = (Accessor*)p->val.asCell();
            p->flags = (uint8_t)(PF_ACC | (flags & (PF_E | PF_C)));
        } else
            o->remove(k);
    }
    if (!acc) {
        acc = alloc<Accessor>();
        o->add(k, Value::cell(acc), (uint8_t)(PF_ACC | (flags & (PF_E | PF_C))));
    }
    if (!getter.isEmpty()) acc->get = getter;
    if (!setter.isEmpty()) acc->set = setter;
}

bool Vm::hasOwn(JSObject* o, PropKey k) {
    if (o->cls == Cls::Host && !k.isSymbol()) {
        JSHost* h = (JSHost*)o;
        if (h->hooks && h->hooks->has) return o->find(k) || h->hooks->has(*this, o, k);
    }
    Prop tmp;
    return getOwn(o, k, tmp);
}
bool Vm::has(JSObject* o, PropKey k) {
    for (JSObject* cur = o; cur; cur = cur->proto) {
        if (cur->cls == Cls::Proxy) return proxyHas(cur, k);
        if (hasOwn(cur, k)) return true;
    }
    return false;
}
bool Vm::del(JSObject* o, PropKey k) {
    if (o->cls == Cls::Proxy) return proxyDelete(o, k);
    if (o->cls == Cls::Host && !k.isSymbol()) {
        JSHost* h = (JSHost*)o;
        if (h->hooks && h->hooks->del && h->hooks->del(*this, o, k)) return true;
        if (h->hooks && h->hooks->has && !(h->hooks->attrs & PF_C) && h->hooks->has(*this, o, k)) return false;
    }
    if ((o->cls == Cls::Array || o->cls == Cls::Arguments) && k.isIndex()) {
        JSArray* arr = (JSArray*)o;
        uint32_t i = k.asIndex();
        if (i < arr->elems.size() && !arr->elems[i].isEmpty()) {
            if (o->oflags & (OF_FROZEN | OF_SEALED)) return false;
            arr->elems[i] = Value::empty();
            return true;
        }
        if (!(o->oflags & OF_SLOWIDX)) return true;  // otherwise it may be kept as an ordinary property
    }
    Prop* p = o->find(k);
    if (!p) {
        if ((o->cls == Cls::Function || o->cls == Cls::Native || o->cls == Cls::Arguments) && !k.isIndex() && !k.isSymbol()) {
            // a function's name and length are configurable: deleting one leaves a mark, since neither is stored
            if (k.cell() == a.name) o->oflags |= OF_NO_NAME;
            if (k.cell() == a.length) o->oflags |= OF_NO_LENGTH;
            if (k.cell() == a.name || k.cell() == a.length) return true;
        }
        // a property that exists without being stored (an array's length, a string's characters) cannot go
        Prop vp;
        if (o->cls != Cls::Host && virtualProp(*this, o, k, vp) && !(vp.flags & PF_C)) return false;
        return true;
    }
    if (!(p->flags & PF_C) || (o->oflags & OF_FROZEN)) return false;
    o->remove(k);
    return true;
}

void Vm::ownKeys(JSObject* o, std::vector<PropKey>& out, bool strings, bool symbols, bool onlyEnumerable) {
    if (o->cls == Cls::Proxy) {
        std::vector<PropKey> all;
        proxyOwnKeys(o, all);
        for (PropKey k : all) {
            if (k.isSymbol() ? !symbols : !strings) continue;
            if (onlyEnumerable) {
                Prop p{};
                if (!getOwn(o, k, p) || !(p.flags & PF_E)) continue;
            }
            out.push_back(k);
        }
        return;
    }
    if (strings && o->cls == Cls::Host) {
        JSHost* h = (JSHost*)o;
        if (h->hooks && h->hooks->keys) h->hooks->keys(*this, o, out);
    }
    if (strings) {
        size_t firstIndex = out.size();
        if (o->cls == Cls::TypedArray)
            for (uint32_t i = 0; i < ((JSTypedArray*)o)->length; ++i) out.push_back(PropKey::index(i));
        if (o->cls == Cls::Array || o->cls == Cls::Arguments) {
            JSArray* arr = (JSArray*)o;
            for (uint32_t i = 0; i < arr->elems.size(); ++i)
                if (!arr->elems[i].isEmpty()) out.push_back(PropKey::index(i));
        } else if (o->cls == Cls::String) {
            size_t n = ((JSPrimitive*)o)->prim.asString()->s.size();
            for (uint32_t i = 0; i < n; ++i) out.push_back(PropKey::index(i));
        }
        size_t mark = out.size();
        for (auto& p : o->props)
            if (p.key.isIndex() && (!onlyEnumerable || (p.flags & PF_E))) out.push_back(p.key);
        if (out.size() > mark && out.size() - firstIndex > 1)  // index keys come first, in ascending order
            std::sort(out.begin() + firstIndex, out.end(), [](PropKey x, PropKey y) { return x.asIndex() < y.asIndex(); });
        if (!onlyEnumerable && (o->cls == Cls::Array || o->cls == Cls::String)) out.push_back(PropKey::of(a.length));
        for (auto& p : o->props)
            if (p.key.isString() && (!onlyEnumerable || (p.flags & PF_E))) out.push_back(p.key);
    }
    if (symbols)
        for (auto& p : o->props)
            if (p.key.isSymbol() && !((Symbol*)p.key.cell())->isPrivate && (!onlyEnumerable || (p.flags & PF_E))) out.push_back(p.key);
}

uint32_t Vm::lengthOf(Value v) {
    if (v.isObject() && v.asObject()->cls == Cls::Array) return arrayLen((JSArray*)v.asObject());
    if (v.isObject() && v.asObject()->cls == Cls::Arguments && !v.asObject()->find(PropKey::of(a.length)) && !(v.asObject()->oflags & OF_NO_LENGTH))
        return (uint32_t)((JSArray*)v.asObject())->elems.size();
    if (v.isObject() && v.asObject()->cls == Cls::TypedArray) return ((JSTypedArray*)v.asObject())->length;
    if (v.isString()) return (uint32_t)((JSString*)v.asCell())->length();
    double d = toInteger(get(v, a.length));
    if (d <= 0) return 0;
    return d > 4294967295.0 ? 4294967295u : (uint32_t)d;
}

bool Vm::instanceOf(Value v, Value ctor) {
    if (!ctor.isObject()) throwError(Err::TypeError, "Right-hand side of 'instanceof' is not callable");
    Value hi = getObj(ctor.asObject(), PropKey::of(symHasInstance), ctor);
    if (!hi.isNullish()) return toBool(call(hi, ctor, &v, 1));
    if (!isCallable(ctor)) throwError(Err::TypeError, "Right-hand side of 'instanceof' is not callable");
    while (ctor.asObject()->cls == Cls::Bound) ctor = ((JSBound*)ctor.asObject())->target;
    if (!v.isObject()) return false;
    Value pv = get(ctor, a.prototype);
    if (!pv.isObject()) return false;
    for (JSObject* p = getProto(v.asObject()); p; p = getProto(p))
        if (p == pv.asObject()) return true;
    return false;
}

JSObject* Vm::protoFromNewTarget(Value newTarget, JSObject* fallback) {
    if (newTarget.isObject()) {
        Value p = get(newTarget, a.prototype);
        if (p.isObject()) return p.asObject();
    }
    return fallback;
}

void Vm::copyDataProps(JSObject* dst, Value src, JSArray* excluded) {
    if (src.isNullish()) return;
    JSObject* so = toObject(src);
    std::vector<PropKey> keys;
    ownKeys(so, keys, true, true, true);
    for (PropKey k : keys) {
        if (excluded) {
            bool skip = false;
            for (Value e : excluded->elems)
                if (toKey(e) == k) skip = true;
            if (skip) continue;
        }
        defineOwn(dst, k, getObj(so, k, Value::cell(so)), PF_ALL);
    }
}

Value Vm::invoke(Value target, const char* method, const Value* args, int argc) {
    Value f = get(target, method);
    if (!isCallable(f)) throwError(Err::TypeError, "%s is not a function", method);
    return call(f, target, args, argc);
}

// ---------------------------------------------------------------------------- calls
struct DepthGuard {
    Vm& vm;
    explicit DepthGuard(Vm& v) : vm(v) {
        if (++vm.depth > kMaxDepth) {
            --vm.depth;
            vm.throwError(Err::RangeError, "Maximum call stack size exceeded");
        }
    }
    ~DepthGuard() { --vm.depth; }
};

JSFunction* Vm::makeClosure(FuncProto* p, Context* ctx, Frame* parent) {
    JSFunction* f = alloc<JSFunction>();
    f->proto = functionProto;
    f->code = p;
    f->ctx = ctx;
    if (p->isArrow && parent && parent->fn) f->home = parent->fn->home;
    return f;
}

void Vm::pushFrame(JSFunction* f, Value* base, int argc, Value newTarget, Value* ret, bool entry, bool construct) {
    FuncProto* p = f->code;
    if (base + p->nregs + argc + 8 >= stackEnd || frames.size() >= kMaxFrames)
        throwError(Err::RangeError, "Maximum call stack size exceeded");
    JSArray* argsArr = nullptr;
    if (p->needsArgs) {
        argsArr = newArray();
        argsArr->elems.assign(base + 2, base + 2 + argc);
    }
    int np = p->nparams;
    for (Value* r = base + 2 + std::min(argc, np), *e = base + p->nregs; r < e; ++r) *r = Value::undef();
    if (!p->isStrict && !p->isArrow) {
        if (base[1].isNullish()) base[1] = Value::cell(global);
        else if (!base[1].isObject() && !base[1].isEmpty()) base[1] = Value::cell(toObject(base[1]));
    }
    frames.push_back(Frame{f, p, p->code.data(), base, f->ctx, ret, newTarget, argsArr, nullptr, 0, entry, construct});
    stackTop = base + p->nregs;
}

Value Vm::callFunction(JSFunction* f, Value thisv, const Value* args, int argc, Value newTarget, bool construct) {
    FuncProto* p = f->code;
    if (!construct) {
        if (p->isClassCtor) throwError(Err::TypeError, "Class constructor %s cannot be invoked without 'new'", p->name ? utf16to8(p->name->s).c_str() : "");
        if (p->isGenerator) return p->isAsync ? startAsyncGenerator(f, thisv, args, argc) : startGenerator(f, thisv, args, argc);
        if (p->isAsync) return startAsync(f, thisv, args, argc);
    }
    DepthGuard guard(*this);
    if (++pollTicks == 0) pollInterrupt();
    Value* base = stackTop;
    if (base + argc + 8 >= stackEnd) throwError(Err::RangeError, "Maximum call stack size exceeded");
    base[0] = Value::cell(f);
    base[1] = thisv;
    for (int i = 0; i < argc; ++i) base[2 + i] = args[i];
    stackTop = base + 2 + argc;
    pushFrame(f, base, argc, newTarget, nullptr, true, construct);
    return runFrames(frames.size() - 1);
}

Value Vm::call(Value fn, Value thisv, const Value* args, int argc) {
    if (fn.isObject()) {
        JSObject* o = fn.asObject();
        switch (o->cls) {
            case Cls::Function: return callFunction((JSFunction*)o, thisv, args, argc, Value::undef(), false);
            case Cls::Native: {
                DepthGuard guard(*this);
                JSNative* n = (JSNative*)o;
                CallInfo ci{*this, thisv, (Value*)args, argc, Value::undef(), n};
                return n->fn(ci);
            }
            case Cls::Bound: {
                JSBound* b = (JSBound*)o;
                std::vector<Value> all(b->args);
                all.insert(all.end(), args, args + argc);
                RootScope rs(*this, all);
                return call(b->target, b->boundThis, all.data(), (int)all.size());
            }
            case Cls::Proxy:
                if (((JSProxy*)o)->callable) {
                    DepthGuard guard(*this);
                    return proxyCall(o, thisv, args, argc);
                }
                break;
            default: break;
        }
    }
    throwError(Err::TypeError, "%s is not a function", utf16to8(inspect(fn, 0)).c_str());
}

// PrivateMethodOrAccessorAdd (7.3.30): kind 0 method, 1 getter, 2 setter
void Vm::addPrivateMember(JSObject* o, PropKey k, int kind, Value fn) {
    Prop* p = o->find(k);
    if (kind == 0) {
        if (p) throwError(Err::TypeError, "Cannot initialize private methods of a class twice on the same object");
        o->add(k, fn, 0);
        return;
    }
    Accessor* acc;
    if (p) {
        if (!(p->flags & PF_ACC)) throwError(Err::TypeError, "Cannot initialize private methods of a class twice on the same object");
        acc = (Accessor*)p->val.asCell();
        if (!(kind == 1 ? acc->get : acc->set).isUndef()) throwError(Err::TypeError, "Cannot initialize private methods of a class twice on the same object");
    } else {
        acc = alloc<Accessor>();
        o->add(k, Value::cell(acc), PF_ACC);
    }
    (kind == 1 ? acc->get : acc->set) = fn;
}

// InitializeInstanceElements (7.3.34): private methods first, then the fields in order
void Vm::initFields(JSFunction* f, Value obj) {
    if (f->privates.isObject()) {
        JSArray* list = (JSArray*)f->privates.asObject();
        for (size_t i = 0; i + 2 < list->elems.size(); i += 3) addPrivateMember(obj.asObject(), toKey(list->elems[i]), list->elems[i + 1].asInt(), list->elems[i + 2]);
    }
    if (!f->fields.isObject()) return;
    JSArray* arr = (JSArray*)f->fields.asObject();
    for (size_t i = 0; i + 1 < arr->elems.size(); i += 2) {
        Value fnv = arr->elems[i + 1];
        Value v = fnv.isUndef() ? Value::undef() : call(fnv, obj, nullptr, 0);
        PropKey k = toKey(arr->elems[i]);
        if (k.isSymbol() && ((Symbol*)k.cell())->isPrivate) {  // PrivateFieldAdd: once per object, hidden, writable
            JSObject* o = obj.asObject();
            if (o->find(k)) throwError(Err::TypeError, "Cannot initialize a private field twice on the same object");
            o->add(k, v, PF_W);
            continue;
        }
        if (!defineOwn(obj.asObject(), k, v, PF_ALL)) throwError(Err::TypeError, "Cannot define a class field: the object is not extensible");
    }
}

Value Vm::construct(Value fn, const Value* args, int argc, Value newTarget) {
    if (!isConstructor(fn)) throwError(Err::TypeError, "%s is not a constructor", utf16to8(inspect(fn, 0)).c_str());
    JSObject* o = fn.asObject();
    if (o->cls == Cls::Function) {
        JSFunction* f = (JSFunction*)o;
        Value thisv = Value::empty();
        if (!f->code->isDerived) {
            JSObject* obj = newObject(protoFromNewTarget(newTarget, objectProto));
            thisv = Value::cell(obj);
            initFields(f, thisv);
        }
        return callFunction(f, thisv, args, argc, newTarget, true);
    }
    if (o->cls == Cls::Native) {
        DepthGuard guard(*this);
        JSNative* n = (JSNative*)o;
        CallInfo ci{*this, Value::undef(), (Value*)args, argc, newTarget, n};
        return n->fn(ci);
    }
    if (o->cls == Cls::Proxy) {
        DepthGuard guard(*this);
        return proxyConstruct(o, args, argc, newTarget);
    }
    JSBound* b = (JSBound*)o;
    std::vector<Value> all(b->args);
    all.insert(all.end(), args, args + argc);
    RootScope rs(*this, all);
    return construct(b->target, all.data(), (int)all.size(), newTarget == fn ? b->target : newTarget);
}

Value Vm::eval(std::u16string_view src, const std::string& file) {
    auto sp = std::make_shared<const std::u16string>(src);
    FuncProto* p = compileScript(*this, sp, file, false, nullptr);
    JSFunction* f = makeClosure(p, nullptr, nullptr);
    return callFunction(f, Value::cell(global), nullptr, 0, Value::undef(), false);
}

Value Vm::compileFunction(const std::u16string& params, const std::u16string& body, const std::string& file) {
    std::u16string src = u"(function anonymous(" + params + u"\n) {\n" + body + u"\n})";
    return eval(src, file);
}

bool Vm::run(std::u16string_view src, const std::string& file, std::u16string* result, std::u16string* error) {
    bool ok = true;
    try {
        Value v = eval(src, file);
        if (result) *result = inspect(v);
    } catch (JSThrow&) {
        Value e = pending;
        pending = Value::undef();
        if (error) *error = terminated ? std::u16string(u"Script stopped: it ran for too long") : describeError(e);
        ok = false;
    }
    if (terminated) jobQueue.clear();
    runMicrotasks();
    return ok;
}

void Vm::pollInterrupt() {
    if (!terminated && deadlineTick && GetTickCount64() > deadlineTick) terminated = true;
    if (terminated) throwError(Err::Error, "Script stopped: it ran for too long");
}

// ---------------------------------------------------------------------------- iteration
Value Vm::makeIterResult(Value v, bool done) {
    JSObject* r = newObject();
    r->add(PropKey::of(a.value), v, PF_ALL);
    r->add(PropKey::of(a.done), Value::boolean(done), PF_ALL);
    return Value::cell(r);
}

bool Vm::iterStep(JSIter* it, Value& out) {
    switch (it->cls) {
        case Cls::ArrayIter: {
            Value t = it->target;
            if (t.isUndef()) return false;
            uint32_t len = lengthOf(t);
            if (it->pos >= len) {
                it->target = Value::undef();
                return false;
            }
            uint32_t i = it->pos++;
            if (it->mode == 1) {
                out = Value::num(i);
                return true;
            }
            Value v = get(t, PropKey::index(i));
            if (it->mode == 0) {
                out = v;
            } else {
                JSArray* pair = newArray(2);
                pair->elems[0] = Value::num(i);
                pair->elems[1] = v;
                out = Value::cell(pair);
            }
            return true;
        }
        case Cls::StringIter: {
            const std::u16string& s = it->target.asString()->s;
            if (it->pos >= s.size()) return false;
            size_t n = 1;
            char16_t c = s[it->pos];
            if (c >= 0xD800 && c <= 0xDBFF && it->pos + 1 < s.size() && s[it->pos + 1] >= 0xDC00 && s[it->pos + 1] <= 0xDFFF) n = 2;
            out = strv(s.substr(it->pos, n));
            it->pos += (uint32_t)n;
            return true;
        }
        case Cls::MapIter:
        case Cls::SetIter: {
            if (!it->target.isObject()) return false;
            JSMap* m = (JSMap*)it->target.asObject();
            if (it->epoch != m->epoch) {  // the map compacted its entry list since the last step
                if (it->epoch + 1 == m->epoch)
                    it->pos -= (uint32_t)(std::lower_bound(m->removed.begin(), m->removed.end(), it->pos) - m->removed.begin());
                if (it->pos > m->entries.size()) it->pos = (uint32_t)m->entries.size();
                it->epoch = m->epoch;
            }
            while (it->pos < m->entries.size() && m->entries[it->pos].first.isEmpty()) ++it->pos;
            if (it->pos >= m->entries.size()) {
                it->target = Value::undef();
                return false;
            }
            auto& e = m->entries[it->pos++];
            if (it->mode == 2) {
                JSArray* pair = newArray(2);
                pair->elems[0] = e.first;
                pair->elems[1] = it->cls == Cls::SetIter ? e.first : e.second;
                out = Value::cell(pair);
            } else
                out = (it->mode == 1 || it->cls == Cls::SetIter) ? e.first : e.second;
            return true;
        }
        default: return false;
    }
}

bool Vm::iterate(Value iterable, const std::function<bool(Value)>& each) {
    if (iterable.isObject() && (iterable.asObject()->cls == Cls::Array || iterable.asObject()->cls == Cls::Arguments)) {
        JSArray* arr = (JSArray*)iterable.asObject();
        for (size_t i = 0; i < arr->elems.size(); ++i) {
            Value v = arr->elems[i];
            if (!each(v.isEmpty() ? Value::undef() : v)) return false;
        }
        return true;
    }
    if (iterable.isString()) {
        JSIter* it = newIter(Cls::StringIter, iterable, 0);
        Value v;
        while (iterStep(it, v))
            if (!each(v)) return false;
        return true;
    }
    Value m = get(iterable, PropKey::of(symIterator));
    if (!isCallable(m)) throwError(Err::TypeError, "%s is not iterable", utf16to8(inspect(iterable, 0)).c_str());
    Value itv = call(m, iterable, nullptr, 0);
    if (!itv.isObject()) throwError(Err::TypeError, "Result of the Symbol.iterator method is not an object");
    JSObject* ito = itv.asObject();
    if (ito->cls == Cls::ArrayIter || ito->cls == Cls::MapIter || ito->cls == Cls::SetIter || ito->cls == Cls::StringIter) {
        Value v;
        while (iterStep((JSIter*)ito, v))
            if (!each(v)) return false;
        return true;
    }
    Value next = get(itv, a.next);
    for (;;) {
        Value r = call(next, itv, nullptr, 0);
        if (!r.isObject()) throwError(Err::TypeError, "Iterator result is not an object");
        if (toBool(get(r, a.done))) return true;
        if (!each(get(r, a.value))) {
            Value ret = get(itv, a.return_);
            if (isCallable(ret)) call(ret, itv, nullptr, 0);
            return false;
        }
    }
}

// ---------------------------------------------------------------------------- generators and async functions
// True while Array.prototype[Symbol.iterator] and the array iterator's next() are the built-in ones, so that
// iterating a plain array can skip the protocol without anyone being able to tell.
bool Vm::plainArrayIteration() {
    Prop* it = arrayProto->find(PropKey::of(symIterator));
    if (!it || (it->flags & PF_ACC) || !it->val.isObject() || !arrayValuesFn.isObject() || it->val.asObject() != arrayValuesFn.asObject()) return false;
    Prop* next = arrayIterProto->find(PropKey::of(a.next));
    return next && !(next->flags & PF_ACC) && next->val.isObject() && arrayIterNextFn.isObject() && next->val.asObject() == arrayIterNextFn.asObject();
}

Value Vm::startGenerator(JSFunction* f, Value thisv, const Value* args, int argc) {
    FuncProto* p = f->code;
    JSGenerator* g = alloc<JSGenerator>();
    Value pv = p->isAsync ? Value::undef() : fnProtoProp(f);
    g->proto = pv.isObject() ? pv.asObject() : generatorProto;
    g->fn = f;
    g->regs.assign(p->nregs, Value::undef());
    g->regs[0] = Value::cell(f);
    if (!p->isStrict && !p->isArrow) {
        if (thisv.isNullish()) thisv = Value::cell(global);
        else if (!thisv.isObject()) thisv = Value::cell(toObject(thisv));
    }
    g->regs[1] = thisv;
    for (int i = 0; i < argc && i < p->nparams; ++i) g->regs[2 + i] = args[i];
    if (p->needsArgs) {
        g->args = newArray();
        g->args->elems.assign(args, args + argc);
    }
    g->pc = p->code.data();
    g->ctx = f->ctx;
    if (p->isGenerator) {  // bind the parameters now: an error in a default value belongs to the call
        Value keep = Value::cell(g);
        bool done = false;
        resumeGenerator(g, Value::undef(), 0, done);
        g->atStart = !done;
        return keep;
    }
    return Value::cell(g);
}

Value Vm::resumeGenerator(JSGenerator* g, Value sent, int mode, bool& done) {
    done = true;
    if (g->state == 2) throwError(Err::TypeError, "Generator is already running");
    if (g->state == 3) {
        if (mode == 1) throwValue(sent);
        return mode == 2 ? sent : Value::undef();
    }
    if (mode != 0 && (g->state == 0 || g->atStart)) {  // not started: there is nothing to unwind
        g->atStart = false;
        g->state = 3;
        g->regs.clear();
        if (mode == 1) throwValue(sent);
        return sent;
    }
    DepthGuard guard(*this);
    FuncProto* p = g->fn->code;
    Value* base = stackTop;
    if (base + p->nregs + 8 >= stackEnd || frames.size() >= kMaxFrames) throwError(Err::RangeError, "Maximum call stack size exceeded");
    std::copy(g->regs.begin(), g->regs.end(), base);
    bool wasYield = g->state == 1;
    frames.push_back(Frame{g->fn, p, g->pc, base, g->ctx, nullptr, g->newTarget, g->args, g, g->ctxDepth, true, false});
    stackTop = base + p->nregs;
    g->state = 2;
    if (wasYield && g->atStart) {
        g->atStart = false;  // the first next(): its argument has nowhere to go
    } else if (wasYield) {
        if (g->delegating) {  // inside yield*: the inner iterator is given the value, the throw or the return
            g->delegating = false;
            base[g->resumeReg] = sent;
            base[g->resumeReg + 1] = Value::i32(mode);
        } else if (mode == 0)
            base[g->resumeReg] = sent;
        else {
            // throw() raises the value at the yield. return() raises a signal no catch clause sees, so that only
            // the finally blocks on the way out run (ECMA-262 27.5.3.4 GeneratorResumeAbrupt).
            if (mode == 2) g->retVal = sent;
            pending = mode == 2 ? Value::cell(genReturnSignal) : sent;
            resumeThrow = true;
        }
    }
    yielded = false;
    Value v;
    try {
        v = runFrames(frames.size() - 1);
    } catch (JSThrow&) {
        g->state = 3;
        g->regs.clear();
        if (pending.isObject() && pending.asObject() == genReturnSignal) {  // return() has left the body
            pending = Value::undef();
            Value r = g->retVal;
            g->retVal = Value::undef();
            return r;
        }
        throw;
    }
    if (yielded) {
        yielded = false;
        g->state = 1;
        done = false;
        return v;
    }
    g->state = 3;
    g->regs.clear();
    return v;
}

static Value asyncNext(CallInfo& ci) {
    ci.vm.asyncStep((JSGenerator*)ci.callee->data.asCell(), ci.arg(0), 0);
    return Value::undef();
}
static Value asyncThrow(CallInfo& ci) {
    ci.vm.asyncStep((JSGenerator*)ci.callee->data.asCell(), ci.arg(0), 1);
    return Value::undef();
}

void Vm::asyncStep(JSGenerator* g, Value sent, int mode) {
    JSPromise* result = (JSPromise*)g->promise.asCell();
    Value v;
    bool done = false;
    try {
        v = resumeGenerator(g, sent, mode, done);
    } catch (JSThrow&) {
        Value e = pending;
        pending = Value::undef();
        rejectPromise(result, e);
        return;
    }
    if (done) {
        resolvePromise(result, v);
        return;
    }
    JSPromise* awaited = promiseResolve(v);
    JSNative* onF = newNative(asyncNext, "", 1);
    onF->data = Value::cell(g);
    JSNative* onR = newNative(asyncThrow, "", 1);
    onR->data = Value::cell(g);
    promiseThen(awaited, Value::cell(onF), Value::cell(onR), false);
}

Value Vm::startAsync(JSFunction* f, Value thisv, const Value* args, int argc) {
    JSGenerator* g = (JSGenerator*)startGenerator(f, thisv, args, argc).asCell();
    g->isAsync = true;
    JSPromise* p = newPromise();
    g->promise = Value::cell(p);
    asyncStep(g, Value::undef(), 0);
    return Value::cell(p);
}

// ---------------------------------------------------------------------------- promises
JSPromise* Vm::newPromise() {
    JSPromise* p = alloc<JSPromise>();
    p->proto = promiseProto;
    return p;
}

static void settle(Vm& vm, JSPromise* p, uint8_t state, Value v) {
    if (p->state) return;
    p->state = state;
    p->value = v;
    for (auto& r : p->reactions) vm.enqueueJob({(uint8_t)(state == 1 ? 0 : 1), state == 1 ? r.onFulfilled : r.onRejected, r.resultPromise, v});
    if (state == 2 && p->reactions.empty() && !p->handled) vm.rejections.push_back(Value::cell(p));
    p->reactions.clear();
}
void Vm::fulfillPromise(JSPromise* p, Value v) { settle(*this, p, 1, v); }
void Vm::rejectPromise(JSPromise* p, Value v) { settle(*this, p, 2, v); }

void Vm::resolvePromise(JSPromise* p, Value v) {
    if (p->state) return;
    if (v.isObject()) {
        if (v.asObject() == p) {
            rejectPromise(p, Value::cell(newError(Err::TypeError, u"Chaining cycle detected for promise")));
            return;
        }
        Value then;
        try {
            then = get(v, a.then);
        } catch (JSThrow&) {
            Value e = pending;
            pending = Value::undef();
            rejectPromise(p, e);
            return;
        }
        if (isCallable(then)) {
            enqueueJob({2, Value::cell(p), v, then});
            return;
        }
    }
    fulfillPromise(p, v);
}

JSPromise* Vm::promiseThen(JSPromise* p, Value onFul, Value onRej, bool wantResult) {
    JSPromise* res = wantResult ? newPromise() : nullptr;
    Value rv = res ? Value::cell(res) : Value::undef();
    p->handled = true;
    if (p->state == 0) p->reactions.push_back({onFul, onRej, rv});
    else enqueueJob({(uint8_t)(p->state == 1 ? 0 : 1), p->state == 1 ? onFul : onRej, rv, p->value});
    return res;
}

JSPromise* Vm::promiseResolve(Value v) {
    if (v.isObject() && v.asObject()->cls == Cls::Promise) return (JSPromise*)v.asObject();
    JSPromise* p = newPromise();
    resolvePromise(p, v);
    return p;
}

static Value settleFrom(CallInfo& ci, bool resolve) {
    JSObject* flag = ci.callee->data2.asObject();
    if (!flag->props.empty()) return Value::undef();
    flag->add(PropKey::index(0), Value::boolean(true), 0);
    JSPromise* p = (JSPromise*)ci.callee->data.asCell();
    if (resolve) ci.vm.resolvePromise(p, ci.arg(0));
    else ci.vm.rejectPromise(p, ci.arg(0));
    return Value::undef();
}
static Value resolveFn(CallInfo& ci) { return settleFrom(ci, true); }
static Value rejectFn(CallInfo& ci) { return settleFrom(ci, false); }

// The resolve/reject pair handed to promise executors and thenables; only the first call of either has an effect.
void makeResolvingFunctions(Vm& vm, JSPromise* p, Value out[2]) {
    JSObject* flag = vm.newObject(nullptr);
    JSNative* res = vm.newNative(resolveFn, "", 1);
    res->data = Value::cell(p);
    res->data2 = Value::cell(flag);
    JSNative* rej = vm.newNative(rejectFn, "", 1);
    rej->data = Value::cell(p);
    rej->data2 = Value::cell(flag);
    out[0] = Value::cell(res);
    out[1] = Value::cell(rej);
}

void Vm::runJob(const Job& j) {
    if (j.kind == 0 || j.kind == 1) {
        JSPromise* res = j.b.isObject() ? (JSPromise*)j.b.asObject() : nullptr;
        if (!isCallable(j.a)) {
            if (res) {
                if (j.kind == 0) resolvePromise(res, j.c);
                else rejectPromise(res, j.c);
            }
            return;
        }
        try {
            Value x = call(j.a, Value::undef(), &j.c, 1);
            if (res) resolvePromise(res, x);
        } catch (JSThrow&) {
            Value e = pending;
            pending = Value::undef();
            if (res) rejectPromise(res, e);
            else if (onConsole) onConsole(3, describeError(e));
        }
    } else if (j.kind == 2) {
        Value fns[2];
        makeResolvingFunctions(*this, (JSPromise*)j.a.asCell(), fns);
        try {
            call(j.c, j.b, fns, 2);
        } catch (JSThrow&) {
            Value e = pending;
            pending = Value::undef();
            call(fns[1], Value::undef(), &e, 1);
        }
    } else {
        try {
            call(j.a, Value::undef(), &j.b, 1);
        } catch (JSThrow&) {
            Value e = pending;
            pending = Value::undef();
            if (onConsole) onConsole(3, u"Uncaught " + describeError(e));
        }
    }
}

void Vm::runMicrotasks() {
    int guard = 0;
    while (!jobQueue.empty() && guard++ < 1000000) {
        if (terminated) {  // a stopped script must not keep itself alive through promise callbacks
            jobQueue.clear();
            break;
        }
        Job j = jobQueue.front();
        jobQueue.pop_front();
        runJob(j);
        if (gc.shouldCollect()) gc.collect();
    }
    std::vector<Value> rej;
    rej.swap(rejections);
    for (Value v : rej) {
        JSPromise* p = (JSPromise*)v.asCell();
        if (!p->handled && onConsole) onConsole(3, u"Uncaught (in promise) " + describeError(p->value));
    }
}

// ---------------------------------------------------------------------------- inspect (console output)
static void inspectInto(Vm& vm, Value v, int depth, bool quote, std::u16string& out, std::vector<JSObject*>& seen) {
    if (v.isString()) {
        if (!quote) {
            out += v.asString()->s;
            return;
        }
        out += u'\'';
        for (char16_t c : v.asString()->s) {
            if (c == '\n') out += u"\\n";
            else if (c == '\'') out += u"\\'";
            else out += c;
        }
        out += u'\'';
        return;
    }
    if (v.isSymbol()) {
        Symbol* s = (Symbol*)v.asCell();
        out += u"Symbol(" + (s->desc ? s->desc->s : std::u16string()) + u")";
        return;
    }
    if (!v.isObject()) {
        if (v.isDouble() && v.asDouble() == 0 && std::signbit(v.asDouble())) out += u"-0";
        else out += vm.toString(v)->s;
        return;
    }
    JSObject* o = v.asObject();
    for (JSObject* s : seen)
        if (s == o) {
            out += u"[Circular]";
            return;
        }
    if (vm.isCallable(v)) {
        Value nm = vm.get(v, vm.a.name);
        bool cls = o->cls == Cls::Function && ((JSFunction*)o)->code->isClassCtor;
        out += cls ? u"[class " : u"[Function: ";
        out += nm.isString() && !nm.asString()->s.empty() ? nm.asString()->s : std::u16string(u"(anonymous)");
        out += u"]";
        return;
    }
    if (o->cls == Cls::Error) {
        Value st = vm.get(v, vm.a.stack);
        out += st.isString() ? st.asString()->s : vm.toStr16(vm.get(v, vm.a.name)) + u": " + vm.toStr16(vm.get(v, vm.a.message));
        return;
    }
    if (o->cls == Cls::Boolean || o->cls == Cls::Number || o->cls == Cls::String) {
        out += u"[" + std::u16string(o->cls == Cls::String ? u"String" : o->cls == Cls::Number ? u"Number" : u"Boolean") + u": ";
        inspectInto(vm, ((JSPrimitive*)o)->prim, depth, true, out, seen);
        out += u"]";
        return;
    }
    if (o->cls == Cls::Date || o->cls == Cls::RegExp) {
        out += vm.toString(v)->s;
        return;
    }
    if (o->cls == Cls::Promise) {
        JSPromise* p = (JSPromise*)o;
        out += u"Promise { ";
        if (p->state == 0) out += u"<pending>";
        else {
            if (p->state == 2) out += u"<rejected> ";
            inspectInto(vm, p->value, depth - 1, true, out, seen);
        }
        out += u" }";
        return;
    }
    if (o->cls == Cls::ArrayBuffer) {
        out += u"ArrayBuffer(" + ascii16(std::to_string(((JSArrayBuffer*)o)->bytes.size())) + u")";
        return;
    }
    if (o->cls == Cls::TypedArray) {
        JSTypedArray* t = (JSTypedArray*)o;
        Value tag = vm.getObj(o, PropKey::of(vm.symToStringTag), v);
        out += (tag.isString() ? tag.asString()->s : std::u16string(u"TypedArray")) + u"(" + ascii16(std::to_string(t->length)) + u") [";
        for (uint32_t i = 0; i < t->length && i < 100; ++i) {
            if (i) out += u", ";
            out += vm.toString(typedGet(t, i))->s;
        }
        if (t->length > 100) out += u", ...";
        out += u"]";
        return;
    }
    if (depth < 0) {
        out += (o->cls == Cls::Array) ? u"[Array]" : u"[Object]";
        return;
    }
    seen.push_back(o);
    int count = 0;
    auto sep = [&] {
        if (count++) out += u", ";
    };
    if (o->cls == Cls::Array || o->cls == Cls::Arguments) {
        JSArray* arr = (JSArray*)o;
        out += u"[";
        for (size_t i = 0; i < arr->elems.size(); ++i) {
            sep();
            if (i >= 100) {
                out += u"... " + ascii16(std::to_string(arr->elems.size() - i)) + u" more items";
                break;
            }
            if (arr->elems[i].isEmpty()) out += u"<empty>";
            else inspectInto(vm, arr->elems[i], depth - 1, true, out, seen);
        }
        for (auto& p : o->props)
            if (p.key.isString() && (p.flags & PF_E)) {
                sep();
                out += p.key.str()->s + u": ";
                inspectInto(vm, p.val, depth - 1, true, out, seen);
            }
        out += u"]";
    } else if (o->cls == Cls::Map || o->cls == Cls::Set) {
        JSMap* m = (JSMap*)o;
        out += (o->cls == Cls::Map ? u"Map(" : u"Set(") + ascii16(std::to_string(m->live)) + u") {";
        for (auto& e : m->entries) {
            if (e.first.isEmpty()) continue;
            sep();
            inspectInto(vm, e.first, depth - 1, true, out, seen);
            if (o->cls == Cls::Map) {
                out += u" => ";
                inspectInto(vm, e.second, depth - 1, true, out, seen);
            }
        }
        out += u"}";
    } else {
        // class name prefix for non-plain objects
        if (o->proto && o->proto != vm.objectProto) {
            if (Prop* c = o->proto->find(PropKey::of(vm.a.constructor)); c && c->val.isObject()) {
                Value nm = vm.get(c->val, vm.a.name);
                if (nm.isString() && !nm.asString()->s.empty()) out += nm.asString()->s + u" ";
            }
        } else if (!o->proto)
            out += u"[Object: null prototype] ";
        out += u"{";
        for (auto& p : o->props) {
            if (!(p.flags & PF_E)) continue;
            sep();
            if (count > 60) {
                out += u"...";
                break;
            }
            if (count == 1) out += u" ";
            if (p.key.isIndex()) out += ascii16(std::to_string(p.key.asIndex()));
            else if (p.key.isSymbol()) out += u"[Symbol(" + (((Symbol*)p.key.cell())->desc ? ((Symbol*)p.key.cell())->desc->s : std::u16string()) + u")]";
            else out += p.key.str()->s;
            out += u": ";
            if (p.flags & PF_ACC) out += u"[Getter/Setter]";
            else inspectInto(vm, p.val, depth - 1, true, out, seen);
        }
        out += count ? u" }" : u"}";
    }
    seen.pop_back();
}

std::u16string Vm::inspect(Value v, int depth, bool quoteStrings) {
    std::u16string out;
    std::vector<JSObject*> seen;
    inspectInto(*this, v, depth, quoteStrings, out, seen);
    return out;
}

// ---------------------------------------------------------------------------- the interpreter
// For "x is not a function" errors: names the thing that was called by finding the instruction that loaded it.
static std::string calleeText(const FuncProto* p, const Ins* callIns, uint16_t reg) {
    const Ins* first = p->code.data();
    for (const Ins* q = callIns - 1; q >= first && callIns - q < 64; --q) {
        if (q->a != reg) continue;
        if (q->op == OP_GetProp && q->c < p->consts.size() && p->consts[q->c].isString()) return "." + utf16to8(p->consts[q->c].asString()->s);
        if ((q->op == OP_GetGlobal || q->op == OP_GetGlobalTypeof) && q->bx < p->consts.size() && p->consts[q->bx].isString())
            return utf16to8(p->consts[q->bx].asString()->s);
        if (q->op == OP_GetElem) return "[...]";
        if (q->op == OP_Move || q->op == OP_GetCtx || q->op == OP_Closure || q->op == OP_Call || q->op == OP_LoadK) break;
    }
    return "";
}

static const Handler* findHandler(FuncProto* p, uint32_t at, bool finallyOnly) {
    for (auto& h : p->handlers)
        if (at >= h.start && at < h.end && (h.isFinally || !finallyOnly)) return &h;
    return nullptr;
}

Value Vm::runFrames(size_t base) {
    Frame* fr = &frames.back();
    const Ins* pc = fr->pc;
    const Ins* code = fr->code->code.data();
    Value* regs = fr->regs;
    const Value* K = fr->code->consts.data();
#define LOAD_FRAME()                    \
    fr = &frames.back();                \
    pc = fr->pc;                        \
    code = fr->code->code.data();       \
    regs = fr->regs;                    \
    K = fr->code->consts.data()
#define R(x) regs[x]
#define KEYK(x) PropKey::of(K[x].asCell())
#define NAMEK(x) utf16to8(K[x].asString()->s).c_str()

    for (;;) {
        try {
            if (resumeThrow) {
                resumeThrow = false;
                throw JSThrow{};
            }
            for (;;) {
                const Ins i = *pc++;
                switch (i.op) {
                    case OP_Nop:
                    case OP_Debugger: break;
                    case OP_LoadK: R(i.a) = K[i.bx]; break;
                    case OP_LoadInt: R(i.a) = Value::i32((int32_t)i.bx); break;
                    case OP_LoadUndef: R(i.a) = Value::undef(); break;
                    case OP_LoadNull: R(i.a) = Value::null(); break;
                    case OP_LoadTrue: R(i.a) = Value::boolean(true); break;
                    case OP_LoadFalse: R(i.a) = Value::boolean(false); break;
                    case OP_LoadEmpty: R(i.a) = Value::empty(); break;
                    case OP_Move: R(i.a) = R(i.b); break;

                    case OP_GetGlobal:
                    case OP_GetGlobalTypeof: {
                        PropKey k = KEYK(i.bx);
                        Value v;
                        if (Prop* p = globalLex->find(k)) {
                            if (p->val.isEmpty()) throwError(Err::ReferenceError, "Cannot access '%s' before initialization", NAMEK(i.bx));
                            v = p->val;
                        } else if (Prop* p2 = global->find(k); p2 && !(p2->flags & PF_ACC)) {
                            v = p2->val;
                        } else if (has(global, k)) {
                            v = getObj(global, k, Value::cell(global));
                        } else if (i.op == OP_GetGlobal) {
                            throwError(Err::ReferenceError, "%s is not defined", NAMEK(i.bx));
                        }
                        R(i.a) = i.op == OP_GetGlobal ? v : Value::cell(typeOf(v));
                        break;
                    }
                    case OP_SetGlobal: {
                        PropKey k = KEYK(i.bx);
                        if (Prop* p = globalLex->find(k)) {
                            if (!(p->flags & PF_W)) throwError(Err::TypeError, "Assignment to constant variable.");
                            if (p->val.isEmpty()) throwError(Err::ReferenceError, "Cannot access '%s' before initialization", NAMEK(i.bx));
                            p->val = R(i.a);
                        } else if (!fr->code->isStrict) {
                            setObj(global, k, R(i.a), Value::cell(global));
                        } else {  // strict code: no implicit globals, and a failed assignment is an error
                            fr->pc = pc;
                            if (!has(global, k)) throwError(Err::ReferenceError, "%s is not defined", NAMEK(i.bx));
                            if (!setObj(global, k, R(i.a), Value::cell(global))) failedSet(Value::cell(global), k);
                        }
                        break;
                    }
                    case OP_DeclGlobalVar: {  // a: declared by eval, which makes the property configurable
                        PropKey k = KEYK(i.bx);
                        if (!global->find(k)) global->add(k, Value::undef(), (uint8_t)(PF_W | PF_E | (i.a ? PF_C : 0)));
                        break;
                    }
                    case OP_DeclGlobalLet: {
                        PropKey k = KEYK(i.bx);
                        if (Prop* p = globalLex->find(k)) {
                            p->val = Value::empty();
                            p->flags = i.a ? 0 : PF_W;
                        } else
                            globalLex->add(k, Value::empty(), i.a ? 0 : PF_W);
                        break;
                    }
                    case OP_InitGlobalLet: {
                        PropKey k = KEYK(i.bx);
                        if (Prop* p = globalLex->find(k)) p->val = R(i.a);
                        else globalLex->add(k, R(i.a), PF_W);
                        break;
                    }
                    case OP_DefGlobalFunc: defineOwn(global, KEYK(i.bx), R(i.a), PF_W | PF_E); break;

                    case OP_GetCtx: {
                        Context* c = fr->ctx;
                        for (int d = i.b; d > 0; --d) c = c->parent;
                        R(i.a) = c->slots[i.c];
                        break;
                    }
                    case OP_SetCtx: {
                        Context* c = fr->ctx;
                        for (int d = i.b; d > 0; --d) c = c->parent;
                        c->slots[i.c] = R(i.a);
                        break;
                    }
                    case OP_PushCtx: {
                        Context* c = alloc<Context>();
                        c->parent = fr->ctx;
                        c->slots.assign(i.bx, Value::undef());
                        fr->ctx = c;
                        fr->ctxDepth++;
                        break;
                    }
                    case OP_PopCtx:
                        for (uint32_t n = i.bx; n > 0; --n) {
                            fr->ctx = fr->ctx->parent;
                            fr->ctxDepth--;
                        }
                        break;
                    case OP_CloneCtx: {
                        Context* c = alloc<Context>();
                        c->parent = fr->ctx->parent;
                        c->slots = fr->ctx->slots;
                        fr->ctx = c;
                        break;
                    }
                    case OP_CheckTDZ:
                        if (R(i.a).isEmpty()) throwError(Err::ReferenceError, "Cannot access '%s' before initialization", NAMEK(i.bx));
                        break;
                    case OP_ThrowConst: throwError(Err::TypeError, "Assignment to constant variable.");
                    case OP_Closure: R(i.a) = Value::cell(makeClosure(fr->code->inner[i.bx], fr->ctx, fr)); break;
                    case OP_NewObject: R(i.a) = Value::cell(newObject()); break;
                    case OP_NewArray: R(i.a) = Value::cell(newArray()); break;
                    case OP_ArrayPush: ((JSArray*)R(i.a).asCell())->elems.push_back(R(i.b)); break;
                    case OP_ArrayHole: ((JSArray*)R(i.a).asCell())->elems.push_back(Value::empty()); break;
                    case OP_ArraySpread: {
                        JSArray* arr = (JSArray*)R(i.a).asCell();
                        Value src = R(i.b);
                        fr->pc = pc;
                        if (src.isObject() && src.asObject()->cls == Cls::Array) {
                            JSArray* s = (JSArray*)src.asObject();
                            for (size_t k = 0; k < s->elems.size(); ++k) arr->elems.push_back(s->elems[k].isEmpty() ? Value::undef() : s->elems[k]);
                        } else
                            iterate(src, [arr](Value v) {
                                arr->elems.push_back(v);
                                return true;
                            });
                        break;
                    }
                    case OP_NewRegExp: {
                        Value args[2] = {K[i.b], K[i.c]};
                        fr->pc = pc;
                        R(i.a) = construct(regexpCtor, args, 2, regexpCtor);
                        break;
                    }

                    case OP_GetProp: {
                        Value o = R(i.b);
                        PropKey k = KEYK(i.c);
                        R(i.a) = o.isObject() ? getObj(o.asObject(), k, o) : get(o, k);
                        break;
                    }
                    case OP_SetProp: {
                        Value o = R(i.a);
                        PropKey k = KEYK(i.b);
                        bool ok = o.isObject() ? setObj(o.asObject(), k, R(i.c), o) : set(o, k, R(i.c));
                        if (!ok && fr->code->isStrict) {
                            fr->pc = pc;
                            failedSet(o, k);
                        }
                        break;
                    }
                    case OP_GetElem: {
                        Value o = R(i.b), kv = R(i.c);
                        if (kv.isInt() && kv.asInt() >= 0) {
                            uint32_t idx = (uint32_t)kv.asInt();
                            if (o.isObject()) {
                                JSObject* ob = o.asObject();
                                if (ob->cls == Cls::TypedArray) {
                                    JSTypedArray* ta = (JSTypedArray*)ob;
                                    R(i.a) = idx < ta->length ? typedGet(ta, idx) : Value::undef();
                                    break;
                                }
                                if (ob->cls == Cls::Array || ob->cls == Cls::Arguments) {
                                    auto& e = ((JSArray*)ob)->elems;
                                    if (idx < e.size() && !e[idx].isEmpty()) {
                                        R(i.a) = e[idx];
                                        break;
                                    }
                                }
                            } else if (o.isString()) {
                                const std::u16string& s = o.asString()->s;
                                R(i.a) = idx < s.size() ? strv(std::u16string(1, s[idx])) : Value::undef();
                                break;
                            }
                        }
                        if (o.isNullish()) throwError(Err::TypeError, "Cannot read properties of %s (reading '%s')", o.isNull() ? "null" : "undefined",
                                                      utf16to8(inspect(kv, 0, false)).c_str());
                        R(i.a) = get(o, toKey(kv));
                        break;
                    }
                    case OP_SetElem: {
                        Value o = R(i.a), kv = R(i.b);
                        if (o.isObject() && kv.isInt() && kv.asInt() >= 0) {
                            JSObject* ob = o.asObject();
                            if (ob->cls == Cls::TypedArray) {
                                typedSet(*this, (JSTypedArray*)ob, (uint32_t)kv.asInt(), R(i.c));
                                break;
                            }
                            if (ob->cls == Cls::Array && !(ob->oflags & (OF_FROZEN | OF_NOEXT | OF_SLOWIDX | OF_LENRO)) && ob->proto == arrayProto && !protoIndexProps) {
                                auto& e = ((JSArray*)ob)->elems;
                                uint32_t idx = (uint32_t)kv.asInt();
                                if (idx < e.size()) {
                                    e[idx] = R(i.c);
                                    break;
                                }
                                if (idx == e.size()) {
                                    e.push_back(R(i.c));
                                    break;
                                }
                            }
                        }
                        if (o.isNullish()) throwError(Err::TypeError, "Cannot set properties of %s", o.isNull() ? "null" : "undefined");
                        PropKey k = toKey(kv);
                        if (!set(o, k, R(i.c)) && fr->code->isStrict) {
                            fr->pc = pc;
                            failedSet(o, k);
                        }
                        break;
                    }
                    case OP_DelProp:
                    case OP_DelElem: {
                        Value o = R(i.b);
                        if (o.isNullish()) throwError(Err::TypeError, "Cannot convert undefined or null to object");
                        PropKey k = i.op == OP_DelProp ? KEYK(i.c) : toKey(R(i.c));
                        bool ok = o.isObject() ? del(o.asObject(), k) : true;
                        if (!ok && fr->code->isStrict) {
                            fr->pc = pc;
                            throwError(Err::TypeError, "Cannot delete property '%s' of %s", keyName(*this, k).c_str(), utf16to8(inspect(o, 0)).c_str());
                        }
                        R(i.a) = Value::boolean(ok);
                        break;
                    }
                    case OP_GetSuper: {
                        JSObject* home = fr->fn->home;
                        JSObject* sp = home ? home->proto : nullptr;
                        Value thisv = R(i.c);
                        R(i.a) = sp ? getObj(sp, toKey(R(i.b)), thisv) : Value::undef();
                        break;
                    }
                    case OP_SetSuper: {
                        PropKey k = toKey(R(i.a));
                        if (!set(R(i.c), k, R(i.b)) && fr->code->isStrict) {
                            fr->pc = pc;
                            failedSet(R(i.c), k);
                        }
                        break;
                    }
                    case OP_DefField: defineOwn(R(i.a).asObject(), KEYK(i.b), R(i.c), PF_ALL); break;
                    case OP_DefElem: defineOwn(R(i.a).asObject(), toKey(R(i.b)), R(i.c), PF_ALL); break;
                    case OP_DefMethod:
                    case OP_DefMethodE: {
                        JSObject* o = R(i.a).asObject();
                        Value fv = R(i.c);
                        if (fv.isObject() && fv.asObject()->cls == Cls::Function) ((JSFunction*)fv.asObject())->home = o;
                        defineOwn(o, toKey(R(i.b)), fv, i.op == OP_DefMethodE ? PF_ALL : (PF_W | PF_C));
                        break;
                    }
                    case OP_DefGetter:
                    case OP_DefSetter:
                    case OP_DefGetterE:
                    case OP_DefSetterE: {
                        JSObject* o = R(i.a).asObject();
                        Value fv = R(i.c);
                        if (fv.isObject() && fv.asObject()->cls == Cls::Function) ((JSFunction*)fv.asObject())->home = o;
                        bool getter = i.op == OP_DefGetter || i.op == OP_DefGetterE;
                        bool en = i.op == OP_DefGetterE || i.op == OP_DefSetterE;
                        defineAccessor(o, toKey(R(i.b)), getter ? fv : Value::empty(), getter ? Value::empty() : fv, (uint8_t)(PF_C | (en ? PF_E : 0)));
                        break;
                    }
                    case OP_SetProto: {
                        Value p = R(i.b);
                        if (p.isObject()) R(i.a).asObject()->proto = p.asObject();
                        else if (p.isNull()) R(i.a).asObject()->proto = nullptr;
                        break;
                    }
                    case OP_ObjSpread: copyDataProps(R(i.a).asObject(), R(i.b), nullptr); break;
                    case OP_ObjRest: {
                        JSObject* r = newObject();
                        Value keep = Value::cell(r);
                        copyDataProps(r, R(i.b), (JSArray*)R(i.c).asCell());
                        R(i.a) = keep;
                        break;
                    }
                    case OP_CheckObj:
                        if (R(i.a).isNullish()) throwError(Err::TypeError, "Cannot destructure '%s' as it is %s.", utf16to8(inspect(R(i.a), 0)).c_str(), R(i.a).isNull() ? "null" : "undefined");
                        break;

                    case OP_Add: {
                        Value x = R(i.b), y = R(i.c);
                        if (x.isInt() && y.isInt()) {
                            int64_t s = (int64_t)x.asInt() + y.asInt();
                            R(i.a) = (s >= INT32_MIN && s <= INT32_MAX) ? Value::i32((int32_t)s) : Value::dbl((double)s);
                        } else if (x.isNumber() && y.isNumber()) {
                            R(i.a) = Value::num(x.number() + y.number());
                        } else
                            R(i.a) = add(x, y);
                        break;
                    }
                    case OP_Sub: {
                        Value x = R(i.b), y = R(i.c);
                        if (x.isInt() && y.isInt()) {
                            int64_t s = (int64_t)x.asInt() - y.asInt();
                            R(i.a) = (s >= INT32_MIN && s <= INT32_MAX) ? Value::i32((int32_t)s) : Value::dbl((double)s);
                        } else
                            R(i.a) = Value::num((x.isNumber() ? x.number() : toNumber(x)) - (y.isNumber() ? y.number() : toNumber(y)));
                        break;
                    }
                    case OP_Mul: {
                        Value x = R(i.b), y = R(i.c);
                        if (x.isInt() && y.isInt()) {
                            int64_t s = (int64_t)x.asInt() * y.asInt();
                            if (s != 0 && s >= INT32_MIN && s <= INT32_MAX) {
                                R(i.a) = Value::i32((int32_t)s);
                                break;
                            }
                        }
                        R(i.a) = Value::num((x.isNumber() ? x.number() : toNumber(x)) * (y.isNumber() ? y.number() : toNumber(y)));
                        break;
                    }
                    case OP_Div: {
                        Value x = R(i.b), y = R(i.c);
                        R(i.a) = Value::num((x.isNumber() ? x.number() : toNumber(x)) / (y.isNumber() ? y.number() : toNumber(y)));
                        break;
                    }
                    case OP_Mod: {
                        Value x = R(i.b), y = R(i.c);
                        if (x.isInt() && y.isInt() && x.asInt() >= 0 && y.asInt() > 0) {
                            R(i.a) = Value::i32(x.asInt() % y.asInt());
                        } else {
                            double p = x.isNumber() ? x.number() : toNumber(x), q = y.isNumber() ? y.number() : toNumber(y);
                            R(i.a) = Value::num(std::fmod(p, q));
                        }
                        break;
                    }
                    case OP_Exp: {
                        double p = toNumber(R(i.b)), q = toNumber(R(i.c));
                        R(i.a) = Value::num(std::pow(p, q));
                        break;
                    }
                    case OP_BitAnd: R(i.a) = Value::i32(toInt32(R(i.b)) & toInt32(R(i.c))); break;
                    case OP_BitOr: R(i.a) = Value::i32(toInt32(R(i.b)) | toInt32(R(i.c))); break;
                    case OP_BitXor: R(i.a) = Value::i32(toInt32(R(i.b)) ^ toInt32(R(i.c))); break;
                    case OP_Shl: R(i.a) = Value::i32((int32_t)((uint32_t)toInt32(R(i.b)) << (toInt32(R(i.c)) & 31))); break;
                    case OP_Shr: R(i.a) = Value::i32(toInt32(R(i.b)) >> (toInt32(R(i.c)) & 31)); break;
                    case OP_UShr: R(i.a) = Value::num((double)((uint32_t)toInt32(R(i.b)) >> (toInt32(R(i.c)) & 31))); break;
                    case OP_Neg: {
                        Value x = R(i.b);
                        if (x.isInt() && x.asInt() != 0 && x.asInt() != INT32_MIN) R(i.a) = Value::i32(-x.asInt());
                        else R(i.a) = Value::num(-toNumber(x));
                        break;
                    }
                    case OP_Plus:
                    case OP_ToNumeric: {
                        Value x = R(i.b);
                        R(i.a) = x.isNumber() ? x : Value::num(toNumber(x));
                        break;
                    }
                    case OP_Not: {
                        Value x = R(i.b);
                        R(i.a) = Value::boolean(!(x.isBool() ? x.isTrue() : toBool(x)));
                        break;
                    }
                    case OP_BitNot: R(i.a) = Value::i32(~toInt32(R(i.b))); break;
                    case OP_Inc: {
                        Value x = R(i.b);
                        if (x.isInt() && x.asInt() != INT32_MAX) R(i.a) = Value::i32(x.asInt() + 1);
                        else R(i.a) = Value::num(toNumber(x) + 1);
                        break;
                    }
                    case OP_Dec: {
                        Value x = R(i.b);
                        if (x.isInt() && x.asInt() != INT32_MIN) R(i.a) = Value::i32(x.asInt() - 1);
                        else R(i.a) = Value::num(toNumber(x) - 1);
                        break;
                    }
                    case OP_Typeof: R(i.a) = Value::cell(typeOf(R(i.b))); break;
                    case OP_ToPropKey: {
                        Value x = R(i.b);
                        R(i.a) = x.isObject() ? toPrimitive(x, 2) : x;
                        break;
                    }
                    case OP_StrCat: {
                        Value x = R(i.b), y = R(i.c);
                        if (!x.isString()) x = Value::cell(toString(x));
                        if (!y.isString()) y = Value::cell(toString(y));
                        R(i.a) = concat(x, y);
                        break;
                    }
                    case OP_Eq: R(i.a) = Value::boolean(looseEquals(R(i.b), R(i.c))); break;
                    case OP_Ne: R(i.a) = Value::boolean(!looseEquals(R(i.b), R(i.c))); break;
                    case OP_SEq: {
                        Value x = R(i.b), y = R(i.c);
                        R(i.a) = Value::boolean((x.isInt() && y.isInt()) ? x.b == y.b : strictEquals(x, y));
                        break;
                    }
                    case OP_SNe: {
                        Value x = R(i.b), y = R(i.c);
                        R(i.a) = Value::boolean(!((x.isInt() && y.isInt()) ? x.b == y.b : strictEquals(x, y)));
                        break;
                    }
                    case OP_Lt:
                    case OP_Le:
                    case OP_Gt:
                    case OP_Ge: {
                        Value x = R(i.b), y = R(i.c);
                        int c;
                        if (x.isInt() && y.isInt()) c = x.asInt() < y.asInt() ? -1 : x.asInt() > y.asInt() ? 1 : 0;
                        else c = compare(x, y);
                        bool r = i.op == OP_Lt ? c == -1 : i.op == OP_Le ? (c == -1 || c == 0) : i.op == OP_Gt ? c == 1 : (c == 1 || c == 0);
                        R(i.a) = Value::boolean(r);
                        break;
                    }
                    case OP_In: {
                        Value o = R(i.c);
                        if (!o.isObject()) throwError(Err::TypeError, "Cannot use 'in' operator to search for a key in a non-object");
                        R(i.a) = Value::boolean(has(o.asObject(), toKey(R(i.b))));
                        break;
                    }
                    case OP_InstanceOf: R(i.a) = Value::boolean(instanceOf(R(i.b), R(i.c))); break;

                    case OP_Jmp: {
                        const Ins* t = code + i.bx;
                        if (t < pc) {
                            if (gc.shouldCollect()) gc.collect();
                            if (++pollTicks == 0) pollInterrupt();
                        }
                        pc = t;
                        break;
                    }
                    case OP_JmpT: {
                        Value x = R(i.a);
                        if (x.isBool() ? x.isTrue() : toBool(x)) {
                            const Ins* t = code + i.bx;
                            if (t < pc) {
                                if (gc.shouldCollect()) gc.collect();
                                if (++pollTicks == 0) pollInterrupt();
                            }
                            pc = t;
                        }
                        break;
                    }
                    case OP_JmpF: {
                        Value x = R(i.a);
                        if (!(x.isBool() ? x.isTrue() : toBool(x))) pc = code + i.bx;
                        break;
                    }
                    case OP_JmpNullish:
                        if (R(i.a).isNullish()) pc = code + i.bx;
                        break;
                    case OP_JmpNotNullish:
                        if (!R(i.a).isNullish()) pc = code + i.bx;
                        break;
                    case OP_JmpUndef:
                        if (R(i.a).isUndef()) pc = code + i.bx;
                        break;
                    case OP_JmpNotUndef:
                        if (!R(i.a).isUndef()) pc = code + i.bx;
                        break;

                    case OP_Call: {
                        Value* b2 = regs + i.b;
                        Value callee = b2[0];
                        fr->pc = pc;
                        if (callee.isObject()) {
                            JSObject* co = callee.asObject();
                            if (co->cls == Cls::Function) {
                                JSFunction* f = (JSFunction*)co;
                                FuncProto* p = f->code;
                                if (!p->isGenerator && !p->isAsync && !p->isClassCtor) {
                                    if (gc.shouldCollect()) gc.collect();
                                    if (++pollTicks == 0) pollInterrupt();
                                    pushFrame(f, b2, i.c, Value::undef(), regs + i.a, false, false);
                                    LOAD_FRAME();
                                    break;
                                }
                            } else if (co->cls == Cls::Native) {
                                JSNative* n = (JSNative*)co;
                                CallInfo ci{*this, b2[1], b2 + 2, i.c, Value::undef(), n};
                                Value r = n->fn(ci);
                                regs[i.a] = r;
                                break;
                            }
                        }
                        if (!isCallable(callee)) {
                            std::string what = calleeText(fr->code, pc - 1, i.b);
                            std::string shown = utf16to8(inspect(callee, 0));
                            if (what.empty()) throwError(Err::TypeError, "%s is not a function", shown.c_str());
                            if (what[0] == '.' || what[0] == '[') {
                                std::string owner = b2[1].isObject() ? "object" : utf16to8(inspect(b2[1], 0));
                                throwError(Err::TypeError, "%s%s is not a function (it is %s)", b2[1].isObject() ? "(...)" : owner.c_str(), what.c_str(), shown.c_str());
                            }
                            throwError(Err::TypeError, "%s is not a function (it is %s)", what.c_str(), shown.c_str());
                        }
                        Value r = call(callee, b2[1], b2 + 2, i.c);
                        regs[i.a] = r;
                        break;
                    }
                    case OP_New: {
                        Value* b2 = regs + i.b;
                        fr->pc = pc;
                        if (gc.shouldCollect()) gc.collect();
                        Value r = construct(b2[0], b2 + 2, i.c, b2[0]);
                        regs[i.a] = r;
                        break;
                    }
                    case OP_CallSpread:
                    case OP_NewSpread: {
                        Value* b2 = regs + i.b;
                        fr->pc = pc;
                        JSArray* arr = (JSArray*)b2[2].asCell();
                        Value r = i.op == OP_CallSpread ? call(b2[0], b2[1], arr->elems.data(), (int)arr->elems.size())
                                                         : construct(b2[0], arr->elems.data(), (int)arr->elems.size(), b2[0]);
                        regs[i.a] = r;
                        break;
                    }
                    case OP_SuperCall:
                    case OP_SuperCallSpread: {
                        Value* b2 = regs + i.b;
                        fr->pc = pc;
                        JSObject* parent = fr->fn->proto;
                        if (!parent || !isConstructor(Value::cell(parent))) throwError(Err::TypeError, "Super constructor is not a constructor");
                        if (!regs[1].isEmpty()) throwError(Err::ReferenceError, "Super constructor may only be called once");
                        Value r;
                        if (i.op == OP_SuperCall) r = construct(Value::cell(parent), b2 + 2, i.c, fr->newTarget);
                        else {
                            JSArray* arr = (JSArray*)b2[2].asCell();
                            r = construct(Value::cell(parent), arr->elems.data(), (int)arr->elems.size(), fr->newTarget);
                        }
                        regs[1] = r;
                        initFields(fr->fn, r);
                        regs[i.a] = r;
                        break;
                    }
                    case OP_Return:
                    case OP_RetUndef: {
                        Value rv = i.op == OP_Return ? R(i.a) : Value::undef();
                        if (fr->construct && !rv.isObject()) {
                            rv = regs[1];
                            if (rv.isEmpty()) throwError(Err::ReferenceError, "Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
                        }
                        bool entry = fr->entry;
                        Value* ret = fr->ret;
                        frames.pop_back();
                        if (entry) {
                            stackTop = regs;
                            return rv;
                        }
                        *ret = rv;
                        LOAD_FRAME();
                        stackTop = regs + fr->code->nregs;
                        break;
                    }
                    case OP_Throw:
                        fr->pc = pc;
                        freshError = nullptr;
                        if (traceThrows && onConsole) {
                            Value thrown = R(i.a);
                            traceThrows = false;
                            std::u16string text = u"[throw] " + describeError(thrown);
                            if (!(thrown.isObject() && thrown.asObject()->cls == Cls::Error)) text += captureStack();
                            traceThrows = true;
                            onConsole(2, text);
                        }
                        throwValue(R(i.a));
                    case OP_Catch:
                        R(i.a) = pending;
                        pending = Value::undef();
                        break;

                    case OP_GetIter: {
                        Value src = R(i.b);
                        fr->pc = pc;
                        if (src.isObject() && src.asObject()->cls == Cls::Array && src.asObject()->proto == arrayProto && plainArrayIteration()) {
                            R(i.a) = Value::cell(newIter(Cls::ArrayIter, src, 0));
                            R(i.a + 1) = Value::empty();
                        } else if (src.isString()) {
                            R(i.a) = Value::cell(newIter(Cls::StringIter, src, 0));
                            R(i.a + 1) = Value::empty();
                        } else {
                            Value m = get(src, PropKey::of(symIterator));
                            if (!isCallable(m)) throwError(Err::TypeError, "%s is not iterable", utf16to8(inspect(src, 0)).c_str());
                            Value itv = call(m, src, nullptr, 0);
                            if (!itv.isObject()) throwError(Err::TypeError, "Result of the Symbol.iterator method is not an object");
                            Cls c = itv.asObject()->cls;
                            R(i.a) = itv;
                            if (c == Cls::ArrayIter || c == Cls::MapIter || c == Cls::SetIter || c == Cls::StringIter) R(i.a + 1) = Value::empty();
                            else R(i.a + 1) = get(itv, a.next);
                        }
                        break;
                    }
                    case OP_IterNext:
                    case OP_IterNextRet: {
                        Value itv = R(i.b);
                        fr->pc = pc;
                        if (!itv.isObject()) {
                            R(i.a) = Value::undef();
                            R(i.c) = Value::boolean(true);
                            break;
                        }
                        if (R(i.b + 1).isEmpty()) {
                            Value out;
                            if (iterStep((JSIter*)itv.asObject(), out)) {
                                R(i.a) = out;
                                R(i.c) = Value::boolean(false);
                            } else {
                                R(i.a) = Value::undef();
                                R(i.c) = Value::boolean(true);
                                R(i.b) = Value::undef();
                            }
                            break;
                        }
                        R(i.b) = Value::undef();  // an iterator whose next() throws is finished: nobody closes it
                        Value r = call(R(i.b + 1), itv, nullptr, 0);
                        if (!r.isObject()) throwError(Err::TypeError, "Iterator result is not an object");
                        bool finished = toBool(get(r, a.done));
                        R(i.b) = itv;
                        if (finished) {
                            R(i.a) = i.op == OP_IterNextRet ? get(r, a.value) : Value::undef();
                            R(i.c) = Value::boolean(true);
                            R(i.b) = Value::undef();
                        } else {
                            R(i.a) = get(r, a.value);
                            R(i.c) = Value::boolean(false);
                        }
                        break;
                    }
                    case OP_IterClose:
                    case OP_IterCloseQuiet: {
                        Value itv = R(i.a);
                        R(i.a) = Value::undef();  // closing happens once, even if return() throws
                        if (!itv.isObject() || R(i.a + 1).isEmpty()) break;
                        fr->pc = pc;
                        if (i.op == OP_IterClose) {
                            Value ret = get(itv, a.return_);
                            if (isCallable(ret) && !call(ret, itv, nullptr, 0).isObject()) throwError(Err::TypeError, "Iterator result is not an object");
                        } else {
                            Value keep = pending;  // an exception is on its way: whatever return() does is ignored
                            try {
                                Value ret = get(itv, a.return_);
                                if (isCallable(ret)) call(ret, itv, nullptr, 0);
                            } catch (JSThrow&) {
                            }
                            pending = keep;
                        }
                        break;
                    }
                    case OP_GetAsyncIter: {
                        fr->pc = pc;
                        Value next;
                        Value it = getAsyncIterator(R(i.b), next);
                        R(i.a) = it;
                        R(i.a + 1) = next;
                        break;
                    }
                    case OP_IterCall:
                        fr->pc = pc;
                        R(i.a) = call(R(i.b + 1), R(i.b), nullptr, 0);
                        break;
                    case OP_IterResult: {
                        Value r = R(i.b);
                        fr->pc = pc;
                        if (!r.isObject()) throwError(Err::TypeError, "Iterator result %s is not an object", utf16to8(inspect(r, 0)).c_str());
                        bool d = toBool(get(r, a.done));
                        R(i.c) = Value::boolean(d);
                        R(i.a) = d ? Value::undef() : get(r, a.value);
                        break;
                    }
                    case OP_IterReturnCall:
                    case OP_IterReturnCallQuiet: {
                        Value itv = R(i.b);
                        R(i.b) = Value::undef();
                        R(i.c) = Value::boolean(false);
                        if (!itv.isObject()) break;
                        fr->pc = pc;
                        if (i.op == OP_IterReturnCall) {
                            Value ret = get(itv, a.return_);
                            if (ret.isNullish()) break;
                            R(i.a) = call(ret, itv, nullptr, 0);
                            R(i.c) = Value::boolean(true);
                        } else {
                            Value keep = pending;
                            try {
                                Value ret = get(itv, a.return_);
                                if (isCallable(ret)) {
                                    R(i.a) = call(ret, itv, nullptr, 0);
                                    R(i.c) = Value::boolean(true);
                                }
                            } catch (JSThrow&) {
                            }
                            pending = keep;
                        }
                        break;
                    }
                    case OP_CheckIterResult:
                        if (!R(i.a).isObject()) throwError(Err::TypeError, "Iterator result %s is not an object", utf16to8(inspect(R(i.a), 0)).c_str());
                        break;
                    case OP_DelegateCall:
                        fr->pc = pc;
                        R(i.a) = delegateCall(&R(i.b));
                        break;
                    case OP_DelegateResult: {
                        fr->pc = pc;
                        Value v;
                        bool d = false;
                        delegateResult(&R(i.b), v, d);
                        R(i.c) = Value::boolean(d);
                        if (d) R(i.a) = v;
                        break;
                    }
                    case OP_ForInPrep: {
                        Value src = R(i.b);
                        JSIter* it = newIter(Cls::ForIn, src, 0);
                        R(i.a) = Value::cell(it);
                        if (!src.isNullish()) {
                            JSObject* o = toObject(src);
                            it->target = Value::cell(o);
                            std::vector<PropKey> keys, seen;
                            for (JSObject* cur = o; cur; cur = getProto(cur)) {
                                keys.clear();
                                ownKeys(cur, keys, true, false, false);
                                for (PropKey k : keys) {
                                    bool dup = false;
                                    for (PropKey s : seen)
                                        if (s == k) {
                                            dup = true;
                                            break;
                                        }
                                    if (dup) continue;
                                    seen.push_back(k);
                                    Prop pr;
                                    if (getOwn(cur, k, pr) && (pr.flags & PF_E)) it->keys.push_back(keyToValue(k));
                                }
                                if (seen.size() > 100000) break;
                            }
                        }
                        break;
                    }
                    case OP_ForInNext: {
                        JSIter* it = (JSIter*)R(i.b).asCell();
                        bool found = false;
                        while (it->pos < it->keys.size()) {
                            Value k = it->keys[it->pos++];
                            if (it->target.isObject() && has(it->target.asObject(), toKey(k))) {
                                R(i.a) = k;
                                found = true;
                                break;
                            }
                        }
                        R(i.c) = Value::boolean(!found);
                        break;
                    }
                    case OP_CreateArgs: {
                        JSArray* arr = fr->args ? fr->args : newArray();
                        JSArray* copy = newArray();
                        copy->elems = arr->elems;
                        copy->cls = Cls::Arguments;
                        copy->proto = objectProto;
                        copy->add(PropKey::of(symIterator), getObj(arrayProto, PropKey::of(symIterator), Value::cell(arrayProto)), PF_W | PF_C);
                        R(i.a) = Value::cell(copy);
                        break;
                    }
                    case OP_CreateRest: {
                        JSArray* rest = newArray();
                        if (fr->args && fr->args->elems.size() > i.b) rest->elems.assign(fr->args->elems.begin() + i.b, fr->args->elems.end());
                        R(i.a) = Value::cell(rest);
                        break;
                    }
                    case OP_ThisCheck:
                        if (regs[1].isEmpty()) throwError(Err::ReferenceError, "Must call super constructor in derived class before accessing 'this'");
                        break;
                    case OP_LoadNewTarget: R(i.a) = fr->newTarget; break;
                    case OP_MakeClass: {
                        JSFunction* ctor = (JSFunction*)R(i.a).asCell();
                        JSObject* proto = newObject();
                        Value keep = Value::cell(proto);
                        if (i.b != 0xFFFF) {
                            Value parent = R(i.b);
                            if (parent.isNull()) {
                                proto->proto = nullptr;
                            } else {
                                if (!isConstructor(parent)) throwError(Err::TypeError, "Class extends value %s is not a constructor or null", utf16to8(inspect(parent, 0)).c_str());
                                Value pp = get(parent, a.prototype);
                                if (!pp.isObject() && !pp.isNull()) throwError(Err::TypeError, "Class extends value does not have a valid prototype property");
                                proto->proto = pp.isObject() ? pp.asObject() : nullptr;
                                ctor->proto = parent.asObject();
                            }
                        }
                        ctor->add(PropKey::of(a.prototype), keep, 0);
                        proto->add(PropKey::of(a.constructor), Value::cell(ctor), PF_W | PF_C);
                        ctor->home = proto;
                        R(i.c) = keep;
                        break;
                    }
                    case OP_AddField: {
                        JSFunction* ctor = (JSFunction*)R(i.a).asCell();
                        if (!ctor->fields.isObject()) ctor->fields = Value::cell(newArray());
                        JSArray* arr = (JSArray*)ctor->fields.asObject();
                        Value fv = R(i.c);
                        if (fv.isObject() && fv.asObject()->cls == Cls::Function) ((JSFunction*)fv.asObject())->home = ctor->home;
                        arr->elems.push_back(R(i.b));
                        arr->elems.push_back(fv);
                        break;
                    }
                    case OP_DefStaticField: {
                        JSFunction* ctor = (JSFunction*)R(i.a).asCell();
                        Value fv = R(i.c);
                        fr->pc = pc;
                        Value v;
                        if (fv.isObject()) {
                            if (fv.asObject()->cls == Cls::Function) ((JSFunction*)fv.asObject())->home = ctor;
                            v = call(fv, R(i.a), nullptr, 0);
                        }
                        defineOwn(ctor, toKey(R(i.b)), v, PF_ALL);
                        break;
                    }
                    case OP_ScopeHas: {
                        Value o = R(i.b);
                        bool found = false;
                        if (o.isObject()) {
                            fr->pc = pc;
                            PropKey k = toKey(R(i.c));
                            found = has(o.asObject(), k);
                            if (found && o.asObject()->proto) {  // names an object asks to be left out of `with`
                                Value blocked = getObj(o.asObject(), PropKey::of(symUnscopables), o);
                                if (blocked.isObject() && toBool(getObj(blocked.asObject(), k, blocked))) found = false;
                            }
                        }
                        R(i.a) = Value::boolean(found);
                        break;
                    }
                    case OP_NewScopeObj: R(i.a) = Value::cell(newObject(nullptr)); break;
                    case OP_ScopeDeclare: {
                        JSObject* o = R(i.a).asObject();
                        PropKey k = toKey(R(i.b));
                        if (!o->find(k)) o->add(k, Value::undef(), PF_ALL);
                        break;
                    }
                    case OP_ToObject:
                        fr->pc = pc;
                        R(i.a) = Value::cell(toObject(R(i.b)));
                        break;
                    case OP_DirectEval: {
                        uint32_t site = (pc++)->bx;
                        Value callee = R(i.b);
                        fr->pc = pc;
                        Value* argv = &R(i.b + 2);
                        if (callee.isObject() && evalFn.isObject() && callee.asObject() == evalFn.asObject()) {
                            if (i.c == 0 || !argv[0].isString()) {  // eval of a non-string returns it unchanged
                                R(i.a) = i.c ? argv[0] : Value::undef();
                                break;
                            }
                            auto src = std::make_shared<const std::u16string>(argv[0].asString()->s);
                            FuncProto* code = compileEval(*this, src, fr->code->evalSites[site]);
                            JSFunction* fn = makeClosure(code, fr->ctx, fr);
                            R(i.a) = callFunction(fn, Value::undef(), nullptr, 0, Value::undef(), false);
                        } else
                            R(i.a) = call(callee, Value::undef(), argv, i.c);
                        break;
                    }
                    case OP_ClassInit: {
                        JSFunction* ctor = (JSFunction*)R(i.a).asCell();
                        Value fv = R(i.b);
                        fr->pc = pc;
                        if (fv.isObject() && fv.asObject()->cls == Cls::Function) ((JSFunction*)fv.asObject())->home = ctor;
                        call(fv, R(i.a), nullptr, 0);
                        break;
                    }
                    case OP_ModuleInit: moduleInit(*this, R(i.a), fr->ctx); break;
                    case OP_ImportCall: {
                        if (importHook.isUndef()) throwError(Err::TypeError, "import() is not available here");
                        Value args[3] = {R(i.b), R(i.b + 1), R(i.b + 2)};
                        fr->pc = pc;
                        R(i.a) = call(importHook, Value::undef(), args, 3);
                        break;
                    }
                    case OP_NewPrivate: {
                        Symbol* sym = newSymbol(K[i.bx].asString());
                        sym->isPrivate = true;
                        R(i.a) = Value::cell(sym);
                        break;
                    }
                    case OP_PrivMethod: {
                        uint32_t how = (pc++)->bx;
                        JSFunction* ctor = (JSFunction*)R(i.a).asCell();
                        Value fnv = R(i.c);
                        if (how & 4) {  // static: the class itself has it
                            if (fnv.isObject() && fnv.asObject()->cls == Cls::Function) ((JSFunction*)fnv.asObject())->home = ctor;
                            addPrivateMember(ctor, toKey(R(i.b)), (int)(how & 3), fnv);
                        } else {
                            if (fnv.isObject() && fnv.asObject()->cls == Cls::Function) ((JSFunction*)fnv.asObject())->home = ctor->home;
                            if (!ctor->privates.isObject()) ctor->privates = Value::cell(newArray());
                            JSArray* list = (JSArray*)ctor->privates.asObject();
                            list->elems.push_back(R(i.b));
                            list->elems.push_back(Value::i32((int32_t)(how & 3)));
                            list->elems.push_back(fnv);
                        }
                        break;
                    }
                    case OP_GetPrivate: {
                        Value o = R(i.b);
                        fr->pc = pc;
                        PropKey k = toKey(R(i.c));
                        Prop* p = o.isObject() ? o.asObject()->find(k) : nullptr;
                        if (!p) throwError(Err::TypeError, "Cannot read private member %s from an object whose class did not declare it", keyName(*this, k).c_str());
                        if (p->flags & PF_ACC) {
                            Accessor* acc = (Accessor*)p->val.asCell();
                            if (acc->get.isUndef()) throwError(Err::TypeError, "Private member %s was defined without a getter", keyName(*this, k).c_str());
                            R(i.a) = call(acc->get, o, nullptr, 0);
                        } else
                            R(i.a) = p->val;
                        break;
                    }
                    case OP_SetPrivate: {
                        Value o = R(i.a);
                        fr->pc = pc;
                        PropKey k = toKey(R(i.b));
                        Prop* p = o.isObject() ? o.asObject()->find(k) : nullptr;
                        if (!p) throwError(Err::TypeError, "Cannot write private member %s to an object whose class did not declare it", keyName(*this, k).c_str());
                        if (p->flags & PF_ACC) {
                            Accessor* acc = (Accessor*)p->val.asCell();
                            if (acc->set.isUndef()) throwError(Err::TypeError, "Private member %s was defined without a setter", keyName(*this, k).c_str());
                            Value v = R(i.c);
                            call(acc->set, o, &v, 1);
                        } else if (!(p->flags & PF_W))
                            throwError(Err::TypeError, "Private method %s is not writable", keyName(*this, k).c_str());
                        else
                            p->val = R(i.c);
                        break;
                    }
                    case OP_HasPrivate: {
                        Value o = R(i.c);
                        if (!o.isObject()) throwError(Err::TypeError, "Cannot use 'in' operator to search for a private member in a non-object");
                        R(i.a) = Value::boolean(o.asObject()->find(toKey(R(i.b))) != nullptr);
                        break;
                    }
                    case OP_Yield:
                    case OP_YieldDelegate:
                    case OP_Await: {
                        JSGenerator* g = fr->gen;
                        if (!g) throwError(Err::SyntaxError, "await/yield is only valid in async functions and generators");
                        Value v = R(i.b);
                        g->suspendedAtAwait = i.op == OP_Await;
                        g->delegating = i.op == OP_YieldDelegate;
                        g->regs.assign(regs, regs + fr->code->nregs);
                        g->pc = pc;
                        g->ctx = fr->ctx;
                        g->ctxDepth = fr->ctxDepth;
                        g->resumeReg = i.a;
                        frames.pop_back();
                        stackTop = regs;
                        yielded = true;
                        return v;
                    }
                    default: throwError(Err::Error, "internal: bad opcode %d", (int)i.op);
                }
            }
        } catch (JSThrow&) {
            // Find a handler in the frames this invocation owns; otherwise let the C++ caller see the exception.
            if (freshError && pending.isObject() && pending.asObject() == freshError) {
                frames.back().pc = pc;
                if (Prop* st = freshError->find(PropKey::of(a.stack)); st && st->val.isString()) {
                    std::u16string head = st->val.asString()->s;
                    size_t nl = head.find(u'\n');
                    if (nl != std::u16string::npos) head.resize(nl);
                    st->val = strv(head + captureStack());
                }
            }
            freshError = nullptr;
            bool returning = pending.isObject() && pending.asObject() == genReturnSignal;
            for (;;) {
                fr = &frames.back();
                uint32_t at = (uint32_t)(pc - fr->code->code.data());
                if (at > 0) --at;
                if (const Handler* h = terminated ? nullptr : findHandler(fr->code, at, returning)) {
                    while (fr->ctxDepth > h->ctxDepth) {
                        fr->ctx = fr->ctx->parent;
                        fr->ctxDepth--;
                    }
                    code = fr->code->code.data();
                    pc = code + h->target;
                    regs = fr->regs;
                    K = fr->code->consts.data();
                    stackTop = regs + fr->code->nregs;
                    break;
                }
                Value* fbase = fr->regs;
                bool entry = frames.size() - 1 == base;
                frames.pop_back();
                stackTop = fbase;
                if (entry) throw;
                pc = frames.back().pc;
                stackTop = frames.back().regs + frames.back().code->nregs;
            }
        }
    }
#undef LOAD_FRAME
#undef R
#undef KEYK
#undef NAMEK
}

}  // namespace tb::js
