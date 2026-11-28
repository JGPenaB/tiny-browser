// ArrayBuffer, SharedArrayBuffer, the typed arrays, DataView and Atomics (ECMA-262 10.4.5, 23.2 and 25.1 to 25.4),
// and the byte-oriented web APIs that sit on them: TextEncoder, TextDecoder and crypto.
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>

#include "builtins.h"

namespace tb::js {

static const char* kTypedNames[] = {"Int8Array",   "Uint8Array",   "Uint8ClampedArray", "Int16Array",   "Uint16Array",   "Int32Array",
                                    "Uint32Array", "Float32Array", "Float64Array",      "Float16Array", "BigInt64Array", "BigUint64Array"};
static const int kTypeCount = 12;
static const double kMaxBufferBytes = 1073741824.0;

void JSTypedArray::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(buffer);
}

namespace {

bool isBig(TA t) { return t == TA::I64 || t == TA::U64; }
int sizeOf(TA t) { return kTaSize[(int)t]; }

// ---------------------------------------------------------------------------- raw element values
double halfToDouble(uint16_t h) {
    int exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    double v;
    if (exp == 0) v = std::ldexp((double)man, -24);
    else if (exp == 31) v = man ? NAN : INFINITY;
    else v = std::ldexp((double)(man + 1024), exp - 25);
    return (h & 0x8000) ? -v : v;
}
// Rounds straight from a double to the nearest half-precision value, ties to even.
uint16_t doubleToHalf(double d) {
    if (d != d) return 0x7E00;
    uint16_t sign = std::signbit(d) ? 0x8000 : 0;
    double a = std::fabs(d);
    if (a == 0) return sign;
    if (a >= 65520.0) return sign | 0x7C00;
    int e;
    std::frexp(a, &e);
    int exp = e - 1;  // a is in [2^exp, 2^(exp+1))
    if (exp < -14) return sign | (uint16_t)std::nearbyint(std::ldexp(a, 24));  // subnormal: steps of 2^-24
    double r = std::nearbyint(std::ldexp(a, 10 - exp));                         // 1024 to 2048
    if (r == 2048) {
        r = 1024;
        ++exp;
    }
    if (exp > 15) return sign | 0x7C00;
    return sign | (uint16_t)((exp + 15) << 10) | (uint16_t)((int)r - 1024);
}
uint32_t toUint32(double d) {  // modulo 2^32
    if (!std::isfinite(d)) return 0;
    double m = std::fmod(std::trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (uint32_t)m;
}
uint8_t clampByte(double d) {  // ToUint8Clamp: round half to even
    if (!(d > 0)) return 0;
    if (d >= 255) return 255;
    double f = std::floor(d);
    if (f + 0.5 < d) return (uint8_t)(f + 1);
    if (d < f + 0.5) return (uint8_t)f;
    return (uint8_t)(std::fmod(f, 2) == 0 ? f : f + 1);
}

// One element, in this machine's (little-endian) byte order.
Value loadRaw(Vm& vm, const uint8_t* p, TA type) {
    switch (type) {
        case TA::I8: return Value::i32((int8_t)*p);
        case TA::U8:
        case TA::U8C: return Value::i32(*p);
        case TA::I16: {
            int16_t v;
            memcpy(&v, p, 2);
            return Value::i32(v);
        }
        case TA::U16: {
            uint16_t v;
            memcpy(&v, p, 2);
            return Value::i32(v);
        }
        case TA::I32: {
            int32_t v;
            memcpy(&v, p, 4);
            return Value::i32(v);
        }
        case TA::U32: {
            uint32_t v;
            memcpy(&v, p, 4);
            return Value::num((double)v);
        }
        case TA::F32: {
            float v;
            memcpy(&v, p, 4);
            return Value::num(v);
        }
        case TA::F64: {
            double v;
            memcpy(&v, p, 8);
            return Value::num(v);
        }
        case TA::F16: {
            uint16_t v;
            memcpy(&v, p, 2);
            return Value::num(halfToDouble(v));
        }
        case TA::I64: {
            int64_t v;
            memcpy(&v, p, 8);
            return V(bigFromI64(vm, v));
        }
        default: {
            uint64_t v;
            memcpy(&v, p, 8);
            return V(bigFromU64(vm, v));
        }
    }
}
void storeNum(uint8_t* p, TA type, double d) {
    switch (type) {
        case TA::I8:
        case TA::U8: *p = (uint8_t)toUint32(d); break;
        case TA::U8C: *p = clampByte(d); break;
        case TA::I16:
        case TA::U16: {
            uint16_t v = (uint16_t)toUint32(d);
            memcpy(p, &v, 2);
            break;
        }
        case TA::I32:
        case TA::U32: {
            uint32_t v = toUint32(d);
            memcpy(p, &v, 4);
            break;
        }
        case TA::F32: {
            float v = (float)d;
            memcpy(p, &v, 4);
            break;
        }
        case TA::F64: memcpy(p, &d, 8); break;
        case TA::F16: {
            uint16_t v = doubleToHalf(d);
            memcpy(p, &v, 2);
            break;
        }
        default: break;
    }
}
// The value an element of this type holds: ToBigInt or ToNumber of what was given.
Value toElement(Vm& vm, TA type, Value v) {
    if (isBig(type)) return V(vm.toBigInt(v));
    return v.isNumber() ? v : Value::num(vm.toNumber(v));
}
void storeElement(uint8_t* p, TA type, Value numeric) {
    if (isBig(type)) {
        uint64_t v = bigToU64(numeric.asBigInt());
        memcpy(p, &v, 8);
    } else
        storeNum(p, type, numeric.number());
}
uint8_t* at(const JSTypedArray* t, double i) { return t->buffer->data() + t->offset + (size_t)i * sizeOf(t->type); }

// ---------------------------------------------------------------------------- shared helpers
// ToIntegerOrInfinity with -0 turned into +0
double toInt(Vm& vm, Value v) { return vm.toInteger(v) + 0.0; }
// ToClampedIndex: a relative position turned into one between 0 and len
double clampIndex(Vm& vm, Value v, double len) {
    double r = vm.toInteger(v);
    if (r < 0) return std::max(len + r, 0.0);
    return std::min(r, len) + 0.0;  // never -0
}
double lengthOfArrayLike(Vm& vm, Value o) {
    double d = vm.toInteger(vm.get(o, vm.a.length));
    return d <= 0 ? 0 : std::min(d, 9007199254740991.0);
}
// GetMethod: undefined when the property is undefined or null
Value getMethod(Vm& vm, Value o, PropKey k) {
    Value m = vm.get(o, k);
    if (m.isNullish()) return Value::undef();
    if (!vm.isCallable(m)) vm.throwError(Err::TypeError, "%s is not a function", utf16to8(vm.inspect(m, 0)).c_str());
    return m;
}
// IteratorToList(GetIteratorFromMethod(obj, method))
void iterToList(Vm& vm, Value obj, Value method, std::vector<Value>& out) {
    if (obj.isObject() && obj.asObject()->cls == Cls::Array && method.isObject() && vm.arrayValuesFn.isObject() &&
        method.asObject() == vm.arrayValuesFn.asObject() && vm.plainArrayIteration() && !(obj.asObject()->oflags & OF_SLOWIDX)) {
        auto& elems = ((JSArray*)obj.asObject())->elems;
        if (std::find_if(elems.begin(), elems.end(), [](Value v) { return v.isEmpty(); }) == elems.end()) {
            out = elems;
            return;
        }
    }
    Value itv = vm.call(method, obj, nullptr, 0);
    if (!itv.isObject()) vm.throwError(Err::TypeError, "Result of the Symbol.iterator method is not an object");
    Value next = vm.get(itv, vm.a.next);
    for (;;) {
        Value r = vm.call(next, itv, nullptr, 0);
        if (!r.isObject()) vm.throwError(Err::TypeError, "Iterator result is not an object");
        if (vm.toBool(vm.get(r, vm.a.done))) return;
        out.push_back(vm.get(r, vm.a.value));
    }
}
// SpeciesConstructor (7.3.22)
Value speciesCtor(Vm& vm, JSObject* o, Value fallback) {
    Value c = vm.getObj(o, PropKey::of(vm.a.constructor), V(o));
    if (c.isUndef()) return fallback;
    if (!c.isObject()) vm.throwError(Err::TypeError, "The constructor property is not an object");
    Value s = vm.get(c, PropKey::of(vm.symSpecies));
    if (s.isNullish()) return fallback;
    if (!vm.isConstructor(s)) vm.throwError(Err::TypeError, "The Symbol.species property is not a constructor");
    return s;
}

JSArrayBuffer* asBuffer(Value v) { return v.isObject() && v.asObject()->cls == Cls::ArrayBuffer ? (JSArrayBuffer*)v.asObject() : nullptr; }
JSTypedArray* asTyped(Value v) { return v.isObject() && v.asObject()->cls == Cls::TypedArray ? (JSTypedArray*)v.asObject() : nullptr; }

// AllocateArrayBuffer once the prototype is known
JSArrayBuffer* allocBuffer(Vm& vm, JSObject* proto, double byteLength, double maxLen = -1, bool shared = false) {
    if (byteLength > kMaxBufferBytes || maxLen > kMaxBufferBytes) vm.throwError(Err::RangeError, "Array buffer allocation failed");
    JSArrayBuffer* b = vm.alloc<JSArrayBuffer>();
    b->proto = proto;
    b->shared = shared;
    b->maxLen = maxLen;
    if (shared) {
        b->block = newSharedBlock((size_t)byteLength, (size_t)std::max(byteLength, maxLen));
        if (!b->block) vm.throwError(Err::RangeError, "Array buffer allocation failed");
    } else {
        try {
            b->bytes.assign((size_t)byteLength, 0);
        } catch (std::bad_alloc&) {
            vm.throwError(Err::RangeError, "Array buffer allocation failed");
        }
    }
    vm.gc.noteExternal(b->size());
    return b;
}
}  // namespace
SharedBlock::~SharedBlock() {
    if (base) VirtualFree(base, 0, MEM_RELEASE);
}
std::shared_ptr<SharedBlock> newSharedBlock(size_t length, size_t limit) {
    auto b = std::make_shared<SharedBlock>();
    b->limit = std::max<size_t>(limit, 1);
    b->base = (uint8_t*)VirtualAlloc(nullptr, b->limit, MEM_RESERVE, PAGE_READWRITE);
    if (!b->base || !growSharedBlock(*b, length)) return nullptr;
    return b;
}
// New pages arrive zeroed, and asking again for pages already in use changes nothing: agents may grow at once.
bool growSharedBlock(SharedBlock& b, size_t length) {
    if (length > b.limit || (length && !VirtualAlloc(b.base, length, MEM_COMMIT, PAGE_READWRITE))) return false;
    size_t now = b.length.load();
    while (now < length && !b.length.compare_exchange_weak(now, length)) {
    }
    return true;
}
namespace {
void detach(JSArrayBuffer* b) {
    std::vector<uint8_t>().swap(b->bytes);
    b->detached = true;
}

// RequireInternalSlot(O, [[TypedArrayName]])
JSTypedArray* thisTyped(CallInfo& ci) {
    JSTypedArray* t = asTyped(ci.thisv);
    if (!t) ci.vm.throwError(Err::TypeError, "this is not a typed array");
    return t;
}
[[noreturn]] void outOfBounds(Vm& vm) { vm.throwError(Err::TypeError, "The typed array's buffer is detached or has become too small for it"); }
[[noreturn]] void immutableError(Vm& vm) { vm.throwError(Err::TypeError, "Cannot write to an immutable ArrayBuffer"); }
// ValidateTypedArray (23.2.4.4); write: the caller is about to change elements
JSTypedArray* valid(Vm& vm, Value v, bool write = false) {
    JSTypedArray* t = asTyped(v);
    if (!t) vm.throwError(Err::TypeError, "this is not a typed array");
    if (write && t->buffer->immutable) immutableError(vm);
    if (typedOutOfBounds(t)) outOfBounds(vm);
    return t;
}
// TypedArrayGetElement: undefined when k names no element right now
Value element(Vm& vm, const JSTypedArray* t, double k) { return k >= 0 && k < typedLength(t) ? typedGet(vm, t, (uint32_t)k) : Value::undef(); }

JSTypedArray* makeTyped(Vm& vm, TA type, double length) {
    if (length * sizeOf(type) > kMaxBufferBytes) vm.throwError(Err::RangeError, "Invalid typed array length: %g", length);
    JSArrayBuffer* b = allocBuffer(vm, vm.arrayBufferProto, length * sizeOf(type));
    JSTypedArray* t = vm.alloc<JSTypedArray>();
    t->proto = vm.typedProtos[(int)type];
    t->buffer = b;
    t->length = (uint32_t)length;
    t->type = type;
    return t;
}

// TypedArrayCreateFromConstructor (23.2.4.1)
JSTypedArray* createFromCtor(Vm& vm, Value ctor, const Value* args, int argc, bool write = false) {
    Value r = vm.construct(ctor, args, argc, ctor);
    JSTypedArray* t = valid(vm, r, write);
    if (argc == 1 && args[0].isNumber() && (double)typedLength(t) < args[0].number()) vm.throwError(Err::TypeError, "The typed array that was created is too short");
    return t;
}
// TypedArraySpeciesCreate (23.2.4.3)
JSTypedArray* speciesCreate(Vm& vm, JSTypedArray* exemplar, const Value* args, int argc, bool write = false) {
    Value ctor = speciesCtor(vm, exemplar, vm.typedCtors[(int)exemplar->type]);
    JSTypedArray* r = createFromCtor(vm, ctor, args, argc, write);
    if (isBig(r->type) != isBig(exemplar->type)) vm.throwError(Err::TypeError, "Cannot mix BigInt typed arrays with other typed arrays");
    return r;
}

// ---------------------------------------------------------------------------- the typed array constructors
JSFN(typedBaseCtor) { ci.vm.throwError(Err::TypeError, "Abstract class TypedArray not directly constructable"); }

// InitializeTypedArrayFromArrayBuffer (23.2.5.1.3)
void initFromBuffer(Vm& vm, JSTypedArray* t, JSArrayBuffer* b, Value byteOffset, Value length) {
    int sz = sizeOf(t->type);
    const char* name = kTypedNames[(int)t->type];
    double offset = vm.toIndex(byteOffset);
    if (std::fmod(offset, sz) != 0) vm.throwError(Err::RangeError, "Start offset of %s should be a multiple of %d", name, sz);
    double newLength = length.isUndef() ? 0 : vm.toIndex(length);
    if (b->detached) vm.throwError(Err::TypeError, "Cannot make a %s on a detached ArrayBuffer", name);
    double blen = (double)b->size();
    if (length.isUndef() && b->maxLen >= 0) {
        if (offset > blen) vm.throwError(Err::RangeError, "Start offset %g is outside the bounds of the buffer", offset);
        t->tracking = true;
    } else {
        double newByteLength;
        if (length.isUndef()) {
            if (std::fmod(blen, sz) != 0) vm.throwError(Err::RangeError, "Byte length of %s should be a multiple of %d", name, sz);
            newByteLength = blen - offset;
            if (newByteLength < 0) vm.throwError(Err::RangeError, "Start offset %g is outside the bounds of the buffer", offset);
        } else {
            newByteLength = newLength * sz;
            if (offset + newByteLength > blen) vm.throwError(Err::RangeError, "Invalid typed array length: %g", newLength);
        }
        t->length = (uint32_t)(newByteLength / sz);
    }
    t->buffer = b;
    t->offset = (uint32_t)offset;
}

JSFN(typedCtor) {
    Vm& vm = ci.vm;
    TA type = (TA)ci.callee->data.asInt();
    int sz = sizeOf(type);
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Constructor %s requires 'new'", kTypedNames[(int)type]);
    if (ci.argc == 0 || !ci.args[0].isObject()) {
        double n = ci.argc ? vm.toIndex(ci.args[0]) : 0;
        JSObject* proto = vm.protoFromNewTarget(ci.newTarget, vm.typedProtos[(int)type]);
        JSTypedArray* t = makeTyped(vm, type, n);
        t->proto = proto;
        return V(t);
    }
    JSObject* proto = vm.protoFromNewTarget(ci.newTarget, vm.typedProtos[(int)type]);
    JSTypedArray* t = vm.alloc<JSTypedArray>();
    t->type = type;
    t->proto = proto;
    Value first = ci.args[0];
    auto attach = [&](double n) {  // AllocateTypedArrayBuffer
        if (n * sz > kMaxBufferBytes) vm.throwError(Err::RangeError, "Invalid typed array length: %g", n);
        t->buffer = allocBuffer(vm, vm.arrayBufferProto, n * sz);
        t->length = (uint32_t)n;
    };
    if (JSTypedArray* src = asTyped(first)) {  // InitializeTypedArrayFromTypedArray
        if (typedOutOfBounds(src)) outOfBounds(vm);
        uint32_t n = typedLength(src);
        attach(n);
        if (src->type == type) {
            if (n) memcpy(t->buffer->data(), at(src, 0), (size_t)n * sz);
        } else {
            if (isBig(src->type) != isBig(type)) vm.throwError(Err::TypeError, "Cannot mix BigInt typed arrays with other typed arrays");
            for (uint32_t i = 0; i < n; ++i) storeElement(at(t, i), type, typedGet(vm, src, i));
        }
    } else if (JSArrayBuffer* b = asBuffer(first)) {
        initFromBuffer(vm, t, b, ci.arg(1), ci.arg(2));
    } else {  // InitializeTypedArrayFromElements
        std::vector<Value> vals;
        RootScope keep(vm, vals);
        Value method = getMethod(vm, first, PropKey::of(vm.symIterator));
        bool iterable = !method.isUndef();
        double n;
        if (iterable) {
            iterToList(vm, first, method, vals);
            n = (double)vals.size();
        } else
            n = lengthOfArrayLike(vm, first);
        attach(n);
        for (uint32_t k = 0; k < (uint32_t)n; ++k) typedSet(vm, t, k, iterable ? vals[k] : vm.get(first, PropKey::index(k)));
    }
    return V(t);
}

JSFN(typedFrom) {
    Vm& vm = ci.vm;
    Value ctor = ci.thisv, mapper = ci.arg(1), source = ci.arg(0);
    if (!vm.isConstructor(ctor)) vm.throwError(Err::TypeError, "this is not a constructor");
    bool mapping = !mapper.isUndef();
    if (mapping && !vm.isCallable(mapper)) vm.throwError(Err::TypeError, "The mapping function is not callable");
    Value method = getMethod(vm, source, PropKey::of(vm.symIterator));
    std::vector<Value> vals;
    RootScope keep(vm, vals);
    Value arrayLike;
    double len;
    if (!method.isUndef()) {
        iterToList(vm, source, method, vals);
        len = (double)vals.size();
    } else {
        arrayLike = V(vm.toObject(source));
        len = lengthOfArrayLike(vm, arrayLike);
    }
    Value n = Value::num(len);
    JSTypedArray* target = createFromCtor(vm, ctor, &n, 1, true);
    for (double k = 0; k < len; ++k) {
        Value kv = arrayLike.isObject() ? vm.get(arrayLike, PropKey::index((uint32_t)k)) : vals[(size_t)k];
        if (mapping) {
            Value args[2] = {kv, Value::num(k)};
            kv = vm.call(mapper, ci.arg(2), args, 2);
        }
        typedSet(vm, target, (uint32_t)k, kv);
    }
    return V(target);
}
JSFN(typedOf) {
    Vm& vm = ci.vm;
    if (!vm.isConstructor(ci.thisv)) vm.throwError(Err::TypeError, "this is not a constructor");
    Value n = Value::i32(ci.argc);
    JSTypedArray* target = createFromCtor(vm, ci.thisv, &n, 1, true);
    for (int k = 0; k < ci.argc; ++k) typedSet(vm, target, (uint32_t)k, ci.args[k]);
    return V(target);
}
JSFN(returnThis) { return ci.thisv; }

// ---------------------------------------------------------------------------- %TypedArray%.prototype
JSFN(typedInfo) {  // data: 0 length, 1 byteLength, 2 byteOffset, 3 buffer
    JSTypedArray* t = thisTyped(ci);
    int what = ci.callee->data.asInt();
    if (what == 3) return V(t->buffer);
    if (typedOutOfBounds(t)) return Value::i32(0);
    if (what == 0) return Value::num((double)typedLength(t));
    if (what == 1) return Value::num((double)typedLength(t) * sizeOf(t->type));
    return Value::num((double)t->offset);
}
JSFN(typedTag) {
    JSTypedArray* t = asTyped(ci.thisv);
    return t ? ci.vm.strv(kTypedNames[(int)t->type]) : Value::undef();
}
JSFN(typedAt) {
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    double len = typedLength(t), rel = vm.toInteger(ci.arg(0));
    double k = rel >= 0 ? rel : len + rel;
    if (k < 0 || k >= len) return Value::undef();
    return element(vm, t, k);
}
JSFN(typedCopyWithin) {
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv, true);
    double len = typedLength(t);
    double to = clampIndex(vm, ci.arg(0), len), from = clampIndex(vm, ci.arg(1), len);
    double end = ci.arg(2).isUndef() ? len : clampIndex(vm, ci.arg(2), len);
    double count = std::min(end - from, len - to);
    if (count > 0) {
        if (typedOutOfBounds(t)) outOfBounds(vm);
        len = typedLength(t);
        count = std::min({count, len - from, len - to});
        if (count > 0) memmove(at(t, to), at(t, from), (size_t)count * sizeOf(t->type));
    }
    return ci.thisv;
}
JSFN(typedIter) {  // data: 0 values, 1 keys, 2 entries
    valid(ci.vm, ci.thisv);
    return V(ci.vm.newIter(Cls::ArrayIter, ci.thisv, (uint8_t)ci.callee->data.asInt()));
}
JSFN(typedLoop) {  // data: 0 every, 1 some, 2 forEach, 3 find, 4 findIndex, 5 findLast, 6 findLastIndex
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    uint32_t len = typedLength(t);
    int mode = ci.callee->data.asInt();
    Value fn = ci.arg(0);
    if (!vm.isCallable(fn)) vm.throwError(Err::TypeError, "%s is not a function", utf16to8(vm.inspect(fn, 0)).c_str());
    for (uint32_t n = 0; n < len; ++n) {
        uint32_t k = mode >= 5 ? len - 1 - n : n;
        Value v = element(vm, t, k);
        Value args[3] = {v, Value::num(k), ci.thisv};
        bool r = vm.toBool(vm.call(fn, ci.arg(1), args, 3));
        if (mode == 0 && !r) return Value::boolean(false);
        if (mode == 1 && r) return Value::boolean(true);
        if ((mode == 3 || mode == 5) && r) return v;
        if ((mode == 4 || mode == 6) && r) return Value::num(k);
    }
    if (mode <= 1) return Value::boolean(mode == 0);
    return (mode == 4 || mode == 6) ? Value::i32(-1) : Value::undef();
}
JSFN(typedFill) {
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv, true);
    double len = typedLength(t);
    Value v = toElement(vm, t->type, ci.arg(0));
    double k = clampIndex(vm, ci.arg(1), len);
    double end = ci.arg(2).isUndef() ? len : clampIndex(vm, ci.arg(2), len);
    if (typedOutOfBounds(t)) outOfBounds(vm);
    end = std::min(end, (double)typedLength(t));
    for (; k < end; ++k) storeElement(at(t, k), t->type, v);
    return ci.thisv;
}
JSFN(typedFilter) {
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    uint32_t len = typedLength(t);
    Value fn = ci.arg(0);
    if (!vm.isCallable(fn)) vm.throwError(Err::TypeError, "%s is not a function", utf16to8(vm.inspect(fn, 0)).c_str());
    std::vector<Value> kept;
    RootScope keep(vm, kept);
    for (uint32_t k = 0; k < len; ++k) {
        Value v = element(vm, t, k);
        Value args[3] = {v, Value::num(k), ci.thisv};
        if (vm.toBool(vm.call(fn, ci.arg(1), args, 3))) kept.push_back(v);
    }
    Value n = Value::num((double)kept.size());
    JSTypedArray* r = speciesCreate(vm, t, &n, 1, true);
    for (size_t i = 0; i < kept.size(); ++i) typedSet(vm, r, (uint32_t)i, kept[i]);
    return V(r);
}
JSFN(typedIncludes) {  // data: 0 includes, 1 indexOf
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    double len = typedLength(t);
    bool indexOf = ci.callee->data.asInt() == 1;
    if (len == 0) return indexOf ? Value::i32(-1) : Value::boolean(false);
    for (double k = clampIndex(vm, ci.arg(1), len); k < len; ++k) {
        if (indexOf) {
            if (k < typedLength(t) && vm.strictEquals(ci.arg(0), typedGet(vm, t, (uint32_t)k))) return Value::num(k);
        } else if (vm.sameValueZero(ci.arg(0), element(vm, t, k)))
            return Value::boolean(true);
    }
    return indexOf ? Value::i32(-1) : Value::boolean(false);
}
JSFN(typedLastIndexOf) {
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    double len = typedLength(t);
    if (len == 0) return Value::i32(-1);
    double k = len - 1;
    if (ci.argc > 1) {
        double rel = vm.toInteger(ci.args[1]);
        k = std::min(rel >= 0 ? rel : len + rel, len - 1);
    }
    for (; k >= 0; --k)
        if (k < typedLength(t) && vm.strictEquals(ci.arg(0), typedGet(vm, t, (uint32_t)k))) return Value::num(k + 0.0);
    return Value::i32(-1);
}
JSFN(typedJoin) {  // data: 1 = toLocaleString
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    uint32_t len = typedLength(t);
    bool locale = ci.callee->data.asInt() == 1;
    std::u16string sep = u",";
    if (!locale && !ci.arg(0).isUndef()) sep = vm.toString(ci.arg(0))->s;
    std::u16string out;
    for (uint32_t k = 0; k < len; ++k) {
        if (k) out += sep;
        Value e = element(vm, t, k);
        if (e.isNullish()) continue;
        if (locale) {  // ECMA-402 19.6.1: the locales and options go on to each element
            Value args[2] = {ci.arg(0), ci.arg(1)};
            e = vm.call(vm.get(e, "toLocaleString"), e, args, 2);
        }
        out += vm.toString(e)->s;
    }
    return vm.strv(std::move(out));
}
JSFN(typedMap) {
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    uint32_t len = typedLength(t);
    Value fn = ci.arg(0);
    if (!vm.isCallable(fn)) vm.throwError(Err::TypeError, "%s is not a function", utf16to8(vm.inspect(fn, 0)).c_str());
    Value n = Value::num(len);
    JSTypedArray* r = speciesCreate(vm, t, &n, 1, true);
    for (uint32_t k = 0; k < len; ++k) {
        Value args[3] = {element(vm, t, k), Value::num(k), ci.thisv};
        typedSet(vm, r, k, vm.call(fn, ci.arg(1), args, 3));
    }
    return V(r);
}
JSFN(typedReduce) {  // data: 1 = reduceRight
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    uint32_t len = typedLength(t);
    bool right = ci.callee->data.asInt() == 1;
    Value fn = ci.arg(0);
    if (!vm.isCallable(fn)) vm.throwError(Err::TypeError, "%s is not a function", utf16to8(vm.inspect(fn, 0)).c_str());
    if (len == 0 && ci.argc < 2) vm.throwError(Err::TypeError, "Reduce of empty array with no initial value");
    uint32_t n = 0;
    Value acc = ci.argc >= 2 ? ci.args[1] : element(vm, t, right ? len - 1 - n++ : n++);
    for (; n < len; ++n) {
        uint32_t k = right ? len - 1 - n : n;
        Value args[4] = {acc, element(vm, t, k), Value::num(k), ci.thisv};
        acc = vm.call(fn, Value::undef(), args, 4);
    }
    return acc;
}
JSFN(typedReverse) {
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv, true);
    uint32_t len = typedLength(t);
    int sz = sizeOf(t->type);
    for (uint32_t lo = 0, hi = len; lo + 1 < hi; ++lo) {
        --hi;
        uint8_t tmp[8];
        memcpy(tmp, at(t, lo), sz);
        memcpy(at(t, lo), at(t, hi), sz);
        memcpy(at(t, hi), tmp, sz);
    }
    return ci.thisv;
}
JSFN(typedSetFn) {
    Vm& vm = ci.vm;
    JSTypedArray* t = thisTyped(ci);
    if (t->buffer->immutable) immutableError(vm);
    double off = vm.toInteger(ci.arg(1));
    if (off < 0) vm.throwError(Err::RangeError, "offset is out of bounds");
    if (JSTypedArray* s = asTyped(ci.arg(0))) {  // SetTypedArrayFromTypedArray
        if (typedOutOfBounds(t) || typedOutOfBounds(s)) outOfBounds(vm);
        double tl = typedLength(t), sl = typedLength(s);
        if (off == INFINITY || sl + off > tl) vm.throwError(Err::RangeError, "offset is out of bounds");
        if (isBig(t->type) != isBig(s->type)) vm.throwError(Err::TypeError, "Cannot mix BigInt typed arrays with other typed arrays");
        int ssz = sizeOf(s->type);
        if (s->type == t->type) {
            if (sl) memmove(at(t, off), at(s, 0), (size_t)sl * ssz);
        } else {
            std::vector<uint8_t> copy;  // the source is read as it was before any element is written
            const uint8_t* sp = at(s, 0);
            if (s->buffer == t->buffer) {
                copy.assign(sp, sp + (size_t)sl * ssz);
                sp = copy.data();
            }
            for (double i = 0; i < sl; ++i) storeElement(at(t, off + i), t->type, loadRaw(vm, sp + (size_t)i * ssz, s->type));
        }
        return Value::undef();
    }
    // SetTypedArrayFromArrayLike
    if (typedOutOfBounds(t)) outOfBounds(vm);
    double tl = typedLength(t);
    Value src = V(vm.toObject(ci.arg(0)));
    double sl = lengthOfArrayLike(vm, src);
    if (off == INFINITY || sl + off > tl) vm.throwError(Err::RangeError, "offset is out of bounds");
    for (double k = 0; k < sl; ++k) typedSet(vm, t, (uint32_t)(off + k), vm.get(src, PropKey::index((uint32_t)k)));
    return Value::undef();
}
JSFN(typedSlice) {
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    double len = typedLength(t);
    double start = clampIndex(vm, ci.arg(0), len);
    double end = ci.arg(1).isUndef() ? len : clampIndex(vm, ci.arg(1), len);
    double count = std::max(end - start, 0.0);
    Value n = Value::num(count);
    JSTypedArray* r = speciesCreate(vm, t, &n, 1, true);
    if (count > 0) {
        if (typedOutOfBounds(t)) outOfBounds(vm);
        end = std::min(end, (double)typedLength(t));
        count = std::max(end - start, 0.0);
        if (r->type == t->type) {
            const uint8_t* from = at(t, start);  // byte by byte, front to back, as the spec does: the two may overlap
            uint8_t* to = at(r, 0);
            for (size_t i = 0, bytes = (size_t)count * sizeOf(t->type); i < bytes; ++i) to[i] = from[i];
        } else {
            uint32_t out = 0;
            for (double k = start; k < end; ++k) typedSet(vm, r, out++, element(vm, t, k));
        }
    }
    return V(r);
}

// CompareTypedArrayElements without a comparator: numbers in order, -0 before +0, NaN last
bool numberBefore(double a, double b) {
    if (a < b) return true;
    if (a == 0 && b == 0) return std::signbit(a) && !std::signbit(b);
    return a == a && b != b;
}
void sortElements(Vm& vm, std::vector<Value>& vals, Value comparator, bool big) {
    if (comparator.isUndef()) {
        if (big) std::stable_sort(vals.begin(), vals.end(), [](Value a, Value b) { return bigCompare(a.asBigInt(), b.asBigInt()) < 0; });
        else std::stable_sort(vals.begin(), vals.end(), [](Value a, Value b) { return numberBefore(a.number(), b.number()); });
        return;
    }
    // a merge sort that never reads outside the list, whatever the comparator answers
    std::vector<Value> tmp(vals.size());
    RootScope keep(vm, tmp);
    auto before = [&](Value a, Value b) {
        Value args[2] = {a, b};
        double r = vm.toNumber(vm.call(comparator, Value::undef(), args, 2));
        return r < 0;
    };
    for (size_t width = 1; width < vals.size(); width *= 2) {
        for (size_t lo = 0; lo < vals.size(); lo += 2 * width) {
            size_t mid = std::min(lo + width, vals.size()), hi = std::min(lo + 2 * width, vals.size());
            size_t a = lo, b = mid, o = lo;
            while (a < mid && b < hi) tmp[o++] = before(vals[b], vals[a]) ? vals[b++] : vals[a++];
            while (a < mid) tmp[o++] = vals[a++];
            while (b < hi) tmp[o++] = vals[b++];
        }
        vals.swap(tmp);
    }
}
JSFN(typedSort) {  // data: 1 = toSorted
    Vm& vm = ci.vm;
    Value comparator = ci.arg(0);
    if (!comparator.isUndef() && !vm.isCallable(comparator)) vm.throwError(Err::TypeError, "The comparison function must be either a function or undefined");
    bool copy = ci.callee->data.asInt() == 1;
    JSTypedArray* t = valid(vm, ci.thisv, !copy);
    uint32_t len = typedLength(t);
    JSTypedArray* target = copy ? makeTyped(vm, t->type, len) : t;
    std::vector<Value> vals(len);
    RootScope keep(vm, vals);
    for (uint32_t i = 0; i < len; ++i) vals[i] = typedGet(vm, t, i);
    sortElements(vm, vals, comparator, isBig(t->type));
    uint32_t now = typedLength(target);
    for (uint32_t i = 0; i < len && i < now; ++i) storeElement(at(target, i), target->type, vals[i]);
    return V(target);
}
JSFN(typedSubarray) {
    Vm& vm = ci.vm;
    JSTypedArray* t = thisTyped(ci);
    double srcLen = typedOutOfBounds(t) ? 0 : typedLength(t);
    double start = clampIndex(vm, ci.arg(0), srcLen);
    double begin = t->offset + start * sizeOf(t->type);
    if (t->tracking && ci.arg(1).isUndef()) {
        Value args[2] = {V(t->buffer), Value::num(begin)};
        return V(speciesCreate(vm, t, args, 2));
    }
    double end = ci.arg(1).isUndef() ? srcLen : clampIndex(vm, ci.arg(1), srcLen);
    Value args[3] = {V(t->buffer), Value::num(begin), Value::num(std::max(end - start, 0.0))};
    return V(speciesCreate(vm, t, args, 3));
}
JSFN(typedToReversed) {
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    uint32_t len = typedLength(t);
    JSTypedArray* r = makeTyped(vm, t->type, len);
    int sz = sizeOf(t->type);
    for (uint32_t k = 0; k < len; ++k) memcpy(at(r, k), at(t, len - 1 - k), sz);
    return V(r);
}
JSFN(typedWith) {
    Vm& vm = ci.vm;
    JSTypedArray* t = valid(vm, ci.thisv);
    double len = typedLength(t), rel = vm.toInteger(ci.arg(0));
    double index = rel >= 0 ? rel : len + rel;
    Value v = toElement(vm, t->type, ci.arg(1));
    if (!(index >= 0 && index < typedLength(t))) vm.throwError(Err::RangeError, "Invalid typed array index");
    JSTypedArray* r = makeTyped(vm, t->type, len);
    for (double k = 0; k < len; ++k) typedSet(vm, r, (uint32_t)k, k == index ? v : element(vm, t, k));
    return V(r);
}

// ---------------------------------------------------------------------------- ArrayBuffer and SharedArrayBuffer
JSArrayBuffer* thisBuffer(CallInfo& ci, bool shared) {
    JSArrayBuffer* b = asBuffer(ci.thisv);
    if (!b || b->shared != shared) ci.vm.throwError(Err::TypeError, shared ? "this is not a SharedArrayBuffer" : "this is not an ArrayBuffer");
    return b;
}
JSFN(bufferCtor) {  // data: 1 = SharedArrayBuffer
    Vm& vm = ci.vm;
    bool shared = ci.callee->data.asInt() == 1;
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Constructor %s requires 'new'", shared ? "SharedArrayBuffer" : "ArrayBuffer");
    double len = vm.toIndex(ci.arg(0));
    double maxLen = -1;
    if (ci.arg(1).isObject()) {
        Value m = vm.get(ci.arg(1), "maxByteLength");
        if (!m.isUndef()) maxLen = vm.toIndex(m);
    }
    if (maxLen >= 0 && len > maxLen) vm.throwError(Err::RangeError, "The length is greater than maxByteLength");
    JSObject* proto = vm.protoFromNewTarget(ci.newTarget, shared ? vm.sharedBufferProto : vm.arrayBufferProto);
    return V(allocBuffer(vm, proto, len, maxLen, shared));
}
JSFN(bufferInfo) {  // data: 0 byteLength, 1 maxByteLength, 2 resizable or growable, 3 detached; data2: 1 = shared
    JSArrayBuffer* b = thisBuffer(ci, ci.callee->data2.asInt() == 1);
    switch (ci.callee->data.asInt()) {
        case 0: return Value::num((double)b->size());
        case 1: return Value::num(b->detached ? 0 : b->maxLen >= 0 ? b->maxLen : (double)b->size());
        case 2: return Value::boolean(b->maxLen >= 0);
        case 3: return Value::boolean(b->detached);
        default: return Value::boolean(b->immutable);
    }
}
JSFN(bufferResize) {  // data: 1 = SharedArrayBuffer.prototype.grow
    Vm& vm = ci.vm;
    bool shared = ci.callee->data.asInt() == 1;
    JSArrayBuffer* b = thisBuffer(ci, shared);
    if (b->maxLen < 0) vm.throwError(Err::TypeError, "The buffer has a fixed length");
    double n = vm.toIndex(ci.arg(0));
    if (b->detached) vm.throwError(Err::TypeError, "The ArrayBuffer is detached");
    double cur = (double)b->size();
    if (n > b->maxLen || (shared && n < cur)) vm.throwError(Err::RangeError, "Invalid length for this buffer");
    if (!shared) b->bytes.resize((size_t)n, 0);
    else if (!growSharedBlock(*b->block, (size_t)n)) vm.throwError(Err::RangeError, "Array buffer allocation failed");
    if (n > cur) vm.gc.noteExternal((size_t)(n - cur));
    return Value::undef();
}
JSFN(bufferSlice) {  // data: 1 = shared
    Vm& vm = ci.vm;
    bool shared = ci.callee->data.asInt() == 1;
    JSArrayBuffer* b = thisBuffer(ci, shared);
    if (b->detached) vm.throwError(Err::TypeError, "The ArrayBuffer is detached");
    double len = (double)b->size();
    double first = clampIndex(vm, ci.arg(0), len);
    double final = ci.arg(1).isUndef() ? len : clampIndex(vm, ci.arg(1), len);
    double newLen = std::max(final - first, 0.0);
    Value ctor = speciesCtor(vm, b, shared ? vm.sharedBufferCtor : vm.arrayBufferCtor);
    Value n = Value::num(newLen);
    Value made = vm.construct(ctor, &n, 1, ctor);
    JSArrayBuffer* nb = asBuffer(made);
    if (!nb || nb->shared != shared) vm.throwError(Err::TypeError, "The species constructor did not return a buffer of the same kind");
    if (nb->detached) vm.throwError(Err::TypeError, "The new ArrayBuffer is detached");
    if (nb->immutable) immutableError(vm);
    if (nb == b) vm.throwError(Err::TypeError, "The species constructor returned the same buffer");
    if ((double)nb->size() < newLen) vm.throwError(Err::TypeError, "The new buffer is too small");
    if (b->detached) vm.throwError(Err::TypeError, "The ArrayBuffer is detached");
    double room = (double)b->size() - first;
    if (room > 0) memcpy(nb->data(), b->data() + (size_t)first, (size_t)std::min(newLen, room));
    return made;
}
JSFN(bufferTransfer) {  // data: 0 transfer, 1 transferToFixedLength, 2 transferToImmutable
    Vm& vm = ci.vm;
    JSArrayBuffer* b = thisBuffer(ci, false);
    int mode = ci.callee->data.asInt();
    double newLen = ci.arg(0).isUndef() ? (double)b->size() : vm.toIndex(ci.arg(0));
    if (b->detached) vm.throwError(Err::TypeError, "The ArrayBuffer is detached");
    if (b->immutable) vm.throwError(Err::TypeError, "An immutable ArrayBuffer cannot be transferred");
    double maxLen = mode == 0 ? b->maxLen : -1;
    if (maxLen >= 0 && newLen > maxLen) vm.throwError(Err::RangeError, "The length is greater than maxByteLength");
    JSArrayBuffer* nb = allocBuffer(vm, vm.arrayBufferProto, newLen, maxLen);
    size_t n = std::min(nb->size(), b->size());
    if (n) memcpy(nb->data(), b->data(), n);
    nb->immutable = mode == 2;
    detach(b);
    return V(nb);
}
JSFN(bufferSliceToImmutable) {
    Vm& vm = ci.vm;
    JSArrayBuffer* b = thisBuffer(ci, false);
    if (b->detached) vm.throwError(Err::TypeError, "The ArrayBuffer is detached");
    double len = (double)b->size();
    double first = clampIndex(vm, ci.arg(0), len);
    double final = ci.arg(1).isUndef() ? len : clampIndex(vm, ci.arg(1), len);
    double newLen = std::max(final - first, 0.0);
    if (b->detached) vm.throwError(Err::TypeError, "The ArrayBuffer is detached");
    if ((double)b->size() < final) vm.throwError(Err::RangeError, "The ArrayBuffer became too short");
    JSArrayBuffer* nb = allocBuffer(vm, vm.arrayBufferProto, newLen);
    if (newLen > 0) memcpy(nb->data(), b->data() + (size_t)first, (size_t)newLen);
    nb->immutable = true;
    return V(nb);
}
JSFN(bufferIsView) {
    Value v = ci.arg(0);
    return Value::boolean(v.isObject() && (v.asObject()->cls == Cls::TypedArray || v.asObject()->cls == Cls::DataView));
}

// ---------------------------------------------------------------------------- DataView (kept like a byte view)
JSFN(dataViewCtor) {
    Vm& vm = ci.vm;
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Constructor DataView requires 'new'");
    JSArrayBuffer* b = asBuffer(ci.arg(0));
    if (!b) vm.throwError(Err::TypeError, "First argument to DataView constructor must be an ArrayBuffer");
    double offset = vm.toIndex(ci.arg(1));
    if (b->detached) vm.throwError(Err::TypeError, "The ArrayBuffer is detached");
    double blen = (double)b->size();
    if (offset > blen) vm.throwError(Err::RangeError, "Start offset %g is outside the bounds of the buffer", offset);
    bool tracking = false;
    double viewLen = 0;
    if (ci.arg(2).isUndef()) {
        if (b->maxLen >= 0) tracking = true;
        else viewLen = blen - offset;
    } else {
        viewLen = vm.toIndex(ci.arg(2));
        if (offset + viewLen > blen) vm.throwError(Err::RangeError, "Invalid DataView length %g", viewLen);
    }
    JSObject* proto = vm.protoFromNewTarget(ci.newTarget, vm.dataViewProto);
    if (b->detached) vm.throwError(Err::TypeError, "The ArrayBuffer is detached");
    blen = (double)b->size();
    if (offset > blen || (!ci.arg(2).isUndef() && offset + viewLen > blen)) vm.throwError(Err::RangeError, "Invalid DataView length %g", viewLen);
    JSTypedArray* v = vm.alloc<JSTypedArray>();
    v->cls = Cls::DataView;
    v->proto = proto;
    v->buffer = b;
    v->offset = (uint32_t)offset;
    v->length = (uint32_t)viewLen;
    v->tracking = tracking;
    v->type = TA::U8;
    return V(v);
}
JSTypedArray* thisView(CallInfo& ci) {
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != Cls::DataView) ci.vm.throwError(Err::TypeError, "this is not a DataView");
    return (JSTypedArray*)ci.thisv.asObject();
}
JSFN(dataViewInfo) {  // data: 0 buffer, 1 byteLength, 2 byteOffset
    JSTypedArray* v = thisView(ci);
    int what = ci.callee->data.asInt();
    if (what == 0) return V(v->buffer);
    if (typedOutOfBounds(v)) return Value::i32(0);
    return Value::num((double)(what == 1 ? typedLength(v) : v->offset));
}
JSFN(dataViewAccess) {  // data: element type, data2: 1 = setter
    Vm& vm = ci.vm;
    JSTypedArray* v = thisView(ci);
    TA type = (TA)ci.callee->data.asInt();
    bool isSet = ci.callee->data2.asInt() == 1;
    int sz = sizeOf(type);
    if (isSet && v->buffer->immutable) immutableError(vm);
    double pos = vm.toIndex(ci.arg(0));
    Value value = isSet ? toElement(vm, type, ci.arg(1)) : Value::undef();
    bool little = vm.toBool(ci.arg(isSet ? 2 : 1));
    if (typedOutOfBounds(v)) vm.throwError(Err::TypeError, "The DataView's buffer is detached or has become too small for it");
    if (pos + sz > (double)typedLength(v)) vm.throwError(Err::RangeError, "Offset is outside the bounds of the DataView");
    uint8_t* p = v->buffer->data() + v->offset + (size_t)pos;
    uint8_t tmp[8] = {};  // this machine is little-endian
    if (isSet) {
        storeElement(tmp, type, value);
        for (int i = 0; i < sz; ++i) p[i] = tmp[little ? i : sz - 1 - i];
        return Value::undef();
    }
    for (int i = 0; i < sz; ++i) tmp[little ? i : sz - 1 - i] = p[i];
    return loadRaw(vm, tmp, type);
}

// ---------------------------------------------------------------------------- Atomics
// ValidateIntegerTypedArray (25.4.3.1)
JSTypedArray* atomicArray(Vm& vm, Value v, bool waitable, bool write = false) {
    JSTypedArray* t = valid(vm, v, write);
    bool ok = waitable ? (t->type == TA::I32 || t->type == TA::I64) : !(t->type == TA::F32 || t->type == TA::F64 || t->type == TA::F16 || t->type == TA::U8C);
    if (!ok) vm.throwError(Err::TypeError, "Atomics cannot work on a %s", kTypedNames[(int)t->type]);
    return t;
}
// ValidateAtomicAccess: where the element starts in the buffer
size_t atomicPos(Vm& vm, JSTypedArray* t, Value index) {
    double len = typedLength(t), i = vm.toIndex(index);
    if (i >= len) vm.throwError(Err::RangeError, "Invalid atomic access index");
    return t->offset + (size_t)i * sizeOf(t->type);
}
uint8_t* revalidate(Vm& vm, JSTypedArray* t, size_t pos) {
    if (typedOutOfBounds(t)) outOfBounds(vm);
    if (pos + sizeOf(t->type) > t->buffer->size()) vm.throwError(Err::RangeError, "Invalid atomic access index");
    return t->buffer->data() + pos;
}
Value atomicValue(Vm& vm, JSTypedArray* t, Value v) { return isBig(t->type) ? V(vm.toBigInt(v)) : Value::num(toInt(vm, v)); }

// One step on a place in a buffer that no other agent can come between (25.4.3.12 AtomicReadModifyWrite).
// op: 0 add, 1 and, 2 exchange, 3 or, 4 sub, 5 xor, 6 load, 7 store, 8 compare with a and put b. Returns what was there.
template <class T>
uint64_t atomicStepOf(uint8_t* p, int op, uint64_t a, uint64_t b) {
    std::atomic_ref<T> cell(*reinterpret_cast<T*>(p));
    T v = (T)a;
    switch (op) {
        case 0: return cell.fetch_add(v);
        case 1: return cell.fetch_and(v);
        case 2: return cell.exchange(v);
        case 3: return cell.fetch_or(v);
        case 4: return cell.fetch_sub(v);
        case 5: return cell.fetch_xor(v);
        case 6: return cell.load();
        case 7:
            cell.store(v);
            return v;
        default:
            cell.compare_exchange_strong(v, (T)b);
            return v;
    }
}
uint64_t atomicStep(uint8_t* p, TA type, int op, uint64_t a = 0, uint64_t b = 0) {
    switch (sizeOf(type)) {
        case 1: return atomicStepOf<uint8_t>(p, op, a, b);
        case 2: return atomicStepOf<uint16_t>(p, op, a, b);
        case 4: return atomicStepOf<uint32_t>(p, op, a, b);
        default: return atomicStepOf<uint64_t>(p, op, a, b);
    }
}
// An element's value as the bits it is stored with, and back
uint64_t elementBits(TA type, Value v) {
    uint8_t raw[8] = {};
    storeElement(raw, type, v);
    uint64_t bits = 0;
    memcpy(&bits, raw, 8);
    return bits;
}
Value bitsValue(Vm& vm, TA type, uint64_t bits) {
    uint8_t raw[8];
    memcpy(raw, &bits, 8);
    return loadRaw(vm, raw, type);
}

JSFN(atomicsOp) {  // data: 0 add, 1 and, 2 exchange, 3 or, 4 sub, 5 xor
    Vm& vm = ci.vm;
    JSTypedArray* t = atomicArray(vm, ci.arg(0), false, true);
    size_t pos = atomicPos(vm, t, ci.arg(1));
    Value v = atomicValue(vm, t, ci.arg(2));
    uint8_t* p = revalidate(vm, t, pos);
    return bitsValue(vm, t->type, atomicStep(p, t->type, ci.callee->data.asInt(), elementBits(t->type, v)));
}
JSFN(atomicsCompareExchange) {
    Vm& vm = ci.vm;
    JSTypedArray* t = atomicArray(vm, ci.arg(0), false, true);
    size_t pos = atomicPos(vm, t, ci.arg(1));
    Value expected = atomicValue(vm, t, ci.arg(2));
    Value replacement = atomicValue(vm, t, ci.arg(3));
    uint8_t* p = revalidate(vm, t, pos);
    return bitsValue(vm, t->type, atomicStep(p, t->type, 8, elementBits(t->type, expected), elementBits(t->type, replacement)));
}
JSFN(atomicsLoad) {
    Vm& vm = ci.vm;
    JSTypedArray* t = atomicArray(vm, ci.arg(0), false);
    size_t pos = atomicPos(vm, t, ci.arg(1));
    return bitsValue(vm, t->type, atomicStep(revalidate(vm, t, pos), t->type, 6));
}
JSFN(atomicsStore) {
    Vm& vm = ci.vm;
    JSTypedArray* t = atomicArray(vm, ci.arg(0), false, true);
    size_t pos = atomicPos(vm, t, ci.arg(1));
    Value v = atomicValue(vm, t, ci.arg(2));
    atomicStep(revalidate(vm, t, pos), t->type, 7, elementBits(t->type, v));
    return v;
}
JSFN(atomicsIsLockFree) {
    double n = ci.vm.toInteger(ci.arg(0));
    return Value::boolean(n == 1 || n == 2 || n == 4 || n == 8);
}

// ---- waiting and notifying (25.4.1 WaiterList records, 25.4.3.14 DoWait). The lists of who waits where belong to
// all agents together: they are keyed by the address of the place in shared memory, and one lock is the critical
// section of every list.
struct Waiter {
    std::condition_variable wake;        // Atomics.wait: the thread sleeps on this until it is notified
    bool notified = false;
    Vm* vm = nullptr;                    // Atomics.waitAsync: the engine that owns the promise, and the waiter's id there
    uint32_t id = 0;
    std::shared_ptr<SharedBlock> block;  // keeps the address from being handed to another buffer meanwhile
};
struct WaitHub {
    std::mutex lock;
    std::unordered_map<const uint8_t*, std::deque<std::shared_ptr<Waiter>>> lists;  // oldest waiter first
};
WaitHub& waitHub() {
    static WaitHub* hub = new WaitHub;  // never destroyed: other agents' threads may still run when the process ends
    return *hub;
}
// Both need hub.lock held.
void enlist(WaitHub& hub, const uint8_t* place, const std::shared_ptr<Waiter>& w) { hub.lists[place].push_back(w); }
bool delist(WaitHub& hub, const uint8_t* place, const Waiter* w) {  // false when a notification took it first
    auto at = hub.lists.find(place);
    if (at == hub.lists.end()) return false;
    auto& list = at->second;
    auto it = std::find_if(list.begin(), list.end(), [w](const std::shared_ptr<Waiter>& x) { return x.get() == w; });
    if (it == list.end()) return false;
    list.erase(it);
    if (list.empty()) hub.lists.erase(at);
    return true;
}
double monotonicMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Settles this agent's waitAsync promise number `id`, if it is still waiting.
void settleWaiter(Vm& vm, uint32_t id, const char* how) {
    for (size_t i = 0; i < vm.asyncWaiters.size(); ++i)
        if (vm.asyncWaiters[i].id == id) {
            Value resolve = vm.asyncWaiters[i].resolve;
            vm.asyncWaiters.erase(vm.asyncWaiters.begin() + i);
            Value v = vm.strv(how);
            vm.call(resolve, Value::undef(), &v, 1);
            return;
        }
}
}  // namespace
void settleNotifiedWaiters(Vm& vm) {
    std::vector<uint32_t> ids;
    {
        std::lock_guard<std::mutex> g(vm.notifiedLock);
        ids.swap(vm.notifiedWaiters);
    }
    for (uint32_t id : ids) settleWaiter(vm, id, "ok");
}
void forgetWaiters(Vm& vm) {
    WaitHub& hub = waitHub();
    std::lock_guard<std::mutex> critical(hub.lock);
    for (auto at = hub.lists.begin(); at != hub.lists.end();) {
        auto& list = at->second;
        list.erase(std::remove_if(list.begin(), list.end(), [&vm](const std::shared_ptr<Waiter>& w) { return w->vm == &vm; }), list.end());
        at = list.empty() ? hub.lists.erase(at) : std::next(at);
    }
}
namespace {

JSFN(atomicsNotify) {
    Vm& vm = ci.vm;
    JSTypedArray* t = atomicArray(vm, ci.arg(0), true);
    size_t pos = atomicPos(vm, t, ci.arg(1));
    double count = INFINITY;
    if (!ci.arg(2).isUndef()) count = std::max(vm.toInteger(ci.arg(2)), 0.0);
    if (!t->buffer->shared) return Value::i32(0);
    const uint8_t* place = t->buffer->data() + pos;
    WaitHub& hub = waitHub();
    std::vector<uint32_t> mine;  // this agent's own waitAsync promises among those woken
    double woken = 0;
    {
        std::lock_guard<std::mutex> critical(hub.lock);
        auto at = hub.lists.find(place);
        while (at != hub.lists.end() && !at->second.empty() && woken < count) {
            std::shared_ptr<Waiter> w = at->second.front();
            at->second.pop_front();
            ++woken;
            if (!w->vm) {  // a sleeping thread
                w->notified = true;
                w->wake.notify_one();
            } else if (w->vm == &vm) {
                mine.push_back(w->id);
            } else {  // another agent's promise: that agent settles it. Its engine cannot go away while the lock is held.
                std::lock_guard<std::mutex> g(w->vm->notifiedLock);
                w->vm->notifiedWaiters.push_back(w->id);
            }
        }
        if (at != hub.lists.end() && at->second.empty()) hub.lists.erase(at);
    }
    for (uint32_t id : mine) settleWaiter(vm, id, "ok");
    return Value::num(woken);
}
JSFN(atomicsWaitTimedOut) {  // data: the waiter's id
    Vm& vm = ci.vm;
    uint32_t id = (uint32_t)ci.callee->data.asInt();
    for (const Vm::AsyncWaiter& a : vm.asyncWaiters) {
        if (a.id != id) continue;
        WaitHub& hub = waitHub();
        bool waiting = false;
        {
            std::lock_guard<std::mutex> critical(hub.lock);
            auto at = hub.lists.find(a.place);
            if (at != hub.lists.end())
                for (const std::shared_ptr<Waiter>& w : at->second)
                    if (w->vm == &vm && w->id == id) {
                        waiting = delist(hub, a.place, w.get());
                        break;
                    }
        }
        if (waiting) settleWaiter(vm, id, "timed-out");  // otherwise a notification is on its way
        break;
    }
    return Value::undef();
}
// The arguments of Atomics.wait and Atomics.waitAsync, checked and converted in the order DoWait gives
struct WaitCall {
    JSTypedArray* t;
    size_t pos;
    uint64_t expected;
    double timeout;  // milliseconds, possibly infinite
};
WaitCall waitCall(CallInfo& ci, const char* name) {
    Vm& vm = ci.vm;
    WaitCall c;
    c.t = atomicArray(vm, ci.arg(0), true);
    if (!c.t->buffer->shared) vm.throwError(Err::TypeError, "%s needs a typed array on a SharedArrayBuffer", name);
    c.pos = atomicPos(vm, c.t, ci.arg(1));
    c.expected = isBig(c.t->type) ? bigToU64(vm.toBigInt(ci.arg(2))) : (uint64_t)(uint32_t)vm.toInt32(ci.arg(2));
    double q = vm.toNumber(ci.arg(3));
    c.timeout = q != q ? INFINITY : std::max(q, 0.0);
    return c;
}
// DoWait (25.4.3.14) in its asynchronous form
JSFN(atomicsWaitAsync) {
    Vm& vm = ci.vm;
    WaitCall c = waitCall(ci, "Atomics.waitAsync");
    uint8_t* place = c.t->buffer->data() + c.pos;
    JSObject* result = vm.newObject();
    WaitHub& hub = waitHub();
    std::unique_lock<std::mutex> critical(hub.lock);
    uint64_t witness = atomicStep(place, c.t->type, 6);
    if (witness != c.expected || c.timeout == 0) {
        critical.unlock();
        vm.def(result, "async", Value::boolean(false), PF_ALL);
        vm.def(result, "value", vm.strv(witness != c.expected ? "not-equal" : "timed-out"), PF_ALL);
        return V(result);
    }
    auto w = std::make_shared<Waiter>();
    w->vm = &vm;
    w->id = vm.nextWaiterId++;
    w->block = c.t->buffer->block;
    enlist(hub, place, w);
    critical.unlock();
    JSPromise* promise = vm.newPromise();
    Value fns[2];
    makeResolvingFunctions(vm, promise, fns);
    vm.asyncWaiters.push_back({fns[0], w->id, place});
    if (c.timeout != INFINITY) {
        Value setTimer = vm.get(V(vm.global), "setTimeout");
        if (vm.isCallable(setTimer)) {
            JSNative* onTimeout = vm.newNative(atomicsWaitTimedOut, "", 0);
            onTimeout->data = Value::i32((int32_t)w->id);
            Value args[2] = {V(onTimeout), Value::num(c.timeout)};
            vm.call(setTimer, Value::undef(), args, 2);
        }
    }
    vm.def(result, "async", Value::boolean(true), PF_ALL);
    vm.def(result, "value", V(promise), PF_ALL);
    return V(result);
}
JSFN(atomicsWait) {
    Vm& vm = ci.vm;
    WaitCall c = waitCall(ci, "Atomics.wait");
    if (!vm.canBlock) vm.throwError(Err::TypeError, "Atomics.wait cannot block this thread");
    uint8_t* place = c.t->buffer->data() + c.pos;
    WaitHub& hub = waitHub();
    std::unique_lock<std::mutex> critical(hub.lock);
    if (atomicStep(place, c.t->type, 6) != c.expected) return vm.strv("not-equal");
    auto w = std::make_shared<Waiter>();
    w->block = c.t->buffer->block;
    enlist(hub, place, w);
    double start = monotonicMs();
    while (!w->notified) {
        double left = c.timeout - (monotonicMs() - start);
        if (left <= 0) break;
        // short naps, so that a script past its time limit still stops
        w->wake.wait_for(critical, std::chrono::duration<double, std::milli>(std::min(left, 20.0)));
        uint64_t deadline = vm.deadlineTick;
        if (deadline && GetTickCount64() > deadline) break;
    }
    bool notified = w->notified;
    if (!notified) delist(hub, place, w.get());
    critical.unlock();
    if (!notified) vm.pollInterrupt();
    return vm.strv(notified ? "ok" : "timed-out");
}
JSFN(atomicsPause) {
    Value n = ci.arg(0);
    if (!n.isUndef() && !(n.isNumber() && std::isfinite(n.number()) && n.number() == std::trunc(n.number())))
        ci.vm.throwError(Err::TypeError, "Atomics.pause takes an integer");
    return Value::undef();
}
JSFN(mathF16round) {
    double d = ci.vm.toNumber(ci.arg(0));
    return Value::num(halfToDouble(doubleToHalf(d)));
}

// ---------------------------------------------------------------------------- Uint8Array: base64 and hex text
struct Decoded {
    std::vector<uint8_t> bytes;
    size_t read = 0;
    const char* error = nullptr;
};
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int base64Value(char16_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
int hexValue(char16_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// DecodeBase64Chunk: 2, 3 or 4 characters; false when strict decoding finds bits that would be lost
bool decodeChunk(const int* chunk, int n, bool strict, std::vector<uint8_t>& out) {
    int c2 = n > 2 ? chunk[2] : 0, c3 = n > 3 ? chunk[3] : 0;
    uint8_t b0 = (uint8_t)((chunk[0] << 2) | (chunk[1] >> 4)), b1 = (uint8_t)(((chunk[1] & 15) << 4) | (c2 >> 2)), b2 = (uint8_t)(((c2 & 3) << 6) | c3);
    if (strict && ((n == 2 && b1) || (n == 3 && b2))) return false;
    out.push_back(b0);
    if (n > 2) out.push_back(b1);
    if (n > 3) out.push_back(b2);
    return true;
}
// FromBase64: lastChunk is 0 loose, 1 strict, 2 stop-before-partial
Decoded fromBase64(const std::u16string& s, bool url, int lastChunk, double maxLength) {
    Decoded r;
    if (maxLength == 0) return r;
    int chunk[4] = {}, chunkLen = 0;
    size_t index = 0, length = s.size();
    auto skip = [&](size_t i) {
        while (i < length && (s[i] == 9 || s[i] == 10 || s[i] == 12 || s[i] == 13 || s[i] == 32)) ++i;
        return i;
    };
    for (;;) {
        index = skip(index);
        if (index == length) {
            if (chunkLen > 0) {
                if (lastChunk == 2) return r;
                if (lastChunk == 1 || chunkLen == 1) {
                    r.error = "The base64 text ends in the middle of a group";
                    return r;
                }
                decodeChunk(chunk, chunkLen, false, r.bytes);
            }
            r.read = length;
            return r;
        }
        char16_t c = s[index++];
        if (c == '=') {
            if (chunkLen < 2) {
                r.error = "Unexpected padding in the base64 text";
                return r;
            }
            index = skip(index);
            if (chunkLen == 2) {
                if (index == length) {
                    if (lastChunk != 2) r.error = "The base64 padding is incomplete";
                    return r;
                }
                if (s[index] == '=') index = skip(index + 1);
            }
            if (index < length) {
                r.error = "Unexpected text after the base64 padding";
                return r;
            }
            if (!decodeChunk(chunk, chunkLen, lastChunk == 1, r.bytes)) {
                r.error = "The last base64 group has extra bits";
                return r;
            }
            r.read = length;
            return r;
        }
        if (url) {
            if (c == '+' || c == '/') c = 0;
            else if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        int v = base64Value(c);
        if (v < 0) {
            r.error = "The text has a character that is not base64";
            return r;
        }
        double remaining = maxLength - (double)r.bytes.size();
        if ((remaining == 1 && chunkLen == 2) || (remaining == 2 && chunkLen == 3)) return r;
        chunk[chunkLen++] = v;
        if (chunkLen == 4) {
            decodeChunk(chunk, 4, false, r.bytes);
            chunkLen = 0;
            r.read = index;
            if ((double)r.bytes.size() == maxLength) return r;
        }
    }
}
Decoded fromHex(const std::u16string& s, double maxLength) {
    Decoded r;
    if (s.size() % 2) {
        r.error = "The hex text has an odd number of characters";
        return r;
    }
    while (r.read < s.size() && (double)r.bytes.size() < maxLength) {
        int hi = hexValue(s[r.read]), lo = hexValue(s[r.read + 1]);
        if (hi < 0 || lo < 0) {
            r.error = "The text has a character that is not a hex digit";
            return r;
        }
        r.read += 2;
        r.bytes.push_back((uint8_t)(hi * 16 + lo));
    }
    return r;
}
// ValidateUint8Array
JSTypedArray* thisBytes(CallInfo& ci, bool write = false) {
    JSTypedArray* t = asTyped(ci.thisv);
    if (!t || t->type != TA::U8) ci.vm.throwError(Err::TypeError, "this is not a Uint8Array");
    if (write && t->buffer->immutable) immutableError(ci.vm);
    return t;
}
// GetOptionsObject, then one string option that must be one of the choices; the default is choice 0
Value optionsObject(Vm& vm, Value options) {
    if (!options.isUndef() && !options.isObject()) vm.throwError(Err::TypeError, "The options argument must be an object");
    return options;
}
int stringOption(Vm& vm, Value options, const char* name, std::initializer_list<const char16_t*> choices) {
    Value v = options.isObject() ? vm.get(options, name) : Value::undef();
    if (v.isUndef()) return 0;
    int i = 0;
    if (v.isString())
        for (const char16_t* c : choices) {
            if (v.asString()->s == c) return i;
            ++i;
        }
    vm.throwError(Err::TypeError, "Invalid value for the %s option", name);
}
JSFN(bytesToBase64) {
    Vm& vm = ci.vm;
    JSTypedArray* t = thisBytes(ci);
    Value options = optionsObject(vm, ci.arg(0));
    bool url = stringOption(vm, options, "alphabet", {u"base64", u"base64url"}) == 1;
    bool omitPadding = options.isObject() && vm.toBool(vm.get(options, "omitPadding"));
    if (typedOutOfBounds(t)) outOfBounds(vm);
    const uint8_t* p = at(t, 0);
    size_t n = typedLength(t);
    std::u16string out;
    out.reserve((n + 2) / 3 * 4);
    auto put = [&](int v) {
        char c = kBase64[v & 63];
        out += (char16_t)(url && c == '+' ? '-' : url && c == '/' ? '_' : c);
    };
    for (size_t i = 0; i < n; i += 3) {
        uint32_t a = p[i], b = i + 1 < n ? p[i + 1] : 0, c = i + 2 < n ? p[i + 2] : 0;
        put(a >> 2);
        put((a << 4) | (b >> 4));
        if (i + 1 < n) put((b << 2) | (c >> 6));
        else if (!omitPadding) out += u'=';
        if (i + 2 < n) put(c);
        else if (!omitPadding) out += u'=';
    }
    return vm.strv(std::move(out));
}
JSFN(bytesToHex) {
    Vm& vm = ci.vm;
    JSTypedArray* t = thisBytes(ci);
    if (typedOutOfBounds(t)) outOfBounds(vm);
    const uint8_t* p = at(t, 0);
    size_t n = typedLength(t);
    std::u16string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out += (char16_t)"0123456789abcdef"[p[i] >> 4];
        out += (char16_t)"0123456789abcdef"[p[i] & 15];
    }
    return vm.strv(std::move(out));
}
JSFN(bytesFromText) {  // data: 0 base64, 1 hex; data2: 1 = write into this array (setFromBase64, setFromHex)
    Vm& vm = ci.vm;
    bool hex = ci.callee->data.asInt() == 1, into = ci.callee->data2.asInt() == 1;
    JSTypedArray* t = into ? thisBytes(ci, true) : nullptr;
    if (!ci.arg(0).isString()) vm.throwError(Err::TypeError, "The first argument must be a string");
    bool url = false;
    int lastChunk = 0;
    if (!hex) {
        Value options = optionsObject(vm, ci.arg(1));
        url = stringOption(vm, options, "alphabet", {u"base64", u"base64url"}) == 1;
        lastChunk = stringOption(vm, options, "lastChunkHandling", {u"loose", u"strict", u"stop-before-partial"});
    }
    double maxLength = 9007199254740991.0;
    if (into) {
        if (typedOutOfBounds(t)) outOfBounds(vm);
        maxLength = typedLength(t);
    }
    const std::u16string& text = ci.arg(0).asString()->s;
    Decoded r = hex ? fromHex(text, maxLength) : fromBase64(text, url, lastChunk, maxLength);
    if (into) {  // what was decoded before an error is still written
        if (!r.bytes.empty()) memcpy(at(t, 0), r.bytes.data(), r.bytes.size());
        if (r.error) vm.throwError(Err::SyntaxError, "%s", r.error);
        JSObject* result = vm.newObject();
        vm.def(result, "read", Value::num((double)r.read), PF_ALL);
        vm.def(result, "written", Value::num((double)r.bytes.size()), PF_ALL);
        return V(result);
    }
    if (r.error) vm.throwError(Err::SyntaxError, "%s", r.error);
    JSTypedArray* made = makeTyped(vm, TA::U8, (double)r.bytes.size());
    if (!r.bytes.empty()) memcpy(at(made, 0), r.bytes.data(), r.bytes.size());
    return V(made);
}

// ---------------------------------------------------------------------------- bytes in, bytes out
bool bytesOf(Value v, const uint8_t*& p, size_t& n) {
    if (!v.isObject()) return false;
    JSObject* o = v.asObject();
    if (o->cls == Cls::ArrayBuffer) {
        p = ((JSArrayBuffer*)o)->data();
        n = ((JSArrayBuffer*)o)->size();
        return true;
    }
    if (o->cls == Cls::TypedArray || o->cls == Cls::DataView) {
        JSTypedArray* t = (JSTypedArray*)o;
        p = t->buffer->data() + t->offset;
        n = (size_t)typedLength(t) * sizeOf(t->type);
        return true;
    }
    return false;
}
JSFN(textEncoderCtor) {
    Vm& vm = ci.vm;
    JSObject* o = vm.newObject(vm.protoFromNewTarget(ci.newTarget, ci.callee->data2.asObject()));
    vm.def(o, "encoding", vm.strv("utf-8"), PF_ALL);
    return V(o);
}
JSFN(textEncode) {
    Vm& vm = ci.vm;
    std::string bytes = ci.arg(0).isUndef() ? std::string() : utf16to8(vm.toStr16(ci.arg(0)));
    JSTypedArray* t = newTypedArray(vm, TA::U8, (uint32_t)bytes.size());
    if (!bytes.empty()) memcpy(t->buffer->data(), bytes.data(), bytes.size());
    return V(t);
}
JSFN(textDecode) {
    Vm& vm = ci.vm;
    const uint8_t* p = nullptr;
    size_t n = 0;
    if (ci.arg(0).isUndef()) return V(vm.a.empty);
    if (!bytesOf(ci.arg(0), p, n)) vm.throwError(Err::TypeError, "TextDecoder.decode: the argument is not an ArrayBuffer or a view");
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) p += 3, n -= 3;
    return vm.strv(utf8to16(std::string_view((const char*)p, n)));
}
JSFN(cryptoGetRandomValues) {
    Vm& vm = ci.vm;
    Value v = ci.arg(0);
    JSTypedArray* t = asTyped(v);
    if (!t || t->type == TA::F32 || t->type == TA::F64 || t->type == TA::F16)
        vm.throwError(Err::TypeError, "crypto.getRandomValues: the argument is not an integer typed array");
    size_t n = (size_t)typedLength(t) * sizeOf(t->type);
    if (n > 65536) vm.throwError(Err::Error, "QuotaExceededError: at most 65536 random bytes per call");
    if (n) BCryptGenRandom(nullptr, t->buffer->data() + t->offset, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return v;
}
JSFN(cryptoRandomUUID) {
    uint8_t b[16];
    BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    char s[40];
    snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
             b[10], b[11], b[12], b[13], b[14], b[15]);
    return ci.vm.strv(s);
}

}  // namespace

// ---------------------------------------------------------------------------- what the rest of the engine uses
Value typedGet(Vm& vm, const JSTypedArray* t, uint32_t i) { return loadRaw(vm, at(t, i), t->type); }

// TypedArraySetElement (10.4.5.17): the value is converted first, and only stored if the index still names an element
void typedSet(Vm& vm, JSTypedArray* t, uint32_t i, Value v) {
    Value n = toElement(vm, t->type, v);
    if (i < typedLength(t)) storeElement(at(t, i), t->type, n);
}

JSTypedArray* newTypedArray(Vm& vm, TA type, uint32_t length) { return makeTyped(vm, type, length); }

// What a property key means to a typed array (10.4.5): 0 an ordinary key, 1 the index of an element, 2 a numeric key
// that names no element. Keys of the last kind never reach the ordinary property code.
int typedKeyKind(Vm& vm, const JSTypedArray* t, PropKey k) {
    if (k.isIndex()) return k.asIndex() < typedLength(t) ? 1 : 2;
    if (!k.isString()) return 0;
    const std::u16string& s = k.str()->s;
    if (s.empty()) return 0;
    char16_t c = s[0];
    if (!((c >= '0' && c <= '9') || c == '-' || c == 'I' || c == 'N')) return 0;
    if (s == u"-0") return 2;
    return vm.numberToString(vm.stringToNumber(s))->s == s ? 2 : 0;  // CanonicalNumericIndexString
}

// ToIndex (7.1.22)
double Vm::toIndex(Value v) {
    if (v.isUndef()) return 0;
    double d = toInteger(v);
    if (d < 0 || d > 9007199254740991.0) throwError(Err::RangeError, "Invalid index");
    return d + 0.0;
}

void setupTypedArrays(Vm& vm) {
    auto getter = [&](JSObject* o, PropKey key, const char* name, NativeFn fn, int data, int data2 = 0) {
        JSNative* g = vm.newNative(fn, name, 0);
        g->data = Value::i32(data);
        g->data2 = Value::i32(data2);
        vm.defineAccessor(o, key, V(g), Value::undef(), PF_C);
    };
    auto named = [&](JSObject* o, const char* name, const char* fnName, NativeFn fn, int data, int data2 = 0) {
        getter(o, PropKey::of(vm.atom(name)), fnName, fn, data, data2);
    };
    auto method = [&](JSObject* o, const char* name, NativeFn fn, int length, int data = 0, int data2 = 0) {
        JSNative* n = vm.defFn(o, name, fn, length);
        n->data = Value::i32(data);
        n->data2 = Value::i32(data2);
        return n;
    };

    // ArrayBuffer and SharedArrayBuffer
    for (int shared = 0; shared < 2; ++shared) {
        JSObject* proto = vm.newObject();
        const char* name = shared ? "SharedArrayBuffer" : "ArrayBuffer";
        JSNative* c = makeCtor(vm, name, bufferCtor, 1, proto);
        c->data = Value::i32(shared);
        (shared ? vm.sharedBufferProto : vm.arrayBufferProto) = proto;
        (shared ? vm.sharedBufferCtor : vm.arrayBufferCtor) = V(c);
        getter(c, PropKey::of(vm.symSpecies), "get [Symbol.species]", returnThis, 0);
        named(proto, "byteLength", "get byteLength", bufferInfo, 0, shared);
        named(proto, "maxByteLength", "get maxByteLength", bufferInfo, 1, shared);
        named(proto, shared ? "growable" : "resizable", shared ? "get growable" : "get resizable", bufferInfo, 2, shared);
        method(proto, shared ? "grow" : "resize", bufferResize, 1, shared);
        method(proto, "slice", bufferSlice, 2, shared);
        if (!shared) {
            vm.defFn(c, "isView", bufferIsView, 1);
            named(proto, "detached", "get detached", bufferInfo, 3, 0);
            named(proto, "immutable", "get immutable", bufferInfo, 4, 0);
            method(proto, "transfer", bufferTransfer, 0, 0);
            method(proto, "transferToFixedLength", bufferTransfer, 0, 1);
            method(proto, "transferToImmutable", bufferTransfer, 0, 2);
            method(proto, "sliceToImmutable", bufferSliceToImmutable, 2);
        }
        vm.defineOwn(proto, PropKey::of(vm.symToStringTag), vm.strv(name), PF_C);
    }

    // %TypedArray% and its prototype
    JSObject* base = vm.newObject();
    JSNative* baseCtor = vm.newNative(typedBaseCtor, "TypedArray", 0);
    baseCtor->oflags |= OF_CTOR;
    baseCtor->add(PropKey::of(vm.a.prototype), V(base), 0);
    vm.defineOwn(base, PropKey::of(vm.a.constructor), V(baseCtor), PF_W | PF_C);
    vm.defFn(baseCtor, "from", typedFrom, 1);
    vm.defFn(baseCtor, "of", typedOf, 0);
    getter(baseCtor, PropKey::of(vm.symSpecies), "get [Symbol.species]", returnThis, 0);
    named(base, "length", "get length", typedInfo, 0);
    named(base, "byteLength", "get byteLength", typedInfo, 1);
    named(base, "byteOffset", "get byteOffset", typedInfo, 2);
    named(base, "buffer", "get buffer", typedInfo, 3);
    getter(base, PropKey::of(vm.symToStringTag), "get [Symbol.toStringTag]", typedTag, 0);
    method(base, "at", typedAt, 1);
    method(base, "copyWithin", typedCopyWithin, 2);
    method(base, "entries", typedIter, 0, 2);
    method(base, "keys", typedIter, 0, 1);
    JSNative* values = method(base, "values", typedIter, 0, 0);
    vm.defineOwn(base, PropKey::of(vm.symIterator), V(values), PF_W | PF_C);
    method(base, "every", typedLoop, 1, 0);
    method(base, "some", typedLoop, 1, 1);
    method(base, "forEach", typedLoop, 1, 2);
    method(base, "find", typedLoop, 1, 3);
    method(base, "findIndex", typedLoop, 1, 4);
    method(base, "findLast", typedLoop, 1, 5);
    method(base, "findLastIndex", typedLoop, 1, 6);
    method(base, "fill", typedFill, 1);
    method(base, "filter", typedFilter, 1);
    method(base, "includes", typedIncludes, 1, 0);
    method(base, "indexOf", typedIncludes, 1, 1);
    method(base, "lastIndexOf", typedLastIndexOf, 1);
    method(base, "join", typedJoin, 1, 0);
    method(base, "toLocaleString", typedJoin, 0, 1);
    method(base, "map", typedMap, 1);
    method(base, "reduce", typedReduce, 1, 0);
    method(base, "reduceRight", typedReduce, 1, 1);
    method(base, "reverse", typedReverse, 0);
    method(base, "set", typedSetFn, 1);
    method(base, "slice", typedSlice, 2);
    method(base, "sort", typedSort, 1, 0);
    method(base, "toSorted", typedSort, 1, 1);
    method(base, "subarray", typedSubarray, 2);
    method(base, "toReversed", typedToReversed, 0);
    method(base, "with", typedWith, 2);
    vm.def(base, "toString", vm.get(V(vm.arrayProto), "toString"));

    for (int i = 0; i < kTypeCount; ++i) {
        JSObject* proto = vm.newObject(base);
        vm.typedProtos[i] = proto;
        JSNative* c = makeCtor(vm, kTypedNames[i], typedCtor, 3, proto);
        c->proto = baseCtor;
        c->data = Value::i32(i);
        vm.typedCtors[i] = V(c);
        vm.def(c, "BYTES_PER_ELEMENT", Value::i32(kTaSize[i]), 0);
        vm.def(proto, "BYTES_PER_ELEMENT", Value::i32(kTaSize[i]), 0);
    }

    JSObject* bytesCtor = vm.typedCtors[(int)TA::U8].asObject();
    JSObject* bytesProto = vm.typedProtos[(int)TA::U8];
    method(bytesCtor, "fromBase64", bytesFromText, 1, 0, 0);
    method(bytesCtor, "fromHex", bytesFromText, 1, 1, 0);
    method(bytesProto, "setFromBase64", bytesFromText, 1, 0, 1);
    method(bytesProto, "setFromHex", bytesFromText, 1, 1, 1);
    method(bytesProto, "toBase64", bytesToBase64, 0);
    method(bytesProto, "toHex", bytesToHex, 0);

    // DataView
    vm.dataViewProto = vm.newObject();
    makeCtor(vm, "DataView", dataViewCtor, 1, vm.dataViewProto);
    named(vm.dataViewProto, "buffer", "get buffer", dataViewInfo, 0);
    named(vm.dataViewProto, "byteLength", "get byteLength", dataViewInfo, 1);
    named(vm.dataViewProto, "byteOffset", "get byteOffset", dataViewInfo, 2);
    struct {
        const char* name;
        TA type;
    } kinds[] = {{"Int8", TA::I8},       {"Uint8", TA::U8},     {"Int16", TA::I16},    {"Uint16", TA::U16},     {"Int32", TA::I32},     {"Uint32", TA::U32},
                 {"Float16", TA::F16}, {"Float32", TA::F32}, {"Float64", TA::F64}, {"BigInt64", TA::I64}, {"BigUint64", TA::U64}};
    for (auto& k : kinds)
        for (int set = 0; set < 2; ++set) {
            std::string name = std::string(set ? "set" : "get") + k.name;
            method(vm.dataViewProto, name.c_str(), dataViewAccess, set ? 2 : 1, (int)k.type, set);
        }
    vm.defineOwn(vm.dataViewProto, PropKey::of(vm.symToStringTag), vm.strv("DataView"), PF_C);

    // Atomics
    JSObject* atomics = vm.newObject();
    vm.def(vm.global, "Atomics", V(atomics));
    const char* ops[] = {"add", "and", "exchange", "or", "sub", "xor"};
    for (int i = 0; i < 6; ++i) method(atomics, ops[i], atomicsOp, 3, i);
    method(atomics, "compareExchange", atomicsCompareExchange, 4);
    method(atomics, "load", atomicsLoad, 2);
    method(atomics, "store", atomicsStore, 3);
    method(atomics, "isLockFree", atomicsIsLockFree, 1);
    method(atomics, "notify", atomicsNotify, 3);
    method(atomics, "wait", atomicsWait, 4);
    method(atomics, "waitAsync", atomicsWaitAsync, 4);
    method(atomics, "pause", atomicsPause, 0);
    vm.defineOwn(atomics, PropKey::of(vm.symToStringTag), vm.strv("Atomics"), PF_C);

    Value math = vm.get(V(vm.global), "Math");
    if (math.isObject()) vm.defFn(math.asObject(), "f16round", mathF16round, 1);

    // TextEncoder / TextDecoder (UTF-8)
    JSObject* encProto = vm.newObject();
    makeCtor(vm, "TextEncoder", textEncoderCtor, 0, encProto)->data2 = V(encProto);
    vm.defFn(encProto, "encode", textEncode, 1);
    JSObject* decProto = vm.newObject();
    makeCtor(vm, "TextDecoder", textEncoderCtor, 0, decProto)->data2 = V(decProto);
    vm.defFn(decProto, "decode", textDecode, 1);

    JSObject* crypto = vm.newObject();
    vm.def(vm.global, "crypto", V(crypto));
    vm.defFn(crypto, "getRandomValues", cryptoGetRandomValues, 1);
    vm.defFn(crypto, "randomUUID", cryptoRandomUUID, 0);
}

}  // namespace tb::js
