var __pass = 0, __fail = 0, __current = "";
function show(v) {
  if (typeof v === "string") return JSON.stringify(v);
  if (typeof v === "number") return Object.is(v, -0) ? "-0" : String(v);
  if (v === undefined) return "undefined";
  if (typeof v === "function") return "function";
  if (typeof v === "symbol") return v.toString();
  try { return JSON.stringify(v, function (k, x) { return x === undefined ? "__undef__" : (typeof x === "number" && x !== x) ? "__NaN__" : x; }); }
  catch (e) { return String(v); }
}
function eq(actual, expected, msg) {
  var a = show(actual), e = show(expected);
  if (a === e) { __pass++; return; }
  __fail++;
  console.log("FAIL [" + __current + "] " + (msg || "") + "\n   expected: " + e + "\n   actual:   " + a);
}
function ok(cond, msg) { eq(!!cond, true, msg); }
function throws(fn, type, msg) {
  try { fn(); } catch (e) {
    if (!type || e instanceof type) { __pass++; return; }
    __fail++; console.log("FAIL [" + __current + "] " + (msg || "") + " threw wrong type: " + e); return;
  }
  __fail++; console.log("FAIL [" + __current + "] " + (msg || "") + " did not throw");
}
function test(name, fn) {
  __current = name;
  try { fn(); } catch (e) { __fail++; console.log("FAIL [" + name + "] threw " + (e && e.stack ? e.stack : e)); }
}
function done() { console.log(__fail ? "FAILED " + __fail + " of " + (__pass + __fail) : "ok " + __pass); }
