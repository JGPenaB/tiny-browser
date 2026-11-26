// BigInt: literals, operators, conversions and the BigInt built-in (ECMA-262 6.1.6.2, 21.2).
test("literals and typeof", function () {
  eq([typeof 1n, typeof Object(1n), String(123n), `${-5n}`, "" + 0x1Fn, 0b101n === 5n, 0o17n === 15n, 1_000n === 1000n], ["bigint", "object", "123", "-5", "31", true, true, true]);
  eq(String(123456789012345678901234567890n), "123456789012345678901234567890");
  throws(() => eval("1.5n"), SyntaxError); throws(() => eval("1e3n"), SyntaxError); throws(() => eval("01n"), SyntaxError);
  eq([({ 1n: "a" })[1], [10, 20, 30][2n], Object.keys({ 12345678901234567890n: 1 })[0]], ["a", 30, "12345678901234567890"]);
});
test("arithmetic", function () {
  eq(String(2n ** 100n), "1267650600228229401496703205376");
  eq(String(123456789012345678901234567890n * 987654321098765432109876543210n), "121932631137021795226185032733622923332237463801111263526900");
  eq(String(121932631137021795226185032733622923332237463801111263526900n / 987654321098765432109876543210n), "123456789012345678901234567890");
  eq(String(10n ** 30n % 7n ** 20n), String(BigInt("1000000000000000000000000000000") - (10n ** 30n / 7n ** 20n) * 7n ** 20n));
  eq([7n / 2n, -7n / 2n, 7n % -2n, -7n % 2n, 0n * -1n].map(String), ["3", "-3", "1", "-1", "0"]);
  eq([5n + 3n, 5n - 8n, -5n + 3n, -(3n), - -3n].map(String), ["8", "-3", "-2", "-3", "3"]);
  let x = 9007199254740993n; x++; eq(String(x), "9007199254740994"); x--; --x; eq(String(x), "9007199254740992"); eq(String(x++), "9007199254740992");
  throws(() => 1n / 0n, RangeError); throws(() => 1n % 0n, RangeError); throws(() => 2n ** -1n, RangeError);
  throws(() => 1n + 1, TypeError); throws(() => 1 * 1n, TypeError); throws(() => +1n, TypeError); throws(() => 1n >>> 0n, TypeError);
  eq([(-1n) ** 3n, 0n ** 0n, 1n ** 1000000000000n].map(String), ["-1", "1", "1"]);
});
test("bitwise and shifts", function () {
  eq([5n & 3n, 5n | 3n, 5n ^ 3n, ~5n, ~-1n, -5n & 3n, -5n | 3n, -5n ^ 3n, -1n & 0xFFFFFFFFFFFFFFFFFn].map(String), ["1", "7", "6", "-6", "0", "3", "-5", "-8", "295147905179352825855"]);
  eq([1n << 100n, (1n << 100n) >> 98n, -5n >> 1n, -1n >> 100n, 5n >> 100n, 1n << -1n, 8n >> -2n, -8n >> 3n, -9n >> 3n].map(String),
    ["1267650600228229401496703205376", "4", "-3", "-1", "0", "0", "32", "-1", "-2"]);
});
test("comparison and equality", function () {
  eq([1n < 2n, 2n < 1n, -1n < 1n, 1n < 1.5, 2n > 1.5, 1n <= 1, 1n >= 1, 1n < "2", "1" < 2n, 1n < "x", 1n > NaN, 1n < Infinity, -(2n ** 2000n) > -Infinity], [true, false, true, true, true, true, true, true, true, false, false, true, true]);
  eq([1n == 1, 1 == 1n, 1n == "1", "0x10" == 16n, 1n == true, 1n === 1, 1n === 1n, 0n == "", 1n == 1.5, 1n == "1.0", 2n ** 64n == 2 ** 64, 2n ** 64n + 1n == 2 ** 64], [true, true, true, true, true, false, true, true, false, false, true, false]);
  eq([Object.is(0n, -0n), [1n, 2n].includes(2n), [1n, 2n].indexOf(2n), new Set([1n, 1n, 2n]).size, new Map([[10n ** 30n, "big"]]).get(10n ** 30n)], [true, true, 1, 2, "big"]);
  eq([!!0n, !!1n, 0n ? 1 : 2, 0n || "f", 1n && "t"], [false, true, 2, "f", "t"]);
  eq([3n, 1n, 2n, -1n].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0)).map(String), ["-1", "1", "2", "3"]);
});
test("conversions", function () {
  eq([Number(2n ** 53n + 1n), Number(-(2n ** 1024n)), Number(2n ** 1023n) === 2 ** 1023, Number(9007199254740993n), Number(9007199254740995n), parseInt(12n), Number(0n)], [9007199254740992, -Infinity, true, 9007199254740992, 9007199254740996, 12, 0]);
  eq([BigInt(10), BigInt("  42 "), BigInt("-12"), BigInt("0b11"), BigInt(true), BigInt(2 ** 80), BigInt(""), BigInt(Number.MAX_SAFE_INTEGER), BigInt("0XfF")].map(String), ["10", "42", "-12", "3", "1", "1208925819614629174706176", "0", "9007199254740991", "255"]);
  throws(() => BigInt("-0x0"), SyntaxError);
  throws(() => BigInt(1.5), RangeError); throws(() => BigInt(NaN), RangeError); throws(() => BigInt("1n"), SyntaxError); throws(() => BigInt("1.0"), SyntaxError);
  throws(() => BigInt(undefined), TypeError); throws(() => BigInt(null), TypeError); throws(() => BigInt(Symbol()), TypeError); throws(() => new BigInt(1), TypeError);
  throws(() => Math.max(1n), TypeError); throws(() => Number.prototype.toString.call(1n), TypeError); throws(() => isNaN(1n), TypeError);
  eq([(255n).toString(16), (-255n).toString(2), (35n).toString(36), (10n ** 20n).toString(7), 1n.toString(), Object(7n).valueOf() === 7n, (5n).toLocaleString()], ["ff", "-11111111", "z", "344015313561621001452562", "1", true, "5"]);
  throws(() => (1n).toString(1), RangeError); throws(() => BigInt.prototype.toString.call(1), TypeError);
  eq([Object.prototype.toString.call(1n), BigInt.prototype[Symbol.toStringTag], BigInt.length, BigInt.name], ["[object BigInt]", "BigInt", 1, "BigInt"]);
  throws(() => JSON.stringify({ a: 1n }), TypeError);
  BigInt.prototype.toJSON = function () { return this.toString() + "n"; }; eq(JSON.stringify({ a: 1n }), '{"a":"1n"}'); delete BigInt.prototype.toJSON;
});
test("asIntN and asUintN", function () {
  eq([BigInt.asUintN(8, 257n), BigInt.asUintN(8, -1n), BigInt.asUintN(64, -1n), BigInt.asUintN(0, 5n), BigInt.asUintN(3, 8n), BigInt.asUintN(200, 5n), BigInt.asUintN(65, -1n)].map(String), ["1", "255", "18446744073709551615", "0", "0", "5", "36893488147419103231"]);
  eq([BigInt.asIntN(8, 127n), BigInt.asIntN(8, 128n), BigInt.asIntN(8, 255n), BigInt.asIntN(8, -128n), BigInt.asIntN(8, -129n), BigInt.asIntN(64, 2n ** 63n), BigInt.asIntN(64, 2n ** 64n - 1n), BigInt.asIntN(1, 1n), BigInt.asIntN(0, 1n), BigInt.asIntN(2 ** 53 - 1, -5n), BigInt.asIntN(32, -(2n ** 31n)), BigInt.asIntN(33, 2n ** 32n)].map(String),
    ["127", "-128", "-1", "-128", "127", "-9223372036854775808", "-1", "-1", "0", "-5", "-2147483648", "-4294967296"]);
  throws(() => BigInt.asIntN(-1, 0n), RangeError); throws(() => BigInt.asUintN(8, 1), TypeError);
});
done();
