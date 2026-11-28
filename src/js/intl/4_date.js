// ---------------------------------------------------------------- Intl.DateTimeFormat (11)
// Formats are ICU patterns: the options pick a skeleton ("yMMMd", "jjmm"), the locale's pattern generator turns it
// into the pattern that is used, and the resolved options are read back from that pattern's letters.
var COMPONENTS = [
  ['weekday', ['narrow', 'short', 'long']], ['era', ['narrow', 'short', 'long']], ['year', ['2-digit', 'numeric']],
  ['month', ['2-digit', 'numeric', 'narrow', 'short', 'long']], ['day', ['2-digit', 'numeric']], ['dayPeriod', ['narrow', 'short', 'long']],
  ['hour', ['2-digit', 'numeric']], ['minute', ['2-digit', 'numeric']], ['second', ['2-digit', 'numeric']], ['fractionalSecondDigits', undefined],
  ['timeZoneName', ['short', 'long', 'shortOffset', 'longOffset', 'shortGeneric', 'longGeneric']]];
var HOUR_LETTERS = { h11: 'K', h12: 'h', h23: 'H', h24: 'k' };
var STYLES = ['full', 'long', 'medium', 'short'];
function hourCycleOfLetter(c) { return c === 'K' ? 'h11' : c === 'h' ? 'h12' : c === 'H' ? 'h23' : c === 'k' ? 'h24' : undefined; }
// The letters of a pattern outside its quoted literals, with how often each comes in a row: [[letter, count], ...]
function patternFields(pattern) {
  var out = list(), i = 0, n = pattern.length, quoted = false;
  while (i < n) {
    var c = pattern[i];
    if (c === "'") { quoted = !quoted; i++; continue; }
    if (quoted || !isAlphaCode(charCodeAt(pattern, i))) { i++; continue; }
    var j = i;
    while (j < n && pattern[j] === c) j++;
    push(out, [c, j - i]);
    i = j;
  }
  return out;
}
// Changes the hour letters of a pattern (outside quotes) to the one of an hour cycle
function withHourLetter(pattern, letter) {
  var out = '', quoted = false;
  for (var i = 0; i < pattern.length; i++) {
    var c = pattern[i];
    if (c === "'") quoted = !quoted;
    out += !quoted && (c === 'h' || c === 'H' || c === 'K' || c === 'k') ? letter : c;
  }
  return out;
}
// What a pattern shows of each component of Table 16
function componentsOfPattern(pattern) {
  var fields = patternFields(pattern), r = record({});
  var text = function (n) { return n === 4 ? 'long' : n === 5 ? 'narrow' : 'short'; };
  for (var i = 0; i < fields.length; i++) {
    var c = fields[i][0], n = fields[i][1];
    if (c === 'E' || c === 'c' || c === 'e') r.weekday = n === 6 ? 'short' : text(n);
    else if (c === 'G') r.era = text(n);
    else if (c === 'y' || c === 'Y' || c === 'u' || c === 'U' || c === 'r') r.year = n === 2 ? '2-digit' : 'numeric';
    else if (c === 'M' || c === 'L') r.month = n === 1 ? 'numeric' : n === 2 ? '2-digit' : text(n);
    else if (c === 'd') r.day = n === 2 ? '2-digit' : 'numeric';
    else if (c === 'B' || c === 'b') r.dayPeriod = text(n);
    else if (c === 'h' || c === 'H' || c === 'K' || c === 'k') { r.hour = n === 2 ? '2-digit' : 'numeric'; r.hourLetter = c; }
    else if (c === 'm') r.minute = n === 2 ? '2-digit' : 'numeric';
    else if (c === 's') r.second = n === 2 ? '2-digit' : 'numeric';
    else if (c === 'S') r.fractionalSecondDigits = n > 3 ? 3 : n;
    else if (c === 'z') r.timeZoneName = n === 4 ? 'long' : 'short';
    else if (c === 'O' || c === 'Z' || c === 'X' || c === 'x' || c === 'V') r.timeZoneName = n === 4 ? 'longOffset' : 'shortOffset';
    else if (c === 'v') r.timeZoneName = n === 4 ? 'longGeneric' : 'shortGeneric';
  }
  return r;
}
// The ICU skeleton for the components asked for
function skeletonOf(opts, hourCycle) {
  var s = '', v;
  var text = function (letter, value) { return value === 'narrow' ? repeat(letter, 5) : value === 'long' ? repeat(letter, 4) : repeat(letter, 3); };
  if ((v = opts.weekday) !== undefined) s += text('E', v);
  if ((v = opts.era) !== undefined) s += v === 'narrow' ? 'GGGGG' : v === 'long' ? 'GGGG' : 'G';
  if ((v = opts.year) !== undefined) s += v === '2-digit' ? 'yy' : 'y';
  if ((v = opts.month) !== undefined) s += v === 'numeric' ? 'M' : v === '2-digit' ? 'MM' : text('M', v);
  if ((v = opts.day) !== undefined) s += v === '2-digit' ? 'dd' : 'd';
  if ((v = opts.dayPeriod) !== undefined) s += v === 'narrow' ? 'BBBBB' : v === 'long' ? 'BBBB' : 'B';
  if ((v = opts.hour) !== undefined) s += repeat(HOUR_LETTERS[hourCycle], v === '2-digit' ? 2 : 1);
  if ((v = opts.minute) !== undefined) s += v === '2-digit' ? 'mm' : 'm';
  if ((v = opts.second) !== undefined) s += v === '2-digit' ? 'ss' : 's';
  if ((v = opts.fractionalSecondDigits) !== undefined) s += repeat('S', v);
  if ((v = opts.timeZoneName) !== undefined) {
    s += v === 'short' ? 'z' : v === 'long' ? 'zzzz' : v === 'shortOffset' ? 'O' : v === 'longOffset' ? 'OOOO' : v === 'shortGeneric' ? 'v' : 'vvvv';
  }
  return s;
}
function icuDateLocale(r) { return r.dataLocale + '-u-ca-' + r.calendar + '-nu-' + r.numberingSystem; }
var DATE_DATA = new $Map();
// The locale data of 11.2.3: calendars (the region's own first), numbering systems, hour cycles
function dateTimeData(locale) {
  var data = mapGet(DATE_DATA, locale);
  if (data !== undefined) return data;
  var calendars = list(), preferred = calendarsOfLocale(record({ locale: locale, calendar: undefined })), all = availableCalendars(), i;
  push(calendars, preferred[0]);
  for (i = 0; i < all.length; i++) if (all[i] !== preferred[0]) push(calendars, all[i]);
  var letter = function (skeleton) {
    var fields = patternFields(icu.dateBestPattern(locale, skeleton));
    for (var k = 0; k < fields.length; k++) { var hc = hourCycleOfLetter(fields[k][0]); if (hc !== undefined) return hc; }
    return undefined;
  };
  var hourCycle = letter('j'), hour12 = letter('h'), hour24 = letter('H');
  data = record({ ca: calendars, nu: numberingSystemData(locale).nu, hc: [null, 'h11', 'h12', 'h23', 'h24'],
                  hourCycle: hourCycle === undefined ? 'h23' : hourCycle, hourCycle12: hour12 === 'h11' ? 'h11' : 'h12',
                  hourCycle24: hour24 === 'h24' ? 'h24' : 'h23' });
  mapSet(DATE_DATA, locale, data);
  return data;
}
// IsTimeZoneOffsetString and ParseTimeZoneOffsetString (ECMA-262 21.4.1.33): the offset in minutes; null for an
// offset with seconds, which is well-formed but no time zone; undefined for anything else
function parseOffsetMinutes(s) {
  var c = s[0];
  if (c !== '+' && c !== '-' && c !== '−') return undefined;
  var digits = '', colons = 0, i = 1, fraction = false;
  for (; i < s.length; i++) {
    var ch = s[i];
    if (isDigitCode(charCodeAt(s, i))) digits += ch;
    else if (ch === ':' && !fraction) { colons++; if (digits.length % 2 !== 0 || digits.length === 0) return undefined; }
    else if ((ch === '.' || ch === ',') && digits.length === 6 && !fraction) { fraction = true; digits += '|'; }
    else return undefined;
  }
  var whole = fraction ? strSlice(digits, 0, 6) : digits;
  if (whole.length !== 2 && whole.length !== 4 && whole.length !== 6) return undefined;
  if (colons !== 0 && colons !== (whole.length / 2) - 1) return undefined;
  if (fraction && digits.length === 7) return undefined;
  var hours = +strSlice(whole, 0, 2), minutes = whole.length >= 4 ? +strSlice(whole, 2, 4) : 0, seconds = whole.length === 6 ? +strSlice(whole, 4, 6) : 0;
  if (hours > 23 || minutes > 59 || seconds > 59) return undefined;
  if (whole.length === 6) return null;
  var total = hours * 60 + minutes;
  return c === '+' ? total : -total;
}
function formatOffsetIdentifier(minutes) {
  var sign = minutes < 0 ? '-' : '+', a = minutes < 0 ? -minutes : minutes, h = floor(a / 60), m = a % 60;
  return sign + (h < 10 ? '0' : '') + h + ':' + (m < 10 ? '0' : '') + m;
}
function systemTimeZoneIdentifier() {
  var id = icu.defaultTimeZone(), zone = id === undefined ? undefined : namedZone(id);
  return zone === undefined ? 'UTC' : zone.primary;
}
function icuTimeZone(timeZone) {
  if (timeZone[0] === '+' || timeZone[0] === '-') return 'GMT' + timeZone;
  return timeZone === 'UTC' ? 'UTC' : icuZone(timeZone);
}
// CreateDateTimeFormat (11.1.2); required and defaults are 'date', 'time', 'any' and 'date', 'time', 'all'
function createDateTimeFormat(newTarget, locales, options, required, defaults, toLocaleStringTimeZone) {
  var o = create(newTarget === undefined ? DateTimeFormatProto : protoFor(newTarget, DateTimeFormatProto));
  var hour12;
  var resolution = resolveOptions(locales, options, true,
    [['ca', 'calendar'], ['nu', 'numberingSystem'], ['hour12', 'hour12', 'boolean'], ['hc', 'hourCycle', 'string', ['h11', 'h12', 'h23', 'h24']]],
    ['ca', 'hc', 'nu'], dateTimeData, function (opt) {
      hour12 = opt.hour12;
      opt.hour12 = undefined;
      if (hour12 !== undefined) opt.hc = null;
    });
  options = resolution.options;
  var resolved = resolution.resolved, data = resolved.localeData;
  var r = record({ kind: 'DateTimeFormat', locale: resolved.locale, dataLocale: resolved.dataLocale, calendar: resolved.ca,
                   numberingSystem: resolved.nu });
  var hc = resolved.hc;
  if (hour12 === 'true') hc = data.hourCycle12;
  else if (hour12 === 'false') hc = data.hourCycle24;
  else if (hc === null) hc = data.hourCycle;
  var timeZone = options.timeZone;
  if (timeZone === undefined) timeZone = toLocaleStringTimeZone !== undefined ? toLocaleStringTimeZone : systemTimeZoneIdentifier();
  else {
    if (toLocaleStringTimeZone !== undefined) throwType('A ZonedDateTime is formatted in its own time zone');
    timeZone = toString(timeZone);
  }
  var offset = parseOffsetMinutes(timeZone);
  if (offset === null) throwRange(timeZone + ' has seconds, which a time zone offset cannot');
  if (offset !== undefined) timeZone = formatOffsetIdentifier(offset);
  else {
    var zone = namedZone(timeZone);
    if (zone === undefined) throwRange(timeZone + ' is not a time zone');
    timeZone = zone.id;
  }
  r.timeZone = timeZone;
  var opts = record({}), explicit = false;
  for (var i = 0; i < COMPONENTS.length; i++) {
    var name = COMPONENTS[i][0], value;
    if (name === 'fractionalSecondDigits') value = getNumberOption(options, name, 1, 3, undefined);
    else value = getOption(options, name, 'string', COMPONENTS[i][1], undefined);
    opts[name] = value;
    if (value !== undefined) explicit = true;
  }
  getOption(options, 'formatMatcher', 'string', ['basic', 'best fit'], 'best fit');
  var dateStyle = getOption(options, 'dateStyle', 'string', STYLES, undefined);
  var timeStyle = getOption(options, 'timeStyle', 'string', STYLES, undefined);
  r.dateStyle = dateStyle;
  r.timeStyle = timeStyle;
  var locale = icuDateLocale(r), main;
  var DATE_KEYS = ['weekday', 'era', 'year', 'month', 'day'], TIME_KEYS = ['dayPeriod', 'hour', 'minute', 'second', 'fractionalSecondDigits'];
  if (dateStyle !== undefined || timeStyle !== undefined) {
    if (explicit) throwType('dateStyle and timeStyle cannot be used with other date and time options');
    if (required === 'date' && timeStyle !== undefined) throwType('Only a date can be formatted here');
    if (required === 'time' && dateStyle !== undefined) throwType('Only a time can be formatted here');
    var pattern = icu.dateStylePattern(locale, dateStyle === undefined ? -1 : arrayIndexOf(STYLES, dateStyle), timeStyle === undefined ? -1 : arrayIndexOf(STYLES, timeStyle));
    var skeleton = icu.dateSkeleton(pattern), styleComponents = componentsOfPattern(pattern);
    if (styleComponents.hour !== undefined && hourCycleOfLetter(styleComponents.hourLetter) !== hc) {
      // the style's own hour cycle is not the one asked for: the same fields, with the other hours
      skeleton = withHourLetter(removeLetters(skeleton, 'ab'), HOUR_LETTERS[hc]);
      pattern = withHourLetter(icu.dateBestPattern(locale, skeleton), HOUR_LETTERS[hc]);
    }
    main = formatRecord(pattern, skeleton);
    r.plainDate = dateStyle !== undefined ? adjustStyleFormat(locale, main, DATE_KEYS, hc) : null;
    r.plainYearMonth = dateStyle !== undefined ? adjustStyleFormat(locale, main, ['era', 'year', 'month'], hc) : null;
    r.plainMonthDay = dateStyle !== undefined ? adjustStyleFormat(locale, main, ['month', 'day'], hc) : null;
    r.plainTime = timeStyle !== undefined ? adjustStyleFormat(locale, main, TIME_KEYS, hc) : null;
    r.plainDateTime = adjustStyleFormat(locale, main, arrayConcat(DATE_KEYS, TIME_KEYS), hc);
    r.instant = main;
  } else {
    main = getDateTimeFormat(locale, opts, required, defaults, 'all', hc);
    r.plainDate = getDateTimeFormat(locale, opts, 'date', 'date', 'relevant', hc);
    r.plainYearMonth = getDateTimeFormat(locale, opts, 'year-month', 'year-month', 'relevant', hc);
    r.plainMonthDay = getDateTimeFormat(locale, opts, 'month-day', 'month-day', 'relevant', hc);
    r.plainTime = getDateTimeFormat(locale, opts, 'time', 'time', 'relevant', hc);
    r.plainDateTime = getDateTimeFormat(locale, opts, 'any', 'all', 'relevant', hc);
    r.instant = getDateTimeFormat(locale, opts, 'any', toLocaleStringTimeZone !== undefined ? 'zoned' : 'all', 'all', hc);
  }
  r.main = main;
  r.pattern = main.pattern;
  r.icuLocale = locale;
  r.icuTimeZone = icuTimeZone(timeZone);
  r.components = main.components;
  if (main.components.hour !== undefined) r.hourCycle = hc;
  wmSet(slots, o, r);
  return o;
}
var arrayConcat = uncurry($Array.prototype.concat);
// A DateTime Format Record: the ICU pattern, the skeleton its ranges are made from, and the components it shows
function formatRecord(pattern, skeleton) {
  return record({ pattern: pattern, skeleton: removeLetters(skeleton, 'ab'), components: componentsOfPattern(pattern) });
}
function bestFormat(locale, opts, hc) {
  var skeleton = skeletonOf(opts, hc), pattern = icu.dateBestPattern(locale, skeleton);
  if (opts.hour !== undefined) pattern = withHourLetter(pattern, HOUR_LETTERS[hc]);
  return formatRecord(pattern, skeleton);
}
// GetDateTimeFormat (Temporal 15.6.1); required is 'date', 'time', 'year-month', 'month-day' or 'any', defaults the
// same or 'zoned' or 'all', inherit 'all' or 'relevant'. null when the options ask only for fields that do not apply.
var REQUIRED_OPTIONS = record({ date: ['weekday', 'year', 'month', 'day'], time: ['dayPeriod', 'hour', 'minute', 'second', 'fractionalSecondDigits'],
  'year-month': ['year', 'month'], 'month-day': ['month', 'day'],
  any: ['weekday', 'year', 'month', 'day', 'dayPeriod', 'hour', 'minute', 'second', 'fractionalSecondDigits'] });
var DEFAULT_OPTIONS = record({ date: ['year', 'month', 'day'], time: ['hour', 'minute', 'second'], 'year-month': ['year', 'month'],
  'month-day': ['month', 'day'], zoned: ['year', 'month', 'day', 'hour', 'minute', 'second'], all: ['year', 'month', 'day', 'hour', 'minute', 'second'] });
function getDateTimeFormat(locale, opts, required, defaults, inherit, hc) {
  var formatOptions = record({}), i;
  if (inherit === 'all') for (i = 0; i < COMPONENTS.length; i++) formatOptions[COMPONENTS[i][0]] = opts[COMPONENTS[i][0]];
  else if (required === 'date' || required === 'year-month' || required === 'any') formatOptions.era = opts.era;
  var anyPresent = false, all = REQUIRED_OPTIONS.any, needDefaults = true;
  for (i = 0; i < all.length; i++) if (opts[all[i]] !== undefined) anyPresent = true;
  var requiredOptions = REQUIRED_OPTIONS[required];
  for (i = 0; i < requiredOptions.length; i++) {
    var value = opts[requiredOptions[i]];
    if (value !== undefined) { formatOptions[requiredOptions[i]] = value; needDefaults = false; }
  }
  if (needDefaults) {
    if (anyPresent && inherit === 'relevant') return null;
    var defaultOptions = DEFAULT_OPTIONS[defaults];
    for (i = 0; i < defaultOptions.length; i++) formatOptions[defaultOptions[i]] = 'numeric';
    if (defaults === 'zoned' && formatOptions.timeZoneName === undefined) formatOptions.timeZoneName = 'short';
  }
  return bestFormat(locale, formatOptions, hc);
}
// AdjustDateTimeStyleFormat (Temporal 15.6.2): the style's own format if it shows only allowed fields, else the best
// format for the allowed fields it shows
function adjustStyleFormat(locale, base, allowed, hc) {
  var conflicting = false, i;
  for (i = 0; i < COMPONENTS.length; i++) if (base.components[COMPONENTS[i][0]] !== undefined && !contains(allowed, COMPONENTS[i][0])) conflicting = true;
  if (!conflicting) return base;
  var formatOptions = record({});
  for (i = 0; i < allowed.length; i++) formatOptions[allowed[i]] = base.components[allowed[i]];
  return bestFormat(locale, formatOptions, hc);
}
function removeLetters(s, letters) {
  var out = '';
  for (var i = 0; i < s.length; i++) if (strIndexOf(letters, s[i]) < 0) out += s[i];
  return out;
}
function timeClip(t) {
  if (t !== t || t === Infinity || t === -Infinity || t > 8.64e15 || t < -8.64e15) return NaN;
  return (t < 0 ? -floor(-t) : floor(t)) + 0;
}
// ToDateTimeFormattable (Temporal 15.6.11): a Temporal object as what the Temporal parts say of it, else a Number
var temporalInfo = natives.temporalInfo;
function toFormattable(value) {
  var info = isObject(value) ? temporalInfo(value) : undefined;
  return info !== undefined ? info : +value;
}
function daysFromCivil(y, m, d) {  // days since 1970-01-01 of a proleptic Gregorian date
  y -= m <= 2 ? 1 : 0;
  var era = floor(y / 400), yoe = y - era * 400, mp = (m + 9) % 12;
  return era * 146097 + yoe * 365 + floor(yoe / 4) - floor(yoe / 100) + floor((153 * mp + 2) / 5) + d - 1 - 719468;
}
function utcMs(info, hour, minute, second, ms) {
  return daysFromCivil(info.year, info.month, info.day) * 86400000 + ((hour * 60 + minute) * 60 + second) * 1000 + ms;
}
// HandleDateTimeValue (Temporal 15.6.22): { format, ms, plain }
function handleDateTimeValue(r, x) {
  if (typeof x === 'number') {
    x = timeClip(x);
    if (x !== x) throwRange('Invalid time value');
    return record({ format: r.main, ms: x, plain: false });
  }
  var kind = x.kind, format;
  if (kind === 'PlainDate') {
    if (x.calendar !== r.calendar && x.calendar !== 'iso8601') throwRange('The calendars differ');
    format = r.plainDate;
    if (format === null) throwType('This format has no fields of a date');
    return record({ format: format, ms: utcMs(x, 12, 0, 0, 0), plain: true });
  }
  if (kind === 'PlainYearMonth' || kind === 'PlainMonthDay') {
    if (x.calendar !== r.calendar) throwRange('The calendars differ');
    format = kind === 'PlainYearMonth' ? r.plainYearMonth : r.plainMonthDay;
    if (format === null) throwType('This format has no fields of that date');
    return record({ format: format, ms: utcMs(x, 12, 0, 0, 0), plain: true });
  }
  if (kind === 'PlainTime') {
    if (r.plainTime === null) throwType('This format has no fields of a time');
    return record({ format: r.plainTime, ms: utcMs(record({ year: 1970, month: 1, day: 1 }), x.hour, x.minute, x.second, x.millisecond), plain: true });
  }
  if (kind === 'PlainDateTime') {
    if (x.calendar !== 'iso8601' && x.calendar !== r.calendar) throwRange('The calendars differ');
    return record({ format: r.plainDateTime, ms: utcMs(x, x.hour, x.minute, x.second, x.millisecond), plain: true });
  }
  if (kind === 'Instant') return record({ format: r.instant, ms: x.epochMs, plain: false });
  throwType('A ZonedDateTime cannot be formatted here: use its toLocaleString');
}
var $Date = Date;
// The ICU date field ids (UDateFormatField) as part types
var DATE_FIELDS = ['era', 'year', 'month', 'day', 'hour', 'hour', 'minute', 'second', 'fractionalSecond', 'weekday', 'literal', 'literal',
                   'literal', 'literal', 'dayPeriod', 'hour', 'hour', 'timeZoneName', 'year', 'weekday', 'year', 'literal', 'literal',
                   'timeZoneName', 'timeZoneName', 'weekday', 'month', 'literal', 'literal', 'timeZoneName', 'yearName', 'timeZoneName',
                   'timeZoneName', 'timeZoneName', 'relatedYear', 'dayPeriod', 'dayPeriod'];
function dateParts(raw, withSource) {
  var text = raw[0], n = text.length, type = list(), width = list(), source = list(), i, k;
  for (i = 0; i < n; i++) { type[i] = 'literal'; width[i] = Infinity; source[i] = 'shared'; }
  for (k = 1; k < raw.length; k += 4) {
    var category = raw[k], field = raw[k + 1], start = raw[k + 2], end = raw[k + 3];
    if (category === 0x1005) {  // UFIELD_CATEGORY_DATE_INTERVAL_SPAN
      for (i = start; i < end; i++) source[i] = field === 0 ? 'startRange' : 'endRange';
      continue;
    }
    if (category !== 1) continue;
    var t = DATE_FIELDS[field];
    if (t === undefined) continue;
    for (i = start; i < end; i++) if (end - start < width[i]) { type[i] = t; width[i] = end - start; }
  }
  var parts = list();
  for (i = 0; i < n;) {
    var j = i + 1;
    while (j < n && type[j] === type[i] && source[j] === source[i] && (type[i] === 'literal' || width[j] === width[i])) j++;
    push(parts, record({ type: type[i], value: strSlice(text, i, j), source: withSource ? source[i] : undefined }));
    i = j;
  }
  return parts;
}
function formatValue(r, v, parts) {
  return icu.dateFormat(r.icuLocale, v.plain ? 'UTC' : r.icuTimeZone, v.format.pattern, v.ms, parts);
}
function formatDateTime(r, x) { return formatValue(r, handleDateTimeValue(r, x), false); }
function formatDateTimeToParts(r, x) { return partsArray(dateParts(formatValue(r, handleDateTimeValue(r, x), true), false)); }
// SameTemporalType (Temporal 15.6.13)
function sameTemporalType(x, y) { return typeof x !== 'number' && typeof y !== 'number' && x.kind === y.kind; }
function dateRange(r, start, end, parts) {
  if (start === undefined || end === undefined) throwType('A range needs a start and an end');
  var x = toFormattable(start), y = toFormattable(end);
  if ((typeof x !== 'number' || typeof y !== 'number') && !sameTemporalType(x, y)) throwType('A range needs two values of the same type');
  var vx = handleDateTimeValue(r, x), vy = handleDateTimeValue(r, y);
  var raw = icu.dateFormatRange(r.icuLocale, vx.plain ? 'UTC' : r.icuTimeZone, vx.format.skeleton, vx.ms, vy.ms, parts);
  if (!parts) return raw;
  return partsArray(dateParts(raw, true));
}
function formattableOrNow(date) { return date === undefined ? $Date.now() : toFormattable(date); }
function dateTimeFormatOf(x) { return slotOf(unwrapLegacy(x, 'DateTimeFormat', DateTimeFormat), 'DateTimeFormat'); }

var DateTimeFormatProto;
function DateTimeFormat() {
  var o = createDateTimeFormat(new.target === undefined ? DateTimeFormat : new.target, arguments[0], arguments[1], 'any', 'date');
  return chainLegacy(o, new.target, this, DateTimeFormat);
}
DateTimeFormatProto = setUpConstructor(DateTimeFormat, {
  resolvedOptions() {
    var r = dateTimeFormatOf(this), out = {}, i;
    var base = ['locale', 'calendar', 'numberingSystem', 'timeZone'];
    for (i = 0; i < base.length; i++) createDataProperty(out, base[i], r[base[i]]);
    if (r.hourCycle !== undefined) {
      createDataProperty(out, 'hourCycle', r.hourCycle);
      createDataProperty(out, 'hour12', r.hourCycle === 'h11' || r.hourCycle === 'h12');
    }
    if (r.dateStyle === undefined && r.timeStyle === undefined) {
      for (i = 0; i < COMPONENTS.length; i++) {
        var value = r.components[COMPONENTS[i][0]];
        if (value !== undefined) createDataProperty(out, COMPONENTS[i][0], value);
      }
    }
    if (r.dateStyle !== undefined) createDataProperty(out, 'dateStyle', r.dateStyle);
    if (r.timeStyle !== undefined) createDataProperty(out, 'timeStyle', r.timeStyle);
    return out;
  },
  get format() {
    var r = dateTimeFormatOf(this);
    if (r.boundFormat === undefined) r.boundFormat = builtinFunction('', 1, function (date) { return formatDateTime(r, formattableOrNow(date)); });
    return r.boundFormat;
  },
  formatToParts(date) { var r = slotOf(this, 'DateTimeFormat'); return formatDateTimeToParts(r, formattableOrNow(date)); },
  formatRange(startDate, endDate) { return dateRange(slotOf(this, 'DateTimeFormat'), startDate, endDate, false); },
  formatRangeToParts(startDate, endDate) { return dateRange(slotOf(this, 'DateTimeFormat'), startDate, endDate, true); }
});
defineProperty(DateTimeFormat, 'supportedLocalesOf', { value: builtinFunction('supportedLocalesOf', 1, supportedLocalesOf), writable: true, enumerable: false, configurable: true });

// Date.prototype.toLocaleString, toLocaleDateString and toLocaleTimeString (ECMA-402 21.4.1-3), called by the engine
// with the time value
natives.dateToLocaleString = function (x, locales, options, required, defaults) {
  if (x !== x) return 'Invalid Date';
  return formatDateTime(slotOf(createDateTimeFormat(undefined, locales, options, required, defaults), 'DateTimeFormat'), x);
};
// The toLocaleString methods of Temporal (Temporal 15.11), called by Temporal with what it knows of the object
natives.temporalToLocaleString = function (info, locales, options, required, defaults, timeZone) {
  var r = slotOf(createDateTimeFormat(undefined, locales, options, required, defaults, timeZone), 'DateTimeFormat');
  if (info.kind === 'ZonedDateTime') {
    if (info.calendar !== 'iso8601' && info.calendar !== r.calendar) throwRange('The calendars differ');
    info = record({ kind: 'Instant', epochMs: info.epochMs });
  }
  return formatDateTime(r, info);
};
