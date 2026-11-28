// ---------------------------------------------------------------- calendars other than ISO 8601
// As the Intl era and month code proposal specifies them (https://tc39.es/proposal-intl-era-monthcode/, section 4).
// ICU gives the structure of each calendar year (icu.calendarYear: its months, with their month codes, first days
// and lengths); everything else is computed from that, as the proposal's algorithms say.
var CALENDARS = ['buddhist', 'chinese', 'coptic', 'dangi', 'ethioaa', 'ethiopic', 'ethiopic-amete-alem', 'gregory', 'hebrew', 'indian',
                 'islamic-civil', 'islamic-tbla', 'islamic-umalqura', 'islamicc', 'iso8601', 'japanese', 'persian', 'roc'];
// The calendars whose months are those of ISO 8601, with the offset of their arithmetic years (Table 4)
var ISO_MONTH_CALENDARS = create(null);
ISO_MONTH_CALENDARS.gregory = 0; ISO_MONTH_CALENDARS.japanese = 0; ISO_MONTH_CALENDARS.buddhist = 543; ISO_MONTH_CALENDARS.roc = -1911;
// Table 2: [calendar, era, aliases, minimum era year, maximum era year, kind, offset]
var ERAS = [
  ['buddhist', 'be', [], -Infinity, Infinity, 'epoch'], ['coptic', 'am', [], -Infinity, Infinity, 'epoch'],
  ['ethioaa', 'aa', [], -Infinity, Infinity, 'epoch'], ['ethiopic', 'am', [], 1, Infinity, 'epoch'], ['ethiopic', 'aa', [], -Infinity, 5500, 'offset', -5499],
  ['gregory', 'ce', ['ad'], 1, Infinity, 'epoch'], ['gregory', 'bce', ['bc'], 1, Infinity, 'negative'], ['hebrew', 'am', [], -Infinity, Infinity, 'epoch'],
  ['indian', 'shaka', [], -Infinity, Infinity, 'epoch'], ['islamic-civil', 'ah', [], 1, Infinity, 'epoch'], ['islamic-civil', 'bh', [], 1, Infinity, 'negative'],
  ['islamic-tbla', 'ah', [], 1, Infinity, 'epoch'], ['islamic-tbla', 'bh', [], 1, Infinity, 'negative'],
  ['islamic-umalqura', 'ah', [], 1, Infinity, 'epoch'], ['islamic-umalqura', 'bh', [], 1, Infinity, 'negative'],
  ['japanese', 'reiwa', [], 1, Infinity, 'offset', 2019], ['japanese', 'heisei', [], 1, 31, 'offset', 1989], ['japanese', 'showa', [], 1, 64, 'offset', 1926],
  ['japanese', 'taisho', [], 1, 15, 'offset', 1912], ['japanese', 'meiji', [], 6, 45, 'offset', 1868], ['japanese', 'ce', ['ad'], 1, 1872, 'epoch'],
  ['japanese', 'bce', ['bc'], 1, Infinity, 'negative'], ['persian', 'ap', [], -Infinity, Infinity, 'epoch'], ['roc', 'roc', [], 1, Infinity, 'epoch'],
  ['roc', 'broc', [], 1, Infinity, 'negative']];
// The first days of the Japanese eras since Meiji (the Meiji era counts only from 1873, as the proposal says)
var JAPANESE_ERAS = [['reiwa', 2019, 5, 1], ['heisei', 1989, 1, 8], ['showa', 1926, 12, 25], ['taisho', 1912, 7, 30], ['meiji', 1873, 1, 1]];
function calendarSupportsEra(calendar) {
  for (var i = 0; i < ERAS.length; i++) if (ERAS[i][0] === calendar) return true;
  return false;
}
// CanonicalizeEraInCalendar (4.1.2)
function canonicalizeEraInCalendar(calendar, era) {
  for (var i = 0; i < ERAS.length; i++) {
    if (ERAS[i][0] !== calendar) continue;
    if (ERAS[i][1] === era || arrayIndexOf(ERAS[i][2], era) >= 0) return ERAS[i][1];
  }
  return undefined;
}
// CalendarDateArithmeticYearForEraYear (4.1.12)
function arithmeticYearForEraYear(calendar, era, eraYear) {
  for (var i = 0; i < ERAS.length; i++) {
    var row = ERAS[i];
    if (row[0] !== calendar || row[1] !== era) continue;
    if (row[5] === 'epoch') return eraYear;
    if (row[5] === 'negative') return 1 - eraYear;
    return row[6] + eraYear - 1;
  }
  return eraYear;
}
// The era and era year of a date (4.1.9, 4.1.10)
function eraOf(calendar, date, year) {
  if (calendar === 'japanese') {
    for (var i = 0; i < JAPANESE_ERAS.length; i++) {
      var e = JAPANESE_ERAS[i];
      if (compareISODate(date, isoDate(e[1], e[2], e[3])) >= 0) {
        var offset = e[0] === 'meiji' ? 1868 : e[1];
        return [e[0], date.year - offset + 1];
      }
    }
    return date.year >= 1 ? ['ce', date.year] : ['bce', 1 - date.year];
  }
  if (calendar === 'gregory') return year >= 1 ? ['ce', year] : ['bce', 1 - year];
  if (calendar === 'roc') return year >= 1 ? ['roc', year] : ['broc', 1 - year];
  if (calendar === 'ethiopic') return year >= 1 ? ['am', year] : ['aa', year + 5500];
  if (calendar === 'islamic-civil' || calendar === 'islamic-tbla' || calendar === 'islamic-umalqura') return year >= 1 ? ['ah', year] : ['bh', 1 - year];
  for (var j = 0; j < ERAS.length; j++) if (ERAS[j][0] === calendar) return [ERAS[j][1], year];
  return undefined;
}

// The calendars with fixed rules are computed here, as Calendrical Calculations (Reingold and Dershowitz) gives
// them; ICU is asked only about the ones that need observation or tables (Chinese, Korean, Persian, Umm al-Qura).
// Days are counted from 1970-01-01, which is R.D. 719163.
var RD_1970 = 719163;
function monthList(codes, starts, lengths) {
  var months = list();
  for (var i = 0; i < codes.length; i++) { var m = create(null); m.code = codes[i]; m.start = starts[i]; m.length = lengths[i]; arrayPush(months, m); }
  return months;
}
function yearRecord(start, end, codes, lengths) {
  var info = create(null), starts = list(), at = start;
  for (var i = 0; i < lengths.length; i++) { arrayPush(starts, at); at += lengths[i]; }
  info.start = start; info.end = end; info.months = monthList(codes, starts, lengths);
  return info;
}
var TWELVE = ['M01', 'M02', 'M03', 'M04', 'M05', 'M06', 'M07', 'M08', 'M09', 'M10', 'M11', 'M12'];
// Tabular Hijri: 30-year cycles with 11 leap years; the civil epoch is 622-07-19 ISO, the astronomical one a day earlier
function islamicNewYear(calendar, year) {
  var epoch = (calendar === 'islamic-tbla' ? 227014 : 227015) - RD_1970;
  return epoch + (year - 1) * 354 + floor((3 + 11 * year) / 30);
}
function islamicYear(calendar, year) {
  var leap = ((14 + 11 * year) % 30 + 30) % 30 < 11, lengths = list();
  for (var m = 1; m <= 12; m++) arrayPush(lengths, m === 12 && leap ? 30 : m % 2 === 1 ? 30 : 29);
  return yearRecord(islamicNewYear(calendar, year), islamicNewYear(calendar, year + 1), TWELVE, lengths);
}
// Coptic and Ethiopic: twelve months of 30 days and one of 5, or 6 in a year that is 3 modulo 4
function copticNewYear(calendar, year) {
  if (calendar === 'coptic') return 103605 - RD_1970 + 365 * (year - 1) + floor(year / 4);
  if (calendar === 'ethioaa') year -= 5500;  // the Amete Alem year counts 5500 years more
  return 2796 - RD_1970 + 365 * (year - 1) + floor(year / 4);
}
function copticYear(calendar, year) {
  var lengths = list(), codes = list(), leap = ((calendar === 'ethioaa' ? year - 5500 : year) % 4 + 4) % 4 === 3;
  for (var m = 1; m <= 13; m++) { arrayPush(codes, m < 13 ? TWELVE[m - 1] : 'M13'); arrayPush(lengths, m < 13 ? 30 : leap ? 6 : 5); }
  return yearRecord(copticNewYear(calendar, year), copticNewYear(calendar, year + 1), codes, lengths);
}
// Hebrew: the molad of Tishri with its postponements; years of 353 to 385 days
function hebrewElapsedDays(year) {
  var months = floor((235 * year - 234) / 19), parts = 12084 + 13753 * months, days = 29 * months + floor(parts / 25920);
  return ((3 * (days + 1)) % 7 + 7) % 7 < 3 ? days + 1 : days;
}
function hebrewNewYear(year) {
  var ny0 = hebrewElapsedDays(year - 1), ny1 = hebrewElapsedDays(year), ny2 = hebrewElapsedDays(year + 1);
  var correction = ny2 - ny1 === 356 ? 2 : ny1 - ny0 === 382 ? 1 : 0;
  return -1373427 - RD_1970 + ny1 + correction;
}
function hebrewYear(year) {
  var start = hebrewNewYear(year), end = hebrewNewYear(year + 1), length = end - start, leap = ((7 * year + 1) % 19 + 19) % 19 < 7;
  var codes = list(), lengths = list();
  var add = function (code, days) { arrayPush(codes, code); arrayPush(lengths, days); };
  add('M01', 30); add('M02', length % 10 === 5 ? 30 : 29); add('M03', length % 10 === 3 ? 29 : 30); add('M04', 29); add('M05', 30);
  if (leap) add('M05L', 30);
  add('M06', 29); add('M07', 30); add('M08', 29); add('M09', 30); add('M10', 29); add('M11', 30); add('M12', 29);
  return yearRecord(start, end, codes, lengths);
}
// Indian national (Saka): the year starts on ISO March 22 of year + 78, March 21 in a leap year; Chaitra has 30 days
// (31 in a leap year), the next five months 31 and the last six 30
function indianNewYear(year) { var iso = year + 78; return epochDays(iso, 3, isLeapYear(iso) ? 21 : 22); }
function indianYear(year) {
  var leap = isLeapYear(year + 78), lengths = [leap ? 31 : 30, 31, 31, 31, 31, 31, 30, 30, 30, 30, 30, 30];
  return yearRecord(indianNewYear(year), indianNewYear(year + 1), TWELVE, lengths);
}
function arithmeticYearInfo(calendar, year) {
  var offset = ISO_MONTH_CALENDARS[calendar];
  if (offset !== undefined) {  // the months of ISO 8601
    var iso = year - offset, lengths = list();
    for (var m = 1; m <= 12; m++) arrayPush(lengths, daysInMonth(iso, m));
    return yearRecord(epochDays(iso, 1, 1), epochDays(iso + 1, 1, 1), TWELVE, lengths);
  }
  if (calendar === 'islamic-civil' || calendar === 'islamic-tbla') return islamicYear(calendar, year);
  if (calendar === 'coptic' || calendar === 'ethiopic' || calendar === 'ethioaa') return copticYear(calendar, year);
  if (calendar === 'hebrew') return hebrewYear(year);
  if (calendar === 'indian') return indianYear(year);
  return undefined;
}
// A first guess at the arithmetic year of a day, which nonIsoDate then corrects by single years
function calendarYearOfDay(calendar, day) {
  if (calendar === 'islamic-civil' || calendar === 'islamic-tbla') return floor((day - islamicNewYear(calendar, 1)) / 354.36667) + 1;
  if (calendar === 'coptic' || calendar === 'ethiopic' || calendar === 'ethioaa') return floor((day - copticNewYear(calendar, calendar === 'ethioaa' ? 5501 : 1)) / 365.25) + (calendar === 'ethioaa' ? 5501 : 1);
  if (calendar === 'hebrew') return floor((day - hebrewNewYear(1)) / 365.24682) + 1;
  if (calendar === 'indian') return dateFromEpochDays(day).year - 78;
  return icu.calendarYearOf(calendar, day);
}
// The months of a calendar year: { start, end, months: [{ code, start, length }] }, days since 1970-01-01
var YEAR_CACHE = new $Map();
function calendarYearInfo(calendar, year) {
  var key = calendar + '|' + year, info = mapGet(YEAR_CACHE, key);
  if (info !== undefined) return info;
  info = arithmeticYearInfo(calendar, year);
  if (info !== undefined) { mapSet(YEAR_CACHE, key, info); return info; }
  var raw = icu.calendarYear(calendar, year);
  if (raw === undefined) throwRange('The ' + calendar + ' calendar is not available');
  var months = list();
  for (var i = 2; i + 2 < raw.length; i += 3) {
    var m = create(null);
    m.code = raw[i]; m.start = raw[i + 1]; m.length = raw[i + 2];
    arrayPush(months, m);
  }
  info = create(null);
  info.start = raw[0]; info.end = raw[1]; info.months = months;
  mapSet(YEAR_CACHE, key, info);
  return info;
}
function isLunisolar(calendar) { return calendar === 'chinese' || calendar === 'dangi' || calendar === 'hebrew'; }
// CalendarISOToDate for a calendar other than ISO 8601 (NonISOCalendarISOToDate, 4.1.14)
function nonIsoDate(calendar, date) {
  var r = create(null), offset = ISO_MONTH_CALENDARS[calendar], era;
  if (offset !== undefined) {
    r.year = date.year + offset; r.month = date.month; r.monthCode = monthCode(date.month); r.day = date.day;
    r.daysInMonth = daysInMonth(date.year, date.month); r.daysInYear = daysInYear(date.year); r.monthsInYear = 12;
    r.inLeapYear = isLeapYear(date.year); r.dayOfYear = dayOfYear(date);
  } else {
    var day = epochDays(date.year, date.month, date.day), year = calendarYearOfDay(calendar, day), info = calendarYearInfo(calendar, year);
    while (day < info.start) info = calendarYearInfo(calendar, --year);
    while (day >= info.end) info = calendarYearInfo(calendar, ++year);
    var k = info.months.length - 1;
    while (k > 0 && info.months[k].start > day) k--;
    var month = info.months[k];
    r.year = year; r.month = k + 1; r.monthCode = month.code; r.day = day - month.start + 1;
    r.daysInMonth = month.length; r.daysInYear = info.end - info.start; r.monthsInYear = info.months.length;
    r.inLeapYear = isLunisolar(calendar) ? info.months.length > 12 : r.daysInYear > (strSlice(calendar, 0, 7) === 'islamic' ? 354 : 365);
    r.dayOfYear = day - info.start + 1;
  }
  era = eraOf(calendar, date, r.year);
  r.era = era === undefined ? undefined : era[0];
  r.eraYear = era === undefined ? undefined : era[1];
  return r;
}
// CalendarISOToDate (12.3.?) for every calendar: the calendar date record of an ISO date
function calendarDate(calendar, date) {
  if (calendar !== 'iso8601') return nonIsoDate(calendar, date);
  var r = create(null);
  r.era = undefined; r.eraYear = undefined; r.year = date.year; r.month = date.month; r.monthCode = monthCode(date.month); r.day = date.day;
  r.daysInMonth = daysInMonth(date.year, date.month); r.daysInYear = daysInYear(date.year); r.monthsInYear = 12;
  r.inLeapYear = isLeapYear(date.year); r.dayOfYear = dayOfYear(date);
  return r;
}
function calendarMonthsInYear(calendar, year) {
  return ISO_MONTH_CALENDARS[calendar] !== undefined ? 12 : calendarYearInfo(calendar, year).months.length;
}
function calendarDaysInMonth(calendar, year, month) {
  var offset = ISO_MONTH_CALENDARS[calendar];
  if (offset !== undefined) return daysInMonth(year - offset, month);
  return calendarYearInfo(calendar, year).months[month - 1].length;
}
// Arithmetic years this far out are past the ISO limits whatever the calendar, and past what ICU can compute
function checkCalendarYear(calendar, year) { if (year > 300000 || year < -300000) throwRange('The year is out of range'); }
// CalendarIntegersToISO (4.1.13)
function calendarIntegersToISO(calendar, year, month, day) {
  checkCalendarYear(calendar, year);
  var offset = ISO_MONTH_CALENDARS[calendar];
  if (offset !== undefined) {
    if (!isValidISODate(year - offset, month, day)) throwRange('The date does not exist in the ' + calendar + ' calendar');
    return isoDate(year - offset, month, day);
  }
  var info = calendarYearInfo(calendar, year);
  if (month < 1 || month > info.months.length || day < 1 || day > info.months[month - 1].length) throwRange('The date does not exist in the ' + calendar + ' calendar');
  return dateFromEpochDays(info.months[month - 1].start + day - 1);
}
// IsValidMonthCodeForCalendar (4.1.4)
function isValidMonthCodeForCalendar(calendar, code) {
  var p = parseMonthCode(code);
  if (!p.leap && p.month >= 1 && p.month <= 12) return true;
  if (!p.leap && p.month === 13) return calendar === 'coptic' || calendar === 'ethiopic' || calendar === 'ethioaa';
  if (p.leap) {
    if (calendar === 'chinese' || calendar === 'dangi') return p.month >= 1 && p.month <= 12;
    if (calendar === 'hebrew') return p.month === 5;
  }
  return false;
}
// YearContainsMonthCode (4.1.5)
function yearContainsMonthCode(calendar, year, code) {
  if (!parseMonthCode(code).leap) return true;
  checkCalendarYear(calendar, year);
  var months = calendarYearInfo(calendar, year).months;
  for (var i = 0; i < months.length; i++) if (months[i].code === code) return true;
  return false;
}
// ConstrainMonthCode (4.1.6)
function constrainMonthCode(calendar, year, code, overflow) {
  if (yearContainsMonthCode(calendar, year, code)) return code;
  if (overflow === 'reject') throwRange(code + ' is not a month of that year');
  if (calendar === 'hebrew') return 'M06';
  return monthCode(parseMonthCode(code).month);
}
// MonthCodeToOrdinal (4.1.7)
function monthCodeToOrdinal(calendar, year, code) {
  if (ISO_MONTH_CALENDARS[calendar] !== undefined || (!isLunisolar(calendar))) return parseMonthCode(code).month;
  var months = calendarYearInfo(calendar, year).months;
  for (var i = 0; i < months.length; i++) if (months[i].code === code) return i + 1;
  throwRange(code + ' is not a month of that year');
}
// BalanceNonISODate (4.1.16)
function balanceNonIsoDate(calendar, year, month, day) {
  var monthsInYear = calendarMonthsInYear(calendar, year);
  while (month <= 0) { year--; checkCalendarYear(calendar, year); monthsInYear = calendarMonthsInYear(calendar, year); month += monthsInYear; }
  while (month > monthsInYear) { month -= monthsInYear; year++; checkCalendarYear(calendar, year); monthsInYear = calendarMonthsInYear(calendar, year); }
  var days = calendarDaysInMonth(calendar, year, month);
  while (day <= 0) {
    month--;
    if (month === 0) { year--; checkCalendarYear(calendar, year); monthsInYear = calendarMonthsInYear(calendar, year); month = monthsInYear; }
    days = calendarDaysInMonth(calendar, year, month);
    day += days;
  }
  while (day > days) {
    day -= days;
    month++;
    if (month > monthsInYear) { year++; checkCalendarYear(calendar, year); monthsInYear = calendarMonthsInYear(calendar, year); month = 1; }
    days = calendarDaysInMonth(calendar, year, month);
  }
  var r = create(null);
  r.year = year; r.month = month; r.day = day;
  return r;
}
// CompareSurpasses (3.5.5): monthOrCode is an ordinal month or a month code
function compareSurpasses(sign, year, monthOrCode, day, target) {
  if (year !== target.year) return sign * (year - target.year) > 0;
  if (typeof monthOrCode === 'string') {
    if (monthOrCode !== target.monthCode) return sign * (monthOrCode < target.monthCode ? -1 : 1) > 0;
  } else if (monthOrCode !== target.month) return sign * (monthOrCode - target.month) > 0;
  if (day !== target.day) return sign * (day - target.day) > 0;
  return false;
}
// NonISODateSurpasses (4.1.17)
function nonIsoDateSurpasses(calendar, sign, from, to, years, months, weeks, days) {
  var parts = calendarDate(calendar, from), target = calendarDate(calendar, to);
  var y0 = parts.year + years;
  if (compareSurpasses(sign, y0, parts.monthCode, parts.day, target)) return true;
  var m0 = monthCodeToOrdinal(calendar, y0, constrainMonthCode(calendar, y0, parts.monthCode, 'constrain'));
  var monthsAdded = balanceNonIsoDate(calendar, y0, m0 + months, 1);
  if (compareSurpasses(sign, monthsAdded.year, monthsAdded.month, parts.day, target)) return true;
  if (weeks === 0 && days === 0) return false;
  var endOfMonth = balanceNonIsoDate(calendar, monthsAdded.year, monthsAdded.month + 1, 0);
  var day = parts.day <= endOfMonth.day ? parts.day : endOfMonth.day;
  var balanced = balanceNonIsoDate(calendar, endOfMonth.year, endOfMonth.month, day + 7 * weeks + days);
  return compareSurpasses(sign, balanced.year, balanced.month, balanced.day, target);
}
// NonISODateAdd (4.1.18)
function nonIsoDateAdd(calendar, date, d, overflow) {
  var parts = calendarDate(calendar, date), y0 = parts.year + d.years;
  checkCalendarYear(calendar, y0);
  var m0 = monthCodeToOrdinal(calendar, y0, constrainMonthCode(calendar, y0, parts.monthCode, overflow));
  var endOfMonth = balanceNonIsoDate(calendar, y0, m0 + d.months + 1, 0), day = parts.day;
  if (day > endOfMonth.day) {
    if (overflow === 'reject') throwRange('The day does not exist in that month');
    day = endOfMonth.day;
  }
  var balanced = balanceNonIsoDate(calendar, endOfMonth.year, endOfMonth.month, day + 7 * d.weeks + d.days);
  var result = calendarIntegersToISO(calendar, balanced.year, balanced.month, balanced.day);
  if (!isoDateWithinLimits(result)) throwRange('The date is out of range');
  return result;
}
// NonISODateUntil (4.1.19)
function nonIsoDateUntil(calendar, one, two, largestUnit) {
  var sign = -compareISODate(one, two);
  if (sign === 0) return dateDuration(0, 0, 0, 0);
  var years = 0, months = 0, weeks = 0, days = 0, candidate;
  if (largestUnit === YEAR) {
    for (candidate = sign; !nonIsoDateSurpasses(calendar, sign, one, two, candidate, 0, 0, 0); candidate += sign) years = candidate;
  }
  if (largestUnit === YEAR || largestUnit === MONTH) {
    for (candidate = sign; !nonIsoDateSurpasses(calendar, sign, one, two, years, candidate, 0, 0); candidate += sign) months = candidate;
  }
  if (largestUnit === WEEK) {
    for (candidate = sign; !nonIsoDateSurpasses(calendar, sign, one, two, years, months, candidate, 0); candidate += sign) weeks = candidate;
  }
  // the days that are left, counted at once: the steps of one day at a time give the same
  var start = nonIsoDateAdd(calendar, one, dateDuration(years, months, weeks, 0), 'constrain');
  days = epochDays(two.year, two.month, two.day) - epochDays(start.year, start.month, start.day);
  if (sign * days < 0) days = 0;
  return dateDuration(years, months, weeks, days);
}
// NonISOResolveFields (4.1.24)
function nonIsoResolveFields(calendar, fields, type) {
  var needsYear = type === 'date' || type === 'year-month' || fields.monthCode === undefined || fields.month !== undefined;
  var needsDay = type === 'date' || type === 'month-day', eras = calendarSupportsEra(calendar);
  if (needsYear && fields.year === undefined) {
    if (!eras) throwType('year is required');
    if (fields.era === undefined || fields.eraYear === undefined) throwType('year, or era and eraYear, are required');
  }
  if (eras) {
    if (fields.era !== undefined && fields.eraYear === undefined) throwType('eraYear is required with era');
    if (fields.eraYear !== undefined && fields.era === undefined) throwType('era is required with eraYear');
  }
  if (needsDay && fields.day === undefined) throwType('day is required');
  if (fields.month === undefined && fields.monthCode === undefined) throwType('month or monthCode is required');
  if (eras && fields.eraYear !== undefined) {
    var era = canonicalizeEraInCalendar(calendar, fields.era);
    if (era === undefined) throwRange(fields.era + ' is not an era of the ' + calendar + ' calendar');
    var year = arithmeticYearForEraYear(calendar, era, fields.eraYear);
    if (fields.year !== undefined && fields.year !== year) throwRange('year and eraYear disagree');
    fields.year = year;
  }
  fields.era = undefined;
  fields.eraYear = undefined;
  if (fields.monthCode !== undefined) {
    if (!isValidMonthCodeForCalendar(calendar, fields.monthCode)) throwRange(fields.monthCode + ' is not a month code of the ' + calendar + ' calendar');
    if (fields.year !== undefined) {
      checkCalendarYear(calendar, fields.year);
      var code = yearContainsMonthCode(calendar, fields.year, fields.monthCode) ? fields.monthCode : constrainMonthCode(calendar, fields.year, fields.monthCode, 'constrain');
      var month = monthCodeToOrdinal(calendar, fields.year, code);
      if (fields.month !== undefined && fields.month !== month) throwRange('month and monthCode disagree');
      fields.month = month;
    }
  }
}
// NonISOCalendarDateToISO (4.1.20)
function nonIsoCalendarDateToISO(calendar, fields, overflow) {
  checkCalendarYear(calendar, fields.year);
  if (fields.monthCode !== undefined) constrainMonthCode(calendar, fields.year, fields.monthCode, overflow);
  var monthsInYear = calendarMonthsInYear(calendar, fields.year), month = fields.month;
  if (month > monthsInYear) {
    if (overflow === 'reject') throwRange('The month does not exist in that year');
    month = monthsInYear;
  }
  var days = calendarDaysInMonth(calendar, fields.year, month), day = fields.day;
  if (day > days) {
    if (overflow === 'reject') throwRange('The day does not exist in that month');
    day = days;
  }
  return calendarIntegersToISO(calendar, fields.year, month, day);
}
// The years of Table 6 for the Chinese and Korean calendars: [code, days 1-29, day 30]; 0 where none is known
var LUNISOLAR_REFERENCE = [['M01', 1972, 1970], ['M01L', 0, 0], ['M02', 1972, 1972], ['M02L', 1947, 0], ['M03', 1972, 1966], ['M03L', 1966, 1955],
  ['M04', 1972, 1970], ['M04L', 1963, 1944], ['M05', 1972, 1972], ['M05L', 1971, 1952], ['M06', 1972, 1971], ['M06L', 1960, 1941],
  ['M07', 1972, 1972], ['M07L', 1968, 1938], ['M08', 1972, 1971], ['M08L', 1957, 0], ['M09', 1972, 1972], ['M09L', 2014, 0],
  ['M10', 1972, 1972], ['M10L', 1984, 0], ['M11', 1972, 1970], ['M11L', 2034, 0], ['M12', 1972, 1972], ['M12L', 0, 0]];
// The latest day with that month code and day from 1900 to 1972, else the earliest from 1973 to 2035
function referenceDateFor(calendar, code, day) {
  var limit = epochDays(1972, 12, 31), first = epochDays(1900, 1, 1), last = epochDays(2035, 12, 31);
  var year = calendarDate(calendar, isoDate(1972, 12, 31)).year, info, i, at;
  for (var y = year; ; y--) {
    info = calendarYearInfo(calendar, y);
    if (info.end <= first) break;
    for (i = info.months.length - 1; i >= 0; i--) {
      var m = info.months[i];
      at = m.start + day - 1;
      if (m.code === code && day <= m.length && at <= limit && at >= first) return dateFromEpochDays(at);
    }
  }
  for (var z = year; ; z++) {
    info = calendarYearInfo(calendar, z);
    if (info.start > last) break;
    for (i = 0; i < info.months.length; i++) {
      var n = info.months[i];
      at = n.start + day - 1;
      if (n.code === code && day <= n.length && at > limit && at <= last) return dateFromEpochDays(at);
    }
  }
  return undefined;
}
// The most days a month with that code has in any year (in the years searched for reference dates)
function maxDaysInMonthCode(calendar, code) {
  if (calendar === 'chinese' || calendar === 'dangi') return 30;
  var best = 0, year = calendarDate(calendar, isoDate(1972, 12, 31)).year;
  for (var y = year - 75; y <= year + 65; y++) {
    var months = calendarYearInfo(calendar, y).months;
    for (var i = 0; i < months.length; i++) if (months[i].code === code && months[i].length > best) best = months[i].length;
  }
  return best;
}
// NonISOMonthDayToISOReferenceDate (4.1.21)
function nonIsoMonthDayToISOReferenceDate(calendar, fields, overflow) {
  var code, days;
  if (fields.year !== undefined) {
    checkCalendarYear(calendar, fields.year);
    var monthsInYear = calendarMonthsInYear(calendar, fields.year), month = fields.month;
    if (month > monthsInYear) {
      if (overflow === 'reject') throwRange('The month does not exist in that year');
      month = monthsInYear;
    }
    if (fields.monthCode === undefined) code = calendarDate(calendar, calendarIntegersToISO(calendar, fields.year, month, 1)).monthCode;
    else code = constrainMonthCode(calendar, fields.year, fields.monthCode, overflow);
    days = calendarDaysInMonth(calendar, fields.year, month);
  } else {
    code = fields.monthCode;
    days = maxDaysInMonthCode(calendar, code);
  }
  var day = fields.day;
  if (day > days) {
    if (overflow === 'reject') throwRange('The day does not exist in that month');
    day = days;
  }
  if (calendar === 'chinese' || calendar === 'dangi') {
    for (var i = 0; i < LUNISOLAR_REFERENCE.length; i++) {
      var row = LUNISOLAR_REFERENCE[i];
      if (row[0] !== code) continue;
      if (row[1] === 0 || (day === 30 && row[2] === 0)) {
        if (overflow === 'reject') throwRange('That month and day are not known to occur');
        code = monthCode(parseMonthCode(code).month);
      }
    }
  }
  var result = referenceDateFor(calendar, code, day);
  if (result === undefined && day === 30) result = referenceDateFor(calendar, code, 29);
  if (result === undefined) throwRange('That month and day do not occur in the ' + calendar + ' calendar');
  return result;
}
