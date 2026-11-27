// ---------------------------------------------------------------- RFC 9557 / ISO 8601 strings (13.31-13.38)
// A small recursive-descent parser over the grammar of 13.31. Each piece takes the string and a position and returns
// the position after what it matched, or -1; what it read goes into the record `r`.
function isDigit(s, i) { var c = charCodeAt(s, i); return c >= 48 && c <= 57; }
function isAlpha(s, i) { var c = charCodeAt(s, i); return (c >= 65 && c <= 90) || (c >= 97 && c <= 122); }
function digits(s, i, n) { for (var k = 0; k < n; k++) if (!isDigit(s, i + k)) return -1; return i + n; }
function numberAt(s, i, n) { return +strSlice(s, i, i + n); }
function charAt(s, i) { return i < s.length ? s[i] : ''; }
function isSign(s, i) { var c = charAt(s, i); return c === '+' || c === '-'; }

function parseYear(s, i, r) {
  if (isSign(s, i)) {
    if (digits(s, i + 1, 6) < 0) return -1;
    var text = strSlice(s, i, i + 7);
    if (text === '-000000') return -1;
    r.year = +text;
    return i + 7;
  }
  if (digits(s, i, 4) < 0) return -1;
  r.year = numberAt(s, i, 4);
  return i + 4;
}
function parseMonth(s, i, r) {
  if (digits(s, i, 2) < 0) return -1;
  var m = numberAt(s, i, 2);
  if (m < 1 || m > 12) return -1;
  r.month = m;
  return i + 2;
}
function parseDay(s, i, r) {
  if (digits(s, i, 2) < 0) return -1;
  var d = numberAt(s, i, 2);
  if (d < 1 || d > 31) return -1;
  r.day = d;
  return i + 2;
}
function validMonthDay(m, d) { return !(d === 31 && (m === 2 || m === 4 || m === 6 || m === 9 || m === 11)) && !(m === 2 && d === 30); }
// DateSpec[?Extended]: both separators present or both absent
function parseDateSpec(s, i, r) {
  var j = parseYear(s, i, r);
  if (j < 0) return -1;
  var extended = charAt(s, j) === '-';
  if (extended) j++;
  j = parseMonth(s, j, r);
  if (j < 0) return -1;
  if (extended) { if (charAt(s, j) !== '-') return -1; j++; } else if (charAt(s, j) === '-') return -1;
  j = parseDay(s, j, r);
  if (j < 0) return -1;
  if (!validMonthDay(r.month, r.day)) return -1;
  if (r.month === 2 && r.day === 29 && !isLeapYear(r.year)) return -1;
  return j;
}
function parseYearMonthSpec(s, i, r) {
  var j = parseYear(s, i, r);
  if (j < 0) return -1;
  if (charAt(s, j) === '-') j++;
  return parseMonth(s, j, r);
}
function parseMonthDaySpec(s, i, r) {
  var j = i;
  if (charAt(s, j) === '-' && charAt(s, j + 1) === '-') j += 2;
  j = parseMonth(s, j, r);
  if (j < 0) return -1;
  if (charAt(s, j) === '-') j++;
  j = parseDay(s, j, r);
  if (j < 0 || !validMonthDay(r.month, r.day)) return -1;
  return j;
}
function parseFraction(s, i, r) {
  var c = charAt(s, i);
  if (c !== '.' && c !== ',') return i;
  var j = i + 1;
  while (j < s.length && isDigit(s, j) && j - i <= 9) j++;
  if (j === i + 1 || (j < s.length && isDigit(s, j))) return -1;
  r.fraction = strSlice(s, i + 1, j);
  return j;
}
// TimeSpec[?Extended]
function parseTimeSpec(s, i, r) {
  if (digits(s, i, 2) < 0) return -1;
  var h = numberAt(s, i, 2);
  if (h > 23) return -1;
  r.hour = h; r.minute = 0; r.second = 0; r.fraction = '';
  var j = i + 2;
  var extended = charAt(s, j) === ':';
  var k = extended ? j + 1 : j;
  if (digits(s, k, 2) < 0) return j;
  var mi = numberAt(s, k, 2);
  if (mi > 59) return extended ? -1 : j;
  r.minute = mi;
  j = k + 2;
  if (extended ? charAt(s, j) !== ':' : charAt(s, j) === ':') return j;
  k = extended ? j + 1 : j;
  if (digits(s, k, 2) < 0) return extended ? -1 : j;
  var sec = numberAt(s, k, 2);
  if (sec > 60) return -1;
  r.second = sec;
  return parseFraction(s, k + 2, r);
}
// UTCOffset[?SubMinutePrecision]: returns the end; the text is s.slice(i, end)
function parseUTCOffset(s, i, subMinute, r) {
  if (!isSign(s, i) || digits(s, i + 1, 2) < 0) return -1;
  if (numberAt(s, i + 1, 2) > 23) return -1;
  var j = i + 3, parts = 1;
  var extended = charAt(s, j) === ':';
  var k = extended ? j + 1 : j;
  if (digits(s, k, 2) >= 0 && numberAt(s, k, 2) <= 59) {
    j = k + 2; parts = 2;
    if (subMinute) {
      var ext2 = charAt(s, j) === ':';
      if (ext2 === extended) {
        var m = extended ? j + 1 : j;
        if (digits(s, m, 2) >= 0 && numberAt(s, m, 2) <= 59) {
          j = m + 2; parts = 3;
          var f = { fraction: '' };
          j = parseFraction(s, j, f);
          if (j < 0) return -1;
        } else if (extended) return -1;
      }
    }
  } else if (extended) return -1;
  if (r) { r.offset = strSlice(s, i, j); r.offsetParts = parts; }
  return j;
}
function parseTZName(s, i) {
  var j = i;
  for (;;) {
    var c = charAt(s, j);
    if (!(isAlpha(s, j) || c === '.' || c === '_')) return -1;
    j++;
    while (j < s.length) {
      c = s[j];
      if (isAlpha(s, j) || isDigit(s, j) || c === '.' || c === '_' || c === '-' || c === '+') j++;
      else break;
    }
    if (charAt(s, j) !== '/') return j;
    j++;
  }
}
// TimeZoneIdentifier: an offset to minutes, or an IANA name
function parseTimeZoneIdentifierAt(s, i) {
  var j = parseUTCOffset(s, i, false, null);
  if (j >= 0) return j;
  return parseTZName(s, i);
}
// TimeZoneAnnotation? Annotations? up to the end of the string; annotations collect into r.annotations
function parseAnnotations(s, i, r, allowTimeZone) {
  var j = i, first = true;
  r.annotations = [];
  while (charAt(s, j) === '[') {
    var k = j + 1, critical = false;
    if (charAt(s, k) === '!') { critical = true; k++; }
    var eq = -1;
    for (var m = k; m < s.length && s[m] !== ']'; m++) if (s[m] === '=') { eq = m; break; }
    if (eq < 0) {
      if (!first || !allowTimeZone) return -1;
      var end = parseTimeZoneIdentifierAt(s, k);
      if (end < 0 || charAt(s, end) !== ']') return -1;
      r.tzAnnotation = strSlice(s, k, end);
      j = end + 1;
    } else {
      // AnnotationKey = AnnotationValue
      var c = charAt(s, k);
      if (!((c >= 'a' && c <= 'z') || c === '_')) return -1;
      var p = k + 1;
      while (p < eq) { c = s[p]; if (!((c >= 'a' && c <= 'z') || c === '_' || c === '-' || isDigit(s, p))) return -1; p++; }
      var key = strSlice(s, k, eq);
      var vEnd = parseAnnotationValue(s, eq + 1);
      if (vEnd < 0 || charAt(s, vEnd) !== ']') return -1;
      arrayPush(r.annotations, { key: key, value: strSlice(s, eq + 1, vEnd), critical: critical });
      j = vEnd + 1;
    }
    first = false;
  }
  return j;
}
function parseAnnotationValue(s, i) {
  var j = i;
  for (;;) {
    var start = j;
    while (j < s.length && (isAlpha(s, j) || isDigit(s, j))) j++;
    if (j === start) return -1;
    if (charAt(s, j) !== '-') return j;
    j++;
  }
}
// DateTime[Z, TimeRequired] followed by DateTimeUTCOffset; offsetRule: 'none' (no offset), 'optional', 'required'
function parseDateTimePart(s, i, r, allowZ, timeRequired, offsetRequired) {
  var j = parseDateSpec(s, i, r);
  if (j < 0) return -1;
  var c = charAt(s, j);
  if (c !== 'T' && c !== 't' && c !== ' ') return timeRequired || offsetRequired ? -1 : j;
  j = parseTimeSpec(s, j + 1, r);
  if (j < 0) return -1;
  r.hasTime = true;
  c = charAt(s, j);
  if (c === 'Z' || c === 'z') {
    if (!allowZ) return -1;
    r.z = true;
    return j + 1;
  }
  var k = parseUTCOffset(s, j, true, r);
  if (k >= 0) return k;
  return offsetRequired ? -1 : j;
}
function newParse() { return { year: undefined, month: 1, day: 1, hour: undefined, minute: 0, second: 0, fraction: '', hasTime: false, z: false, offset: undefined, offsetParts: 0, tzAnnotation: undefined, annotations: [] }; }
// The goals of 13.31, each matching the whole string
var GOALS = {
  zonedDateTime: function (s) {
    var r = newParse(), j = parseDateTimePart(s, 0, r, true, false, false);
    if (j < 0) return null;
    j = parseAnnotations(s, j, r, true);
    return j === s.length && r.tzAnnotation !== undefined ? r : null;
  },
  dateTime: function (s) {
    var r = newParse(), j = parseDateTimePart(s, 0, r, false, false, false);
    if (j < 0) return null;
    j = parseAnnotations(s, j, r, true);
    return j === s.length ? r : null;
  },
  instant: function (s) {
    var r = newParse(), j = parseDateTimePart(s, 0, r, true, true, true);
    if (j < 0) return null;
    j = parseAnnotations(s, j, r, true);
    return j === s.length ? r : null;
  },
  time: function (s) {
    var r = newParse();
    var c = charAt(s, 0), j;
    if (c === 'T' || c === 't') {
      j = parseTimeSpec(s, 1, r);
    } else {
      j = parseTimeSpec(s, 0, r);
    }
    if (j >= 0) {
      r.hasTime = true;
      var k = parseUTCOffset(s, j, true, r);
      if (k >= 0) j = k;
      var timeEnd = j;
      if (charAt(s, j) === 'Z' || charAt(s, j) === 'z') j = -1;
      else j = parseAnnotations(s, j, r, true);
      if (j === s.length) {
        if (c !== 'T' && c !== 't') {  // 13.31.3: a time without T must not read as a month-day or year-month
          var head = strSlice(s, 0, timeEnd), t = newParse();
          if (parseMonthDaySpec(head, 0, t) === head.length) return null;
          t = newParse();
          if (parseYearMonthSpec(head, 0, t) === head.length) return null;
        }
        return r;
      }
    }
    r = newParse();
    j = parseDateTimePart(s, 0, r, false, true, false);
    if (j < 0) return null;
    j = parseAnnotations(s, j, r, true);
    return j === s.length ? r : null;
  },
  yearMonth: function (s) {
    var r = newParse(), j = parseYearMonthSpec(s, 0, r);
    if (j >= 0) {
      r.day = undefined;
      j = parseAnnotations(s, j, r, true);
      if (j === s.length) return r;
    }
    return GOALS.dateTime(s);
  },
  monthDay: function (s) {
    var r = newParse(), j = parseMonthDaySpec(s, 0, r);
    if (j >= 0) {
      j = parseAnnotations(s, j, r, true);
      if (j === s.length) { r.yearAbsent = true; return r; }
    }
    return GOALS.dateTime(s);
  }
};
// ParseISODateTime (13.35): formats is a list of GOALS keys
function parseISODateTime(s, formats) {
  for (var f = 0; f < formats.length; f++) {
    var r = GOALS[formats[f]](s);
    if (r === null) continue;
    var calendar, calendarCritical = false;
    for (var a = 0; a < r.annotations.length; a++) {
      var ann = r.annotations[a];
      if (ann.key === 'u-ca') {
        if (calendar === undefined) { calendar = ann.value; calendarCritical = ann.critical; }
        else if (ann.critical || calendarCritical) throwRange('Conflicting calendar annotations');
      } else if (ann.critical) {
        throwRange('Unknown critical annotation [' + ann.key + ']');
      }
    }
    if ((formats[f] === 'yearMonth' && r.day === undefined) || (formats[f] === 'monthDay' && r.yearAbsent)) {
      if (calendar !== undefined && toLowerCase(calendar) !== 'iso8601') throwRange('This form needs the ISO 8601 calendar');
    }
    var fr = r.fraction + '000000000';
    var t = r.hasTime ? time(r.hour, r.minute, r.second === 60 ? 59 : r.second, +strSlice(fr, 0, 3), +strSlice(fr, 3, 6), +strSlice(fr, 6, 9)) : undefined;
    return {
      year: r.yearAbsent ? undefined : r.year, month: r.month, day: r.day === undefined ? 1 : r.day, time: t,
      z: r.z, offsetString: r.offset, offsetParts: r.offsetParts, timeZoneAnnotation: r.tzAnnotation, calendar: calendar
    };
  }
  throwRange('Invalid ISO 8601 string');
}
var ALL_FORMATS = ['zonedDateTime', 'dateTime', 'instant', 'time', 'monthDay', 'yearMonth'];
// ParseDateTimeUTCOffset (14.6.11): nanoseconds, RangeError when the text is not a UTC offset
function parseUTCOffsetNs(text) {
  var r = {};
  if (parseUTCOffset(text, 0, true, r) !== text.length) throwRange(text + ' is not a UTC offset');
  var sign = text[0] === '-' ? -1 : 1;
  var hours = numberAt(text, 1, 2), rest = strSlice(text, 3), minutes = 0, seconds = 0, ns = 0;
  if (rest[0] === ':') rest = strSlice(rest, 1);
  if (rest.length >= 2) { minutes = numberAt(rest, 0, 2); rest = strSlice(rest, 2); }
  if (rest[0] === ':') rest = strSlice(rest, 1);
  if (rest.length >= 2 && isDigit(rest, 0)) { seconds = numberAt(rest, 0, 2); rest = strSlice(rest, 2); }
  if (rest.length > 1) ns = +strSlice(strSlice(rest, 1) + '000000000', 0, 9);
  return sign * (((hours * 60 + minutes) * 60 + seconds) * 1e9 + ns);
}
function isOffsetIdentifier(s) { return parseUTCOffset(s, 0, false, null) === s.length; }

// ParseTemporalDurationString (13.37)
function parseDurationString(s) {
  var i = 0, sign = 1;
  if (isSign(s, 0)) { sign = s[0] === '-' ? -1 : 1; i = 1; }
  if (charAt(s, i) !== 'P' && charAt(s, i) !== 'p') throwRange('Invalid duration string');
  i++;
  var values = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0], any = false, fractionNs = 0n, fractionUnit = -1;
  var dateOrder = 'YMWD', timeOrder = 'HMS', last = -1, inTime = false, timeAny = false;
  while (i < s.length) {
    var c = s[i];
    if (c === 'T' || c === 't') {
      if (inTime) throwRange('Invalid duration string');
      inTime = true; last = -1; i++;
      if (i >= s.length) throwRange('Invalid duration string');
      continue;
    }
    var start = i;
    while (i < s.length && isDigit(s, i)) i++;
    if (i === start) throwRange('Invalid duration string');
    var whole = strSlice(s, start, i), fraction = undefined;
    if (charAt(s, i) === '.' || charAt(s, i) === ',') {
      var fs = i + 1;
      i = fs;
      while (i < s.length && isDigit(s, i)) i++;
      if (i === fs || i - fs > 9) throwRange('Invalid duration string');
      fraction = strSlice(s, fs, i);
    }
    var d = toLowerCase(charAt(s, i)), order = inTime ? timeOrder : dateOrder, k = strIndexOf(order, toUpperCase(d));
    i++;
    if (k <= last || k < 0) throwRange('Invalid duration string');
    if (fractionUnit >= 0) throwRange('Only the last part of a duration may have a fraction');
    last = k;
    var unit = inTime ? HOUR + k : (k === 0 ? YEAR : k === 1 ? MONTH : k === 2 ? WEEK : DAY);
    if (fraction !== undefined) {
      if (!inTime) throwRange('Only hours, minutes and seconds may have a fraction');
      fractionUnit = unit;
      fractionNs = $BigInt(fraction) * UNIT_NS[unit] / (10n ** $BigInt(fraction.length));
    }
    values[unit] = toIntegerWithTruncation(whole);
    any = true;
    if (inTime) timeAny = true;
  }
  if (!any || (inTime && !timeAny)) throwRange('Invalid duration string');
  if (fractionUnit >= 0) {  // 13.37 steps 23-31: the fraction spills into the smaller units, each floored
    for (var u = fractionUnit + 1; u <= NANOSECOND; u++) {
      var q = fractionNs / UNIT_NS[u];
      values[u] = $Number(q);
      fractionNs -= q * UNIT_NS[u];
    }
  }
  for (var v = 0; v < 10; v++) values[v] = values[v] * sign + 0;
  return createDuration(values);
}
function toUpperCase(c) { return c >= 'a' && c <= 'z' ? $String.fromCharCode(charCodeAt(c, 0) - 32) : c; }
