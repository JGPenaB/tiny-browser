// Built-ins that are written in JavaScript on top of the native ones: iterator helpers (ECMA-262 27.1.3 and
// 27.1.4), the Set methods (24.2.4), Object.groupBy, Map.groupBy, Array.fromAsync, Promise.try and a few string
// functions. The code holds on to the originals of everything it uses, so page scripts that replace or patch
// built-ins later cannot change how these behave. Run once per engine instance, before any other script.
#pragma once

namespace tb::js {

static const char kBuiltinsJs[] = R"JS((function (natives) {
'use strict';
var $Object = Object, $TypeError = TypeError, $RangeError = RangeError, $Symbol = Symbol, $WeakMap = WeakMap, $Set = Set, $Map = Map,
    $Promise = Promise, $String = String, $Infinity = Infinity;
var apply = Reflect.apply, ownKeys = Reflect.ownKeys, defineProperty = $Object.defineProperty, create = $Object.create,
    getOwnPropertyDescriptor = $Object.getOwnPropertyDescriptor, getPrototypeOf = $Object.getPrototypeOf, trunc = Math.trunc;
var symIterator = $Symbol.iterator, symAsyncIterator = $Symbol.asyncIterator, symToStringTag = $Symbol.toStringTag;
var fnCall = Function.prototype.call;
var uncurry = fnCall.bind.bind(fnCall);  // uncurry(method)(thisValue, ...args)
var wmGet = uncurry($WeakMap.prototype.get), wmSet = uncurry($WeakMap.prototype.set), wmHas = uncurry($WeakMap.prototype.has);
var GeneratorProto = getPrototypeOf((function* () {}).prototype);
var genNext = uncurry(GeneratorProto.next), genReturn = uncurry(GeneratorProto.return);
var setHas = uncurry($Set.prototype.has), setAdd = uncurry($Set.prototype.add), setDelete = uncurry($Set.prototype.delete),
    setValues = uncurry($Set.prototype.values), setSize = uncurry(getOwnPropertyDescriptor($Set.prototype, 'size').get);
var setIterNext = uncurry(getPrototypeOf(new $Set()[symIterator]()).next);
var mapGet = uncurry($Map.prototype.get), mapSet = uncurry($Map.prototype.set), mapHas = uncurry($Map.prototype.has);
var charCodeAt = uncurry($String.prototype.charCodeAt), strSlice = uncurry($String.prototype.slice), numToString = uncurry(Number.prototype.toString);
var fromCharCode = $String.fromCharCode;

function isObject(x) { return (typeof x === 'object' && x !== null) || typeof x === 'function'; }
// Functions defined here are installed the way built-ins are: writable, configurable, not enumerable.
function install(target, source) {
  var keys = ownKeys(source);
  for (var i = 0; i < keys.length; i++) {
    var d = getOwnPropertyDescriptor(source, keys[i]);
    d.enumerable = false;
    defineProperty(target, keys[i], d);
  }
}
function data(target, key, value) { defineProperty(target, key, { value: value, writable: true, enumerable: true, configurable: true }); }

// ---- iterator plumbing (7.4)
function directRecord(iterator) { return { iterator: iterator, next: iterator.next }; }  // GetIteratorDirect
function getIterator(obj) {  // GetIterator(obj, sync)
  var method = obj[symIterator];
  if (method === undefined || method === null) throw new $TypeError('The value is not iterable');
  var iterator = apply(method, obj, []);
  if (!isObject(iterator)) throw new $TypeError('Result of the Symbol.iterator method is not an object');
  return directRecord(iterator);
}
function step(rec) {  // IteratorStep: the result object, or null once done
  var result = apply(rec.next, rec.iterator, []);
  if (!isObject(result)) throw new $TypeError('Iterator result ' + $String(result) + ' is not an object');
  return result.done ? null : result;
}
function close(iterator) {  // IteratorClose after a normal or return completion
  var method = iterator.return;
  if (method === undefined || method === null) return;
  var result = apply(method, iterator, []);
  if (!isObject(result)) throw new $TypeError('Iterator result ' + $String(result) + ' is not an object');
}
function closeQuietly(iterator) {  // IteratorClose after a throw completion: the original error wins
  try {
    var method = iterator.return;
    if (method !== undefined && method !== null) apply(method, iterator, []);
  } catch (e) {}
}

// ---- Iterator, Iterator.prototype and iterator helpers
var IteratorProto = getPrototypeOf(getPrototypeOf([][symIterator]()));
var Iterator = function Iterator() {
  if (new.target === undefined || new.target === Iterator) throw new $TypeError('Iterator is an abstract class and cannot be constructed directly');
};
defineProperty(Iterator, 'prototype', { value: IteratorProto, writable: false, enumerable: false, configurable: false });

// SetterThatIgnoresPrototypeProperties (27.1.4.1.x): assigning on an iterator makes an own property
function ignoringSetter(self, key, value) {
  if (!isObject(self)) throw new $TypeError('Iterator.prototype setter called on a non-object');
  if (self === IteratorProto) throw new $TypeError('Cannot assign to this property of Iterator.prototype');
  if (getOwnPropertyDescriptor(self, key) === undefined) data(self, key, value);
  else self[key] = value;
}
var accessors = {
  get constructor() { return Iterator; },
  set constructor(v) { ignoringSetter(this, 'constructor', v); },
  get [symToStringTag]() { return 'Iterator'; },
  set [symToStringTag](v) { ignoringSetter(this, symToStringTag, v); }
};
install(IteratorProto, accessors);

var helperState = new $WeakMap();  // helper -> { gen, underlying, started }
var HelperProto = create(IteratorProto);
function makeHelper(gen, rec) {
  var helper = create(HelperProto);
  wmSet(helperState, helper, { gen: gen, underlying: rec.iterator, started: false });
  return helper;
}
install(HelperProto, {
  next() {
    var s = wmGet(helperState, this);
    if (s === undefined) throw new $TypeError('next method called on an object that is not an Iterator Helper');
    s.started = true;
    return genNext(s.gen);
  },
  return() {
    var s = wmGet(helperState, this);
    if (s === undefined) throw new $TypeError('return method called on an object that is not an Iterator Helper');
    if (!s.started) {  // never started: finish it and close what it wraps
      s.started = true;
      genReturn(s.gen);
      close(s.underlying);
      return { value: undefined, done: true };
    }
    return genReturn(s.gen);
  }
});
defineProperty(HelperProto, symToStringTag, { value: 'Iterator Helper', writable: false, enumerable: false, configurable: true });

// Each helper's work is a generator. A value is handed out with `yield`; if the consumer stops there
// (return()), the wrapped iterator is closed on the way out.
function* mapGen(rec, mapper) {
  for (var counter = 0; ; counter++) {
    var r = step(rec);
    if (r === null) return;
    var mapped, value = r.value;
    try { mapped = mapper(value, counter); } catch (e) { closeQuietly(rec.iterator); throw e; }
    var resumed = false;
    try { yield mapped; resumed = true; } finally { if (!resumed) close(rec.iterator); }
  }
}
function* filterGen(rec, predicate) {
  for (var counter = 0; ; counter++) {
    var r = step(rec);
    if (r === null) return;
    var selected, value = r.value;
    try { selected = predicate(value, counter); } catch (e) { closeQuietly(rec.iterator); throw e; }
    if (!selected) continue;
    var resumed = false;
    try { yield value; resumed = true; } finally { if (!resumed) close(rec.iterator); }
  }
}
function* takeGen(rec, limit) {
  for (var remaining = limit; ;) {
    if (remaining === 0) { close(rec.iterator); return; }
    if (remaining !== $Infinity) remaining--;
    var r = step(rec);
    if (r === null) return;
    var value = r.value, resumed = false;
    try { yield value; resumed = true; } finally { if (!resumed) close(rec.iterator); }
  }
}
function* dropGen(rec, limit) {
  for (var remaining = limit; remaining > 0;) {
    if (remaining !== $Infinity) remaining--;
    if (step(rec) === null) return;
  }
  for (;;) {
    var r = step(rec);
    if (r === null) return;
    var value = r.value, resumed = false;
    try { yield value; resumed = true; } finally { if (!resumed) close(rec.iterator); }
  }
}
function flattenable(obj) {  // GetIteratorFlattenable(obj, reject-primitives)
  if (!isObject(obj)) throw new $TypeError('The mapper must return an iterable or iterator object');
  var method = obj[symIterator], iterator;
  if (method === undefined || method === null) iterator = obj;
  else iterator = apply(method, obj, []);
  if (!isObject(iterator)) throw new $TypeError('Result of the Symbol.iterator method is not an object');
  return directRecord(iterator);
}
function* flatMapGen(rec, mapper) {
  for (var counter = 0; ; counter++) {
    var r = step(rec);
    if (r === null) return;
    var inner, value = r.value;
    try { inner = flattenable(mapper(value, counter)); } catch (e) { closeQuietly(rec.iterator); throw e; }
    for (;;) {
      var ir;
      try { ir = step(inner); } catch (e) { closeQuietly(rec.iterator); throw e; }
      if (ir === null) break;
      var innerValue;
      try { innerValue = ir.value; } catch (e) { closeQuietly(rec.iterator); throw e; }
      var resumed = false;
      try { yield innerValue; resumed = true; } finally {
        if (!resumed) {
          try { close(inner.iterator); } catch (e) { closeQuietly(rec.iterator); throw e; }
          close(rec.iterator);
        }
      }
    }
  }
}
function needObject(o, name) { if (!isObject(o)) throw new $TypeError('Iterator.prototype.' + name + ' called on a non-object'); }
function needCallable(o, f, what) {
  if (typeof f !== 'function') { closeQuietly(o); throw new $TypeError(what + ' is not a function'); }
}
function toLimit(o, limit) {
  var n;
  try { n = +limit; } catch (e) { closeQuietly(o); throw e; }
  if (n !== n) { closeQuietly(o); throw new $RangeError('The limit must be a number'); }
  n = trunc(n);
  if (n < 0) { closeQuietly(o); throw new $RangeError('The limit must not be negative'); }
  return n;
}
install(IteratorProto, {
  map(mapper) { needObject(this, 'map'); needCallable(this, mapper, 'The mapper'); var rec = directRecord(this); return makeHelper(mapGen(rec, mapper), rec); },
  filter(predicate) { needObject(this, 'filter'); needCallable(this, predicate, 'The predicate'); var rec = directRecord(this); return makeHelper(filterGen(rec, predicate), rec); },
  take(limit) { needObject(this, 'take'); var n = toLimit(this, limit); var rec = directRecord(this); return makeHelper(takeGen(rec, n), rec); },
  drop(limit) { needObject(this, 'drop'); var n = toLimit(this, limit); var rec = directRecord(this); return makeHelper(dropGen(rec, n), rec); },
  flatMap(mapper) { needObject(this, 'flatMap'); needCallable(this, mapper, 'The mapper'); var rec = directRecord(this); return makeHelper(flatMapGen(rec, mapper), rec); },
  reduce(reducer) {
    needObject(this, 'reduce'); needCallable(this, reducer, 'The reducer');
    var rec = directRecord(this), accumulator, counter = 0;
    if (arguments.length < 2) {
      var first = step(rec);
      if (first === null) throw new $TypeError('Reduce of a done iterator with no initial value');
      accumulator = first.value;
      counter = 1;
    } else accumulator = arguments[1];
    for (;; counter++) {
      var r = step(rec);
      if (r === null) return accumulator;
      var value = r.value;
      try { accumulator = reducer(accumulator, value, counter); } catch (e) { closeQuietly(rec.iterator); throw e; }
    }
  },
  toArray() {
    needObject(this, 'toArray');
    var rec = directRecord(this), items = [], n = 0;
    for (;;) {
      var r = step(rec);
      if (r === null) return items;
      data(items, n++, r.value);
    }
  },
  forEach(procedure) {
    needObject(this, 'forEach'); needCallable(this, procedure, 'The callback');
    var rec = directRecord(this);
    for (var counter = 0; ; counter++) {
      var r = step(rec);
      if (r === null) return undefined;
      var value = r.value;
      try { procedure(value, counter); } catch (e) { closeQuietly(rec.iterator); throw e; }
    }
  },
  some(predicate) {
    needObject(this, 'some'); needCallable(this, predicate, 'The predicate');
    var rec = directRecord(this);
    for (var counter = 0; ; counter++) {
      var r = step(rec);
      if (r === null) return false;
      var value = r.value, found;
      try { found = predicate(value, counter); } catch (e) { closeQuietly(rec.iterator); throw e; }
      if (found) { close(rec.iterator); return true; }
    }
  },
  every(predicate) {
    needObject(this, 'every'); needCallable(this, predicate, 'The predicate');
    var rec = directRecord(this);
    for (var counter = 0; ; counter++) {
      var r = step(rec);
      if (r === null) return true;
      var value = r.value, ok;
      try { ok = predicate(value, counter); } catch (e) { closeQuietly(rec.iterator); throw e; }
      if (!ok) { close(rec.iterator); return false; }
    }
  },
  find(predicate) {
    needObject(this, 'find'); needCallable(this, predicate, 'The predicate');
    var rec = directRecord(this);
    for (var counter = 0; ; counter++) {
      var r = step(rec);
      if (r === null) return undefined;
      var value = r.value, found;
      try { found = predicate(value, counter); } catch (e) { closeQuietly(rec.iterator); throw e; }
      if (found) { close(rec.iterator); return value; }
    }
  }
});

// Iterator.from: iterators that do not inherit from Iterator.prototype are wrapped
var wrapped = new $WeakMap();  // wrapper -> record
var WrapProto = create(IteratorProto);
install(WrapProto, {
  next() {
    var rec = wmGet(wrapped, this);
    if (rec === undefined) throw new $TypeError('next method called on an incompatible receiver');
    return apply(rec.next, rec.iterator, []);
  },
  return() {
    var rec = wmGet(wrapped, this);
    if (rec === undefined) throw new $TypeError('return method called on an incompatible receiver');
    var method = rec.iterator.return;
    if (method === undefined || method === null) return { value: undefined, done: true };
    return apply(method, rec.iterator, []);
  }
});
install(Iterator, {
  from(O) {
    var iterator;
    if (typeof O === 'string') iterator = apply(O[symIterator], O, []);
    else {
      if (!isObject(O)) throw new $TypeError('Iterator.from requires an object or a string');
      var method = O[symIterator];
      iterator = method === undefined || method === null ? O : apply(method, O, []);
    }
    if (!isObject(iterator)) throw new $TypeError('Result of the Symbol.iterator method is not an object');
    var rec = directRecord(iterator);
    if (natives.inherits(iterator, IteratorProto)) return iterator;
    var wrapper = create(WrapProto);
    wmSet(wrapped, wrapper, rec);
    return wrapper;
  }
});
defineProperty(globalThis, 'Iterator', { value: Iterator, writable: true, enumerable: false, configurable: true });
)JS"
R"JS(
// ---- Object.groupBy and Map.groupBy (7.3.35 GroupBy)
function groupBy(items, callback, addTo) {
  if (items === undefined || items === null) throw new $TypeError('Cannot group ' + items);
  if (typeof callback !== 'function') throw new $TypeError('The callback is not a function');
  var rec = getIterator(items);
  for (var k = 0; ; k++) {
    var r = step(rec);
    if (r === null) return;
    var value = r.value;
    try { addTo(callback(value, k), value); } catch (e) { closeQuietly(rec.iterator); throw e; }
  }
}
install($Object, {
  groupBy(items, callback) {
    var out = create(null);
    groupBy(items, callback, function (key, value) {
      var holder = {};
      holder[key] = 1;  // ToPropertyKey, once
      key = ownKeys(holder)[0];
      var group = getOwnPropertyDescriptor(out, key);
      if (group === undefined) data(out, key, [value]);
      else data(group.value, group.value.length, value);
    });
    return out;
  }
});
install($Map, {
  groupBy(items, callback) {
    var out = new $Map();
    groupBy(items, callback, function (key, value) {
      if (key === 0) key = 0;  // -0 becomes +0
      var group = mapGet(out, key);
      if (group === undefined && !mapHas(out, key)) mapSet(out, key, [value]);
      else data(group, group.length, value);
    });
    return out;
  }
});

// ---- Set methods (24.2.4.5 onward)
function setRecord(other) {  // GetSetRecord
  if (!isObject(other)) throw new $TypeError('The argument must be a set-like object');
  var size = +other.size;
  if (size !== size) throw new $TypeError("The 'size' of the argument is not a number");
  size = trunc(size);
  if (size < 0) throw new $RangeError("The 'size' of the argument is negative");
  var has = other.has;
  if (typeof has !== 'function') throw new $TypeError("The 'has' of the argument is not a function");
  var keys = other.keys;
  if (typeof keys !== 'function') throw new $TypeError("The 'keys' of the argument is not a function");
  return { set: other, size: size, has: has, keys: keys };
}
function keysOf(rec) {
  var iterator = apply(rec.keys, rec.set, []);
  if (!isObject(iterator)) throw new $TypeError("The 'keys' method did not return an object");
  return directRecord(iterator);
}
function copySet(s) {
  var out = new $Set(), it = setValues(s);
  for (var r = setIterNext(it); !r.done; r = setIterNext(it)) setAdd(out, r.value);
  return out;
}
function eachOwn(s, fn) {  // the elements of a real Set, also ones added while it is walked; fn returns true to stop
  var it = setValues(s);
  for (var r = setIterNext(it); !r.done; r = setIterNext(it)) if (fn(r.value)) return true;
  return false;
}
install($Set.prototype, {
  union(other) {
    var size = setSize(this), rec = setRecord(other), keys = keysOf(rec), out = copySet(this);
    for (var r = step(keys); r !== null; r = step(keys)) setAdd(out, r.value);
    return out;
  },
  intersection(other) {
    var self = this, size = setSize(this), rec = setRecord(other), out = new $Set();
    if (size <= rec.size) {
      eachOwn(self, function (e) { if (apply(rec.has, rec.set, [e])) setAdd(out, e); });
    } else {
      var keys = keysOf(rec);
      for (var r = step(keys); r !== null; r = step(keys)) { var v = r.value; if (setHas(self, v)) setAdd(out, v); }
    }
    return out;
  },
  difference(other) {
    var self = this, size = setSize(this), rec = setRecord(other), out = copySet(this);
    if (size <= rec.size) {
      eachOwn(self, function (e) { if (apply(rec.has, rec.set, [e])) setDelete(out, e); });
    } else {
      var keys = keysOf(rec);
      for (var r = step(keys); r !== null; r = step(keys)) setDelete(out, r.value);
    }
    return out;
  },
  symmetricDifference(other) {
    var size = setSize(this), rec = setRecord(other), keys = keysOf(rec), out = copySet(this);
    for (var r = step(keys); r !== null; r = step(keys)) {
      var v = r.value;
      if (setHas(this, v)) setDelete(out, v);
      else setAdd(out, v);
    }
    return out;
  },
  isSubsetOf(other) {
    var size = setSize(this), rec = setRecord(other);
    if (size > rec.size) return false;
    return !eachOwn(this, function (e) { return !apply(rec.has, rec.set, [e]); });
  },
  isSupersetOf(other) {
    var size = setSize(this), rec = setRecord(other);
    if (size < rec.size) return false;
    var keys = keysOf(rec);
    for (var r = step(keys); r !== null; r = step(keys)) if (!setHas(this, r.value)) { close(keys.iterator); return false; }
    return true;
  },
  isDisjointFrom(other) {
    var size = setSize(this), rec = setRecord(other);
    if (size <= rec.size) return !eachOwn(this, function (e) { return !!apply(rec.has, rec.set, [e]); });
    var keys = keysOf(rec);
    for (var r = step(keys); r !== null; r = step(keys)) if (setHas(this, r.value)) { close(keys.iterator); return false; }
    return true;
  }
});

)JS"
R"JS(
// ---- Promise: the combinators and finally (27.2.4, 27.2.5.3)
var newCapability = natives.newPromiseCapability, promiseResolve = natives.promiseResolve, $AggregateError = AggregateError;
var DONE = {};
// IteratorStepValue: rec.done records that the iterator finished or failed, after which it must not be closed
function stepValue(rec) {
  try {
    var result = apply(rec.next, rec.iterator, []);
    if (!isObject(result)) throw new $TypeError('Iterator result ' + $String(result) + ' is not an object');
    if (result.done) {
      rec.done = true;
      return DONE;
    }
    return result.value;
  } catch (e) {
    rec.done = true;
    throw e;
  }
}
function listToArray(list) {  // CreateArrayFromList
  var out = [];
  for (var i = 0; i < list.length; i++) data(out, i, list[i]);
  return out;
}
function invokeThen(promise, onFulfilled, onRejected) { return apply(promise.then, promise, [onFulfilled, onRejected]); }
function combine(C, iterable, perform) {
  var cap = newCapability(C), promiseResolveFn, rec;
  try {
    promiseResolveFn = C.resolve;  // GetPromiseResolve
    if (typeof promiseResolveFn !== 'function') throw new $TypeError('The promise constructor has no resolve method');
    rec = getIterator(iterable);
    rec.done = false;
  } catch (e) {
    (0, cap.reject)(e);
    return cap.promise;
  }
  try {
    return perform(rec, C, cap, promiseResolveFn);
  } catch (e) {
    if (!rec.done) closeQuietly(rec.iterator);
    (0, cap.reject)(e);
    return cap.promise;
  }
}
// The functions handed to then() for one element: each pair runs at most once between them.
function elementFunction(state, index, called, wrap, finish) {
  return (x) => {
    if (called.value) return undefined;
    called.value = true;
    data(state.values, index, wrap(x));
    if (--state.remaining === 0) return finish(state);
    return undefined;
  };
}
function same(x) { return x; }
function fulfilledRecord(x) { return { status: 'fulfilled', value: x }; }
function rejectedRecord(x) { return { status: 'rejected', reason: x }; }
function aggregate(errors) {
  var error = new $AggregateError([], 'All promises were rejected');
  defineProperty(error, 'errors', { value: listToArray(errors), writable: true, enumerable: false, configurable: true });
  return error;
}
function performCombinator(kind) {  // 0 all, 1 allSettled, 2 any
  return function (rec, C, cap, promiseResolveFn) {
    var state = { values: [], remaining: 1 }, index = 0;
    var finish = kind === 2 ? (s) => (0, cap.reject)(aggregate(s.values)) : (s) => (0, cap.resolve)(listToArray(s.values));
    for (;;) {
      var next = stepValue(rec);
      if (next === DONE) {
        if (--state.remaining === 0) {
          if (kind === 2) throw aggregate(state.values);
          (0, cap.resolve)(listToArray(state.values));
        }
        return cap.promise;
      }
      data(state.values, index, undefined);
      var nextPromise = apply(promiseResolveFn, C, [next]);
      var called = { value: false };
      var onFulfilled = kind === 2 ? cap.resolve : elementFunction(state, index, called, kind === 1 ? fulfilledRecord : same, finish);
      var onRejected = kind === 0 ? cap.reject : elementFunction(state, index, kind === 1 ? called : { value: false }, kind === 1 ? rejectedRecord : same, finish);
      state.remaining++;
      invokeThen(nextPromise, onFulfilled, onRejected);
      index++;
    }
  };
}
var performAll = performCombinator(0), performAllSettled = performCombinator(1), performAny = performCombinator(2);
function performRace(rec, C, cap, promiseResolveFn) {
  for (;;) {
    var next = stepValue(rec);
    if (next === DONE) return cap.promise;
    invokeThen(apply(promiseResolveFn, C, [next]), cap.resolve, cap.reject);
  }
}
// Promise.allKeyed and Promise.allSettledKeyed (the await dictionary proposal)
function keyedResult(state) {
  var obj = create(null);
  for (var i = 0; i < state.keys.length; i++) data(obj, state.keys[i], state.values[i]);
  return obj;
}
function combineKeyed(C, promises, settled) {
  var cap = newCapability(C);
  try {
    var promiseResolveFn = C.resolve;
    if (typeof promiseResolveFn !== 'function') throw new $TypeError('The promise constructor has no resolve method');
    if (!isObject(promises)) throw new $TypeError('The argument must be an object');
    var allKeys = ownKeys(promises), state = { keys: [], values: [], remaining: 1 }, index = 0;
    var finish = (s) => (0, cap.resolve)(keyedResult(s));
    for (var i = 0; i < allKeys.length; i++) {
      var key = allKeys[i], desc = getOwnPropertyDescriptor(promises, key);
      if (desc === undefined || !desc.enumerable) continue;
      var value = promises[key];
      data(state.keys, index, key);
      data(state.values, index, undefined);
      var nextPromise = apply(promiseResolveFn, C, [value]);
      var called = { value: false };
      var onFulfilled = elementFunction(state, index, called, settled ? fulfilledRecord : same, finish);
      var onRejected = settled ? elementFunction(state, index, called, rejectedRecord, finish) : cap.reject;
      state.remaining++;
      invokeThen(nextPromise, onFulfilled, onRejected);
      index++;
    }
    if (--state.remaining === 0) (0, cap.resolve)(keyedResult(state));
  } catch (e) {
    (0, cap.reject)(e);
  }
  return cap.promise;
}
install($Promise, {
  all(iterable) { return combine(this, iterable, performAll); },
  allSettled(iterable) { return combine(this, iterable, performAllSettled); },
  any(iterable) { return combine(this, iterable, performAny); },
  race(iterable) { return combine(this, iterable, performRace); },
  allKeyed(promises) { return combineKeyed(this, promises, false); },
  allSettledKeyed(promises) { return combineKeyed(this, promises, true); },
  try(callback, ...args) {
    var C = this;
    if (!isObject(C)) throw new $TypeError('Promise.try called on a non-object');
    var value;
    try {
      value = apply(callback, undefined, args);
    } catch (e) {
      var cap = newCapability(C);
      (0, cap.reject)(e);
      return cap.promise;
    }
    return promiseResolve(C, value);
  }
});
install($Promise.prototype, {
  finally(onFinally) {
    var promise = this;
    if (!isObject(promise)) throw new $TypeError('Promise.prototype.finally called on a non-object');
    var C = natives.speciesConstructor(promise, $Promise);
    if (typeof onFinally !== 'function') return invokeThen(promise, onFinally, onFinally);
    return invokeThen(promise, (value) => {
      var p = promiseResolve(C, onFinally());
      return apply(p.then, p, [() => value]);
    }, (reason) => {
      var p = promiseResolve(C, onFinally());
      return apply(p.then, p, [() => { throw reason; }]);
    });
  }
});

)JS"
R"JS(
// ---- Array.fromAsync (2.1.1.1 of the proposal, now 23.1.2.2)
install(Array, {
  fromAsync(asyncItems) {
    var C = this, mapper = arguments[1], thisArg = arguments[2];
    return (async function () {
      var mapping = mapper !== undefined;
      if (mapping && typeof mapper !== 'function') throw new $TypeError('The mapper is not a function');
      var usingAsync = asyncItems[symAsyncIterator], usingSync;
      if (usingAsync === undefined || usingAsync === null) {
        usingAsync = undefined;
        usingSync = asyncItems[symIterator];
        if (usingSync === null) usingSync = undefined;
      }
      var rec;
      if (usingAsync !== undefined) {
        var asyncIterator = apply(usingAsync, asyncItems, []);
        if (!isObject(asyncIterator)) throw new $TypeError('Result of the Symbol.asyncIterator method is not an object');
        rec = directRecord(asyncIterator);
      } else if (usingSync !== undefined) {
        var syncIterator = apply(usingSync, asyncItems, []);
        if (!isObject(syncIterator)) throw new $TypeError('Result of the Symbol.iterator method is not an object');
        rec = directRecord(natives.asyncFromSync(syncIterator));
      }
      var A, k = 0;
      if (rec !== undefined) {
        A = natives.isConstructor(C) ? new C() : [];
        for (;; k++) {
          var result = await apply(rec.next, rec.iterator, []);
          if (!isObject(result)) throw new $TypeError('Iterator result is not an object');
          if (result.done) { A.length = k; return A; }
          var value = result.value;
          try {
            if (mapping) value = await apply(mapper, thisArg, [value, k]);
            data(A, k, value);
          } catch (e) {
            try { var ret = rec.iterator.return; if (ret !== undefined && ret !== null) await apply(ret, rec.iterator, []); } catch (ignored) {}
            throw e;
          }
        }
      }
      var arrayLike = $Object(asyncItems), len = natives.toLength(arrayLike.length);
      A = natives.isConstructor(C) ? new C(len) : natives.arrayCreate(len);
      for (; k < len; k++) {
        var item = await arrayLike[k];
        if (mapping) item = await apply(mapper, thisArg, [item, k]);
        data(A, k, item);
      }
      A.length = len;
      return A;
    })();
  }
});

// ---- strings: well-formed UTF-16 (22.1.3.10, 22.1.3.31), RegExp.escape (22.2.5.1), Error.isError
function coerceString(self, name) {
  if (self === undefined || self === null) throw new $TypeError('String.prototype.' + name + ' called on null or undefined');
  return natives.toString(self);
}
install($String.prototype, {
  isWellFormed() {
    var s = coerceString(this, 'isWellFormed');
    for (var i = 0; i < s.length; i++) {
      var c = charCodeAt(s, i);
      if (c >= 0xD800 && c <= 0xDBFF) {
        var d = i + 1 < s.length ? charCodeAt(s, i + 1) : 0;
        if (d < 0xDC00 || d > 0xDFFF) return false;
        i++;
      } else if (c >= 0xDC00 && c <= 0xDFFF) return false;
    }
    return true;
  },
  toWellFormed() {
    var s = coerceString(this, 'toWellFormed'), out = '', from = 0;
    for (var i = 0; i < s.length; i++) {
      var c = charCodeAt(s, i), lone = false;
      if (c >= 0xD800 && c <= 0xDBFF) {
        var d = i + 1 < s.length ? charCodeAt(s, i + 1) : 0;
        if (d < 0xDC00 || d > 0xDFFF) lone = true;
        else i++;
      } else if (c >= 0xDC00 && c <= 0xDFFF) lone = true;
      if (lone) { out += strSlice(s, from, i) + '�'; from = i + 1; }
    }
    return out + strSlice(s, from);
  }
});
function hex(c, width) { var h = numToString(c, 16); while (h.length < width) h = '0' + h; return h; }
install(RegExp, {
  escape(S) {
    if (typeof S !== 'string') throw new $TypeError('RegExp.escape requires a string');
    var out = '';
    for (var i = 0; i < S.length; i++) {
      var c = charCodeAt(S, i), ch = S[i];
      if (i === 0 && ((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122))) out += '\\x' + hex(c, 2);
      else if (c === 94 || c === 36 || c === 92 || c === 46 || c === 42 || c === 43 || c === 63 || c === 40 || c === 41 || c === 91 || c === 93 ||
               c === 123 || c === 125 || c === 124 || c === 47) out += '\\' + ch;
      else if (c === 9) out += '\\t';
      else if (c === 10) out += '\\n';
      else if (c === 11) out += '\\v';
      else if (c === 12) out += '\\f';
      else if (c === 13) out += '\\r';
      else if (c === 44 || c === 45 || c === 61 || c === 60 || c === 62 || c === 35 || c === 38 || c === 33 || c === 37 || c === 58 || c === 59 ||
               c === 64 || c === 126 || c === 39 || c === 96 || c === 34 || c === 32) out += '\\x' + hex(c, 2);
      else if (c === 0xA0 || c === 0x1680 || (c >= 0x2000 && c <= 0x200A) || c === 0x2028 || c === 0x2029 || c === 0x202F || c === 0x205F ||
               c === 0x3000 || c === 0xFEFF) out += c <= 0xFF ? '\\x' + hex(c, 2) : '\\u' + hex(c, 4);
      else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < S.length && charCodeAt(S, i + 1) >= 0xDC00 && charCodeAt(S, i + 1) <= 0xDFFF) { out += ch + S[i + 1]; i++; }
      else if (c >= 0xD800 && c <= 0xDFFF) out += '\\u' + hex(c, 4);
      else out += ch;
    }
    return out;
  }
});
install(Error, { isError(arg) { return natives.isError(arg); } });
)JS"
R"JS(
// ---- Explicit resource management: SuppressedError, DisposableStack, AsyncDisposableStack
var symDispose = $Symbol.dispose, symAsyncDispose = $Symbol.asyncDispose, $Error = Error, construct = Reflect.construct;
var sameValue = $Object.is;
function hidden(target, key, value) { defineProperty(target, key, { value: value, writable: true, enumerable: false, configurable: true }); }
function fixedPrototype(ctor, proto) {
  defineProperty(ctor, 'prototype', { value: proto, writable: false, enumerable: false, configurable: false });
  hidden(proto, 'constructor', ctor);
}
function protoFor(newTarget, fallback) {  // GetPrototypeFromConstructor
  var proto = newTarget.prototype;
  return isObject(proto) ? proto : fallback;
}
var SuppressedError = function SuppressedError(error, suppressed, message) {
  var proto = new.target === undefined ? SuppressedErrorProto : protoFor(new.target, SuppressedErrorProto);
  var O = construct($Error, [], SuppressedError);
  if (proto !== SuppressedErrorProto) $Object.setPrototypeOf(O, proto);
  if (message !== undefined) hidden(O, 'message', natives.toString(message));
  hidden(O, 'error', error);
  hidden(O, 'suppressed', suppressed);
  return O;
};
$Object.setPrototypeOf(SuppressedError, $Error);
var SuppressedErrorProto = create($Error.prototype);
fixedPrototype(SuppressedError, SuppressedErrorProto);
hidden(SuppressedErrorProto, 'message', '');
hidden(SuppressedErrorProto, 'name', 'SuppressedError');
hidden(globalThis, 'SuppressedError', SuppressedError);

function getMethod(V, key) {
  var m = V[key];
  if (m === undefined || m === null) return undefined;
  if (typeof m !== 'function') throw new $TypeError('The dispose method is not a function');
  return m;
}
// CreateDisposableResource: { value, method, async, fallback }; fallback: a sync dispose method used by an async stack
function createResource(V, async) {
  if (V === undefined || V === null) return { value: undefined, method: undefined, async: async, fallback: false };
  if (!isObject(V)) throw new $TypeError('Only an object, null or undefined can be used as a resource');
  var method, fallback = false;
  if (async) {
    method = getMethod(V, symAsyncDispose);
    if (method === undefined) {
      method = getMethod(V, symDispose);
      fallback = true;
    }
  } else {
    method = getMethod(V, symDispose);
  }
  if (method === undefined) throw new $TypeError('The resource has no dispose method');
  return { value: V, method: method, async: async, fallback: fallback };
}
function suppress(completion, error) {  // a later failure wraps the earlier one
  if (!completion.threw) return { threw: true, value: error };
  var wrapped = construct($Error, [], SuppressedError);
  hidden(wrapped, 'error', error);
  hidden(wrapped, 'suppressed', completion.value);
  return { threw: true, value: wrapped };
}
function disposeResources(list, completion) {  // DisposeResources for sync resources only
  for (var i = list.length - 1; i >= 0; i--) {
    var r = list[i];
    try { apply(r.method, r.value, []); } catch (e) { completion = suppress(completion, e); }
  }
  return completion;
}
function makeStack(async) {
  var states = new $WeakMap(), name = async ? 'AsyncDisposableStack' : 'DisposableStack';
  function state(stack) {
    var s = isObject(stack) ? wmGet(states, stack) : undefined;
    if (s === undefined) throw new $TypeError('The receiver is not ' + (async ? 'an ' : 'a ') + name);
    return s;
  }
  function open(stack) {
    var s = state(stack);
    if (s.disposed) throw new ReferenceError('The ' + name + ' has already been disposed');
    return s;
  }
  var proto = {}, Stack;
  if (async) {
    Stack = function AsyncDisposableStack() {
      if (new.target === undefined) throw new $TypeError("Constructor AsyncDisposableStack requires 'new'");
      var stack = create(protoFor(new.target, proto));
      wmSet(states, stack, { disposed: false, list: [] });
      return stack;
    };
  } else {
    Stack = function DisposableStack() {
      if (new.target === undefined) throw new $TypeError("Constructor DisposableStack requires 'new'");
      var stack = create(protoFor(new.target, proto));
      wmSet(states, stack, { disposed: false, list: [] });
      return stack;
    };
  }
  fixedPrototype(Stack, proto);
  install(proto, {
    adopt(value, onDispose) {
      var s = open(this);
      if (typeof onDispose !== 'function') throw new $TypeError('The second argument must be a function');
      data(s.list, s.list.length, { value: undefined, method: () => onDispose(value), async: async, fallback: false });
      return value;
    },
    defer(onDispose) {
      var s = open(this);
      if (typeof onDispose !== 'function') throw new $TypeError('The argument must be a function');
      data(s.list, s.list.length, { value: undefined, method: onDispose, async: async, fallback: false });
    },
    get disposed() { return state(this).disposed; },
    move() {
      var s = open(this), moved = create(proto);
      wmSet(states, moved, { disposed: false, list: s.list });
      s.list = [];
      s.disposed = true;
      return moved;
    },
    use(value) {
      var s = open(this);
      if (!async && (value === undefined || value === null)) return value;
      data(s.list, s.list.length, createResource(value, async));
      return value;
    }
  });
  if (async) {
    var run = async function (stack) {  // DisposeResources, with its awaits; settles in the turn the last step finishes
      var s = state(stack);
      if (s.disposed) return undefined;
      s.disposed = true;
      var list = s.list, completion = { threw: false, value: undefined }, needsAwait = false, hasAwaited = false;
      s.list = [];
      for (var i = list.length - 1; i >= 0; i--) {
        var r = list[i];
        if (r.method === undefined) {
          needsAwait = true;
          continue;
        }
        try {
          if (r.fallback) {  // a sync dispose method: its result is not awaited, and a failure arrives a turn later
            var failed = false, failure;
            try { apply(r.method, r.value, []); } catch (e) { failed = true; failure = e; }
            hasAwaited = true;
            await undefined;
            if (failed) throw failure;
          } else {
            var result = apply(r.method, r.value, []);
            hasAwaited = true;
            await result;
          }
        } catch (e) {
          completion = suppress(completion, e);
        }
      }
      if (needsAwait && !hasAwaited) await undefined;
      if (completion.threw) throw completion.value;
      return undefined;
    };
    install(proto, { disposeAsync() { return run(this); } });
    hidden(proto, symAsyncDispose, proto.disposeAsync);
  } else {
    install(proto, {
      dispose() {
        var s = state(this);
        if (s.disposed) return undefined;
        s.disposed = true;
        var list = s.list;
        s.list = [];
        var completion = disposeResources(list, { threw: false, value: undefined });
        if (completion.threw) throw completion.value;
        return undefined;
      }
    });
    hidden(proto, symDispose, proto.dispose);
  }
  defineProperty(proto, symToStringTag, { value: name, writable: false, enumerable: false, configurable: true });
  hidden(globalThis, name, Stack);
}
makeStack(false);
makeStack(true);
install(IteratorProto, {
  [symDispose]() {
    var ret = getMethod(this, 'return');
    if (ret !== undefined) apply(ret, this, []);
  }
});
install(getPrototypeOf(getPrototypeOf((async function* () {}).prototype)), {
  [symAsyncDispose]() {
    var O = this, cap = natives.newPromiseCapability($Promise);
    try {
      var ret = getMethod(O, 'return');
      if (ret === undefined) {
        (0, cap.resolve)(undefined);
      } else {
        var wrapper = natives.promiseResolve($Promise, apply(ret, O, []));
        natives.promiseThen(wrapper, (0, (value) => undefined), undefined, cap.resolve, cap.reject);
      }
    } catch (e) {
      (0, cap.reject)(e);
    }
    return cap.promise;
  }
});
)JS"
R"JS(
// ---- WeakRef and FinalizationRegistry (26.1, 26.2). Targets are held strongly for now, which the spec allows:
// an implementation never has to collect anything, so cleanup callbacks simply never run.
var keyFor = $Symbol.keyFor;
function canBeHeldWeakly(v) { return isObject(v) || (typeof v === 'symbol' && keyFor(v) === undefined); }
var weakRefTargets = new $WeakMap(), registries = new $WeakMap();
var WeakRefProto = {}, RegistryProto = {};
var WeakRef = function WeakRef(target) {
  if (new.target === undefined) throw new $TypeError("Constructor WeakRef requires 'new'");
  if (!canBeHeldWeakly(target)) throw new $TypeError('WeakRef: the target must be an object or a non-registered symbol');
  var ref = create(protoFor(new.target, WeakRefProto));
  wmSet(weakRefTargets, ref, { target: target });
  return ref;
};
fixedPrototype(WeakRef, WeakRefProto);
install(WeakRefProto, {
  deref() {
    var s = isObject(this) ? wmGet(weakRefTargets, this) : undefined;
    if (s === undefined) throw new $TypeError('The receiver is not a WeakRef');
    return s.target;
  }
});
defineProperty(WeakRefProto, symToStringTag, { value: 'WeakRef', writable: false, enumerable: false, configurable: true });
hidden(globalThis, 'WeakRef', WeakRef);
var FinalizationRegistry = function FinalizationRegistry(cleanupCallback) {
  if (new.target === undefined) throw new $TypeError("Constructor FinalizationRegistry requires 'new'");
  if (typeof cleanupCallback !== 'function') throw new $TypeError('The cleanup callback must be a function');
  var registry = create(protoFor(new.target, RegistryProto));
  wmSet(registries, registry, { cells: [] });
  return registry;
};
fixedPrototype(FinalizationRegistry, RegistryProto);
function registryState(registry) {
  var s = isObject(registry) ? wmGet(registries, registry) : undefined;
  if (s === undefined) throw new $TypeError('The receiver is not a FinalizationRegistry');
  return s;
}
install(RegistryProto, {
  register(target, heldValue) {
    var s = registryState(this), unregisterToken = arguments[2];
    if (!canBeHeldWeakly(target)) throw new $TypeError('The target must be an object or a non-registered symbol');
    if (sameValue(target, heldValue)) throw new $TypeError('The held value must not be the target');
    if (!canBeHeldWeakly(unregisterToken)) {
      if (unregisterToken !== undefined) throw new $TypeError('The unregister token must be an object or a non-registered symbol');
    } else {
      data(s.cells, s.cells.length, unregisterToken);
    }
  },
  unregister(unregisterToken) {
    var s = registryState(this);
    if (!canBeHeldWeakly(unregisterToken)) throw new $TypeError('The unregister token must be an object or a non-registered symbol');
    var kept = [], removed = false;
    for (var i = 0; i < s.cells.length; i++) {
      if (s.cells[i] === unregisterToken) removed = true;
      else data(kept, kept.length, s.cells[i]);
    }
    s.cells = kept;
    return removed;
  }
});
defineProperty(RegistryProto, symToStringTag, { value: 'FinalizationRegistry', writable: false, enumerable: false, configurable: true });
hidden(globalThis, 'FinalizationRegistry', FinalizationRegistry);

// ---- Object.prototype: the legacy accessor methods of Annex B (B.2.2)
function thisObject(self) {
  if (self === undefined || self === null) throw new $TypeError('Cannot convert undefined or null to object');
  return $Object(self);
}
function lookupAccessor(self, P, which) {
  var O = thisObject(self), key = natives.toPropertyKey(P);
  while (O !== null) {
    var desc = getOwnPropertyDescriptor(O, key);
    if (desc !== undefined) return 'value' in desc ? undefined : desc[which];
    O = getPrototypeOf(O);
  }
  return undefined;
}
install($Object.prototype, {
  __defineGetter__(P, getter) {
    var O = thisObject(this);
    if (typeof getter !== 'function') throw new $TypeError('The getter must be a function');
    defineProperty(O, P, { get: getter, enumerable: true, configurable: true });
  },
  __defineSetter__(P, setter) {
    var O = thisObject(this);
    if (typeof setter !== 'function') throw new $TypeError('The setter must be a function');
    defineProperty(O, P, { set: setter, enumerable: true, configurable: true });
  },
  __lookupGetter__(P) { return lookupAccessor(this, P, 'get'); },
  __lookupSetter__(P) { return lookupAccessor(this, P, 'set'); }
});

// ---- String.prototype: the HTML methods of Annex B (B.2.2)
function createHTML(string, tag, attribute, value) {
  if (string === undefined || string === null) throw new $TypeError('String.prototype method called on null or undefined');
  var S = natives.toString(string), open = '<' + tag;
  if (attribute !== '') {
    var V = natives.toString(value), escaped = '';
    for (var i = 0; i < V.length; i++) escaped += charCodeAt(V, i) === 34 ? '&quot;' : strSlice(V, i, i + 1);
    open += ' ' + attribute + '="' + escaped + '"';
  }
  return open + '>' + S + '</' + tag + '>';
}
install($String.prototype, {
  anchor(name) { return createHTML(this, 'a', 'name', name); },
  big() { return createHTML(this, 'big', '', ''); },
  blink() { return createHTML(this, 'blink', '', ''); },
  bold() { return createHTML(this, 'b', '', ''); },
  fixed() { return createHTML(this, 'tt', '', ''); },
  fontcolor(color) { return createHTML(this, 'font', 'color', color); },
  fontsize(size) { return createHTML(this, 'font', 'size', size); },
  italics() { return createHTML(this, 'i', '', ''); },
  link(url) { return createHTML(this, 'a', 'href', url); },
  small() { return createHTML(this, 'small', '', ''); },
  strike() { return createHTML(this, 'strike', '', ''); },
  sub() { return createHTML(this, 'sub', '', ''); },
  sup() { return createHTML(this, 'sup', '', ''); }
});

// ---- RegExp.prototype: the methods behind String's match, matchAll, replace, search and split (22.2.6)
var $RegExp = RegExp, RegExpProto = $RegExp.prototype, builtinExec = natives.regexpBuiltinExec, toStr = natives.toString,
    toInt = natives.toInteger, toLength = natives.toLength, strIndexOf = uncurry($String.prototype.indexOf);
var symMatch = $Symbol.match, symMatchAll = $Symbol.matchAll, symReplace = $Symbol.replace, symSearch = $Symbol.search, symSplit = $Symbol.split;
function regExpExec(R, S) {  // RegExpExec (22.2.7.1)
  var exec = R.exec;
  if (typeof exec === 'function') {
    var result = apply(exec, R, [S]);
    if (result !== null && !isObject(result)) throw new $TypeError('The exec method returned something that is neither an object nor null');
    return result;
  }
  return builtinExec(R, S);
}
function advanceStringIndex(S, index, unicode) {
  if (!unicode || index + 1 >= S.length) return index + 1;
  var lead = charCodeAt(S, index);
  if (lead < 0xD800 || lead > 0xDBFF) return index + 1;
  var trail = charCodeAt(S, index + 1);
  return trail >= 0xDC00 && trail <= 0xDFFF ? index + 2 : index + 1;
}
function hasFlag(flags, letter) { return strIndexOf(flags, letter) !== -1; }
function thisRegExpObject(R, name) {
  if (!isObject(R)) throw new $TypeError('RegExp.prototype[' + name + '] called on a non-object');
  return R;
}
function getSubstitution(matched, str, position, captures, namedCaptures, template) {  // 22.1.3.19.1
  var stringLength = str.length, n = template.length, result = '', i = 0;
  while (i < n) {
    var c = charCodeAt(template, i), d = i + 1 < n ? charCodeAt(template, i + 1) : -1;
    if (c !== 36 || d === -1) {
      result += strSlice(template, i, i + 1);
      i++;
    } else if (d === 36) {
      result += '$';
      i += 2;
    } else if (d === 96) {
      result += strSlice(str, 0, position);
      i += 2;
    } else if (d === 38) {
      result += matched;
      i += 2;
    } else if (d === 39) {
      var tail = position + matched.length;
      result += strSlice(str, tail < stringLength ? tail : stringLength);
      i += 2;
    } else if (d >= 48 && d <= 57) {
      var e = i + 2 < n ? charCodeAt(template, i + 2) : -1;
      var digitCount = e >= 48 && e <= 57 ? 2 : 1;
      var index = digitCount === 2 ? (d - 48) * 10 + (e - 48) : d - 48;
      if (index > captures.length && digitCount === 2) {
        digitCount = 1;
        index = d - 48;
      }
      if (index >= 1 && index <= captures.length) {
        var capture = captures[index - 1];
        if (capture !== undefined) result += capture;
      } else {
        result += strSlice(template, i, i + 1 + digitCount);
      }
      i += 1 + digitCount;
    } else if (d === 60) {
      var gt = strIndexOf(template, '>', i);
      if (gt === -1 || namedCaptures === undefined) {
        result += '$<';
        i += 2;
      } else {
        var named = namedCaptures[strSlice(template, i + 2, gt)];
        if (named !== undefined) result += toStr(named);
        i = gt + 1;
      }
    } else {
      result += '$';
      i++;
    }
  }
  return result;
}
var regExpIterState = new $WeakMap();
var RegExpStringIteratorProto = create(IteratorProto);
install(RegExpStringIteratorProto, {
  next() {
    var s = isObject(this) ? wmGet(regExpIterState, this) : undefined;
    if (s === undefined) throw new $TypeError('next method called on an object that is not a RegExp String Iterator');
    if (s.done) return { value: undefined, done: true };
    var match = regExpExec(s.regexp, s.string);
    if (match === null) {
      s.done = true;
      return { value: undefined, done: true };
    }
    if (!s.global) {
      s.done = true;
      return { value: match, done: false };
    }
    if (toStr(match[0]) === '') s.regexp.lastIndex = advanceStringIndex(s.string, toLength(s.regexp.lastIndex), s.unicode);
    return { value: match, done: false };
  }
});
defineProperty(RegExpStringIteratorProto, symToStringTag, { value: 'RegExp String Iterator', writable: false, enumerable: false, configurable: true });
)JS"
R"JS(
install(RegExpProto, {
  [symMatch](string) {
    var rx = thisRegExpObject(this, 'Symbol.match'), S = toStr(string), flags = toStr(rx.flags);
    if (!hasFlag(flags, 'g')) return regExpExec(rx, S);
    var fullUnicode = hasFlag(flags, 'u') || hasFlag(flags, 'v');
    rx.lastIndex = 0;
    var A = [], n = 0;
    for (;;) {
      var result = regExpExec(rx, S);
      if (result === null) return n === 0 ? null : A;
      var matchStr = toStr(result[0]);
      data(A, n++, matchStr);
      if (matchStr === '') rx.lastIndex = advanceStringIndex(S, toLength(rx.lastIndex), fullUnicode);
    }
  },
  [symMatchAll](string) {
    var R = thisRegExpObject(this, 'Symbol.matchAll'), S = toStr(string);
    var C = natives.speciesConstructor(R, $RegExp), flags = toStr(R.flags);
    var matcher = new C(R, flags);
    matcher.lastIndex = toLength(R.lastIndex);
    var iterator = create(RegExpStringIteratorProto);
    wmSet(regExpIterState, iterator, { regexp: matcher, string: S, global: hasFlag(flags, 'g'), unicode: hasFlag(flags, 'u') || hasFlag(flags, 'v'), done: false });
    return iterator;
  },
  [symReplace](string, replaceValue) {
    var rx = thisRegExpObject(this, 'Symbol.replace'), S = toStr(string), lengthS = S.length;
    var functional = typeof replaceValue === 'function';
    if (!functional) replaceValue = toStr(replaceValue);
    var flags = toStr(rx.flags), global = hasFlag(flags, 'g');
    if (global) rx.lastIndex = 0;
    var results = [], count = 0, result;
    for (;;) {
      result = regExpExec(rx, S);
      if (result === null) break;
      data(results, count++, result);
      if (!global) break;
      if (toStr(result[0]) === '') {
        var thisIndex = toLength(rx.lastIndex);
        rx.lastIndex = advanceStringIndex(S, thisIndex, hasFlag(flags, 'u') || hasFlag(flags, 'v'));
      }
    }
    var accumulated = '', nextSourcePosition = 0;
    for (var i = 0; i < count; i++) {
      result = results[i];
      var nCaptures = toLength(result.length) - 1;
      if (nCaptures < 0) nCaptures = 0;
      var matched = toStr(result[0]), position = toInt(result.index);
      position = position < 0 ? 0 : position > lengthS ? lengthS : position;
      var captures = [];
      for (var n = 1; n <= nCaptures; n++) {
        var capture = result[n];
        if (capture !== undefined) capture = toStr(capture);
        data(captures, n - 1, capture);
      }
      var namedCaptures = result.groups, replacement;
      if (functional) {
        var args = [matched], argc = 1;
        for (n = 0; n < nCaptures; n++) data(args, argc++, captures[n]);
        data(args, argc++, position);
        data(args, argc++, S);
        if (namedCaptures !== undefined) data(args, argc++, namedCaptures);
        replacement = toStr(apply(replaceValue, undefined, args));
      } else {
        if (namedCaptures !== undefined) {
          if (namedCaptures === null) throw new $TypeError('Cannot convert null to object');
          namedCaptures = $Object(namedCaptures);
        }
        replacement = getSubstitution(matched, S, position, captures, namedCaptures, replaceValue);
      }
      if (position >= nextSourcePosition) {
        accumulated += strSlice(S, nextSourcePosition, position) + replacement;
        nextSourcePosition = position + matched.length;
      }
    }
    return nextSourcePosition >= lengthS ? accumulated : accumulated + strSlice(S, nextSourcePosition);
  },
  [symSearch](string) {
    var rx = thisRegExpObject(this, 'Symbol.search'), S = toStr(string);
    var previous = rx.lastIndex;
    if (!sameValue(previous, 0)) rx.lastIndex = 0;
    var result = regExpExec(rx, S), current = rx.lastIndex;
    if (!sameValue(current, previous)) rx.lastIndex = previous;
    return result === null ? -1 : result.index;
  },
  [symSplit](string, limit) {
    var rx = thisRegExpObject(this, 'Symbol.split'), S = toStr(string);
    var C = natives.speciesConstructor(rx, $RegExp), flags = toStr(rx.flags);
    var unicodeMatching = hasFlag(flags, 'u') || hasFlag(flags, 'v');
    var splitter = new C(rx, hasFlag(flags, 'y') ? flags : flags + 'y');
    var A = [], lengthA = 0, lim = limit === undefined ? 4294967295 : limit >>> 0;
    if (lim === 0) return A;
    var size = S.length;
    if (size === 0) {
      if (regExpExec(splitter, S) === null) data(A, 0, S);
      return A;
    }
    var p = 0, q = 0;
    while (q < size) {
      splitter.lastIndex = q;
      var z = regExpExec(splitter, S);
      if (z === null) {
        q = advanceStringIndex(S, q, unicodeMatching);
        continue;
      }
      var e = toLength(splitter.lastIndex);
      if (e > size) e = size;
      if (e === p) {
        q = advanceStringIndex(S, q, unicodeMatching);
        continue;
      }
      data(A, lengthA++, strSlice(S, p, q));
      if (lengthA === lim) return A;
      p = e;
      var numberOfCaptures = toLength(z.length) - 1;
      for (var i = 1; i <= numberOfCaptures; i++) {
        data(A, lengthA++, z[i]);
        if (lengthA === lim) return A;
      }
      q = p;
    }
    data(A, lengthA, strSlice(S, p, size));
    return A;
  }
});
}))JS";

}  // namespace tb::js
