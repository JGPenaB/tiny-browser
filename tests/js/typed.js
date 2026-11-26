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
done();
