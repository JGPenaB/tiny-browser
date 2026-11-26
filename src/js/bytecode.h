// Register-machine instruction set. Each instruction is 8 bytes: op, a, and either (b, c) or a 32-bit bx.
#pragma once
#include <cstdint>

namespace tb::js {

enum Op : uint16_t {
    OP_Nop,
    OP_LoadK,       // a = consts[bx]
    OP_LoadInt,     // a = (int32)bx
    OP_LoadUndef, OP_LoadNull, OP_LoadTrue, OP_LoadFalse, OP_LoadEmpty,
    OP_Move,        // a = b
    OP_GetGlobal,   // a = global[consts[bx]]  (ReferenceError if missing)
    OP_GetGlobalTypeof,
    OP_SetGlobal,   // global[consts[bx]] = a
    OP_DeclGlobalVar, OP_DeclGlobalLet /* a: 1 = const */, OP_InitGlobalLet, OP_DefGlobalFunc,
    OP_GetCtx,      // a = ctx[depth b][slot c]
    OP_SetCtx,      // ctx[depth b][slot c] = a
    OP_PushCtx,     // new context with bx slots
    OP_PopCtx,      // pop bx contexts
    OP_CloneCtx,    // replace the current context with a copy (per-iteration bindings)
    OP_CheckTDZ,    // throw ReferenceError if a is uninitialised (name = consts[bx])
    OP_ThrowConst,  // TypeError: assignment to constant (name = consts[bx])
    OP_Closure,     // a = closure of inner[bx]
    OP_NewObject, OP_NewArray, OP_ArrayPush /* a.push(b) */, OP_ArrayHole, OP_ArraySpread, OP_NewRegExp /* a = /consts[b]/consts[c] */,
    OP_GetProp,     // a = b[consts[c]]
    OP_SetProp,     // a[consts[b]] = c
    OP_GetElem,     // a = b[c]
    OP_SetElem,     // a[b] = c
    OP_DelProp, OP_DelElem,  // a = delete b[...]
    OP_GetSuper,    // a = super[b]  (b: key register)
    OP_SetSuper,    // super[a] = b
    OP_DefField,    // define own data property: a[consts[b]] = c
    OP_DefElem,     // define own data property: a[b] = c
    OP_DefMethod, OP_DefMethodE,  // a[b] = function c with [[HomeObject]] a (E: enumerable)
    OP_DefGetter, OP_DefSetter, OP_DefGetterE, OP_DefSetterE,
    OP_SetProto,    // a.__proto__ = b (object literal)
    OP_ObjSpread,   // copy own enumerable properties of b into a
    OP_ObjRest,     // a = copy of b without the keys listed in array c
    OP_CheckObj,    // TypeError if a is null or undefined
    OP_Add, OP_Sub, OP_Mul, OP_Div, OP_Mod, OP_Exp, OP_BitAnd, OP_BitOr, OP_BitXor, OP_Shl, OP_Shr, OP_UShr,
    OP_Neg, OP_Plus, OP_Not, OP_BitNot, OP_Inc, OP_Dec, OP_ToNumeric, OP_Typeof, OP_ToPropKey, OP_StrCat,
    OP_Eq, OP_Ne, OP_SEq, OP_SNe, OP_Lt, OP_Le, OP_Gt, OP_Ge, OP_In, OP_InstanceOf,
    OP_Jmp, OP_JmpT, OP_JmpF, OP_JmpNullish, OP_JmpNotNullish, OP_JmpUndef, OP_JmpNotUndef,
    OP_Call,        // a = call r[b](this r[b+1], args r[b+2..b+2+c))
    OP_New,         // a = new r[b](args r[b+2..b+2+c))
    OP_CallSpread,  // a = call r[b](this r[b+1], ...array r[b+2])
    OP_NewSpread,
    OP_SuperCall, OP_SuperCallSpread,
    OP_Return, OP_RetUndef, OP_Throw,
    OP_Catch,       // a = the exception being handled
    OP_GetIter,     // a, a+1 = iterator of b and its next method
    OP_IterNext,    // a = next value of iterator at b, c = done flag
    OP_IterNextRet, // like IterNext, but when the iterator finishes a receives its return value (yield*)
    OP_IterClose,   // close iterator at a
    OP_ForInPrep,   // a = enumeration state for b
    OP_ForInNext,   // a = next key from state b, c = done flag
    OP_CreateArgs, OP_CreateRest /* a = arguments from index b */,
    OP_ThisCheck,   // ReferenceError if `this` (r1) is uninitialised
    OP_LoadNewTarget,
    OP_MakeClass,   // a = constructor, b = parent register (0xFFFF: none), c = receives the prototype object
    OP_AddField,    // instance field: constructor a, key b, initialiser c
    OP_DefStaticField,
    OP_NewPrivate,  // a = fresh private name (description consts[bx])
    OP_Yield,       // a = yield b
    OP_Await,       // a = await b
    OP_GetAsyncIter,  // a, a+1 = async iterator of b and its next method
    OP_IterCall,      // a = b.next(), the raw result (b, b+1 as set by GetAsyncIter)
    OP_IterResult,    // unpack the iterator result b: a = value, c = done
    OP_IterReturnCall,       // a = b.return() if there is one, c = whether it was called; the iterator in b is cleared
    OP_IterReturnCallQuiet,  // the same, with errors swallowed
    OP_CheckIterResult,      // TypeError unless a is an object
    OP_IterCloseQuiet,       // close the iterator in a, ignoring what return() does: an exception is passing
    OP_DelegateCall,    // yield*: a = result of next/throw/return on the inner iterator at b (b+2 received, b+3 mode)
    OP_DelegateResult,  // yield*: read the result at b+4: c = done, a = final value when done
    OP_YieldDelegate,   // yield b from inside yield*; the resumption lands in a (value) and a+1 (mode)
    OP_DirectEval,  // a = call b with c arguments; when b is the built-in eval, run the text in this scope (site: next word)
    OP_ClassInit,   // run the static block b with `this` set to the class a
    OP_ModuleInit,  // module start: bind the module whose namespace is in a to the current scope
    OP_ImportCall,  // a = import(b, b+1), called from the file named in b+2
    OP_Debugger,
    OP__Count
};

}  // namespace tb::js
