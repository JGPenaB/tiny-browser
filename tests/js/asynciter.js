// Generators finishing early, yield* forwarding, async generators and for await (ECMA-262 14.7.5, 15.5, 27.5, 27.6).
test("generator return() runs finally blocks", function () {
  const log = [];
  function* g() { try { log.push("start"); yield 1; log.push("not reached"); } catch (e) { log.push("catch"); } finally { log.push("finally"); } log.push("after"); }
  let it = g(); it.next();
  eq(it.return(7), { value: 7, done: true }); eq(log, ["start", "finally"]); eq(it.next(), { value: undefined, done: true });
  function* nested() { try { try { yield 1; } finally { log.push("inner"); } } finally { log.push("outer"); } }
  log.length = 0; it = nested(); it.next(); it.return(); eq(log, ["inner", "outer"]);
  function* yieldsInFinally() { try { yield 1; } finally { yield "cleanup"; log.push("done"); } }
  log.length = 0; it = yieldsInFinally(); it.next();
  eq(it.return(5), { value: "cleanup", done: false }); eq(it.next(), { value: 5, done: true }); eq(log, ["done"]);
  function* overrides() { try { yield 1; } finally { return "from finally"; } }
  it = overrides(); it.next(); eq(it.return("asked"), { value: "from finally", done: true });
  function* throwsInFinally() { try { yield 1; } finally { throw new RangeError("cleanup failed"); } }
  it = throwsInFinally(); it.next(); throws(() => it.return(1), RangeError); eq(it.next(), { value: undefined, done: true });
  it = g(); eq(it.return(3), { value: 3, done: true }); eq(it.next().done, true);  // never started: nothing runs
  it = g(); it.next(); eq(it.throw(new TypeError("x")), { value: undefined, done: true }); eq(log.slice(-3), ["catch", "finally", "after"]);
});
test("for-of closes its iterator", function () {
  const log = [];
  function source(extra) {
    let i = 0;
    return Object.assign({ [Symbol.iterator]() { return this; }, next() { return { value: i++, done: i > 5 }; }, return(v) { log.push("return"); return {}; } }, extra);
  }
  for (const x of source()) { if (x === 1) break; } eq(log, ["return"]);
  log.length = 0; try { for (const x of source()) throw new Error("body"); } catch (e) { log.push(e.message); } eq(log, ["return", "body"]);
  log.length = 0; (function () { for (const x of source()) return x; })(); eq(log, ["return"]);
  log.length = 0; outer: for (const a of source()) { for (const b of source()) continue outer; } eq(log.length, 5);
  log.length = 0; for (const x of source()) { } eq(log, []);  // ran to the end: not closed
  log.length = 0; try { for (const [a] of source()) { } } catch (e) { log.push(e.constructor.name); } eq(log, ["return", "TypeError"]);
  // an error from return() is reported on break, and ignored when the body already threw
  const bad = { return() { throw new RangeError("return failed"); } };
  throws(() => { for (const x of source(bad)) break; }, RangeError);
  let seen; try { for (const x of source(bad)) throw new SyntaxError("first"); } catch (e) { seen = e.constructor.name; } eq(seen, "SyntaxError");
  throws(() => { for (const x of source({ return() { return 1; } })) break; }, TypeError);
  // an error from next() does not close the iterator
  log.length = 0; try { for (const x of source({ next() { throw new Error("next"); } })) { } } catch (e) { } eq(log, []);
  function* gen() { try { yield 1; yield 2; } finally { log.push("generator closed"); } }
  log.length = 0; for (const x of gen()) break; eq(log, ["generator closed"]);
  log.length = 0; const [first] = gen(); eq([first, log.join()], [1, "generator closed"]);
  log.length = 0; const [p, q, r] = gen(); eq([p, q, r, log.join()], [1, 2, undefined, "generator closed"]);
  log.length = 0; const [...all] = gen(); eq([all, log.join()], [[1, 2], "generator closed"]);
  log.length = 0; try { const [a = (() => { throw new Error("default"); })()] = source({ next() { return { value: undefined, done: false }; } }); } catch (e) { log.push(e.message); } eq(log, ["return", "default"]);
  log.length = 0; let m, n; [m, n] = source(); eq([m, n, log.join()], [0, 1, "return"]);
});
test("yield* forwards next, throw and return", function () {
  const log = [];
  function* inner() { try { const a = yield "i1"; log.push("got " + a); const b = yield "i2"; log.push("got " + b); return "inner result"; } catch (e) { log.push("caught " + e); yield "recovered"; } finally { log.push("inner finally"); } }
  function* outer() { try { const r = yield* inner(); log.push("r=" + r); yield "after"; } finally { log.push("outer finally"); } }
  let it = outer();
  eq([it.next("ignored").value, it.next("A").value, it.next("B").value, it.next().done], ["i1", "i2", "after", true]);
  eq(log, ["got A", "got B", "inner finally", "r=inner result", "outer finally"]);
  log.length = 0; it = outer(); it.next();
  eq(it.throw("boom"), { value: "recovered", done: false }); eq(it.next().value, "after"); eq(log, ["caught boom", "inner finally", "r=undefined"]);
  log.length = 0; it = outer(); it.next();
  eq(it.return("stop"), { value: "stop", done: true }); eq(log, ["inner finally", "outer finally"]);
  // the inner iterator's own result objects are passed through untouched
  const result = { value: 1, done: false, extra: true };
  const custom = { [Symbol.iterator]() { return this; }, next() { return result; } };
  function* pass() { yield* custom; }
  ok(pass().next() === result);
  // an inner iterator without throw(): it is closed and a TypeError reports the protocol violation
  const noThrow = { [Symbol.iterator]() { return this; }, next() { return { value: 1, done: false }; }, return() { log.push("closed"); return {}; } };
  function* overNoThrow() { yield* noThrow; }
  log.length = 0; it = overNoThrow(); it.next(); throws(() => it.throw(new RangeError("x")), TypeError); eq(log, ["closed"]);
  // return() when the inner iterator is not finished by it
  const stubborn = { [Symbol.iterator]() { return this; }, next() { return { value: "n", done: false }; }, return(v) { return { value: "still going", done: false }; } };
  function* overStubborn() { yield* stubborn; }
  it = overStubborn(); it.next(); eq(it.return(1), { value: "still going", done: false });
  function* overArray() { const r = yield* [1, 2]; return r; }
  eq([...overArray()], [1, 2]); it = overArray(); it.next(); eq(it.return(9), { value: 9, done: true });
  function* overString() { yield* "ab"; } eq([...overString()].join(), "a,b");
  throws(() => { function* bad() { yield* 5; } bad().next(); }, TypeError);
});

(async function () {
  __current = "async generators";
  const log = [];
  const later = (v, ms) => new Promise(r => setTimeout(() => r(v), ms || 1));
  async function* count(n) { for (let i = 1; i <= n; i++) { await null; yield i; } return "end"; }
  let g = count(2);
  eq([Object.prototype.toString.call(g), typeof g[Symbol.asyncIterator], g[Symbol.asyncIterator]() === g, g.next() instanceof Promise], ["[object AsyncGenerator]", "function", true, true]);
  eq([await g.next(), await g.next(), await g.next()], [{ value: 2, done: false }, { value: "end", done: true }, { value: undefined, done: true }]);
  g = count(3);  // requests queue up and are answered in order
  eq(await Promise.all([g.next(), g.next(), g.next(), g.next(), g.next()]), [{ value: 1, done: false }, { value: 2, done: false }, { value: 3, done: false }, { value: "end", done: true }, { value: undefined, done: true }]);
  async function* awaitsYields() { const got = yield later("p1"); log.push(got); yield Promise.resolve("p2"); return later("ret"); }
  g = awaitsYields();
  eq([(await g.next()).value, (await g.next("sent")).value, await g.next(), log.join()], ["p1", "p2", { value: "ret", done: true }, "sent"]);
  async function* rejectsYield() { try { yield Promise.reject(new RangeError("bad")); } catch (e) { yield "caught " + e.message; } }
  eq((await rejectsYield().next()).value, "caught bad");
  async function* withFinally() { try { yield 1; yield 2; } finally { log.push("finally"); await null; log.push("finally done"); } }
  log.length = 0; g = withFinally(); await g.next();
  eq([await g.return(later("r")), log.join(), await g.next()], [{ value: "r", done: true }, "finally,finally done", { value: undefined, done: true }]);
  g = withFinally(); await g.next();
  const thrown = await g.throw(new SyntaxError("t")).catch(e => e.constructor.name); eq(thrown, "SyntaxError");
  g = withFinally(); eq(await g.return("early"), { value: "early", done: true }); eq((await g.next()).done, true);
  g = withFinally(); eq(await g.throw("x").catch(e => "rejected " + e), "rejected x"); eq((await g.next()).done, true);
  eq(await g.return(Promise.reject(new Error("r"))).catch(e => e.message), "r");
  async function* throwsLater() { yield 1; throw new EvalError("body"); }
  g = throwsLater(); await g.next(); eq(await g.next().catch(e => e.constructor.name), "EvalError"); eq(await g.next(), { value: undefined, done: true });
  eq(await count.prototype.__proto__.next.call({}).catch(e => e.constructor.name), "TypeError");
  eq([typeof count.prototype, Object.getPrototypeOf(count.prototype) === Object.getPrototypeOf(g).__proto__, count.prototype.__proto__.__proto__[Symbol.asyncIterator].call(5)], ["object", true, 5]);
  const obj = { async *items() { yield "m"; } }; eq((await obj.items().next()).value, "m");
  class C { static async *s() { yield 1; } async *i() { yield this instanceof C; } } eq([(await C.s().next()).value, (await new C().i().next()).value], [1, true]);

  __current = "for await";
  let sum = 0; for await (const x of count(4)) sum += x; eq(sum, 10);
  const got = []; for await (const x of [later("a", 3), "b", Promise.resolve("c")]) got.push(x); eq(got, ["a", "b", "c"]);
  got.length = 0; for await (const x of "hi") got.push(x); eq(got, ["h", "i"]);
  log.length = 0;
  const source = (extra) => { let i = 0; return Object.assign({ [Symbol.asyncIterator]() { return this; }, next() { return later({ value: i++, done: i > 4 }); }, return() { log.push("return"); return later({}).then(v => { log.push("return awaited"); return v; }); } }, extra); };
  for await (const x of source()) { if (x === 1) break; } eq(log, ["return", "return awaited"]);
  log.length = 0; try { for await (const x of source()) throw new Error("body"); } catch (e) { log.push(e.message); } eq(log, ["return", "return awaited", "body"]);
  log.length = 0; await (async function () { for await (const x of source()) return x; })(); eq(log, ["return", "return awaited"]);
  log.length = 0; for await (const x of source()) { } eq(log, []);
  let err; try { for await (const x of source({ return() { return later(5); } })) break; } catch (e) { err = e.constructor.name; } eq(err, "TypeError");
  err = "none"; try { for await (const x of source({ return() { return Promise.reject(new RangeError("r")); } })) throw new SyntaxError("first"); } catch (e) { err = e.constructor.name; } eq(err, "SyntaxError");
  err = "none"; try { for await (const x of source({ next() { return later(7); } })) { } } catch (e) { err = e.constructor.name; } eq(err, "TypeError");
  err = "none"; try { for await (const x of 5) { } } catch (e) { err = e.constructor.name; } eq(err, "TypeError");
  err = "none"; try { for await (const x of [Promise.reject(new EvalError("in list")), 2]) { } } catch (e) { err = e.constructor.name; } eq(err, "EvalError");
  // a sync iterator behind for await is closed when one of its values rejects
  log.length = 0;
  function* syncGen() { try { yield Promise.reject(new Error("rejected value")); yield 2; } finally { log.push("sync closed"); } }
  try { for await (const x of syncGen()) { } } catch (e) { log.push(e.message); } eq(log, ["sync closed", "rejected value"]);
  const pairs = []; for await (const [k, v] of new Map([["a", 1]])) pairs.push(k + v); eq(pairs, ["a1"]);
  let target = {}; for await (target.x of [1, 2]); eq(target.x, 2);
  label: for await (const x of count(3)) { for await (const y of count(3)) { if (y === 2) continue label; sum += 100; } } eq(sum, 310);

  __current = "yield* in async generators";
  async function* innerAsync() { try { const a = yield "a1"; yield "got " + a; return "inner done"; } finally { log.push("inner finally"); } }
  async function* outerAsync() { const r = yield* innerAsync(); yield r; yield* [later("s1"), "s2"]; yield* count(1); }
  log.length = 0; g = outerAsync(); const values = [];
  values.push((await g.next()).value, (await g.next("X")).value, (await g.next()).value);
  for await (const v of g) values.push(v);
  eq(values, ["a1", "got X", "inner done", "s1", "s2", 1]); eq(log, ["inner finally"]);
  log.length = 0; g = outerAsync(); await g.next(); eq(await g.return("bye"), { value: "bye", done: true }); eq(log, ["inner finally"]);
  g = outerAsync(); await g.next(); eq(await g.throw(new RangeError("in")).catch(e => e.constructor.name), "RangeError");
  async function* awaitInside() { const v = await later(20); yield v + (await later(1)); }
  eq((await awaitInside().next()).value, 21);
  async function* collect() { const out = []; for await (const x of count(3)) out.push(x); yield out; }
  eq((await collect().next()).value, [1, 2, 3]);
  done();
})().catch(e => { console.log("FAIL main rejected: " + (e && e.stack || e)); });
