// Temporal (TC39 proposal, https://tc39.es/proposal-temporal/): the whole API written in JavaScript on top of the
// engine. tools/gen_temporal.py turns these files into temporal_js.h; the engine compiles them the first time the
// global `Temporal` is read (prelude.cpp temporalNatives). Section numbers in comments are those of the proposal.
//
// Supported: the ISO 8601 calendar; UTC, offset time zones (+01:00) and the named time zones of the IANA database,
// whose rules come from ICU (natives.icu, icu.cpp).
(function (natives) {
'use strict';
var icu = natives.icu, protoFor = natives.protoFor, toPrimitiveString = natives.toPrimitiveString, toBigIntNative = natives.toBigInt;
var $Object = Object, $TypeError = TypeError, $RangeError = RangeError, $BigInt = BigInt, $Number = Number, $String = String;
var $Symbol = Symbol, $Map = Map, $WeakMap = WeakMap, $Math = Math, $Date = Date;
var create = $Object.create, defineProperty = $Object.defineProperty, getOwnPropertyDescriptor = $Object.getOwnPropertyDescriptor,
    getOwnPropertyNames = $Object.getOwnPropertyNames, setPrototypeOf = $Object.setPrototypeOf;
var floor = $Math.floor, trunc = $Math.trunc, abs = $Math.abs, max = $Math.max, min = $Math.min, pow = $Math.pow;
var isFinite = $Number.isFinite, isInteger = $Number.isInteger, dateNow = $Date.now;
var fnCall = Function.prototype.call;
var uncurry = fnCall.bind.bind(fnCall);
var wmGet = uncurry($WeakMap.prototype.get), wmSet = uncurry($WeakMap.prototype.set);
var mapGet = uncurry($Map.prototype.get), mapSet = uncurry($Map.prototype.set), mapHas = uncurry($Map.prototype.has);
var strSlice = uncurry($String.prototype.slice), strIndexOf = uncurry($String.prototype.indexOf), charCodeAt = uncurry($String.prototype.charCodeAt),
    toLowerCase = uncurry($String.prototype.toLowerCase), padStart = uncurry($String.prototype.padStart), strRepeat = uncurry($String.prototype.repeat);
var arrayPush = uncurry(Array.prototype.push), arraySort = uncurry(Array.prototype.sort), arrayIndexOf = uncurry(Array.prototype.indexOf),
    arrayJoin = uncurry(Array.prototype.join);
var bigToString = uncurry($BigInt.prototype.toString), numToString = uncurry($Number.prototype.toString);
var symToStringTag = $Symbol.toStringTag;

function throwRange(message) { throw new $RangeError(message); }
function throwType(message) { throw new $TypeError(message); }
function isObject(x) { return (typeof x === 'object' && x !== null) || typeof x === 'function'; }
function toString(x) { if (typeof x === 'symbol') throwType('Cannot convert a Symbol value to a string'); return `${x}`; }
function pad(n, width) { return padStart(numToString(n), width, '0'); }

// ---------------------------------------------------------------- internal slots
// Every Temporal object keeps its state in a record held here, so the objects themselves have no own properties.
var slots = new $WeakMap();
function slotOf(x, kind) {
  var r = isObject(x) ? wmGet(slots, x) : undefined;
  if (r === undefined || r.kind !== kind) throwType('this is not a Temporal.' + kind);
  return r;
}
function kindOf(x) { var r = isObject(x) ? wmGet(slots, x) : undefined; return r === undefined ? undefined : r.kind; }
function recordOf(x) { return wmGet(slots, x); }
// Installs the getters of an object literal on a prototype the way a class body would: non-enumerable, configurable
function copyAccessors(target, source) {
  var names = getOwnPropertyNames(source);
  for (var i = 0; i < names.length; i++) {
    defineProperty(target, names[i], { get: getOwnPropertyDescriptor(source, names[i]).get, enumerable: false, configurable: true });
  }
}

// ---------------------------------------------------------------- numbers
var nsPerDay = 86400000000000n, nsMaxInstant = 8640000000000000000000n, nsMinInstant = -8640000000000000000000n;
var maxTimeDuration = 9007199254740991999999999n;
function bigAbs(x) { return x < 0n ? -x : x; }
function bigSign(x) { return x < 0n ? -1 : x > 0n ? 1 : 0; }
function floorDiv(a, b) { var q = a / b; if ((a % b !== 0n) && ((a < 0n) !== (b < 0n))) q -= 1n; return q; }
function bitLength(x) { return x === 0n ? 0 : bigToString(x, 2).length; }
// n / d as the closest Number, both BigInts (the "quotient of two time durations" the proposal asks for)
function divideToNumber(n, d) {
  if (d < 0n) { n = -n; d = -d; }
  var negative = n < 0n;
  if (negative) n = -n;
  if (n === 0n) return 0;
  var shift = max(0, 66 - (bitLength(n) - bitLength(d)));
  var scaled = n << $BigInt(shift);
  var q = scaled / d;
  if (scaled % d !== 0n) q |= 1n;  // the sticky bit: the quotient has 64+ bits, so it only decides ties
  var result = $Number(q);
  while (shift > 1000) { result /= pow(2, 1000); shift -= 1000; }
  result /= pow(2, shift);
  return negative ? -result : result;
}

// ---------------------------------------------------------------- units and rounding (13.5, 13.27-13.30)
var UNITS = ['year', 'month', 'week', 'day', 'hour', 'minute', 'second', 'millisecond', 'microsecond', 'nanosecond'];
var PLURALS = ['years', 'months', 'weeks', 'days', 'hours', 'minutes', 'seconds', 'milliseconds', 'microseconds', 'nanoseconds'];
var YEAR = 0, MONTH = 1, WEEK = 2, DAY = 3, HOUR = 4, MINUTE = 5, SECOND = 6, MILLISECOND = 7, MICROSECOND = 8, NANOSECOND = 9;
var UNIT_NS = [undefined, undefined, undefined, 86400000000000n, 3600000000000n, 60000000000n, 1000000000n, 1000000n, 1000n, 1n];
var MAX_INCREMENT = [undefined, undefined, undefined, undefined, 24, 60, 60, 1000, 1000, 1000];
var AUTO = 'auto';
function isCalendarUnit(u) { return u <= WEEK; }
function isDateUnit(u) { return u <= DAY; }
function largerUnit(a, b) { return a < b ? a : b; }

var ROUNDING_MODES = ['ceil', 'floor', 'expand', 'trunc', 'halfCeil', 'halfFloor', 'halfExpand', 'halfTrunc', 'halfEven'];
function negateRoundingMode(m) {
  return m === 'ceil' ? 'floor' : m === 'floor' ? 'ceil' : m === 'halfCeil' ? 'halfFloor' : m === 'halfFloor' ? 'halfCeil' : m;
}
function unsignedRoundingMode(mode, negative) {
  switch (mode) {
    case 'ceil': return negative ? 'zero' : 'infinity';
    case 'floor': return negative ? 'infinity' : 'zero';
    case 'expand': return 'infinity';
    case 'trunc': return 'zero';
    case 'halfCeil': return negative ? 'halfZero' : 'halfInfinity';
    case 'halfFloor': return negative ? 'halfInfinity' : 'halfZero';
    case 'halfExpand': return 'halfInfinity';
    case 'halfTrunc': return 'halfZero';
    default: return 'halfEven';
  }
}
// ApplyUnsignedRoundingMode on x = r1 + fraction, where fraction = num / den is in [0, 1); r1 counts whole steps
function chooseRounded(num, den, r1IsEven, unsigned) {
  if (num === 0n) return false;  // false: r1, true: r2
  if (unsigned === 'zero') return false;
  if (unsigned === 'infinity') return true;
  var twice = num * 2n;
  if (twice < den) return false;
  if (twice > den) return true;
  if (unsigned === 'halfZero') return false;
  if (unsigned === 'halfInfinity') return true;
  return !r1IsEven;
}
// RoundNumberToIncrement for BigInts
function roundBigToIncrement(x, increment, mode) {
  var negative = x < 0n, ax = negative ? -x : x;
  var q = ax / increment, r = ax % increment;
  var up = chooseRounded(r, increment, q % 2n === 0n, unsignedRoundingMode(mode, negative));
  var rounded = (up ? q + 1n : q) * increment;
  return negative ? -rounded : rounded;
}
// RoundNumberToIncrementAsIfPositive (13.30)
function roundBigToIncrementAsIfPositive(x, increment, mode) {
  var q = floorDiv(x, increment), r = x - q * increment;
  var up = chooseRounded(r, increment, q % 2n === 0n, unsignedRoundingMode(mode, false));
  return (up ? q + 1n : q) * increment;
}
// RoundNumberToIncrement for integral Numbers
function roundNumberToIncrement(x, increment, mode) {
  return $Number(roundBigToIncrement($BigInt(x), $BigInt(increment), mode));
}

// ---------------------------------------------------------------- conversions (13.39-13.41, 14.5.1)
function toIntegerWithTruncation(v) {
  var n = +v;
  if (n !== n || n === Infinity || n === -Infinity) throwRange('A finite number is required');
  return trunc(n) + 0;
}
function toPositiveIntegerWithTruncation(v) {
  var n = toIntegerWithTruncation(v);
  if (n <= 0) throwRange('A positive integer is required');
  return n;
}
function toIntegerIfIntegral(v) {
  var n = +v;
  if (!isInteger(n)) throwRange('An integer is required');
  return n + 0;
}
function toBigInt(v) { return toBigIntNative(v); }

// ---------------------------------------------------------------- options (13.6-13.19, 14.5.2)
function getOptionsObject(options) {
  if (options === undefined) return create(null);
  if (isObject(options)) return options;
  throwType('Options must be an object');
}
function getOption(options, property, values, fallback, required) {
  var value = options[property];
  if (value === undefined) {
    if (required) throwRange(property + ' is required');
    return fallback;
  }
  value = toString(value);
  if (values !== undefined && arrayIndexOf(values, value) < 0) throwRange(value + ' is not a valid value for ' + property);
  return value;
}
function getOverflowOption(options) { return getOption(options, 'overflow', ['constrain', 'reject'], 'constrain'); }
function getDisambiguationOption(options) { return getOption(options, 'disambiguation', ['compatible', 'earlier', 'later', 'reject'], 'compatible'); }
function getOffsetOption(options, fallback) { return getOption(options, 'offset', ['prefer', 'use', 'ignore', 'reject'], fallback); }
function getShowCalendarOption(options) { return getOption(options, 'calendarName', ['auto', 'always', 'never', 'critical'], 'auto'); }
function getShowTimeZoneOption(options) { return getOption(options, 'timeZoneName', ['auto', 'never', 'critical'], 'auto'); }
function getShowOffsetOption(options) { return getOption(options, 'offset', ['auto', 'never'], 'auto'); }
function getRoundingModeOption(options, fallback) { return getOption(options, 'roundingMode', ROUNDING_MODES, fallback); }
function getRoundingIncrementOption(options) {
  var value = options.roundingIncrement;
  if (value === undefined) return 1;
  var increment = toIntegerWithTruncation(value);
  if (increment < 1 || increment > 1e9) throwRange('roundingIncrement is out of range');
  return increment;
}
function validateRoundingIncrement(increment, dividend, inclusive) {
  var maximum = inclusive ? dividend : dividend - 1;
  if (increment > maximum) throwRange('roundingIncrement is out of range');
  if (dividend % increment !== 0) throwRange('roundingIncrement does not divide evenly');
}
function getFractionalSecondDigitsOption(options) {
  var value = options.fractionalSecondDigits;
  if (value === undefined) return AUTO;
  if (typeof value !== 'number') {
    if (toString(value) !== 'auto') throwRange('fractionalSecondDigits is invalid');
    return AUTO;
  }
  if (value !== value || value === Infinity || value === -Infinity) throwRange('fractionalSecondDigits is invalid');
  var digits = floor(value);
  if (digits < 0 || digits > 9) throwRange('fractionalSecondDigits is out of range');
  return digits;
}
// GetTemporalUnitValuedOption (13.17): a unit index, AUTO, or undefined for UNSET
function getUnitOption(options, key, required) {
  var value = getOption(options, key, undefined, undefined, required);
  if (value === undefined) return undefined;
  if (value === 'auto') return AUTO;
  var i = arrayIndexOf(UNITS, value);
  if (i < 0) i = arrayIndexOf(PLURALS, value);
  if (i < 0) throwRange(value + ' is not a valid value for ' + key);
  return i;
}
// ValidateTemporalUnitValue (13.18): group is 'date', 'time' or 'datetime'
function validateUnit(value, group, extra) {
  if (value === undefined) return;
  if (extra !== undefined && arrayIndexOf(extra, value) >= 0) return;
  if (value === AUTO) throwRange('auto is not allowed here');
  if (isDateUnit(value) ? group === 'date' || group === 'datetime' : group === 'time' || group === 'datetime') return;
  throwRange(UNITS[value] + ' is not allowed here');
}
function toSecondsStringPrecision(smallestUnit, digits) {
  switch (smallestUnit) {
    case MINUTE: return { precision: 'minute', unit: MINUTE, increment: 1 };
    case SECOND: return { precision: 0, unit: SECOND, increment: 1 };
    case MILLISECOND: return { precision: 3, unit: MILLISECOND, increment: 1 };
    case MICROSECOND: return { precision: 6, unit: MICROSECOND, increment: 1 };
    case NANOSECOND: return { precision: 9, unit: NANOSECOND, increment: 1 };
  }
  if (digits === AUTO) return { precision: AUTO, unit: NANOSECOND, increment: 1 };
  if (digits === 0) return { precision: 0, unit: SECOND, increment: 1 };
  if (digits <= 3) return { precision: digits, unit: MILLISECOND, increment: pow(10, 3 - digits) };
  if (digits <= 6) return { precision: digits, unit: MICROSECOND, increment: pow(10, 6 - digits) };
  return { precision: digits, unit: NANOSECOND, increment: pow(10, 9 - digits) };
}
// GetDifferenceSettings (13.43)
function getDifferenceSettings(operation, options, group, disallowed, fallbackSmallest, smallestLargestDefault) {
  var largestUnit = getUnitOption(options, 'largestUnit');
  var roundingIncrement = getRoundingIncrementOption(options);
  var roundingMode = getRoundingModeOption(options, 'trunc');
  var smallestUnit = getUnitOption(options, 'smallestUnit');
  validateUnit(largestUnit, group, [AUTO]);
  if (largestUnit === undefined) largestUnit = AUTO;
  if (arrayIndexOf(disallowed, largestUnit) >= 0) throwRange('largestUnit is not allowed here');
  validateUnit(smallestUnit, group);
  if (smallestUnit === undefined) smallestUnit = fallbackSmallest;
  if (arrayIndexOf(disallowed, smallestUnit) >= 0) throwRange('smallestUnit is not allowed here');
  var defaultLargest = largerUnit(smallestLargestDefault, smallestUnit);
  if (largestUnit === AUTO) largestUnit = defaultLargest;
  if (largerUnit(largestUnit, smallestUnit) !== largestUnit) throwRange('largestUnit must be larger than smallestUnit');
  var maximum = MAX_INCREMENT[smallestUnit];
  if (maximum !== undefined) validateRoundingIncrement(roundingIncrement, maximum, false);
  if (operation === 'since') roundingMode = negateRoundingMode(roundingMode);
  return { smallestUnit: smallestUnit, largestUnit: largestUnit, roundingMode: roundingMode, roundingIncrement: roundingIncrement };
}
// A string option bag for the methods that take one string argument instead of an object (round, total, ...)
function stringToOptions(value, key) {
  if (typeof value === 'string') { var o = create(null); o[key] = value; return o; }
  return getOptionsObject(value);
}

// ---------------------------------------------------------------- ISO dates and times
function isLeapYear(y) { return y % 4 === 0 && (y % 100 !== 0 || y % 400 === 0); }
function daysInMonth(y, m) { return m === 2 ? (isLeapYear(y) ? 29 : 28) : (m === 4 || m === 6 || m === 9 || m === 11) ? 30 : 31; }
function daysInYear(y) { return isLeapYear(y) ? 366 : 365; }
// ISODateToEpochDays (13.1), with a 1-based month that may lie outside 1-12
function epochDays(y, m, d) {
  y += floor((m - 1) / 12);
  m = ((m - 1) % 12 + 12) % 12 + 1;
  var yy = m <= 2 ? y - 1 : y;
  var era = floor(yy / 400), yoe = yy - era * 400, mp = (m + 9) % 12;
  var doy = floor((153 * mp + 2) / 5) + d - 1;
  return era * 146097 + yoe * 365 + floor(yoe / 4) - floor(yoe / 100) + doy - 719468;
}
function dateFromEpochDays(z) {
  z += 719468;
  var era = floor(z / 146097), doe = z - era * 146097;
  var yoe = floor((doe - floor(doe / 1460) + floor(doe / 36524) - floor(doe / 146096)) / 365);
  var doy = doe - (365 * yoe + floor(yoe / 4) - floor(yoe / 100));
  var mp = floor((5 * doy + 2) / 153);
  var d = doy - floor((153 * mp + 2) / 5) + 1, m = mp < 10 ? mp + 3 : mp - 9;
  return { year: yoe + era * 400 + (m <= 2 ? 1 : 0), month: m, day: d };
}
function isoDate(year, month, day) { return { year: year, month: month, day: day }; }
function isValidISODate(y, m, d) { return m >= 1 && m <= 12 && d >= 1 && d <= daysInMonth(y, m); }
function addDaysToISODate(date, days) { return days === 0 ? date : dateFromEpochDays(epochDays(date.year, date.month, date.day) + days); }
function compareISODate(a, b) {
  if (a.year !== b.year) return a.year < b.year ? -1 : 1;
  if (a.month !== b.month) return a.month < b.month ? -1 : 1;
  if (a.day !== b.day) return a.day < b.day ? -1 : 1;
  return 0;
}
function balanceISOYearMonth(y, m) { y += floor((m - 1) / 12); m = ((m - 1) % 12 + 12) % 12 + 1; return { year: y, month: m }; }
function regulateISODate(y, m, d, overflow) {
  if (overflow === 'constrain') {
    m = min(max(m, 1), 12);
    d = min(max(d, 1), daysInMonth(y, m));
  } else if (!isValidISODate(y, m, d)) {
    throwRange('The date does not exist');
  }
  return isoDate(y, m, d);
}
function dayOfWeek(date) { var d = (epochDays(date.year, date.month, date.day) + 4) % 7; if (d < 0) d += 7; return d === 0 ? 7 : d; }
function dayOfYear(date) { return epochDays(date.year, date.month, date.day) - epochDays(date.year, 1, 1) + 1; }
// ISOWeekOfYear (12.3.18)
function weekOfYear(date) {
  var year = date.year, doy = dayOfYear(date), dow = dayOfWeek(date);
  var week = floor((doy + 7 - dow + 3) / 7);
  if (week < 1) {
    var jan1 = dayOfWeek(isoDate(year, 1, 1));
    if (jan1 === 5 || (jan1 === 6 && isLeapYear(year - 1))) return { week: 53, year: year - 1 };
    return { week: 52, year: year - 1 };
  }
  if (week === 53 && daysInYear(year) - doy < 4 - dow) return { week: 1, year: year + 1 };
  return { week: week, year: year };
}
function checkISODaysRange(date) { if (abs(epochDays(date.year, date.month, date.day)) > 1e8) throwRange('The date is out of range'); }

function time(h, mi, s, ms, us, ns) { return { hour: h, minute: mi, second: s, millisecond: ms, microsecond: us, nanosecond: ns }; }
var MIDNIGHT = time(0, 0, 0, 0, 0, 0);
function isValidTime(h, mi, s, ms, us, ns) {
  return h >= 0 && h <= 23 && mi >= 0 && mi <= 59 && s >= 0 && s <= 59 && ms >= 0 && ms <= 999 && us >= 0 && us <= 999 && ns >= 0 && ns <= 999;
}
function timeToNs(t) { return ((((t.hour * 60 + t.minute) * 60 + t.second) * 1000 + t.millisecond) * 1000 + t.microsecond) * 1000 + t.nanosecond; }
// BalanceTime (4.5.10) on Numbers that are exact integers; returns the time with the whole days that spilled over
function balanceTime(h, mi, s, ms, us, ns) {
  us += floor(ns / 1000); ns = ((ns % 1000) + 1000) % 1000;
  ms += floor(us / 1000); us = ((us % 1000) + 1000) % 1000;
  s += floor(ms / 1000); ms = ((ms % 1000) + 1000) % 1000;
  mi += floor(s / 60); s = ((s % 60) + 60) % 60;
  h += floor(mi / 60); mi = ((mi % 60) + 60) % 60;
  var days = floor(h / 24); h = ((h % 24) + 24) % 24;
  var t = time(h, mi, s, ms, us, ns);
  t.days = days;
  return t;
}
// AddTime (4.5.15) with a BigInt time duration
function addTime(t, d) {
  var total = $BigInt(timeToNs(t)) + d;
  var days = floorDiv(total, nsPerDay), rest = $Number(total - days * nsPerDay);
  var r = balanceTime(0, 0, 0, 0, 0, rest);
  r.days = $Number(days);
  return r;
}
function differenceTime(a, b) { return $BigInt(timeToNs(b) - timeToNs(a)); }
function compareTime(a, b) { var x = timeToNs(a), y = timeToNs(b); return x < y ? -1 : x > y ? 1 : 0; }
function regulateTime(h, mi, s, ms, us, ns, overflow) {
  if (overflow === 'constrain') {
    return time(min(max(h, 0), 23), min(max(mi, 0), 59), min(max(s, 0), 59), min(max(ms, 0), 999), min(max(us, 0), 999), min(max(ns, 0), 999));
  }
  if (!isValidTime(h, mi, s, ms, us, ns)) throwRange('The time does not exist');
  return time(h, mi, s, ms, us, ns);
}
// RoundTime (4.5.16)
function roundTime(t, increment, unit, mode) {
  var quantity;
  if (unit === DAY || unit === HOUR) quantity = timeToNs(t);
  else if (unit === MINUTE) quantity = ((t.minute * 60 + t.second) * 1000 + t.millisecond) * 1e6 + t.microsecond * 1000 + t.nanosecond;
  else if (unit === SECOND) quantity = t.second * 1e9 + t.millisecond * 1e6 + t.microsecond * 1000 + t.nanosecond;
  else if (unit === MILLISECOND) quantity = t.millisecond * 1e6 + t.microsecond * 1000 + t.nanosecond;
  else if (unit === MICROSECOND) quantity = t.microsecond * 1000 + t.nanosecond;
  else quantity = t.nanosecond;
  var unitLength = $Number(UNIT_NS[unit]);
  var result = roundNumberToIncrement(quantity, increment * unitLength, mode) / unitLength;
  switch (unit) {
    case DAY: var r = time(0, 0, 0, 0, 0, 0); r.days = result; return r;
    case HOUR: return balanceTime(result, 0, 0, 0, 0, 0);
    case MINUTE: return balanceTime(t.hour, result, 0, 0, 0, 0);
    case SECOND: return balanceTime(t.hour, t.minute, result, 0, 0, 0);
    case MILLISECOND: return balanceTime(t.hour, t.minute, t.second, result, 0, 0);
    case MICROSECOND: return balanceTime(t.hour, t.minute, t.second, t.millisecond, result, 0);
    default: return balanceTime(t.hour, t.minute, t.second, t.millisecond, t.microsecond, result);
  }
}

// ISO date-time records: { date, time }
function dateTime(date, t) { return { date: date, time: t }; }
function utcEpochNs(date, t) { return $BigInt(epochDays(date.year, date.month, date.day)) * nsPerDay + $BigInt(timeToNs(t)); }
function isValidEpochNs(ns) { return ns >= nsMinInstant && ns <= nsMaxInstant; }
function isoDateTimeWithinLimits(date, t) {
  if (abs(epochDays(date.year, date.month, date.day)) > 1e8 + 1) return false;
  var ns = utcEpochNs(date, t);
  return ns > nsMinInstant - nsPerDay && ns < nsMaxInstant + nsPerDay;
}
function isoDateWithinLimits(date) { return isoDateTimeWithinLimits(date, time(12, 0, 0, 0, 0, 0)); }
function isoYearMonthWithinLimits(date) {
  if (date.year < -271821 || date.year > 275760) return false;
  if (date.year === -271821 && date.month < 4) return false;
  if (date.year === 275760 && date.month > 9) return false;
  return true;
}
function balanceISODateTime(y, mo, d, h, mi, s, ms, us, ns) {
  var t = balanceTime(h, mi, s, ms, us, ns);
  return dateTime(addDaysToISODate(isoDate(y, mo, d), t.days), t);
}
function isoPartsFromEpoch(ns) {
  var days = floorDiv(ns, nsPerDay), rest = $Number(ns - days * nsPerDay);
  return dateTime(dateFromEpochDays($Number(days)), balanceTime(0, 0, 0, 0, 0, rest));
}
function compareISODateTime(a, b) { return compareISODate(a.date, b.date) || compareTime(a.time, b.time); }
function roundISODateTime(dt, increment, unit, mode) {
  var t = roundTime(dt.time, increment, unit, mode);
  return dateTime(addDaysToISODate(dt.date, t.days), t);
}

// ---------------------------------------------------------------- formatting (13.25, 13.26, 3.5.10, 11.1.5-11.1.7)
function padISOYear(y) {
  if (y >= 0 && y <= 9999) return pad(y, 4);
  return (y > 0 ? '+' : '-') + pad(abs(y), 6);
}
function formatFractionalSeconds(sub, precision) {
  var s;
  if (precision === AUTO) {
    if (sub === 0) return '';
    s = pad(sub, 9);
    var end = 9;
    while (charCodeAt(s, end - 1) === 48) end--;
    s = strSlice(s, 0, end);
  } else {
    if (precision === 0) return '';
    s = strSlice(pad(sub, 9), 0, precision);
  }
  return '.' + s;
}
function formatTimeString(h, mi, s, sub, precision, separated) {
  var sep = separated === false ? '' : ':';
  var out = pad(h, 2) + sep + pad(mi, 2);
  if (precision === 'minute') return out;
  return out + sep + pad(s, 2) + formatFractionalSeconds(sub, precision);
}
function formatOffsetMinutes(offsetMinutes, separated) {
  var sign = offsetMinutes >= 0 ? '+' : '-', a = abs(offsetMinutes);
  return sign + formatTimeString(floor(a / 60), a % 60, 0, 0, 'minute', separated);
}
function formatUTCOffsetNs(offsetNs) {
  var sign = offsetNs >= 0 ? '+' : '-', a = abs(offsetNs);
  var h = floor(a / 3.6e12), mi = floor(a / 6e10) % 60, s = floor(a / 1e9) % 60, sub = a % 1e9;
  return sign + formatTimeString(h, mi, s, sub, s === 0 && sub === 0 ? 'minute' : AUTO);
}
function formatOffsetRounded(offsetNs) {
  var rounded = roundNumberToIncrement(offsetNs, 6e10, 'halfExpand');
  return formatOffsetMinutes(rounded / 6e10);
}
function formatCalendarAnnotation(id, show) {
  if (show === 'never') return '';
  if (show === 'auto' && id === 'iso8601') return '';
  return '[' + (show === 'critical' ? '!' : '') + 'u-ca=' + id + ']';
}
function dateString(date) { return padISOYear(date.year) + '-' + pad(date.month, 2) + '-' + pad(date.day, 2); }
function timeRecordToString(t, precision) {
  return formatTimeString(t.hour, t.minute, t.second, t.millisecond * 1e6 + t.microsecond * 1000 + t.nanosecond, precision);
}
function dateTimeToString(dt, calendar, precision, showCalendar) {
  return dateString(dt.date) + 'T' + timeRecordToString(dt.time, precision) + formatCalendarAnnotation(calendar, showCalendar);
}
