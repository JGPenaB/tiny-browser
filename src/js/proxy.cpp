// Proxy objects (ECMA-262 10.5 and 28.2): every operation on the object goes to a handler's trap when it has
// one, then the result is checked against what the target's own non-configurable state allows.
#include <algorithm>

#include "builtins.h"

namespace tb::js {

namespace {

JSProxy* P(JSObject* o) { return (JSProxy*)o; }

// The handler's trap, or undefined to fall through to the target. Throws when the proxy was revoked.
Value trap(Vm& vm, JSProxy* p, const char* name) {
    if (!p->handler.isObject()) vm.throwError(Err::TypeError, "Cannot perform '%s' on a proxy that has been revoked", name);
    Value t = vm.get(p->handler, PropKey::of(vm.atom(name)));
    if (t.isNullish()) return Value::undef();
    if (!vm.isCallable(t)) vm.throwError(Err::TypeError, "The proxy handler's '%s' trap is not a function", name);
    return t;
}
JSObject* target(JSProxy* p) { return p->target.asObject(); }
[[noreturn]] void invariant(Vm& vm, const char* trapName, const char* what) {
    vm.throwError(Err::TypeError, "'%s' on proxy: %s", trapName, what);
}

bool sameKey(PropKey a, PropKey b) { return a == b; }

}  // namespace

void JSProxy::trace(Gc& g) {
    JSObject::trace(g);
    g.mark(target);
    g.mark(handler);
}

JSObject* Vm::getProto(JSObject* o) {
    if (o->cls != Cls::Proxy) return o->proto;
    JSProxy* p = P(o);
    Value t = trap(*this, p, "getPrototypeOf");
    if (t.isUndef()) return getProto(target(p));
    Value args[1] = {p->target};
    Value r = call(t, p->handler, args, 1);
    if (!r.isObject() && !r.isNull()) invariant(*this, "getPrototypeOf", "the trap returned neither an object nor null");
    JSObject* proto = r.isObject() ? r.asObject() : nullptr;
    if (isExtensible(target(p))) return proto;
    if (getProto(target(p)) != proto) invariant(*this, "getPrototypeOf", "the target is not extensible and has a different prototype");
    return proto;
}

// OrdinarySetPrototypeOf and the proxy trap; false when refused
bool Vm::setProtoOf(JSObject* o, JSObject* proto) {
    if (o->cls == Cls::Proxy) {
        JSProxy* p = P(o);
        Value t = trap(*this, p, "setPrototypeOf");
        if (t.isUndef()) return setProtoOf(target(p), proto);
        Value args[2] = {p->target, proto ? Value::cell(proto) : Value::null()};
        if (!toBool(call(t, p->handler, args, 2))) return false;
        if (isExtensible(target(p))) return true;
        if (getProto(target(p)) != proto) invariant(*this, "setPrototypeOf", "the target is not extensible and has a different prototype");
        return true;
    }
    if (o->proto == proto) return true;
    if ((o->oflags & OF_NOEXT) || o == objectProto) return false;  // Object.prototype's prototype is immutable
    for (JSObject* q = proto; q; q = q->proto) {
        if (q == o) return false;  // would make a cycle
        if (q->cls == Cls::Proxy) break;
    }
    o->proto = proto;
    return true;
}

bool Vm::isExtensible(JSObject* o) {
    if (o->cls != Cls::Proxy) return !(o->oflags & OF_NOEXT);
    JSProxy* p = P(o);
    Value t = trap(*this, p, "isExtensible");
    if (t.isUndef()) return isExtensible(target(p));
    Value args[1] = {p->target};
    bool r = toBool(call(t, p->handler, args, 1));
    if (r != isExtensible(target(p))) invariant(*this, "isExtensible", "the trap result does not reflect the extensibility of the target");
    return r;
}

bool Vm::preventExtensions(JSObject* o) {
    if (o->cls != Cls::Proxy) {
        o->oflags |= OF_NOEXT;
        return true;
    }
    JSProxy* p = P(o);
    Value t = trap(*this, p, "preventExtensions");
    if (t.isUndef()) return preventExtensions(target(p));
    Value args[1] = {p->target};
    bool r = toBool(call(t, p->handler, args, 1));
    if (r && isExtensible(target(p))) invariant(*this, "preventExtensions", "the trap returned true but the target is extensible");
    return r;
}

bool Vm::proxyGetOwn(JSObject* o, PropKey k, PropDesc& out) {
    JSProxy* p = P(o);
    Value t = trap(*this, p, "getOwnPropertyDescriptor");
    if (t.isUndef()) return getOwnDesc(target(p), k, out);
    Value args[2] = {p->target, keyToValue(k)};
    Value r = call(t, p->handler, args, 2);
    if (!r.isObject() && !r.isUndef()) invariant(*this, "getOwnPropertyDescriptor", "the trap returned neither an object nor undefined");
    PropDesc targetDesc;
    bool has = getOwnDesc(target(p), k, targetDesc);
    if (r.isUndef()) {
        if (!has) return false;
        if (!targetDesc.configurable) invariant(*this, "getOwnPropertyDescriptor", "the trap reported a non-configurable property as missing");
        if (!isExtensible(target(p))) invariant(*this, "getOwnPropertyDescriptor", "the trap reported a property of a non-extensible target as missing");
        return false;
    }
    bool extensible = isExtensible(target(p));
    PropDesc d;
    toPropDesc(r, d);
    // CompletePropertyDescriptor
    if (!d.hasGet && !d.hasSet) {
        d.hasValue = true;
        d.hasWritable = true;
    } else {
        d.hasGet = d.hasSet = true;
    }
    d.hasEnumerable = d.hasConfigurable = true;
    if (!compatibleDesc(extensible, d, has, targetDesc)) invariant(*this, "getOwnPropertyDescriptor", "the trap returned a descriptor that is incompatible with the target's property");
    if (!d.configurable) {
        if (!has || targetDesc.configurable) invariant(*this, "getOwnPropertyDescriptor", "the trap reported a property as non-configurable that is configurable or missing on the target");
        if (d.hasWritable && !d.writable && targetDesc.hasWritable && targetDesc.writable)
            invariant(*this, "getOwnPropertyDescriptor", "the trap reported a property as non-writable that is writable on the target");
    }
    out = d;
    return true;
}

bool Vm::proxyDefineOwn(JSObject* o, PropKey k, const PropDesc& d) {
    JSProxy* p = P(o);
    Value t = trap(*this, p, "defineProperty");
    if (t.isUndef()) return defineOwnDesc(target(p), k, d);
    Value args[3] = {p->target, keyToValue(k), fromPropDesc(d)};
    if (!toBool(call(t, p->handler, args, 3))) return false;
    PropDesc targetDesc;
    bool has = getOwnDesc(target(p), k, targetDesc);
    bool extensible = isExtensible(target(p));
    bool settingConfigFalse = d.hasConfigurable && !d.configurable;
    if (!has) {
        if (!extensible) invariant(*this, "defineProperty", "the trap added a property to a non-extensible target");
        if (settingConfigFalse) invariant(*this, "defineProperty", "the trap defined a non-configurable property that does not exist on the target");
    } else {
        if (!compatibleDesc(extensible, d, true, targetDesc)) invariant(*this, "defineProperty", "the descriptor is incompatible with the target's property");
        if (settingConfigFalse && targetDesc.configurable) invariant(*this, "defineProperty", "the trap defined a non-configurable property that is configurable on the target");
        if (targetDesc.hasWritable && !targetDesc.configurable && targetDesc.writable && d.hasWritable && !d.writable)
            invariant(*this, "defineProperty", "the trap made a property non-writable that is writable and non-configurable on the target");
    }
    return true;
}

bool Vm::proxyHas(JSObject* o, PropKey k) {
    JSProxy* p = P(o);
    Value t = trap(*this, p, "has");
    if (t.isUndef()) return has(target(p), k);
    Value args[2] = {p->target, keyToValue(k)};
    bool r = toBool(call(t, p->handler, args, 2));
    if (!r) {
        PropDesc targetDesc;
        if (getOwnDesc(target(p), k, targetDesc)) {
            if (!targetDesc.configurable) invariant(*this, "has", "the trap hid a non-configurable property of the target");
            if (!isExtensible(target(p))) invariant(*this, "has", "the trap hid a property of a non-extensible target");
        }
    }
    return r;
}

Value Vm::proxyGet(JSObject* o, PropKey k, Value receiver) {
    JSProxy* p = P(o);
    Value t = trap(*this, p, "get");
    if (t.isUndef()) return getObj(target(p), k, receiver);
    Value args[3] = {p->target, keyToValue(k), receiver};
    Value r = call(t, p->handler, args, 3);
    PropDesc targetDesc;
    if (getOwnDesc(target(p), k, targetDesc) && !targetDesc.configurable) {
        if (targetDesc.hasWritable && !targetDesc.writable && !sameValue(r, targetDesc.value))
            invariant(*this, "get", "the trap returned a different value for a non-writable, non-configurable property");
        if (targetDesc.hasGet && targetDesc.get.isUndef() && !r.isUndef())
            invariant(*this, "get", "the trap returned a value for a non-configurable accessor property without a getter");
    }
    return r;
}

bool Vm::proxySet(JSObject* o, PropKey k, Value v, Value receiver) {
    JSProxy* p = P(o);
    Value t = trap(*this, p, "set");
    if (t.isUndef()) return setObj(target(p), k, v, receiver);
    Value args[4] = {p->target, keyToValue(k), v, receiver};
    if (!toBool(call(t, p->handler, args, 4))) return false;
    PropDesc targetDesc;
    if (getOwnDesc(target(p), k, targetDesc) && !targetDesc.configurable) {
        if (targetDesc.hasWritable && !targetDesc.writable && !sameValue(v, targetDesc.value))
            invariant(*this, "set", "the trap changed the value of a non-writable, non-configurable property");
        if (targetDesc.hasGet && targetDesc.set.isUndef()) invariant(*this, "set", "the trap set a non-configurable accessor property without a setter");
    }
    return true;
}

bool Vm::proxyDelete(JSObject* o, PropKey k) {
    JSProxy* p = P(o);
    Value t = trap(*this, p, "deleteProperty");
    if (t.isUndef()) return del(target(p), k);
    Value args[2] = {p->target, keyToValue(k)};
    if (!toBool(call(t, p->handler, args, 2))) return false;
    PropDesc targetDesc;
    if (!getOwnDesc(target(p), k, targetDesc)) return true;
    if (!targetDesc.configurable) invariant(*this, "deleteProperty", "the trap deleted a non-configurable property of the target");
    if (!isExtensible(target(p))) invariant(*this, "deleteProperty", "the trap deleted a property of a non-extensible target");
    return true;
}

void Vm::proxyOwnKeys(JSObject* o, std::vector<PropKey>& out) {
    JSProxy* p = P(o);
    Value t = trap(*this, p, "ownKeys");
    if (t.isUndef()) {
        ownKeys(target(p), out, true, true, false);
        return;
    }
    Value args[1] = {p->target};
    Value list = call(t, p->handler, args, 1);
    if (!list.isObject()) invariant(*this, "ownKeys", "the trap did not return an object");
    std::vector<PropKey> result;
    double len = toInteger(get(list, a.length));
    for (double i = 0; i < len; ++i) {
        Value e = get(list, toKey(Value::num(i)));
        if (!e.isString() && !e.isSymbol()) invariant(*this, "ownKeys", "the trap result contains something that is neither a string nor a symbol");
        PropKey k = toKey(e);
        for (PropKey seen : result)
            if (sameKey(seen, k)) invariant(*this, "ownKeys", "the trap result contains a duplicate key");
        result.push_back(k);
    }
    bool extensible = isExtensible(target(p));
    std::vector<PropKey> targetKeys, configurable, nonconfigurable;
    ownKeys(target(p), targetKeys, true, true, false);
    for (PropKey k : targetKeys) {
        PropDesc d;
        if (getOwnDesc(target(p), k, d) && !d.configurable) nonconfigurable.push_back(k);
        else configurable.push_back(k);
    }
    if (!(extensible && nonconfigurable.empty())) {
        std::vector<PropKey> unchecked = result;
        auto take = [&](PropKey k) {
            for (size_t i = 0; i < unchecked.size(); ++i)
                if (sameKey(unchecked[i], k)) {
                    unchecked.erase(unchecked.begin() + i);
                    return true;
                }
            return false;
        };
        for (PropKey k : nonconfigurable)
            if (!take(k)) invariant(*this, "ownKeys", "the trap result leaves out a non-configurable key of the target");
        if (!extensible) {
            for (PropKey k : configurable)
                if (!take(k)) invariant(*this, "ownKeys", "the trap result leaves out a key of the non-extensible target");
            if (!unchecked.empty()) invariant(*this, "ownKeys", "the trap result adds a key to the non-extensible target");
        }
    }
    out.insert(out.end(), result.begin(), result.end());
}

Value Vm::proxyCall(JSObject* o, Value thisv, const Value* args, int argc) {
    JSProxy* p = P(o);
    Value t = trap(*this, p, "apply");
    if (t.isUndef()) return call(p->target, thisv, args, argc);
    JSArray* list = newArray();
    list->elems.assign(args, args + argc);
    Value a3[3] = {p->target, thisv, Value::cell(list)};
    return call(t, p->handler, a3, 3);
}

Value Vm::proxyConstruct(JSObject* o, const Value* args, int argc, Value newTarget) {
    JSProxy* p = P(o);
    Value t = trap(*this, p, "construct");
    if (t.isUndef()) return construct(p->target, args, argc, newTarget);
    JSArray* list = newArray();
    list->elems.assign(args, args + argc);
    Value a3[3] = {p->target, Value::cell(list), newTarget};
    Value r = call(t, p->handler, a3, 3);
    if (!r.isObject()) invariant(*this, "construct", "the trap did not return an object");
    return r;
}

namespace {

JSProxy* proxyCreate(Vm& vm, Value target, Value handler) {
    if (!target.isObject() || !handler.isObject()) vm.throwError(Err::TypeError, "Cannot create proxy with a non-object as target or handler");
    JSProxy* p = vm.alloc<JSProxy>();
    p->target = target;
    p->handler = handler;
    p->callable = vm.isCallable(target);
    p->constructible = vm.isConstructor(target);
    return p;
}
JSFN(proxyCtor) {
    if (!ci.isConstruct()) ci.vm.throwError(Err::TypeError, "Constructor Proxy requires 'new'");
    return V(proxyCreate(ci.vm, ci.arg(0), ci.arg(1)));
}
JSFN(proxyRevoke) {
    Value pv = ci.callee->data;
    if (!pv.isObject()) return Value::undef();
    JSProxy* p = (JSProxy*)pv.asObject();
    ci.callee->data = Value::null();
    p->handler = Value::null();
    return Value::undef();
}
JSFN(proxyRevocable) {
    Vm& vm = ci.vm;
    JSProxy* p = proxyCreate(vm, ci.arg(0), ci.arg(1));
    Value keep = V(p);
    JSNative* revoke = vm.newNative(proxyRevoke, "", 0);
    revoke->data = keep;
    JSObject* result = vm.newObject();
    result->add(PropKey::of(vm.atom("proxy")), keep, PF_ALL);
    result->add(PropKey::of(vm.atom("revoke")), V(revoke), PF_ALL);
    return V(result);
}

}  // namespace

void setupProxy(Vm& vm) {
    JSNative* c = vm.newNative(proxyCtor, "Proxy", 2);
    c->oflags |= OF_CTOR;
    vm.def(vm.global, "Proxy", V(c), PF_W | PF_C);
    vm.defFn(c, "revocable", proxyRevocable, 2);
}

}  // namespace tb::js
