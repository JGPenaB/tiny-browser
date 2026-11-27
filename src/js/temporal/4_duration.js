// ---------------------------------------------------------------- durations (7.5)
// Date Duration Records { years, months, weeks, days }; Internal Duration Records { date, time } where time is a
// BigInt number of nanoseconds (a "time duration").
function dateDuration(years, months, weeks, days) { return { years: years, months: months, weeks: weeks, days: days }; }
var ZERO_DATE_DURATION = dateDuration(0, 0, 0, 0);
function createDateDuration(years, months, weeks, days) {
  if (!isValidDuration([years, months, weeks, days, 0, 0, 0, 0, 0, 0])) throwRange('The duration is out of range');
  return dateDuration(years, months, weeks, days);
}
function adjustDateDuration(d, days, weeks, months) {
  return createDateDuration(d.years, months === undefined ? d.months : months, weeks === undefined ? d.weeks : weeks, days);
}
function dateDurationSign(d) {
  var v = [d.years, d.months, d.weeks, d.days];
  for (var i = 0; i < 4; i++) { if (v[i] < 0) return -1; if (v[i] > 0) return 1; }
  return 0;
}
function internalDuration(date, t) { return { date: date, time: t }; }
function internalDurationSign(d) { var s = dateDurationSign(d.date); return s !== 0 ? s : bigSign(d.time); }
function timeDurationFromComponents(h, mi, s, ms, us, ns) {
  return (((($BigInt(h) * 60n + $BigInt(mi)) * 60n + $BigInt(s)) * 1000n + $BigInt(ms)) * 1000n + $BigInt(us)) * 1000n + $BigInt(ns);
}
function addTimeDuration(a, b) { var r = a + b; if (bigAbs(r) > maxTimeDuration) throwRange('The duration is out of range'); return r; }
function add24HourDays(d, days) { var r = d + $BigInt(days) * nsPerDay; if (bigAbs(r) > maxTimeDuration) throwRange('The duration is out of range'); return r; }
function roundTimeDurationToIncrement(d, increment, mode) {
  var r = roundBigToIncrement(d, increment, mode);
  if (bigAbs(r) > maxTimeDuration) throwRange('The duration is out of range');
  return r;
}
function roundTimeDuration(d, increment, unit, mode) { return roundTimeDurationToIncrement(d, UNIT_NS[unit] * $BigInt(increment), mode); }
function totalTimeDuration(d, unit) { return divideToNumber(d, UNIT_NS[unit]); }

// IsValidDuration (7.5.16); v is the list of the ten fields
function isValidDuration(v) {
  var sign = 0;
  for (var i = 0; i < 10; i++) {
    if (v[i] < 0) { if (sign > 0) return false; sign = -1; } else if (v[i] > 0) { if (sign < 0) return false; sign = 1; }
  }
  if (abs(v[0]) >= 4294967296 || abs(v[1]) >= 4294967296 || abs(v[2]) >= 4294967296) return false;
  var ns = $BigInt(v[3]) * nsPerDay + timeDurationFromComponents(v[4], v[5], v[6], v[7], v[8], v[9]);
  return bigAbs(ns) < 9007199254740992000000000n;
}
var DurationProto;
// CreateTemporalDuration (7.5.19): v is the list of the ten fields
function createDuration(v, newTarget) {
  if (!isValidDuration(v)) throwRange('The duration is out of range');
  var o = create(newTarget === undefined ? DurationProto : protoFor(newTarget, DurationProto));
  wmSet(slots, o, { kind: 'Duration', v: [v[0] + 0, v[1] + 0, v[2] + 0, v[3] + 0, v[4] + 0, v[5] + 0, v[6] + 0, v[7] + 0, v[8] + 0, v[9] + 0] });
  return o;
}
function durationFields(d) { return recordOf(d).v; }
function negatedDuration(v) { var n = []; for (var i = 0; i < 10; i++) n[i] = -v[i] + 0; return n; }
function durationSign(v) { for (var i = 0; i < 10; i++) { if (v[i] < 0) return -1; if (v[i] > 0) return 1; } return 0; }
function defaultLargestUnit(v) { for (var i = 0; i < 9; i++) if (v[i] !== 0) return i; return NANOSECOND; }
function toInternalDuration(v) {
  return internalDuration(createDateDuration(v[0], v[1], v[2], v[3]), timeDurationFromComponents(v[4], v[5], v[6], v[7], v[8], v[9]));
}
function toInternalDurationWith24HourDays(v) {
  var t = add24HourDays(timeDurationFromComponents(v[4], v[5], v[6], v[7], v[8], v[9]), v[3]);
  return internalDuration(createDateDuration(v[0], v[1], v[2], 0), t);
}
function toDateDurationWithoutTime(v) {
  var d = toInternalDurationWith24HourDays(v);
  return createDateDuration(d.date.years, d.date.months, d.date.weeks, $Number(d.time / nsPerDay));
}
// TemporalDurationFromInternal (7.5.8): the list of ten fields, with the time part balanced up to largestUnit
function durationFromInternal(d, largestUnit) {
  var sign = bigSign(d.time), ns = bigAbs(d.time), days = 0n, hours = 0n, minutes = 0n, seconds = 0n, ms = 0n, us = 0n;
  if (largestUnit <= MICROSECOND) { us = ns / 1000n; ns %= 1000n; }
  if (largestUnit <= MILLISECOND) { ms = us / 1000n; us %= 1000n; }
  if (largestUnit <= SECOND) { seconds = ms / 1000n; ms %= 1000n; }
  if (largestUnit <= MINUTE) { minutes = seconds / 60n; seconds %= 60n; }
  if (largestUnit <= HOUR) { hours = minutes / 60n; minutes %= 60n; }
  if (largestUnit <= DAY) { days = hours / 24n; hours %= 24n; }
  return [d.date.years, d.date.months, d.date.weeks, d.date.days + $Number(days) * sign, $Number(hours) * sign, $Number(minutes) * sign,
          $Number(seconds) * sign, $Number(ms) * sign, $Number(us) * sign, $Number(ns) * sign];
}
// ToTemporalPartialDurationRecord (7.5.18): the list of ten fields, undefined where absent
function toPartialDuration(like) {
  if (!isObject(like)) throwType('A duration-like object is required');
  var order = ['days', 'hours', 'microseconds', 'milliseconds', 'minutes', 'months', 'nanoseconds', 'seconds', 'weeks', 'years'];
  var index = [DAY, HOUR, MICROSECOND, MILLISECOND, MINUTE, MONTH, NANOSECOND, SECOND, WEEK, YEAR];
  var result = [undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined], any = false;
  for (var i = 0; i < 10; i++) {
    var value = like[order[i]];
    if (value !== undefined) { result[index[i]] = toIntegerIfIntegral(value); any = true; }
  }
  if (!any) throwType('A duration-like object needs at least one unit');
  return result;
}
// ToTemporalDuration (7.5.12): the list of ten fields
function toDurationFields(item) {
  if (kindOf(item) === 'Duration') return durationFields(item);
  if (!isObject(item)) {
    if (typeof item !== 'string') throwType('A duration must be an object or a string');
    return durationFields(parseDurationString(item));
  }
  var partial = toPartialDuration(item), v = [];
  for (var i = 0; i < 10; i++) v[i] = partial[i] === undefined ? 0 : partial[i];
  if (!isValidDuration(v)) throwRange('The duration is out of range');
  return v;
}
function toDuration(item) { return createDuration(toDurationFields(item)); }
// TemporalDurationToString (7.5.40)
function durationToString(v, precision) {
  var sign = durationSign(v), date = '', t = '';
  if (v[0] !== 0) date += abs(v[0]) + 'Y';
  if (v[1] !== 0) date += abs(v[1]) + 'M';
  if (v[2] !== 0) date += abs(v[2]) + 'W';
  if (v[3] !== 0) date += abs(v[3]) + 'D';
  if (v[4] !== 0) t += abs(v[4]) + 'H';
  if (v[5] !== 0) t += abs(v[5]) + 'M';
  var zeroMinutesAndHigher = defaultLargestUnit(v) >= SECOND;
  var seconds = timeDurationFromComponents(0, 0, v[6], v[7], v[8], v[9]);
  if (seconds !== 0n || zeroMinutesAndHigher || precision !== AUTO) {
    var a = bigAbs(seconds);
    t += bigToString(a / 1000000000n) + formatFractionalSeconds($Number(a % 1000000000n), precision) + 'S';
  }
  var result = (sign < 0 ? '-' : '') + 'P' + date;
  if (t !== '') result += 'T' + t;
  return result;
}

// ---------------------------------------------------------------- relative rounding (7.5.32-7.5.39)
// The epoch nanoseconds of a date-time, in a time zone or (timeZone undefined) as if in UTC
function epochNsOf(dt, timeZone) { return timeZone === undefined ? utcEpochNs(dt.date, dt.time) : getEpochNsFor(timeZone, dt, 'compatible'); }
function computeNudgeWindow(sign, d, originNs, dt, timeZone, calendar, increment, unit, additionalShift) {
  var r1, r2, startDate, endDate;
  if (unit === YEAR) {
    var years = roundNumberToIncrement(d.date.years, increment, 'trunc');
    r1 = additionalShift ? years + increment * sign : years;
    r2 = r1 + increment * sign;
    startDate = createDateDuration(r1, 0, 0, 0);
    endDate = createDateDuration(r2, 0, 0, 0);
  } else if (unit === MONTH) {
    var months = roundNumberToIncrement(d.date.months, increment, 'trunc');
    r1 = additionalShift ? months + increment * sign : months;
    r2 = r1 + increment * sign;
    startDate = adjustDateDuration(d.date, 0, 0, r1);
    endDate = adjustDateDuration(d.date, 0, 0, r2);
  } else if (unit === WEEK) {
    var weeksStart = calendarDateAdd(calendar, dt.date, adjustDateDuration(d.date, 0, 0), 'constrain');
    var weeksEnd = addDaysToISODate(weeksStart, d.date.days);
    var until = calendarDateUntil(calendar, weeksStart, weeksEnd, WEEK);
    var weeks = roundNumberToIncrement(d.date.weeks + until.weeks, increment, 'trunc');
    r1 = weeks;
    r2 = weeks + increment * sign;
    startDate = adjustDateDuration(d.date, 0, r1);
    endDate = adjustDateDuration(d.date, 0, r2);
  } else {
    var days = roundNumberToIncrement(d.date.days, increment, 'trunc');
    r1 = days;
    r2 = days + increment * sign;
    startDate = adjustDateDuration(d.date, r1);
    endDate = adjustDateDuration(d.date, r2);
  }
  // The proposal takes originNs whenever r1 = 0, but that is the start only when no larger unit moves it: test262
  // (exact-multiple-of-larger-unit) rounds P1M to weeks from the end of the month, so it is used for an empty start.
  var startNs = dateDurationSign(startDate) === 0 ? originNs : epochNsOf(dateTime(calendarDateAdd(calendar, dt.date, startDate, 'constrain'), dt.time), timeZone);
  var endNs = epochNsOf(dateTime(calendarDateAdd(calendar, dt.date, endDate, 'constrain'), dt.time), timeZone);
  return { r1: r1, r2: r2, startNs: startNs, endNs: endNs, start: internalDuration(startDate, 0n), end: internalDuration(endDate, 0n) };
}
function nudgeToCalendarUnit(sign, d, originNs, destNs, dt, timeZone, calendar, increment, unit, mode) {
  var expanded = false;
  var w = computeNudgeWindow(sign, d, originNs, dt, timeZone, calendar, increment, unit, false);
  if (sign === 1 ? !(w.startNs <= destNs && destNs <= w.endNs) : !(w.endNs <= destNs && destNs <= w.startNs)) {
    w = computeNudgeWindow(sign, d, originNs, dt, timeZone, calendar, increment, unit, true);
    expanded = true;
  }
  var num = destNs - w.startNs, den = w.endNs - w.startNs;
  if (den < 0n) { num = -num; den = -den; }
  // total = r1 + progress × increment × sign, computed as one exact quotient
  var total = divideToNumber($BigInt(w.r1) * den + num * $BigInt(increment * sign), den);
  var roundUp;
  if (num === den) roundUp = true;
  else roundUp = chooseRounded(num, den, (abs(w.r1) / increment) % 2 === 0, unsignedRoundingMode(mode, sign < 0));
  var result;
  if (roundUp) { expanded = true; result = { duration: w.end, nudgedNs: w.endNs, expanded: expanded }; }
  else result = { duration: w.start, nudgedNs: w.startNs, expanded: expanded };
  return { nudge: result, total: total };
}
function nudgeToZonedTime(sign, d, dt, timeZone, calendar, increment, unit, mode) {
  var start = calendarDateAdd(calendar, dt.date, d.date, 'constrain');
  var startNs = getEpochNsFor(timeZone, dateTime(start, dt.time), 'compatible');
  var endNs = getEpochNsFor(timeZone, dateTime(addDaysToISODate(start, sign), dt.time), 'compatible');
  var daySpan = endNs - startNs;
  var unitLength = UNIT_NS[unit] * $BigInt(increment);
  var rounded = roundTimeDurationToIncrement(d.time, unitLength, mode);
  var beyond = addTimeDuration(rounded, -daySpan), dayDelta = 0, nudged, didRoundBeyondDay = false;
  if (bigSign(beyond) !== -sign) {
    didRoundBeyondDay = true;
    dayDelta = sign;
    rounded = roundTimeDurationToIncrement(beyond, unitLength, mode);
    nudged = rounded + endNs;
  } else {
    nudged = rounded + startNs;
  }
  var date = adjustDateDuration(d.date, d.date.days + dayDelta);
  return { duration: internalDuration(date, rounded), nudgedNs: nudged, expanded: didRoundBeyondDay };
}
function nudgeToDayOrTime(d, destNs, largestUnit, increment, smallestUnit, mode) {
  var t = add24HourDays(d.time, d.date.days);
  var rounded = roundTimeDurationToIncrement(t, UNIT_NS[smallestUnit] * $BigInt(increment), mode);
  var diff = addTimeDuration(rounded, -t);
  var wholeDays = $Number(t / nsPerDay), roundedWholeDays = $Number(rounded / nsPerDay);
  var dayDelta = roundedWholeDays - wholeDays;
  var dayDeltaSign = dayDelta < 0 ? -1 : dayDelta > 0 ? 1 : 0;
  var expanded = dayDeltaSign === bigSign(t);
  var nudged = diff + destNs, days = 0, remainder = rounded;
  if (largestUnit <= DAY) { days = roundedWholeDays; remainder = rounded - $BigInt(roundedWholeDays) * nsPerDay; }
  return { duration: internalDuration(adjustDateDuration(d.date, days), remainder), nudgedNs: nudged, expanded: expanded };
}
function bubbleRelativeDuration(sign, d, nudgedNs, dt, timeZone, calendar, largestUnit, smallestUnit) {
  if (smallestUnit === largestUnit) return d;
  for (var unit = smallestUnit - 1; unit >= largestUnit; unit--) {
    if (unit === WEEK && largestUnit !== WEEK) continue;
    var end;
    if (unit === YEAR) end = createDateDuration(d.date.years + sign, 0, 0, 0);
    else if (unit === MONTH) end = adjustDateDuration(d.date, 0, 0, d.date.months + sign);
    else end = adjustDateDuration(d.date, 0, d.date.weeks + sign);
    var endNs = epochNsOf(dateTime(calendarDateAdd(calendar, dt.date, end, 'constrain'), dt.time), timeZone);
    if (bigSign(nudgedNs - endNs) !== -sign) d = internalDuration(end, 0n);
    else break;
  }
  return d;
}
function roundRelativeDuration(d, originNs, destNs, dt, timeZone, calendar, largestUnit, increment, smallestUnit, mode) {
  var irregular = isCalendarUnit(smallestUnit) || (timeZone !== undefined && smallestUnit === DAY);
  var sign = internalDurationSign(d) < 0 ? -1 : 1, nudge;
  if (irregular) nudge = nudgeToCalendarUnit(sign, d, originNs, destNs, dt, timeZone, calendar, increment, smallestUnit, mode).nudge;
  else if (timeZone !== undefined) nudge = nudgeToZonedTime(sign, d, dt, timeZone, calendar, increment, smallestUnit, mode);
  else nudge = nudgeToDayOrTime(d, destNs, largestUnit, increment, smallestUnit, mode);
  d = nudge.duration;
  if (nudge.expanded && smallestUnit !== WEEK) {
    d = bubbleRelativeDuration(sign, d, nudge.nudgedNs, dt, timeZone, calendar, largestUnit, largerUnit(smallestUnit, DAY));
  }
  return d;
}
function totalRelativeDuration(d, originNs, destNs, dt, timeZone, calendar, unit) {
  if (isCalendarUnit(unit) || (timeZone !== undefined && unit === DAY)) {
    var sign = internalDurationSign(d) < 0 ? -1 : 1;
    return nudgeToCalendarUnit(sign, d, originNs, destNs, dt, timeZone, calendar, 1, unit, 'trunc').total;
  }
  return totalTimeDuration(add24HourDays(d.time, d.date.days), unit);
}
// DateDurationDays (7.5.29)
function dateDurationDays(d, plainRelativeTo) {
  var ymw = adjustDateDuration(d, 0);
  if (dateDurationSign(ymw) === 0) return d.days;
  var r = recordOf(plainRelativeTo);
  var later = calendarDateAdd(r.calendar, r.date, ymw, 'constrain');
  return d.days + epochDays(later.year, later.month, later.day) - epochDays(r.date.year, r.date.month, r.date.day);
}
