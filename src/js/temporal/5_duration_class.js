// ---------------------------------------------------------------- Temporal.Duration (7)
// GetTemporalRelativeToOption (13.19): { plain, zoned }
function getRelativeToOption(options) {
  var value = options.relativeTo;
  if (value === undefined) return { plain: undefined, zoned: undefined };
  var offsetBehaviour = 'option', matchBehaviour = 'exactly', calendar, date, t, timeZone, offsetString;
  if (isObject(value)) {
    var k = kindOf(value);
    if (k === 'ZonedDateTime') return { plain: undefined, zoned: value };
    if (k === 'PlainDate') return { plain: value, zoned: undefined };
    if (k === 'PlainDateTime') return { plain: createPlainDate(recordOf(value).date, recordOf(value).calendar), zoned: undefined };
    calendar = calendarWithISODefault(value);
    var fields = prepareCalendarFields(calendar, value, ['year', 'month', 'monthCode', 'day'], ['hour', 'minute', 'second', 'millisecond', 'microsecond', 'nanosecond', 'offset', 'timeZone'], []);
    var result = interpretDateTimeFields(calendar, fields, 'constrain');
    timeZone = fields.timeZone;
    offsetString = fields.offset;
    if (offsetString === undefined) offsetBehaviour = 'wall';
    date = result.date;
    t = result.time;
  } else {
    if (typeof value !== 'string') throwType('relativeTo must be an object or a string');
    var parsed = parseISODateTime(value, ['zonedDateTime', 'dateTime']);
    offsetString = parsed.offsetString;
    if (parsed.timeZoneAnnotation === undefined) timeZone = undefined;
    else {
      timeZone = toTimeZoneIdentifier(parsed.timeZoneAnnotation);
      if (parsed.z) offsetBehaviour = 'exact';
      else if (offsetString === undefined) offsetBehaviour = 'wall';
      matchBehaviour = 'minutes';
      if (offsetString !== undefined && parsed.offsetParts > 2) matchBehaviour = 'exactly';
    }
    calendar = canonicalizeCalendar(parsed.calendar === undefined ? 'iso8601' : parsed.calendar);
    date = isoDate(parsed.year, parsed.month, parsed.day);
    t = parsed.time;
  }
  if (timeZone === undefined) return { plain: createPlainDate(date, calendar), zoned: undefined };
  var offsetNs = offsetBehaviour === 'option' ? parseUTCOffsetNs(offsetString) : 0;
  var ns = interpretISODateTimeOffset(date, t, offsetBehaviour, offsetNs, timeZone, 'compatible', 'reject', matchBehaviour);
  return { plain: undefined, zoned: createZonedDateTime(ns, timeZone, calendar) };
}
// Adds a duration (as an Internal Duration Record) to a PlainDate: the end as an ISO date-time
function plainRelativeTarget(plainRelativeTo, v) {
  var d = toInternalDurationWith24HourDays(v);
  var targetTime = addTime(MIDNIGHT, d.time);
  var r = recordOf(plainRelativeTo);
  var dateDur = adjustDateDuration(d.date, targetTime.days);
  return dateTime(calendarDateAdd(r.calendar, r.date, dateDur, 'constrain'), targetTime);
}
function addDurations(operation, v, other) {
  var w = toDurationFields(other);
  if (operation === 'subtract') w = negatedDuration(w);
  var largestUnit = largerUnit(defaultLargestUnit(v), defaultLargestUnit(w));
  if (isCalendarUnit(largestUnit)) throwRange('Adding years, months or weeks needs a relativeTo starting point');
  var a = toInternalDurationWith24HourDays(v), b = toInternalDurationWith24HourDays(w);
  return createDuration(durationFromInternal(internalDuration(ZERO_DATE_DURATION, addTimeDuration(a.time, b.time)), largestUnit));
}
function durationGetter(i) { return function () { return slotOf(this, 'Duration').v[i]; }; }

class Duration extends $Object {
  constructor() {
    var a = arguments, v = [];
    for (var i = 0; i < 10; i++) v[i] = a[i] === undefined ? 0 : toIntegerIfIntegral(a[i]);
    return createDuration(v, new.target);
  }
  static from(item) { return toDuration(item); }
  static compare(one, two) {
    var a = toDurationFields(one), b = toDurationFields(two);
    var options = getOptionsObject(arguments[2]);
    var relativeTo = getRelativeToOption(options);
    var same = true;
    for (var i = 0; i < 10; i++) if (a[i] !== b[i]) same = false;
    if (same) return 0;
    var lu1 = defaultLargestUnit(a), lu2 = defaultLargestUnit(b);
    var d1 = toInternalDuration(a), d2 = toInternalDuration(b);
    if (relativeTo.zoned !== undefined && (isDateUnit(lu1) || isDateUnit(lu2))) {
      var z = recordOf(relativeTo.zoned);
      var after1 = addZonedDateTime(z.ns, z.timeZone, z.calendar, d1, 'constrain');
      var after2 = addZonedDateTime(z.ns, z.timeZone, z.calendar, d2, 'constrain');
      return after1 > after2 ? 1 : after1 < after2 ? -1 : 0;
    }
    var days1, days2;
    if (isCalendarUnit(lu1) || isCalendarUnit(lu2)) {
      if (relativeTo.plain === undefined) throwRange('Comparing years, months or weeks needs a relativeTo starting point');
      days1 = dateDurationDays(d1.date, relativeTo.plain);
      days2 = dateDurationDays(d2.date, relativeTo.plain);
    } else {
      days1 = a[3]; days2 = b[3];
    }
    var t1 = add24HourDays(d1.time, days1), t2 = add24HourDays(d2.time, days2);
    return t1 > t2 ? 1 : t1 < t2 ? -1 : 0;
  }
  get years() { return slotOf(this, 'Duration').v[0]; }
  get months() { return slotOf(this, 'Duration').v[1]; }
  get weeks() { return slotOf(this, 'Duration').v[2]; }
  get days() { return slotOf(this, 'Duration').v[3]; }
  get hours() { return slotOf(this, 'Duration').v[4]; }
  get minutes() { return slotOf(this, 'Duration').v[5]; }
  get seconds() { return slotOf(this, 'Duration').v[6]; }
  get milliseconds() { return slotOf(this, 'Duration').v[7]; }
  get microseconds() { return slotOf(this, 'Duration').v[8]; }
  get nanoseconds() { return slotOf(this, 'Duration').v[9]; }
  get sign() { return durationSign(slotOf(this, 'Duration').v); }
  get blank() { return durationSign(slotOf(this, 'Duration').v) === 0; }
  with(temporalDurationLike) {
    var v = slotOf(this, 'Duration').v, partial = toPartialDuration(temporalDurationLike), w = [];
    for (var i = 0; i < 10; i++) w[i] = partial[i] === undefined ? v[i] : partial[i];
    return createDuration(w);
  }
  negated() { return createDuration(negatedDuration(slotOf(this, 'Duration').v)); }
  abs() { var v = slotOf(this, 'Duration').v, w = []; for (var i = 0; i < 10; i++) w[i] = abs(v[i]); return createDuration(w); }
  add(other) { return addDurations('add', slotOf(this, 'Duration').v, other); }
  subtract(other) { return addDurations('subtract', slotOf(this, 'Duration').v, other); }
  round(roundTo) {
    var v = slotOf(this, 'Duration').v;
    if (roundTo === undefined) throwType('round needs an argument');
    var options = stringToOptions(roundTo, 'smallestUnit');
    var smallestUnitPresent = true, largestUnitPresent = true;
    var largestUnit = getUnitOption(options, 'largestUnit');
    var relativeTo = getRelativeToOption(options);
    var roundingIncrement = getRoundingIncrementOption(options);
    var roundingMode = getRoundingModeOption(options, 'halfExpand');
    var smallestUnit = getUnitOption(options, 'smallestUnit');
    validateUnit(smallestUnit, 'datetime');
    if (smallestUnit === undefined) { smallestUnitPresent = false; smallestUnit = NANOSECOND; }
    var existingLargestUnit = defaultLargestUnit(v);
    var defaultLargest = largerUnit(existingLargestUnit, smallestUnit);
    if (largestUnit === undefined) { largestUnitPresent = false; largestUnit = defaultLargest; }
    else if (largestUnit === AUTO) largestUnit = defaultLargest;
    if (!smallestUnitPresent && !largestUnitPresent) throwRange('smallestUnit or largestUnit is required');
    if (largerUnit(largestUnit, smallestUnit) !== largestUnit) throwRange('largestUnit must be larger than smallestUnit');
    var maximum = MAX_INCREMENT[smallestUnit];
    if (maximum !== undefined) validateRoundingIncrement(roundingIncrement, maximum, false);
    if (roundingIncrement > 1 && largestUnit !== smallestUnit && isDateUnit(smallestUnit)) throwRange('roundingIncrement needs largestUnit to equal smallestUnit');
    var d;
    if (relativeTo.zoned !== undefined) {
      var z = recordOf(relativeTo.zoned);
      d = toInternalDuration(v);
      var target = addZonedDateTime(z.ns, z.timeZone, z.calendar, d, 'constrain');
      d = differenceZonedDateTimeWithRounding(z.ns, target, z.timeZone, z.calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode);
      if (isDateUnit(largestUnit)) largestUnit = HOUR;
      return createDuration(durationFromInternal(d, largestUnit));
    }
    if (relativeTo.plain !== undefined) {
      var p = recordOf(relativeTo.plain);
      var targetDateTime = plainRelativeTarget(relativeTo.plain, v);
      d = differencePlainDateTimeWithRounding(dateTime(p.date, MIDNIGHT), targetDateTime, p.calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode);
      return createDuration(durationFromInternal(d, largestUnit));
    }
    if (isCalendarUnit(existingLargestUnit) || isCalendarUnit(largestUnit)) throwRange('Rounding years, months or weeks needs a relativeTo starting point');
    d = toInternalDurationWith24HourDays(v);
    if (smallestUnit === DAY) {
      var days = $Number(roundBigToIncrement(d.time, nsPerDay * $BigInt(roundingIncrement), roundingMode) / nsPerDay);
      d = internalDuration(createDateDuration(0, 0, 0, days), 0n);
    } else {
      d = internalDuration(ZERO_DATE_DURATION, roundTimeDuration(d.time, roundingIncrement, smallestUnit, roundingMode));
    }
    return createDuration(durationFromInternal(d, largestUnit));
  }
  total(totalOf) {
    var v = slotOf(this, 'Duration').v;
    if (totalOf === undefined) throwType('total needs an argument');
    var options = stringToOptions(totalOf, 'unit');
    var relativeTo = getRelativeToOption(options);
    var unit = getUnitOption(options, 'unit', true);
    validateUnit(unit, 'datetime');
    if (relativeTo.zoned !== undefined) {
      var z = recordOf(relativeTo.zoned);
      var target = addZonedDateTime(z.ns, z.timeZone, z.calendar, toInternalDuration(v), 'constrain');
      return differenceZonedDateTimeWithTotal(z.ns, target, z.timeZone, z.calendar, unit);
    }
    if (relativeTo.plain !== undefined) {
      var p = recordOf(relativeTo.plain);
      return differencePlainDateTimeWithTotal(dateTime(p.date, MIDNIGHT), plainRelativeTarget(relativeTo.plain, v), p.calendar, unit);
    }
    if (isCalendarUnit(defaultLargestUnit(v)) || isCalendarUnit(unit)) throwRange('A total in years, months or weeks needs a relativeTo starting point');
    return totalTimeDuration(toInternalDurationWith24HourDays(v).time, unit);
  }
  toString() {
    var v = slotOf(this, 'Duration').v;
    var options = getOptionsObject(arguments[0]);
    var digits = getFractionalSecondDigitsOption(options);
    var roundingMode = getRoundingModeOption(options, 'trunc');
    var smallestUnit = getUnitOption(options, 'smallestUnit');
    validateUnit(smallestUnit, 'time');
    if (smallestUnit === HOUR || smallestUnit === MINUTE) throwRange('smallestUnit must be seconds or smaller');
    var precision = toSecondsStringPrecision(smallestUnit, digits);
    if (precision.unit === NANOSECOND && precision.increment === 1) return durationToString(v, precision.precision);
    var largestUnit = defaultLargestUnit(v);
    var d = toInternalDuration(v);
    d = internalDuration(d.date, roundTimeDuration(d.time, precision.increment, precision.unit, roundingMode));
    var rounded = durationFromInternal(d, largerUnit(largestUnit, SECOND));
    if (!isValidDuration(rounded)) throwRange('The duration is out of range');
    return durationToString(rounded, precision.precision);
  }
  toJSON() { return durationToString(slotOf(this, 'Duration').v, AUTO); }
  toLocaleString() { return durationToString(slotOf(this, 'Duration').v, AUTO); }
  valueOf() { throwType('Use Temporal.Duration.compare to compare durations'); }
}
DurationProto = Duration.prototype;

// ---------------------------------------------------------------- Temporal.Instant (8)
var InstantProto;
function createInstant(ns, newTarget) {
  if (!isValidEpochNs(ns)) throwRange('The instant is out of range');
  var o = create(newTarget === undefined ? InstantProto : protoFor(newTarget, InstantProto));
  wmSet(slots, o, { kind: 'Instant', ns: ns });
  return o;
}
// ToTemporalInstant (8.5.3): epoch nanoseconds
function toInstantNs(item) {
  if (isObject(item)) {
    var k = kindOf(item);
    if (k === 'Instant' || k === 'ZonedDateTime') return recordOf(item).ns;
    item = toPrimitiveString(item);
  }
  if (typeof item !== 'string') throwType('An instant must be a string or a Temporal object');
  var parsed = parseISODateTime(item, ['instant']);
  var offsetNs = parsed.z ? 0 : parseUTCOffsetNs(parsed.offsetString), t = parsed.time;
  var balanced = balanceISODateTime(parsed.year, parsed.month, parsed.day, t.hour, t.minute, t.second, t.millisecond, t.microsecond, t.nanosecond - offsetNs);
  checkISODaysRange(balanced.date);
  var ns = utcEpochNs(balanced.date, balanced.time);
  if (!isValidEpochNs(ns)) throwRange('The instant is out of range');
  return ns;
}
function addInstant(ns, d) { var r = d + ns; if (!isValidEpochNs(r)) throwRange('The instant is out of range'); return r; }
function differenceInstant(ns1, ns2, increment, smallestUnit, mode) {
  return internalDuration(ZERO_DATE_DURATION, roundTimeDuration(ns2 - ns1, increment, smallestUnit, mode));
}
function roundInstant(ns, increment, unit, mode) { return roundBigToIncrementAsIfPositive(ns, UNIT_NS[unit] * $BigInt(increment), mode); }
function instantToString(ns, timeZone, precision) {
  var outputZone = timeZone === undefined ? 'UTC' : timeZone;
  var dt = getISODateTimeFor(outputZone, ns);
  var s = dateTimeToString(dt, 'iso8601', precision, 'never');
  return s + (timeZone === undefined ? 'Z' : formatOffsetRounded(getOffsetNsFor(outputZone, ns)));
}
function differenceTemporalInstant(operation, ns, other, options) {
  var otherNs = toInstantNs(other);
  var settings = getDifferenceSettings(operation, getOptionsObject(options), 'time', [], NANOSECOND, SECOND);
  var d = differenceInstant(ns, otherNs, settings.roundingIncrement, settings.smallestUnit, settings.roundingMode);
  var v = durationFromInternal(d, settings.largestUnit);
  return createDuration(operation === 'since' ? negatedDuration(v) : v);
}
function addDurationToInstant(operation, ns, like) {
  var v = toDurationFields(like);
  if (operation === 'subtract') v = negatedDuration(v);
  if (isDateUnit(defaultLargestUnit(v))) throwRange('An instant can only add hours or smaller units');
  return createInstant(addInstant(ns, toInternalDurationWith24HourDays(v).time));
}

class Instant extends $Object {
  constructor(epochNanoseconds) {
    var ns = toBigInt(epochNanoseconds);
    if (!isValidEpochNs(ns)) throwRange('The instant is out of range');
    return createInstant(ns, new.target);
  }
  static from(item) { return createInstant(toInstantNs(item)); }
  static fromEpochMilliseconds(epochMilliseconds) {
    var n = +epochMilliseconds;
    if (!isInteger(n)) throwRange('epochMilliseconds must be an integer');
    var ns = $BigInt(n) * 1000000n;
    if (!isValidEpochNs(ns)) throwRange('The instant is out of range');
    return createInstant(ns);
  }
  static fromEpochNanoseconds(epochNanoseconds) {
    var ns = toBigInt(epochNanoseconds);
    if (!isValidEpochNs(ns)) throwRange('The instant is out of range');
    return createInstant(ns);
  }
  static compare(one, two) { var a = toInstantNs(one), b = toInstantNs(two); return a < b ? -1 : a > b ? 1 : 0; }
  get epochMilliseconds() { return $Number(floorDiv(slotOf(this, 'Instant').ns, 1000000n)); }
  get epochNanoseconds() { return slotOf(this, 'Instant').ns; }
  add(temporalDurationLike) { return addDurationToInstant('add', slotOf(this, 'Instant').ns, temporalDurationLike); }
  subtract(temporalDurationLike) { return addDurationToInstant('subtract', slotOf(this, 'Instant').ns, temporalDurationLike); }
  until(other) { return differenceTemporalInstant('until', slotOf(this, 'Instant').ns, other, arguments[1]); }
  since(other) { return differenceTemporalInstant('since', slotOf(this, 'Instant').ns, other, arguments[1]); }
  round(roundTo) {
    var ns = slotOf(this, 'Instant').ns;
    if (roundTo === undefined) throwType('round needs an argument');
    var options = stringToOptions(roundTo, 'smallestUnit');
    var roundingIncrement = getRoundingIncrementOption(options);
    var roundingMode = getRoundingModeOption(options, 'halfExpand');
    var smallestUnit = getUnitOption(options, 'smallestUnit', true);
    validateUnit(smallestUnit, 'time');
    var maximum = [, , , , 24, 1440, 86400, 86400000, 86400000000, 86400000000000][smallestUnit];
    validateRoundingIncrement(roundingIncrement, maximum, true);
    return createInstant(roundInstant(ns, roundingIncrement, smallestUnit, roundingMode));
  }
  equals(other) { return slotOf(this, 'Instant').ns === toInstantNs(other); }
  toString() {
    var ns = slotOf(this, 'Instant').ns;
    var options = getOptionsObject(arguments[0]);
    var digits = getFractionalSecondDigitsOption(options);
    var roundingMode = getRoundingModeOption(options, 'trunc');
    var smallestUnit = getUnitOption(options, 'smallestUnit');
    var timeZone = options.timeZone;
    validateUnit(smallestUnit, 'time');
    if (smallestUnit === HOUR) throwRange('smallestUnit must be minutes or smaller');
    if (timeZone !== undefined) timeZone = toTimeZoneIdentifier(timeZone);
    var precision = toSecondsStringPrecision(smallestUnit, digits);
    var rounded = roundInstant(ns, precision.increment, precision.unit, roundingMode);
    if (!isValidEpochNs(rounded)) throwRange('The instant is out of range');
    return instantToString(rounded, timeZone, precision.precision);
  }
  toJSON() { return instantToString(slotOf(this, 'Instant').ns, undefined, AUTO); }
  toLocaleString() { return instantToString(slotOf(this, 'Instant').ns, undefined, AUTO); }
  valueOf() { throwType('Use Temporal.Instant.compare to compare instants'); }
  toZonedDateTimeISO(timeZone) {
    var ns = slotOf(this, 'Instant').ns;
    return createZonedDateTime(ns, toTimeZoneIdentifier(timeZone), 'iso8601');
  }
}
InstantProto = Instant.prototype;
