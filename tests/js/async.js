const order = [];
test("promise basics (sync part)", function () {
  const p = Promise.resolve(1); ok(p instanceof Promise); eq(typeof p.then, "function"); eq(Object.prototype.toString.call(p), "[object Promise]");
  throws(() => Promise(), TypeError); throws(() => new Promise(), TypeError);
});
order.push("sync start");
setTimeout(() => order.push("timeout 0"), 0);
setTimeout(() => order.push("timeout 10"), 10);
Promise.resolve().then(() => order.push("micro 1")).then(() => order.push("micro 3"));
queueMicrotask(() => order.push("micro 2"));
(async () => { order.push("async body"); await null; order.push("after await"); })();
order.push("sync end");

const results = {};
function sleep(ms, v) { return new Promise(r => setTimeout(() => r(v), ms)); }
async function main() {
  await sleep(25);
  results.chain = await Promise.resolve(1).then(v => v + 1).then(v => { throw new Error("e" + v); }).catch(e => e.message).finally(() => { results.fin = true; });
  results.all = await Promise.all([1, sleep(5, 2), Promise.resolve(3)]);
  results.race = await Promise.race([sleep(20, "slow"), sleep(1, "fast")]);
  results.settled = (await Promise.allSettled([Promise.reject(new Error("no")), 5])).map(r => r.status);
  results.any = await Promise.any([Promise.reject(1), sleep(2, "any-ok")]);
  try { await Promise.any([Promise.reject(1)]); } catch (e) { results.anyErr = e instanceof AggregateError && e.errors.length; }
  try { await Promise.all([sleep(1), Promise.reject("rej")]); } catch (e) { results.allRej = e; }
  try { await (async () => { throw new TypeError("async throw"); })(); } catch (e) { results.asyncThrow = e.message; }
  results.thenable = await { then(res) { res("thenable"); } };
  results.nested = await new Promise(r => r(new Promise(r2 => r2("inner"))));
  async function loop() { let s = 0; for (let i = 0; i < 5; i++) s += await sleep(0, i); return s; }
  results.loop = await loop();
  class Svc { async get(x) { return x * 2; } static async make() { return new Svc(); } }
  results.cls = await (await Svc.make()).get(21);
  const arrow = async x => { const y = await x; return y + 1; }; results.arrow = await arrow(Promise.resolve(1));
  results.seq = []; for (const p of [sleep(3, "a"), sleep(1, "b")]) results.seq.push(await p);
  results.tryFinally = await (async () => { try { return await sleep(1, "tf"); } finally { results.tfFin = true; } })();
  const wr = Promise.withResolvers(); setTimeout(() => wr.resolve("wr"), 1); results.withResolvers = await wr.promise;
  let ticks = 0; const iv = setInterval(() => { if (++ticks === 3) clearInterval(iv); }, 1); await sleep(40); results.interval = ticks;
  const cancelled = setTimeout(() => { results.cancelled = "should not run"; }, 1); clearTimeout(cancelled);
  results.finallyPass = await Promise.resolve("keep").finally(() => "ignored");
  return "main done";
}
main().then(v => {
  test("event loop order", function () { eq(order, ["sync start", "async body", "sync end", "micro 1", "micro 2", "after await", "micro 3", "timeout 0", "timeout 10"]); });
  test("async results", function () {
    eq(v, "main done");
    eq(results, { fin: true, chain: "e2", all: [1, 2, 3], race: "fast", settled: ["rejected", "fulfilled"], any: "any-ok", anyErr: 1, allRej: "rej", asyncThrow: "async throw",
      thenable: "thenable", nested: "inner", loop: 10, cls: 42, arrow: 2, seq: ["a", "b"], tfFin: true, tryFinally: "tf", withResolvers: "wr", interval: 3, finallyPass: "keep" });
  });
  done();
}, e => { console.log("FAIL main rejected: " + (e && e.stack || e)); });
