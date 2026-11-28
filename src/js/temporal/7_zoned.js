// ---------------------------------------------------------------- Temporal.ZonedDateTime (6)
var ZonedDateTimeProto;
function createZonedDateTime(ns, timeZone, calendar, newTarget) {
  var o = create(newTarget === undefined ? ZonedDateTimeProto : protoFor(newTarget, ZonedDateTimeProto));
  wmSet(slots, o, { kind: 'ZonedDateTime', ns: ns, timeZone: timeZone, calendar: calendar });
  return o;
}
// InterpretISODateTimeOffset (6.5.1); t undefined is START-OF-DAY. offsetNs is a Number.
function interpretISODateTimeOffset(date, t, offsetBehaviour, offsetNs, timeZone, disambiguation, offsetOption, matchBehaviour) {
  if (t === undefined) return getStartOfDay(timeZone, date);
  var dt = dateTime(date, t);
  if (offsetBehaviour === 'wall' || (offsetBehaviour === 'option' && offsetOption === 'ignore')) return getEpochNsFor(timeZone, dt, disambiguation);
  if (offsetBehaviour === 'exact' || (offsetBehaviour === 'option' && offsetOption === 'use')) {
    var balanced = balanceISODateTime(date.year, date.month, date.day, t.hour, t.minute, t.second, t.millisecond, t.microsecond, t.nanosecond - offsetNs);
    checkISODaysRange(balanced.date);
    var ns = utcEpochNs(balanced.date, balanced.time);
    if (!isValidEpochNs(ns)) throwRange('The date and time are out of range');
    return ns;
  }
  checkISODaysRange(date);
  var utc = utcEpochNs(date, t);
  var possible = getPossibleEpochNs(timeZone, dt);
  for (var i = 0; i < possible.length; i++) {
    var candidateOffset = $Number(utc - possible[i]);
    if (candidateOffset === offsetNs) return possible[i];
    if (matchBehaviour === 'minutes' && roundNumberToIncrement(candidateOffset, 6e10, 'halfExpand') === offsetNs) return possible[i];
  }
  if (offsetOption === 'reject') throwRange('The offset does not match the time zone');
  return disambiguate(possible, timeZone, dt, disambiguation);
}
// ToTemporalZonedDateTime (6.5.2)
function toZonedDateTime(item, options) {
  var hasUTCDesignator = false, matchBehaviour = 'exactly', calendar, timeZone, offsetString, disambiguation, offsetOption, date, t, resolved;
  if (isObject(item)) {
    if (kindOf(item) === 'ZonedDateTime') {
      var r = recordOf(item);
      resolved = getOptionsObject(options);
      getDisambiguationOption(resolved);
      getOffsetOption(resolved, 'reject');
      getOverflowOption(resolved);
      return createZonedDateTime(r.ns, r.timeZone, r.calendar);
    }
    calendar = calendarWithISODefault(item);
    var fields = prepareCalendarFields(calendar, item, ['year', 'month', 'monthCode', 'day'], ['hour', 'minute', 'second', 'millisecond', 'microsecond', 'nanosecond', 'offset', 'timeZone'], ['timeZone']);
    timeZone = fields.timeZone;
    offsetString = fields.offset;
    resolved = getOptionsObject(options);
    disambiguation = getDisambiguationOption(resolved);
    offsetOption = getOffsetOption(resolved, 'reject');
    var overflow = getOverflowOption(resolved);
    var result = interpretDateTimeFields(calendar, fields, overflow);
    date = result.date;
    t = result.time;
  } else {
    if (typeof item !== 'string') throwType('A zoned date-time must be an object or a string');
    var parsed = parseISODateTime(item, ['zonedDateTime']);
    timeZone = toTimeZoneIdentifier(parsed.timeZoneAnnotation);
    offsetString = parsed.offsetString;
    if (parsed.z) hasUTCDesignator = true;
    calendar = canonicalizeCalendar(parsed.calendar === undefined ? 'iso8601' : parsed.calendar);
    matchBehaviour = 'minutes';
    if (offsetString !== undefined && parsed.offsetParts > 2) matchBehaviour = 'exactly';
    resolved = getOptionsObject(options);
    disambiguation = getDisambiguationOption(resolved);
    offsetOption = getOffsetOption(resolved, 'reject');
    getOverflowOption(resolved);
    date = isoDate(parsed.year, parsed.month, parsed.day);
    t = parsed.time;
  }
  var offsetBehaviour = hasUTCDesignator ? 'exact' : offsetString === undefined ? 'wall' : 'option';
  var offsetNs = offsetBehaviour === 'option' ? parseUTCOffsetNs(offsetString) : 0;
  var ns = interpretISODateTimeOffset(date, t, offsetBehaviour, offsetNs, timeZone, disambiguation, offsetOption, matchBehaviour);
  return createZonedDateTime(ns, timeZone, calendar);
}
// TemporalZonedDateTimeToString (6.5.4)
function zonedDateTimeToString(r, precision, showCalendar, showTimeZone, showOffset, increment, unit, mode) {
  var ns = roundInstant(r.ns, increment === undefined ? 1 : increment, unit === undefined ? NANOSECOND : unit, mode === undefined ? 'trunc' : mode);
  var offsetNs = getOffsetNsFor(r.timeZone, ns);
  var s = dateTimeToString(getISODateTimeFor(r.timeZone, ns), 'iso8601', precision, 'never');
  if (showOffset !== 'never') s += formatOffsetRounded(offsetNs);
  if (showTimeZone !== 'never') s += '[' + (showTimeZone === 'critical' ? '!' : '') + r.timeZone + ']';
  return s + formatCalendarAnnotation(r.calendar, showCalendar);
}
// AddZonedDateTime (6.5.5)
function addZonedDateTime(ns, timeZone, calendar, d, overflow) {
  if (dateDurationSign(d.date) === 0) return addInstant(ns, d.time);
  var dt = getISODateTimeFor(timeZone, ns);
  var added = calendarDateAdd(calendar, dt.date, d.date, overflow);
  if (!isoDateTimeWithinLimits(added, dt.time)) throwRange('The date and time are out of range');
  return addInstant(getEpochNsFor(timeZone, dateTime(added, dt.time), 'compatible'), d.time);
}
// DifferenceZonedDateTime (6.5.6)
function differenceZonedDateTime(ns1, ns2, timeZone, calendar, largestUnit) {
  if (ns1 === ns2) return internalDuration(ZERO_DATE_DURATION, 0n);
  var start = getISODateTimeFor(timeZone, ns1), end = getISODateTimeFor(timeZone, ns2);
  if (compareISODate(start.date, end.date) === 0) return internalDuration(ZERO_DATE_DURATION, ns2 - ns1);
  var sign = ns2 - ns1 < 0n ? 1 : -1, maxDayCorrection = sign === -1 ? 2 : 1, dayCorrection = 0;
  var t = differenceTime(start.time, end.time);
  if (bigSign(t) === sign) dayCorrection++;
  var success = false, intermediate;
  while (dayCorrection <= maxDayCorrection && !success) {
    intermediate = dateTime(addDaysToISODate(end.date, dayCorrection * sign), start.time);
    t = ns2 - getEpochNsFor(timeZone, intermediate, 'compatible');
    if (sign !== bigSign(t)) success = true;
    dayCorrection++;
  }
  var dateDifference = calendarDateUntil(calendar, start.date, intermediate.date, largerUnit(largestUnit, DAY));
  return internalDuration(dateDifference, t);
}
function differenceZonedDateTimeWithRounding(ns1, ns2, timeZone, calendar, largestUnit, increment, smallestUnit, mode) {
  if (!isDateUnit(largestUnit)) return differenceInstant(ns1, ns2, increment, smallestUnit, mode);
  var difference = differenceZonedDateTime(ns1, ns2, timeZone, calendar, largestUnit);
  if (smallestUnit === NANOSECOND && increment === 1) return difference;
  return roundRelativeDuration(difference, ns1, ns2, getISODateTimeFor(timeZone, ns1), timeZone, calendar, largestUnit, increment, smallestUnit, mode);
}
function differenceZonedDateTimeWithTotal(ns1, ns2, timeZone, calendar, unit) {
  if (!isDateUnit(unit)) return totalTimeDuration(ns2 - ns1, unit);
  var difference = differenceZonedDateTime(ns1, ns2, timeZone, calendar, unit);
  return totalRelativeDuration(difference, ns1, ns2, getISODateTimeFor(timeZone, ns1), timeZone, calendar, unit);
}
// DifferenceTemporalZonedDateTime (6.5.9)
function differenceTemporalZonedDateTime(operation, r, other, options) {
  var o = recordOf(toZonedDateTime(other));
  if (r.calendar !== o.calendar) throwRange('The calendars differ');
  var settings = getDifferenceSettings(operation, getOptionsObject(options), 'datetime', [], NANOSECOND, HOUR);
  var v;
  if (!isDateUnit(settings.largestUnit)) {
    v = durationFromInternal(differenceInstant(r.ns, o.ns, settings.roundingIncrement, settings.smallestUnit, settings.roundingMode), settings.largestUnit);
    return createDuration(operation === 'since' ? negatedDuration(v) : v);
  }
  if (!timeZoneEquals(r.timeZone, o.timeZone)) throwRange('Differences in days or larger units need the same time zone');
  if (r.ns === o.ns) return createDuration([0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  var d = differenceZonedDateTimeWithRounding(r.ns, o.ns, r.timeZone, r.calendar, settings.largestUnit, settings.roundingIncrement, settings.smallestUnit, settings.roundingMode);
  v = durationFromInternal(d, HOUR);
  return createDuration(operation === 'since' ? negatedDuration(v) : v);
}
// AddDurationToZonedDateTime (6.5.10)
function addDurationToZonedDateTime(operation, r, like, options) {
  var v = toDurationFields(like);
  if (operation === 'subtract') v = negatedDuration(v);
  var overflow = getOverflowOption(getOptionsObject(options));
  return createZonedDateTime(addZonedDateTime(r.ns, r.timeZone, r.calendar, toInternalDuration(v), overflow), r.timeZone, r.calendar);
}
function zonedOf(x) { return slotOf(x, 'ZonedDateTime'); }
function zonedDateTimeOf(x) { var r = zonedOf(x); return getISODateTimeFor(r.timeZone, r.ns); }

class ZonedDateTime extends $Object {
  constructor(epochNanoseconds, timeZone) {
    var ns = toBigInt(epochNanoseconds);
    if (!isValidEpochNs(ns)) throwRange('The instant is out of range');
    if (typeof timeZone !== 'string') throwType('The time zone must be a string');
    var parsed = parseTimeZoneIdentifier(timeZone);
    if (parsed.offsetMinutes === undefined) {
      var zone = namedZone(parsed.name);
      if (zone === undefined) throwRange('Unknown time zone ' + parsed.name);
      timeZone = zone.id;
    } else timeZone = formatOffsetMinutes(parsed.offsetMinutes);
    var calendar = arguments[2];
    if (calendar === undefined) calendar = 'iso8601';
    if (typeof calendar !== 'string') throwType('The calendar must be a string');
    return createZonedDateTime(ns, timeZone, canonicalizeCalendar(calendar), new.target);
  }
  static from(item) { return toZonedDateTime(item, arguments[1]); }
  static compare(one, two) { var a = recordOf(toZonedDateTime(one)).ns, b = recordOf(toZonedDateTime(two)).ns; return a < b ? -1 : a > b ? 1 : 0; }
  get calendarId() { return zonedOf(this).calendar; }
  get timeZoneId() { return zonedOf(this).timeZone; }
  get epochMilliseconds() { return $Number(floorDiv(zonedOf(this).ns, 1000000n)); }
  get epochNanoseconds() { return zonedOf(this).ns; }
  get hoursInDay() {
    var r = zonedOf(this), today = getISODateTimeFor(r.timeZone, r.ns).date;
    return totalTimeDuration(getStartOfDay(r.timeZone, addDaysToISODate(today, 1)) - getStartOfDay(r.timeZone, today), HOUR);
  }
  get offsetNanoseconds() { var r = zonedOf(this); return getOffsetNsFor(r.timeZone, r.ns); }
  get offset() { var r = zonedOf(this); return formatUTCOffsetNs(getOffsetNsFor(r.timeZone, r.ns)); }
  with(temporalZonedDateTimeLike) {
    var r = zonedOf(this);
    if (!isPartialTemporalObject(temporalZonedDateTimeLike)) throwType('with() needs a partial date-time object');
    var offsetNs = getOffsetNsFor(r.timeZone, r.ns), dt = getISODateTimeFor(r.timeZone, r.ns);
    var fields = isoDateToFields(r.calendar, dt.date, 'date');
    for (var i = 0; i < 6; i++) fields[TIME_FIELDS[i]] = dt.time[TIME_FIELDS[i]];
    fields.offset = formatUTCOffsetNs(offsetNs);
    var partial = prepareCalendarFields(r.calendar, temporalZonedDateTimeLike, ['year', 'month', 'monthCode', 'day'], ['hour', 'minute', 'second', 'millisecond', 'microsecond', 'nanosecond', 'offset'], 'partial');
    fields = mergeFields(r.calendar, fields, partial);
    var options = getOptionsObject(arguments[1]);
    var disambiguation = getDisambiguationOption(options);
    var offset = getOffsetOption(options, 'prefer');
    var overflow = getOverflowOption(options);
    var result = interpretDateTimeFields(r.calendar, fields, overflow);
    var ns = interpretISODateTimeOffset(result.date, result.time, 'option', parseUTCOffsetNs(fields.offset), r.timeZone, disambiguation, offset, 'exactly');
    return createZonedDateTime(ns, r.timeZone, r.calendar);
  }
  withPlainTime() {
    var r = zonedOf(this), plainTimeLike = arguments[0], dt = getISODateTimeFor(r.timeZone, r.ns), ns;
    if (plainTimeLike === undefined) ns = getStartOfDay(r.timeZone, dt.date);
    else ns = getEpochNsFor(r.timeZone, dateTime(dt.date, recordOf(toPlainTime(plainTimeLike)).time), 'compatible');
    return createZonedDateTime(ns, r.timeZone, r.calendar);
  }
  withTimeZone(timeZoneLike) { var r = zonedOf(this); return createZonedDateTime(r.ns, toTimeZoneIdentifier(timeZoneLike), r.calendar); }
  withCalendar(calendarLike) { var r = zonedOf(this); return createZonedDateTime(r.ns, r.timeZone, toCalendarIdentifier(calendarLike)); }
  add(temporalDurationLike) { return addDurationToZonedDateTime('add', zonedOf(this), temporalDurationLike, arguments[1]); }
  subtract(temporalDurationLike) { return addDurationToZonedDateTime('subtract', zonedOf(this), temporalDurationLike, arguments[1]); }
  until(other) { return differenceTemporalZonedDateTime('until', zonedOf(this), other, arguments[1]); }
  since(other) { return differenceTemporalZonedDateTime('since', zonedOf(this), other, arguments[1]); }
  round(roundTo) {
    var r = zonedOf(this);
    if (roundTo === undefined) throwType('round needs an argument');
    var options = stringToOptions(roundTo, 'smallestUnit');
    var roundingIncrement = getRoundingIncrementOption(options);
    var roundingMode = getRoundingModeOption(options, 'halfExpand');
    var smallestUnit = getUnitOption(options, 'smallestUnit', true);
    validateUnit(smallestUnit, 'time', [DAY]);
    if (smallestUnit === DAY) validateRoundingIncrement(roundingIncrement, 1, true);
    else validateRoundingIncrement(roundingIncrement, MAX_INCREMENT[smallestUnit], false);
    if (smallestUnit === NANOSECOND && roundingIncrement === 1) return createZonedDateTime(r.ns, r.timeZone, r.calendar);
    var dt = getISODateTimeFor(r.timeZone, r.ns), ns;
    if (smallestUnit === DAY) {
      var startNs = getStartOfDay(r.timeZone, dt.date), endNs = getStartOfDay(r.timeZone, addDaysToISODate(dt.date, 1));
      ns = roundTimeDurationToIncrement(r.ns - startNs, endNs - startNs, roundingMode) + startNs;
    } else {
      var rounded = roundISODateTime(dt, roundingIncrement, smallestUnit, roundingMode);
      ns = interpretISODateTimeOffset(rounded.date, rounded.time, 'option', getOffsetNsFor(r.timeZone, r.ns), r.timeZone, 'compatible', 'prefer', 'exactly');
    }
    return createZonedDateTime(ns, r.timeZone, r.calendar);
  }
  equals(other) {
    var r = zonedOf(this), o = recordOf(toZonedDateTime(other));
    return r.ns === o.ns && timeZoneEquals(r.timeZone, o.timeZone) && r.calendar === o.calendar;
  }
  toString() {
    var r = zonedOf(this);
    var options = getOptionsObject(arguments[0]);
    var showCalendar = getShowCalendarOption(options);
    var digits = getFractionalSecondDigitsOption(options);
    var showOffset = getShowOffsetOption(options);
    var roundingMode = getRoundingModeOption(options, 'trunc');
    var smallestUnit = getUnitOption(options, 'smallestUnit');
    var showTimeZone = getShowTimeZoneOption(options);
    validateUnit(smallestUnit, 'time');
    if (smallestUnit === HOUR) throwRange('smallestUnit must be minutes or smaller');
    var precision = toSecondsStringPrecision(smallestUnit, digits);
    return zonedDateTimeToString(r, precision.precision, showCalendar, showTimeZone, showOffset, precision.increment, precision.unit, roundingMode);
  }
  toLocaleString() { var r = zonedOf(this); return intlToLocaleString(r, this, arguments, 'any', 'all', r.timeZone); }
  toJSON() { return zonedDateTimeToString(zonedOf(this), AUTO, 'auto', 'auto', 'auto'); }
  valueOf() { throwType('Use Temporal.ZonedDateTime.compare to compare zoned date-times'); }
  startOfDay() {
    var r = zonedOf(this);
    return createZonedDateTime(getStartOfDay(r.timeZone, getISODateTimeFor(r.timeZone, r.ns).date), r.timeZone, r.calendar);
  }
  getTimeZoneTransition(directionParam) {
    var r = zonedOf(this);
    if (directionParam === undefined) throwType('getTimeZoneTransition needs a direction');
    var options = stringToOptions(directionParam, 'direction');
    var direction = getOption(options, 'direction', ['next', 'previous'], undefined, true);
    if (isOffsetIdentifier(r.timeZone)) return null;
    var transition = getNamedTimeZoneTransition(r.timeZone, r.ns, direction === 'next');
    return transition === null ? null : createZonedDateTime(transition, r.timeZone, r.calendar);
  }
  toInstant() { return createInstant(zonedOf(this).ns); }
  toPlainDate() { var r = zonedOf(this); return createPlainDate(getISODateTimeFor(r.timeZone, r.ns).date, r.calendar); }
  toPlainTime() { var r = zonedOf(this); return createPlainTime(getISODateTimeFor(r.timeZone, r.ns).time); }
  toPlainDateTime() { var r = zonedOf(this); return createPlainDateTime(getISODateTimeFor(r.timeZone, r.ns), r.calendar); }
}
ZonedDateTimeProto = ZonedDateTime.prototype;
copyAccessors(ZonedDateTimeProto, dateGetters(function (x) {
  var r = zonedOf(x), out = create(null);
  out.date = getISODateTimeFor(r.timeZone, r.ns).date;
  out.calendar = r.calendar;
  return out;
}));
copyAccessors(ZonedDateTimeProto, timeGetters(function (x) { return zonedDateTimeOf(x).time; }));

// ---------------------------------------------------------------- Temporal.PlainYearMonth (9)
var PlainYearMonthProto, PlainMonthDayProto;
function createPlainYearMonth(date, calendar, newTarget) {
  if (!isoYearMonthWithinLimits(date)) throwRange('The month is out of range');
  var o = create(newTarget === undefined ? PlainYearMonthProto : protoFor(newTarget, PlainYearMonthProto));
  wmSet(slots, o, { kind: 'PlainYearMonth', date: date, calendar: calendar });
  return o;
}
// ToTemporalYearMonth (9.5.2)
function toPlainYearMonth(item, options) {
  if (isObject(item)) {
    if (kindOf(item) === 'PlainYearMonth') {
      var r = recordOf(item);
      getOverflowOption(getOptionsObject(options));
      return createPlainYearMonth(r.date, r.calendar);
    }
    var calendar = calendarWithISODefault(item);
    var fields = prepareCalendarFields(calendar, item, ['year', 'month', 'monthCode'], [], []);
    var overflow = getOverflowOption(getOptionsObject(options));
    return createPlainYearMonth(calendarYearMonthFromFields(calendar, fields, overflow), calendar);
  }
  if (typeof item !== 'string') throwType('A year and month must be an object or a string');
  var parsed = parseISODateTime(item, ['yearMonth']);
  var cal = canonicalizeCalendar(parsed.calendar === undefined ? 'iso8601' : parsed.calendar);
  getOverflowOption(getOptionsObject(options));
  var date = isoDate(parsed.year, parsed.month, parsed.day);
  if (!isoYearMonthWithinLimits(date)) throwRange('The month is out of range');
  return createPlainYearMonth(calendarYearMonthFromFields(cal, isoDateToFields(cal, date, 'year-month'), 'constrain'), cal);
}
function yearMonthToString(r, showCalendar) {
  var s = padISOYear(r.date.year) + '-' + pad(r.date.month, 2);
  if (showCalendar === 'always' || showCalendar === 'critical' || r.calendar !== 'iso8601') s += '-' + pad(r.date.day, 2);
  return s + formatCalendarAnnotation(r.calendar, showCalendar);
}
function firstOfMonth(r) {
  var fields = isoDateToFields(r.calendar, r.date, 'year-month');
  fields.day = 1;
  return calendarDateFromFields(r.calendar, fields, 'constrain');
}
// DifferenceTemporalPlainYearMonth (9.5.7)
function differenceTemporalPlainYearMonth(operation, r, other, options) {
  var o = recordOf(toPlainYearMonth(other));
  if (r.calendar !== o.calendar) throwRange('The calendars differ');
  var settings = getDifferenceSettings(operation, getOptionsObject(options), 'date', [WEEK, DAY], MONTH, YEAR);
  if (compareISODate(r.date, o.date) === 0) return createDuration([0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  var thisDate = firstOfMonth(r), otherDate = firstOfMonth(o);
  var dateDifference = calendarDateUntil(r.calendar, thisDate, otherDate, settings.largestUnit);
  var d = internalDuration(adjustDateDuration(dateDifference, 0, 0), 0n);
  if (settings.smallestUnit !== MONTH || settings.roundingIncrement !== 1) {
    var dt = dateTime(thisDate, MIDNIGHT);
    d = roundRelativeDuration(d, utcEpochNs(thisDate, MIDNIGHT), utcEpochNs(otherDate, MIDNIGHT), dt, undefined, r.calendar,
                              settings.largestUnit, settings.roundingIncrement, settings.smallestUnit, settings.roundingMode);
  }
  var v = durationFromInternal(d, DAY);
  return createDuration(operation === 'since' ? negatedDuration(v) : v);
}
// AddDurationToYearMonth (9.5.8)
function addDurationToYearMonth(operation, r, like, options) {
  var v = toDurationFields(like);
  if (operation === 'subtract') v = negatedDuration(v);
  var d = toInternalDuration(v);
  var overflow = getOverflowOption(getOptionsObject(options));
  if (d.date.weeks !== 0 || d.date.days !== 0 || d.time !== 0n) throwRange('Only years and months can be added to a year and month');
  var added = calendarDateAdd(r.calendar, firstOfMonth(r), d.date, overflow);
  return createPlainYearMonth(calendarYearMonthFromFields(r.calendar, isoDateToFields(r.calendar, added, 'year-month'), overflow), r.calendar);
}
function yearMonthOf(x) { return slotOf(x, 'PlainYearMonth'); }

class PlainYearMonth extends $Object {
  constructor(isoYear, isoMonth) {
    var reference = arguments[3];
    if (reference === undefined) reference = 1;
    var y = toIntegerWithTruncation(isoYear), m = toIntegerWithTruncation(isoMonth);
    var calendar = arguments[2];
    if (calendar === undefined) calendar = 'iso8601';
    if (typeof calendar !== 'string') throwType('The calendar must be a string');
    calendar = canonicalizeCalendar(calendar);
    var ref = toIntegerWithTruncation(reference);
    if (!isValidISODate(y, m, ref)) throwRange('The date does not exist');
    return createPlainYearMonth(isoDate(y, m, ref), calendar, new.target);
  }
  static from(item) { return toPlainYearMonth(item, arguments[1]); }
  static compare(one, two) { return compareISODate(recordOf(toPlainYearMonth(one)).date, recordOf(toPlainYearMonth(two)).date); }
  get calendarId() { return yearMonthOf(this).calendar; }
  get era() { var r = yearMonthOf(this); return calendarDate(r.calendar, r.date).era; }
  get eraYear() { var r = yearMonthOf(this); return calendarDate(r.calendar, r.date).eraYear; }
  get year() { var r = yearMonthOf(this); return calendarDate(r.calendar, r.date).year; }
  get month() { var r = yearMonthOf(this); return calendarDate(r.calendar, r.date).month; }
  get monthCode() { var r = yearMonthOf(this); return calendarDate(r.calendar, r.date).monthCode; }
  get daysInYear() { var r = yearMonthOf(this); return calendarDate(r.calendar, r.date).daysInYear; }
  get daysInMonth() { var r = yearMonthOf(this); return calendarDate(r.calendar, r.date).daysInMonth; }
  get monthsInYear() { var r = yearMonthOf(this); return calendarDate(r.calendar, r.date).monthsInYear; }
  get inLeapYear() { var r = yearMonthOf(this); return calendarDate(r.calendar, r.date).inLeapYear; }
  with(temporalYearMonthLike) {
    var r = yearMonthOf(this);
    if (!isPartialTemporalObject(temporalYearMonthLike)) throwType('with() needs a partial year-month object');
    var fields = isoDateToFields(r.calendar, r.date, 'year-month');
    var partial = prepareCalendarFields(r.calendar, temporalYearMonthLike, ['year', 'month', 'monthCode'], [], 'partial');
    fields = mergeFields(r.calendar, fields, partial);
    var overflow = getOverflowOption(getOptionsObject(arguments[1]));
    return createPlainYearMonth(calendarYearMonthFromFields(r.calendar, fields, overflow), r.calendar);
  }
  add(temporalDurationLike) { return addDurationToYearMonth('add', yearMonthOf(this), temporalDurationLike, arguments[1]); }
  subtract(temporalDurationLike) { return addDurationToYearMonth('subtract', yearMonthOf(this), temporalDurationLike, arguments[1]); }
  until(other) { return differenceTemporalPlainYearMonth('until', yearMonthOf(this), other, arguments[1]); }
  since(other) { return differenceTemporalPlainYearMonth('since', yearMonthOf(this), other, arguments[1]); }
  equals(other) {
    var r = yearMonthOf(this), o = recordOf(toPlainYearMonth(other));
    return compareISODate(r.date, o.date) === 0 && r.calendar === o.calendar;
  }
  toString() { var r = yearMonthOf(this); return yearMonthToString(r, getShowCalendarOption(getOptionsObject(arguments[0]))); }
  toLocaleString() { return intlToLocaleString(yearMonthOf(this), this, arguments, 'date', 'date'); }
  toJSON() { return yearMonthToString(yearMonthOf(this), 'auto'); }
  valueOf() { throwType('Use Temporal.PlainYearMonth.compare to compare year-months'); }
  toPlainDate(item) {
    var r = yearMonthOf(this);
    if (!isObject(item)) throwType('toPlainDate needs an object with a day');
    var fields = isoDateToFields(r.calendar, r.date, 'year-month');
    var input = prepareCalendarFields(r.calendar, item, ['day'], [], []);
    return createPlainDate(calendarDateFromFields(r.calendar, mergeFields(r.calendar, fields, input), 'constrain'), r.calendar);
  }
}
PlainYearMonthProto = PlainYearMonth.prototype;

// ---------------------------------------------------------------- Temporal.PlainMonthDay (10)
function createPlainMonthDay(date, calendar, newTarget) {
  if (!isoDateWithinLimits(date)) throwRange('The date is out of range');
  var o = create(newTarget === undefined ? PlainMonthDayProto : protoFor(newTarget, PlainMonthDayProto));
  wmSet(slots, o, { kind: 'PlainMonthDay', date: date, calendar: calendar });
  return o;
}
// ToTemporalMonthDay (10.5.1)
function toPlainMonthDay(item, options) {
  if (isObject(item)) {
    if (kindOf(item) === 'PlainMonthDay') {
      var r = recordOf(item);
      getOverflowOption(getOptionsObject(options));
      return createPlainMonthDay(r.date, r.calendar);
    }
    var calendar = calendarWithISODefault(item);
    var fields = prepareCalendarFields(calendar, item, ['year', 'month', 'monthCode', 'day'], [], []);
    var overflow = getOverflowOption(getOptionsObject(options));
    return createPlainMonthDay(calendarMonthDayFromFields(calendar, fields, overflow), calendar);
  }
  if (typeof item !== 'string') throwType('A month and day must be an object or a string');
  var parsed = parseISODateTime(item, ['monthDay']);
  var cal = canonicalizeCalendar(parsed.calendar === undefined ? 'iso8601' : parsed.calendar);
  getOverflowOption(getOptionsObject(options));
  return createPlainMonthDay(isoDate(1972, parsed.month, parsed.day), cal);
}
function monthDayToString(r, showCalendar) {
  var s = pad(r.date.month, 2) + '-' + pad(r.date.day, 2);
  if (showCalendar === 'always' || showCalendar === 'critical' || r.calendar !== 'iso8601') s = padISOYear(r.date.year) + '-' + s;
  return s + formatCalendarAnnotation(r.calendar, showCalendar);
}
function monthDayOf(x) { return slotOf(x, 'PlainMonthDay'); }

class PlainMonthDay extends $Object {
  constructor(isoMonth, isoDay) {
    var reference = arguments[3];
    if (reference === undefined) reference = 1972;
    var m = toIntegerWithTruncation(isoMonth), d = toIntegerWithTruncation(isoDay);
    var calendar = arguments[2];
    if (calendar === undefined) calendar = 'iso8601';
    if (typeof calendar !== 'string') throwType('The calendar must be a string');
    calendar = canonicalizeCalendar(calendar);
    var y = toIntegerWithTruncation(reference);
    if (!isValidISODate(y, m, d)) throwRange('The date does not exist');
    return createPlainMonthDay(isoDate(y, m, d), calendar, new.target);
  }
  static from(item) { return toPlainMonthDay(item, arguments[1]); }
  get calendarId() { return monthDayOf(this).calendar; }
  get monthCode() { var r = monthDayOf(this); return calendarDate(r.calendar, r.date).monthCode; }
  get day() { var r = monthDayOf(this); return calendarDate(r.calendar, r.date).day; }
  with(temporalMonthDayLike) {
    var r = monthDayOf(this);
    if (!isPartialTemporalObject(temporalMonthDayLike)) throwType('with() needs a partial month-day object');
    var fields = isoDateToFields(r.calendar, r.date, 'month-day');
    var partial = prepareCalendarFields(r.calendar, temporalMonthDayLike, ['year', 'month', 'monthCode', 'day'], [], 'partial');
    fields = mergeFields(r.calendar, fields, partial);
    var overflow = getOverflowOption(getOptionsObject(arguments[1]));
    return createPlainMonthDay(calendarMonthDayFromFields(r.calendar, fields, overflow), r.calendar);
  }
  equals(other) {
    var r = monthDayOf(this), o = recordOf(toPlainMonthDay(other));
    return compareISODate(r.date, o.date) === 0 && r.calendar === o.calendar;
  }
  toString() { var r = monthDayOf(this); return monthDayToString(r, getShowCalendarOption(getOptionsObject(arguments[0]))); }
  toLocaleString() { return intlToLocaleString(monthDayOf(this), this, arguments, 'date', 'date'); }
  toJSON() { return monthDayToString(monthDayOf(this), 'auto'); }
  valueOf() { throwType('Use equals to compare month-days'); }
  toPlainDate(item) {
    var r = monthDayOf(this);
    if (!isObject(item)) throwType('toPlainDate needs an object with a year');
    var fields = isoDateToFields(r.calendar, r.date, 'month-day');
    var input = prepareCalendarFields(r.calendar, item, ['year'], [], []);
    return createPlainDate(calendarDateFromFields(r.calendar, mergeFields(r.calendar, fields, input), 'constrain'), r.calendar);
  }
}
PlainMonthDayProto = PlainMonthDay.prototype;

// ---------------------------------------------------------------- Temporal.Now (2)
function systemEpochNs() { return $BigInt(dateNow()) * 1000000n; }
function systemDateTime(temporalTimeZoneLike) {
  var timeZone = temporalTimeZoneLike === undefined ? systemTimeZone() : toTimeZoneIdentifier(temporalTimeZoneLike);
  return getISODateTimeFor(timeZone, systemEpochNs());
}
var Now = {
  timeZoneId() { return systemTimeZone(); },
  instant() { return createInstant(systemEpochNs()); },
  plainDateTimeISO() { return createPlainDateTime(systemDateTime(arguments[0]), 'iso8601'); },
  zonedDateTimeISO() {
    var tz = arguments[0];
    var timeZone = tz === undefined ? systemTimeZone() : toTimeZoneIdentifier(tz);
    return createZonedDateTime(systemEpochNs(), timeZone, 'iso8601');
  },
  plainDateISO() { return createPlainDate(systemDateTime(arguments[0]).date, 'iso8601'); },
  plainTimeISO() { return createPlainTime(systemDateTime(arguments[0]).time); }
};

// ---------------------------------------------------------------- the Temporal object (1)
var Temporal = {};
function hidden(target, name, value) { defineProperty(target, name, { value: value, writable: true, enumerable: false, configurable: true }); }
function tag(target, name) { defineProperty(target, symToStringTag, { value: name, writable: false, enumerable: false, configurable: true }); }
var classes = { Duration: Duration, Instant: Instant, PlainDate: PlainDate, PlainDateTime: PlainDateTime, PlainMonthDay: PlainMonthDay,
                PlainTime: PlainTime, PlainYearMonth: PlainYearMonth, ZonedDateTime: ZonedDateTime };
var classNames = getOwnPropertyNames(classes);
for (var ci = 0; ci < classNames.length; ci++) {
  var cls = classes[classNames[ci]];
  setPrototypeOf(cls, P.Function.value.prototype);  // `extends Object` only spares the constructors a `this` they never use
  tag(cls.prototype, 'Temporal.' + classNames[ci]);
  hidden(Temporal, classNames[ci], cls);
}
var nowObject = {}, nowNames = getOwnPropertyNames(Now);
for (var ni = 0; ni < nowNames.length; ni++) hidden(nowObject, nowNames[ni], Now[nowNames[ni]]);
tag(nowObject, 'Temporal.Now');
hidden(Temporal, 'Now', nowObject);
tag(Temporal, 'Temporal');
natives.temporal = Temporal;
natives.protos = create(null);  // for protoFor when a constructor of another realm has no prototype object
for (var pi = 0; pi < classNames.length; pi++) natives.protos[classNames[pi]] = classes[classNames[pi]].prototype;
// What Intl.DateTimeFormat needs of a Temporal object (ECMA-402 with Temporal, 15.6.22), or undefined for any other
// value. Its fields are those of the ISO date and time; epochMs for an Instant or ZonedDateTime.
// For Intl.DurationFormat, which takes a duration string as Temporal does
natives.durationFromString = function (s) { return temporalInfo(recordOf(parseDurationString(s))); };
natives.info = function (x) {
  var kind = kindOf(x);
  if (kind === undefined) return undefined;
  return temporalInfo(recordOf(x));
};
// Date.prototype.toTemporalInstant (14.1.1): installed by the engine without building Temporal until it is first read
var dateValueOf = uncurry(P.Date.proto.valueOf);
natives.toTemporalInstant = {
  toTemporalInstant() {
    var t = dateValueOf(this);
    if (t !== t) throwRange('Invalid Date');
    return createInstant($BigInt(t) * 1000000n);
  }
}.toTemporalInstant;
})
