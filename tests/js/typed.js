test("typed arrays", function () {
  const u8 = new Uint8Array(4);
  eq([u8.length, u8.byteLength, u8[0], u8[9], u8 instanceof Uint8Array, Array.isArray(u8), typeof Uint8Array], [4, 4, 0, undefined, true, false, "function"]);
  u8[0] = 257; u8[1] = -1; u8[2] = 3.9; u8[3] = "7"; u8[4] = 1;
  eq(Array.from(u8), [1, 255, 3, 7]); eq(u8.length, 4);
  const c = new Uint8ClampedArray([300, -5, 1.5, 2.5, 0.4]); eq(Array.from(c), [255, 0, 2, 2, 0]);
  const i16 = new Int16Array([32768, -32769, 5]); eq(Array.from(i16), [-32768, 32767, 5]);
  const f32 = new Float32Array([1.5, 0.1]); eq(f32[0], 1.5); ok(Math.abs(f32[1] - 0.1) < 1e-7 && f32[1] !== 0.1);
  const f64 = new Float64Array([0.1, NaN, -0]); eq([f64[0], isNaN(f64[1]), Object.is(f64[2], -0)], [0.1, true, true]);
  const u32 = new Uint32Array([4294967295, -1]); eq(Array.from(u32), [4294967295, 4294967295]); eq(new Int32Array([2147483648])[0], -2147483648);
  eq([Uint8Array.BYTES_PER_ELEMENT, Float64Array.BYTES_PER_ELEMENT, new Int32Array(1).BYTES_PER_ELEMENT], [1, 8, 4]);
  eq(Array.from(new Uint8Array(new Set([5, 6]))), [5, 6]); eq(Array.from(Uint16Array.from("123", Number)), [1, 2, 3]); eq(Array.from(Int8Array.of(1, 2)), [1, 2]);
  eq([...new Uint8Array([9, 8])], [9, 8]); eq(Object.keys(new Uint8Array(2)), ["0", "1"]); eq(JSON.stringify(new Uint8Array([1, 2])), '{"0":1,"1":2}');
  throws(() => Uint8Array(2), TypeError); throws(() => new Uint8Array(-1), RangeError); throws(() => new Int32Array(new ArrayBuffer(6)), RangeError);
});
test("buffers and views share bytes", function () {
  const buf = new ArrayBuffer(8); eq([buf.byteLength, buf instanceof ArrayBuffer, ArrayBuffer.isView(buf), ArrayBuffer.isView(new Uint8Array(1))], [8, true, false, true]);
  const bytes = new Uint8Array(buf), words = new Uint32Array(buf), tail = new Uint8Array(buf, 4, 2);
  words[0] = 0x04030201; eq(Array.from(bytes.subarray(0, 4)), [1, 2, 3, 4]); tail[0] = 0xAA; eq(bytes[4], 0xAA); eq([tail.byteOffset, tail.length, tail.buffer === buf], [4, 2, true]);
  const sub = bytes.subarray(1, 3); sub[0] = 99; eq(bytes[1], 99); const copy = bytes.slice(1, 3); copy[0] = 7; eq(bytes[1], 99);
  eq(new Uint8Array(buf.slice(0, 2)).length, 2);
  const dv = new DataView(buf); dv.setUint16(0, 0x1234); eq([bytes[0], bytes[1], dv.getUint16(0), dv.getUint16(0, true)], [0x12, 0x34, 0x1234, 0x3412]);
  dv.setFloat64(0, Math.PI, true); eq(new Float64Array(buf)[0], Math.PI); dv.setInt8(7, -2); eq([dv.getInt8(7), dv.getUint8(7), dv.byteLength], [-2, 254, 8]);
  throws(() => dv.getUint32(6), RangeError);
});
test("typed array methods", function () {
  const a = new Int32Array([5, 1, 4, 2, 3]);
  eq(Array.from(a.slice().sort()), [1, 2, 3, 4, 5]); eq(Array.from(new Float64Array([10, 9, 1]).sort()), [1, 9, 10]); eq(Array.from(a.slice().sort((x, y) => y - x)), [5, 4, 3, 2, 1]);
  eq(Array.from(a.map(x => x * 2)), [10, 2, 8, 4, 6]); ok(a.map(x => x) instanceof Int32Array); eq(Array.from(a.filter(x => x > 2)), [5, 4, 3]);
  eq([a.indexOf(4), a.includes(9), a.join("-"), a.reduce((s, x) => s + x, 0), a.find(x => x < 3), a.some(x => x > 4), a.every(x => x > 1), a.at(-1)], [2, false, "5-1-4-2-3", 15, 1, true, false, 3]);
  let seen = 0; a.forEach(x => seen += x); eq(seen, 15); eq(Array.from(a.slice().reverse()), [3, 2, 4, 1, 5]); eq(Array.from(new Uint8Array(4).fill(7, 1, 3)), [0, 7, 7, 0]);
  const t = new Uint8Array(5); t.set([1, 2, 3], 1); eq(Array.from(t), [0, 1, 2, 3, 0]); t.set(new Uint8Array([9]), 4); eq(t[4], 9); throws(() => t.set([1, 2], 4), RangeError);
  eq(Array.from(new Uint8Array([1, 2, 3, 4, 5]).copyWithin(0, 3)), [4, 5, 3, 4, 5]); eq(String(new Uint8Array([1, 2])), "1,2"); eq(Array.from(t.entries())[1], [1, 1]);
  eq(Object.prototype.toString.call(new Uint8Array(1)), "[object Uint8Array]");
  let sum = 0; const big = new Float64Array(200000); for (let i = 0; i < big.length; i++) big[i] = i * 0.5; for (let i = 0; i < big.length; i++) sum += big[i]; eq(sum, 9999950000);
});
test("text encoding and random values", function () {
  const bytes = new TextEncoder().encode("hé 😀"); eq(Array.from(bytes), [104, 195, 169, 32, 240, 159, 152, 128]); ok(bytes instanceof Uint8Array);
  eq(new TextDecoder().decode(bytes), "hé 😀"); eq(new TextDecoder("utf-8").decode(bytes.buffer), "hé 😀"); eq(new TextDecoder().decode(new Uint8Array([0xEF, 0xBB, 0xBF, 65])), "A");
  const r = crypto.getRandomValues(new Uint8Array(32)); ok(r.some(x => x !== 0)); ok(crypto.getRandomValues(new Uint32Array(4)).join() !== crypto.getRandomValues(new Uint32Array(4)).join());
  ok(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(crypto.randomUUID()));
});
test("the shared TypedArray intrinsic", function () {
  const TA = Object.getPrototypeOf(Int8Array);
  eq([TA.name, Object.getPrototypeOf(Uint8Array) === TA, Object.getPrototypeOf(Int8Array.prototype) === TA.prototype, TA.prototype.toString === Array.prototype.toString, TA.prototype[Symbol.iterator] === TA.prototype.values], ["TypedArray", true, true, true, true]);
  throws(() => new TA(), TypeError); throws(() => TA.prototype.length, TypeError); throws(() => Int8Array(1), TypeError);
  eq([new Uint8Array(2)[Symbol.toStringTag], TA.prototype[Symbol.toStringTag], Object.getOwnPropertyNames(Int8Array.prototype).sort()], ["Uint8Array", undefined, ["BYTES_PER_ELEMENT", "constructor"]]);
  class Mine extends Uint8Array { } const m = new Mine([3, 1, 2]);
  ok(m.map(x => x * 2) instanceof Mine); ok(m.subarray(1) instanceof Mine); ok(m.toSorted() instanceof Uint8Array && !(m.toSorted() instanceof Mine));
  eq([[...m.toSorted()], [...m.toReversed()], [...m.with(0, 9)], [...m], m.at(-1), m.findLast(x => x > 1), m.findLastIndex(x => x > 5)], [[1, 2, 3], [2, 1, 3], [9, 1, 2], [3, 1, 2], 2, 2, -1]);
  eq([[...Float64Array.of(3, NaN, -0, 0, -1).sort()].map(x => Object.is(x, -0) ? "-0" : String(x)), [...new Int8Array([5, 1, 4]).sort((a, b) => b - a)]], [["-1", "-0", "0", "3", "NaN"], [5, 4, 1]]);
});
test("numeric keys on typed arrays", function () {
  const t = new Uint8Array([7, 8]);
  eq([t[5], t["-0"], t["1.5"], "1" in t, "2" in t, "-0" in t, "1.5" in t, delete t[5], Reflect.defineProperty(t, "2", { value: 1 }), Reflect.defineProperty(t, "0", { value: 9 }), t[0]], [undefined, undefined, undefined, true, false, false, false, true, false, true, 9]);
  t[9] = 1; t["1.5"] = 1; t.foo = 1; eq([Object.keys(t), Object.getOwnPropertyDescriptor(t, 1)], [["0", "1", "foo"], { value: 8, writable: true, enumerable: true, configurable: true }]);
  throws(() => (function () { "use strict"; delete t[0]; })(), TypeError); throws(() => Object.freeze(t), TypeError); ok(Object.isFrozen(Object.freeze(new Uint8Array(0))));
  const child = Object.create(t); child[5] = 1; eq([Object.keys(child), child[0]], [[], 9]);
  eq([new Uint8ClampedArray([1.5, 2.5, 300, -5, NaN]).join(), new Float16Array([1.337, 65520, 1e-8])[0], new Float16Array([65520])[0], Math.f16round(5.5), Math.f16round(2.00001)], ["2,2,255,0,0", 1.3369140625, Infinity, 5.5, 2]);
});
test("BigInt typed arrays", function () {
  const b = new BigInt64Array([1n, -1n, 2n ** 63n]); const u = new BigUint64Array(b);
  eq([String(b[2]), String(u[1]), typeof b[0], b.includes(-1n), b.indexOf(1n), String(b.toSorted()[0]), BigInt64Array.BYTES_PER_ELEMENT], ["-9223372036854775808", "18446744073709551615", "bigint", true, 0, "-9223372036854775808", 8]);
  throws(() => { b[0] = 1; }, TypeError); throws(() => new BigInt64Array(new Int8Array(1)), TypeError); throws(() => new Int8Array(b), TypeError); throws(() => b.fill(1), TypeError);
  const dv = new DataView(new ArrayBuffer(16)); dv.setBigInt64(0, -2n); dv.setBigUint64(8, 2n ** 64n - 1n, true); dv.setFloat16(2, 1.5);
  eq([String(dv.getBigInt64(8)), String(dv.getBigUint64(8, true)), dv.getUint8(0), dv.getFloat16(2), dv.getUint16(2)], ["-1", "18446744073709551615", 255, 1.5, 0x3E00]);
});
test("resizable, detached and immutable buffers", function () {
  const rab = new ArrayBuffer(4, { maxByteLength: 8 }); const tracking = new Uint8Array(rab), fixed = new Uint8Array(rab, 0, 4), dv = new DataView(rab, 2);
  eq([rab.resizable, rab.maxByteLength, tracking.length, new ArrayBuffer(1).resizable], [true, 8, 4, false]);
  rab.resize(8); eq([tracking.length, fixed.length, dv.byteLength, rab.byteLength], [8, 4, 6, 8]);
  rab.resize(2); eq([tracking.length, fixed.length, fixed.byteLength, fixed.byteOffset, fixed[0], [...tracking]], [2, 0, 0, 0, undefined, [0, 0]]);
  throws(() => fixed.fill(1), TypeError); throws(() => [...fixed], TypeError); throws(() => dv.getUint8(0), RangeError); throws(() => rab.resize(9), RangeError); throws(() => new ArrayBuffer(9, { maxByteLength: 8 }), RangeError);
  const ab = new ArrayBuffer(8); const view = new Uint8Array(ab); view.set([1, 2, 3]);
  const moved = ab.transfer(4); eq([ab.detached, ab.byteLength, view.length, view[0], moved.byteLength, [...new Uint8Array(moved)]], [true, 0, 0, undefined, 4, [1, 2, 3, 0]]);
  throws(() => view.at(0), TypeError); throws(() => ab.slice(0), TypeError); throws(() => new Uint8Array(ab), TypeError); throws(() => ab.transfer(), TypeError);
  eq([moved.transferToFixedLength(2).byteLength, new ArrayBuffer(2, { maxByteLength: 4 }).transfer().resizable, new ArrayBuffer(2, { maxByteLength: 4 }).transferToFixedLength().resizable], [2, true, false]);
  const frozen = new Uint8Array([1, 2, 3]).buffer.transferToImmutable(); const fv = new Uint8Array(frozen);
  eq([frozen.immutable, fv[1], Reflect.set(fv, 0, 9), fv[0], Object.getOwnPropertyDescriptor(fv, 0).writable, [...new Uint8Array(frozen.sliceToImmutable(1))]], [true, 2, false, 1, false, [2, 3]]);
  throws(() => fv.fill(0), TypeError); throws(() => new DataView(frozen).setUint8(0, 1), TypeError); throws(() => frozen.transfer(), TypeError); throws(() => (function () { "use strict"; fv[0] = 1; })(), TypeError);
  ok(Object.isFrozen(Object.freeze(fv)));
});
test("shared buffers, Atomics and byte text", function () {
  const sab = new SharedArrayBuffer(8, { maxByteLength: 16 }); const ia = new Int32Array(sab);
  eq([sab.growable, Atomics.add(ia, 0, 5), Atomics.sub(ia, 0, 2), Atomics.exchange(ia, 1, 7), Atomics.compareExchange(ia, 1, 7, 9), Atomics.load(ia, 1), Atomics.store(ia, 0, 2.9), Atomics.or(ia, 0, 5), Atomics.xor(ia, 0, 1), Atomics.and(ia, 0, 3), ia[0]], [true, 0, 5, 0, 7, 9, 2, 2, 7, 6, 2]);
  sab.grow(16); eq([sab.byteLength, ia.length, Atomics.isLockFree(4), Atomics.notify(ia, 0), Atomics.wait(ia, 0, 5), Atomics.wait(ia, 0, 2, 10), Object.prototype.toString.call(Atomics)], [16, 4, true, 0, "not-equal", "timed-out", "[object Atomics]"]);
  throws(() => sab.grow(8), RangeError); throws(() => Atomics.add(new Float64Array(1), 0, 1), TypeError); throws(() => Atomics.load(ia, 9), RangeError); throws(() => Atomics.wait(new Int32Array(4), 0, 0), TypeError);
  eq([Atomics.waitAsync(ia, 0, 9).value, Atomics.waitAsync(ia, 0, 2, 0).value, Atomics.waitAsync(ia, 0, 2).async], ["not-equal", "timed-out", true]);
  const bytes = new Uint8Array([72, 101, 108, 108, 111, 255, 254]);
  eq([bytes.toBase64(), bytes.toBase64({ alphabet: "base64url", omitPadding: true }), bytes.toHex(), [...Uint8Array.fromBase64("SGVsbG8=")], [...Uint8Array.fromBase64(" SGVs bG8 ")], [...Uint8Array.fromHex("00ff10")]], ["SGVsbG///g==", "SGVsbG___g", "48656c6c6ffffe", [72, 101, 108, 108, 111], [72, 101, 108, 108, 111], [0, 255, 16]]);
  const into = new Uint8Array(4); eq([into.setFromBase64("SGVsbG8="), [...into], into.setFromHex("0102"), [...into]], [{ read: 4, written: 3 }, [72, 101, 108, 0], { read: 4, written: 2 }, [1, 2, 108, 0]]);
  throws(() => Uint8Array.fromBase64("SGVsbG8", { lastChunkHandling: "strict" }), SyntaxError); throws(() => Uint8Array.fromHex("abc"), SyntaxError); throws(() => Uint8Array.fromBase64("a-b_", { alphabet: "base64" }), SyntaxError); throws(() => bytes.toBase64({ alphabet: "x" }), TypeError);
});
test("agents: threads with an engine each, sharing memory (9.7, 25.4)", function () {
  const ia = new Int32Array(new SharedArrayBuffer(16));
  const nextReport = () => { let r; while ((r = __agentGetReport()) === null) __agentSleep(1); return r; };
  const wakeOne = () => { let n; while ((n = Atomics.notify(ia, 0, 1)) === 0) __agentSleep(1); return n; };
  const body = "__agentReceive(function (sab) { var ia = new Int32Array(sab); Atomics.add(ia, 1, 1);" +
    " __agentReport(NAME + ' ' + Atomics.wait(ia, 0, 0, LIMIT)); __agentLeaving(); });";
  __agentStart("var NAME = 'first', LIMIT = Infinity; " + body); __agentStart("var NAME = 'second', LIMIT = Infinity; " + body);
  __agentBroadcast(ia.buffer);
  while (Atomics.load(ia, 1) !== 2);  // both agents run, and their additions arrive in this agent's view of the memory
  eq(wakeOne(), 1); const one = nextReport(); eq(wakeOne(), 1); const two = nextReport();
  eq([[one, two].sort(), Atomics.notify(ia, 0)], [["first ok", "second ok"], 0]);
  __agentStart("var NAME = 'third', LIMIT = 20; " + body); __agentBroadcast(ia.buffer);
  eq([nextReport(), Atomics.load(ia, 1)], ["third timed-out", 3]);
});
done();
