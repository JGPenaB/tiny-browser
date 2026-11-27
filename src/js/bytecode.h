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
    OP_GetPrivate,   // a = b.#c: the object must have the private member (ECMA-262 7.3.31 PrivateGet)
    OP_SetPrivate,   // a.#b = c (7.3.32 PrivateSet)
    OP_HasPrivate,   // a = #b in c
    OP_PrivMethod,   // class a gets the private method/getter/setter b = c; the next word: kind (0 method, 1 get, 2 set), +4 static
    OP_ScopeHas,      // a = the scope object b has a binding named c (for `with`: honours Symbol.unscopables)
    OP_NewScopeObj,   // a = a fresh object without a prototype, for variables created by eval
    OP_ScopeDeclare,  // the scope object a gets a property named b, undefined, unless it has one
    OP_ToObject,      // a = ToObject(b)
    OP_DirectEval,  // a = call b with c arguments; when b is the built-in eval, run the text in this scope (site: next word)
    OP_ClassInit,   // run the static block b with `this` set to the class a
    OP_ModuleInit,  // module start: bind the module whose namespace is in a to the current scope
    OP_ImportCall,  // a = import(b, b+1), called from the file named in b+2
    OP_DefGlobalFuncD,  // like OP_DefGlobalFunc, from eval code: the property can be deleted
    OP_WithHas,          // HasBinding of a `with` object (9.1.1.2.1): a = found, b = object, c = name; honours Symbol.unscopables
    OP_WithGet,          // GetBindingValue of an object environment (9.1.1.2.6): a = value, b = object, c = name
    OP_TailCall,         // OP_Call in tail position of strict code (15.10.3 PrepareForTailCall): a plain function takes over the frame
    OP_AddDisposable,    // AddDisposableResource (9.5.2): a = the scope's resource list, b = value, c = 1 for `await using`
    OP_DisposeNext,      // DisposeResources, one stretch: a = a value to await or empty when all is done, b = the list, c = the completion's error so far
    OP_DisposeMerge,     // an awaited disposal failed: a = the error so far (becomes a SuppressedError when there was one), b = the new error
    OP_ThrowIfSet,       // throw a unless it is empty
    OP_JmpEmpty,         // jump to bx when a is empty
    OP_ThrowBadTarget,   // ReferenceError: the target of an assignment was a call (B.3.6)
    OP_SetCtxTdz,        // like OP_SetCtx, for a let, const or class: ReferenceError while the binding is uninitialised; the next word's bx = name
    OP_CheckGlobalDecl,  // may the global name consts[bx] be declared? a: 0 let/const/class, 1 var, 2 function (16.1.7 steps 3 to 10)
    OP_SyncArg,       // a parameter was assigned: a = value, b/c = depth and slot of the hidden arguments variable; the next word's bx = index
    OP_GetTemplate,   // a = the strings object already made for template site bx, or undefined
    OP_SetTemplate,   // GetTemplateObject: a = strings array, b = raw array, c = site; both are frozen and kept
    OP_DelGlobal,     // a = delete of the global name consts[bx]: false for a binding that cannot be removed
    OP_SuperCallIn,   // super(...) from an arrow or eval: a = result, b = base (constructor in b, new.target in b+1, arguments after), c = count or 0xFFFF for a spread array
    OP_BindThis,      // ReferenceError if b (the current `this`) is already set: super() ran twice
    OP_InitFields,    // run the field initialisers of class constructor a on object b
    OP_IterCloseAbrupt,  // close iterator a because of b: quietly for an exception, checked when b is a generator's return
    OP_ThrowUnresolved,  // ReferenceError: consts[bx] is not defined
    OP_RequireObj,  // TypeError unless a can have properties (not undefined or null); consts[bx]: the key, for the message
    OP_SetFnName,   // the anonymous function a is named after the property key b; c: 1 a getter, 2 a setter
    OP_Debugger,
    OP__Count
};

}  // namespace tb::js
