// ShadowRealm (the TC39 proposal, https://tc39.es/proposal-shadowrealm/; section numbers below are its own): a
// realm that code reaches only through text to evaluate and modules to import. No object crosses the boundary
// between the two sides: a primitive passes as it is, a function is replaced by a wrapper made for the side it
// arrives on, and anything else is refused.
#include <algorithm>
#include <cmath>

#include "builtins.h"

namespace tb::js {

namespace {

// CreateTypeErrorCopy (2.2): the pending exception came from the other side, and the running realm gets a
// TypeError in its place. No code of either side may run here, so only the plain message of an Error is kept.
[[noreturn]] void throwCopy(Vm& vm) {
    Value thrown = vm.pending;
    vm.pending = Value::undef();
    std::u16string text = u"An error was thrown on the other side of a ShadowRealm boundary";
    if (thrown.isObject() && thrown.asObject()->cls == Cls::Error) {
        Prop* message = thrown.asObject()->find(PropKey::of(vm.a.message));
        if (message && !(message->flags & PF_ACC) && message->val.isString()) text += u": " + message->val.asString()->s;
    }
    vm.throwError(Err::TypeError, text);
}

Value wrapValue(Vm& vm, uint16_t into, Value v);

// The [[Call]] of a wrapped function exotic object (2.1, 2.3 OrdinaryWrappedFunctionCall). The wrapper is a native
// function of the realm it was made for; data holds the function it stands for.
JSFN(wrappedCall) {
    Vm& vm = ci.vm;
    Value target = ci.callee->data;
    uint16_t mine = vm.currentRealm, theirs = vm.functionRealm(target.asObject());
    std::vector<Value> args;
    RootScope keep(vm, args);
    for (int i = 0; i < ci.argc; ++i) args.push_back(wrapValue(vm, theirs, ci.args[i]));
    args.push_back(wrapValue(vm, theirs, ci.thisv));
    Value result;
    try {
        result = vm.call(target, args.back(), args.data(), ci.argc);
    } catch (JSThrow&) {
        if (vm.terminated) throw;
        throwCopy(vm);
    }
    return wrapValue(vm, mine, result);
}

// GetWrappedValue (3.1.5): what `v` is on the side of realm `into`. A function becomes a new wrapper there
// (3.1.1 WrappedFunctionCreate), with the target's length and name (3.1.2 CopyNameAndLength).
Value wrapValue(Vm& vm, uint16_t into, Value v) {
    if (!v.isObject()) return v;
    if (!vm.isCallable(v)) vm.throwError(Err::TypeError, "Only primitive values and functions can cross a ShadowRealm boundary");
    JSNative* wrapper;
    {
        RealmScope there(vm, into);
        wrapper = vm.newNative(wrappedCall, "", 0);
    }
    wrapper->data = v;
    wrapper->oflags |= OF_NO_NAME | OF_NO_LENGTH;  // its own two are the copies made below
    try {
        double length = 0;
        if (vm.hasOwn(v.asObject(), PropKey::of(vm.a.length))) {
            Value targetLength = vm.get(v, vm.a.length);
            if (targetLength.isNumber()) {
                double d = targetLength.number();
                length = d == INFINITY ? d : d == -INFINITY ? 0 : std::max(vm.toInteger(targetLength), 0.0);
            }
        }
        Value name = vm.get(v, vm.a.name);
        wrapper->add(PropKey::of(vm.a.length), Value::num(length), PF_C);
        wrapper->add(PropKey::of(vm.a.name), name.isString() ? name : V(vm.a.empty), PF_C);
    } catch (JSThrow&) {
        if (vm.terminated) throw;
        vm.pending = Value::undef();
        vm.throwError(Err::TypeError, "The length and name of a function could not be read at a ShadowRealm boundary");
    }
    return V(wrapper);
}

// ValidateShadowRealmObject (3.1.6): the number of the realm behind the receiver
uint16_t shadowRealmOf(CallInfo& ci, const char* method) {
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != Cls::ShadowRealm) ci.vm.throwError(Err::TypeError, "%s: the receiver is not a ShadowRealm", method);
    return (uint16_t)((JSPrimitive*)ci.thisv.asObject())->prim.asInt();
}

// The new realm's module loader asks the realm that made it how to resolve and fetch (data: that realm's host
// object, data2: the name of the function there).
JSFN(forwardToHost) {
    Vm& vm = ci.vm;
    Value host = ci.callee->data;
    return vm.call(vm.get(host, PropKey::of(vm.atomOf(ci.callee->data2.asString()))), host, ci.args, ci.argc);
}

JSFN(shadowRealmCtor) {  // 3.2.1
    Vm& vm = ci.vm;
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Constructor ShadowRealm requires 'new'");
    JSPrimitive* o = vm.alloc<JSPrimitive>();
    o->cls = Cls::ShadowRealm;
    o->proto = vm.protoFromNewTarget(ci.newTarget, ci.callee->data2.asObject());
    Value result = V(o), creatorHost = vm.moduleHost;
    uint16_t realm = vm.createRealm();
    o->prim = Value::i32(realm);
    // HostInitializeShadowRealm (3.6.1): the realm loads modules the way its maker does, and code running in it
    // has no handle on the loader
    RealmScope there(vm, realm);
    for (const char* hook : {"resolve", "fetch"}) {
        JSNative* forward = vm.newNative(forwardToHost, hook, 2);
        forward->data = creatorHost;
        forward->data2 = vm.strv(hook);
        vm.set(vm.moduleHost, hook, V(forward));
    }
    vm.del(vm.global, PropKey::of(vm.atom("__modules")));
    return result;
}

JSFN(shadowEvaluate) {  // 3.4.1, with PerformShadowRealmEval (3.1.3): the text runs as an indirect eval of the other realm
    Vm& vm = ci.vm;
    uint16_t evalRealm = shadowRealmOf(ci, "ShadowRealm.prototype.evaluate"), callerRealm = vm.currentRealm;
    if (!ci.arg(0).isString()) vm.throwError(Err::TypeError, "ShadowRealm.prototype.evaluate takes the source text as a string");
    FuncProto* code = compileIndirectEval(vm, ci.arg(0).asString()->s);  // a SyntaxError is an error of this side
    Value result;
    try {
        RealmScope there(vm, evalRealm);
        result = runIndirectEval(vm, code);
    } catch (JSThrow&) {
        if (vm.terminated) throw;
        throwCopy(vm);
    }
    return wrapValue(vm, callerRealm, result);
}

JSFN(exportGetter) {  // 3.1.4.1; data: the name of the export wanted from the namespace object it is given
    Vm& vm = ci.vm;
    Value exports = ci.arg(0);
    PropKey name = vm.toKey(ci.callee->data);
    if (!exports.isObject() || !vm.hasOwn(exports.asObject(), name))
        vm.throwError(Err::TypeError, "The module has no export named '%s'", utf16to8(ci.callee->data.asString()->s).c_str());
    return wrapValue(vm, vm.currentRealm, vm.get(exports, name));
}
JSFN(importValueError) {  // 3.1.4.2
    ci.vm.pending = ci.arg(0);
    throwCopy(ci.vm);
}
JSFN(shadowImportValue) {  // 3.4.2, with ShadowRealmImportValue (3.1.4)
    Vm& vm = ci.vm;
    uint16_t evalRealm = shadowRealmOf(ci, "ShadowRealm.prototype.importValue");
    Value specifier = V(vm.toString(ci.arg(0)));
    if (!ci.arg(1).isString()) vm.throwError(Err::TypeError, "ShadowRealm.prototype.importValue takes the export's name as a string");
    // the import starts in the other realm; specifiers are resolved against the script that asked
    Value inner;
    {
        Value args[3] = {specifier, Value::undef(), vm.strv(utf8to16(vm.activeFile("")))};
        RealmScope there(vm, evalRealm);
        inner = vm.call(vm.importHook, Value::undef(), args, 3);
    }
    JSNative* onFulfilled = vm.newNative(exportGetter, "", 1);
    onFulfilled->data = ci.arg(1);
    JSNative* onRejected = vm.newNative(importValueError, "", 1);
    return V(vm.promiseThen((JSPromise*)inner.asObject(), V(onFulfilled), V(onRejected)));
}

}  // namespace

void setupShadowRealm(Vm& vm) {
    JSObject* proto = vm.newObject();
    makeCtor(vm, "ShadowRealm", shadowRealmCtor, 0, proto)->data2 = V(proto);
    vm.defFn(proto, "evaluate", shadowEvaluate, 1);
    vm.defFn(proto, "importValue", shadowImportValue, 2);
    vm.defineOwn(proto, PropKey::of(vm.symToStringTag), vm.strv("ShadowRealm"), PF_C);
}

}  // namespace tb::js
