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
done();
