// Shared helpers for the standard library source files.
#pragma once
#include "js.h"

namespace tb::js {

#define JSFN(name) static Value name(CallInfo& ci)

inline Value V(const Cell* c) { return Value::cell(c); }

inline bool isJsSpace(char16_t c) {
    return c == ' ' || (c >= 9 && c <= 13) || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

// Creates a constructor function, links it with its prototype object and publishes it as a global.
JSNative* makeCtor(Vm& vm, const char* name, NativeFn fn, int length, JSObject* proto);
void setupFunctionKinds(Vm& vm);
Value wrapPrimitive(CallInfo& ci, Value prim, Cls cls, JSObject* proto);
void defSym(Vm& vm, JSObject* o, Symbol* sym, NativeFn fn, const char* name, int length);
void makeResolvingFunctions(Vm& vm, JSPromise* p, Value out[2]);  // vm.cpp
Value objectProtoToString(CallInfo& ci);

void setupString(Vm& vm);
void setupJSON(Vm& vm);
void setupPromise(Vm& vm);
void setupDate(Vm& vm);
void setupRegExp(Vm& vm);
void setupTypedArrays(Vm& vm);
void setupModules(Vm& vm);
void setupShadowRealm(Vm& vm);
// An indirect eval (19.2.1.1 PerformEval with direct = false) in two steps, each in the realm that is running then
FuncProto* compileIndirectEval(Vm& vm, const std::u16string& text);
Value runIndirectEval(Vm& vm, FuncProto* code);
void setupProxy(Vm& vm);
void setupBigInt(Vm& vm);
void setupJsBuiltins(Vm& vm);
// File name of the built-ins written in JavaScript: their functions print as native code.
constexpr const char* kBuiltinFile = "<builtins>";
void setupArrayMethods(Vm& vm, JSObject* proto, JSNative* ctor);
void setupAsyncIteration(Vm& vm);

// Regular expression hooks (regexp.cpp). The four direct routes may only be taken when regexpIsPristine holds.
bool isRegExp(Value v);                 // has the internal slots of a RegExp
bool isRegExpLike(Vm& vm, Value v);     // IsRegExp (7.2.6)
bool regexpIsPristine(Vm& vm, Value v); // v is a RegExp that no script has customised, and neither is RegExp.prototype
void regexpSnapshot(Vm& vm);
Value regexpBuiltinExec(Vm& vm, Value rx, JSString* s);
Value regexpReplace(Vm& vm, Value rx, JSString* s, Value replacement, bool all);
Value regexpMatch(Vm& vm, Value rx, JSString* s);
Value regexpSearch(Vm& vm, Value rx, JSString* s);
Value regexpSplit(Vm& vm, Value rx, JSString* s, uint32_t limit);

// Expands $&, $1, $<name>... in a replacement template. caps holds start,end pairs (-1 when a group did not take part).
std::u16string expandReplacement(const std::u16string& tpl, const std::u16string& subject, size_t mStart, size_t mEnd,
                                 const std::vector<int>* caps, const std::vector<std::pair<std::u16string, int>>* names);

}  // namespace tb::js
