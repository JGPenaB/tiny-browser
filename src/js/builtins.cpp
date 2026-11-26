// Standard library, part 1: Object, Function, Array, Error, Symbol, Boolean, Number, Math, Reflect, console, globals.
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "builtins.h"

namespace tb::js {

JSNative* makeCtor(Vm& vm, const char* name, NativeFn fn, int length, JSObject* proto) {
    JSNative* c = vm.newNative(fn, name, length);
    c->oflags |= OF_CTOR;
    if (proto) {
        c->add(PropKey::of(vm.a.prototype), V(proto), 0);
        vm.defineOwn(proto, PropKey::of(vm.a.constructor), V(c), PF_W | PF_C);
    }
    vm.def(vm.global, name, V(c));
    return c;
}

Value wrapPrimitive(CallInfo& ci, Value prim, Cls cls, JSObject* proto) {
    JSPrimitive* p = ci.vm.alloc<JSPrimitive>();
    p->cls = cls;
    p->proto = ci.vm.protoFromNewTarget(ci.newTarget, proto);
    p->prim = prim;
    return V(p);
}

void defSym(Vm& vm, JSObject* o, Symbol* sym, NativeFn fn, const char* name, int length) {
    vm.defineOwn(o, PropKey::of(sym), V(vm.newNative(fn, name, length)), PF_W | PF_C);
}

static JSArray* fastArray(JSObject* o) { return (o->cls == Cls::Array || o->cls == Cls::Arguments) ? (JSArray*)o : nullptr; }

static void needFn(Vm& vm, Value f) {
    if (!vm.isCallable(f)) vm.throwError(Err::TypeError, "%s is not a function", utf16to8(vm.inspect(f, 0)).c_str());
}

// ---------------------------------------------------------------------------- Object
JSFN(objectCtor) {
    Vm& vm = ci.vm;
    if (ci.isConstruct() && ci.newTarget != V(ci.callee)) return V(vm.newObject(vm.protoFromNewTarget(ci.newTarget, vm.objectProto)));
    Value v = ci.arg(0);
    if (v.isNullish()) return V(vm.newObject());
    return V(vm.toObject(v));
}

static Value objectEnumerate(CallInfo& ci, int mode) {
    Vm& vm = ci.vm;
    JSObject* o = vm.toObject(ci.arg(0));
    std::vector<PropKey> keys;
    vm.ownKeys(o, keys, true, false, true);
    JSArray* out = vm.newArray();
    for (PropKey k : keys) {
        if (mode == 0) {
            out->elems.push_back(vm.keyToValue(k));
            continue;
        }
        Value v = vm.getObj(o, k, V(o));
        if (mode == 1) {
            out->elems.push_back(v);
        } else {
            JSArray* pair = vm.newArray(2);
            pair->elems[0] = vm.keyToValue(k);
            pair->elems[1] = v;
            out->elems.push_back(V(pair));
        }
    }
    return V(out);
}
JSFN(objectKeys) { return objectEnumerate(ci, 0); }
JSFN(objectValues) { return objectEnumerate(ci, 1); }
JSFN(objectEntries) { return objectEnumerate(ci, 2); }

JSFN(objectAssign) {
    Vm& vm = ci.vm;
    JSObject* target = vm.toObject(ci.arg(0));
    for (int i = 1; i < ci.argc; ++i) {
        Value src = ci.args[i];
        if (src.isNullish()) continue;
        JSObject* so = vm.toObject(src);
        std::vector<PropKey> keys;
        vm.ownKeys(so, keys, true, true, true);
        for (PropKey k : keys) vm.setObj(target, k, vm.getObj(so, k, V(so)), V(target));
    }
    return V(target);
}

static void defineFromDescriptor(Vm& vm, JSObject* o, PropKey k, Value desc) {
    if (!desc.isObject()) vm.throwError(Err::TypeError, "Property description must be an object");
    JSObject* d = desc.asObject();
    auto field = [&](JSString* name, Value& out) {
        PropKey fk = PropKey::of(name);
        if (!vm.has(d, fk)) return false;
        out = vm.getObj(d, fk, desc);
        return true;
    };
    Value getter, setter, value, tmp;
    bool hasGet = field(vm.a.get, getter), hasSet = field(vm.a.set, setter), hasValue = field(vm.a.value, value);
    Prop cur{};
    bool exists = vm.getOwn(o, k, cur);
    uint8_t flags = exists ? (uint8_t)(cur.flags & PF_ALL) : 0;
    if (field(vm.a.enumerable, tmp)) flags = vm.toBool(tmp) ? (flags | PF_E) : (flags & ~PF_E);
    if (field(vm.a.configurable, tmp)) flags = vm.toBool(tmp) ? (flags | PF_C) : (flags & ~PF_C);
    if (field(vm.a.writable, tmp)) flags = vm.toBool(tmp) ? (flags | PF_W) : (flags & ~PF_W);
    if (exists && !(cur.flags & PF_C)) {
        bool wasAcc = (cur.flags & PF_ACC) != 0;
        bool bad = (hasGet || hasSet) || wasAcc || ((flags ^ cur.flags) & (PF_E | PF_C)) || (!(cur.flags & PF_W) && (flags & PF_W)) ||
                   (hasValue && !(cur.flags & PF_W) && !vm.sameValue(value, cur.val));
        if (bad) vm.throwError(Err::TypeError, u"Cannot redefine property: " + vm.toStr16(vm.keyToValue(k)));
    }
    if (hasGet || hasSet) {
        if ((hasGet && !getter.isUndef() && !vm.isCallable(getter)) || (hasSet && !setter.isUndef() && !vm.isCallable(setter)))
            vm.throwError(Err::TypeError, "Getter or setter must be a function");
        vm.defineAccessor(o, k, hasGet ? getter : Value::empty(), hasSet ? setter : Value::empty(), flags);
        return;
    }
    Value v = hasValue ? value : (exists && !(cur.flags & PF_ACC) ? cur.val : Value::undef());
    if (exists && (cur.flags & PF_ACC)) o->remove(k);
    if (!vm.defineOwn(o, k, v, flags)) vm.throwError(Err::TypeError, "Cannot define property, object is not extensible");
}

JSFN(objectDefineProperty) {
    Vm& vm = ci.vm;
    if (!ci.arg(0).isObject()) vm.throwError(Err::TypeError, "Object.defineProperty called on non-object");
    defineFromDescriptor(vm, ci.arg(0).asObject(), vm.toKey(ci.arg(1)), ci.arg(2));
    return ci.arg(0);
}
static void defineProps(Vm& vm, JSObject* o, Value props) {
    JSObject* p = vm.toObject(props);
    std::vector<PropKey> keys;
    vm.ownKeys(p, keys, true, true, true);
    for (PropKey k : keys) defineFromDescriptor(vm, o, k, vm.getObj(p, k, V(p)));
}
JSFN(objectDefineProperties) {
    Vm& vm = ci.vm;
    if (!ci.arg(0).isObject()) vm.throwError(Err::TypeError, "Object.defineProperties called on non-object");
    defineProps(vm, ci.arg(0).asObject(), ci.arg(1));
    return ci.arg(0);
}
JSFN(objectCreate) {
    Vm& vm = ci.vm;
    Value proto = ci.arg(0);
    if (!proto.isObject() && !proto.isNull()) vm.throwError(Err::TypeError, "Object prototype may only be an Object or null");
    JSObject* o = vm.newObject(proto.isObject() ? proto.asObject() : nullptr);
    if (!ci.arg(1).isUndef()) defineProps(vm, o, ci.arg(1));
    return V(o);
}
JSFN(objectGetPrototypeOf) {
    Vm& vm = ci.vm;
    if (ci.arg(0).isNullish()) vm.throwError(Err::TypeError, "Cannot convert undefined or null to object");
    JSObject* p = vm.protoOf(ci.arg(0));
    return p ? V(p) : Value::null();
}
static void setProto(Vm& vm, Value o, Value p) {
    if (!p.isObject() && !p.isNull()) vm.throwError(Err::TypeError, "Object prototype may only be an Object or null");
    if (!o.isObject()) return;
    for (JSObject* q = p.isObject() ? p.asObject() : nullptr; q; q = q->proto)
        if (q == o.asObject()) vm.throwError(Err::TypeError, "Cyclic __proto__ value");
    o.asObject()->proto = p.isObject() ? p.asObject() : nullptr;
}
JSFN(objectSetPrototypeOf) {
    setProto(ci.vm, ci.arg(0), ci.arg(1));
    return ci.arg(0);
}
static Value keyList(Vm& vm, Value target, bool strings, bool symbols) {
    JSObject* o = vm.toObject(target);
    std::vector<PropKey> keys;
    vm.ownKeys(o, keys, strings, symbols, false);
    JSArray* out = vm.newArray();
    for (PropKey k : keys) out->elems.push_back(vm.keyToValue(k));
    return V(out);
}
JSFN(objectGetOwnPropertyNames) { return keyList(ci.vm, ci.arg(0), true, false); }
JSFN(objectGetOwnPropertySymbols) { return keyList(ci.vm, ci.arg(0), false, true); }

static Value descriptorOf(Vm& vm, JSObject* o, PropKey k) {
    Prop p{};
    if (!vm.getOwn(o, k, p)) return Value::undef();
    JSObject* d = vm.newObject();
    if (p.flags & PF_ACC) {
        Accessor* acc = (Accessor*)p.val.asCell();
        d->add(PropKey::of(vm.a.get), acc->get, PF_ALL);
        d->add(PropKey::of(vm.a.set), acc->set, PF_ALL);
    } else {
        d->add(PropKey::of(vm.a.value), p.val, PF_ALL);
        d->add(PropKey::of(vm.a.writable), Value::boolean((p.flags & PF_W) != 0), PF_ALL);
    }
    d->add(PropKey::of(vm.a.enumerable), Value::boolean((p.flags & PF_E) != 0), PF_ALL);
    d->add(PropKey::of(vm.a.configurable), Value::boolean((p.flags & PF_C) != 0), PF_ALL);
    return V(d);
}
JSFN(objectGetOwnPropertyDescriptor) { return descriptorOf(ci.vm, ci.vm.toObject(ci.arg(0)), ci.vm.toKey(ci.arg(1))); }
JSFN(objectGetOwnPropertyDescriptors) {
    Vm& vm = ci.vm;
    JSObject* o = vm.toObject(ci.arg(0));
    std::vector<PropKey> keys;
    vm.ownKeys(o, keys, true, true, false);
    JSObject* out = vm.newObject();
    for (PropKey k : keys) vm.defineOwn(out, k, descriptorOf(vm, o, k), PF_ALL);
    return V(out);
}

static Value restrict(CallInfo& ci, int level) {  // 0 preventExtensions, 1 seal, 2 freeze
    Value v = ci.arg(0);
    if (!v.isObject()) return v;
    JSObject* o = v.asObject();
    o->oflags |= OF_NOEXT;
    if (level == 2) o->oflags |= OF_FROZEN;
    for (auto& p : o->props) {
        if (level >= 1) p.flags &= ~PF_C;
        if (level == 2 && !(p.flags & PF_ACC)) p.flags &= ~PF_W;
    }
    return v;
}
JSFN(objectPreventExtensions) { return restrict(ci, 0); }
JSFN(objectSeal) { return restrict(ci, 1); }
JSFN(objectFreeze) { return restrict(ci, 2); }
JSFN(objectIsExtensible) { return Value::boolean(ci.arg(0).isObject() && !(ci.arg(0).asObject()->oflags & OF_NOEXT)); }
JSFN(objectIsFrozen) { return Value::boolean(!ci.arg(0).isObject() || (ci.arg(0).asObject()->oflags & OF_FROZEN)); }
JSFN(objectIsSealed) {
    Value v = ci.arg(0);
    if (!v.isObject()) return Value::boolean(true);
    JSObject* o = v.asObject();
    if (!(o->oflags & OF_NOEXT)) return Value::boolean(false);
    for (auto& p : o->props)
        if (p.flags & PF_C) return Value::boolean(false);
    return Value::boolean(true);
}
JSFN(objectFromEntries) {
    Vm& vm = ci.vm;
    JSObject* o = vm.newObject();
    vm.iterate(ci.arg(0), [&](Value e) {
        vm.defineOwn(o, vm.toKey(vm.get(e, PropKey::index(0))), vm.get(e, PropKey::index(1)), PF_ALL);
        return true;
    });
    return V(o);
}
JSFN(objectIs) { return Value::boolean(ci.vm.sameValue(ci.arg(0), ci.arg(1))); }
JSFN(objectHasOwn) { return Value::boolean(ci.vm.hasOwn(ci.vm.toObject(ci.arg(0)), ci.vm.toKey(ci.arg(1)))); }

JSFN(objProtoHasOwnProperty) {
    PropKey k = ci.vm.toKey(ci.arg(0));
    return Value::boolean(ci.vm.hasOwn(ci.vm.toObject(ci.thisv), k));
}
JSFN(objProtoIsPrototypeOf) {
    Value v = ci.arg(0);
    if (!v.isObject() || !ci.thisv.isObject()) return Value::boolean(false);
    for (JSObject* p = v.asObject()->proto; p; p = p->proto)
        if (p == ci.thisv.asObject()) return Value::boolean(true);
    return Value::boolean(false);
}
JSFN(objProtoPropertyIsEnumerable) {
    Prop p{};
    PropKey k = ci.vm.toKey(ci.arg(0));
    return Value::boolean(ci.vm.getOwn(ci.vm.toObject(ci.thisv), k, p) && (p.flags & PF_E));
}
Value objectProtoToString(CallInfo& ci) {
    Vm& vm = ci.vm;
    if (ci.thisv.isUndef()) return vm.strv("[object Undefined]");
    if (ci.thisv.isNull()) return vm.strv("[object Null]");
    JSObject* o = vm.toObject(ci.thisv);
    Value tag = vm.getObj(o, PropKey::of(vm.symToStringTag), ci.thisv);
    if (tag.isString()) return vm.strv(u"[object " + tag.asString()->s + u"]");
    const char* n = "Object";
    switch (o->cls) {
        case Cls::Array: n = "Array"; break;
        case Cls::Arguments: n = "Arguments"; break;
        case Cls::Function:
        case Cls::Native:
        case Cls::Bound: n = "Function"; break;
        case Cls::Error: n = "Error"; break;
        case Cls::Boolean: n = "Boolean"; break;
        case Cls::Number: n = "Number"; break;
        case Cls::String: n = "String"; break;
        case Cls::Date: n = "Date"; break;
        case Cls::RegExp: n = "RegExp"; break;
        default: break;
    }
    return vm.strv(std::string("[object ") + n + "]");
}
JSFN(objProtoToLocaleString) { return ci.vm.invoke(ci.thisv, "toString"); }
JSFN(objProtoValueOf) { return V(ci.vm.toObject(ci.thisv)); }
JSFN(objProtoGetProto) {
    if (ci.thisv.isNullish()) ci.vm.throwError(Err::TypeError, "Cannot convert undefined or null to object");
    JSObject* p = ci.vm.protoOf(ci.thisv);
    return p ? V(p) : Value::null();
}
JSFN(objProtoSetProto) {
    if (ci.arg(0).isObject() || ci.arg(0).isNull()) setProto(ci.vm, ci.thisv, ci.arg(0));
    return Value::undef();
}

// ---------------------------------------------------------------------------- Function
JSFN(functionCtor) {
    Vm& vm = ci.vm;
    std::u16string params, body;
    for (int i = 0; i + 1 < ci.argc; ++i) {
        if (i) params += u",";
        params += vm.toStr16(ci.args[i]);
    }
    if (ci.argc) body = vm.toStr16(ci.args[ci.argc - 1]);
    return vm.compileFunction(params, body, "<Function>");
}
JSFN(fnCall) { return ci.vm.call(ci.thisv, ci.arg(0), ci.argc > 1 ? ci.args + 1 : nullptr, ci.argc > 1 ? ci.argc - 1 : 0); }

static void spreadArgs(Vm& vm, Value list, std::vector<Value>& out) {
    if (list.isNullish()) return;
    if (!list.isObject()) vm.throwError(Err::TypeError, "CreateListFromArrayLike called on non-object");
    if (JSArray* arr = fastArray(list.asObject())) {
        out.reserve(arr->elems.size());
        for (Value v : arr->elems) out.push_back(v.isEmpty() ? Value::undef() : v);
        return;
    }
    uint32_t n = vm.lengthOf(list);
    for (uint32_t i = 0; i < n; ++i) out.push_back(vm.get(list, PropKey::index(i)));
}
JSFN(fnApply) {
    std::vector<Value> args;
    RootScope rs(ci.vm, args);
    spreadArgs(ci.vm, ci.arg(1), args);
    return ci.vm.call(ci.thisv, ci.arg(0), args.data(), (int)args.size());
}
JSFN(fnBind) {
    Vm& vm = ci.vm;
    if (!vm.isCallable(ci.thisv)) vm.throwError(Err::TypeError, "Bind must be called on a function");
    JSBound* b = vm.alloc<JSBound>();
    b->proto = vm.functionProto;
    b->target = ci.thisv;
    b->boundThis = ci.arg(0);
    for (int i = 1; i < ci.argc; ++i) b->args.push_back(ci.args[i]);
    std::u16string name = u"bound ";
    Value n = vm.get(ci.thisv, vm.a.name);
    if (n.isString()) name += n.asString()->s;
    b->add(PropKey::of(vm.a.name), vm.strv(name), PF_C);
    double len = vm.toNumber(vm.get(ci.thisv, vm.a.length)) - (ci.argc > 1 ? ci.argc - 1 : 0);
    b->add(PropKey::of(vm.a.length), Value::num(len > 0 ? len : 0), PF_C);
    return V(b);
}
JSFN(fnToString) {
    Vm& vm = ci.vm;
    Value t = ci.thisv;
    if (t.isObject() && t.asObject()->cls == Cls::Function) {
        FuncProto* p = ((JSFunction*)t.asObject())->code;
        if (p->source && p->srcEnd > p->srcStart && p->srcEnd <= p->source->size())
            return vm.strv(p->source->substr(p->srcStart, p->srcEnd - p->srcStart));
    }
    if (!vm.isCallable(t)) vm.throwError(Err::TypeError, "Function.prototype.toString requires that 'this' be a Function");
    Value n = vm.get(t, vm.a.name);
    return vm.strv(u"function " + (n.isString() ? n.asString()->s : std::u16string()) + u"() { [native code] }");
}
JSFN(fnHasInstance) {
    Vm& vm = ci.vm;
    Value ctor = ci.thisv, v = ci.arg(0);
    if (!vm.isCallable(ctor) || !v.isObject()) return Value::boolean(false);
    while (ctor.asObject()->cls == Cls::Bound) ctor = ((JSBound*)ctor.asObject())->target;
    Value pv = vm.get(ctor, vm.a.prototype);
    if (!pv.isObject()) return Value::boolean(false);
    for (JSObject* p = v.asObject()->proto; p; p = p->proto)
        if (p == pv.asObject()) return Value::boolean(true);
    return Value::boolean(false);
}

// ---------------------------------------------------------------------------- Array
// Uniform access to "this" for array methods: real arrays take the fast path, anything else goes through properties.
struct AL {
    Vm& vm;
    JSObject* o;
    Value v;
    JSArray* arr;
    AL(Vm& vm_, Value thisv) : vm(vm_) {
        o = vm.toObject(thisv);
        v = V(o);
        arr = fastArray(o);
    }
    uint32_t len() { return arr ? (uint32_t)arr->elems.size() : vm.lengthOf(v); }
    Value get(uint32_t i) {
        if (arr && i < arr->elems.size() && !arr->elems[i].isEmpty()) return arr->elems[i];
        return vm.getObj(o, PropKey::index(i), v);
    }
    bool has(uint32_t i) {
        if (arr) return i < arr->elems.size() && !arr->elems[i].isEmpty();
        return vm.has(o, PropKey::index(i));
    }
};

static double relIndex(Vm& vm, Value v, double len, double def) {
    if (v.isUndef()) return def;
    double d = vm.toInteger(v);
    if (d < 0) return std::max(0.0, d + len);
    return std::min(d, len);
}

JSFN(arrayCtor) {
    Vm& vm = ci.vm;
    JSArray* a = vm.newArray();
    if (ci.isConstruct()) a->proto = vm.protoFromNewTarget(ci.newTarget, vm.arrayProto);
    if (ci.argc == 1 && ci.args[0].isNumber()) {
        double n = ci.args[0].number();
        if (n < 0 || n != std::floor(n) || n > 134217728.0) vm.throwError(Err::RangeError, "Invalid array length");
        a->elems.assign((size_t)n, Value::empty());
    } else if (ci.argc)
        a->elems.assign(ci.args, ci.args + ci.argc);
    return V(a);
}
JSFN(arrayIsArray) { return Value::boolean(ci.vm.isArray(ci.arg(0))); }
JSFN(arrayOf) {
    JSArray* a = ci.vm.newArray();
    if (ci.argc) a->elems.assign(ci.args, ci.args + ci.argc);
    return V(a);
}
JSFN(arrayFrom) {
    Vm& vm = ci.vm;
    Value src = ci.arg(0), mapFn = ci.arg(1), thisArg = ci.arg(2);
    if (src.isNullish()) vm.throwError(Err::TypeError, "%s is not iterable", src.isNull() ? "null" : "undefined");
    if (!mapFn.isUndef()) needFn(vm, mapFn);
    JSArray* out = vm.newArray();
    uint32_t idx = 0;
    auto push = [&](Value v) {
        if (!mapFn.isUndef()) {
            Value args[2] = {v, Value::num(idx)};
            v = vm.call(mapFn, thisArg, args, 2);
        }
        out->elems.push_back(v);
        ++idx;
        return true;
    };
    if (src.isString() || (src.isObject() && !vm.get(src, PropKey::of(vm.symIterator)).isNullish())) {
        vm.iterate(src, push);
    } else {
        uint32_t n = vm.lengthOf(src);
        for (uint32_t i = 0; i < n; ++i) push(vm.get(src, PropKey::index(i)));
    }
    return V(out);
}

// Runs a mutating operation directly on real arrays; for other array-likes it works on a copy and writes it back.
template <class F>
static Value mutate(CallInfo& ci, F op) {
    Vm& vm = ci.vm;
    JSObject* o = vm.toObject(ci.thisv);
    if (JSArray* arr = fastArray(o)) {
        if (o->oflags & OF_FROZEN) vm.throwError(Err::TypeError, "Cannot modify a frozen array");
        return op(vm, arr);
    }
    Value ov = V(o);
    uint32_t len = vm.lengthOf(ov);
    JSArray* tmp = vm.newArray();
    for (uint32_t i = 0; i < len; ++i) tmp->elems.push_back(vm.has(o, PropKey::index(i)) ? vm.getObj(o, PropKey::index(i), ov) : Value::empty());
    Value r = op(vm, tmp);
    for (uint32_t i = 0; i < tmp->elems.size(); ++i) {
        if (tmp->elems[i].isEmpty()) vm.del(o, PropKey::index(i));
        else vm.setObj(o, PropKey::index(i), tmp->elems[i], ov);
    }
    for (uint32_t i = (uint32_t)tmp->elems.size(); i < len; ++i) vm.del(o, PropKey::index(i));
    vm.setObj(o, PropKey::of(vm.a.length), Value::num((double)tmp->elems.size()), ov);
    return r;
}

JSFN(arrayPush) {
    return mutate(ci, [&](Vm&, JSArray* a) {
        if (ci.argc) a->elems.insert(a->elems.end(), ci.args, ci.args + ci.argc);
        return Value::num((double)a->elems.size());
    });
}
JSFN(arrayPop) {
    return mutate(ci, [](Vm&, JSArray* a) {
        if (a->elems.empty()) return Value::undef();
        Value v = a->elems.back();
        a->elems.pop_back();
        return v.isEmpty() ? Value::undef() : v;
    });
}
JSFN(arrayShift) {
    return mutate(ci, [](Vm&, JSArray* a) {
        if (a->elems.empty()) return Value::undef();
        Value v = a->elems.front();
        a->elems.erase(a->elems.begin());
        return v.isEmpty() ? Value::undef() : v;
    });
}
JSFN(arrayUnshift) {
    return mutate(ci, [&](Vm&, JSArray* a) {
        if (ci.argc) a->elems.insert(a->elems.begin(), ci.args, ci.args + ci.argc);
        return Value::num((double)a->elems.size());
    });
}
JSFN(arraySplice) {
    return mutate(ci, [&](Vm& vm, JSArray* a) {
        double len = (double)a->elems.size();
        size_t start = (size_t)relIndex(vm, ci.arg(0), len, 0);
        size_t del = 0;
        if (ci.argc == 1) del = (size_t)len - start;
        else if (ci.argc > 1) del = (size_t)std::min(std::max(vm.toInteger(ci.args[1]), 0.0), len - (double)start);
        JSArray* removed = vm.newArray();
        // the conversions above may have run script that shrank the array
        if (start > a->elems.size()) start = a->elems.size();
        if (del > a->elems.size() - start) del = a->elems.size() - start;
        removed->elems.assign(a->elems.begin() + start, a->elems.begin() + start + del);
        a->elems.erase(a->elems.begin() + start, a->elems.begin() + start + del);
        if (ci.argc > 2) a->elems.insert(a->elems.begin() + start, ci.args + 2, ci.args + ci.argc);
        return V(removed);
    });
}
JSFN(arrayReverse) {
    return mutate(ci, [&](Vm& vm, JSArray* a) {
        std::reverse(a->elems.begin(), a->elems.end());
        return V(vm.toObject(ci.thisv));
    });
}
JSFN(arrayFill) {
    return mutate(ci, [&](Vm& vm, JSArray* a) {
        double len = (double)a->elems.size();
        size_t s = (size_t)relIndex(vm, ci.arg(1), len, 0), e = (size_t)relIndex(vm, ci.arg(2), len, len);
        for (size_t i = s; i < e && i < a->elems.size(); ++i) a->elems[i] = ci.arg(0);
        return V(vm.toObject(ci.thisv));
    });
}

static void mergeSort(std::vector<Value>& v, std::vector<Value>& tmp, size_t lo, size_t hi, const std::function<bool(Value, Value)>& less) {
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
    for (k = lo; k < hi; ++k) v[k] = tmp[k];
}

static void sortArray(Vm& vm, JSArray* a, Value cmp) {
    std::vector<Value> vals, tmp, keys;
    RootScope r1(vm, vals), r2(vm, tmp), r3(vm, keys);
    size_t undefs = 0, total = a->elems.size();
    for (Value v : a->elems) {
        if (v.isEmpty()) continue;
        if (v.isUndef()) ++undefs;
        else vals.push_back(v);
    }
    size_t holes = total - undefs - vals.size();
    tmp.resize(vals.size());
    if (!cmp.isUndef()) {
        mergeSort(vals, tmp, 0, vals.size(), [&](Value x, Value y) {
            Value args[2] = {x, y};
            return vm.toNumber(vm.call(cmp, Value::undef(), args, 2)) < 0;
        });
    } else {
        bool allNumbers = true, allStrings = true;
        for (Value v : vals) {
            if (!v.isNumber()) allNumbers = false;
            if (!v.isString()) allStrings = false;
        }
        if (allStrings) {
            mergeSort(vals, tmp, 0, vals.size(), [](Value x, Value y) { return x.asString()->s < y.asString()->s; });
        } else {
            // default order compares the string forms; pair each value with its string once
            std::vector<Value> order(vals.size());
            keys.reserve(vals.size());
            for (Value v : vals) keys.push_back(V(vm.toString(v)));
            for (size_t i = 0; i < order.size(); ++i) order[i] = Value::i32((int32_t)i);
            std::vector<Value> otmp(order.size());
            mergeSort(order, otmp, 0, order.size(), [&](Value x, Value y) { return keys[x.asInt()].asString()->s < keys[y.asInt()].asString()->s; });
            for (size_t i = 0; i < order.size(); ++i) tmp[i] = vals[order[i].asInt()];
            vals.swap(tmp);
            (void)allNumbers;
        }
    }
    a->elems.assign(vals.begin(), vals.end());
    a->elems.insert(a->elems.end(), undefs, Value::undef());
    a->elems.insert(a->elems.end(), holes, Value::empty());
}
JSFN(arraySort) {
    if (!ci.arg(0).isUndef()) needFn(ci.vm, ci.arg(0));
    return mutate(ci, [&](Vm& vm, JSArray* a) {
        sortArray(vm, a, ci.arg(0));
        return V(vm.toObject(ci.thisv));
    });
}

static JSArray* copyOf(Vm& vm, Value thisv) {
    AL a(vm, thisv);
    uint32_t len = a.len();
    JSArray* out = vm.newArray();
    for (uint32_t i = 0; i < len; ++i) out->elems.push_back(a.get(i));
    return out;
}
JSFN(arrayToSorted) {
    if (!ci.arg(0).isUndef()) needFn(ci.vm, ci.arg(0));
    JSArray* out = copyOf(ci.vm, ci.thisv);
    sortArray(ci.vm, out, ci.arg(0));
    return V(out);
}
JSFN(arrayToReversed) {
    JSArray* out = copyOf(ci.vm, ci.thisv);
    std::reverse(out->elems.begin(), out->elems.end());
    return V(out);
}
JSFN(arrayWith) {
    Vm& vm = ci.vm;
    JSArray* out = copyOf(vm, ci.thisv);
    double i = vm.toInteger(ci.arg(0)), len = (double)out->elems.size();
    if (i < 0) i += len;
    if (i < 0 || i >= len) vm.throwError(Err::RangeError, "Invalid index");
    out->elems[(size_t)i] = ci.arg(1);
    return V(out);
}

JSFN(arraySlice) {
    Vm& vm = ci.vm;
    AL a(vm, ci.thisv);
    double len = a.len();
    uint32_t s = (uint32_t)relIndex(vm, ci.arg(0), len, 0), e = (uint32_t)relIndex(vm, ci.arg(1), len, len);
    JSArray* out = vm.newArray();
    if (a.arr) {
        size_t n = a.arr->elems.size();
        if (e > n) e = (uint32_t)n;
        if (s < e) out->elems.assign(a.arr->elems.begin() + s, a.arr->elems.begin() + e);
    } else
        for (uint32_t i = s; i < e; ++i) out->elems.push_back(a.has(i) ? a.get(i) : Value::empty());
    return V(out);
}
JSFN(arrayConcat) {
    Vm& vm = ci.vm;
    JSArray* out = vm.newArray();
    auto addv = [&](Value v) {
        if (vm.isArray(v)) {
            JSArray* s = (JSArray*)v.asObject();
            out->elems.insert(out->elems.end(), s->elems.begin(), s->elems.end());
        } else
            out->elems.push_back(v);
    };
    addv(V(vm.toObject(ci.thisv)));
    for (int i = 0; i < ci.argc; ++i) addv(ci.args[i]);
    return V(out);
}
static Value joinWith(Vm& vm, Value thisv, const std::u16string& sep) {
    AL a(vm, thisv);
    uint32_t len = a.len();
    std::u16string out;
    for (uint32_t i = 0; i < len; ++i) {
        if (i) out += sep;
        Value e = a.get(i);
        if (!e.isNullish()) out += vm.toStr16(e);
    }
    return vm.strv(std::move(out));
}
JSFN(arrayJoin) {
    std::u16string sep = ci.arg(0).isUndef() ? std::u16string(u",") : ci.vm.toStr16(ci.arg(0));
    return joinWith(ci.vm, ci.thisv, sep);
}
JSFN(arrayToString) {
    if (!ci.thisv.isObject()) return objectProtoToString(ci);
    return joinWith(ci.vm, ci.thisv, u",");
}

static Value arraySearch(CallInfo& ci, int mode) {  // 0 indexOf, 1 lastIndexOf, 2 includes
    Vm& vm = ci.vm;
    AL a(vm, ci.thisv);
    double len = a.len();
    Value x = ci.arg(0);
    Value miss = mode == 2 ? Value::boolean(false) : Value::i32(-1);
    if (len == 0) return miss;
    if (mode == 1) {
        double from = ci.argc > 1 ? vm.toInteger(ci.args[1]) : len - 1;
        if (from < 0) from += len;
        if (from >= len) from = len - 1;
        for (double i = from; i >= 0; --i)
            if (a.has((uint32_t)i) && vm.strictEquals(a.get((uint32_t)i), x)) return Value::num(i);
        return miss;
    }
    uint32_t from = (uint32_t)relIndex(vm, ci.arg(1), len, 0);
    for (uint32_t i = from; i < (uint32_t)len; ++i) {
        if (mode == 2) {
            if (vm.sameValueZero(a.get(i), x)) return Value::boolean(true);
        } else if (a.has(i) && vm.strictEquals(a.get(i), x))
            return Value::num(i);
    }
    return miss;
}
JSFN(arrayIndexOf) { return arraySearch(ci, 0); }
JSFN(arrayLastIndexOf) { return arraySearch(ci, 1); }
JSFN(arrayIncludes) { return arraySearch(ci, 2); }

enum { IT_forEach, IT_map, IT_filter, IT_some, IT_every, IT_find, IT_findIndex, IT_findLast, IT_findLastIndex };
static Value arrayEach(CallInfo& ci, int mode) {
    Vm& vm = ci.vm;
    AL a(vm, ci.thisv);
    uint32_t len = a.len();
    Value fn = ci.arg(0), thisArg = ci.arg(1);
    needFn(vm, fn);
    JSArray* out = (mode == IT_map || mode == IT_filter) ? vm.newArray() : nullptr;
    if (mode == IT_map) out->elems.assign(len, Value::empty());
    bool rev = mode == IT_findLast || mode == IT_findLastIndex, findLike = mode >= IT_find;
    for (uint32_t n = 0; n < len; ++n) {
        uint32_t i = rev ? len - 1 - n : n;
        if (!findLike && !a.has(i)) continue;
        Value v = a.get(i);
        Value args[3] = {v, Value::num(i), a.v};
        Value r = vm.call(fn, thisArg, args, 3);
        switch (mode) {
            case IT_map:
                if (i < out->elems.size()) out->elems[i] = r;
                break;
            case IT_filter:
                if (vm.toBool(r)) out->elems.push_back(v);
                break;
            case IT_some:
                if (vm.toBool(r)) return Value::boolean(true);
                break;
            case IT_every:
                if (!vm.toBool(r)) return Value::boolean(false);
                break;
            case IT_find:
            case IT_findLast:
                if (vm.toBool(r)) return v;
                break;
            case IT_findIndex:
            case IT_findLastIndex:
                if (vm.toBool(r)) return Value::num(i);
                break;
            default: break;
        }
    }
    switch (mode) {
        case IT_map:
        case IT_filter: return V(out);
        case IT_some: return Value::boolean(false);
        case IT_every: return Value::boolean(true);
        case IT_findIndex:
        case IT_findLastIndex: return Value::i32(-1);
        default: return Value::undef();
    }
}
JSFN(arrayForEach) { return arrayEach(ci, IT_forEach); }
JSFN(arrayMap) { return arrayEach(ci, IT_map); }
JSFN(arrayFilter) { return arrayEach(ci, IT_filter); }
JSFN(arraySome) { return arrayEach(ci, IT_some); }
JSFN(arrayEvery) { return arrayEach(ci, IT_every); }
JSFN(arrayFind) { return arrayEach(ci, IT_find); }
JSFN(arrayFindIndex) { return arrayEach(ci, IT_findIndex); }
JSFN(arrayFindLast) { return arrayEach(ci, IT_findLast); }
JSFN(arrayFindLastIndex) { return arrayEach(ci, IT_findLastIndex); }

static Value arrayFold(CallInfo& ci, bool right) {
    Vm& vm = ci.vm;
    AL a(vm, ci.thisv);
    uint32_t len = a.len(), n = 0;
    Value fn = ci.arg(0);
    needFn(vm, fn);
    Value acc;
    if (ci.argc >= 2) {
        acc = ci.args[1];
    } else {
        bool found = false;
        for (; n < len && !found; ++n) {
            uint32_t i = right ? len - 1 - n : n;
            if (a.has(i)) {
                acc = a.get(i);
                found = true;
            }
        }
        if (!found) vm.throwError(Err::TypeError, "Reduce of empty array with no initial value");
    }
    for (; n < len; ++n) {
        uint32_t i = right ? len - 1 - n : n;
        if (!a.has(i)) continue;
        Value args[4] = {acc, a.get(i), Value::num(i), a.v};
        acc = vm.call(fn, Value::undef(), args, 4);
    }
    return acc;
}
JSFN(arrayReduce) { return arrayFold(ci, false); }
JSFN(arrayReduceRight) { return arrayFold(ci, true); }

static void flatInto(Vm& vm, JSArray* out, Value src, double depth) {
    AL a(vm, src);
    uint32_t len = a.len();
    for (uint32_t i = 0; i < len; ++i) {
        if (!a.has(i)) continue;
        Value v = a.get(i);
        if (depth >= 1 && vm.isArray(v)) flatInto(vm, out, v, depth - 1);
        else out->elems.push_back(v);
    }
}
JSFN(arrayFlat) {
    Vm& vm = ci.vm;
    double depth = ci.arg(0).isUndef() ? 1 : std::min(vm.toInteger(ci.arg(0)), 1000.0);
    JSArray* out = vm.newArray();
    flatInto(vm, out, V(vm.toObject(ci.thisv)), depth);
    return V(out);
}
JSFN(arrayFlatMap) {
    Vm& vm = ci.vm;
    AL a(vm, ci.thisv);
    uint32_t len = a.len();
    needFn(vm, ci.arg(0));
    JSArray* out = vm.newArray();
    for (uint32_t i = 0; i < len; ++i) {
        if (!a.has(i)) continue;
        Value args[3] = {a.get(i), Value::num(i), a.v};
        Value r = vm.call(ci.arg(0), ci.arg(1), args, 3);
        if (vm.isArray(r)) flatInto(vm, out, r, 0);
        else out->elems.push_back(r);
    }
    return V(out);
}
JSFN(arrayAt) {
    Vm& vm = ci.vm;
    AL a(vm, ci.thisv);
    double len = a.len(), i = vm.toInteger(ci.arg(0));
    if (i < 0) i += len;
    if (i < 0 || i >= len) return Value::undef();
    return a.get((uint32_t)i);
}
JSFN(arrayKeys) { return V(ci.vm.newIter(Cls::ArrayIter, V(ci.vm.toObject(ci.thisv)), 1)); }
JSFN(arrayValues) { return V(ci.vm.newIter(Cls::ArrayIter, V(ci.vm.toObject(ci.thisv)), 0)); }
JSFN(arrayEntries) { return V(ci.vm.newIter(Cls::ArrayIter, V(ci.vm.toObject(ci.thisv)), 2)); }

// ---------------------------------------------------------------------------- Error
static const char* kErrorNames[] = {"Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError", "AggregateError"};

JSFN(errorCtor) {
    Vm& vm = ci.vm;
    int type = ci.callee->data.asInt();
    JSObject* e = vm.newObject(vm.protoFromNewTarget(ci.newTarget, ci.callee->data2.asObject()), Cls::Error);
    int first = type == 7 ? 1 : 0;
    std::u16string msg;
    if (!ci.arg(first).isUndef()) {
        msg = vm.toStr16(ci.arg(first));
        e->add(PropKey::of(vm.a.message), vm.strv(msg), PF_W | PF_C);
    }
    Value opts = ci.arg(first + 1);
    if (opts.isObject() && vm.has(opts.asObject(), PropKey::of(vm.a.cause))) e->add(PropKey::of(vm.a.cause), vm.get(opts, vm.a.cause), PF_W | PF_C);
    if (type == 7) {
        JSArray* errs = vm.newArray();
        vm.iterate(ci.arg(0), [&](Value v) {
            errs->elems.push_back(v);
            return true;
        });
        e->add(PropKey::of(vm.a.errors), V(errs), PF_W | PF_C);
    }
    Value nm = vm.get(V(e), vm.a.name);
    std::u16string head = nm.isString() ? nm.asString()->s : std::u16string(u"Error");
    if (!msg.empty()) head += u": " + msg;
    e->add(PropKey::of(vm.a.stack), vm.strv(head + vm.captureStack()), PF_W | PF_C);
    return V(e);
}
JSFN(errorToString) {
    Vm& vm = ci.vm;
    if (!ci.thisv.isObject()) vm.throwError(Err::TypeError, "Error.prototype.toString requires that 'this' be an Object");
    Value n = vm.get(ci.thisv, vm.a.name), m = vm.get(ci.thisv, vm.a.message);
    std::u16string name = n.isUndef() ? std::u16string(u"Error") : vm.toStr16(n);
    std::u16string msg = m.isUndef() ? std::u16string() : vm.toStr16(m);
    if (name.empty()) return vm.strv(msg);
    if (msg.empty()) return vm.strv(name);
    return vm.strv(name + u": " + msg);
}
JSFN(errorCaptureStackTrace) {
    Vm& vm = ci.vm;
    if (ci.arg(0).isObject()) {
        CallInfo sub{vm, ci.arg(0), nullptr, 0, Value::undef(), ci.callee};
        Value head = ci.arg(0).asObject()->cls == Cls::Error ? errorToString(sub) : vm.strv("Error");
        vm.defineOwn(ci.arg(0).asObject(), PropKey::of(vm.a.stack), vm.strv(head.asString()->s + vm.captureStack()), PF_W | PF_C);
    }
    return Value::undef();
}

// ---------------------------------------------------------------------------- Symbol
JSFN(symbolCtor) {
    Vm& vm = ci.vm;
    if (ci.isConstruct()) vm.throwError(Err::TypeError, "Symbol is not a constructor");
    return V(vm.newSymbol(ci.arg(0).isUndef() ? nullptr : vm.toString(ci.arg(0))));
}
JSFN(symbolFor) {
    Vm& vm = ci.vm;
    JSString* key = vm.toString(ci.arg(0));
    auto it = vm.symbolRegistry.find(key->s);
    if (it != vm.symbolRegistry.end()) return V(it->second);
    Symbol* s = vm.newSymbol(key);
    vm.symbolRegistry[key->s] = s;
    return V(s);
}
JSFN(symbolKeyFor) {
    Vm& vm = ci.vm;
    if (!ci.arg(0).isSymbol()) vm.throwError(Err::TypeError, "Symbol.keyFor requires a symbol");
    for (auto& kv : vm.symbolRegistry)
        if (kv.second == ci.arg(0).asCell()) return vm.strv(kv.first);
    return Value::undef();
}
static Symbol* thisSymbol(CallInfo& ci) {
    if (ci.thisv.isSymbol()) return (Symbol*)ci.thisv.asCell();
    if (ci.thisv.isObject() && ci.thisv.asObject()->cls == Cls::SymbolObj) return (Symbol*)((JSPrimitive*)ci.thisv.asObject())->prim.asCell();
    ci.vm.throwError(Err::TypeError, "Receiver is not a symbol");
}
JSFN(symbolToString) {
    Symbol* s = thisSymbol(ci);
    return ci.vm.strv(u"Symbol(" + (s->desc ? s->desc->s : std::u16string()) + u")");
}
JSFN(symbolValueOf) { return V(thisSymbol(ci)); }
JSFN(symbolDescription) {
    Symbol* s = thisSymbol(ci);
    return s->desc ? V(s->desc) : Value::undef();
}

// ---------------------------------------------------------------------------- Boolean
JSFN(booleanCtor) {
    Value b = Value::boolean(ci.vm.toBool(ci.arg(0)));
    return ci.isConstruct() ? wrapPrimitive(ci, b, Cls::Boolean, ci.vm.booleanProto) : b;
}
static bool thisBoolean(CallInfo& ci) {
    if (ci.thisv.isBool()) return ci.thisv.isTrue();
    if (ci.thisv.isObject() && ci.thisv.asObject()->cls == Cls::Boolean) return ((JSPrimitive*)ci.thisv.asObject())->prim.isTrue();
    ci.vm.throwError(Err::TypeError, "Receiver is not a boolean");
}
JSFN(booleanToString) { return V(thisBoolean(ci) ? ci.vm.a.true_ : ci.vm.a.false_); }
JSFN(booleanValueOf) { return Value::boolean(thisBoolean(ci)); }

// ---------------------------------------------------------------------------- Number
JSFN(numberCtor) {
    Value n = ci.argc ? Value::num(ci.vm.toNumber(ci.args[0])) : Value::i32(0);
    return ci.isConstruct() ? wrapPrimitive(ci, n, Cls::Number, ci.vm.numberProto) : n;
}
static double thisNumber(CallInfo& ci) {
    if (ci.thisv.isNumber()) return ci.thisv.number();
    if (ci.thisv.isObject() && ci.thisv.asObject()->cls == Cls::Number) return ((JSPrimitive*)ci.thisv.asObject())->prim.number();
    ci.vm.throwError(Err::TypeError, "Receiver is not a number");
}
JSFN(numberToStringFn) {
    Vm& vm = ci.vm;
    double d = thisNumber(ci);
    double radix = ci.arg(0).isUndef() ? 10 : vm.toInteger(ci.arg(0));
    if (radix < 2 || radix > 36) vm.throwError(Err::RangeError, "toString() radix must be between 2 and 36");
    return V(vm.numberToString(d, (int)radix));
}
JSFN(numberValueOf) { return Value::num(thisNumber(ci)); }
JSFN(numberToFixed) {
    Vm& vm = ci.vm;
    double d = thisNumber(ci), fd = vm.toInteger(ci.arg(0));
    if (fd < 0 || fd > 100) vm.throwError(Err::RangeError, "toFixed() digits argument must be between 0 and 100");
    int f = (int)fd;
    if (!std::isfinite(d) || std::fabs(d) >= 1e21) return V(vm.numberToString(d));
    double scaled = std::fabs(d) * std::pow(10.0, f);
    if (scaled < 9e15 && scaled - std::floor(scaled) == 0.5) d = std::copysign((std::floor(scaled) + 1) / std::pow(10.0, f), d);
    char buf[512];
    snprintf(buf, sizeof buf, "%.*f", f, d);
    std::string s = buf;
    if (d == 0 && s[0] == '-') s.erase(0, 1);
    return vm.strv(s);
}
static std::string fixExponent(const std::string& s) {  // C prints e+01, JavaScript prints e+1
    size_t e = s.find('e');
    if (e == std::string::npos) return s;
    int ex = atoi(s.c_str() + e + 1);
    return s.substr(0, e) + "e" + (ex >= 0 ? "+" : "-") + std::to_string(std::abs(ex));
}
JSFN(numberToPrecision) {
    Vm& vm = ci.vm;
    double d = thisNumber(ci);
    if (ci.arg(0).isUndef() || !std::isfinite(d)) return V(vm.numberToString(d));
    double pd = vm.toInteger(ci.arg(0));
    if (pd < 1 || pd > 100) vm.throwError(Err::RangeError, "toPrecision() argument must be between 1 and 100");
    int p = (int)pd;
    char buf[512];
    snprintf(buf, sizeof buf, "%.*e", p - 1, d);
    std::string s = buf;
    int e = atoi(s.c_str() + s.find('e') + 1);
    if (e < -6 || e >= p) return vm.strv(fixExponent(s));
    snprintf(buf, sizeof buf, "%.*f", std::max(0, p - 1 - e), d);
    return vm.strv(buf);
}
JSFN(numberToExponential) {
    Vm& vm = ci.vm;
    double d = thisNumber(ci);
    if (!std::isfinite(d)) return V(vm.numberToString(d));
    char buf[512];
    if (ci.arg(0).isUndef()) {
        auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);
        return vm.strv(fixExponent(std::string(buf, r.ptr)));
    }
    double fd = vm.toInteger(ci.arg(0));
    if (fd < 0 || fd > 100) vm.throwError(Err::RangeError, "toExponential() argument must be between 0 and 100");
    snprintf(buf, sizeof buf, "%.*e", (int)fd, d);
    return vm.strv(fixExponent(buf));
}
JSFN(numberToLocaleString) {
    Vm& vm = ci.vm;
    double d = thisNumber(ci);
    if (!std::isfinite(d)) return V(vm.numberToString(d));
    char buf[512];
    snprintf(buf, sizeof buf, "%.3f", std::fabs(d));
    std::string s = buf, frac = s.substr(s.find('.') + 1), ip = s.substr(0, s.find('.'));
    while (!frac.empty() && frac.back() == '0') frac.pop_back();
    std::string out;
    for (size_t i = 0; i < ip.size(); ++i) {
        if (i && (ip.size() - i) % 3 == 0) out += ',';
        out += ip[i];
    }
    if (!frac.empty()) out += "." + frac;
    if (d < 0 && out != "0") out.insert(out.begin(), '-');
    return vm.strv(out);
}
JSFN(numberIsInteger) {
    Value v = ci.arg(0);
    return Value::boolean(v.isNumber() && std::isfinite(v.number()) && v.number() == std::floor(v.number()));
}
JSFN(numberIsSafeInteger) {
    Value v = ci.arg(0);
    return Value::boolean(v.isNumber() && std::isfinite(v.number()) && v.number() == std::floor(v.number()) && std::fabs(v.number()) <= 9007199254740991.0);
}
JSFN(numberIsFinite) { return Value::boolean(ci.arg(0).isNumber() && std::isfinite(ci.arg(0).number())); }
JSFN(numberIsNaN) { return Value::boolean(ci.arg(0).isNumber() && std::isnan(ci.arg(0).number())); }

static int digitValue(char16_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}
JSFN(globalParseInt) {
    Vm& vm = ci.vm;
    JSString* str = vm.toString(ci.arg(0));
    int radix = vm.toInt32(ci.arg(1));
    const std::u16string& s = str->s;
    size_t i = 0, n = s.size();
    while (i < n && isJsSpace(s[i])) ++i;
    bool neg = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    bool hexOk = radix == 0 || radix == 16;
    if (radix == 0) radix = 10;
    if (radix < 2 || radix > 36) return Value::dbl(NAN);
    if (hexOk && i + 1 < n && s[i] == '0' && (s[i + 1] | 0x20) == 'x') {
        i += 2;
        radix = 16;
    }
    double v = 0;
    bool any = false;
    for (; i < n; ++i) {
        int dg = digitValue(s[i]);
        if (dg >= radix) break;
        v = v * radix + dg;
        any = true;
    }
    if (!any) return Value::dbl(NAN);
    return Value::num(neg ? -v : v);
}
JSFN(globalParseFloat) {
    Vm& vm = ci.vm;
    const std::u16string& s = vm.toStr16(ci.arg(0));
    size_t i = 0, n = s.size();
    while (i < n && isJsSpace(s[i])) ++i;
    std::string t;
    if (i < n && (s[i] == '+' || s[i] == '-')) t += (char)s[i++];
    if (s.compare(i, 8, u"Infinity") == 0) return Value::dbl(t == "-" ? -INFINITY : INFINITY);
    bool digits = false;
    while (i < n && s[i] >= '0' && s[i] <= '9') t += (char)s[i++], digits = true;
    if (i < n && s[i] == '.') {
        t += '.';
        ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') t += (char)s[i++], digits = true;
    }
    if (!digits) return Value::dbl(NAN);
    if (i < n && (s[i] | 0x20) == 'e') {
        size_t j = i + 1;
        std::string ex = "e";
        if (j < n && (s[j] == '+' || s[j] == '-')) ex += (char)s[j++];
        if (j < n && s[j] >= '0' && s[j] <= '9') {
            while (j < n && s[j] >= '0' && s[j] <= '9') ex += (char)s[j++];
            t += ex;
        }
    }
    return Value::num(strtod(t.c_str(), nullptr));
}
JSFN(globalIsNaN) { return Value::boolean(std::isnan(ci.vm.toNumber(ci.arg(0)))); }
JSFN(globalIsFinite) { return Value::boolean(std::isfinite(ci.vm.toNumber(ci.arg(0)))); }

// ---------------------------------------------------------------------------- Math
#define MATH1(name, expr)                          \
    JSFN(math_##name) {                            \
        double x = ci.vm.toNumber(ci.arg(0));      \
        return Value::num(expr);                   \
    }
MATH1(abs, std::fabs(x))
MATH1(ceil, std::ceil(x))
MATH1(floor, std::floor(x))
MATH1(trunc, std::trunc(x))
MATH1(sign, x > 0 ? 1.0 : x < 0 ? -1.0 : x)
MATH1(sqrt, std::sqrt(x))
MATH1(cbrt, std::cbrt(x))
MATH1(exp, std::exp(x))
MATH1(expm1, std::expm1(x))
MATH1(log, std::log(x))
MATH1(log2, std::log2(x))
MATH1(log10, std::log10(x))
MATH1(log1p, std::log1p(x))
MATH1(sin, std::sin(x))
MATH1(cos, std::cos(x))
MATH1(tan, std::tan(x))
MATH1(asin, std::asin(x))
MATH1(acos, std::acos(x))
MATH1(atan, std::atan(x))
MATH1(sinh, std::sinh(x))
MATH1(cosh, std::cosh(x))
MATH1(tanh, std::tanh(x))
MATH1(asinh, std::asinh(x))
MATH1(acosh, std::acosh(x))
MATH1(atanh, std::atanh(x))
MATH1(fround, (double)(float)x)
JSFN(math_round) {
    double x = ci.vm.toNumber(ci.arg(0));
    if (!std::isfinite(x)) return Value::dbl(x);
    double r = std::floor(x);
    if (x - r >= 0.5) r += 1;
    return Value::num(r);
}
JSFN(math_pow) { return Value::num(std::pow(ci.vm.toNumber(ci.arg(0)), ci.vm.toNumber(ci.arg(1)))); }
JSFN(math_atan2) { return Value::num(std::atan2(ci.vm.toNumber(ci.arg(0)), ci.vm.toNumber(ci.arg(1)))); }
JSFN(math_imul) { return Value::i32((int32_t)((uint32_t)ci.vm.toInt32(ci.arg(0)) * (uint32_t)ci.vm.toInt32(ci.arg(1)))); }
JSFN(math_clz32) {
    uint32_t x = ci.vm.toUint32(ci.arg(0));
    int n = 0;
    while (n < 32 && !(x & (0x80000000u >> n))) ++n;
    return Value::i32(n);
}
JSFN(math_hypot) {
    double s = 0;
    for (int i = 0; i < ci.argc; ++i) {
        double x = ci.vm.toNumber(ci.args[i]);
        s += x * x;
    }
    return Value::num(std::sqrt(s));
}
static Value minMax(CallInfo& ci, bool isMax) {
    double r = isMax ? -INFINITY : INFINITY;
    bool nan = false;
    for (int i = 0; i < ci.argc; ++i) {
        double x = ci.args[i].isNumber() ? ci.args[i].number() : ci.vm.toNumber(ci.args[i]);
        if (x != x) nan = true;
        else if (isMax ? (x > r || (x == 0 && r == 0 && !std::signbit(x))) : (x < r || (x == 0 && r == 0 && std::signbit(x)))) r = x;
    }
    return nan ? Value::dbl(NAN) : Value::num(r);
}
JSFN(math_max) { return minMax(ci, true); }
JSFN(math_min) { return minMax(ci, false); }
JSFN(math_random) {
    static uint64_t s0 = 0, s1 = 0;
    if (!s0 && !s1) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        s0 = (uint64_t)q.QuadPart * 0x9E3779B97F4A7C15ull + GetTickCount64();
        s1 = s0 ^ 0xD1B54A32D192ED03ull;
        if (!s0) s0 = 1;
    }
    uint64_t x = s0, y = s1;
    s0 = y;
    x ^= x << 23;
    s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return Value::dbl((double)((s1 + y) >> 11) / 9007199254740992.0);
}

// ---------------------------------------------------------------------------- Reflect
static JSObject* needObject(Vm& vm, Value v, const char* what) {
    if (!v.isObject()) vm.throwError(Err::TypeError, "%s called on non-object", what);
    return v.asObject();
}
JSFN(reflectApply) {
    std::vector<Value> args;
    RootScope rs(ci.vm, args);
    spreadArgs(ci.vm, ci.arg(2), args);
    return ci.vm.call(ci.arg(0), ci.arg(1), args.data(), (int)args.size());
}
JSFN(reflectConstruct) {
    std::vector<Value> args;
    RootScope rs(ci.vm, args);
    spreadArgs(ci.vm, ci.arg(1), args);
    return ci.vm.construct(ci.arg(0), args.data(), (int)args.size(), ci.argc > 2 ? ci.args[2] : ci.arg(0));
}
JSFN(reflectDefineProperty) {
    Vm& vm = ci.vm;
    JSObject* o = needObject(vm, ci.arg(0), "Reflect.defineProperty");
    try {
        defineFromDescriptor(vm, o, vm.toKey(ci.arg(1)), ci.arg(2));
    } catch (JSThrow&) {
        vm.pending = Value::undef();
        return Value::boolean(false);
    }
    return Value::boolean(true);
}
JSFN(reflectDeleteProperty) { return Value::boolean(ci.vm.del(needObject(ci.vm, ci.arg(0), "Reflect.deleteProperty"), ci.vm.toKey(ci.arg(1)))); }
JSFN(reflectGet) {
    JSObject* o = needObject(ci.vm, ci.arg(0), "Reflect.get");
    return ci.vm.getObj(o, ci.vm.toKey(ci.arg(1)), ci.argc > 2 ? ci.args[2] : ci.arg(0));
}
JSFN(reflectSet) {
    JSObject* o = needObject(ci.vm, ci.arg(0), "Reflect.set");
    return Value::boolean(ci.vm.setObj(o, ci.vm.toKey(ci.arg(1)), ci.arg(2), ci.argc > 3 ? ci.args[3] : ci.arg(0)));
}
JSFN(reflectHas) { return Value::boolean(ci.vm.has(needObject(ci.vm, ci.arg(0), "Reflect.has"), ci.vm.toKey(ci.arg(1)))); }
JSFN(reflectOwnKeys) {
    needObject(ci.vm, ci.arg(0), "Reflect.ownKeys");
    return keyList(ci.vm, ci.arg(0), true, true);
}
JSFN(reflectGetPrototypeOf) {
    JSObject* p = needObject(ci.vm, ci.arg(0), "Reflect.getPrototypeOf")->proto;
    return p ? V(p) : Value::null();
}
JSFN(reflectSetPrototypeOf) {
    needObject(ci.vm, ci.arg(0), "Reflect.setPrototypeOf");
    setProto(ci.vm, ci.arg(0), ci.arg(1));
    return Value::boolean(true);
}
JSFN(reflectGetOwnPropertyDescriptor) {
    return descriptorOf(ci.vm, needObject(ci.vm, ci.arg(0), "Reflect.getOwnPropertyDescriptor"), ci.vm.toKey(ci.arg(1)));
}
JSFN(reflectIsExtensible) { return Value::boolean(!(needObject(ci.vm, ci.arg(0), "Reflect.isExtensible")->oflags & OF_NOEXT)); }
JSFN(reflectPreventExtensions) {
    needObject(ci.vm, ci.arg(0), "Reflect.preventExtensions")->oflags |= OF_NOEXT;
    return Value::boolean(true);
}

// ---------------------------------------------------------------------------- console
static std::u16string formatLog(CallInfo& ci, int first) {
    Vm& vm = ci.vm;
    std::u16string out;
    int next = first;
    if (ci.argc > first && ci.args[first].isString() && ci.args[first].asString()->s.find(u'%') != std::u16string::npos && ci.argc > first + 1) {
        const std::u16string& f = ci.args[first].asString()->s;
        next = first + 1;
        for (size_t i = 0; i < f.size(); ++i) {
            if (f[i] != '%' || i + 1 >= f.size()) {
                out += f[i];
                continue;
            }
            char16_t c = f[++i];
            if (c == '%') {
                out += u'%';
            } else if (next < ci.argc && (c == 's' || c == 'd' || c == 'i' || c == 'f' || c == 'o' || c == 'O' || c == 'c')) {
                Value a = ci.args[next++];
                if (c == 's') out += a.isString() ? a.asString()->s : vm.inspect(a, 1, false);
                else if (c == 'd' || c == 'i') out += a.isObject() ? std::u16string(u"NaN") : vm.numberToString(std::trunc(vm.toNumber(a)))->s;
                else if (c == 'f') out += a.isObject() ? std::u16string(u"NaN") : vm.numberToString(vm.toNumber(a))->s;
                else if (c != 'c') out += vm.inspect(a, 2, true);
            } else {
                out += u'%';
                out += c;
            }
        }
    }
    for (int i = next; i < ci.argc; ++i) {
        if (!out.empty() || i > first) out += u' ';
        out += vm.inspect(ci.args[i], 2, false);
    }
    return out;
}
JSFN(consoleLog) {
    Vm& vm = ci.vm;
    int level = ci.callee->data.asInt();
    std::u16string text = formatLog(ci, 0);
    if (level == 4) {  // trace
        text = u"Trace: " + text + vm.captureStack();
        level = 0;
    }
    if (vm.onConsole) vm.onConsole(level, text);
    return Value::undef();
}
JSFN(consoleAssert) {
    Vm& vm = ci.vm;
    if (vm.toBool(ci.arg(0))) return Value::undef();
    std::u16string text = u"Assertion failed";
    std::u16string rest = formatLog(ci, 1);
    if (!rest.empty()) text += u": " + rest;
    if (vm.onConsole) vm.onConsole(3, text);
    return Value::undef();
}
JSFN(consoleNoop) { return Value::undef(); }

// ---------------------------------------------------------------------------- URI coding and other globals
static Value uriEncode(CallInfo& ci, const char* keep) {
    std::string in = utf16to8(ci.vm.toStr16(ci.arg(0)));
    std::string out;
    for (unsigned char c : in) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c < 128 && strchr(keep, c) && c)) {
            out += (char)c;
        } else {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        }
    }
    return ci.vm.strv(out);
}
static Value uriDecode(CallInfo& ci, const char* keepEscaped) {
    Vm& vm = ci.vm;
    std::string in = utf16to8(vm.toStr16(ci.arg(0)));
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out += in[i];
            continue;
        }
        int hi = i + 2 < in.size() ? digitValue((char16_t)(unsigned char)in[i + 1]) : 99, lo = i + 2 < in.size() ? digitValue((char16_t)(unsigned char)in[i + 2]) : 99;
        if (hi > 15 || lo > 15) vm.throwError(Err::URIError, "URI malformed");
        char c = (char)(hi * 16 + lo);
        if ((unsigned char)c < 128 && c && strchr(keepEscaped, c)) out += in.substr(i, 3);
        else out += c;
        i += 2;
    }
    // reject byte sequences that are not UTF-8
    if (!out.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, out.data(), (int)out.size(), nullptr, 0) == 0)
        vm.throwError(Err::URIError, "URI malformed");
    return vm.strv(utf8to16(out));
}
JSFN(globalEncodeURIComponent) { return uriEncode(ci, "-_.!~*'()"); }
JSFN(globalEncodeURI) { return uriEncode(ci, "-_.!~*'();/?:@&=+$,#"); }
JSFN(globalDecodeURIComponent) { return uriDecode(ci, ""); }
JSFN(globalDecodeURI) { return uriDecode(ci, ";/?:@&=+$,#"); }
JSFN(globalEscape) {
    const std::u16string& s = ci.vm.toStr16(ci.arg(0));
    std::string out;
    for (char16_t c : s) {
        char b[8];
        if (c < 128 && (isalnum(c) || strchr("@*_+-./", (char)c))) out += (char)c;
        else if (c < 256) snprintf(b, sizeof b, "%%%02X", c), out += b;
        else snprintf(b, sizeof b, "%%u%04X", c), out += b;
    }
    return ci.vm.strv(out);
}
JSFN(globalUnescape) {
    const std::u16string& s = ci.vm.toStr16(ci.arg(0));
    std::u16string out;
    auto hex = [&](size_t at, int n, unsigned& v) {
        if (at + n > s.size()) return false;
        v = 0;
        for (int k = 0; k < n; ++k) {
            int d = digitValue(s[at + k]);
            if (d > 15) return false;
            v = v * 16 + d;
        }
        return true;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned v;
        if (s[i] == '%' && i + 1 < s.size() && s[i + 1] == 'u' && hex(i + 2, 4, v)) out += (char16_t)v, i += 5;
        else if (s[i] == '%' && hex(i + 1, 2, v)) out += (char16_t)v, i += 2;
        else out += s[i];
    }
    return ci.vm.strv(std::move(out));
}
JSFN(globalEval) {
    if (!ci.arg(0).isString()) return ci.arg(0);
    return ci.vm.eval(ci.arg(0).asString()->s, "<eval>");
}
JSFN(globalQueueMicrotask) {
    needFn(ci.vm, ci.arg(0));
    ci.vm.enqueueJob({3, ci.arg(0), Value::undef(), Value::undef()});
    return Value::undef();
}

// ---------------------------------------------------------------------------- setup
void Vm::setupGlobals() {
    Vm& vm = *this;
    auto sym = [&](const char* name) { return newSymbol(str(name)); };
    symIterator = sym("Symbol.iterator");
    symAsyncIterator = sym("Symbol.asyncIterator");
    symHasInstance = sym("Symbol.hasInstance");
    symToPrimitive = sym("Symbol.toPrimitive");
    symToStringTag = sym("Symbol.toStringTag");
    symSpecies = sym("Symbol.species");

    {
        JSArray* ap = alloc<JSArray>();
        ap->proto = objectProto;
        arrayProto = ap;
    }
    auto primProto = [&](Cls cls, Value prim) {
        JSPrimitive* p = alloc<JSPrimitive>();
        p->cls = cls;
        p->proto = objectProto;
        p->prim = prim;
        return p;
    };
    stringProto = primProto(Cls::String, V(a.empty));
    numberProto = primProto(Cls::Number, Value::i32(0));
    booleanProto = primProto(Cls::Boolean, Value::boolean(false));
    symbolProto = newObject(objectProto);

    def(global, "globalThis", V(global));
    def(global, "undefined", Value::undef(), 0);
    def(global, "NaN", Value::dbl(NAN), 0);
    def(global, "Infinity", Value::dbl(INFINITY), 0);

    // Object
    JSNative* objectC = makeCtor(vm, "Object", objectCtor, 1, objectProto);
    defFn(objectC, "keys", objectKeys, 1);
    defFn(objectC, "values", objectValues, 1);
    defFn(objectC, "entries", objectEntries, 1);
    defFn(objectC, "assign", objectAssign, 2);
    defFn(objectC, "create", objectCreate, 2);
    defFn(objectC, "defineProperty", objectDefineProperty, 3);
    defFn(objectC, "defineProperties", objectDefineProperties, 2);
    defFn(objectC, "getPrototypeOf", objectGetPrototypeOf, 1);
    defFn(objectC, "setPrototypeOf", objectSetPrototypeOf, 2);
    defFn(objectC, "getOwnPropertyNames", objectGetOwnPropertyNames, 1);
    defFn(objectC, "getOwnPropertySymbols", objectGetOwnPropertySymbols, 1);
    defFn(objectC, "getOwnPropertyDescriptor", objectGetOwnPropertyDescriptor, 2);
    defFn(objectC, "getOwnPropertyDescriptors", objectGetOwnPropertyDescriptors, 1);
    defFn(objectC, "preventExtensions", objectPreventExtensions, 1);
    defFn(objectC, "seal", objectSeal, 1);
    defFn(objectC, "freeze", objectFreeze, 1);
    defFn(objectC, "isExtensible", objectIsExtensible, 1);
    defFn(objectC, "isFrozen", objectIsFrozen, 1);
    defFn(objectC, "isSealed", objectIsSealed, 1);
    defFn(objectC, "fromEntries", objectFromEntries, 1);
    defFn(objectC, "is", objectIs, 2);
    defFn(objectC, "hasOwn", objectHasOwn, 2);
    defFn(objectProto, "hasOwnProperty", objProtoHasOwnProperty, 1);
    defFn(objectProto, "isPrototypeOf", objProtoIsPrototypeOf, 1);
    defFn(objectProto, "propertyIsEnumerable", objProtoPropertyIsEnumerable, 1);
    defFn(objectProto, "toString", objectProtoToString, 0);
    defFn(objectProto, "toLocaleString", objProtoToLocaleString, 0);
    defFn(objectProto, "valueOf", objProtoValueOf, 0);
    defGetter(objectProto, "__proto__", objProtoGetProto, objProtoSetProto);

    // Function
    makeCtor(vm, "Function", functionCtor, 1, functionProto);
    defFn(functionProto, "call", fnCall, 1);
    defFn(functionProto, "apply", fnApply, 2);
    defFn(functionProto, "bind", fnBind, 1);
    defFn(functionProto, "toString", fnToString, 0);
    defSym(vm, functionProto, symHasInstance, fnHasInstance, "[Symbol.hasInstance]", 1);

    // Array
    JSNative* arrayC = makeCtor(vm, "Array", arrayCtor, 1, arrayProto);
    defFn(arrayC, "isArray", arrayIsArray, 1);
    defFn(arrayC, "of", arrayOf, 0);
    defFn(arrayC, "from", arrayFrom, 1);
    struct {
        const char* name;
        NativeFn fn;
        int len;
    } arrayFns[] = {
        {"push", arrayPush, 1}, {"pop", arrayPop, 0}, {"shift", arrayShift, 0}, {"unshift", arrayUnshift, 1},
        {"splice", arraySplice, 2}, {"reverse", arrayReverse, 0}, {"fill", arrayFill, 1}, {"sort", arraySort, 1},
        {"toSorted", arrayToSorted, 1}, {"toReversed", arrayToReversed, 0}, {"with", arrayWith, 2}, {"slice", arraySlice, 2},
        {"concat", arrayConcat, 1}, {"join", arrayJoin, 1}, {"toString", arrayToString, 0}, {"toLocaleString", arrayToString, 0},
        {"indexOf", arrayIndexOf, 1}, {"lastIndexOf", arrayLastIndexOf, 1}, {"includes", arrayIncludes, 1},
        {"forEach", arrayForEach, 1}, {"map", arrayMap, 1}, {"filter", arrayFilter, 1}, {"some", arraySome, 1},
        {"every", arrayEvery, 1}, {"find", arrayFind, 1}, {"findIndex", arrayFindIndex, 1}, {"findLast", arrayFindLast, 1},
        {"findLastIndex", arrayFindLastIndex, 1}, {"reduce", arrayReduce, 1}, {"reduceRight", arrayReduceRight, 1},
        {"flat", arrayFlat, 0}, {"flatMap", arrayFlatMap, 1}, {"at", arrayAt, 1}, {"keys", arrayKeys, 0},
        {"entries", arrayEntries, 0},
    };
    for (auto& f : arrayFns) defFn(arrayProto, f.name, f.fn, f.len);
    JSNative* values = defFn(arrayProto, "values", arrayValues, 0);
    defineOwn(arrayProto, PropKey::of(symIterator), V(values), PF_W | PF_C);

    // Errors
    JSNative* errorC = nullptr;
    for (int i = 0; i < 8; ++i) {
        JSObject* proto = newObject(i == 0 ? objectProto : errorProto);
        if (i == 0) errorProto = proto;
        if (i < 7) errorProtos[i] = proto;
        JSNative* c = makeCtor(vm, kErrorNames[i], errorCtor, i == 7 ? 2 : 1, proto);
        c->data = Value::i32(i);
        c->data2 = V(proto);
        if (i == 0) errorC = c;
        else c->proto = errorC;
        def(proto, "name", strv(kErrorNames[i]));
        def(proto, "message", V(a.empty));
    }
    defFn(errorProto, "toString", errorToString, 0);
    defFn(errorC, "captureStackTrace", errorCaptureStackTrace, 1);
    def(errorC, "stackTraceLimit", Value::i32(10), PF_ALL);

    // Symbol
    JSNative* symbolC = makeCtor(vm, "Symbol", symbolCtor, 0, symbolProto);
    defFn(symbolC, "for", symbolFor, 1);
    defFn(symbolC, "keyFor", symbolKeyFor, 1);
    def(symbolC, "iterator", V(symIterator), 0);
    def(symbolC, "asyncIterator", V(symAsyncIterator), 0);
    def(symbolC, "hasInstance", V(symHasInstance), 0);
    def(symbolC, "toPrimitive", V(symToPrimitive), 0);
    def(symbolC, "toStringTag", V(symToStringTag), 0);
    def(symbolC, "species", V(symSpecies), 0);
    defFn(symbolProto, "toString", symbolToString, 0);
    defFn(symbolProto, "valueOf", symbolValueOf, 0);
    defGetter(symbolProto, "description", symbolDescription);
    defSym(vm, symbolProto, symToPrimitive, symbolValueOf, "[Symbol.toPrimitive]", 1);

    // Boolean
    makeCtor(vm, "Boolean", booleanCtor, 1, booleanProto);
    defFn(booleanProto, "toString", booleanToString, 0);
    defFn(booleanProto, "valueOf", booleanValueOf, 0);

    // Number
    JSNative* numberC = makeCtor(vm, "Number", numberCtor, 1, numberProto);
    defFn(numberProto, "toString", numberToStringFn, 1);
    defFn(numberProto, "valueOf", numberValueOf, 0);
    defFn(numberProto, "toFixed", numberToFixed, 1);
    defFn(numberProto, "toPrecision", numberToPrecision, 1);
    defFn(numberProto, "toExponential", numberToExponential, 1);
    defFn(numberProto, "toLocaleString", numberToLocaleString, 0);
    defFn(numberC, "isInteger", numberIsInteger, 1);
    defFn(numberC, "isSafeInteger", numberIsSafeInteger, 1);
    defFn(numberC, "isFinite", numberIsFinite, 1);
    defFn(numberC, "isNaN", numberIsNaN, 1);
    JSNative* pInt = defFn(global, "parseInt", globalParseInt, 2);
    JSNative* pFloat = defFn(global, "parseFloat", globalParseFloat, 1);
    def(numberC, "parseInt", V(pInt));
    def(numberC, "parseFloat", V(pFloat));
    def(numberC, "MAX_SAFE_INTEGER", Value::dbl(9007199254740991.0), 0);
    def(numberC, "MIN_SAFE_INTEGER", Value::dbl(-9007199254740991.0), 0);
    def(numberC, "MAX_VALUE", Value::dbl(1.7976931348623157e308), 0);
    def(numberC, "MIN_VALUE", Value::dbl(5e-324), 0);
    def(numberC, "EPSILON", Value::dbl(2.220446049250313e-16), 0);
    def(numberC, "POSITIVE_INFINITY", Value::dbl(INFINITY), 0);
    def(numberC, "NEGATIVE_INFINITY", Value::dbl(-INFINITY), 0);
    def(numberC, "NaN", Value::dbl(NAN), 0);
    defFn(global, "isNaN", globalIsNaN, 1);
    defFn(global, "isFinite", globalIsFinite, 1);

    // Math
    JSObject* math = newObject();
    def(global, "Math", V(math));
    struct {
        const char* name;
        NativeFn fn;
        int len;
    } mathFns[] = {
        {"abs", math_abs, 1}, {"ceil", math_ceil, 1}, {"floor", math_floor, 1}, {"trunc", math_trunc, 1}, {"sign", math_sign, 1},
        {"sqrt", math_sqrt, 1}, {"cbrt", math_cbrt, 1}, {"exp", math_exp, 1}, {"expm1", math_expm1, 1}, {"log", math_log, 1},
        {"log2", math_log2, 1}, {"log10", math_log10, 1}, {"log1p", math_log1p, 1}, {"sin", math_sin, 1}, {"cos", math_cos, 1},
        {"tan", math_tan, 1}, {"asin", math_asin, 1}, {"acos", math_acos, 1}, {"atan", math_atan, 1}, {"sinh", math_sinh, 1},
        {"cosh", math_cosh, 1}, {"tanh", math_tanh, 1}, {"asinh", math_asinh, 1}, {"acosh", math_acosh, 1},
        {"atanh", math_atanh, 1}, {"fround", math_fround, 1}, {"round", math_round, 1}, {"pow", math_pow, 2},
        {"atan2", math_atan2, 2}, {"imul", math_imul, 2}, {"clz32", math_clz32, 1}, {"hypot", math_hypot, 2},
        {"max", math_max, 2}, {"min", math_min, 2}, {"random", math_random, 0},
    };
    for (auto& f : mathFns) defFn(math, f.name, f.fn, f.len);
    def(math, "PI", Value::dbl(3.141592653589793), 0);
    def(math, "E", Value::dbl(2.718281828459045), 0);
    def(math, "LN2", Value::dbl(0.6931471805599453), 0);
    def(math, "LN10", Value::dbl(2.302585092994046), 0);
    def(math, "LOG2E", Value::dbl(1.4426950408889634), 0);
    def(math, "LOG10E", Value::dbl(0.4342944819032518), 0);
    def(math, "SQRT2", Value::dbl(1.4142135623730951), 0);
    def(math, "SQRT1_2", Value::dbl(0.7071067811865476), 0);
    defineOwn(math, PropKey::of(symToStringTag), strv("Math"), PF_C);

    // Reflect
    JSObject* reflect = newObject();
    def(global, "Reflect", V(reflect));
    defFn(reflect, "apply", reflectApply, 3);
    defFn(reflect, "construct", reflectConstruct, 2);
    defFn(reflect, "defineProperty", reflectDefineProperty, 3);
    defFn(reflect, "deleteProperty", reflectDeleteProperty, 2);
    defFn(reflect, "get", reflectGet, 2);
    defFn(reflect, "set", reflectSet, 3);
    defFn(reflect, "has", reflectHas, 2);
    defFn(reflect, "ownKeys", reflectOwnKeys, 1);
    defFn(reflect, "getPrototypeOf", reflectGetPrototypeOf, 1);
    defFn(reflect, "setPrototypeOf", reflectSetPrototypeOf, 2);
    defFn(reflect, "getOwnPropertyDescriptor", reflectGetOwnPropertyDescriptor, 2);
    defFn(reflect, "isExtensible", reflectIsExtensible, 1);
    defFn(reflect, "preventExtensions", reflectPreventExtensions, 1);

    // console
    JSObject* console = newObject();
    def(global, "console", V(console));
    struct {
        const char* name;
        int level;
    } logFns[] = {{"log", 0}, {"debug", 0}, {"dir", 0}, {"table", 0}, {"info", 1}, {"warn", 2}, {"error", 3}, {"trace", 4}};
    for (auto& f : logFns) defFn(console, f.name, consoleLog, 0, PF_ALL)->data = Value::i32(f.level);
    defFn(console, "assert", consoleAssert, 0, PF_ALL);
    for (const char* n : {"group", "groupCollapsed", "groupEnd", "time", "timeEnd", "timeLog", "count", "clear"}) defFn(console, n, consoleNoop, 0, PF_ALL);

    // other globals
    defFn(global, "encodeURIComponent", globalEncodeURIComponent, 1);
    defFn(global, "encodeURI", globalEncodeURI, 1);
    defFn(global, "decodeURIComponent", globalDecodeURIComponent, 1);
    defFn(global, "decodeURI", globalDecodeURI, 1);
    defFn(global, "escape", globalEscape, 1);
    defFn(global, "unescape", globalUnescape, 1);
    defFn(global, "eval", globalEval, 1);
    defFn(global, "queueMicrotask", globalQueueMicrotask, 1);

    setupString(vm);
    setupJSON(vm);
    setupPromise(vm);
    setupDate(vm);
    setupRegExp(vm);
}

}  // namespace tb::js
