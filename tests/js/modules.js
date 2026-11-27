// ES modules, loaded from an in-memory "file system" through the engine's loader (ECMA-262 16.2).
const files = {};
const log = [];
globalThis.log = log;
__modules.host.resolve = function (spec, referrer) {
  if (spec.startsWith("./")) return "mem:/" + spec.slice(2);
  if (spec.startsWith("mem:")) return spec;
  throw new TypeError("Cannot resolve '" + spec + "' from " + referrer);
};
__modules.host.fetch = (url, type) => (url in files ? Promise.resolve(type === "bytes" ? new TextEncoder().encode(files[url]).buffer : files[url])
  : Promise.reject(new TypeError("not found: " + url)));
function mod(name, src) { files["mem:/" + name] = src; }
async function load(name) { const r = await __modules.load("mem:/" + name); await __modules.run(r); return __modules.namespace(r); }
async function rejects(promise, type, msg) {
  try { await promise; } catch (e) {
    if (e instanceof type) { __pass++; return e; }
    __fail++; console.log("FAIL [" + __current + "] " + (msg || "") + " rejected with the wrong type: " + e); return e;
  }
  __fail++; console.log("FAIL [" + __current + "] " + (msg || "") + " did not reject");
}
function strictSet(o, k, v) { "use strict"; o[k] = v; }

(async function () {
  __current = "imports and exports";
  mod("a.js", `export const x = 1; export let count = 0; export function inc() { count++; }
    export default function hello() { return 'hi'; } export class K {} var hidden = 'not exported';`);
  mod("b.js", `import hello, { x, count, inc, K as Klass } from './a.js'; import * as A from './a.js';
    inc(); export const seen = [x, count, hello(), typeof Klass, A.x, A.count, hello.name, typeof hidden];
    inc(); export const later = () => count; export { x as renamed }; export var v = 'var'; v += '!';`);
  const B = await load("b.js");
  eq(B.seen, [1, 1, "hi", "function", 1, 1, "hello", "undefined"]);
  eq([B.later(), B.renamed, B.v], [2, 1, "var!"]);

  __current = "imports are live, read-only and hoisted";
  mod("c.js", `export const early = typeof inc; import { count, inc } from './a.js';
    export function tryAssign() { try { count = 5; return 'no error'; } catch (e) { return e.constructor.name; } }
    export function tryUpdate() { try { count++; return 'no error'; } catch (e) { return e.constructor.name; } }
    export function current() { return count; }`);
  const C = await load("c.js");
  eq([C.early, C.tryAssign(), C.tryUpdate(), C.current()], ["function", "TypeError", "TypeError", 2]);
  (await load("a.js")).inc();
  eq(C.current(), 3);

  __current = "namespace object";
  const A = await load("a.js");
  eq(Object.keys(A), ["K", "count", "default", "inc", "x"]);
  eq([Object.getPrototypeOf(A), A[Symbol.toStringTag], Object.prototype.toString.call(A), Object.isExtensible(A)], [null, "Module", "[object Module]", false]);
  eq(Object.getOwnPropertyDescriptor(A, "x"), { value: 1, writable: true, enumerable: true, configurable: false });
  eq(Object.getOwnPropertyDescriptor(A, Symbol.toStringTag), { value: "Module", writable: false, enumerable: false, configurable: false });
  eq(["x" in A, "nope" in A, A.nope, A.hidden, Object.getOwnPropertyDescriptor(A, "nope"), A.default()], [true, false, undefined, undefined, undefined, "hi"]);
  throws(() => strictSet(A, "x", 2), TypeError); throws(() => strictSet(A, "fresh", 2), TypeError);
  eq([delete A.x, delete A.nope, A.x, A === (await load("a.js"))], [false, true, 1, true]);
  eq(JSON.stringify(Object.entries(A).filter(e => typeof e[1] !== "function")), '[["count",3],["x",1]]');

  __current = "re-exports";
  mod("d1.js", `export const p = 'p1', shared = 's1'; export default 'd1 default';`);
  mod("d2.js", `export const q = 'q2', shared = 's2';`);
  mod("d.js", `export * from './d1.js'; export * from './d2.js'; export { x as ex, default as adef } from './a.js';
    export * as ANS from './a.js'; import { p } from './d1.js'; export { p as viaImport, p as "string name" };`);
  const D = await load("d.js");
  eq(Object.keys(D), ["ANS", "adef", "ex", "p", "q", "string name", "viaImport"]);
  eq([D.p, D.q, D.ex, D.adef(), D.ANS.x, D.ANS === A, D.viaImport, D.shared, D.default, D["string name"]], ["p1", "q2", 1, "hi", 1, true, "p1", undefined, undefined, "p1"]);
  mod("d3.js", `export * from './d.js'; export * from './d1.js'; import { "string name" as sn } from './d.js'; export { sn };`);
  const D3 = await load("d3.js");
  eq([D3.p, D3.sn, Object.keys(D3).length], ["p1", "p1", 8]);

  __current = "errors found before anything runs";
  log.length = 0;
  mod("e1.js", `log.push('e1 ran'); import { missing } from './a.js';`);
  mod("e2.js", `import { shared } from './d.js';`);
  mod("e3.js", `import './nowhere.js'; log.push('e3 ran');`);
  mod("e4.js", `export const a = 1; export const a = 2;`);
  mod("e5.js", `export { nothing };`);
  mod("e6.js", `const z = 1; export { z, z as y, z };`);
  mod("e7.js", `function f() { import x from './a.js'; }`);
  mod("e8.js", `export default 1; export default 2;`);
  mod("e9.js", `import { x } from './a.js'; let x = 2;`);
  mod("e10.js", `export { default } from './d2.js';`);
  mod("e11.js", `import './e1.js'; log.push('e11 ran');`);
  mod("e12.js", `if (true) { export const inner = 1; }`);
  const e1 = await rejects(load("e1.js"), SyntaxError, "missing export");
  ok(/does not provide an export named 'missing'/.test(e1.message), e1.message);
  const e2 = await rejects(load("e2.js"), SyntaxError, "ambiguous export");
  ok(/conflicting star exports/.test(e2.message), e2.message);
  await rejects(load("e3.js"), TypeError, "module that cannot be fetched");
  for (const n of [4, 5, 6, 7, 8, 9, 12]) await rejects(load("e" + n + ".js"), SyntaxError, "e" + n);
  await rejects(load("e10.js"), SyntaxError, "re-export of a missing default");
  await rejects(load("e11.js"), SyntaxError, "dependency that does not link");
  eq(log, []);
  eq((await load("a.js")).x, 1, "a.js still works after failed links");

  __current = "cycles";
  mod("cy1.js", `import { fromTwo, readOne } from './cy2.js'; export function one() { return 'one'; } export let late = 'late';
    export const result = fromTwo(); export const tdz = readOne;`);
  mod("cy2.js", `import { one, late } from './cy1.js'; export function fromTwo() { return one() + '+two'; }
    export let early; try { late; early = 'no tdz'; } catch (e) { early = e.constructor.name; }
    export const callOne = one(); export function readOne() { return late; }`);
  const C1 = await load("cy1.js"), C2 = await load("cy2.js");
  eq([C1.result, C1.tdz(), C2.early, C2.callOne], ["one+two", "late", "ReferenceError", "one"]);
  mod("self.js", `import * as me from './self.js'; export let viaKeys; try { Object.keys(me); viaKeys = 'no tdz'; } catch (e) { viaKeys = e.constructor.name; } export const keys = Reflect.ownKeys(me).filter(k => typeof k === 'string'); export let ok; try { me.keys2; ok = 'no tdz'; } catch (e) { ok = e.constructor.name; } export const keys2 = 1;`);
  const S = await load("self.js");
  eq([S.keys, S.ok, S.viaKeys], [["keys", "keys2", "ok", "viaKeys"], "ReferenceError", "ReferenceError"]);
  mod("star1.js", `export * from './star2.js'; export const s1 = 1;`);
  mod("star2.js", `export * from './star1.js'; export const s2 = 2;`);
  eq(Object.keys(await load("star1.js")), ["s1", "s2"]);

  __current = "evaluation order";
  log.length = 0;
  mod("o1.js", `log.push('o1');`);
  mod("o2.js", `import './o1.js'; log.push('o2');`);
  mod("o3.js", `log.push('o3'); import './o2.js'; import './o1.js'; import './o4.js';`);
  mod("o4.js", `log.push('o4');`);
  await load("o3.js"); await load("o2.js"); await load("o3.js");
  eq(log, ["o1", "o2", "o4", "o3"]);

  __current = "evaluation errors";
  log.length = 0;
  mod("t1.js", `log.push('t1'); throw new RangeError('boom');`);
  mod("t2.js", `import './t1.js'; log.push('t2');`);
  const t = await rejects(load("t2.js"), RangeError);
  const again = await load("t2.js").catch(e => e), direct = await load("t1.js").catch(e => e);
  eq([again === t, direct === t, log.join()], [true, true, "t1"]);

  __current = "top-level await";
  log.length = 0;
  mod("w1.js", `log.push('w1 start'); export const v = await new Promise(r => setTimeout(() => r(7), 5)); log.push('w1 end');`);
  mod("w2.js", `log.push('w2'); export const two = 2;`);
  mod("w3.js", `import { v } from './w1.js'; import { two } from './w2.js'; log.push('w3 ' + (v + two)); export const sum = v + two;`);
  mod("w0.js", `import { sum } from './w3.js'; import './w6.js'; log.push('w0'); export { sum };`);
  mod("w6.js", `log.push('w6');`);
  const W = await load("w0.js");
  eq([W.sum, log.join()], [9, "w1 start,w2,w6,w1 end,w3 9,w0"]);
  mod("w4.js", `log.push('w4'); await Promise.reject(new EvalError('tla'));`);
  mod("w5.js", `import './w4.js'; log.push('w5');`);
  log.length = 0;
  const w = await rejects(load("w5.js"), EvalError);
  eq([log.join(), (await load("w5.js").catch(e => e)) === w], ["w4", true]);
  mod("w7.js", `export let state = 'before'; if (false) await 0; state = 'after';`);
  eq((await load("w7.js")).state, "after");
  mod("w8.js", `export const seen = []; for await (const x of [Promise.resolve(1), 2]) seen.push(x); export default class { static { seen.push('static block'); } }`);
  eq((await load("w8.js")).seen, [1, 2, "static block"]);

  __current = "import() and import.meta";
  mod("dy.js", `export const ns = await import('./a.js'); export const same = ns === (await import('./a.js'));
    export const fail = await import('./nowhere.js').catch(e => e.constructor.name);
    export const meta = import.meta.url, resolved = import.meta.resolve('./zzz.js'), proto = Object.getPrototypeOf(import.meta);
    export const sameMeta = import.meta === import.meta; export const lazy = () => import('./lazy.js');`);
  mod("lazy.js", `log.push('lazy'); export default 'lazy value';`);
  log.length = 0;
  const DY = await load("dy.js");
  eq([DY.ns === A, DY.same, DY.fail, DY.meta, DY.resolved, DY.proto, DY.sameMeta, log.length], [true, true, "TypeError", "mem:/dy.js", "mem:/zzz.js", null, true, 0]);
  eq([(await DY.lazy()).default, (await DY.lazy()).default, log.join()], ["lazy value", "lazy value", "lazy"]);
  // code made by eval or Function belongs to the script that made it: its import() resolves against that file (9.4.1)
  mod("ev.js", `export const viaEval = await eval("import('./a.js')"), viaFn = await new Function("return import('./a.js')")();`);
  const referrers = [], plainResolve = __modules.host.resolve;
  __modules.host.resolve = (spec, referrer) => (referrers.push(referrer), plainResolve(spec, referrer));
  const EV = await load("ev.js"); __modules.host.resolve = plainResolve;
  eq([EV.viaEval === A, EV.viaFn === A, referrers], [true, true, ["mem:/ev.js", "mem:/ev.js"]]);
  // importing bytes: a Uint8Array over an immutable ArrayBuffer
  mod("usebytes.js", `import raw from './notes.bin' with { type: 'bytes' }; export { raw };`); mod("notes.bin", "AZ");
  const raw = (await load("usebytes.js")).raw;
  eq([raw instanceof Uint8Array, [...raw], raw.buffer.immutable, (await import("mem:/notes.bin", { with: { type: "bytes" } })).default === raw], [true, [65, 90], true, true]);
  // ShadowRealm.prototype.importValue loads the module inside that realm, found the way this realm finds modules
  mod("sv.js", `export const n = 7, o = {}; export function twice(x) { return x * 2; }`);
  const shadow = new ShadowRealm(), twice = await shadow.importValue("./sv.js", "twice");
  eq([await shadow.importValue("./sv.js", "n"), twice(4), Object.getPrototypeOf(twice) === Function.prototype], [7, 8, true]);
  await rejects(shadow.importValue("./sv.js", "o"), TypeError); await rejects(shadow.importValue("./sv.js", "missing"), TypeError);
  await rejects(shadow.importValue("./nowhere.js", "n"), TypeError);
  const viaScript = await import("mem:/a.js");
  ok(viaScript === A, "import() from a classic script");
  const p = import("mem:/a.js"); ok(p instanceof Promise);
  await rejects(import("mem:/nowhere.js"), TypeError); await rejects(import("bare"), TypeError); await rejects(import(Symbol("s")), TypeError);
  await rejects(import("mem:/a.js", 5), TypeError); await rejects(import("mem:/a.js", { with: { type: "json" } }), SyntaxError); await rejects(import("mem:/e1.js"), SyntaxError);
  await rejects(import("mem:/a.js", { with: { type: "css" } }), TypeError); await rejects(import("mem:/a.js", { with: { other: "x" } }), SyntaxError);
  mod("data.json", '{"list": [1, 2], "__proto__": 5}'); mod("notes.txt", "plain text");
  mod("usejson.js", `import data from './data.json' with { type: 'json' }; import again from './data.json' with { type: "json" };
    import * as ns from './data.json' with { type: 'json' }; import text from './notes.txt' with { type: 'text' };
    export const out = [data.list.join(), data === again, Object.keys(ns).join(), Object.hasOwn(data, '__proto__'), text];`);
  eq((await load("usejson.js")).out, ["1,2", true, "default", true, "plain text"]);
  eq((await import("mem:/data.json", { with: { type: "json" } })).default.list, [1, 2]);
  mod("badattr.js", `import x from './data.json' with { kind: 'json' };`); await rejects(load("badattr.js"), SyntaxError);
  eq((await import("mem:/a.js", {})).x, 1); eq((await import("mem:/a.js", undefined)).x, 1);
  let order = []; const pending = import({ toString() { order.push("toString"); return "mem:/a.js"; } }); order.push("after"); await pending; eq(order, ["toString", "after"]);

  __current = "default exports";
  mod("df1.js", `export default 40 + 2;`);
  mod("df2.js", `export default function () { return 'anon'; }`);
  mod("df3.js", `export default class { static who() { return 'cls'; } }`);
  mod("df4.js", `export default async function named() {}`);
  mod("df5.js", `export default (a, b) => a + b;`);
  mod("df6.js", `export default class Named {} export const inner = Named;`);
  mod("df7.js", `import f from './df8.js'; export const early = f();`);
  mod("df8.js", `import './df7.js'; export default function () { return 'hoisted default'; }`);
  mod("df.js", `import a from './df1.js'; import b from './df2.js'; import c from './df3.js'; import d from './df4.js'; import e from './df5.js';
    import f, { inner } from './df6.js'; import { early } from './df7.js'; import './df8.js';
    export const all = [a, b(), b.name, c.who(), c.name, d.name, e(1, 2), e.name, f === inner, f.name, early];`);
  eq((await load("df.js")).all, [42, "anon", "default", "cls", "default", "named", 3, "default", true, "Named", "hoisted default"]);

  __current = "module code";
  mod("s.js", `export const self = this; export const inner = (function () { return this; })();
    export const awaitIsKeyword = (() => { try { return new Function('await', 'return 1')(); } catch (e) { return 'reserved'; } })();
    var top = 1; export const leaked = 'top' in globalThis; export { top as "a b" };`);
  const SM = await load("s.js");
  eq([SM.self, SM.inner, SM.leaked, SM["a b"]], [undefined, undefined, false, 1]);
  done();
})().catch(e => { console.log("FAIL main rejected: " + (e && e.stack || e)); });
