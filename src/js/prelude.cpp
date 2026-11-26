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
    auto src = std::make_shared<const std::u16string>(utf8to16(kBuiltinsJs));
    FuncProto* p = compileScript(vm, src, kBuiltinFile, false, nullptr);
    JSFunction* outer = vm.makeClosure(p, nullptr, nullptr);
    Value make = vm.callFunction(outer, V(vm.global), nullptr, 0, Value::undef(), false);
    vm.call(make, Value::undef(), &keep, 1);
}

}  // namespace tb::js
