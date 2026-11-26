// Array.prototype and the Array constructor's functions, written to the algorithms of ECMA-262 23.1: they work
// on any object with a length, through its properties. Plain arrays, where nobody can observe the difference,
// take direct routes over the element store.
#include <algorithm>
#include <cmath>
#include <functional>

#include "builtins.h"

namespace tb::js {

Value objectProtoToString(CallInfo& ci);

namespace {

const double kMaxLength = 9007199254740991.0;  // 2^53 - 1

// A plain array: every element lives in the dense store with default attributes, and no prototype holds
// index-named properties. Reading, writing and deleting its elements directly is then exactly what the
// property-by-property algorithms would do.
bool plain(Vm& vm, JSObject* o) {
    return o->cls == Cls::Array && !(o->oflags & (OF_SLOWIDX | OF_FROZEN | OF_SEALED | OF_NOEXT | OF_LENRO)) && o->proto == vm.arrayProto && !vm.protoIndexProps;
}

double toLength(Vm& vm, Value v) {
    double d = vm.toInteger(v);
    if (!(d > 0)) return 0;
    return d > kMaxLength ? kMaxLength : d;
}

// `this` as an object, with LengthOfArrayLike and the property operations the algorithms are written in.
struct Arr {
    Vm& vm;
    JSObject* o;
    Value v;
    double len = 0;
    Arr(Vm& vm_, Value thisv, bool withLength = true) : vm(vm_) {
        o = vm.toObject(thisv);
        v = V(o);
        if (withLength) len = o->cls == Cls::Array ? (double)arrayLen((JSArray*)o) : toLength(vm, vm.getObj(o, PropKey::of(vm.a.length), v));
    }
    bool isPlain() const { return plain(vm, o); }
    JSArray* arr() const { return (JSArray*)o; }
    PropKey key(double i) const { return i < 4294967295.0 ? PropKey::index((uint32_t)i) : vm.toKey(Value::num(i)); }
    bool has(double i) {
        if (isPlain()) return i < (double)arr()->elems.size() && !arr()->elems[(size_t)i].isEmpty();
        return vm.has(o, key(i));
    }
    Value get(double i) {
        if (isPlain()) {
            if (i < (double)arr()->elems.size() && !arr()->elems[(size_t)i].isEmpty()) return arr()->elems[(size_t)i];
            return Value::undef();
        }
        return vm.getObj(o, key(i), v);
    }
    void set(double i, Value x) {  // Set(O, P, V, true)
        if (isPlain() && i < (double)arr()->elems.size()) {
            arr()->elems[(size_t)i] = x;
            return;
        }
        PropKey k = key(i);
        if (!vm.setObj(o, k, x, v)) vm.failedSet(v, k);
    }
    void del(double i) {  // DeletePropertyOrThrow
        if (isPlain()) {
            if (i < (double)arr()->elems.size()) arr()->elems[(size_t)i] = Value::empty();
            return;
        }
        PropKey k = key(i);
        if (!vm.del(o, k)) vm.throwError(Err::TypeError, "Cannot delete property '%s' of %s", utf16to8(vm.toStr16(vm.keyToValue(k))).c_str(), utf16to8(vm.inspect(v, 0)).c_str());
    }
    void setLength(double n) {
        PropKey k = PropKey::of(vm.a.length);
        if (!vm.setObj(o, k, Value::num(n), v)) vm.failedSet(v, k);
    }
};

void needCallable(Vm& vm, Value f) {
    if (!vm.isCallable(f)) vm.throwError(Err::TypeError, "%s is not a function", utf16to8(vm.inspect(f, 0)).c_str());
}

// CreateDataPropertyOrThrow
void createData(Vm& vm, JSObject* a, double i, Value x) {
    if (plain(vm, a) && i == (double)((JSArray*)a)->elems.size()) {
        ((JSArray*)a)->elems.push_back(x);
        return;
    }
    PropKey k = i < 4294967295.0 ? PropKey::index((uint32_t)i) : vm.toKey(Value::num(i));
    if (!vm.defineElement(a, k, x, PF_ALL)) vm.throwError(Err::TypeError, "Cannot add property %s, object is not extensible", utf16to8(vm.toStr16(vm.keyToValue(k))).c_str());
}
void setLengthOf(Vm& vm, JSObject* a, double n) {
    PropKey k = PropKey::of(vm.a.length);
    if (a->cls == Cls::Array && plain(vm, a) && n == (double)((JSArray*)a)->elems.size()) return;
    if (!vm.setObj(a, k, Value::num(n), V(a))) vm.failedSet(V(a), k);
}

JSArray* arrayCreate(Vm& vm, double length) {
    if (length > 4294967295.0) vm.throwError(Err::RangeError, "Invalid array length");
    JSArray* a = vm.newArray();
    if (length > 50000000.0) {
        a->oflags |= OF_SLOWIDX;
        a->slowLen = (uint32_t)length;
    } else if (length > 0)
        a->elems.assign((size_t)length, Value::empty());
    return a;
}

// ArraySpeciesCreate (23.1.3.1.1 in older editions, now 10.4.2.4's companion)
JSObject* speciesCreate(Vm& vm, JSObject* original, double length) {
    if (!vm.isArray(V(original))) return arrayCreate(vm, length);
    Value c = vm.getObj(original, PropKey::of(vm.a.constructor), V(original));
    if (c.isObject()) {
        if (c.asObject() == vm.arrayCtorValue.asObject()) {
            // the built-in Array with its built-in species: the common case
            Prop* sp = c.asObject()->find(PropKey::of(vm.symSpecies));
            if (sp && (sp->flags & PF_ACC) && ((Accessor*)sp->val.asCell())->get.isObject() && ((Accessor*)sp->val.asCell())->get.asObject() == vm.arraySpeciesGetter.asObject())
                return arrayCreate(vm, length);
        }
        c = vm.getObj(c.asObject(), PropKey::of(vm.symSpecies), c);
        if (c.isNull()) c = Value::undef();
    }
    if (c.isUndef()) return arrayCreate(vm, length);
    if (!vm.isConstructor(c)) vm.throwError(Err::TypeError, "object.constructor[Symbol.species] is not a constructor");
    Value n = Value::num(length);
    Value made = vm.construct(c, &n, 1, c);
    if (!made.isObject()) vm.throwError(Err::TypeError, "The species constructor did not return an object");
    return made.asObject();
}

// start/end arguments: a relative index clamped into [0, len]
double relative(Vm& vm, Value arg, double len, double missing) {
    if (arg.isUndef()) return missing;
    double r = vm.toInteger(arg);
    if (r < 0) return std::max(len + r, 0.0);
    return std::min(r, len);
}

// ---------------------------------------------------------------------------- stack-like methods
JSFN(push) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    if (a.isPlain()) {
        JSArray* arr = a.arr();
        if ((double)arr->elems.size() + ci.argc <= 4294967295.0) {
            arr->elems.insert(arr->elems.end(), ci.args, ci.args + ci.argc);
            return Value::num((double)arr->elems.size());
        }
    }
    double len = a.len;
    if (len + ci.argc > kMaxLength) vm.throwError(Err::TypeError, "Pushing %d elements on an array-like of length %.0f is disallowed by its maximum length", ci.argc, len);
    for (int i = 0; i < ci.argc; ++i) {
        PropKey k = a.key(len);
        if (!vm.setObj(a.o, k, ci.args[i], a.v)) vm.failedSet(a.v, k);
        len += 1;
    }
    a.setLength(len);
    return Value::num(len);
}
JSFN(pop) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    if (a.isPlain()) {
        JSArray* arr = a.arr();
        if (arr->elems.empty()) return Value::undef();
        Value x = arr->elems.back();
        arr->elems.pop_back();
        return x.isEmpty() ? Value::undef() : x;
    }
    if (a.len == 0) {
        a.setLength(0);
        return Value::undef();
    }
    double last = a.len - 1;
    Value x = vm.getObj(a.o, a.key(last), a.v);
    a.del(last);
    a.setLength(last);
    return x;
}
JSFN(shift) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    if (a.isPlain()) {
        JSArray* arr = a.arr();
        if (arr->elems.empty()) return Value::undef();
        Value x = arr->elems.front();
        arr->elems.erase(arr->elems.begin());
        return x.isEmpty() ? Value::undef() : x;
    }
    if (a.len == 0) {
        a.setLength(0);
        return Value::undef();
    }
    Value first = a.get(0);
    for (double k = 1; k < a.len; ++k) {
        if (a.has(k)) a.set(k - 1, a.get(k));
        else a.del(k - 1);
    }
    a.del(a.len - 1);
    a.setLength(a.len - 1);
    return first;
}
JSFN(unshift) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    if (a.isPlain() && (double)a.arr()->elems.size() + ci.argc <= 4294967295.0) {
        JSArray* arr = a.arr();
        arr->elems.insert(arr->elems.begin(), ci.args, ci.args + ci.argc);
        return Value::num((double)arr->elems.size());
    }
    double len = a.len;
    if (ci.argc > 0) {
        if (len + ci.argc > kMaxLength) vm.throwError(Err::TypeError, "Unshifting %d elements on an array-like of length %.0f is disallowed by its maximum length", ci.argc, len);
        for (double k = len; k > 0; --k) {
            double from = k - 1, to = k + ci.argc - 1;
            if (a.has(from)) a.set(to, a.get(from));
            else a.del(to);
        }
        for (int j = 0; j < ci.argc; ++j) a.set(j, ci.args[j]);
    }
    a.setLength(len + ci.argc);
    return Value::num(len + ci.argc);
}

// ---------------------------------------------------------------------------- slicing and splicing
JSFN(slice) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    double k = relative(vm, ci.arg(0), a.len, 0), fin = relative(vm, ci.arg(1), a.len, a.len);
    double count = std::max(fin - k, 0.0);
    JSObject* out = speciesCreate(vm, a.o, count);
    if (a.isPlain() && plain(vm, out) && out != a.o) {
        JSArray* src = a.arr();
        JSArray* dst = (JSArray*)out;
        size_t s = (size_t)std::min(k, (double)src->elems.size()), e = (size_t)std::min(fin, (double)src->elems.size());
        dst->elems.clear();
        if (s < e) dst->elems.assign(src->elems.begin() + s, src->elems.begin() + e);
        dst->elems.resize((size_t)count, Value::empty());
        return V(out);
    }
    double n = 0;
    for (; k < fin; ++k, ++n)
        if (a.has(k)) createData(vm, out, n, a.get(k));
    setLengthOf(vm, out, n);
    return V(out);
}
JSFN(splice) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    double len = a.len;
    double start = relative(vm, ci.arg(0), len, 0);
    int itemCount = ci.argc > 2 ? ci.argc - 2 : 0;
    double delCount = 0;
    if (ci.argc == 1) delCount = len - start;
    else if (ci.argc > 1) delCount = std::min(std::max(vm.toInteger(ci.args[1]), 0.0), len - start);
    if (len + itemCount - delCount > kMaxLength) vm.throwError(Err::TypeError, "Splicing would make the array-like longer than its maximum length");
    JSObject* out = speciesCreate(vm, a.o, delCount);
    if (a.isPlain() && plain(vm, out) && out != a.o && (double)a.arr()->elems.size() == len) {
        JSArray* src = a.arr();
        JSArray* dst = (JSArray*)out;
        size_t s = (size_t)start, d = (size_t)delCount;
        dst->elems.assign(src->elems.begin() + s, src->elems.begin() + s + d);
        src->elems.erase(src->elems.begin() + s, src->elems.begin() + s + d);
        if (itemCount) src->elems.insert(src->elems.begin() + s, ci.args + 2, ci.args + ci.argc);
        return V(out);
    }
    for (double k = 0; k < delCount; ++k)
        if (a.has(start + k)) createData(vm, out, k, a.get(start + k));
    setLengthOf(vm, out, delCount);
    if (itemCount < delCount) {
        for (double k = start; k < len - delCount; ++k) {
            double from = k + delCount, to = k + itemCount;
            if (a.has(from)) a.set(to, a.get(from));
            else a.del(to);
        }
        for (double k = len; k > len - delCount + itemCount; --k) a.del(k - 1);
    } else if (itemCount > delCount) {
        for (double k = len - delCount; k > start; --k) {
            double from = k + delCount - 1, to = k + itemCount - 1;
            if (a.has(from)) a.set(to, a.get(from));
            else a.del(to);
        }
    }
    for (int j = 0; j < itemCount; ++j) {
        PropKey k = a.key(start + j);
        if (!vm.setObj(a.o, k, ci.args[2 + j], a.v)) vm.failedSet(a.v, k);
    }
    a.setLength(len - delCount + itemCount);
    return V(out);
}
JSFN(toSpliced) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    double len = a.len, start = relative(vm, ci.arg(0), len, 0);
    int insertCount = ci.argc > 2 ? ci.argc - 2 : 0;
    double skip = 0;
    if (ci.argc == 1) skip = len - start;
    else if (ci.argc > 1) skip = std::min(std::max(vm.toInteger(ci.args[1]), 0.0), len - start);
    double newLen = len + insertCount - skip;
    if (newLen > kMaxLength) vm.throwError(Err::TypeError, "The result would be longer than the maximum array length");
    JSArray* out = arrayCreate(vm, newLen);
    double i = 0, r = start + skip;
    for (; i < start; ++i) vm.defineElement(out, a.key(i), a.get(i), PF_ALL);
    for (int j = 0; j < insertCount; ++j, ++i) vm.defineElement(out, a.key(i), ci.args[2 + j], PF_ALL);
    for (; i < newLen; ++i, ++r) vm.defineElement(out, a.key(i), a.get(r), PF_ALL);
    return V(out);
}

// IsConcatSpreadable (23.1.3.2.1)
bool spreadable(Vm& vm, Value e) {
    if (!e.isObject()) return false;
    Value s = vm.getObj(e.asObject(), PropKey::of(vm.symIsConcatSpreadable), e);
    if (!s.isUndef()) return vm.toBool(s);
    return vm.isArray(e);
}
JSFN(concat) {
    Vm& vm = ci.vm;
    JSObject* o = vm.toObject(ci.thisv);
    JSObject* out = speciesCreate(vm, o, 0);
    double n = 0;
    for (int idx = -1; idx < ci.argc; ++idx) {
        Value e = idx < 0 ? V(o) : ci.args[idx];
        if (!spreadable(vm, e)) {
            if (n >= kMaxLength) vm.throwError(Err::TypeError, "The result would be longer than the maximum array length");
            createData(vm, out, n, e);
            n += 1;
            continue;
        }
        JSObject* src = e.asObject();
        if (plain(vm, src) && plain(vm, out) && out != src && n == (double)((JSArray*)out)->elems.size()) {
            JSArray* s = (JSArray*)src;
            JSArray* d = (JSArray*)out;
            d->elems.insert(d->elems.end(), s->elems.begin(), s->elems.end());
            n += (double)s->elems.size();
            continue;
        }
        Arr s(vm, e);
        if (n + s.len > kMaxLength) vm.throwError(Err::TypeError, "The result would be longer than the maximum array length");
        for (double k = 0; k < s.len; ++k, n += 1)
            if (s.has(k)) createData(vm, out, n, s.get(k));
    }
    setLengthOf(vm, out, n);
    if (out->cls == Cls::Array && n > (double)arrayLen((JSArray*)out)) {  // trailing holes
        PropKey k = PropKey::of(vm.a.length);
        vm.setObj(out, k, Value::num(n), V(out));
    }
    return V(out);
}
JSFN(copyWithin) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    double len = a.len;
    double to = relative(vm, ci.arg(0), len, 0), from = relative(vm, ci.arg(1), len, 0), fin = relative(vm, ci.arg(2), len, len);
    double count = std::min(fin - from, len - to);
    double dir = 1;
    if (from < to && to < from + count) {
        dir = -1;
        from += count - 1;
        to += count - 1;
    }
    for (; count > 0; --count, from += dir, to += dir) {
        if (a.has(from)) a.set(to, a.get(from));
        else a.del(to);
    }
    return a.v;
}
JSFN(fill) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    double k = relative(vm, ci.arg(1), a.len, 0), fin = relative(vm, ci.arg(2), a.len, a.len);
    for (; k < fin; ++k) {
        PropKey key = a.key(k);
        if (a.isPlain() && k < (double)a.arr()->elems.size()) a.arr()->elems[(size_t)k] = ci.arg(0);
        else if (!vm.setObj(a.o, key, ci.arg(0), a.v)) vm.failedSet(a.v, key);
    }
    return a.v;
}
JSFN(reverse) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    if (a.isPlain()) {
        std::reverse(a.arr()->elems.begin(), a.arr()->elems.end());
        return a.v;
    }
    double middle = std::floor(a.len / 2);
    for (double lower = 0; lower != middle; ++lower) {
        double upper = a.len - lower - 1;
        bool lowerExists = a.has(lower);
        Value lowerValue = lowerExists ? a.get(lower) : Value::undef();
        bool upperExists = a.has(upper);
        Value upperValue = upperExists ? a.get(upper) : Value::undef();
        if (lowerExists && upperExists) {
            a.set(lower, upperValue);
            a.set(upper, lowerValue);
        } else if (upperExists) {
            a.set(lower, upperValue);
            a.del(upper);
        } else if (lowerExists) {
            a.del(lower);
            a.set(upper, lowerValue);
        }
    }
    return a.v;
}
JSFN(toReversed) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    JSArray* out = arrayCreate(vm, a.len);
    for (double k = 0; k < a.len; ++k) vm.defineElement(out, a.key(k), a.get(a.len - k - 1), PF_ALL);
    return V(out);
}
JSFN(with) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    double rel = vm.toInteger(ci.arg(0));
    double actual = rel >= 0 ? rel : a.len + rel;
    if (actual >= a.len || actual < 0) vm.throwError(Err::RangeError, "Invalid index : %.0f", rel);
    JSArray* out = arrayCreate(vm, a.len);
    for (double k = 0; k < a.len; ++k) vm.defineElement(out, a.key(k), k == actual ? ci.arg(1) : a.get(k), PF_ALL);
    return V(out);
}
JSFN(at) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    double rel = vm.toInteger(ci.arg(0));
    double k = rel >= 0 ? rel : a.len + rel;
    if (k < 0 || k >= a.len) return Value::undef();
    return a.get(k);
}

// ---------------------------------------------------------------------------- searching and joining
JSFN(includes) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    if (a.len == 0) return Value::boolean(false);
    double n = vm.toInteger(ci.arg(1));
    if (n == INFINITY) return Value::boolean(false);
    double k = n >= 0 ? n : std::max(a.len + n, 0.0);
    for (; k < a.len; ++k)
        if (vm.sameValueZero(a.get(k), ci.arg(0))) return Value::boolean(true);
    return Value::boolean(false);
}
JSFN(indexOf) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    if (a.len == 0) return Value::i32(-1);
    double n = vm.toInteger(ci.arg(1));
    if (n == INFINITY) return Value::i32(-1);
    double k = n >= 0 ? n : std::max(a.len + n, 0.0);
    for (; k < a.len; ++k)
        if (a.has(k) && vm.strictEquals(a.get(k), ci.arg(0))) return Value::num(k);
    return Value::i32(-1);
}
JSFN(lastIndexOf) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    if (a.len == 0) return Value::i32(-1);
    double n = ci.argc > 1 ? vm.toInteger(ci.args[1]) : a.len - 1;
    if (n == -INFINITY) return Value::i32(-1);
    double k = n >= 0 ? std::min(n, a.len - 1) : a.len + n;
    for (; k >= 0; --k)
        if (a.has(k) && vm.strictEquals(a.get(k), ci.arg(0))) return Value::num(k);
    return Value::i32(-1);
}

// join calling itself through an element that is the array again would never end: such elements print as empty
struct JoinGuard {
    Vm& vm;
    bool cyclic = false;
    JoinGuard(Vm& v, JSObject* o) : vm(v) {
        for (JSObject* p : vm.joinStack)
            if (p == o) cyclic = true;
        vm.joinStack.push_back(o);
    }
    ~JoinGuard() { vm.joinStack.pop_back(); }
};
JSFN(join) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    std::u16string sep = ci.arg(0).isUndef() ? std::u16string(u",") : vm.toStr16(ci.arg(0));
    JoinGuard guard(vm, a.o);
    std::u16string out;
    if (guard.cyclic) return vm.strv(out);
    for (double k = 0; k < a.len; ++k) {
        if (k > 0) out += sep;
        Value e = a.get(k);
        if (!e.isNullish()) out += vm.toStr16(e);
        if (out.size() > 0x1FFFFFFF) vm.throwError(Err::RangeError, "Invalid string length");
    }
    return vm.strv(std::move(out));
}
JSFN(toString) {
    Vm& vm = ci.vm;
    JSObject* o = vm.toObject(ci.thisv);
    Value func = vm.getObj(o, PropKey::of(vm.atom("join")), V(o));
    if (!vm.isCallable(func)) {
        CallInfo inner{vm, V(o), nullptr, 0, Value::undef(), ci.callee};
        return objectProtoToString(inner);
    }
    return vm.call(func, V(o), nullptr, 0);
}
JSFN(toLocaleString) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    JoinGuard guard(vm, a.o);
    std::u16string out;
    if (guard.cyclic) return vm.strv(out);
    for (double k = 0; k < a.len; ++k) {
        if (k > 0) out += u',';
        Value e = a.get(k);
        if (e.isNullish()) continue;
        Value fn = vm.get(e, PropKey::of(vm.atom("toLocaleString")));
        out += vm.toStr16(vm.call(fn, e, nullptr, 0));
    }
    return vm.strv(std::move(out));
}

// ---------------------------------------------------------------------------- callbacks
enum { EachForEach, EachMap, EachFilter, EachSome, EachEvery };
Value each(CallInfo& ci, int mode) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    Value fn = ci.arg(0), thisArg = ci.arg(1);
    needCallable(vm, fn);
    JSObject* out = mode == EachMap ? speciesCreate(vm, a.o, a.len) : mode == EachFilter ? speciesCreate(vm, a.o, 0) : nullptr;
    Value keep = out ? V(out) : Value::undef();
    (void)keep;
    double to = 0;
    for (double k = 0; k < a.len; ++k) {
        if (!a.has(k)) continue;
        Value x = a.get(k);
        Value args[3] = {x, Value::num(k), a.v};
        Value r = vm.call(fn, thisArg, args, 3);
        switch (mode) {
            case EachMap: {
                JSArray* o = (JSArray*)out;
                if (plain(vm, out) && k < (double)o->elems.size()) o->elems[(size_t)k] = r;
                else if (!vm.defineElement(out, a.key(k), r, PF_ALL)) vm.throwError(Err::TypeError, "Cannot add property %.0f, object is not extensible", k);
                break;
            }
            case EachFilter:
                if (vm.toBool(r)) createData(vm, out, to++, x);
                break;
            case EachSome:
                if (vm.toBool(r)) return Value::boolean(true);
                break;
            case EachEvery:
                if (!vm.toBool(r)) return Value::boolean(false);
                break;
        }
    }
    if (mode == EachMap || mode == EachFilter) return V(out);
    return mode == EachSome ? Value::boolean(false) : mode == EachEvery ? Value::boolean(true) : Value::undef();
}
JSFN(forEach) { return each(ci, EachForEach); }
JSFN(map) { return each(ci, EachMap); }
JSFN(filter) { return each(ci, EachFilter); }
JSFN(some) { return each(ci, EachSome); }
JSFN(every) { return each(ci, EachEvery); }

// find, findIndex, findLast, findLastIndex: FindViaPredicate (7.3.36)
Value findVia(CallInfo& ci, bool fromEnd, bool wantIndex) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    Value fn = ci.arg(0);
    needCallable(vm, fn);
    for (double n = 0; n < a.len; ++n) {
        double k = fromEnd ? a.len - 1 - n : n;
        Value x = a.get(k);
        Value args[3] = {x, Value::num(k), a.v};
        if (vm.toBool(vm.call(fn, ci.arg(1), args, 3))) return wantIndex ? Value::num(k) : x;
    }
    return wantIndex ? Value::i32(-1) : Value::undef();
}
JSFN(find) { return findVia(ci, false, false); }
JSFN(findIndex) { return findVia(ci, false, true); }
JSFN(findLast) { return findVia(ci, true, false); }
JSFN(findLastIndex) { return findVia(ci, true, true); }

Value fold(CallInfo& ci, bool right) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    Value fn = ci.arg(0);
    needCallable(vm, fn);
    double k = right ? a.len - 1 : 0, step = right ? -1 : 1;
    auto inRange = [&] { return right ? k >= 0 : k < a.len; };
    Value acc;
    if (ci.argc > 1) acc = ci.args[1];
    else {
        bool found = false;
        for (; inRange() && !found; k += step)
            if (a.has(k)) {
                acc = a.get(k);
                found = true;
            }
        if (!found) vm.throwError(Err::TypeError, "Reduce of empty array with no initial value");
    }
    for (; inRange(); k += step) {
        if (!a.has(k)) continue;
        Value args[4] = {acc, a.get(k), Value::num(k), a.v};
        acc = vm.call(fn, Value::undef(), args, 4);
    }
    return acc;
}
JSFN(reduce) { return fold(ci, false); }
JSFN(reduceRight) { return fold(ci, true); }

struct Deep {  // nested arrays can be arbitrarily deep; the native stack is not
    Vm& vm;
    explicit Deep(Vm& v) : vm(v) {
        if (++vm.depth > 150) {
            --vm.depth;
            vm.throwError(Err::RangeError, "Maximum call stack size exceeded");
        }
    }
    ~Deep() { --vm.depth; }
};

// FlattenIntoArray (23.1.3.13.1)
double flatten(Vm& vm, JSObject* target, Value source, double sourceLen, double start, double depth, Value mapper, Value thisArg) {
    Arr s(vm, source, false);
    double targetIndex = start;
    for (double i = 0; i < sourceLen; ++i) {
        if (!s.has(i)) continue;
        Value element = s.get(i);
        if (!mapper.isUndef()) {
            Value args[3] = {element, Value::num(i), source};
            element = vm.call(mapper, thisArg, args, 3);
        }
        if (depth > 0 && vm.isArray(element)) {
            Arr inner(vm, element);
            Deep guard(vm);
            targetIndex = flatten(vm, target, element, inner.len, targetIndex, depth - 1, Value::undef(), Value::undef());
        } else {
            if (targetIndex >= kMaxLength) vm.throwError(Err::TypeError, "The result would be longer than the maximum array length");
            createData(vm, target, targetIndex, element);
            targetIndex += 1;
        }
    }
    return targetIndex;
}
JSFN(flat) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    double depth = 1;
    if (!ci.arg(0).isUndef()) {
        depth = vm.toInteger(ci.arg(0));
        if (depth < 0) depth = 0;
    }
    JSObject* out = speciesCreate(vm, a.o, 0);
    flatten(vm, out, a.v, a.len, 0, depth, Value::undef(), Value::undef());
    return V(out);
}
JSFN(flatMap) {
    Vm& vm = ci.vm;
    Arr a(vm, ci.thisv);
    needCallable(vm, ci.arg(0));
    JSObject* out = speciesCreate(vm, a.o, 0);
    flatten(vm, out, a.v, a.len, 0, 1, ci.arg(0), ci.arg(1));
    return V(out);
}

// ---------------------------------------------------------------------------- sorting
void mergeSort(std::vector<Value>& v, std::vector<Value>& tmp, size_t lo, size_t hi, const std::function<bool(Value, Value)>& less) {
    if (hi - lo < 2) return;
    if (hi - lo <= 6) {  // insertion sort for short runs
        for (size_t i = lo + 1; i < hi; ++i) {
            Value x = v[i];
            size_t j = i;
            while (j > lo && less(x, v[j - 1])) {
                v[j] = v[j - 1];
                --j;
            }
            v[j] = x;
        }
        return;
    }
    size_t mid = lo + (hi - lo) / 2;
    mergeSort(v, tmp, lo, mid, less);
    mergeSort(v, tmp, mid, hi, less);
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) tmp[k++] = less(v[j], v[i]) ? v[j++] : v[i++];
    while (i < mid) tmp[k++] = v[i++];
    while (j < hi) tmp[k++] = v[j++];
    std::copy(tmp.begin() + lo, tmp.begin() + hi, v.begin() + lo);
}

// SortIndexedProperties with SortCompare (23.1.3.30.1/2): the values come back sorted, undefined ones last.
void sortValues(Vm& vm, std::vector<Value>& vals, Value comparator) {
    std::vector<Value> defined, tmp, keys;
    size_t undefs = 0;
    for (Value x : vals) {
        if (x.isUndef()) ++undefs;
        else defined.push_back(x);
    }
    RootScope r1(vm, defined), r2(vm, tmp), r3(vm, keys);
    tmp.resize(defined.size());
    if (!comparator.isUndef()) {
        mergeSort(defined, tmp, 0, defined.size(), [&](Value x, Value y) {
            Value args[2] = {x, y};
            double c = vm.toNumber(vm.call(comparator, Value::undef(), args, 2));
            return c < 0;
        });
    } else {
        // the default order compares the elements as strings; each one is converted once
        std::vector<Value> order(defined.size());
        for (Value x : defined) keys.push_back(V(vm.toString(x)));
        for (size_t i = 0; i < order.size(); ++i) order[i] = Value::i32((int32_t)i);
        std::vector<Value> otmp(order.size());
        mergeSort(order, otmp, 0, order.size(), [&](Value x, Value y) { return keys[(size_t)x.asInt()].asString()->s < keys[(size_t)y.asInt()].asString()->s; });
        for (size_t i = 0; i < order.size(); ++i) tmp[i] = defined[(size_t)order[i].asInt()];
        defined.swap(tmp);
    }
    vals.assign(defined.begin(), defined.end());
    vals.insert(vals.end(), undefs, Value::undef());
}
JSFN(sort) {
    Vm& vm = ci.vm;
    if (!ci.arg(0).isUndef()) needCallable(vm, ci.arg(0));
    Arr a(vm, ci.thisv);
    std::vector<Value> vals;
    RootScope roots(vm, vals);
    for (double k = 0; k < a.len; ++k)
        if (a.has(k)) vals.push_back(a.get(k));
    sortValues(vm, vals, ci.arg(0));
    double j = 0;
    for (Value x : vals) {
        PropKey key = a.key(j);
        if (!vm.setObj(a.o, key, x, a.v)) vm.failedSet(a.v, key);
        j += 1;
    }
    // the holes move to the end: with fewer values than the length, the rest of the indices are removed
    if (a.isPlain()) {
        JSArray* arr = a.arr();
        for (size_t i = (size_t)j; i < arr->elems.size(); ++i) arr->elems[i] = Value::empty();
    } else
        for (; j < a.len; ++j)
            if (a.has(j)) a.del(j);
    return a.v;
}
JSFN(toSorted) {
    Vm& vm = ci.vm;
    if (!ci.arg(0).isUndef()) needCallable(vm, ci.arg(0));
    Arr a(vm, ci.thisv);
    JSArray* out = arrayCreate(vm, a.len);
    std::vector<Value> vals;
    RootScope roots(vm, vals);
    for (double k = 0; k < a.len; ++k) vals.push_back(a.get(k));  // holes read through as undefined
    sortValues(vm, vals, ci.arg(0));
    out->elems.assign(vals.begin(), vals.end());
    return V(out);
}

// ---------------------------------------------------------------------------- Array.from, Array.of, Array.isArray
JSObject* constructWith(Vm& vm, Value c, bool hasLen, double len) {
    if (!vm.isConstructor(c)) return arrayCreate(vm, hasLen ? len : 0);
    Value n = Value::num(len);
    Value made = vm.construct(c, &n, hasLen ? 1 : 0, c);
    if (!made.isObject()) vm.throwError(Err::TypeError, "The constructor did not return an object");
    return made.asObject();
}
JSFN(from) {
    Vm& vm = ci.vm;
    Value c = ci.thisv, items = ci.arg(0), mapper = ci.arg(1), thisArg = ci.arg(2);
    bool mapping = !mapper.isUndef();
    if (mapping) needCallable(vm, mapper);
    Value usingIterator = items.isNullish() ? Value::undef() : vm.get(items, PropKey::of(vm.symIterator));
    if (!usingIterator.isNullish()) {
        needCallable(vm, usingIterator);
        JSObject* a = constructWith(vm, c, false, 0);
        Value keep = V(a);
        Value iterator = vm.call(usingIterator, items, nullptr, 0);
        if (!iterator.isObject()) vm.throwError(Err::TypeError, "Result of the Symbol.iterator method is not an object");
        Value next = vm.get(iterator, vm.a.next);
        double k = 0;
        for (;;) {
            Value result = vm.call(next, iterator, nullptr, 0);
            if (!result.isObject()) vm.throwError(Err::TypeError, "Iterator result is not an object");
            if (vm.toBool(vm.get(result, vm.a.done))) break;
            Value value = vm.get(result, vm.a.value);
            try {
                if (mapping) {
                    Value args[2] = {value, Value::num(k)};
                    value = vm.call(mapper, thisArg, args, 2);
                }
                createData(vm, a, k, value);
            } catch (JSThrow&) {  // IteratorClose, keeping the error
                Value e = vm.pending;
                try {
                    Value ret = vm.get(iterator, vm.a.return_);
                    if (vm.isCallable(ret)) vm.call(ret, iterator, nullptr, 0);
                } catch (JSThrow&) {
                }
                vm.pending = Value::undef();
                vm.throwValue(e);
            }
            k += 1;
        }
        setLengthOf(vm, a, k);
        return keep;
    }
    Arr src(vm, items);
    JSObject* a = constructWith(vm, c, true, src.len);
    for (double k = 0; k < src.len; ++k) {
        Value value = src.get(k);
        if (mapping) {
            Value args[2] = {value, Value::num(k)};
            value = vm.call(mapper, thisArg, args, 2);
        }
        if (plain(vm, a) && k < (double)((JSArray*)a)->elems.size()) ((JSArray*)a)->elems[(size_t)k] = value;
        else if (!vm.defineElement(a, src.key(k), value, PF_ALL)) vm.throwError(Err::TypeError, "Cannot add property %.0f, object is not extensible", k);
    }
    PropKey lk = PropKey::of(vm.a.length);
    if (!vm.setObj(a, lk, Value::num(src.len), V(a))) vm.failedSet(V(a), lk);
    return V(a);
}
JSFN(of) {
    Vm& vm = ci.vm;
    JSObject* a = constructWith(vm, ci.thisv, true, ci.argc);
    for (int k = 0; k < ci.argc; ++k)
        if (!vm.defineElement(a, PropKey::index((uint32_t)k), ci.args[k], PF_ALL)) vm.throwError(Err::TypeError, "Cannot add property %d, object is not extensible", k);
    PropKey lk = PropKey::of(vm.a.length);
    if (!vm.setObj(a, lk, Value::num(ci.argc), V(a))) vm.failedSet(V(a), lk);
    return V(a);
}
JSFN(isArray) { return Value::boolean(ci.vm.isArray(ci.arg(0))); }
JSFN(speciesGetter) { return ci.thisv; }

}  // namespace

void setupArrayMethods(Vm& vm, JSObject* proto, JSNative* ctor) {
    vm.defFn(ctor, "isArray", isArray, 1);
    vm.defFn(ctor, "of", of, 0);
    vm.defFn(ctor, "from", from, 1);
    JSNative* species = vm.newNative(speciesGetter, "get [Symbol.species]", 0);
    vm.arraySpeciesGetter = V(species);
    vm.defineAccessor(ctor, PropKey::of(vm.symSpecies), V(species), Value::undef(), PF_C);
    struct {
        const char* name;
        NativeFn fn;
        int len;
    } fns[] = {
        {"at", at, 1},           {"concat", concat, 1},     {"copyWithin", copyWithin, 2}, {"every", every, 1},           {"fill", fill, 1},
        {"filter", filter, 1},   {"find", find, 1},         {"findIndex", findIndex, 1},   {"findLast", findLast, 1},     {"findLastIndex", findLastIndex, 1},
        {"flat", flat, 0},       {"flatMap", flatMap, 1},   {"forEach", forEach, 1},       {"includes", includes, 1},     {"indexOf", indexOf, 1},
        {"join", join, 1},       {"lastIndexOf", lastIndexOf, 1}, {"map", map, 1},         {"pop", pop, 0},               {"push", push, 1},
        {"reduce", reduce, 1},   {"reduceRight", reduceRight, 1}, {"reverse", reverse, 0}, {"shift", shift, 0},           {"slice", slice, 2},
        {"some", some, 1},       {"sort", sort, 1},         {"splice", splice, 2},         {"toLocaleString", toLocaleString, 0}, {"toReversed", toReversed, 0},
        {"toSorted", toSorted, 1}, {"toSpliced", toSpliced, 2}, {"toString", toString, 0}, {"unshift", unshift, 1},       {"with", with, 2},
    };
    for (auto& f : fns) vm.defFn(proto, f.name, f.fn, f.len);
}

}  // namespace tb::js
