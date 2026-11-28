// ---------------------------------------------------------------- Temporal.PlainDate (3)
var PlainDateProto, PlainTimeProto, PlainDateTimeProto;
function createPlainDate(date, calendar, newTarget) {
  if (!isoDateWithinLimits(date)) throwRange('The date is out of range');
  var o = create(newTarget === undefined ? PlainDateProto : protoFor(newTarget, PlainDateProto));
  wmSet(slots, o, { kind: 'PlainDate', date: date, calendar: calendar });
  return o;
}
// ToTemporalDate (3.5.4): a PlainDate
function toPlainDate(item, options) {
  if (isObject(item)) {
    var k = kindOf(item), r = recordOf(item);
    if (k === 'PlainDate') { getOverflowOption(getOptionsObject(options)); return createPlainDate(r.date, r.calendar); }
    if (k === 'ZonedDateTime') {
      var dt = getISODateTimeFor(r.timeZone, r.ns);
      getOverflowOption(getOptionsObject(options));
      return createPlainDate(dt.date, r.calendar);
    }
    if (k === 'PlainDateTime') { getOverflowOption(getOptionsObject(options)); return createPlainDate(r.date, r.calendar); }
    var calendar = calendarWithISODefault(item);
    var fields = prepareCalendarFields(calendar, item, ['year', 'month', 'monthCode', 'day'], [], []);
    var overflow = getOverflowOption(getOptionsObject(options));
    return createPlainDate(calendarDateFromFields(calendar, fields, overflow), calendar);
  }
  if (typeof item !== 'string') throwType('A date must be an object or a string');
  var parsed = parseISODateTime(item, ['dateTime']);
  var cal = canonicalizeCalendar(parsed.calendar === undefined ? 'iso8601' : parsed.calendar);
  getOverflowOption(getOptionsObject(options));
  return createPlainDate(isoDate(parsed.year, parsed.month, parsed.day), cal);
}
function dateGetters(dateOf) {
  // the calendar date properties shared by PlainDate, PlainDateTime and ZonedDateTime
  return {
    get era() { dateOf(this); return undefined; },
    get eraYear() { dateOf(this); return undefined; },
    get year() { return dateOf(this).year; },
    get month() { return dateOf(this).month; },
    get monthCode() { return monthCode(dateOf(this).month); },
    get day() { return dateOf(this).day; },
    get dayOfWeek() { return dayOfWeek(dateOf(this)); },
    get dayOfYear() { return dayOfYear(dateOf(this)); },
    get weekOfYear() { return weekOfYear(dateOf(this)).week; },
    get yearOfWeek() { return weekOfYear(dateOf(this)).year; },
    get daysInWeek() { dateOf(this); return 7; },
    get daysInMonth() { var d = dateOf(this); return daysInMonth(d.year, d.month); },
    get daysInYear() { return daysInYear(dateOf(this).year); },
    get monthsInYear() { dateOf(this); return 12; },
    get inLeapYear() { return isLeapYear(dateOf(this).year); }
  };
}
function differenceTemporalPlainDate(operation, r, other, options) {
  var o = recordOf(toPlainDate(other));
  if (r.calendar !== o.calendar) throwRange('The calendars differ');
  var settings = getDifferenceSettings(operation, getOptionsObject(options), 'date', [], DAY, DAY);
  if (compareISODate(r.date, o.date) === 0) return createDuration([0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  var d = internalDuration(calendarDateUntil(r.calendar, r.date, o.date, settings.largestUnit), 0n);
  if (settings.smallestUnit !== DAY || settings.roundingIncrement !== 1) {
    var dt = dateTime(r.date, MIDNIGHT);
    d = roundRelativeDuration(d, utcEpochNs(r.date, MIDNIGHT), utcEpochNs(o.date, MIDNIGHT), dt, undefined, r.calendar,
                              settings.largestUnit, settings.roundingIncrement, settings.smallestUnit, settings.roundingMode);
  }
  var v = durationFromInternal(d, DAY);
  return createDuration(operation === 'since' ? negatedDuration(v) : v);
}
function addDurationToDate(operation, r, like, options) {
  var v = toDurationFields(like);
  if (operation === 'subtract') v = negatedDuration(v);
  var dateDur = toDateDurationWithoutTime(v);
  var overflow = getOverflowOption(getOptionsObject(options));
  return createPlainDate(calendarDateAdd(r.calendar, r.date, dateDur, overflow), r.calendar);
}
// IsPartialTemporalObject (13.24)
function isPartialTemporalObject(value) {
  if (!isObject(value)) return false;
  var k = kindOf(value);
  if (k === 'PlainDate' || k === 'PlainDateTime' || k === 'PlainMonthDay' || k === 'PlainTime' || k === 'PlainYearMonth' || k === 'ZonedDateTime') return false;
  if (value.calendar !== undefined) return false;
  if (value.timeZone !== undefined) return false;
  return true;
}
function plainDateOf(x) { return slotOf(x, 'PlainDate').date; }

class PlainDate extends $Object {
  constructor(isoYear, isoMonth, isoDay) {
    var y = toIntegerWithTruncation(isoYear), m = toIntegerWithTruncation(isoMonth), d = toIntegerWithTruncation(isoDay);
    var calendar = arguments[3];
    if (calendar === undefined) calendar = 'iso8601';
    if (typeof calendar !== 'string') throwType('The calendar must be a string');
    calendar = canonicalizeCalendar(calendar);
    if (!isValidISODate(y, m, d)) throwRange('The date does not exist');
    return createPlainDate(isoDate(y, m, d), calendar, new.target);
  }
  static from(item) { return toPlainDate(item, arguments[1]); }
  static compare(one, two) { return compareISODate(recordOf(toPlainDate(one)).date, recordOf(toPlainDate(two)).date); }
  get calendarId() { return slotOf(this, 'PlainDate').calendar; }
  toPlainYearMonth() {
    var r = slotOf(this, 'PlainDate');
    return createPlainYearMonth(calendarYearMonthFromFields(r.calendar, isoDateToFields(r.date, 'date'), 'constrain'), r.calendar);
  }
  toPlainMonthDay() {
    var r = slotOf(this, 'PlainDate');
    return createPlainMonthDay(calendarMonthDayFromFields(r.calendar, isoDateToFields(r.date, 'date'), 'constrain'), r.calendar);
  }
  add(temporalDurationLike) { return addDurationToDate('add', slotOf(this, 'PlainDate'), temporalDurationLike, arguments[1]); }
  subtract(temporalDurationLike) { return addDurationToDate('subtract', slotOf(this, 'PlainDate'), temporalDurationLike, arguments[1]); }
  with(temporalDateLike) {
    var r = slotOf(this, 'PlainDate');
    if (!isPartialTemporalObject(temporalDateLike)) throwType('with() needs a partial date object');
    var fields = isoDateToFields(r.date, 'date');
    var partial = prepareCalendarFields(r.calendar, temporalDateLike, ['year', 'month', 'monthCode', 'day'], [], 'partial');
    fields = mergeFields(fields, partial);
    var overflow = getOverflowOption(getOptionsObject(arguments[1]));
    return createPlainDate(calendarDateFromFields(r.calendar, fields, overflow), r.calendar);
  }
  withCalendar(calendarLike) {
    var r = slotOf(this, 'PlainDate');
    return createPlainDate(r.date, toCalendarIdentifier(calendarLike));
  }
  until(other) { return differenceTemporalPlainDate('until', slotOf(this, 'PlainDate'), other, arguments[1]); }
  since(other) { return differenceTemporalPlainDate('since', slotOf(this, 'PlainDate'), other, arguments[1]); }
  equals(other) {
    var r = slotOf(this, 'PlainDate'), o = recordOf(toPlainDate(other));
    return compareISODate(r.date, o.date) === 0 && r.calendar === o.calendar;
  }
  toPlainDateTime() {
    var r = slotOf(this, 'PlainDate');
    return createPlainDateTime(dateTime(r.date, toTimeRecordOrMidnight(arguments[0])), r.calendar);
  }
  toZonedDateTime(item) {
    var r = slotOf(this, 'PlainDate'), timeZone, temporalTime;
    if (isObject(item)) {
      var timeZoneLike = item.timeZone;
      if (timeZoneLike === undefined) timeZone = toTimeZoneIdentifier(item);
      else { timeZone = toTimeZoneIdentifier(timeZoneLike); temporalTime = item.plainTime; }
    } else timeZone = toTimeZoneIdentifier(item);
    var ns;
    if (temporalTime === undefined) ns = getStartOfDay(timeZone, r.date);
    else {
      var t = recordOf(toPlainTime(temporalTime)).time;
      if (!isoDateTimeWithinLimits(r.date, t)) throwRange('The date and time are out of range');
      ns = getEpochNsFor(timeZone, dateTime(r.date, t), 'compatible');
    }
    return createZonedDateTime(ns, timeZone, r.calendar);
  }
  toString() {
    var r = slotOf(this, 'PlainDate');
    var showCalendar = getShowCalendarOption(getOptionsObject(arguments[0]));
    return dateString(r.date) + formatCalendarAnnotation(r.calendar, showCalendar);
  }
  toLocaleString() { return intlToLocaleString(slotOf(this, 'PlainDate'), this, arguments, 'date', 'date'); }
  toJSON() { var r = slotOf(this, 'PlainDate'); return dateString(r.date) + formatCalendarAnnotation(r.calendar, 'auto'); }
  valueOf() { throwType('Use Temporal.PlainDate.compare to compare dates'); }
}
PlainDateProto = PlainDate.prototype;
copyAccessors(PlainDateProto, dateGetters(plainDateOf));

// ---------------------------------------------------------------- Temporal.PlainTime (4)
function createPlainTime(t, newTarget) {
  var o = create(newTarget === undefined ? PlainTimeProto : protoFor(newTarget, PlainTimeProto));
  wmSet(slots, o, { kind: 'PlainTime', time: time(t.hour, t.minute, t.second, t.millisecond, t.microsecond, t.nanosecond) });
  return o;
}
// ToTemporalTimeRecord (4.5.12)
function toTimeRecordFields(like, partial) {
  var names = ['hour', 'microsecond', 'millisecond', 'minute', 'nanosecond', 'second'], result = {}, any = false;
  for (var i = 0; i < 6; i++) {
    var value = like[names[i]];
    if (value !== undefined) { result[names[i]] = toIntegerWithTruncation(value); any = true; }
    else if (!partial) result[names[i]] = 0;
  }
  if (!any) throwType('A time-like object needs at least one time unit');
  return result;
}
function toPlainTime(item, options) {
  var t;
  if (isObject(item)) {
    var k = kindOf(item), r = recordOf(item);
    if (k === 'PlainTime') { getOverflowOption(getOptionsObject(options)); return createPlainTime(r.time); }
    if (k === 'PlainDateTime') { getOverflowOption(getOptionsObject(options)); return createPlainTime(r.time); }
    if (k === 'ZonedDateTime') {
      var dt = getISODateTimeFor(r.timeZone, r.ns);
      getOverflowOption(getOptionsObject(options));
      return createPlainTime(dt.time);
    }
    var f = toTimeRecordFields(item, false);
    var overflow = getOverflowOption(getOptionsObject(options));
    t = regulateTime(f.hour, f.minute, f.second, f.millisecond, f.microsecond, f.nanosecond, overflow);
  } else {
    if (typeof item !== 'string') throwType('A time must be an object or a string');
    t = parseISODateTime(item, ['time']).time;
    getOverflowOption(getOptionsObject(options));
  }
  return createPlainTime(t);
}
function toTimeRecordOrMidnight(item) { return item === undefined ? MIDNIGHT : recordOf(toPlainTime(item)).time; }
function plainTimeOf(x) { return slotOf(x, 'PlainTime').time; }
function differenceTemporalPlainTime(operation, t, other, options) {
  var o = recordOf(toPlainTime(other)).time;
  var settings = getDifferenceSettings(operation, getOptionsObject(options), 'time', [], NANOSECOND, HOUR);
  var d = roundTimeDuration(differenceTime(t, o), settings.roundingIncrement, settings.smallestUnit, settings.roundingMode);
  var v = durationFromInternal(internalDuration(ZERO_DATE_DURATION, d), settings.largestUnit);
  return createDuration(operation === 'since' ? negatedDuration(v) : v);
}
function addDurationToTime(operation, t, like) {
  var v = toDurationFields(like);
  if (operation === 'subtract') v = negatedDuration(v);
  return createPlainTime(addTime(t, toInternalDuration(v).time));
}
function timeGetters(timeOf) {
  return {
    get hour() { return timeOf(this).hour; },
    get minute() { return timeOf(this).minute; },
    get second() { return timeOf(this).second; },
    get millisecond() { return timeOf(this).millisecond; },
    get microsecond() { return timeOf(this).microsecond; },
    get nanosecond() { return timeOf(this).nanosecond; }
  };
}

class PlainTime extends $Object {
  constructor() {
    var a = arguments, v = list();
    for (var i = 0; i < 6; i++) v[i] = a[i] === undefined ? 0 : toIntegerWithTruncation(a[i]);
    if (!isValidTime(v[0], v[1], v[2], v[3], v[4], v[5])) throwRange('The time does not exist');
    return createPlainTime(time(v[0], v[1], v[2], v[3], v[4], v[5]), new.target);
  }
  static from(item) { return toPlainTime(item, arguments[1]); }
  static compare(one, two) { return compareTime(recordOf(toPlainTime(one)).time, recordOf(toPlainTime(two)).time); }
  add(temporalDurationLike) { return addDurationToTime('add', plainTimeOf(this), temporalDurationLike); }
  subtract(temporalDurationLike) { return addDurationToTime('subtract', plainTimeOf(this), temporalDurationLike); }
  with(temporalTimeLike) {
    var t = plainTimeOf(this);
    if (!isPartialTemporalObject(temporalTimeLike)) throwType('with() needs a partial time object');
    var p = toTimeRecordFields(temporalTimeLike, true);
    var overflow = getOverflowOption(getOptionsObject(arguments[1]));
    var get = function (k) { return p[k] !== undefined ? p[k] : t[k]; };
    return createPlainTime(regulateTime(get('hour'), get('minute'), get('second'), get('millisecond'), get('microsecond'), get('nanosecond'), overflow));
  }
  until(other) { return differenceTemporalPlainTime('until', plainTimeOf(this), other, arguments[1]); }
  since(other) { return differenceTemporalPlainTime('since', plainTimeOf(this), other, arguments[1]); }
  round(roundTo) {
    var t = plainTimeOf(this);
    if (roundTo === undefined) throwType('round needs an argument');
    var options = stringToOptions(roundTo, 'smallestUnit');
    var roundingIncrement = getRoundingIncrementOption(options);
    var roundingMode = getRoundingModeOption(options, 'halfExpand');
    var smallestUnit = getUnitOption(options, 'smallestUnit', true);
    validateUnit(smallestUnit, 'time');
    validateRoundingIncrement(roundingIncrement, MAX_INCREMENT[smallestUnit], false);
    return createPlainTime(roundTime(t, roundingIncrement, smallestUnit, roundingMode));
  }
  equals(other) { return compareTime(plainTimeOf(this), recordOf(toPlainTime(other)).time) === 0; }
  toString() {
    var t = plainTimeOf(this);
    var options = getOptionsObject(arguments[0]);
    var digits = getFractionalSecondDigitsOption(options);
    var roundingMode = getRoundingModeOption(options, 'trunc');
    var smallestUnit = getUnitOption(options, 'smallestUnit');
    validateUnit(smallestUnit, 'time');
    if (smallestUnit === HOUR) throwRange('smallestUnit must be minutes or smaller');
    var precision = toSecondsStringPrecision(smallestUnit, digits);
    return timeRecordToString(roundTime(t, precision.increment, precision.unit, roundingMode), precision.precision);
  }
  toLocaleString() { return intlToLocaleString(slotOf(this, 'PlainTime'), this, arguments, 'time', 'time'); }
  toJSON() { return timeRecordToString(plainTimeOf(this), AUTO); }
  valueOf() { throwType('Use Temporal.PlainTime.compare to compare times'); }
}
PlainTimeProto = PlainTime.prototype;
copyAccessors(PlainTimeProto, timeGetters(plainTimeOf));

// ---------------------------------------------------------------- Temporal.PlainDateTime (5)
function createPlainDateTime(dt, calendar, newTarget) {
  if (!isoDateTimeWithinLimits(dt.date, dt.time)) throwRange('The date and time are out of range');
  var o = create(newTarget === undefined ? PlainDateTimeProto : protoFor(newTarget, PlainDateTimeProto));
  wmSet(slots, o, { kind: 'PlainDateTime', date: dt.date, time: dt.time, calendar: calendar });
  return o;
}
// InterpretTemporalDateTimeFields (5.5.5)
function interpretDateTimeFields(calendar, fields, overflow) {
  var date = calendarDateFromFields(calendar, fields, overflow);
  var t = regulateTime(fields.hour, fields.minute, fields.second, fields.millisecond, fields.microsecond, fields.nanosecond, overflow);
  return dateTime(date, t);
}
function toPlainDateTime(item, options) {
  if (isObject(item)) {
    var k = kindOf(item), r = recordOf(item);
    if (k === 'PlainDateTime') { getOverflowOption(getOptionsObject(options)); return createPlainDateTime(dateTime(r.date, r.time), r.calendar); }
    if (k === 'ZonedDateTime') {
      var dt = getISODateTimeFor(r.timeZone, r.ns);
      getOverflowOption(getOptionsObject(options));
      return createPlainDateTime(dt, r.calendar);
    }
    if (k === 'PlainDate') { getOverflowOption(getOptionsObject(options)); return createPlainDateTime(dateTime(r.date, MIDNIGHT), r.calendar); }
    var calendar = calendarWithISODefault(item);
    var fields = prepareCalendarFields(calendar, item, ['year', 'month', 'monthCode', 'day'], TIME_FIELDS, []);
    var overflow = getOverflowOption(getOptionsObject(options));
    return createPlainDateTime(interpretDateTimeFields(calendar, fields, overflow), calendar);
  }
  if (typeof item !== 'string') throwType('A date-time must be an object or a string');
  var parsed = parseISODateTime(item, ['dateTime']);
  var t = parsed.time === undefined ? MIDNIGHT : parsed.time;
  var cal = canonicalizeCalendar(parsed.calendar === undefined ? 'iso8601' : parsed.calendar);
  getOverflowOption(getOptionsObject(options));
  return createPlainDateTime(dateTime(isoDate(parsed.year, parsed.month, parsed.day), t), cal);
}
function plainDateTimeOf(x) { return slotOf(x, 'PlainDateTime'); }
// DifferenceISODateTime (5.5.12)
function differenceISODateTime(dt1, dt2, calendar, largestUnit) {
  var t = differenceTime(dt1.time, dt2.time), timeSign = bigSign(t), dateSign = compareISODate(dt1.date, dt2.date), adjusted = dt2.date;
  if (timeSign === dateSign) { adjusted = addDaysToISODate(adjusted, timeSign); t = add24HourDays(t, -timeSign); }
  var dateLargest = largerUnit(DAY, largestUnit);
  var dd = calendarDateUntil(calendar, dt1.date, adjusted, dateLargest);
  if (largestUnit !== dateLargest) { t = add24HourDays(t, dd.days); dd = dateDuration(dd.years, dd.months, dd.weeks, 0); }
  return internalDuration(dd, t);
}
function differencePlainDateTimeWithRounding(dt1, dt2, calendar, largestUnit, increment, smallestUnit, mode) {
  if (compareISODateTime(dt1, dt2) === 0) return internalDuration(ZERO_DATE_DURATION, 0n);
  if (!isoDateTimeWithinLimits(dt1.date, dt1.time) || !isoDateTimeWithinLimits(dt2.date, dt2.time)) throwRange('The date and time are out of range');
  var diff = differenceISODateTime(dt1, dt2, calendar, largestUnit);
  if (smallestUnit === NANOSECOND && increment === 1) return diff;
  return roundRelativeDuration(diff, utcEpochNs(dt1.date, dt1.time), utcEpochNs(dt2.date, dt2.time), dt1, undefined, calendar, largestUnit, increment, smallestUnit, mode);
}
function differencePlainDateTimeWithTotal(dt1, dt2, calendar, unit) {
  if (compareISODateTime(dt1, dt2) === 0) return 0;
  if (!isoDateTimeWithinLimits(dt1.date, dt1.time) || !isoDateTimeWithinLimits(dt2.date, dt2.time)) throwRange('The date and time are out of range');
  var diff = differenceISODateTime(dt1, dt2, calendar, unit);
  if (unit === NANOSECOND) return $Number(diff.time);
  return totalRelativeDuration(diff, utcEpochNs(dt1.date, dt1.time), utcEpochNs(dt2.date, dt2.time), dt1, undefined, calendar, unit);
}
function differenceTemporalPlainDateTime(operation, r, other, options) {
  var o = recordOf(toPlainDateTime(other));
  if (r.calendar !== o.calendar) throwRange('The calendars differ');
  var settings = getDifferenceSettings(operation, getOptionsObject(options), 'datetime', [], NANOSECOND, DAY);
  var a = dateTime(r.date, r.time), b = dateTime(o.date, o.time);
  if (compareISODateTime(a, b) === 0) return createDuration([0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  var d = differencePlainDateTimeWithRounding(a, b, r.calendar, settings.largestUnit, settings.roundingIncrement, settings.smallestUnit, settings.roundingMode);
  var v = durationFromInternal(d, settings.largestUnit);
  return createDuration(operation === 'since' ? negatedDuration(v) : v);
}
function addDurationToDateTime(operation, r, like, options) {
  var v = toDurationFields(like);
  if (operation === 'subtract') v = negatedDuration(v);
  var overflow = getOverflowOption(getOptionsObject(options));
  var d = toInternalDurationWith24HourDays(v);
  var t = addTime(r.time, d.time);
  var dateDur = adjustDateDuration(d.date, t.days);
  return createPlainDateTime(dateTime(calendarDateAdd(r.calendar, r.date, dateDur, overflow), t), r.calendar);
}

class PlainDateTime extends $Object {
  constructor(isoYear, isoMonth, isoDay) {
    var y = toIntegerWithTruncation(isoYear), mo = toIntegerWithTruncation(isoMonth), d = toIntegerWithTruncation(isoDay), a = arguments, t = list();
    for (var i = 0; i < 6; i++) t[i] = a[i + 3] === undefined ? 0 : toIntegerWithTruncation(a[i + 3]);
    var calendar = a[9];
    if (calendar === undefined) calendar = 'iso8601';
    if (typeof calendar !== 'string') throwType('The calendar must be a string');
    calendar = canonicalizeCalendar(calendar);
    if (!isValidISODate(y, mo, d)) throwRange('The date does not exist');
    if (!isValidTime(t[0], t[1], t[2], t[3], t[4], t[5])) throwRange('The time does not exist');
    return createPlainDateTime(dateTime(isoDate(y, mo, d), time(t[0], t[1], t[2], t[3], t[4], t[5])), calendar, new.target);
  }
  static from(item) { return toPlainDateTime(item, arguments[1]); }
  static compare(one, two) {
    var a = recordOf(toPlainDateTime(one)), b = recordOf(toPlainDateTime(two));
    return compareISODateTime(dateTime(a.date, a.time), dateTime(b.date, b.time));
  }
  get calendarId() { return plainDateTimeOf(this).calendar; }
  with(temporalDateTimeLike) {
    var r = plainDateTimeOf(this);
    if (!isPartialTemporalObject(temporalDateTimeLike)) throwType('with() needs a partial date-time object');
    var fields = isoDateToFields(r.date, 'date');
    for (var i = 0; i < 6; i++) fields[TIME_FIELDS[i]] = r.time[TIME_FIELDS[i]];
    var partial = prepareCalendarFields(r.calendar, temporalDateTimeLike, ['year', 'month', 'monthCode', 'day'], TIME_FIELDS, 'partial');
    fields = mergeFields(fields, partial);
    var overflow = getOverflowOption(getOptionsObject(arguments[1]));
    return createPlainDateTime(interpretDateTimeFields(r.calendar, fields, overflow), r.calendar);
  }
  withPlainTime() {
    var r = plainDateTimeOf(this);
    return createPlainDateTime(dateTime(r.date, toTimeRecordOrMidnight(arguments[0])), r.calendar);
  }
  withCalendar(calendarLike) {
    var r = plainDateTimeOf(this);
    return createPlainDateTime(dateTime(r.date, r.time), toCalendarIdentifier(calendarLike));
  }
  add(temporalDurationLike) { return addDurationToDateTime('add', plainDateTimeOf(this), temporalDurationLike, arguments[1]); }
  subtract(temporalDurationLike) { return addDurationToDateTime('subtract', plainDateTimeOf(this), temporalDurationLike, arguments[1]); }
  until(other) { return differenceTemporalPlainDateTime('until', plainDateTimeOf(this), other, arguments[1]); }
  since(other) { return differenceTemporalPlainDateTime('since', plainDateTimeOf(this), other, arguments[1]); }
  round(roundTo) {
    var r = plainDateTimeOf(this);
    if (roundTo === undefined) throwType('round needs an argument');
    var options = stringToOptions(roundTo, 'smallestUnit');
    var roundingIncrement = getRoundingIncrementOption(options);
    var roundingMode = getRoundingModeOption(options, 'halfExpand');
    var smallestUnit = getUnitOption(options, 'smallestUnit', true);
    validateUnit(smallestUnit, 'time', [DAY]);
    if (smallestUnit === DAY) validateRoundingIncrement(roundingIncrement, 1, true);
    else validateRoundingIncrement(roundingIncrement, MAX_INCREMENT[smallestUnit], false);
    var dt = dateTime(r.date, r.time);
    if (smallestUnit === NANOSECOND && roundingIncrement === 1) return createPlainDateTime(dt, r.calendar);
    return createPlainDateTime(roundISODateTime(dt, roundingIncrement, smallestUnit, roundingMode), r.calendar);
  }
  equals(other) {
    var r = plainDateTimeOf(this), o = recordOf(toPlainDateTime(other));
    return compareISODateTime(dateTime(r.date, r.time), dateTime(o.date, o.time)) === 0 && r.calendar === o.calendar;
  }
  toString() {
    var r = plainDateTimeOf(this);
    var options = getOptionsObject(arguments[0]);
    var showCalendar = getShowCalendarOption(options);
    var digits = getFractionalSecondDigitsOption(options);
    var roundingMode = getRoundingModeOption(options, 'trunc');
    var smallestUnit = getUnitOption(options, 'smallestUnit');
    validateUnit(smallestUnit, 'time');
    if (smallestUnit === HOUR) throwRange('smallestUnit must be minutes or smaller');
    var precision = toSecondsStringPrecision(smallestUnit, digits);
    var result = roundISODateTime(dateTime(r.date, r.time), precision.increment, precision.unit, roundingMode);
    if (!isoDateTimeWithinLimits(result.date, result.time)) throwRange('The date and time are out of range');
    return dateTimeToString(result, r.calendar, precision.precision, showCalendar);
  }
  toLocaleString() { return intlToLocaleString(plainDateTimeOf(this), this, arguments, 'any', 'all'); }
  toJSON() { var r = plainDateTimeOf(this); return dateTimeToString(dateTime(r.date, r.time), r.calendar, AUTO, 'auto'); }
  valueOf() { throwType('Use Temporal.PlainDateTime.compare to compare date-times'); }
  toZonedDateTime(temporalTimeZoneLike) {
    var r = plainDateTimeOf(this);
    var timeZone = toTimeZoneIdentifier(temporalTimeZoneLike);
    var disambiguation = getDisambiguationOption(getOptionsObject(arguments[1]));
    return createZonedDateTime(getEpochNsFor(timeZone, dateTime(r.date, r.time), disambiguation), timeZone, r.calendar);
  }
  toPlainDate() { var r = plainDateTimeOf(this); return createPlainDate(r.date, r.calendar); }
  toPlainTime() { return createPlainTime(plainDateTimeOf(this).time); }
}
PlainDateTimeProto = PlainDateTime.prototype;
copyAccessors(PlainDateTimeProto, dateGetters(function (x) { return plainDateTimeOf(x).date; }));
copyAccessors(PlainDateTimeProto, timeGetters(function (x) { return plainDateTimeOf(x).time; }));
