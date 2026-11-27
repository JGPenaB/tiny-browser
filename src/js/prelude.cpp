// Loads the built-ins that are written in JavaScript (builtins_js.h) and gives them the few native helpers
// they need.
#include "builtins.h"
#include "builtins_js.h"

namespace tb::js {

namespace {

JSFN(inherits) {  // does `proto` appear on the prototype chain of the object
    if (!ci.arg(0).isObject() || !ci.arg(1).isObject()) return Value::boolean(false);
    for (JSObject* p = ci.arg(0).asObject()->proto; p; p = p->proto)
        if (p == ci.arg(1).asObject()) return Value::boolean(true);
    return Value::boolean(false);
}
JSFN(isConstructor) { return Value::boolean(ci.vm.isConstructor(ci.arg(0))); }
JSFN(isError) { return Value::boolean(ci.arg(0).isObject() && ci.arg(0).asObject()->cls == Cls::Error); }
JSFN(asyncFromSync) { return ci.vm.asyncFromSyncIterator(ci.arg(0)); }
JSFN(toLength) {
    double d = ci.vm.toInteger(ci.arg(0));
    if (!(d > 0)) return Value::i32(0);
    return Value::num(d > 9007199254740991.0 ? 9007199254740991.0 : d);
}
JSFN(toStringNative) { return V(ci.vm.toString(ci.arg(0))); }
JSFN(toIntegerNative) { return Value::num(ci.vm.toInteger(ci.arg(0)) + 0.0); }
JSFN(regexpExecNative) { return regexpBuiltinExec(ci.vm, ci.arg(0), ci.vm.toString(ci.arg(1))); }
JSFN(promiseThenNative) {  // PerformPromiseThen(promise, onFulfilled, onRejected, capability's resolve and reject)
    ci.vm.promiseThenWith((JSPromise*)ci.arg(0).asObject(), ci.arg(1), ci.arg(2), ci.arg(3), ci.arg(4));
    return Value::undef();
}
JSFN(toPropertyKeyNative) { return ci.vm.keyToValue(ci.vm.toKey(ci.arg(0))); }
JSFN(isPromise) { return Value::boolean(ci.arg(0).isObject() && ci.arg(0).asObject()->cls == Cls::Promise); }
JSFN(newPromiseCapability) {
    Vm& vm = ci.vm;
    Vm::PromiseCapability cap = vm.newPromiseCapability(ci.arg(0));
    JSObject* o = vm.newObject(nullptr);
    vm.def(o, "promise", cap.promise, PF_ALL);
    vm.def(o, "resolve", cap.resolve, PF_ALL);
    vm.def(o, "reject", cap.reject, PF_ALL);
    return V(o);
}
JSFN(promiseResolve) { return ci.vm.promiseResolveWith(ci.arg(0), ci.arg(1)); }
JSFN(speciesConstructor) { return ci.vm.speciesConstructor(ci.arg(0).asObject(), ci.arg(1)); }
JSFN(iteratorCtor) {  // 27.1.3.1.1 Iterator(): only a subclass may be constructed
    Vm& vm = ci.vm;
    if (!ci.isConstruct() || ci.newTarget == V(ci.callee)) vm.throwError(Err::TypeError, "Iterator is an abstract class and cannot be constructed directly");
    return V(vm.newObject(vm.protoFromNewTarget(ci.newTarget, vm.iteratorProto)));
}
JSFN(protoFor) { return V(ci.vm.protoFromNewTarget(ci.arg(0), ci.arg(1).asObject())); }  // GetPrototypeFromConstructor (10.1.14)
JSFN(intrinsicProto) {  // a prototype made in JavaScript joins the realm's list of them
    ci.vm.intrinsicProtos.push_back(ci.arg(0).asObject());
    return Value::undef();
}
JSFN(arrayCreate) {
    Vm& vm = ci.vm;
    double n = vm.toNumber(ci.arg(0));
    if (n > 4294967295.0) vm.throwError(Err::RangeError, "Invalid array length");
    JSArray* a = vm.newArray();
    if (n > 50000000.0) {
        a->oflags |= OF_SLOWIDX;
        a->slowLen = (uint32_t)n;
    } else if (n > 0)
        a->elems.assign((size_t)n, Value::empty());
    return V(a);
}

}  // namespace

void setupJsBuiltins(Vm& vm) {
    JSObject* natives = vm.newObject();
    Value keep = V(natives);
    vm.defFn(natives, "inherits", inherits, 2);
    vm.defFn(natives, "isConstructor", isConstructor, 1);
    vm.defFn(natives, "isError", isError, 1);
    vm.defFn(natives, "asyncFromSync", asyncFromSync, 1);
    vm.defFn(natives, "toLength", toLength, 1);
    vm.defFn(natives, "arrayCreate", arrayCreate, 1);
    vm.defFn(natives, "toString", toStringNative, 1);
    vm.defFn(natives, "toInteger", toIntegerNative, 1);
    vm.defFn(natives, "regexpBuiltinExec", regexpExecNative, 2);
    vm.defFn(natives, "isPromise", isPromise, 1);
    vm.defFn(natives, "toPropertyKey", toPropertyKeyNative, 1);
    vm.defFn(natives, "promiseThen", promiseThenNative, 5);
    vm.defFn(natives, "newPromiseCapability", newPromiseCapability, 1);
    vm.defFn(natives, "promiseResolve", promiseResolve, 2);
    vm.defFn(natives, "speciesConstructor", speciesConstructor, 2);
    vm.defFn(natives, "protoFor", protoFor, 2);
    vm.defFn(natives, "Iterator", iteratorCtor, 0)->oflags |= OF_CTOR;
    vm.intrinsicProtos.push_back(vm.iteratorProto);
    vm.defFn(natives, "intrinsicProto", intrinsicProto, 1);
    auto src = std::make_shared<const std::u16string>(utf8to16(kBuiltinsJs));
    FuncProto* p = compileScript(vm, src, kBuiltinFile, false, nullptr);
    JSFunction* outer = vm.makeClosure(p, nullptr, nullptr);
    Value make = vm.callFunction(outer, V(vm.global), nullptr, 0, Value::undef(), false);
    vm.call(make, Value::undef(), &keep, 1);
    regexpSnapshot(vm);
}

}  // namespace tb::js
