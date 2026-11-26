// Async iteration (ECMA-262 27.1.6 and 27.6): async generators, the wrapper that lets `for await` and `yield*`
// consume ordinary iterators, and the inner-iterator steps of `yield*` (15.5.5).
#include "builtins.h"

namespace tb::js {

namespace {

enum : uint8_t { AG_Start, AG_Yield, AG_Executing, AG_Draining, AG_Completed };  // [[AsyncGeneratorState]]
enum : uint8_t { RQ_Next, RQ_Throw, RQ_Return };                                 // the completion a request carries

const uint8_t kAsyncFromSyncKind = 201;

Value typeError(Vm& vm, const char* msg) {
    try {
        vm.throwError(Err::TypeError, "%s", msg);
    } catch (JSThrow&) {
    }
    Value e = vm.pending;
    vm.pending = Value::undef();
    return e;
}
JSPromise* rejected(Vm& vm, Value reason) {
    JSPromise* p = vm.newPromise();
    vm.rejectPromise(p, reason);
    return p;
}
// PromiseResolve(%Promise%, v), with a failure turned into a rejected promise
JSPromise* toPromise(Vm& vm, Value v) {
    try {
        return vm.promiseResolve(v);
    } catch (JSThrow&) {
        Value e = vm.pending;
        vm.pending = Value::undef();
        return rejected(vm, e);
    }
}
JSNative* reaction(Vm& vm, NativeFn fn, Value data) {
    JSNative* n = vm.newNative(fn, "", 1);
    n->data = data;
    return n;
}
Value getMethod(Vm& vm, Value o, JSString* name) {  // GetMethod: undefined when missing, TypeError when not callable
    Value m = vm.get(o, name);
    if (m.isNullish()) return Value::undef();
    if (!vm.isCallable(m)) vm.throwError(Err::TypeError, "The iterator's '%s' property is not a function", utf16to8(name->s).c_str());
    return m;
}
// IteratorClose for an error that is already on its way: return() is called and whatever it does is ignored
void closeQuietly(Vm& vm, Value iterator) {
    if (!iterator.isObject()) return;
    try {
        Value ret = getMethod(vm, iterator, vm.a.return_);
        if (!ret.isUndef()) vm.call(ret, iterator, nullptr, 0);
    } catch (JSThrow&) {
        vm.pending = Value::undef();
    }
}

// ---------------------------------------------------------------------------- async generators
JSGenerator* asyncGen(Value v) {
    if (!v.isObject() || v.asObject()->cls != Cls::Generator) return nullptr;
    JSGenerator* g = (JSGenerator*)v.asObject();
    return g->isAsyncGen ? g : nullptr;
}

void run(Vm& vm, JSGenerator* g, int mode, Value value);
void drainQueue(Vm& vm, JSGenerator* g);

// AsyncGeneratorCompleteStep: settles the oldest request
void completeStep(Vm& vm, JSGenerator* g, bool isThrow, Value value, bool done) {
    if (g->queue.empty()) return;
    JSPromise* p = g->queue.front().promise;
    g->queue.erase(g->queue.begin());
    if (isThrow) vm.rejectPromise(p, value);
    else vm.resolvePromise(p, vm.makeIterResult(value, done));
}

JSFN(onAwaitFulfilled) {
    run(ci.vm, (JSGenerator*)ci.callee->data.asCell(), 0, ci.arg(0));
    return Value::undef();
}
JSFN(onAwaitRejected) {
    run(ci.vm, (JSGenerator*)ci.callee->data.asCell(), 1, ci.arg(0));
    return Value::undef();
}
JSFN(onReturnValueFulfilled) {  // the value given to return() has been awaited: now the body returns it
    run(ci.vm, (JSGenerator*)ci.callee->data.asCell(), 2, ci.arg(0));
    return Value::undef();
}
JSFN(onDrainFulfilled) {
    JSGenerator* g = (JSGenerator*)ci.callee->data.asCell();
    completeStep(ci.vm, g, false, ci.arg(0), true);
    drainQueue(ci.vm, g);
    return Value::undef();
}
JSFN(onDrainRejected) {
    JSGenerator* g = (JSGenerator*)ci.callee->data.asCell();
    completeStep(ci.vm, g, true, ci.arg(0), true);
    drainQueue(ci.vm, g);
    return Value::undef();
}
void awaitThen(Vm& vm, JSGenerator* g, Value v, NativeFn onFulfilled, NativeFn onRejected) {
    vm.promiseThen(toPromise(vm, v), V(reaction(vm, onFulfilled, V(g))), V(reaction(vm, onRejected, V(g))), false);
}

// AsyncGeneratorAwaitReturn: a return() that arrives when the body is not running resolves with its awaited value
void awaitReturn(Vm& vm, JSGenerator* g) { awaitThen(vm, g, g->queue.front().value, onDrainFulfilled, onDrainRejected); }

// AsyncGeneratorDrainQueue: the body has finished; answer whatever requests are still waiting
void drainQueue(Vm& vm, JSGenerator* g) {
    while (!g->queue.empty()) {
        JSGenerator::Request next = g->queue.front();
        if (next.type == RQ_Return) {
            awaitReturn(vm, g);
            return;
        }
        completeStep(vm, g, next.type == RQ_Throw, next.type == RQ_Throw ? next.value : Value::undef(), true);
    }
    g->agState = AG_Completed;
}

// AsyncGeneratorResume, and what follows each time the body stops: at an await, at a yield, or for good.
void run(Vm& vm, JSGenerator* g, int mode, Value value) {
    g->agState = AG_Executing;
    for (;;) {
        Value v;
        bool done = false;
        try {
            v = vm.resumeGenerator(g, value, mode, done);
        } catch (JSThrow&) {
            Value e = vm.pending;
            vm.pending = Value::undef();
            g->agState = AG_Draining;
            completeStep(vm, g, true, e, true);
            drainQueue(vm, g);
            return;
        }
        if (done) {
            g->agState = AG_Draining;
            completeStep(vm, g, false, v, true);
            drainQueue(vm, g);
            return;
        }
        if (g->suspendedAtAwait) {
            awaitThen(vm, g, v, onAwaitFulfilled, onAwaitRejected);
            return;
        }
        // AsyncGeneratorYield: the value answers the oldest request. If more requests are waiting the body
        // carries on at once with the next one.
        completeStep(vm, g, false, v, false);
        if (g->queue.empty()) {
            g->agState = AG_Yield;
            return;
        }
        JSGenerator::Request next = g->queue.front();
        if (next.type == RQ_Return) {  // AsyncGeneratorUnwrapYieldResumption: the value is awaited first
            awaitThen(vm, g, next.value, onReturnValueFulfilled, onAwaitRejected);
            return;
        }
        mode = next.type;
        value = next.value;
    }
}

// AsyncGenerator.prototype.next, throw and return (27.6.1.2 to 27.6.1.4)
Value request(CallInfo& ci, uint8_t type) {
    Vm& vm = ci.vm;
    JSPromise* promise = vm.newPromise();
    Value out = V(promise);
    JSGenerator* g = asyncGen(ci.thisv);
    if (!g) {
        vm.rejectPromise(promise, typeError(vm, "AsyncGenerator method called on an incompatible receiver"));
        return out;
    }
    Value value = ci.arg(0);
    uint8_t state = g->agState;
    if (type == RQ_Next) {
        if (state == AG_Completed) {
            vm.resolvePromise(promise, vm.makeIterResult(Value::undef(), true));
            return out;
        }
        g->queue.push_back({type, value, promise});
        if (state == AG_Start || state == AG_Yield) run(vm, g, 0, value);
    } else if (type == RQ_Return) {
        g->queue.push_back({type, value, promise});
        if (state == AG_Start || state == AG_Completed) {
            if (state == AG_Start) {  // the body will never run
                g->state = 3;
                g->regs.clear();
            }
            g->agState = AG_Draining;
            awaitReturn(vm, g);
        } else if (state == AG_Yield) {
            g->agState = AG_Executing;
            awaitThen(vm, g, value, onReturnValueFulfilled, onAwaitRejected);
        }
    } else {
        if (state == AG_Start) {
            g->state = 3;
            g->regs.clear();
            g->agState = state = AG_Completed;
        }
        if (state == AG_Completed) {
            vm.rejectPromise(promise, value);
            return out;
        }
        g->queue.push_back({type, value, promise});
        if (state == AG_Yield) run(vm, g, 1, value);
    }
    return out;
}
JSFN(asyncGeneratorNext) { return request(ci, RQ_Next); }
JSFN(asyncGeneratorThrow) { return request(ci, RQ_Throw); }
JSFN(asyncGeneratorReturn) { return request(ci, RQ_Return); }
JSFN(returnThis) { return ci.thisv; }

// ---------------------------------------------------------------------------- %AsyncFromSyncIteratorPrototype%
JSHost* fromSync(Value v) {
    if (!v.isObject() || v.asObject()->cls != Cls::Host) return nullptr;
    JSHost* h = (JSHost*)v.asObject();
    return h->hostKind == kAsyncFromSyncKind ? h : nullptr;  // slotA: the sync iterator, slotB: its next method
}
JSFN(unwrapNotDone) { return ci.vm.makeIterResult(ci.arg(0), false); }
JSFN(unwrapDone) { return ci.vm.makeIterResult(ci.arg(0), true); }
JSFN(closeAndRethrow) {  // the value turned out to be a rejected promise: close the sync iterator, keep the error
    closeQuietly(ci.vm, ci.callee->data);
    ci.vm.throwValue(ci.arg(0));
}
// AsyncFromSyncIteratorContinuation
Value continuation(Vm& vm, Value result, Value syncIterator, bool closeOnRejection) {
    bool done = false;
    Value value;
    try {
        done = vm.toBool(vm.get(result, vm.a.done));
        value = vm.get(result, vm.a.value);
    } catch (JSThrow&) {
        Value e = vm.pending;
        vm.pending = Value::undef();
        return V(rejected(vm, e));
    }
    JSPromise* wrapper;
    try {
        wrapper = vm.promiseResolve(value);
    } catch (JSThrow&) {
        Value e = vm.pending;
        vm.pending = Value::undef();
        if (!done && closeOnRejection) closeQuietly(vm, syncIterator);
        return V(rejected(vm, e));
    }
    Value onRejected = done || !closeOnRejection ? Value::undef() : V(reaction(vm, closeAndRethrow, syncIterator));
    return V(vm.promiseThen(wrapper, V(reaction(vm, done ? unwrapDone : unwrapNotDone, Value::undef())), onRejected, true));
}
Value fromSyncMethod(CallInfo& ci, int kind) {  // 0 next, 1 throw, 2 return
    Vm& vm = ci.vm;
    JSHost* self = fromSync(ci.thisv);
    if (!self) return V(rejected(vm, typeError(vm, "Async-from-sync iterator method called on an incompatible receiver")));
    Value it = self->slotA;
    try {
        Value result;
        if (kind == 0) {
            result = vm.call(self->slotB, it, ci.args, ci.argc > 0 ? 1 : 0);
        } else {
            Value method = getMethod(vm, it, kind == 1 ? vm.atom("throw") : vm.a.return_);
            if (method.isUndef()) {
                if (kind == 2) return V(vm.promiseResolve(vm.makeIterResult(ci.arg(0), true)));
                // no throw method: give the iterator the chance to clean up, then report the protocol violation
                Value ret = getMethod(vm, it, vm.a.return_);
                if (!ret.isUndef() && !vm.call(ret, it, nullptr, 0).isObject()) vm.throwError(Err::TypeError, "Iterator result is not an object");
                vm.throwError(Err::TypeError, "The iterator does not provide a 'throw' method");
            }
            result = vm.call(method, it, ci.args, ci.argc > 0 ? 1 : 0);
        }
        if (!result.isObject()) vm.throwError(Err::TypeError, "Iterator result %s is not an object", utf16to8(vm.inspect(result, 0)).c_str());
        return continuation(vm, result, it, kind != 2);
    } catch (JSThrow&) {
        Value e = vm.pending;
        vm.pending = Value::undef();
        return V(rejected(vm, e));
    }
}
JSFN(fromSyncNext) { return fromSyncMethod(ci, 0); }
JSFN(fromSyncThrow) { return fromSyncMethod(ci, 1); }
JSFN(fromSyncReturn) { return fromSyncMethod(ci, 2); }

}  // namespace

Value Vm::startAsyncGenerator(JSFunction* f, Value thisv, const Value* args, int argc) {
    JSGenerator* g = (JSGenerator*)startGenerator(f, thisv, args, argc).asCell();
    g->isAsync = true;
    g->isAsyncGen = true;
    Value pv = fnProtoProp(f);
    g->proto = pv.isObject() ? pv.asObject() : asyncGeneratorProto;
    return Value::cell(g);
}

// GetIterator(value, async): the object's own async iterator, or its sync iterator behind a wrapper (7.4.3)
Value Vm::getAsyncIterator(Value src, Value& next) {
    Value method = get(src, PropKey::of(symAsyncIterator));
    if (!method.isNullish()) {
        if (!isCallable(method)) throwError(Err::TypeError, "%s is not async iterable", utf16to8(inspect(src, 0)).c_str());
        Value it = call(method, src, nullptr, 0);
        if (!it.isObject()) throwError(Err::TypeError, "Result of the Symbol.asyncIterator method is not an object");
        next = get(it, a.next);
        return it;
    }
    Value syncMethod = get(src, PropKey::of(symIterator));
    if (!isCallable(syncMethod)) throwError(Err::TypeError, "%s is not async iterable", utf16to8(inspect(src, 0)).c_str());
    Value syncIt = call(syncMethod, src, nullptr, 0);
    if (!syncIt.isObject()) throwError(Err::TypeError, "Result of the Symbol.iterator method is not an object");
    Value wrapper = asyncFromSyncIterator(syncIt);
    next = get(wrapper, a.next);
    return wrapper;
}

// CreateAsyncFromSyncIterator (27.1.6.1)
Value Vm::asyncFromSyncIterator(Value syncIt) {
    JSHost* h = alloc<JSHost>();
    h->hostKind = kAsyncFromSyncKind;
    h->proto = asyncFromSyncProto;
    h->slotA = syncIt;
    Value wrapper = Value::cell(h);
    h->slotB = get(syncIt, a.next);
    return wrapper;
}

// yield*: one call on the inner iterator. regs: iterator, its next method, the value received, the mode
// (0 next, 1 throw, 2 return). Returns the raw result; mode becomes 3 when return() has nothing to call.
Value Vm::delegateCall(Value* regs) {
    Value it = regs[0], next = regs[1], received = regs[2];
    int mode = regs[3].asInt();
    bool builtin = next.isEmpty();  // an array or string iterator from the fast path: it has only next()
    if (mode == 0) {
        if (builtin) {
            Value out;
            bool more = iterStep((JSIter*)it.asObject(), out);
            return makeIterResult(out, !more);
        }
        return call(next, it, &received, 1);
    }
    if (mode == 1) {
        Value method = builtin ? Value::undef() : getMethod(*this, it, atom("throw"));
        if (!method.isUndef()) return call(method, it, &received, 1);
        // no throw method: give the iterator the chance to clean up, then report the protocol violation
        if (!builtin) {
            Value ret = getMethod(*this, it, a.return_);
            if (!ret.isUndef() && !call(ret, it, nullptr, 0).isObject()) throwError(Err::TypeError, "Iterator result is not an object");
        }
        throwError(Err::TypeError, "The iterator does not provide a 'throw' method");
    }
    Value method = builtin ? Value::undef() : getMethod(*this, it, a.return_);
    if (method.isUndef()) {
        regs[3] = Value::i32(3);
        return received;
    }
    return call(method, it, &received, 1);
}

// yield*: reads the (awaited) result of delegateCall, which is in regs[4]. Sets `done`; when done, `value` is
// what yield* evaluates to (mode 0) or what the generator returns (mode 2 or 3).
void Vm::delegateResult(Value* regs, Value& value, bool& done) {
    Value r = regs[4];
    int mode = regs[3].asInt();
    if (mode == 3) {
        value = r;
        done = true;
        return;
    }
    if (!r.isObject()) throwError(Err::TypeError, "Iterator result %s is not an object", utf16to8(inspect(r, 0)).c_str());
    done = toBool(get(r, a.done));
    if (!done) return;
    value = get(r, a.value);
    if (mode != 2) regs[3] = Value::i32(0);
}

void setupAsyncIteration(Vm& vm) {
    vm.genReturnSignal = vm.newObject(nullptr);
    // %AsyncIteratorPrototype%
    vm.asyncIteratorProto = vm.newObject();
    JSNative* self = vm.newNative(returnThis, "[Symbol.asyncIterator]", 0);
    vm.defineOwn(vm.asyncIteratorProto, PropKey::of(vm.symAsyncIterator), V(self), PF_W | PF_C);
    // %AsyncGeneratorPrototype%
    vm.asyncGeneratorProto = vm.newObject(vm.asyncIteratorProto);
    vm.defFn(vm.asyncGeneratorProto, "next", asyncGeneratorNext, 1);
    vm.defFn(vm.asyncGeneratorProto, "throw", asyncGeneratorThrow, 1);
    vm.defFn(vm.asyncGeneratorProto, "return", asyncGeneratorReturn, 1);
    vm.defineOwn(vm.asyncGeneratorProto, PropKey::of(vm.symToStringTag), vm.strv("AsyncGenerator"), PF_C);
    // %AsyncFromSyncIteratorPrototype%
    vm.asyncFromSyncProto = vm.newObject(vm.asyncIteratorProto);
    vm.defFn(vm.asyncFromSyncProto, "next", fromSyncNext, 1);
    vm.defFn(vm.asyncFromSyncProto, "throw", fromSyncThrow, 1);
    vm.defFn(vm.asyncFromSyncProto, "return", fromSyncReturn, 1);
}

}  // namespace tb::js
