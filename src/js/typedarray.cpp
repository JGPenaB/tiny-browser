// ArrayBuffer, the typed array family, DataView, TextEncoder/TextDecoder and crypto.getRandomValues.
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cmath>

#include "builtins.h"

namespace tb::js {

static const int kElemSize[] = {1, 1, 1, 2, 2, 4, 4, 4, 8};
static const char* kTypedNames[] = {"Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
                                    "Int32Array", "Uint32Array", "Float32Array", "Float64Array"};

void JSTypedArray::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(buffer);
}

Value typedGet(const JSTypedArray* t, uint32_t i) {
    const uint8_t* p = t->buffer->bytes.data() + t->offset + (size_t)i * kElemSize[(int)t->type];
    switch (t->type) {
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
            return Value::num((double)v);
        }
        default: {
            double v;
            memcpy(&v, p, 8);
            return Value::num(v);
        }
    }
}

static void storeNumber(JSTypedArray* t, uint32_t i, double d) {
    uint8_t* p = t->buffer->bytes.data() + t->offset + (size_t)i * kElemSize[(int)t->type];
    auto wrap = [d]() -> uint32_t {
        if (!std::isfinite(d)) return 0;
        double m = std::fmod(std::trunc(d), 4294967296.0);
        if (m < 0) m += 4294967296.0;
        return (uint32_t)m;
    };
    switch (t->type) {
        case TA::I8:
        case TA::U8: *p = (uint8_t)wrap(); break;
        case TA::U8C: *p = d != d || d <= 0 ? 0 : d >= 255 ? 255 : (uint8_t)std::nearbyint(d); break;
        case TA::I16:
        case TA::U16: {
            uint16_t v = (uint16_t)wrap();
            memcpy(p, &v, 2);
            break;
        }
        case TA::I32:
        case TA::U32: {
            uint32_t v = wrap();
            memcpy(p, &v, 4);
            break;
        }
        case TA::F32: {
            float v = (float)d;
            memcpy(p, &v, 4);
            break;
        }
        default: memcpy(p, &d, 8);
    }
}
void typedSet(Vm& vm, JSTypedArray* t, uint32_t i, Value v) {
    double d = v.isInt() ? v.asInt() : v.isDouble() ? v.asDouble() : vm.toNumber(v);
    if (i < t->length) storeNumber(t, i, d);
}

JSTypedArray* newTypedArray(Vm& vm, TA type, uint32_t length) {
    JSArrayBuffer* b = vm.alloc<JSArrayBuffer>();
    b->proto = vm.arrayBufferProto;
    b->bytes.assign((size_t)length * kElemSize[(int)type], 0);
    vm.gc.noteExternal(b->bytes.size());
    JSTypedArray* t = vm.alloc<JSTypedArray>();
    t->proto = vm.typedProtos[(int)type];
    t->buffer = b;
    t->length = length;
    t->type = type;
    return t;
}

namespace {

JSTypedArray* thisTyped(CallInfo& ci) {
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != Cls::TypedArray) ci.vm.throwError(Err::TypeError, "this is not a typed array");
    return (JSTypedArray*)ci.thisv.asObject();
}
double relIndex(Vm& vm, Value v, double len, double def) {
    if (v.isUndef()) return def;
    double d = vm.toInteger(v);
    if (d < 0) return std::max(0.0, d + len);
    return std::min(d, len);
}
uint32_t checkedLength(Vm& vm, double n, int elemSize) {
    if (!(n >= 0) || n * elemSize > 1073741824.0) vm.throwError(Err::RangeError, "Invalid typed array length: %g", n);
    return (uint32_t)n;
}
// Reads any array-like or iterable into numbers-to-be.
void collect(Vm& vm, Value src, std::vector<Value>& out) {
    if (src.isObject() && src.asObject()->cls == Cls::TypedArray) {
        JSTypedArray* s = (JSTypedArray*)src.asObject();
        for (uint32_t i = 0; i < s->length; ++i) out.push_back(typedGet(s, i));
    } else if (src.isObject() && vm.isCallable(vm.get(src, PropKey::of(vm.symIterator)))) {
        vm.iterate(src, [&](Value v) {
            out.push_back(v);
            return true;
        });
    } else {
        uint32_t n = vm.lengthOf(src);
        for (uint32_t i = 0; i < n; ++i) out.push_back(vm.get(src, PropKey::index(i)));
    }
}
JSTypedArray* fromValues(Vm& vm, TA type, std::vector<Value>& vals) {
    JSTypedArray* t = newTypedArray(vm, type, checkedLength(vm, (double)vals.size(), kElemSize[(int)type]));
    for (uint32_t i = 0; i < t->length; ++i) typedSet(vm, t, i, vals[i]);
    return t;
}

JSFN(typedCtor) {
    Vm& vm = ci.vm;
    TA type = (TA)ci.callee->data.asInt();
    int sz = kElemSize[(int)type];
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Constructor %s requires 'new'", kTypedNames[(int)type]);
    Value a0 = ci.arg(0);
    JSTypedArray* t = nullptr;
    if (a0.isObject() && a0.asObject()->cls == Cls::ArrayBuffer) {
        JSArrayBuffer* b = (JSArrayBuffer*)a0.asObject();
        double blen = (double)b->bytes.size(), off = vm.toInteger(ci.arg(1));
        if (off < 0 || off > blen || std::fmod(off, sz) != 0) vm.throwError(Err::RangeError, "Start offset of %s is out of bounds or not a multiple of %d", kTypedNames[(int)type], sz);
        double len;
        if (ci.arg(2).isUndef()) {
            if (std::fmod(blen - off, sz) != 0) vm.throwError(Err::RangeError, "Byte length of %s should be a multiple of %d", kTypedNames[(int)type], sz);
            len = (blen - off) / sz;
        } else {
            len = vm.toInteger(ci.arg(2));
            if (len < 0 || off + len * sz > blen) vm.throwError(Err::RangeError, "Invalid typed array length: %g", len);
        }
        t = vm.alloc<JSTypedArray>();
        t->buffer = b;
        t->offset = (uint32_t)off;
        t->length = (uint32_t)len;
        t->type = type;
    } else if (a0.isObject()) {
        std::vector<Value> vals;
        RootScope keep(vm, vals);
        collect(vm, a0, vals);
        t = fromValues(vm, type, vals);
    } else {
        t = newTypedArray(vm, type, checkedLength(vm, a0.isUndef() ? 0 : vm.toInteger(a0), sz));
    }
    t->proto = vm.protoFromNewTarget(ci.newTarget, vm.typedProtos[(int)type]);
    return V(t);
}
TA typeOfCtor(CallInfo& ci) {
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != Cls::Native) ci.vm.throwError(Err::TypeError, "this is not a typed array constructor");
    return (TA)((JSNative*)ci.thisv.asObject())->data.asInt();
}
JSFN(typedFrom) {
    Vm& vm = ci.vm;
    TA type = typeOfCtor(ci);
    std::vector<Value> vals;
    RootScope keep(vm, vals);
    collect(vm, ci.arg(0), vals);
    if (vm.isCallable(ci.arg(1)))
        for (size_t i = 0; i < vals.size(); ++i) {
            Value args[2] = {vals[i], Value::num((double)i)};
            vals[i] = vm.call(ci.arg(1), ci.arg(2), args, 2);
        }
    return V(fromValues(vm, type, vals));
}
JSFN(typedOf) {
    std::vector<Value> vals(ci.args, ci.args + ci.argc);
    RootScope keep(ci.vm, vals);
    return V(fromValues(ci.vm, typeOfCtor(ci), vals));
}
JSFN(typedInfo) {  // data: 0 length, 1 byteLength, 2 byteOffset, 3 buffer
    JSTypedArray* t = thisTyped(ci);
    switch (ci.callee->data.asInt()) {
        case 0: return Value::num((double)t->length);
        case 1: return Value::num((double)t->length * kElemSize[(int)t->type]);
        case 2: return Value::num((double)t->offset);
        default: return V(t->buffer);
    }
}
JSFN(typedSetFn) {
    Vm& vm = ci.vm;
    JSTypedArray* t = thisTyped(ci);
    double off = vm.toInteger(ci.arg(1));
    std::vector<Value> vals;
    RootScope keep(vm, vals);
    collect(vm, ci.arg(0), vals);
    if (off < 0 || off + (double)vals.size() > (double)t->length) vm.throwError(Err::RangeError, "offset is out of bounds");
    for (size_t i = 0; i < vals.size(); ++i) typedSet(vm, t, (uint32_t)off + (uint32_t)i, vals[i]);
    return Value::undef();
}
JSFN(typedSubarray) {
    Vm& vm = ci.vm;
    JSTypedArray* t = thisTyped(ci);
    double len = t->length;
    uint32_t a = (uint32_t)relIndex(vm, ci.arg(0), len, 0), b = (uint32_t)relIndex(vm, ci.arg(1), len, len);
    JSTypedArray* r = vm.alloc<JSTypedArray>();
    r->proto = t->proto;
    r->buffer = t->buffer;
    r->type = t->type;
    r->offset = t->offset + a * kElemSize[(int)t->type];
    r->length = b > a ? b - a : 0;
    return V(r);
}
JSFN(typedSlice) {
    Vm& vm = ci.vm;
    JSTypedArray* t = thisTyped(ci);
    double len = t->length;
    uint32_t a = (uint32_t)relIndex(vm, ci.arg(0), len, 0), b = (uint32_t)relIndex(vm, ci.arg(1), len, len);
    uint32_t n = b > a ? b - a : 0;
    JSTypedArray* r = newTypedArray(vm, t->type, n);
    r->proto = t->proto;
    int sz = kElemSize[(int)t->type];
    if (n) memcpy(r->buffer->bytes.data(), t->buffer->bytes.data() + t->offset + (size_t)a * sz, (size_t)n * sz);
    return V(r);
}
JSFN(typedFill) {
    Vm& vm = ci.vm;
    JSTypedArray* t = thisTyped(ci);
    double v = vm.toNumber(ci.arg(0)), len = t->length;
    uint32_t a = (uint32_t)relIndex(vm, ci.arg(1), len, 0), b = (uint32_t)relIndex(vm, ci.arg(2), len, len);
    for (uint32_t i = a; i < b && i < t->length; ++i) storeNumber(t, i, v);
    return ci.thisv;
}
JSFN(typedMapFilter) {  // data: 0 map, 1 filter
    Vm& vm = ci.vm;
    JSTypedArray* t = thisTyped(ci);
    Value fn = ci.arg(0);
    if (!vm.isCallable(fn)) vm.throwError(Err::TypeError, "callback is not a function");
    bool filter = ci.callee->data.asInt() == 1;
    std::vector<Value> vals;
    RootScope keep(vm, vals);
    for (uint32_t i = 0; i < t->length; ++i) {
        Value v = typedGet(t, i);
        Value args[3] = {v, Value::num(i), ci.thisv};
        Value r = vm.call(fn, ci.arg(1), args, 3);
        if (!filter) vals.push_back(r);
        else if (vm.toBool(r)) vals.push_back(v);
    }
    JSTypedArray* out = fromValues(vm, t->type, vals);
    out->proto = t->proto;
    return V(out);
}
JSFN(typedReverse) {
    JSTypedArray* t = thisTyped(ci);
    for (uint32_t i = 0, j = t->length; i + 1 < j; ++i) {
        --j;
        Value a = typedGet(t, i), b = typedGet(t, j);
        storeNumber(t, i, b.number());
        storeNumber(t, j, a.number());
    }
    return ci.thisv;
}
JSFN(typedSort) {
    Vm& vm = ci.vm;
    JSTypedArray* t = thisTyped(ci);
    Value cmp = ci.arg(0);
    std::vector<double> vals(t->length);
    for (uint32_t i = 0; i < t->length; ++i) vals[i] = typedGet(t, i).number();
    if (vm.isCallable(cmp)) {
        std::stable_sort(vals.begin(), vals.end(), [&](double a, double b) {
            Value args[2] = {Value::num(a), Value::num(b)};
            return vm.toNumber(vm.call(cmp, Value::undef(), args, 2)) < 0;
        });
    } else {
        std::sort(vals.begin(), vals.end(), [](double a, double b) { return (b != b && a == a) || a < b; });
    }
    for (uint32_t i = 0; i < t->length && i < vals.size(); ++i) storeNumber(t, i, vals[i]);
    return ci.thisv;
}
JSFN(typedCopyWithin) {
    Vm& vm = ci.vm;
    JSTypedArray* t = thisTyped(ci);
    double len = t->length;
    uint32_t to = (uint32_t)relIndex(vm, ci.arg(0), len, 0), a = (uint32_t)relIndex(vm, ci.arg(1), len, 0), b = (uint32_t)relIndex(vm, ci.arg(2), len, len);
    if (b > a && to < t->length) {
        uint32_t n = std::min(b - a, t->length - to);
        int sz = kElemSize[(int)t->type];
        uint8_t* base = t->buffer->bytes.data() + t->offset;
        memmove(base + (size_t)to * sz, base + (size_t)a * sz, (size_t)n * sz);
    }
    return ci.thisv;
}

// ---- ArrayBuffer
JSArrayBuffer* thisBuffer(CallInfo& ci) {
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != Cls::ArrayBuffer) ci.vm.throwError(Err::TypeError, "this is not an ArrayBuffer");
    return (JSArrayBuffer*)ci.thisv.asObject();
}
JSFN(bufferCtor) {
    Vm& vm = ci.vm;
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Constructor ArrayBuffer requires 'new'");
    double n = vm.toInteger(ci.arg(0));
    if (n < 0 || n > 1073741824.0) vm.throwError(Err::RangeError, "Array buffer allocation failed");
    JSArrayBuffer* b = vm.alloc<JSArrayBuffer>();
    b->proto = vm.protoFromNewTarget(ci.newTarget, vm.arrayBufferProto);
    b->bytes.assign((size_t)n, 0);
    vm.gc.noteExternal(b->bytes.size());
    return V(b);
}
JSFN(bufferByteLength) { return Value::num((double)thisBuffer(ci)->bytes.size()); }
JSFN(bufferSlice) {
    Vm& vm = ci.vm;
    JSArrayBuffer* b = thisBuffer(ci);
    double len = (double)b->bytes.size();
    size_t a = (size_t)relIndex(vm, ci.arg(0), len, 0), e = (size_t)relIndex(vm, ci.arg(1), len, len);
    JSArrayBuffer* r = vm.alloc<JSArrayBuffer>();
    r->proto = vm.arrayBufferProto;
    if (e > a) r->bytes.assign(b->bytes.begin() + a, b->bytes.begin() + e);
    return V(r);
}
JSFN(bufferIsView) {
    Value v = ci.arg(0);
    return Value::boolean(v.isObject() && (v.asObject()->cls == Cls::TypedArray || v.asObject()->cls == Cls::DataView));
}

// ---- DataView (stored like a byte-typed view)
JSFN(dataViewCtor) {
    Vm& vm = ci.vm;
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Constructor DataView requires 'new'");
    Value a0 = ci.arg(0);
    if (!a0.isObject() || a0.asObject()->cls != Cls::ArrayBuffer) vm.throwError(Err::TypeError, "First argument to DataView constructor must be an ArrayBuffer");
    JSArrayBuffer* b = (JSArrayBuffer*)a0.asObject();
    double blen = (double)b->bytes.size(), off = vm.toInteger(ci.arg(1));
    if (off < 0 || off > blen) vm.throwError(Err::RangeError, "Start offset is outside the bounds of the buffer");
    double len = ci.arg(2).isUndef() ? blen - off : vm.toInteger(ci.arg(2));
    if (len < 0 || off + len > blen) vm.throwError(Err::RangeError, "Invalid DataView length");
    JSTypedArray* v = vm.alloc<JSTypedArray>();
    v->cls = Cls::DataView;
    v->proto = vm.protoFromNewTarget(ci.newTarget, vm.dataViewProto);
    v->buffer = b;
    v->offset = (uint32_t)off;
    v->length = (uint32_t)len;
    v->type = TA::U8;
    return V(v);
}
JSTypedArray* thisView(CallInfo& ci) {
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != Cls::DataView) ci.vm.throwError(Err::TypeError, "this is not a DataView");
    return (JSTypedArray*)ci.thisv.asObject();
}
JSFN(dataViewInfo) {
    JSTypedArray* v = thisView(ci);
    int w = ci.callee->data.asInt();
    return w == 0 ? V(v->buffer) : Value::num((double)(w == 1 ? v->length : v->offset));
}
JSFN(dataViewAccess) {  // data: element type, data2: 1 = setter
    Vm& vm = ci.vm;
    JSTypedArray* v = thisView(ci);
    TA type = (TA)ci.callee->data.asInt();
    bool isSet = ci.callee->data2.asInt() == 1;
    int sz = kElemSize[(int)type];
    double pos = vm.toInteger(ci.arg(0));
    double value = isSet ? vm.toNumber(ci.arg(1)) : 0;
    bool little = vm.toBool(ci.arg(isSet ? 2 : 1));
    if (pos < 0 || pos + sz > (double)v->length) vm.throwError(Err::RangeError, "Offset is outside the bounds of the DataView");
    uint8_t* p = v->buffer->bytes.data() + v->offset + (size_t)pos;
    // run the bytes through a scratch element of the right type; this machine is little-endian
    JSArrayBuffer scratchBuf;
    scratchBuf.bytes.assign(8, 0);
    JSTypedArray scratch;
    scratch.buffer = &scratchBuf;
    scratch.length = 1;
    scratch.type = type;
    uint8_t* s = scratchBuf.bytes.data();
    if (isSet) {
        storeNumber(&scratch, 0, value);
        for (int i = 0; i < sz; ++i) p[i] = s[little ? i : sz - 1 - i];
        return Value::undef();
    }
    for (int i = 0; i < sz; ++i) s[little ? i : sz - 1 - i] = p[i];
    return typedGet(&scratch, 0);
}

// ---- bytes in, bytes out
bool bytesOf(Value v, const uint8_t*& p, size_t& n) {
    if (!v.isObject()) return false;
    JSObject* o = v.asObject();
    if (o->cls == Cls::ArrayBuffer) {
        auto& b = ((JSArrayBuffer*)o)->bytes;
        p = b.data();
        n = b.size();
        return true;
    }
    if (o->cls == Cls::TypedArray || o->cls == Cls::DataView) {
        JSTypedArray* t = (JSTypedArray*)o;
        p = t->buffer->bytes.data() + t->offset;
        n = (size_t)t->length * (o->cls == Cls::DataView ? 1 : kElemSize[(int)t->type]);
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
    if (!bytes.empty()) memcpy(t->buffer->bytes.data(), bytes.data(), bytes.size());
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
    if (!v.isObject() || v.asObject()->cls != Cls::TypedArray) vm.throwError(Err::TypeError, "crypto.getRandomValues: the argument is not an integer typed array");
    JSTypedArray* t = (JSTypedArray*)v.asObject();
    size_t n = (size_t)t->length * kElemSize[(int)t->type];
    if (n > 65536) vm.throwError(Err::Error, "QuotaExceededError: at most 65536 random bytes per call");
    if (n) BCryptGenRandom(nullptr, t->buffer->bytes.data() + t->offset, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
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

void setupTypedArrays(Vm& vm) {
    auto getter = [&](JSObject* o, const char* name, NativeFn fn, int data) {
        JSNative* g = vm.newNative(fn, name, 0);
        g->data = Value::i32(data);
        vm.defineAccessor(o, PropKey::of(vm.atom(name)), V(g), Value::undef(), PF_C);
    };
    // ArrayBuffer
    vm.arrayBufferProto = vm.newObject();
    JSNative* bufC = makeCtor(vm, "ArrayBuffer", bufferCtor, 1, vm.arrayBufferProto);
    vm.defFn(bufC, "isView", bufferIsView, 1);
    getter(vm.arrayBufferProto, "byteLength", bufferByteLength, 0);
    vm.defFn(vm.arrayBufferProto, "slice", bufferSlice, 2);
    vm.defineOwn(vm.arrayBufferProto, PropKey::of(vm.symToStringTag), vm.strv("ArrayBuffer"), PF_C);

    // the shared typed array prototype
    JSObject* base = vm.newObject();
    const char* info[] = {"length", "byteLength", "byteOffset", "buffer"};
    for (int i = 0; i < 4; ++i) getter(base, info[i], typedInfo, i);
    vm.defFn(base, "set", typedSetFn, 1);
    vm.defFn(base, "subarray", typedSubarray, 2);
    vm.defFn(base, "slice", typedSlice, 2);
    vm.defFn(base, "fill", typedFill, 1);
    vm.defFn(base, "map", typedMapFilter, 1)->data = Value::i32(0);
    vm.defFn(base, "filter", typedMapFilter, 1)->data = Value::i32(1);
    vm.defFn(base, "reverse", typedReverse, 0);
    vm.defFn(base, "sort", typedSort, 1);
    vm.defFn(base, "copyWithin", typedCopyWithin, 2);
    // the read-only array methods work on anything with a length and indices, so they are shared
    for (const char* n : {"indexOf", "lastIndexOf", "includes", "join", "forEach", "reduce", "reduceRight", "some", "every", "find", "findIndex",
                          "findLast", "findLastIndex", "at", "keys", "values", "entries", "toString"})
        vm.def(base, n, vm.get(V(vm.arrayProto), n));
    vm.defineOwn(base, PropKey::of(vm.symIterator), vm.get(V(vm.arrayProto), "values"), PF_W | PF_C);

    for (int i = 0; i < 9; ++i) {
        JSObject* proto = vm.newObject(base);
        vm.typedProtos[i] = proto;
        JSNative* c = makeCtor(vm, kTypedNames[i], typedCtor, 3, proto);
        c->data = Value::i32(i);
        vm.def(c, "BYTES_PER_ELEMENT", Value::i32(kElemSize[i]), 0);
        vm.def(proto, "BYTES_PER_ELEMENT", Value::i32(kElemSize[i]), 0);
        vm.defFn(c, "from", typedFrom, 1);
        vm.defFn(c, "of", typedOf, 0);
        vm.defineOwn(proto, PropKey::of(vm.symToStringTag), vm.strv(kTypedNames[i]), PF_C);
    }

    // DataView
    vm.dataViewProto = vm.newObject();
    makeCtor(vm, "DataView", dataViewCtor, 1, vm.dataViewProto);
    const char* dvInfo[] = {"buffer", "byteLength", "byteOffset"};
    for (int i = 0; i < 3; ++i) getter(vm.dataViewProto, dvInfo[i], dataViewInfo, i);
    struct {
        const char* name;
        TA type;
    } kinds[] = {{"Int8", TA::I8}, {"Uint8", TA::U8}, {"Int16", TA::I16}, {"Uint16", TA::U16},
                 {"Int32", TA::I32}, {"Uint32", TA::U32}, {"Float32", TA::F32}, {"Float64", TA::F64}};
    for (auto& k : kinds)
        for (int set = 0; set < 2; ++set) {
            std::string name = std::string(set ? "set" : "get") + k.name;
            JSNative* n = vm.defFn(vm.dataViewProto, name.c_str(), dataViewAccess, set ? 2 : 1);
            n->data = Value::i32((int)k.type);
            n->data2 = Value::i32(set);
        }
    vm.defineOwn(vm.dataViewProto, PropKey::of(vm.symToStringTag), vm.strv("DataView"), PF_C);

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
