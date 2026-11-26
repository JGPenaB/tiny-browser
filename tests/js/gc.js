test("garbage collection under load", function () {
  let keep = [];
  for (let round = 0; round < 40; round++) {
    const junk = [];
    for (let i = 0; i < 20000; i++) junk.push({ id: i, name: "item" + i, tags: [i, i + 1], fn: () => i });
    keep.push(junk[round].fn, junk[19999].name);
    if (keep.length > 20) keep = keep.slice(10);
  }
  eq(keep[keep.length - 1], "item19999"); eq(typeof keep[keep.length - 2], "function"); eq(keep[keep.length - 2](), 39);
  const m = new Map(); for (let i = 0; i < 100000; i++) { m.set("k" + (i % 500), { v: i }); } eq(m.size, 500); eq(m.get("k499").v, 99999);
  let s = ""; for (let i = 0; i < 20000; i++) s += String.fromCharCode(97 + (i % 26)); eq(s.length, 20000); eq(s.slice(-3), "def");
  function Node(v, next) { this.v = v; this.next = next; } let list = null; for (let i = 0; i < 200000; i++) list = new Node(i, i % 1000 ? list : null);
  let len = 0; for (let n = list; n; n = n.next) len++; eq(len, 1000);
  const closures = []; for (let i = 0; i < 50000; i++) { const big = [i, i, i]; closures[i % 100] = () => big[0]; } eq(closures[99](), 49999);
  function* g() { for (let i = 0; i < 30000; i++) yield { i }; } let last; for (const o of g()) last = o; eq(last.i, 29999);
  const parts = []; for (let i = 0; i < 30000; i++) parts.push(JSON.parse(JSON.stringify({ a: [i, { b: "x" + i }] })).a[1].b); eq(parts[29999], "x29999");
  const sorted = Array.from({ length: 50000 }, (_, i) => ({ k: (i * 7919) % 50000 })).sort((a, b) => a.k - b.k); eq([sorted[0].k, sorted[49999].k], [0, 49999]);
});
test("speed sanity", function () {
  const t0 = Date.now(); let x = 0; for (let i = 0; i < 3000000; i++) { x = (x + i * 2) % 1000003; } const loopMs = Date.now() - t0;
  function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } const t1 = Date.now(); eq(fib(27), 196418); const fibMs = Date.now() - t1;
  const t2 = Date.now(); const arr = []; for (let i = 0; i < 300000; i++) arr.push(i * 2); const total = arr.map(v => v + 1).filter(v => v % 3 == 0).reduce((a, b) => a + b, 0); const arrMs = Date.now() - t2;
  const t3 = Date.now(); const o = {}; for (let i = 0; i < 200000; i++) o["key" + (i % 1000)] = i; let str = ""; for (let i = 0; i < 100000; i++) str += "ab"; const strMs = Date.now() - t3;
  console.log("timings ms: loop3M=" + loopMs + " fib27=" + fibMs + " array300k=" + arrMs + " props+strings=" + strMs);
  ok(typeof x === "number" && total > 0 && str.length === 200000); ok(loopMs < 3000 && fibMs < 3000, "too slow");
});
done();
