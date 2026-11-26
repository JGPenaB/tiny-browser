test("variables and scope", function () {
  var a = 1; let b = 2; const c = 3;
  eq(a + b + c, 6);
  { let b = 20; eq(b, 20); }
  eq(b, 2);
  throws(function () { c2; let c2 = 1; }, ReferenceError, "TDZ");
  throws(function () { const k = 1; k = 2; }, TypeError, "const assign");
  eq(typeof notDeclared, "undefined");
  throws(function () { notDeclared; }, ReferenceError);
  eq(hoisted(), 7);
  function hoisted() { return 7; }
  eq(v1, undefined); var v1 = 5;
});
test("closures", function () {
  function counter() { let n = 0; return { inc: () => ++n, get: () => n }; }
  const c1 = counter(), c2 = counter();
  c1.inc(); c1.inc(); c2.inc();
  eq([c1.get(), c2.get()], [2, 1]);
  const fs = [];
  for (let i = 0; i < 3; i++) fs.push(() => i);
  eq(fs.map(f => f()), [0, 1, 2]);
  const gs = [];
  for (var j = 0; j < 3; j++) gs.push(() => j);
  eq(gs.map(f => f()), [3, 3, 3]);
  const hs = [];
  for (const k of [5, 6]) hs.push(() => k);
  eq(hs.map(f => f()), [5, 6]);
  function adder(x) { return function (y) { return function (z) { return x + y + z; }; }; }
  eq(adder(1)(2)(3), 6);
});
test("operators", function () {
  eq(7 % 3, 1); eq(-7 % 3, -1); eq(2 ** 10, 1024); eq(2 ** 3 ** 2, 512);
  eq(1 / 0, Infinity); eq(0.1 + 0.2 === 0.3, false); eq(5 / 2, 2.5);
  eq(1 << 31, -2147483648); eq(-1 >>> 0, 4294967295); eq(-8 >> 1, -4); eq(5 & 3, 1); eq(5 | 3, 7); eq(5 ^ 3, 6); eq(~5, -6);
  eq("5" * "2", 10); eq("5" + 2, "52"); eq(5 + "2", "52"); eq("a" + null + undefined + true, "anullundefinedtrue");
  eq(1 + true, 2); eq([] + [], ""); eq([1, 2] + "", "1,2"); eq({} + "", "[object Object]");
  eq(null ?? "d", "d"); eq(0 ?? "d", 0); eq(0 || "d", "d"); eq(1 && 2, 2);
  eq(1 == "1", true); eq(1 === "1", false); eq(null == undefined, true); eq(null == 0, false); eq(NaN == NaN, false);
  eq("b" > "a", true); eq("10" < "9", true); eq(10 < 9, false); eq("10" < 9, false);
  eq(typeof null, "object"); eq(typeof function () {}, "function"); eq(typeof Symbol(), "symbol");
  let x = 5; x += 3; x *= 2; x -= 1; x /= 3; eq(x, 5); x **= 2; eq(x, 25); x %= 7; eq(x, 4); x <<= 2; eq(x, 16);
  let n = null; n ??= 4; eq(n, 4); n ||= 9; eq(n, 4); n &&= 9; eq(n, 9);
  let i = 1; eq(i++ + ++i, 4); eq(i, 3); eq(i-- - --i, 2);
  eq(void 0, undefined); eq((1, 2, 3), 3);
  eq(0.1 * 3, 0.30000000000000004); eq(9007199254740993, 9007199254740992);
  eq(2147483647 + 1, 2147483648); eq(-2147483648 - 1, -2147483649); eq(65536 * 65536, 4294967296);
  eq(1 / -0, -Infinity); eq(0 * -1, -0); eq(-0 + 0, 0);
  const o = { a: { b: null } };
  eq(o?.a?.b?.c, undefined); eq(o.x?.y.z, undefined); eq(o.f?.(), undefined); eq(o?.["a"].b, null);
  eq("a" in { a: 1 }, true); eq(delete o.a, true); eq("a" in o, false);
  eq([1, 2] instanceof Array, true); eq({} instanceof Array, false);
});
test("control flow", function () {
  let out = [];
  outer: for (let i = 0; i < 3; i++) { for (let j = 0; j < 3; j++) { if (j == 1) continue outer; if (i == 2) break outer; out.push(i + "" + j); } }
  eq(out, ["00", "10"]);
  let n = 0; while (true) { if (++n > 4) break; } eq(n, 5);
  n = 0; do { n++; } while (n < 3); eq(n, 3);
  function sw(x) { switch (x) { case 1: return "one"; case 2: case 3: return "few"; default: return "many"; } }
  eq([sw(1), sw(2), sw(3), sw(9)], ["one", "few", "few", "many"]);
  let fall = ""; switch (2) { case 1: fall += "a"; case 2: fall += "b"; case 3: fall += "c"; break; case 4: fall += "d"; } eq(fall, "bc");
  const keys = []; for (const k in { a: 1, b: 2, 3: 0 }) keys.push(k); eq(keys, ["3", "a", "b"]);
  const inherited = []; function P() { this.own = 1; } P.prototype.inh = 2; for (var k in new P()) inherited.push(k); eq(inherited, ["own", "inh"]);
  let sum = 0; for (const v of [1, 2, 3]) sum += v; eq(sum, 6);
  let chars = ""; for (const ch of "a😀b") chars += "[" + ch + "]"; eq(chars, "[a][😀][b]");
  eq(true ? 1 : 2, 1); eq(false ? 1 : true ? 3 : 4, 3);
  blk: { n = 1; break blk; n = 2; } eq(n, 1);
});
test("exceptions", function () {
  const log = [];
  function f(x) { try { if (x) throw new Error("boom"); return "ok"; } catch (e) { log.push(e.message); return "caught"; } finally { log.push("fin"); } }
  eq([f(0), f(1)], ["ok", "caught"]); eq(log, ["fin", "boom", "fin"]);
  function g() { try { return 1; } finally { return 2; } } eq(g(), 2);
  function h() { for (let i = 0; i < 3; i++) { try { if (i == 1) continue; if (i == 2) break; } finally { log.push("f" + i); } } return log.slice(-3); }
  eq(h(), ["f0", "f1", "f2"]);
  try { null.x; } catch (e) { ok(e instanceof TypeError); ok(e.message.indexOf("null") >= 0); }
  try { undefinedFn(); } catch (e) { ok(e instanceof ReferenceError); eq(e.name, "ReferenceError"); }
  try { throw { code: 42 }; } catch ({ code }) { eq(code, 42); }
  try { throw 5; } catch { log.push("bare"); } eq(log[log.length - 1], "bare");
  const e = new RangeError("r", { cause: "c" }); eq([e.name, e.message, e.cause, String(e)], ["RangeError", "r", "c", "RangeError: r"]);
  ok(e instanceof Error); ok(typeof e.stack === "string" && e.stack.indexOf("RangeError: r") === 0);
  class MyErr extends Error { constructor(m) { super(m); this.name = "MyErr"; } }
  const me = new MyErr("x"); ok(me instanceof MyErr && me instanceof Error); eq(me.message, "x"); eq(String(me), "MyErr: x");
  function nested() { try { try { throw 1; } finally { log.push("inner"); } } catch (v) { return v + 1; } } eq(nested(), 2);
  function deep(n) { return n == 0 ? 0 : 1 + deep(n - 1); } eq(deep(5000), 5000);
  throws(function () { (function inf() { inf(); })(); }, RangeError, "stack overflow");
  throws(function () { null(); }, TypeError);
  throws(function () { ({}).missing(); }, TypeError);
});
test("functions", function () {
  function d(a, b = a + 1, ...rest) { return [a, b, rest]; }
  eq(d(1), [1, 2, []]); eq(d(1, 5, 6, 7), [1, 5, [6, 7]]); eq(d.length, 1); eq(d.name, "d");
  function args() { return arguments.length + ":" + Array.prototype.join.call(arguments, "-"); } eq(args(1, 2, 3), "3:1-2-3");
  const o = { v: 1, m() { return this.v; }, n: function () { return (() => this.v)(); } };
  eq([o.m(), o.n()], [1, 1]);
  const unbound = o.m; eq(unbound.call({ v: 9 }), 9); eq(unbound.apply({ v: 8 }, []), 8); eq(unbound.bind({ v: 7 })(), 7);
  function sum() { let s = 0; for (const a of arguments) s += a; return s; } eq(sum.apply(null, [1, 2, 3]), 6); eq(sum(...[1, 2], 3, ...[4]), 10);
  const bound = function (a, b) { return [this.x, a, b]; }.bind({ x: 1 }, 2); eq(bound(3), [1, 2, 3]); eq(bound.name, "bound ");
  function Ctor(a) { if (!new.target) return "no new"; this.a = a; } eq(Ctor(1), "no new"); eq(new Ctor(2).a, 2);
  eq((function () { return typeof this; })(), "object"); eq((function () { "use strict"; return typeof this; })(), "undefined");
  eq((() => 5)(), 5); eq((x => x * 2)(4), 8); eq(((a, b) => { return a + b; })(1, 2), 3); eq((() => ({ a: 1 }))().a, 1);
  const fact = function f(n) { return n <= 1 ? 1 : n * f(n - 1); }; eq(fact(10), 3628800);
  eq(new Function("a", "b", "return a * b")(6, 7), 42);
  ok(d.toString().indexOf("function d(a, b = a + 1") === 0);
  (function () { var iife = 3; eq(iife, 3); })();
  function getterSetter() { let v = 0; return { get val() { return v; }, set val(x) { v = x * 2; } }; }
  const gs = getterSetter(); gs.val = 5; eq(gs.val, 10);
});
test("destructuring and spread", function () {
  const [a, , b = 9, ...r] = [1, 2, undefined, 4, 5]; eq([a, b, r], [1, 9, [4, 5]]);
  const { x, y: { z = 3, w } = {}, ...others } = { x: 1, y: { w: 2 }, p: 5, q: 6 }; eq([x, z, w, others], [1, 3, 2, { p: 5, q: 6 }]);
  let m, n; [m, n] = [1, 2]; [m, n] = [n, m]; eq([m, n], [2, 1]);
  ({ m, n = 7 } = { m: 4 }); eq([m, n], [4, 7]);
  function f({ a, b = 2 } = {}, [c, d] = [3, 4]) { return [a, b, c, d]; } eq(f(), [undefined, 2, 3, 4]); eq(f({ a: 1 }, [5]), [1, 2, 5, undefined]);
  eq([...[1, 2], ..."ab", ...new Set([3])], [1, 2, "a", "b", 3]);
  eq({ ...{ a: 1, b: 2 }, b: 3, ...null }, { a: 1, b: 3 });
  const key = "dyn"; eq({ [key + 1]: 1, key }, { dyn1: 1, key: "dyn" });
  const [p = 1, q = p + 1] = []; eq([p, q], [1, 2]);
  for (const [k, v] of Object.entries({ a: 1 })) eq(k + v, "a1");
  const { length } = "hello"; eq(length, 5);
  throws(function () { const { a } = null; }, TypeError);
  const o = {}; [o.a, o["b"]] = [1, 2]; eq(o, { a: 1, b: 2 });
});
test("classes", function () {
  class Animal {
    #secret = 42; static count = 0; name; legs = 4;
    constructor(name) { this.name = name; Animal.count++; }
    speak() { return this.name + " makes a sound"; }
    get secret() { return this.#secret; } set secret(v) { this.#secret = v; }
    static create(n) { return new this(n); }
    #priv() { return "p" + this.#secret; } callPriv() { return this.#priv(); }
    toString() { return "Animal(" + this.name + ")"; }
  }
  class Dog extends Animal {
    tricks = [];
    constructor(name) { super(name); this.kind = "dog"; }
    speak() { return super.speak() + " (woof)"; }
    static create(n) { const d = super.create(n); d.made = true; return d; }
    get legsInfo() { return this.legs + " legs"; }
  }
  const d = Dog.create("Rex");
  eq([d.speak(), d.kind, d.made, d.legsInfo, d.secret, d.callPriv(), String(d), Animal.count], ["Rex makes a sound (woof)", "dog", true, "4 legs", 42, "p42", "Animal(Rex)", 1]);
  d.secret = 7; eq(d.secret, 7);
  ok(d instanceof Dog && d instanceof Animal); eq(Object.getPrototypeOf(Dog) === Animal, true); eq(typeof Dog, "function"); eq(Dog.name, "Dog");
  eq(Object.keys(d), ["name", "legs", "tricks", "kind", "made"]);
  throws(function () { Dog(); }, TypeError, "class without new");
  throws(function () { class B extends Animal { constructor() { this.x = 1; } } new B(); }, ReferenceError, "this before super");
  class C extends Dog {} eq(new C("z").name, "z");
  const E = class Named { static who() { return Named.name; } }; eq(E.who(), "Named");
  class Acc { static #n = 0; static next() { return ++Acc.#n; } } Acc.next(); eq(Acc.next(), 2);
  class Arr extends Array { sum() { return this.reduce((a, b) => a + b, 0); } }
  const arr = new Arr(); arr.push(1, 2, 3); eq([arr.sum(), arr.length, arr instanceof Array, Array.isArray(arr)], [6, 3, true, true]);
  class M { ['comp' + 'uted']() { return 1; } static [Symbol.hasInstance](x) { return x === 1; } } eq(new M().computed(), 1); eq(1 instanceof M, true);
  function OldStyle(v) { this.v = v; } OldStyle.prototype.get = function () { return this.v; };
  class FromOld extends OldStyle { get() { return super.get() * 2; } } eq(new FromOld(21).get(), 42);
  const obj = { __proto__: { hi() { return "hi"; } }, hi() { return super.hi() + "!"; } }; eq(obj.hi(), "hi!");
  class Late extends Animal { constructor() { const f = () => this.name; super("late"); this.got = f(); } } eq(new Late().got, "late");
});
test("getters, symbols, prototypes", function () {
  const sym = Symbol("desc"); const o = { [sym]: 1, get g() { return 2; }, set s(v) { this._s = v; } };
  o.s = 3; eq([o[sym], o.g, o._s, sym.toString(), sym.description], [1, 2, 3, "Symbol(desc)", "desc"]);
  eq(Object.keys(o), ["g", "s", "_s"]); eq(Object.getOwnPropertySymbols(o).length, 1); eq(Symbol.for("k") === Symbol.for("k"), true);
  const proto = { inherited: 1 }; const child = Object.create(proto, { own: { value: 2, enumerable: true } });
  eq([child.inherited, child.own, Object.keys(child), child.hasOwnProperty("inherited"), "inherited" in child], [1, 2, ["own"], false, true]);
  Object.defineProperty(child, "ro", { value: 1, writable: false }); child.ro = 5; eq(child.ro, 1);
  const frozen = Object.freeze({ a: 1 }); frozen.a = 2; frozen.b = 3; eq([frozen.a, frozen.b, Object.isFrozen(frozen)], [1, undefined, true]);
  eq(Object.getOwnPropertyDescriptor({ a: 1 }, "a"), { value: 1, writable: true, enumerable: true, configurable: true });
  const withToPrim = { [Symbol.toPrimitive](hint) { return hint === "number" ? 42 : "str"; } }; eq([+withToPrim, `${withToPrim}`, withToPrim + ""], [42, "str", "str"]);
  const vo = { valueOf() { return 10; }, toString() { return "s"; } }; eq([vo + 1, `${vo}`, vo * 2], [11, "s", 20]);
  eq(Object.prototype.toString.call([]), "[object Array]"); eq(Object.prototype.toString.call(null), "[object Null]");
  eq({}.toString(), "[object Object]"); eq(String(Symbol.iterator), "Symbol(Symbol.iterator)");
  const o2 = {}; o2.__proto__ = proto; eq(o2.inherited, 1); eq(Object.getPrototypeOf(o2) === proto, true);
});
test("generators and iterators", function () {
  function* gen(n) { for (let i = 0; i < n; i++) { const got = yield i; if (got) i += got; } return "end"; }
  eq([...gen(3)], [0, 1, 2]);
  const g = gen(10); eq([g.next().value, g.next(5).value, g.next().value], [0, 6, 7]); eq(g.return(9), { value: 9, done: true }); eq(g.next(), { value: undefined, done: true });
  function* inner() { yield 1; yield 2; return "r"; } function* outer() { const r = yield* inner(); yield r; yield* [3, 4]; }
  eq([...outer()], [1, 2, "r", 3, 4]);
  function* thrower() { try { yield 1; } catch (e) { yield "caught " + e; } } const t = thrower(); t.next(); eq(t.throw("x").value, "caught x");
  let closed = false;
  const iterable = { from: 1, to: 4, [Symbol.iterator]() { let c = this.from, t = this.to; return { next: () => c <= t ? { value: c++, done: false } : { value: undefined, done: true }, return() { closed = true; return {}; } }; } };
  eq([...iterable], [1, 2, 3, 4]); for (const v of iterable) { if (v == 2) break; } eq(closed, true);
  const [first, second] = iterable; eq([first, second], [1, 2]);
  function* fibs() { let [a, b] = [0, 1]; for (;;) { yield a; [a, b] = [b, a + b]; } }
  const out = []; for (const f of fibs()) { if (f > 50) break; out.push(f); } eq(out, [0, 1, 1, 2, 3, 5, 8, 13, 21, 34]);
  eq(Array.from({ length: 3 }, (_, i) => i * 2), [0, 2, 4]); eq(Array.from(new Map([[1, 2]]).keys()), [1]);
  const it = [1, 2][Symbol.iterator](); eq([it.next(), it.next(), it.next()], [{ value: 1, done: false }, { value: 2, done: false }, { value: undefined, done: true }]);
  const ent = Object.fromEntries(["a", "b"].entries()); eq(ent, { 0: "a", 1: "b" });
  class Tree { constructor(v, kids = []) { this.v = v; this.kids = kids; } *[Symbol.iterator]() { yield this.v; for (const k of this.kids) yield* k; } }
  eq([...new Tree(1, [new Tree(2, [new Tree(3)]), new Tree(4)])], [1, 2, 3, 4]);
});
test("misc syntax", function () {
  eq(`a${1}b${"x" + `n${2}`}c`, "a1bxn2c"); eq(`line1
line2`.length, 11);
  function tag(strs, ...vals) { return strs.raw.join("|") + "#" + vals.join(","); } eq(tag`a${1}b${2}c\n`, "a|b|c\\n#1,2");
  eq(String.raw`\n${1}`, "\\n1");
  eq(0x1f + 0b11 + 0o17 + 1_000 + .5 + 1e3, 2049.5); eq("\x41B\u{1F600}".length, 4); eq('it\'s', "it's");
  var a = 1
  var b = a
  ++b
  eq(b, 2);
  const o = { if: 1, class: 2, new: 3 }; eq(o.if + o.class + o.new, 6);
  eq([1, , 3].length, 3); eq([, ].length, 1); eq((function () { return })(), undefined);
  label: for (var i = 0; i < 2; i++) continue label;
  eq(((a, b) => a + b).length, 2); eq(typeof async function () {}, "function");
  var getter = { get ["x" + 1]() { return 5; } }; eq(getter.x1, 5);
  eq(1 < 2 < 3, true); eq(3 > 2 > 1, false); eq(!!"0", true); eq(+"  12  ", 12); eq(+"1e3", 1000); eq(+"0x10", 16); eq(+"", 0); eq(+"abc", NaN);
  eq(parseInt("42px"), 42); eq(parseInt("ff", 16), 255); eq(parseInt("0x1A"), 26); eq(parseFloat("3.14abc"), 3.14); eq(parseInt(""), NaN);
  eq((123.456).toFixed(1), "123.5"); eq((0.000001234).toString(), "0.000001234"); eq((1e21).toString(), "1e+21"); eq((255).toString(16), "ff"); eq((0.5).toString(2), "0.1");
  eq(String(1e-7), "1e-7"); eq(String(123456789012345680000), "123456789012345680000"); eq((25).toString(36), "p"); eq(String(-1.5e-10), "-1.5e-10");
  eq(1.005.toFixed(2), "1.00"); eq((2.5).toFixed(0), "3"); eq((1234.5678).toFixed(2), "1234.57"); eq((0).toFixed(2), "0.00"); eq((123456).toPrecision(2), "1.2e+5"); eq((0.00001).toExponential(1), "1.0e-5");
});
done();
