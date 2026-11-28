// ---------------------------------------------------------------- time zones (11)
// The zone table itself (zoneTable, namedZone, icuZone) is in src/js/shared/zones.js.
// ParseTimeZoneIdentifier (11.1.16): { name } or { offsetMinutes }
function parseTimeZoneIdentifier(id) {
  var end = parseTimeZoneIdentifierAt(id, 0);
  if (end !== id.length) throwRange(id + ' is not a time zone identifier');
  if (isSign(id, 0)) return { offsetMinutes: parseUTCOffsetNs(id) / 6e10 };
  return { name: id };
}
// ParseTemporalTimeZoneString (13.38)
function parseTemporalTimeZoneString(s) {
  if (parseTimeZoneIdentifierAt(s, 0) === s.length) return parseTimeZoneIdentifier(s);
  var r = parseISODateTime(s, ALL_FORMATS);
  if (r.timeZoneAnnotation !== undefined) return parseTimeZoneIdentifier(r.timeZoneAnnotation);
  if (r.z) return { name: 'UTC' };
  if (r.offsetString !== undefined) {
    if (r.offsetParts > 2) throwRange('A time zone offset has minutes at most');
    return parseTimeZoneIdentifier(r.offsetString);
  }
  throwRange(s + ' names no time zone');
}
// ToTemporalTimeZoneIdentifier (11.1.8)
function toTimeZoneIdentifier(v) {
  if (kindOf(v) === 'ZonedDateTime') return recordOf(v).timeZone;
  if (typeof v !== 'string') throwType('A time zone must be a string');
  var parsed = parseTemporalTimeZoneString(v);
  if (parsed.offsetMinutes !== undefined) return formatOffsetMinutes(parsed.offsetMinutes);
  var zone = namedZone(parsed.name);
  if (zone === undefined) throwRange('Unknown time zone ' + parsed.name);
  return zone.id;
}
function isUTC(timeZone) { return timeZone === 'UTC' || namedZone(timeZone).primary === 'UTC'; }
// GetOffsetNanosecondsFor (11.1.9), as a Number
function getOffsetNsFor(timeZone, epochNs) {
  if (isSign(timeZone, 0)) return parseUTCOffsetNs(timeZone);
  if (isUTC(timeZone)) return 0;
  return icu.tzOffset(icuZone(timeZone), $Number(floorDiv(epochNs, 1000000n))) * 1e6;
}
function getISODateTimeFor(timeZone, epochNs) {
  var parts = isoPartsFromEpoch(epochNs), t = parts.time;
  return balanceISODateTime(parts.date.year, parts.date.month, parts.date.day, t.hour, t.minute, t.second, t.millisecond, t.microsecond,
                            t.nanosecond + getOffsetNsFor(timeZone, epochNs));
}
// GetPossibleEpochNanoseconds (11.1.13). For a named zone the candidates are the wall time less the offsets in force
// a day before and a day after it; a candidate counts if the zone really has that offset then.
function getPossibleEpochNs(timeZone, dt) {
  var t = dt.time, list;
  if (isSign(timeZone, 0) || isUTC(timeZone)) {
    var offsetMinutes = isSign(timeZone, 0) ? parseUTCOffsetNs(timeZone) / 6e10 : 0;
    var balanced = balanceISODateTime(dt.date.year, dt.date.month, dt.date.day, t.hour, t.minute - offsetMinutes, t.second, t.millisecond,
                                      t.microsecond, t.nanosecond);
    checkISODaysRange(balanced.date);
    list = [utcEpochNs(balanced.date, balanced.time)];
  } else {
    checkISODaysRange(dt.date);
    var wall = utcEpochNs(dt.date, dt.time);
    var before = getOffsetNsFor(timeZone, wall - nsPerDay), after = getOffsetNsFor(timeZone, wall + nsPerDay);
    var offsets = before === after ? [before] : [before, after];
    list = [];
    for (var i = 0; i < offsets.length; i++) {
      var candidate = wall - $BigInt(offsets[i]);
      if (getOffsetNsFor(timeZone, candidate) === offsets[i]) arrayPush(list, candidate);
    }
    if (list.length === 2 && list[0] > list[1]) list = [list[1], list[0]];
  }
  for (var j = 0; j < list.length; j++) if (!isValidEpochNs(list[j])) throwRange('The date and time are out of range');
  return list;
}
// DisambiguatePossibleEpochNanoseconds (11.1.12)
function disambiguate(possible, timeZone, dt, disambiguation) {
  var n = possible.length;
  if (n === 1) return possible[0];
  if (n !== 0) {
    if (disambiguation === 'earlier' || disambiguation === 'compatible') return possible[0];
    if (disambiguation === 'later') return possible[n - 1];
    throwRange('The time is ambiguous in ' + timeZone);
  }
  if (disambiguation === 'reject') throwRange('The time does not exist in ' + timeZone);
  var wall = utcEpochNs(dt.date, dt.time);
  var nanoseconds = getOffsetNsFor(timeZone, wall + nsPerDay) - getOffsetNsFor(timeZone, wall - nsPerDay);
  var shifted = addTime(dt.time, $BigInt(disambiguation === 'earlier' ? -nanoseconds : nanoseconds));
  var moved = getPossibleEpochNs(timeZone, dateTime(addDaysToISODate(dt.date, shifted.days), shifted));
  return disambiguation === 'earlier' ? moved[0] : moved[moved.length - 1];
}
function getEpochNsFor(timeZone, dt, disambiguation) { return disambiguate(getPossibleEpochNs(timeZone, dt), timeZone, dt, disambiguation); }
// GetStartOfDay (11.1.14): when midnight is skipped, the day starts at the transition that skips it
function getStartOfDay(timeZone, date) {
  var possible = getPossibleEpochNs(timeZone, dateTime(date, MIDNIGHT));
  if (possible.length !== 0) return possible[0];
  return getNamedTimeZoneTransition(timeZone, utcEpochNs(date, MIDNIGHT) - nsPerDay, true);
}
// GetNamedTimeZoneNextTransition and GetNamedTimeZonePreviousTransition (11.1.3, 11.1.4). ICU also reports changes
// of a zone's abbreviation or of its split between standard and daylight time, which are not transitions here.
function getNamedTimeZoneTransition(timeZone, epochNs, next) {
  if (isUTC(timeZone)) return null;
  var zone = icuZone(timeZone);
  var ms = next ? $Number(floorDiv(epochNs, 1000000n)) : -$Number(floorDiv(-epochNs, 1000000n));
  for (var guard = 0; guard < 1000; guard++) {
    var at = icu.tzTransition(zone, ms, next);
    if (at === null) return null;
    var ns = $BigInt(at) * 1000000n;
    if (next ? ns > nsMaxInstant : ns < nsMinInstant) return null;
    if (icu.tzOffset(zone, at) !== icu.tzOffset(zone, at - 1)) return ns;
    ms = at;
  }
  return null;
}
function timeZoneEquals(a, b) {
  if (a === b) return true;
  if (!isOffsetIdentifier(a) && !isOffsetIdentifier(b)) return namedZone(a).primary === namedZone(b).primary;
  return false;
}
// SystemTimeZoneIdentifier (14.6.2)
function systemTimeZone() {
  var id = icu.defaultTimeZone();
  var zone = id === undefined ? undefined : namedZone(id);
  return zone === undefined ? 'UTC' : zone.primary;
}

// ---------------------------------------------------------------- calendars (12)
function canonicalizeCalendar(id) {
  if (toLowerCase(id) !== 'iso8601') throwRange('Unknown calendar ' + id);
  return 'iso8601';
}
// ParseTemporalCalendarString (13.36)
function parseCalendarString(s) {
  var parsed;
  try { parsed = parseISODateTime(s, ALL_FORMATS); } catch (e) { parsed = undefined; }
  if (parsed !== undefined) return parsed.calendar === undefined ? 'iso8601' : parsed.calendar;
  if (parseAnnotationValue(s, 0) !== s.length) throwRange(s + ' is not a calendar');
  return s;
}
function toCalendarIdentifier(v) {
  var k = kindOf(v);
  if (k === 'PlainDate' || k === 'PlainDateTime' || k === 'PlainMonthDay' || k === 'PlainYearMonth' || k === 'ZonedDateTime') return recordOf(v).calendar;
  if (typeof v !== 'string') throwType('A calendar must be a string');
  return canonicalizeCalendar(parseCalendarString(v));
}
// GetTemporalCalendarIdentifierWithISODefault (12.3.11)
function calendarWithISODefault(item) {
  var k = kindOf(item);
  if (k === 'PlainDate' || k === 'PlainDateTime' || k === 'PlainMonthDay' || k === 'PlainYearMonth' || k === 'ZonedDateTime') return recordOf(item).calendar;
  var calendarLike = item.calendar;
  if (calendarLike === undefined) return 'iso8601';
  return toCalendarIdentifier(calendarLike);
}
function monthCode(month) { return 'M' + pad(month, 2); }
// ParseMonthCode (12.2.1)
function parseMonthCode(v) {
  var code = toPrimitiveString(v);
  if (typeof code !== 'string') throwType('A month code must be a string');
  var ok = (code.length === 3 || (code.length === 4 && code[3] === 'L')) && code[0] === 'M' && isDigit(code, 1) && isDigit(code, 2);
  if (ok && code[1] === '0' && code[2] === '0' && code.length !== 4) ok = false;
  if (!ok) throwRange(code + ' is not a month code');
  return { month: +strSlice(code, 1, 3), leap: code.length === 4 };
}

// Calendar Fields Records (12.3.2): plain objects whose absent fields are undefined (UNSET)
var FIELD_KEYS = { era: 'era', eraYear: 'eraYear', year: 'year', month: 'month', monthCode: 'monthCode', day: 'day', hour: 'hour', minute: 'minute',
                   second: 'second', millisecond: 'millisecond', microsecond: 'microsecond', nanosecond: 'nanosecond', offset: 'offset', timeZone: 'timeZone' };
var FIELD_ORDER = ['era', 'eraYear', 'year', 'month', 'monthCode', 'day', 'hour', 'minute', 'second', 'millisecond', 'microsecond', 'nanosecond', 'offset', 'timeZone'];
var TIME_FIELDS = ['hour', 'minute', 'second', 'millisecond', 'microsecond', 'nanosecond'];
function convertField(name, value) {
  switch (name) {
    case 'month': case 'day': return toPositiveIntegerWithTruncation(value);
    case 'monthCode': var p = parseMonthCode(value); return monthCode(p.month) + (p.leap ? 'L' : '');
    case 'era': return toString(value);
    case 'offset':
      var offset = toPrimitiveString(value);
      if (typeof offset !== 'string') throwType('An offset must be a string');
      parseUTCOffsetNs(offset);
      return offset;
    case 'timeZone': return toTimeZoneIdentifier(value);
    default: return toIntegerWithTruncation(value);
  }
}
// PrepareCalendarFields (12.3.3); required is 'partial' or a list of field names
function prepareCalendarFields(calendar, fields, calendarNames, otherNames, required) {
  var names = [], i;
  for (i = 0; i < calendarNames.length; i++) arrayPush(names, calendarNames[i]);
  for (i = 0; i < otherNames.length; i++) arrayPush(names, otherNames[i]);
  arraySort(names);
  var result = {}, any = false;
  for (i = 0; i < names.length; i++) {
    var name = names[i], value = fields[name];
    if (value !== undefined) {
      any = true;
      result[name] = convertField(name, value);
    } else if (required !== 'partial') {
      if (arrayIndexOf(required, name) >= 0) throwType(name + ' is required');
      if (arrayIndexOf(TIME_FIELDS, name) >= 0) result[name] = 0;
    }
  }
  if (required === 'partial' && !any) throwType('At least one field is required');
  return result;
}
// ISODateToFields (13.42); type is 'date', 'year-month' or 'month-day'
function isoDateToFields(date, type) {
  var f = { monthCode: monthCode(date.month) };
  if (type !== 'year-month') f.day = date.day;
  if (type !== 'month-day') f.year = date.year;
  return f;
}
// CalendarMergeFields (12.3.5) for the ISO calendar: a month or month code given replaces both
function mergeFields(fields, additional) {
  var merged = {};
  for (var i = 0; i < FIELD_ORDER.length; i++) {
    var key = FIELD_ORDER[i];
    var ignored = additional[key] !== undefined || (key === 'month' && additional.monthCode !== undefined) || (key === 'monthCode' && additional.month !== undefined);
    if (fields[key] !== undefined && !ignored) merged[key] = fields[key];
    if (additional[key] !== undefined) merged[key] = additional[key];
  }
  return merged;
}
// CalendarResolveFields (12.3.31) for the ISO calendar; type is 'date', 'year-month' or 'month-day'
function resolveFields(fields, type) {
  if (type !== 'month-day' && fields.year === undefined) throwType('year is required');
  if (type !== 'year-month' && fields.day === undefined) throwType('day is required');
  if (fields.month === undefined && fields.monthCode === undefined) throwType('month or monthCode is required');
  if (fields.monthCode !== undefined) {
    var parsed = parseMonthCode(fields.monthCode);
    if (parsed.leap) throwRange('The ISO 8601 calendar has no leap months');
    if (parsed.month > 12) throwRange(fields.monthCode + ' is not a month of the ISO 8601 calendar');
    if (fields.month !== undefined && fields.month !== parsed.month) throwRange('month and monthCode disagree');
    fields.month = parsed.month;
  }
}
function calendarDateFromFields(calendar, fields, overflow) {
  resolveFields(fields, 'date');
  var result = regulateISODate(fields.year, fields.month, fields.day, overflow);
  if (!isoDateWithinLimits(result)) throwRange('The date is out of range');
  return result;
}
function calendarYearMonthFromFields(calendar, fields, overflow) {
  fields.day = 1;
  resolveFields(fields, 'year-month');
  var result = regulateISODate(fields.year, fields.month, fields.day, overflow);
  if (!isoYearMonthWithinLimits(result)) throwRange('The month is out of range');
  return result;
}
function calendarMonthDayFromFields(calendar, fields, overflow) {
  resolveFields(fields, 'month-day');
  var result = regulateISODate(fields.year === undefined ? 1972 : fields.year, fields.month, fields.day, overflow);
  return isoDate(1972, result.month, result.day);
}
// CalendarDateAdd (12.3.7)
function calendarDateAdd(calendar, date, d, overflow) {
  var ym = balanceISOYearMonth(date.year + d.years, date.month + d.months);
  var result = addDaysToISODate(regulateISODate(ym.year, ym.month, date.day, overflow), d.days + 7 * d.weeks);
  if (!isoDateWithinLimits(result)) throwRange('The date is out of range');
  return result;
}
// ISODateSurpasses (3.5.6) and CompareSurpasses (3.5.5)
function surpasses(sign, year, month, day, target) {
  if (year !== target.year) return sign * (year - target.year) > 0;
  if (month !== target.month) return sign * (month - target.month) > 0;
  if (day !== target.day) return sign * (day - target.day) > 0;
  return false;
}
function isoDateSurpasses(sign, base, other, years, months, weeks, days) {
  var y0 = base.year + years;
  if (surpasses(sign, y0, base.month, base.day, other)) return true;
  if (months === 0) return false;
  var ym = balanceISOYearMonth(y0, base.month + months);
  if (surpasses(sign, ym.year, ym.month, base.day, other)) return true;
  if (weeks === 0 && days === 0) return false;
  var d = addDaysToISODate(regulateISODate(ym.year, ym.month, base.day, 'constrain'), 7 * weeks + days);
  return surpasses(sign, d.year, d.month, d.day, other);
}
// CalendarDateUntil (12.3.9). The loops of the proposal are run here with a first guess, then corrected by single steps.
function calendarDateUntil(calendar, one, two, largestUnit) {
  var sign = -compareISODate(one, two);
  if (sign === 0) return dateDuration(0, 0, 0, 0);
  var years = 0, months = 0, weeks = 0, days;
  if (largestUnit === YEAR) {
    years = two.year - one.year;
    if (years !== 0 && isoDateSurpasses(sign, one, two, years, 0, 0, 0)) years -= sign;
    while (!isoDateSurpasses(sign, one, two, years + sign, 0, 0, 0)) years += sign;
  }
  if (largestUnit === YEAR || largestUnit === MONTH) {
    months = (two.year - one.year - years) * 12 + two.month - one.month;
    if (months !== 0 && isoDateSurpasses(sign, one, two, years, months, 0, 0)) months -= sign;
    while (months !== 0 && isoDateSurpasses(sign, one, two, years, months, 0, 0)) months -= sign;
    while (!isoDateSurpasses(sign, one, two, years, months + sign, 0, 0)) months += sign;
  }
  var ym = balanceISOYearMonth(one.year + years, one.month + months);
  var start = regulateISODate(ym.year, ym.month, one.day, 'constrain');
  var total = epochDays(two.year, two.month, two.day) - epochDays(start.year, start.month, start.day);
  if (largestUnit === WEEK) { weeks = trunc(total / 7); days = total - weeks * 7; } else days = total;
  return dateDuration(years, months, weeks + 0, days + 0);
}
function calendarISOToDate(date) { return date; }
