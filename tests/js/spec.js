// Behaviors checked against ECMA-262 while filling gaps: class definition order, strict mode, eval, legacy syntax.
test("class static blocks and definition order", function () {
  const log = [];
  class A {
    static x = (log.push("x"), 1);
    static { log.push("block 1"); A.fromBlock = this === A ? A.x + 1 : -1; }
    static y = (log.push("y"), A.fromBlock + 1);
    [(log.push("key"), "m")]() { return "m"; }
    static { log.push("block 2"); try { A = null; } catch (e) { log.push(e.constructor.name); } }
    static self = A;
    inst = (log.push("instance field"), 1);
  }
  eq(log, ["key", "x", "block 1", "y", "block 2", "TypeError"]);
  eq([A.x, A.fromBlock, A.y, A.self === A, new A().m(), A.name], [1, 2, 3, true, "m", "A"]);
  class B { static #secret = 41; static { B.reveal = () => B.#secret + 1; } static { var local = 1; this.sawLocal = typeof local; } }
  eq([B.reveal(), B.sawLocal, typeof local], [42, "number", "undefined"]);
  class Base { static who() { return "base"; } }
  class D extends Base { static tag; static { D.tag = super.who() + "/" + this.name; } }
  eq(D.tag, "base/D");
  const E = class Named { static me = Named; static { Named.viaBlock = Named.me === Named; } };
  eq([E.me === E, E.viaBlock, typeof Named], [true, true, "undefined"]);
  let Outer = class Inner { static keep() { return Inner; } }; const kept = Outer.keep; Outer = null; eq(kept().name, "Inner");
  class F { static count = 0; static make() { F.count++; return new F(); } } let G = F; eq([G.make() instanceof F, F.count], [true, 1]);
  class H { static instance = new H(); n = 5; } eq(H.instance.n, 5);
});
test("private names with in", function () {
  class P { #x = 1; #m() { } static #s; static has(o) { return #x in o; } static hasMethod(o) { return #m in o; } static hasStatic(o) { return #s in o; } }
  eq([P.has(new P()), P.has({}), P.hasMethod(new P()), P.hasStatic(P), P.hasStatic(new P()), P.has(P)], [true, false, true, true, false, false]);
  throws(() => P.has(1), TypeError); throws(() => P.has(null), TypeError);
  throws(() => new Function("class Q { static t(o) { return #nope in o; } }"), SyntaxError);
});
test("strict mode: failed assignments and deletes throw", function () {
  function strict(body) { return new Function("o", "v", "'use strict'; " + body); }
  function sloppy(body) { return new Function("o", "v", body); }
  const frozen = Object.freeze({ a: 1, list: Object.freeze([1]) }), sealed = Object.seal({ a: 1 }), noExt = Object.preventExtensions({ a: 1 });
  const getterOnly = { get g() { return 1; } }, child = Object.create(Object.defineProperty({}, "ro", { value: 1 }));
  throws(() => strict("o.a = 2")(frozen), TypeError); throws(() => strict("o.b = 2")(frozen), TypeError); throws(() => strict("o['a'] = 2")(frozen), TypeError);
  throws(() => strict("o.list[0] = 2")(frozen), TypeError); throws(() => strict("o.list.length = 0")(frozen), TypeError); throws(() => strict("o.list[1] = 2")(frozen), TypeError);
  throws(() => strict("o.b = 2")(sealed), TypeError); strict("o.a = 5")(sealed); eq(sealed.a, 5);
  throws(() => strict("o.b = 2")(noExt), TypeError); throws(() => strict("o.g = 2")(getterOnly), TypeError); throws(() => strict("o.ro = 2")(child), TypeError);
  throws(() => strict("o.x = 1")("text"), TypeError); throws(() => strict("o.x = 1")(5), TypeError); throws(() => strict("o[0] = 'z'")("text"), TypeError);
  throws(() => strict("o.length = 1")("text"), TypeError); throws(() => strict("o.name = 'n'")(function f() { }), TypeError); throws(() => strict("o.length = 3")(function f() { }), TypeError);
  throws(() => strict("o.x += 1")(frozen), TypeError); throws(() => strict("o.a++")(frozen), TypeError); throws(() => strict("({ a: o.a } = { a: 3 })")(frozen), TypeError);
  throws(() => strict("undefined = 1")(), TypeError); throws(() => strict("NaN = 1")(), TypeError); throws(() => strict("Infinity = 1")(), TypeError);
  throws(() => strict("notDeclaredAnywhere = 1")(), ReferenceError); eq(typeof notDeclaredAnywhere, "undefined");
  throws(() => strict("delete o.a")(frozen), TypeError); throws(() => strict("delete o.a")(sealed), TypeError); throws(() => strict("delete o[0]")(frozen.list), TypeError);
  throws(() => strict("delete o.length")([]), TypeError); throws(() => strict("delete Object.prototype")(), TypeError);
  eq([strict("return delete o.missing")(frozen), strict("return delete o.a")({ a: 1 }), strict("return delete o.x")(5)], [true, true, true]);
  // setters still run, and assignment to a primitive goes through one
  let seen; Object.defineProperty(Number.prototype, "viaSetter", { set(x) { seen = [typeof this, x]; }, configurable: true });
  strict("o.viaSetter = 7")(5); eq(seen, ["object", 7]); delete Number.prototype.viaSetter;
  Object.defineProperty(Number.prototype, "strictSetter", { set: strict("globalThis.seenThis = typeof this"), configurable: true });
  sloppy("o.strictSetter = 1")(5); eq(seenThis, "number"); delete Number.prototype.strictSetter; delete globalThis.seenThis;
  // sloppy code: the same operations fail quietly
  sloppy("o.a = 2; o.b = 2; o.list[0] = 9; delete o.a")(frozen); eq([frozen.a, frozen.b, frozen.list[0]], [1, undefined, 1]);
  sloppy("o.x = 1")("text"); sloppy("o.g = 2")(getterOnly); eq(getterOnly.g, 1); eq(sloppy("return delete o.a")(frozen), false);
  sloppy("implicitGlobalFromTest = 3")(); eq(implicitGlobalFromTest, 3); delete globalThis.implicitGlobalFromTest;
  const f = function original() { }; sloppy("o.name = 'changed'; o.length = 9")(f); eq([f.name, f.length], ["original", 0]);
  eq([Reflect.set(frozen, "a", 2), Reflect.set({}, "a", 2), Reflect.deleteProperty(frozen, "a")], [false, true, false]);
  // class bodies are strict
  class K { static fail() { K.name = "other"; } } throws(() => K.fail(), TypeError);
});
test("a function's prototype property is an ordinary writable property", function () {
  function F() { } F.prototype = { m() { return "replaced"; } }; eq(new F().m(), "replaced");
  function G() { } const first = G.prototype; G.prototype = { tag: 2 }; eq([new G().tag, first === G.prototype, Object.getOwnPropertyNames(G).filter(n => n === "prototype").length], [2, false, 1]);
  const made = new Function("'use strict'; function H() {} H.prototype = { v: 3 }; return new H().v")(); eq(made, 3);
  eq(Object.getOwnPropertyDescriptor(F, "prototype").writable, true); eq(Object.getOwnPropertyDescriptor(class { }, "prototype").writable, false);
  throws(() => new Function("'use strict'; class C {} C.prototype = {};")(), TypeError);
});
test("delete with optional chaining", function () {
  const o = { a: { b: 1, c: 2 }, list: [1, 2] }; let none = null;
  eq([delete o?.a.b, o.a.b, delete none?.a.b, delete none?.a, delete none?.[0], delete o?.list?.[0], o.list[0], delete o.a?.c, o.a.c, delete o.missing?.x.y], [true, undefined, true, true, true, true, undefined, true, undefined, true]);
  throws(() => delete o?.missing.x, TypeError);
  eq((function () { "use strict"; const f = Object.freeze({ k: 1 }); try { delete f?.k; return "no error"; } catch (e) { return e.constructor.name; } })(), "TypeError");
});
test("direct eval sees the caller's scope", function () {
  var local = 5; let l2 = 6; const c = 7;
  eq([eval("local + l2 + c"), eval("1; 2; var z = 3; 4"), eval(5), eval()], [18, 4, 5, undefined]);
  eval("local = 9"); eq(local, 9); eval("var local = 10"); eq(local, 10);
  eq((function (a, b) { return (() => eval("[this.v, a + b, arguments.length]"))(); }).call({ v: 1 }, 2, 3), [1, 5, 2]);
  const e = eval; eq([typeof e("typeof local"), (0, eval)("typeof l2")], ["string", "undefined"]);
  eq(eval("(function () { return c * 2; })")(), 14); eq(eval("function inner() { return 'fn'; } inner()"), "fn");
  eval("let onlyInside = 1"); eq(typeof onlyInside, "undefined");
  eq((function () { "use strict"; eval("var strictVar = 1"); return typeof strictVar; })(), "undefined");
  const fs = []; for (let i = 0; i < 2; i++) fs.push(eval("() => i")); eq(fs.map(f => f()), [0, 1]);
  eq((function () { var eval = s => "mine:" + s; return eval("1+1"); })(), "mine:1+1");
  class A { #p = 5; m() { return eval("this.#p"); } } eq(new A().m(), 5);
  throws(() => eval("return 1"), SyntaxError); throws(() => eval("var = ;"), SyntaxError); throws(() => eval("early; let early = 1"), ReferenceError);
  try { throw 7; } catch (err) { eq(eval("err + 1"), 8); }
  (0, eval)("var madeByIndirectEval = 1"); eq(typeof madeByIndirectEval, "number"); delete globalThis.madeByIndirectEval;
});
test("private members follow the brand rules", function () {
  class A { #f = 1; #m() { return "m"; } get #g() { return "g"; } set #s(v) { this.#f = v; } static #sf = 5; static #sm() { return A.#sf; }
    read(o) { return o.#f; } write(o, v) { o.#f = v; } call(o) { return o.#m(); } getter(o) { return o.#g; } setter(o, v) { o.#s = v; return o.#f; }
    assignMethod(o) { o.#m = 1; } assignGetter(o) { o.#g = 1; } readSetter(o) { return o.#s; } static stat(o) { return o.#sm(); } }
  const a = new A();
  eq([a.read(a), a.call(a), a.getter(a), a.setter(a, 9), A.stat(A)], [1, "m", "g", 9, 5]);
  throws(() => a.read({}), TypeError); throws(() => a.write({}, 1), TypeError); throws(() => a.call({}), TypeError);
  throws(() => a.assignMethod(a), TypeError); throws(() => a.assignGetter(a), TypeError); throws(() => a.readSetter(a), TypeError);
  throws(() => A.stat(class extends A { }), TypeError);
  eq([Object.getOwnPropertyNames(a).length, Object.getOwnPropertySymbols(a).length, Reflect.ownKeys(a).length, JSON.stringify(a)], [0, 0, 0, "{}"]);
  const frozen = Object.freeze(new A()); a.write(frozen, 4); eq(a.read(frozen), 4);
  class Base { constructor(o) { return o; } } class Stamp extends Base { #id = 1; static has(o) { return #id in o; } }
  const target = {}; new Stamp(target); eq(Stamp.has(target), true); throws(() => new Stamp(target), TypeError);
  const proxied = new Proxy(a, {}); throws(() => a.read(proxied), TypeError);
});
done();
