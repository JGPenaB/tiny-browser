// ---------------------------------------------------------------- Intl.DurationFormat (13)
var DURATION_UNITS = ['years', 'months', 'weeks', 'days', 'hours', 'minutes', 'seconds', 'milliseconds', 'microseconds', 'nanoseconds'];
var DURATION_NF_UNITS = ['year', 'month', 'week', 'day', 'hour', 'minute', 'second', 'millisecond', 'microsecond', 'nanosecond'];
var DURATION_STYLES = list();
(function () {
  var dateStyles = ['long', 'short', 'narrow'], clock = ['long', 'short', 'narrow', 'numeric', '2-digit'], fraction = ['long', 'short', 'narrow', 'numeric'];
  for (var i = 0; i < 10; i++) DURATION_STYLES[i] = i < 4 ? dateStyles : i < 7 ? clock : fraction;
})();
function isFractionalSecondUnit(unit) { return unit === 'milliseconds' || unit === 'microseconds' || unit === 'nanoseconds'; }
// The digital format of a locale (13.2.3): CLDR's duration patterns, such as "h:mm:ss" or "h.mm.ss"
function digitalFormat(locale) {
  var units = icu.resource('unit', toIcuId(locale), 'durationUnits'), hm = units !== undefined && typeof units.hm === 'string' ? units.hm : 'h:mm';
  var ms = units !== undefined && typeof units.ms === 'string' ? units.ms : 'm:ss';
  var separatorOf = function (pattern, a, b) {
    var i = strIndexOf(pattern, a), j = strIndexOf(pattern, b);
    while (i >= 0 && pattern[i + 1] === a) i++;
    return i >= 0 && j > i ? strSlice(pattern, i + 1, j) : ':';
  };
  return record({ hourMinute: separatorOf(hm, 'h', 'm'), minuteSecond: separatorOf(ms, 'm', 's'), twoDigitHours: strSlice(hm, 0, 2) === 'hh' });
}
// GetDurationUnitOptions (13.5.6) and ValidateDurationUnitStyle (13.5.6.2)
function getDurationUnitOptions(unit, options, baseStyle, styles, digitalBase, prevStyle, twoDigitHours) {
  var style = getOption(options, unit, 'string', styles, undefined), displayDefault = 'always';
  if (style === undefined) {
    if (baseStyle === 'digital') {
      style = digitalBase;
      if (unit !== 'hours' && unit !== 'minutes' && unit !== 'seconds') displayDefault = 'auto';
    } else if (prevStyle === 'fractional' || prevStyle === 'numeric' || prevStyle === '2-digit') {
      style = 'numeric';
      if (unit !== 'minutes' && unit !== 'seconds') displayDefault = 'auto';
    } else { style = baseStyle; displayDefault = 'auto'; }
  }
  if (style === 'numeric' && isFractionalSecondUnit(unit)) { style = 'fractional'; displayDefault = 'auto'; }
  var display = getOption(options, unit + 'Display', 'string', ['auto', 'always'], displayDefault);
  if (display === 'always' && style === 'fractional') throwRange(unit + 'Display cannot be "always" for a fraction of a second');
  if (prevStyle === 'fractional' && style !== 'fractional') throwRange(unit + ' must be numeric after a fraction of a second');
  if ((prevStyle === 'numeric' || prevStyle === '2-digit') && style !== 'fractional' && style !== 'numeric' && style !== '2-digit') {
    throwRange(unit + ' must be numeric after a numeric unit');
  }
  if (unit === 'hours' && twoDigitHours) style = '2-digit';
  if ((unit === 'minutes' || unit === 'seconds') && (prevStyle === 'numeric' || prevStyle === '2-digit')) style = '2-digit';
  return record({ style: style, display: display });
}
class DurationFormat extends $Object {
  constructor() {
    var o = create(protoFor(new.target, DurationFormat.prototype));
    var resolution = resolveOptions(arguments[0], arguments[1], false, [['nu', 'numberingSystem']], ['nu'], numberingSystemData);
    var options = resolution.options, resolved = resolution.resolved, digital = digitalFormat(resolved.dataLocale);
    var r = record({ kind: 'DurationFormat', locale: resolved.locale, dataLocale: resolved.dataLocale, numberingSystem: resolved.nu,
                     hourMinute: digital.hourMinute, minuteSecond: digital.minuteSecond, units: list() });
    r.style = getOption(options, 'style', 'string', ['long', 'short', 'narrow', 'digital'], 'short');
    var prevStyle = '';
    for (var i = 0; i < 10; i++) {
      var unit = DURATION_UNITS[i];
      var unitOptions = getDurationUnitOptions(unit, options, r.style, DURATION_STYLES[i], i < 4 ? 'short' : 'numeric', prevStyle, digital.twoDigitHours);
      r.units[i] = unitOptions;
      if (i >= 4 && i <= 8) prevStyle = unitOptions.style;
    }
    r.fractionalDigits = getNumberOption(options, 'fractionalDigits', 0, 9, undefined);
    wmSet(slots, o, r);
    return o;
  }
  static supportedLocalesOf(locales) { return supportedLocalesOf(locales, arguments[1]); }
  resolvedOptions() {
    var r = slotOf(this, 'DurationFormat'), out = {};
    createDataProperty(out, 'locale', r.locale);
    createDataProperty(out, 'numberingSystem', r.numberingSystem);
    createDataProperty(out, 'style', r.style);
    for (var i = 0; i < 10; i++) {
      createDataProperty(out, DURATION_UNITS[i], r.units[i].style === 'fractional' ? 'numeric' : r.units[i].style);
      createDataProperty(out, DURATION_UNITS[i] + 'Display', r.units[i].display);
    }
    if (r.fractionalDigits !== undefined) createDataProperty(out, 'fractionalDigits', r.fractionalDigits);
    return out;
  }
  format(duration) {
    var parts = partitionDurationFormat(slotOf(this, 'DurationFormat'), toDurationRecord(duration)), s = '';
    for (var i = 0; i < parts.length; i++) s += parts[i].value;
    return s;
  }
  formatToParts(duration) {
    var parts = partitionDurationFormat(slotOf(this, 'DurationFormat'), toDurationRecord(duration)), out = [];
    for (var i = 0; i < parts.length; i++) {
      var o = {};
      createDataProperty(o, 'type', parts[i].type);
      createDataProperty(o, 'value', parts[i].value);
      if (parts[i].unit !== undefined) createDataProperty(o, 'unit', parts[i].unit);
      createDataProperty(out, i, o);
    }
    return out;
  }
}
// ToIntegerIfIntegral (13.5.? ): a Number that is an integer, else a RangeError
function toIntegerIfIntegral(v) {
  var n = +v;
  if (n !== n || n === Infinity || n === -Infinity || floor(n) !== n) throwRange('A duration field must be an integer');
  return n + 0;
}
// ToDurationRecord (13.5.3): the ten fields, in the order of the units. A Temporal.Duration gives its own fields.
function toDurationRecord(input) {
  var info, v = list(), i;
  if (!isObject(input)) {
    if (typeof input !== 'string') throwType('A duration must be an object or a string');
    info = natives.durationFromString(input);  // ToTemporalDuration (Temporal 7.5.12) of the string
  } else info = temporalInfo(input);
  if (info !== undefined && info.kind === 'Duration') {
    for (i = 0; i < 10; i++) v[i] = info.v[i];
    return v;
  }
  var order = ['days', 'hours', 'microseconds', 'milliseconds', 'minutes', 'months', 'nanoseconds', 'seconds', 'weeks', 'years'], any = false;
  for (i = 0; i < 10; i++) v[i] = 0;
  for (i = 0; i < 10; i++) {
    var value = input[order[i]];
    if (value !== undefined) { v[arrayIndexOf(DURATION_UNITS, order[i])] = toIntegerIfIntegral(value); any = true; }
  }
  if (!any) throwType('A duration needs at least one unit');
  if (!isValidDurationRecord(v)) throwRange('The duration is out of range');
  return v;
}
// IsValidDuration (13.5.? ): one sign, years to weeks under 2**32, the rest under 2**53 seconds in all
function isValidDurationRecord(v) {
  var sign = 0, i;
  for (i = 0; i < 10; i++) {
    if (v[i] < 0) { if (sign > 0) return false; sign = -1; } else if (v[i] > 0) { if (sign < 0) return false; sign = 1; }
  }
  for (i = 0; i < 3; i++) if ((v[i] < 0 ? -v[i] : v[i]) >= 4294967296) return false;
  var $BigInt = P.BigInt.value;
  var ns = $BigInt(v[3]) * 86400000000000n + $BigInt(v[4]) * 3600000000000n + $BigInt(v[5]) * 60000000000n + $BigInt(v[6]) * 1000000000n +
           $BigInt(v[7]) * 1000000n + $BigInt(v[8]) * 1000n + $BigInt(v[9]);
  if (ns < 0n) ns = -ns;
  return ns < 9007199254740992000000000n;
}
function durationSign(v) { for (var i = 0; i < 10; i++) { if (v[i] < 0) return -1; if (v[i] > 0) return 1; } return 0; }
// A unit's value plus the fractional units after it (ComputeFractionalDigits, 13.5.7), as an exact decimal string
function withFraction(r, v, index) {
  var $BigInt = P.BigInt.value, total = $BigInt(v[index]) * 1000000000n, exponent = 3;
  for (var i = 7; i < 10; i++) {
    if (r.units[i].style !== 'fractional') continue;
    total += $BigInt(v[i]) * (exponent === 3 ? 1000000n : exponent === 6 ? 1000n : 1n);  // value / 10**exponent, in units of 1e-9
    exponent += 3;
  }
  var negative = total < 0n, a = negative ? -total : total, whole = `${a / 1000000000n}`, frac = `${a % 1000000000n}`;
  while (frac.length < 9) frac = '0' + frac;
  return (negative ? '-' : '') + whole + '.' + frac;
}
function numberParts(r, nfOptions, decimal, unit) {
  nfOptions.numberingSystem = r.numberingSystem;
  var nf = slotOf(new NumberFormat(r.locale, nfOptions), 'NumberFormat'), x = toIntlMathematicalValue(decimal);
  var parts = toParts(icu.numberFormat(nf.dataLocale, nf.skeleton, x.decimal, true), x, false, nf.style), out = list();
  for (var i = 0; i < parts.length; i++) push(out, record({ type: parts[i].type, value: parts[i].value, unit: unit }));
  return out;
}
function fractionOptions(r, nfOptions) {
  if (r.fractionalDigits === undefined) { nfOptions.maximumFractionDigits = 9; nfOptions.minimumFractionDigits = 0; }
  else { nfOptions.maximumFractionDigits = r.fractionalDigits; nfOptions.minimumFractionDigits = r.fractionalDigits; }
  nfOptions.roundingMode = 'trunc';
}
function isZeroDecimal(d) { return +d === 0; }
// FormatNumericUnits (13.5.12)
function formatNumericUnits(r, v, first, signDisplayed) {
  var out = list(), hours = `${v[4]}`, minutes = `${v[5]}`, seconds = `${v[6]}`;
  if (v[7] !== 0 || v[8] !== 0 || v[9] !== 0) seconds = withFraction(r, v, 6);
  var hoursFormatted = first === 'hours' && (v[4] !== 0 || r.units[4].display === 'always');
  var secondsFormatted = !isZeroDecimal(seconds) || r.units[6].display === 'always';
  var minutesFormatted = (first === 'hours' || first === 'minutes') && ((hoursFormatted && secondsFormatted) || v[5] !== 0 || r.units[5].display === 'always');
  var negative = durationSign(v) === -1, piece = function (decimal, index, unit, extra) {
    var nfOptions = create(null);
    if (r.units[index].style === '2-digit') nfOptions.minimumIntegerDigits = 2;
    if (!signDisplayed) nfOptions.signDisplay = 'never';
    nfOptions.useGrouping = false;
    if (extra) fractionOptions(r, nfOptions);
    if (signDisplayed && isZeroDecimal(decimal) && negative) decimal = '-0';
    var parts = numberParts(r, nfOptions, decimal, unit);
    for (var k = 0; k < parts.length; k++) push(out, parts[k]);
  };
  if (hoursFormatted) { piece(hours, 4, 'hour', false); signDisplayed = false; }
  if (minutesFormatted) {
    if (hoursFormatted) push(out, record({ type: 'literal', value: r.hourMinute, unit: undefined }));
    piece(minutes, 5, 'minute', false);
    signDisplayed = false;
  }
  if (secondsFormatted) {
    if (minutesFormatted) push(out, record({ type: 'literal', value: r.minuteSecond, unit: undefined }));
    var negativeZero = signDisplayed && isZeroDecimal(seconds) && negative;
    var nfOptions = create(null);
    if (r.units[6].style === '2-digit') nfOptions.minimumIntegerDigits = 2;
    if (!signDisplayed) nfOptions.signDisplay = 'never';
    nfOptions.useGrouping = false;
    fractionOptions(r, nfOptions);
    var parts = numberParts(r, nfOptions, negativeZero ? '-0' : seconds, 'second');
    for (var k = 0; k < parts.length; k++) push(out, parts[k]);
  }
  return out;
}
// PartitionDurationFormatPattern (13.5.15)
function partitionDurationFormat(r, v) {
  var result = list(), signDisplayed = true, negative = durationSign(v) === -1;
  for (var i = 0; i < 10; i++) {
    var style = r.units[i].style, display = r.units[i].display, unit = DURATION_UNITS[i];
    if (style === 'numeric' || style === '2-digit') {
      var numeric = formatNumericUnits(r, v, unit, signDisplayed);
      if (numeric.length !== 0) push(result, numeric);
      break;
    }
    var nfOptions = create(null), value = `${v[i]}`, last = false;
    if ((unit === 'seconds' && r.units[7].style === 'fractional') || (unit === 'milliseconds' && r.units[8].style === 'fractional') ||
        (unit === 'microseconds' && r.units[9].style === 'fractional')) {
      value = withFraction(r, v, i);
      fractionOptions(r, nfOptions);
      last = true;
    }
    if (display === 'always' || !isZeroDecimal(value)) {
      if (signDisplayed) {
        signDisplayed = false;
        if (isZeroDecimal(value) && negative) value = '-0';
      } else nfOptions.signDisplay = 'never';
      nfOptions.style = 'unit';
      nfOptions.unit = DURATION_NF_UNITS[i];
      nfOptions.unitDisplay = style;
      push(result, numberParts(r, nfOptions, value, DURATION_NF_UNITS[i]));
    }
    if (last) break;
  }
  // ListFormatParts (13.5.14)
  var strings = list(), j, k;
  for (j = 0; j < result.length; j++) { var s = ''; for (k = 0; k < result[j].length; k++) s += result[j][k].value; push(strings, s); }
  var lf = record({ dataLocale: r.dataLocale, type: 'unit', style: r.style === 'digital' ? 'short' : r.style });
  var listed = strings.length === 0 ? list() : listParts(lf, strings), out = list(), index = 0;
  for (j = 0; j < listed.length; j++) {
    if (listed[j].type === 'element') { for (k = 0; k < result[index].length; k++) push(out, result[index][k]); index++; }
    else push(out, record({ type: 'literal', value: listed[j].value, unit: undefined }));
  }
  return out;
}
// Temporal.Duration.prototype.toLocaleString (Temporal 15.11.1.1)
function durationToLocaleString(info, locales, options) {
  var r = slotOf(new DurationFormat(locales, options), 'DurationFormat'), parts = partitionDurationFormat(r, info.v), s = '';
  for (var i = 0; i < parts.length; i++) s += parts[i].value;
  return s;
}
