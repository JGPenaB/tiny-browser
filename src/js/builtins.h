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

// Regular expression hooks used by the String.prototype methods (regexp.cpp).
bool isRegExp(Value v);
Value regexpReplace(Vm& vm, Value rx, JSString* s, Value replacement, bool all);
Value regexpMatch(Vm& vm, Value rx, JSString* s);
Value regexpMatchAll(Vm& vm, Value rx, JSString* s);
Value regexpSearch(Vm& vm, Value rx, JSString* s);
Value regexpSplit(Vm& vm, Value rx, JSString* s, uint32_t limit);

// Expands $&, $1, $<name>... in a replacement template. caps holds start,end pairs (-1 when a group did not take part).
std::u16string expandReplacement(const std::u16string& tpl, const std::u16string& subject, size_t mStart, size_t mEnd,
                                 const std::vector<int>* caps, const std::vector<std::pair<std::u16string, int>>* names);

}  // namespace tb::js
