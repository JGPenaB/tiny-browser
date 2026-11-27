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
test("function body scope and function names (10.2.11)", function () {
  eq((function (a = 1, g = () => a) { var a; return a + " " + g(); })(), "1 1");
  eq((function (a = 1, g = () => a) { var a = 2; return a + " " + g(); })(), "2 1");
  eq((function (a = 1, g = () => a) { a = 5; return a + " " + g(); })(), "5 5");
  eq((function (a = 1, g = () => a) { function a() { } return typeof a + " " + g(); })(), "function 1");
  var x = "outer"; eq((function (g = () => x) { var x = "inner"; return g() + " " + x; })(), "outer inner");
  eq((function (a = 0) { var arguments; return arguments.length; })(1, 2), 2);
  eq((function (a = arguments.length) { function arguments() { } return a + " " + typeof arguments; })(undefined, 2), "2 function");
  eq((function () { return typeof arguments; var arguments = 5; })(), "object");
  eq((function (a = 1) { eval("var q = 6"); return q; })(), 6);
  eq((function (a = 1) { var b = 7; eval("var b = 9"); return b; })(), 9);
  eq((function (a = 1) { { function a() { } } return a; })(), 1);
  eq(((a = 1, g = () => a) => { var a = 2; return a + g(); })(), 3);
  eq([...(function* (a = 1) { var b = 2; yield a; yield b; })()], [1, 2]);
  eq((function f(a = typeof f) { var f; return a + " " + typeof f; })(), "function undefined");
  eq([(function f(f) { return f; })(7), (function f() { var f; return typeof f; })(), (function f() { f = 1; return typeof f; })()], [7, "undefined", "function"]);
  throws(() => (function f() { "use strict"; f = 1; })(), TypeError);
  throws(() => new Function("a = 1", "let a;"), SyntaxError);
  throws(() => new Function("{ async function f() {} async function f() {} }"), SyntaxError);
  new Function("{ function f() {} function f() {} }");
  throws(() => new Function("switch (1) { default: default: }"), SyntaxError);
  throws(() => new Function("async function f(a = await 1) {}"), SyntaxError);
  throws(() => new Function("function* g() { (x = yield) => {}; }"), SyntaxError);
  eq([(function () { var v = 1; return delete v; })(), (function () { return delete arguments; })()], [false, false]);
  globalThis.__gone = 1; eq([delete __gone, "__gone" in globalThis], [true, false]);
});
test("generator, async and dynamic functions (20.2.1.1.1, 27.3, 27.4, 27.7)", function () {
  const GF = Object.getPrototypeOf(function* () { }).constructor, AF = Object.getPrototypeOf(async function () { }).constructor;
  const AGF = Object.getPrototypeOf(async function* () { }).constructor;
  eq([GF.name, AF.name, AGF.name, GF.length, Object.getPrototypeOf(GF) === Function, typeof GeneratorFunction], ["GeneratorFunction", "AsyncFunction", "AsyncGeneratorFunction", 1, true, "undefined"]);
  eq(Object.getPrototypeOf(GF.prototype) === Function.prototype, true);
  eq(GF.prototype.prototype === Object.getPrototypeOf((function* () { }).prototype), true);
  eq([GF.prototype[Symbol.toStringTag], AGF.prototype.prototype[Symbol.toStringTag], AF.prototype[Symbol.toStringTag]], ["GeneratorFunction", "AsyncGenerator", "AsyncFunction"]);
  const g = new GF("a", "b", "yield a; yield b;");
  eq([[...g(1, 2)], g.name, String(g)], [[1, 2], "anonymous", "function* anonymous(a,b\n) {\nyield a; yield b;\n}"]);
  eq(String(new AF("x", "return await x")), "async function anonymous(x\n) {\nreturn await x\n}");
  eq(new Function("return typeof anonymous")(), "undefined");
  throws(() => new Function("/*", "*/){"), SyntaxError); throws(() => new Function("", "}, function(){"), SyntaxError);
  throws(() => new AGF("x = await 42", ""), SyntaxError);
  const gen = function* () { }; const proto = Object.getPrototypeOf(gen).prototype; gen.prototype = null;
  eq(Object.getPrototypeOf(gen()) === proto, true);
  class A { m() { } } eq(String(A), "class A { m() { } }");
  async function af(x) { } eq(String(af), "async function af(x) { }");
  eq(String(Object.getOwnPropertyDescriptor(RegExp, "$&").get), "function () { [native code] }");
});
test("template objects and escapes (13.2.8)", function () {
  const tag = s => s; const f = () => tag`a${1}b`;
  eq([f() === f(), Object.isFrozen(f()), Object.isFrozen(f().raw), f().raw.length], [true, true, true, 2]);
  eq([tag`\u{110000}`[0], tag`\u{110000}`.raw[0], tag`\xg`[0], tag`\1`.raw[0]], [undefined, "\\u{110000}", undefined, "\\1"]);
  throws(() => new Function("`\\8`"), SyntaxError); throws(() => new Function("`\\u{110000}`"), SyntaxError);
  throws(() => new Function("'\\u{110000}'"), SyntaxError);
  function C(x) { this.x = x; } const maker = () => C;
  eq([new maker`t` instanceof C, new maker`t`(5).x], [true, 5]);
  throws(() => new Function('"\\1"; "use strict";'), SyntaxError);
  eq(eval("''/*\r*/''"), "");
});
test("Iterator.concat, zip, zipKeyed, chunks, windows, includes, join (27.1.3)", function () {
  eq([...Iterator.concat([1, 2], new Set([3]), ["a", "b"])], [1, 2, 3, "a", "b"]);
  throws(() => Iterator.concat("ab"), TypeError);
  throws(() => Iterator.concat(1), TypeError);
  eq([...Iterator.zip([[1, 2, 3], ["a", "b"]])], [[1, "a"], [2, "b"]]);
  throws(() => Iterator.zip([[1], "ab"]), TypeError);
  eq([...Iterator.zip([[1, 2, 3], ["a", "b"]], { mode: "longest", padding: ["x", "y"] })], [[1, "a"], [2, "b"], [3, "y"]]);
  throws(() => [...Iterator.zip([[1, 2, 3], ["a", "b"]], { mode: "strict" })], TypeError);
  throws(() => Iterator.zip([], { mode: "other" }), TypeError);
  eq([...Iterator.zipKeyed({ n: [1, 2], s: ["a", "b"] })].map(o => o.n + o.s), ["1a", "2b"]);
  eq(Object.getPrototypeOf(Iterator.zipKeyed({ a: [1] }).next().value), null);
  eq([...[1, 2, 3, 4, 5].values().chunks(2)], [[1, 2], [3, 4], [5]]);
  eq([...[1, 2, 3, 4].values().windows(3)], [[1, 2, 3], [2, 3, 4]]);
  eq([[...[1, 2].values().windows(3)], [...[1, 2].values().windows(3, "allow-partial")]], [[], [[1, 2]]]);
  throws(() => [1].values().chunks(0), RangeError); throws(() => [1].values().chunks("2"), TypeError);
  eq([[1, NaN].values().includes(NaN), [1, 2].values().includes(1, 1), [1, 2].values().includes(2, 1)], [true, false, true]);
  throws(() => [1].values().includes(1, -1), RangeError); throws(() => [1].values().includes(1, 0.5), TypeError);
  eq([[1, null, "a", undefined].values().join(), [1, 2].values().join("-")], ["1,,a,", "1-2"]);
  let closed = 0; const it = { __proto__: Iterator.prototype, next() { return { value: 1, done: false }; }, return() { closed++; return {}; } };
  const z = Iterator.zip([it, it]); z.next(); z.return(); eq(closed, 2);
  throws(() => [1].values().take(Number.MAX_SAFE_INTEGER + 1), RangeError);
});
test("mapped arguments objects (10.4.4)", function () {
  eq((function (a, b) { a = 5; return arguments[0] + "," + arguments[1]; })(1, 2), "5,2");
  eq((function (a, b) { arguments[1] = 9; return a + "," + b; })(1, 2), "1,9");
  eq((function (a) { arguments[1] = 9; return a + "," + arguments.length; })(1), "1,1");
  eq((function (a, b) { b = 7; return arguments[1] + "," + arguments.length; })(1), "undefined,1");
  eq((function (a) { "use strict"; a = 5; return arguments[0]; })(1), 1);
  eq((function (a) { delete arguments[0]; a = 5; return arguments[0]; })(1), undefined);
  eq((function (a) { Object.defineProperty(arguments, "0", { writable: false }); a = 5; return arguments[0]; })(1), 1);
  eq((function (a) { Object.defineProperty(arguments, "0", { value: 8 }); return a; })(1), 8);
  eq((function (a) { (function () { a = 6; })(); return arguments[0]; })(1), 6);
  eq((function (a) { eval("a = 4"); return arguments[0]; })(1), 4);
  eq((function (a) { a++; a += 2; return arguments[0]; })(1), 4);
  eq((function (a, a) { a = 5; return arguments[0] + "," + arguments[1]; })(1, 2), "1,5");
  var keep, set; (function (a) { keep = arguments; set = function (v) { a = v; }; })(1); set(3); eq(keep[0], 3);
  eq((function (a) { a = a || "dflt"; return (function (x) { return x; }).apply(null, arguments); })(0), "dflt");
  eq((function (a = 1) { a = 5; return arguments[0]; })(2), 2);
  eq((function (a) { Object.freeze(arguments); a = 2; return arguments[0]; })(1), 1);
  function inner() { return inner.caller === outer; } function outer() { return inner(); } eq([outer(), inner.caller], [true, null]);
});
test("declarations checked before they are made (16.1.7, 19.2.1.3, B.3)", function () {
  try { (0, eval)("function __notMade(){} function NaN(){}"); } catch (e) { }
  eq(typeof __notMade, "undefined");
  throws(() => { { let x; { eval("var x;"); } } }, SyntaxError);
  throws(() => new Function("const x;"), SyntaxError); throws(() => new Function("let [a];"), SyntaxError);
  throws(() => { f(); let y; function f() { y = 1; } }, ReferenceError);
  throws(() => { for (let z of [z]) { } }, ReferenceError);
  var foo = "before"; try { throw 0; } catch (foo) { var foo = "in catch"; } eq(foo, "before");
  throws(() => new Function("try {} catch ({ f }) { var f; }"), SyntaxError);
  var called = false; function c() { called = true; } throws(() => { c() = 1; }, ReferenceError); eq(called, true);
  throws(() => new Function("f() &&= 1"), SyntaxError); throws(() => new Function("'use strict'; f() = 1"), SyntaxError);
  var probe; for (let i = 0, g = probe = () => i; i < 1; i++) { } eq(probe(), 0);
  eq([eval("for (false; false; false);"), /[\c0]/.exec("\x10")[0]], [undefined, "\x10"]);
  var init, after; (function (f) { eval("init = f; { function f() { } } after = f;"); })(123); eq([init, typeof after], [123, "function"]);
  const e = new Error("m"); eq([Object.hasOwn(e, "stack"), typeof e.stack, typeof Object.getOwnPropertyDescriptor(Error.prototype, "stack").get], [false, "string", "function"]);
  e.stack = "s"; eq([Object.hasOwn(e, "stack"), e.stack], [true, "s"]);
  Array.prototype[1] = "p"; var holes = [0]; holes.length = 2; eq(holes.join(), "0,p"); delete Array.prototype[1];
});
test("syntax rules that were missing (13.6, 13.13, 13.3, 16.2)", function () {
  for (const src of ["-x ** 2", "typeof x ** 2", "a ?? b || c", "a || b ?? c", "a ?? b && c", "a?.b`t`", "class C { #f; m() { #f in #f in this; } }", "const x;",
    "class C extends (o.#x) { #x }"])
    throws(() => new Function(src), SyntaxError, src);
  for (const src of ["(-x) ** 2", "(a ?? b) || c", "a ?? (b || c)", "++x ** 2", "class C { #f; m() { return #f in this; } }"]) new Function(src);
  var x = "global"; (function () { var x = "local"; eval(...["x = 1"]); eq(x, 1); })(); eq(x, "global");
  (function () { var y = "local"; eval?.("var __viaOptional = typeof y"); })(); eq(__viaOptional, "undefined");
  const o = { b() { return this.v; }, v: 42 }; eq([(o?.b)(), o?.b(), (o.b)?.()], [42, 42, 42]);
  class Base { } class D extends Base { constructor() { super(); try { return 0; } catch (e) { return; } } } throws(() => new D(), TypeError);
  throws(() => { class X extends X { } }, ReferenceError);
  var seen; class F extends Base { field = seen(); constructor() { seen = () => this; super(); } } const f = new F(); eq(f.field === f, true);
  class P extends Base { m() { delete super.x; } } throws(() => new P().m(), ReferenceError);
  eq([JSON.stringify(JSON.parse('[1.0, "a"]', (k, v, ctx) => ctx.source ?? v)), JSON.stringify({ n: JSON.rawJSON("1e1000") }), JSON.isRawJSON(JSON.rawJSON("1"))],
     ['["1.0","\\"a\\""]', '{"n":1e1000}', true]);
  eq([(25).toExponential(0), (-0).toPrecision(2), Math.pow(1, NaN), Math.round(-0.2), Math.hypot(NaN, Infinity), Math.sumPrecise([1e20, 0.1, -1e20])], ["3e+1", "0.0", NaN, -0, Infinity, 0.1]);
  eq([new Date(0)[Symbol.toPrimitive]("number"), typeof Date.prototype.setYear, Date.prototype.toGMTString === Date.prototype.toUTCString], [0, "function", true]);
  throws(() => encodeURI("\uD800"), URIError); eq(decodeURI("%F0%9F%98%80"), "\u{1F600}");
});
test("using and await using declarations (14.3.1, 9.5)", function () {
  let log = [];
  const res = (name, fail) => ({ [Symbol.dispose]() { log.push("d:" + name); if (fail) throw new Error("fail " + name); } });
  { using a = res("a"), b = res("b"); log.push("body"); } eq(log, ["body", "d:b", "d:a"]);
  log = []; eq((function () { using a = res("a"); return log.length; })(), 0); eq(log, ["d:a"]);
  log = []; try { { using a = res("a", true); throw new Error("boom"); } } catch (e) { eq([e instanceof SuppressedError, e.error.message, e.suppressed.message], [true, "fail a", "boom"]); }
  log = []; try { { using a = res("a"), b = (() => { throw new Error("init"); })(); } } catch (e) { eq([e.message, log], ["init", ["d:a"]]); }
  log = []; { using n = null, u = undefined; } eq(log, []);
  throws(() => { { using x = 1; } }, TypeError); throws(() => { { using x = {}; } }, TypeError);
  log = []; for (let i = 0; i < 2; i++) { using a = res("a" + i); if (i === 0) continue; break; } eq(log, ["d:a0", "d:a1"]);
  log = []; for (using x of [res("x"), res("y")]) log.push("body"); eq(log, ["body", "d:x", "body", "d:y"]);
  log = []; function* g() { using a = res("a"); yield 1; yield 2; } const it = g(); it.next(); it.return(); eq(log, ["d:a"]);
  throws(() => { { x; using x = null; } }, ReferenceError); throws(() => { { using x = null; x = 1; } }, TypeError);
  var using = 5; using
    = 6; eq(using, 6);
  for (const src of ["{ using x; }", "switch (1) { case 1: using x = null; }", "if (1) using x = null;", "for (using x in {}) ;", "{ await using x = null; }"])
    throws(() => new Function(src), SyntaxError, src);
  throws(() => (0, eval)("using x = null;"), SyntaxError);
  for (let in { k: 1 }) ; eq(let, "k");
});
test("proper tail calls in strict code (15.10)", function () {
  "use strict";
  function count(n, acc) { if (n === 0) return acc; return count(n - 1, acc + 1); }
  eq(count(200000, 0), 200000);
  const even = n => n === 0 ? true : odd(n - 1), odd = n => n === 0 ? false : even(n - 1);
  eq(even(100001), false);
  function viaLogical(n) { return n === 0 || viaLogical(n - 1); } eq(viaLogical(150000), true);
  function viaComma(n) { return n === 0 ? "done" : (0, viaComma(n - 1)); } eq(viaComma(150000), "done");
  function inTry(n) { try { return n === 0 ? 0 : inTry(n - 1) + 0; } catch (e) { return "caught"; } }
  eq(inTry(100), 0);
  function deep(n) { try { return n === 0 ? 0 : deep(n - 1); } catch (e) { return e.constructor.name; } }
  eq(deep(1e6), "RangeError");
  function Ctor(n) { if (n > 0) return new Ctor(n - 1); this.depth = 0; } eq(new Ctor(50).depth, 0);
  const o = { m(n) { return n === 0 ? this.v : this.m(n - 1); }, v: 7 }; eq(o.m(150000), 7);
});
test("finally runs once, with and object environments, small rules", function () {
  let runs = 0; function f() { try { throw "t"; } catch (e) { return "c"; } finally { runs++; throw new RangeError("fin"); } }
  throws(f, RangeError); eq(runs, 1);
  runs = 0; function g() { try { try { return 1; } catch (e) { return "inner caught"; } } finally { runs++; } } eq([g(), runs], [1, 1]);
  runs = 0; function h() { try { try { return 1; } catch (e) { return "wrong"; } finally { runs++; throw new RangeError("fin"); } } catch (e) { return e.constructor.name; } }
  eq([h(), runs], ["RangeError", 1]);
  runs = 0; function loop() { for (let i = 0; i < 3; i++) { try { if (i === 1) continue; if (i === 2) break; } finally { runs++; } } return runs; } eq(loop(), 3);
  eq([(function ([a, b]) { }).length, (function (a, b = 1, c) { }).length, (function (a, ...r) { }).length, (({ x }, y) => { }).length], [1, 1, 1, 2]);
  const gen = { *g() { } }; eq(Object.getPrototypeOf(gen.g.prototype) === Object.getPrototypeOf(function* () { }).prototype, true);
  let sets = 0; const spy = new Proxy({}, { set() { sets++; return true; } }); const saved = Object.getPrototypeOf(Number.prototype);
  Object.setPrototypeOf(Number.prototype, spy); (0).viaProxy = 1; Object.setPrototypeOf(Number.prototype, saved); eq(sets, 1);
  yi\u0065ld: for (;;) break yield;
  throws(() => new Function("var async; for (async of [1]) ;"), SyntaxError); new Function("for (async of => {}; false;) ;");
  eq(new Function("o", "with (o) { var k = delete o.k; } return [o.k, typeof k].join()")({ k: 1 }), "true,undefined");
  const trace = []; const env = new Proxy({ p: 1 }, { has(t, k) { if (typeof k === "string") trace.push("has:" + k); return Reflect.has(t, k); }, get(t, k) { trace.push("get:" + String(k)); return t[k]; } });
  eq([new Function("env", "with (env) { return p; }")(env), trace], [1, ["has:p", "get:Symbol(Symbol.unscopables)", "has:p", "get:p"]]);
});
done();
