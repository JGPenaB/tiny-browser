// ---------------------------------------------------------------- shared by the services (9.2.8)
var toPrimitiveNumber = natives.toPrimitiveNumber;
var FALLBACK = $Symbol('IntlLegacyConstructedSymbol');  // %Intl%.[[FallbackSymbol]] (8.1)
// ResolveOptions (9.2.8). descriptors are [key, property, type, values]; localeData(locale) gives the supported
// values of each relevant key.
function resolveOptions(locales, options, coerce, descriptors, relevantKeys, localeData, modify) {
  var requested = canonicalizeLocaleList(locales);
  if (coerce === 'require' && options === undefined) throwType('Options are required');
  options = coerce === true ? coerceOptionsToObject(options) : getOptionsObject(options);
  var opt = record({ localeMatcher: getOption(options, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit') });
  for (var i = 0; i < descriptors.length; i++) {
    var d = descriptors[i], value = getOption(options, d[1], d[2] === undefined ? 'string' : d[2], d[3], undefined);
    if (value !== undefined) {
      value = toString(value);
      if (!isUnicodeType(value)) throwRange(value + ' is not a valid value for ' + d[1]);
    }
    opt[d[0]] = value;
  }
  if (modify !== undefined) modify(opt);
  return record({ options: options, resolved: resolveLocale(requested, opt, relevantKeys, localeData), opt: opt });
}
// The numbering systems a locale offers: its default one first, then every one with digits of its own (9.1)
function numberingSystemData(locale) {
  var out = list(), def = defaultNumberingSystem(locale), all = availableNumberingSystems();
  if (def === 'native' || def === 'traditio' || def === 'finance' || !contains(all, def)) def = 'latn';
  push(out, def);
  for (var i = 0; i < all.length; i++) if (all[i] !== def) push(out, all[i]);
  return { nu: out };
}
// The constructors of 4.3 Note 1 that may be called as functions (NumberFormat, DateTimeFormat): the object made is
// stored on `this` when `this` is one of theirs
var getPrototypeOf = $Object.getPrototypeOf;
function ordinaryHasInstance(ctor, o) {
  if (!isObject(o)) return false;
  var proto = ctor.prototype;
  while ((o = getPrototypeOf(o)) !== null) if (o === proto) return true;
  return false;
}
function chainLegacy(made, newTarget, self, ctor) {
  if (newTarget === undefined && ordinaryHasInstance(ctor, self)) {
    defineProperty(self, FALLBACK, { value: made, writable: false, enumerable: false, configurable: false });
    return self;
  }
  return made;
}
function unwrapLegacy(x, kind, ctor) {
  if (!isObject(x)) throwType('this is not an Intl.' + kind);
  if (kindOf(x) !== kind && ordinaryHasInstance(ctor, x)) return x[FALLBACK];
  return x;
}
function defineMethods(proto, methods) {
  var names = getOwnPropertyNames(methods);
  for (var i = 0; i < names.length; i++) {
    var d = $Object.getOwnPropertyDescriptor(methods, names[i]);
    if (d.get !== undefined) defineProperty(proto, names[i], { get: d.get, enumerable: false, configurable: true });
    else defineProperty(proto, names[i], { value: d.value, writable: true, enumerable: false, configurable: true });
  }
}
// A function made the way the engine makes its built-ins: it has no prototype and is not a constructor
function builtinFunction(name, length, fn) {
  var f = { [name](a, b) { return fn(a, b); } }[name];
  defineProperty(f, 'length', { value: length, writable: false, enumerable: false, configurable: true });
  return f;
}
// A constructor written as a function: its prototype property is read-only, as for every built-in
function setUpConstructor(ctor, methods) {
  defineProperty(ctor, 'prototype', { writable: false, enumerable: false, configurable: false });
  defineMethods(ctor.prototype, methods);
  return ctor.prototype;
}

// ---------------------------------------------------------------- Intl mathematical values (16.5.15)
// { kind: 'finite' | 'nan' | 'inf', negative, decimal } where decimal is what ICU is given: "-12.5e+3", "NaN", ...
var strTrim = uncurry($String.prototype.trim), $BigInt = BigInt;
function mathematicalValue(decimal, negative) { return record({ kind: 'finite', negative: negative, decimal: decimal }); }
var NAN_VALUE = { kind: 'nan', negative: false, decimal: 'NaN' };
function decimalLiteral(s) {  // StrUnsignedDecimalLiteral without Infinity: digits, a point, an exponent
  var i = 0, n = s.length, digits = 0;
  while (i < n && isDigitCode(charCodeAt(s, i))) { i++; digits++; }
  if (i < n && s[i] === '.') { i++; while (i < n && isDigitCode(charCodeAt(s, i))) { i++; digits++; } }
  if (digits === 0) return false;
  if (i < n && (s[i] === 'e' || s[i] === 'E')) {
    i++;
    if (i < n && (s[i] === '+' || s[i] === '-')) i++;
    var start = i;
    while (i < n && isDigitCode(charCodeAt(s, i))) i++;
    if (i === start) return false;
  }
  return i === n;
}
function toIntlMathematicalValue(value) {
  var prim = toPrimitiveNumber(value), str;
  if (typeof prim === 'bigint') return mathematicalValue(`${prim}`, prim < 0n);
  if (typeof prim === 'string') str = prim;
  else {
    var x = +prim;
    if (x === 0 && 1 / x < 0) return mathematicalValue('-0', true);
    str = `${x}`;
  }
  str = strTrim(str);
  if (str === '') return mathematicalValue('0', false);
  var c = str[0], negative = c === '-', body = c === '-' || c === '+' ? strSlice(str, 1) : str;
  if (body === 'Infinity') return { kind: 'inf', negative: negative, decimal: negative ? '-Infinity' : 'Infinity' };
  if (body !== str || !(c === '0' && str.length > 2 && strIndexOf('xXoObB', str[1]) >= 0)) {
    if (!decimalLiteral(body)) return NAN_VALUE;
  } else {  // 0x, 0o, 0b: a whole number in another base
    try { return mathematicalValue(`${$BigInt(str)}`, false); } catch (e) { return NAN_VALUE; }
  }
  // RoundMVResult: what would round to an infinite or zero Number is infinite or zero
  var rounded = $Number(body);
  if (rounded === Infinity) return { kind: 'inf', negative: negative, decimal: negative ? '-Infinity' : 'Infinity' };
  if (rounded === 0) return mathematicalValue(negative ? '-0' : '0', negative);
  if (body[0] === '.') body = '0' + body;
  return mathematicalValue((negative ? '-' : '') + body, negative);
}

// ---------------------------------------------------------------- number formats as ICU skeletons
var ROUNDING_MODE_STEMS = { ceil: 'ceiling', floor: 'floor', expand: 'up', trunc: 'down', halfCeil: 'half-ceiling', halfFloor: 'half-floor',
                            halfExpand: 'half-up', halfTrunc: 'half-down', halfEven: 'half-even' };
function repeat(c, n) { var s = ''; for (var i = 0; i < n; i++) s += c; return s; }
// The rounding part of a skeleton, from the digit options (16.5.2-16.5.4)
function precisionStem(r) {
  var fraction, significant = repeat('@', r.minimumSignificantDigits) + repeat('#', r.maximumSignificantDigits - r.minimumSignificantDigits), stem;
  if (r.roundingIncrement !== 1) {
    var inc = `${r.roundingIncrement}`, digits = r.maximumFractionDigits;
    while (inc.length <= digits) inc = '0' + inc;
    fraction = 'precision-increment/' + (digits === 0 ? inc : strSlice(inc, 0, inc.length - digits) + '.' + strSlice(inc, inc.length - digits));
    stem = fraction;
  } else {
    fraction = r.maximumFractionDigits === 0 ? 'precision-integer' : '.' + repeat('0', r.minimumFractionDigits) + repeat('#', r.maximumFractionDigits - r.minimumFractionDigits);
    if (r.roundingType === 'significantDigits') stem = significant;
    else if (r.roundingType === 'fractionDigits') stem = fraction;
    else stem = fraction + '/' + significant + (r.roundingType === 'morePrecision' ? 'r' : 's');
  }
  if (r.trailingZeroDisplay === 'stripIfInteger') stem += '/w';
  return stem + ' rounding-mode-' + ROUNDING_MODE_STEMS[r.roundingMode] + (r.minimumIntegerDigits > 1 ? ' integer-width/*' + repeat('0', r.minimumIntegerDigits) : '');
}
function notationStem(r) {
  if (r.notation === 'scientific') return ' scientific';
  if (r.notation === 'engineering') return ' engineering';
  if (r.notation === 'compact') return r.compactDisplay === 'long' ? ' compact-long' : ' compact-short';
  return '';
}
var SIGN_STEMS = { auto: 'sign-auto', never: 'sign-never', always: 'sign-always', exceptZero: 'sign-except-zero', negative: 'sign-negative' };
var ACCOUNTING_STEMS = { auto: 'sign-accounting', never: 'sign-never', always: 'sign-accounting-always', exceptZero: 'sign-accounting-except-zero',
                         negative: 'sign-accounting-negative' };
var GROUPING_STEMS = { auto: 'group-auto', min2: 'group-min2', always: 'group-on-aligned' };
function numberSkeleton(r) {
  var s = precisionStem(r) + notationStem(r);
  if (r.style === 'percent') s += ' percent scale/100';
  else if (r.style === 'currency') {
    s += ' currency/' + r.currency;
    if (r.currencyDisplay === 'code') s += ' unit-width-iso-code';
    else if (r.currencyDisplay === 'narrowSymbol') s += ' unit-width-narrow';
    else if (r.currencyDisplay === 'name') s += ' unit-width-full-name';
  } else if (r.style === 'unit') {
    s += ' unit/' + r.unit + (r.unitDisplay === 'long' ? ' unit-width-full-name' : r.unitDisplay === 'narrow' ? ' unit-width-narrow' : ' unit-width-short');
  }
  if (r.useGrouping !== undefined) s += ' ' + (r.useGrouping === false ? 'group-off' : GROUPING_STEMS[r.useGrouping]);
  if (r.signDisplay !== undefined) s += ' ' + (r.style === 'currency' && r.currencySign === 'accounting' ? ACCOUNTING_STEMS : SIGN_STEMS)[r.signDisplay];
  if (r.numberingSystem !== undefined) s += ' numbering-system/' + r.numberingSystem;
  return s;
}
// The ICU number field ids (UNumberFormatFields) as the part types of 16.5.?; signs are told apart by their text
var NUMBER_FIELDS = ['integer', 'fraction', 'decimal', 'exponentSeparator', 'exponentMinusSign', 'exponentInteger', 'group', 'currency',
                     'percentSign', 'percentSign', 'sign', 'unit', 'compact', 'approximatelySign'];
// Turns ICU's field list [text, category, field, start, end, ...] into parts: each character takes the innermost field
// around it, and runs of the same type become one part. sources marks the range spans (startRange, endRange).
function toParts(raw, x, withSource, style) {
  var text = raw[0], n = text.length, type = list(), width = list(), source = list(), i, k;
  for (i = 0; i < n; i++) { type[i] = 'literal'; width[i] = Infinity; source[i] = 'shared'; }
  for (k = 1; k < raw.length; k += 4) {
    var category = raw[k], field = raw[k + 1], start = raw[k + 2], end = raw[k + 3];
    if (category === 0x1002) {  // UFIELD_CATEGORY_NUMBER_RANGE_SPAN
      for (i = start; i < end; i++) source[i] = field === 0 ? 'startRange' : 'endRange';
      continue;
    }
    if (category !== 2) continue;
    var t = NUMBER_FIELDS[field];
    if (t === 'percentSign' && style === 'unit') t = 'unit';  // the unit "percent" is a unit, not a percent style
    if (t === undefined) continue;
    for (i = start; i < end; i++) if (end - start < width[i]) { type[i] = t; width[i] = end - start; }
  }
  var parts = list();
  for (i = 0; i < n;) {
    var j = i + 1;
    while (j < n && type[j] === type[i] && source[j] === source[i] && !(type[i] === 'sign')) j++;
    var value = strSlice(text, i, j), partType = type[i];
    if (partType === 'sign') partType = value === '+' ? 'plusSign' : 'minusSign';
    if (partType === 'integer' && x !== undefined && x.kind !== 'finite') partType = x.kind === 'nan' ? 'nan' : 'infinity';
    if (partType === 'literal' && x !== undefined && x.kind !== 'finite' && isSpecialText(value)) partType = x.kind === 'nan' ? 'nan' : 'infinity';
    var part = record({ type: partType, value: value, source: withSource ? source[i] : undefined });
    push(parts, part);
    i = j;
  }
  return parts;
}
function isSpecialText(s) { return s === 'NaN' || s === '∞' || strIndexOf(s, '∞') >= 0 || strIndexOf(s, 'NaN') >= 0; }
function partsArray(parts) {
  var out = [];
  for (var i = 0; i < parts.length; i++) {
    var o = {};
    createDataProperty(o, 'type', parts[i].type);
    createDataProperty(o, 'value', parts[i].value);
    if (parts[i].source !== undefined) createDataProperty(o, 'source', parts[i].source);
    defineProperty(out, i, { value: o, writable: true, enumerable: true, configurable: true });
  }
  return out;
}

// ---------------------------------------------------------------- digit options (16.1.2)
function defaultNumberOption(value, minimum, maximum, fallback) {
  if (value === undefined) return fallback;
  value = +value;
  if (value !== value || value < minimum || value > maximum) throwRange('A number option is out of range');
  return floor(value);
}
function getNumberOption(options, property, minimum, maximum, fallback) { return defaultNumberOption(options[property], minimum, maximum, fallback); }
var INCREMENTS = [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000];
function setDigitOptions(r, options, minFDDefault, maxFDDefault, notation) {
  var minInt = getNumberOption(options, 'minimumIntegerDigits', 1, 21, 1);
  var minFD = options.minimumFractionDigits, maxFD = options.maximumFractionDigits;
  var minSD = options.minimumSignificantDigits, maxSD = options.maximumSignificantDigits;
  var increment = getNumberOption(options, 'roundingIncrement', 1, 5000, 1);
  if (!contains(INCREMENTS, increment)) throwRange('roundingIncrement must be one of 1, 2, 5, 10, 20, 25, 50, 100 and so on');
  var mode = getOption(options, 'roundingMode', 'string', ['ceil', 'floor', 'expand', 'trunc', 'halfCeil', 'halfFloor', 'halfExpand', 'halfTrunc', 'halfEven'], 'halfExpand');
  var priority = getOption(options, 'roundingPriority', 'string', ['auto', 'morePrecision', 'lessPrecision'], 'auto');
  var trailing = getOption(options, 'trailingZeroDisplay', 'string', ['auto', 'stripIfInteger'], 'auto');
  if (increment !== 1) maxFDDefault = minFDDefault;
  r.minimumIntegerDigits = minInt;
  r.roundingIncrement = increment;
  r.roundingMode = mode;
  r.trailingZeroDisplay = trailing;
  var hasSD = minSD !== undefined || maxSD !== undefined, hasFD = minFD !== undefined || maxFD !== undefined, needSD, needFD;
  if (priority === 'auto') { needSD = hasSD; needFD = !needSD && !(!hasFD && notation === 'compact'); }
  else { needSD = true; needFD = true; }
  if (needSD) {
    if (hasSD) {
      r.minimumSignificantDigits = defaultNumberOption(minSD, 1, 21, 1);
      r.maximumSignificantDigits = defaultNumberOption(maxSD, r.minimumSignificantDigits, 21, 21);
    } else { r.minimumSignificantDigits = 1; r.maximumSignificantDigits = 21; }
  }
  if (needFD) {
    if (hasFD) {
      var lo = defaultNumberOption(minFD, 0, 100, undefined), hi = defaultNumberOption(maxFD, 0, 100, undefined);
      if (lo === undefined) lo = minFDDefault < hi ? minFDDefault : hi;
      else if (hi === undefined) hi = maxFDDefault > lo ? maxFDDefault : lo;
      else if (lo > hi) throwRange('minimumFractionDigits is more than maximumFractionDigits');
      r.minimumFractionDigits = lo; r.maximumFractionDigits = hi;
    } else { r.minimumFractionDigits = minFDDefault; r.maximumFractionDigits = maxFDDefault; }
  }
  if (!needSD && !needFD) {
    r.minimumFractionDigits = 0; r.maximumFractionDigits = 0; r.minimumSignificantDigits = 1; r.maximumSignificantDigits = 2;
    r.roundingType = 'morePrecision'; r.roundingPriority = 'morePrecision';
  } else if (priority === 'morePrecision' || priority === 'lessPrecision') { r.roundingType = priority; r.roundingPriority = priority; }
  else if (hasSD) { r.roundingType = 'significantDigits'; r.roundingPriority = 'auto'; }
  else { r.roundingType = 'fractionDigits'; r.roundingPriority = 'auto'; }
  if (increment !== 1) {
    if (r.roundingType !== 'fractionDigits') throwType('roundingIncrement needs fraction digit rounding');
    if (r.maximumFractionDigits !== r.minimumFractionDigits) throwRange('roundingIncrement needs equal fraction digit bounds');
  }
}
function isWellFormedCurrencyCode(c) { return c.length === 3 && allOf(toUpperCase(c), function (x) { return x >= 65 && x <= 90; }); }
function isSanctionedUnit(u) { return contains(UNITS, u); }
function isWellFormedUnitIdentifier(u) {
  if (isSanctionedUnit(u)) return true;
  var halves = strSplit(u, '-per-');
  return halves.length === 2 && isSanctionedUnit(halves[0]) && isSanctionedUnit(halves[1]);
}

// ---------------------------------------------------------------- Intl.NumberFormat (16)
var NumberFormatProto;
function NumberFormat() {
  var locales = arguments[0], options = arguments[1];
  var o = create(new.target === undefined ? NumberFormatProto : protoFor(new.target, NumberFormatProto));
  var resolution = resolveOptions(locales, options, true, [['nu', 'numberingSystem']], ['nu'], numberingSystemData);
  options = resolution.options;
  var r = record({ kind: 'NumberFormat', locale: resolution.resolved.locale, dataLocale: resolution.resolved.dataLocale, numberingSystem: resolution.resolved.nu });
  // SetNumberFormatUnitOptions (16.1.3)
  var style = getOption(options, 'style', 'string', ['decimal', 'percent', 'currency', 'unit'], 'decimal');
  r.style = style;
  var currency = getOption(options, 'currency', 'string', undefined, undefined);
  if (currency === undefined) { if (style === 'currency') throwType('A currency format needs a currency'); }
  else if (!isWellFormedCurrencyCode(currency)) throwRange(currency + ' is not a currency code');
  var currencyDisplay = getOption(options, 'currencyDisplay', 'string', ['code', 'symbol', 'narrowSymbol', 'name'], 'symbol');
  var currencySign = getOption(options, 'currencySign', 'string', ['standard', 'accounting'], 'standard');
  var unit = getOption(options, 'unit', 'string', undefined, undefined);
  if (unit === undefined) { if (style === 'unit') throwType('A unit format needs a unit'); }
  else if (!isWellFormedUnitIdentifier(unit)) throwRange(unit + ' is not a unit identifier');
  var unitDisplay = getOption(options, 'unitDisplay', 'string', ['short', 'narrow', 'long'], 'short');
  if (style === 'currency') { r.currency = toUpperCase(currency); r.currencyDisplay = currencyDisplay; r.currencySign = currencySign; }
  if (style === 'unit') { r.unit = unit; r.unitDisplay = unitDisplay; }
  var notation = getOption(options, 'notation', 'string', ['standard', 'scientific', 'engineering', 'compact'], 'standard');
  r.notation = notation;
  var minFDDefault, maxFDDefault;
  if (style === 'currency' && notation === 'standard') minFDDefault = maxFDDefault = icu.currencyDigits(r.currency);
  else if (style === 'percent') { minFDDefault = 0; maxFDDefault = 0; }
  else { minFDDefault = 0; maxFDDefault = 3; }
  setDigitOptions(r, options, minFDDefault, maxFDDefault, notation);
  var compactDisplay = getOption(options, 'compactDisplay', 'string', ['short', 'long'], 'short'), defaultUseGrouping = 'auto';
  if (notation === 'compact') { r.compactDisplay = compactDisplay; defaultUseGrouping = 'min2'; }
  var useGrouping = getBooleanOrStringOption(options, 'useGrouping', ['min2', 'auto', 'always', 'true', 'false'], defaultUseGrouping);
  if (useGrouping === 'true' || useGrouping === 'false') useGrouping = defaultUseGrouping;
  if (useGrouping === true) useGrouping = 'always';
  r.useGrouping = useGrouping;
  r.signDisplay = getOption(options, 'signDisplay', 'string', ['auto', 'never', 'always', 'exceptZero', 'negative'], 'auto');
  r.skeleton = numberSkeleton(r);
  wmSet(slots, o, r);
  return chainLegacy(o, new.target, this, NumberFormat);
}
function getBooleanOrStringOption(options, property, values, fallback) {
  var value = options[property];
  if (value === undefined) return fallback;
  if (value === true) return true;
  if (!value) return false;
  value = toString(value);
  if (!contains(values, value)) throwRange(value + ' is not a valid value for ' + property);
  return value;
}
function numberFormatOf(x) { return slotOf(unwrapLegacy(x, 'NumberFormat', NumberFormat), 'NumberFormat'); }
function formatNumeric(r, x) { return icu.numberFormat(r.dataLocale, r.skeleton, x.decimal, false); }
function formatNumericToParts(r, x) { return partsArray(toParts(icu.numberFormat(r.dataLocale, r.skeleton, x.decimal, true), x, false, r.style)); }
function rangeValues(start, end) {
  if (start === undefined || end === undefined) throwType('A range needs a start and an end');
  var x = toIntlMathematicalValue(start), y = toIntlMathematicalValue(end);
  if (x.kind === 'nan' || y.kind === 'nan') throwRange('A range cannot start or end with NaN');
  return [x, y];
}
var NUMBER_OPTIONS = ['locale', 'numberingSystem', 'style', 'currency', 'currencyDisplay', 'currencySign', 'unit', 'unitDisplay', 'minimumIntegerDigits',
                      'minimumFractionDigits', 'maximumFractionDigits', 'minimumSignificantDigits', 'maximumSignificantDigits', 'useGrouping',
                      'notation', 'compactDisplay', 'signDisplay', 'roundingIncrement', 'roundingMode', 'roundingPriority', 'trailingZeroDisplay'];
function resolvedOptionsObject(r, names) {
  var out = {};
  for (var i = 0; i < names.length; i++) if (r[names[i]] !== undefined) createDataProperty(out, names[i], r[names[i]]);
  return out;
}
NumberFormatProto = setUpConstructor(NumberFormat, {
  resolvedOptions() { return resolvedOptionsObject(numberFormatOf(this), NUMBER_OPTIONS); },
  get format() {
    var r = numberFormatOf(this);
    if (r.boundFormat === undefined) r.boundFormat = builtinFunction('', 1, function (value) { return formatNumeric(r, toIntlMathematicalValue(value)); });
    return r.boundFormat;
  },
  formatToParts(value) { var r = slotOf(this, 'NumberFormat'); return formatNumericToParts(r, toIntlMathematicalValue(value)); },
  formatRange(start, end) {
    var r = slotOf(this, 'NumberFormat'), xy = rangeValues(start, end);
    return icu.numberFormatRange(r.dataLocale, r.skeleton, xy[0].decimal, xy[1].decimal, false);
  },
  formatRangeToParts(start, end) {
    var r = slotOf(this, 'NumberFormat'), xy = rangeValues(start, end);
    return partsArray(toParts(icu.numberFormatRange(r.dataLocale, r.skeleton, xy[0].decimal, xy[1].decimal, true), undefined, true, r.style));
  }
});
defineProperty(NumberFormat, 'supportedLocalesOf', { value: builtinFunction('supportedLocalesOf', 1, supportedLocalesOf), writable: true, enumerable: false, configurable: true });

// ---------------------------------------------------------------- Intl.PluralRules (17)
var PLURAL_ORDER = ['zero', 'one', 'two', 'few', 'many', 'other'];
class PluralRules extends $Object {
  constructor() {
    var locales = arguments[0], options = arguments[1];
    var o = create(protoFor(new.target, PluralRules.prototype));
    var resolution = resolveOptions(locales, options, true, [], [], function () { return {}; });
    options = resolution.options;
    var r = record({ kind: 'PluralRules', locale: resolution.resolved.locale, dataLocale: resolution.resolved.dataLocale });
    r.type = getOption(options, 'type', 'string', ['cardinal', 'ordinal'], 'cardinal');
    r.notation = getOption(options, 'notation', 'string', ['standard', 'scientific', 'engineering', 'compact'], 'standard');
    var compactDisplay = getOption(options, 'compactDisplay', 'string', ['short', 'long'], 'short');
    if (r.notation === 'compact') r.compactDisplay = compactDisplay;
    setDigitOptions(r, options, 0, 3, r.notation);
    r.skeleton = precisionStem(r) + notationStem(r);
    wmSet(slots, o, r);
    return o;
  }
  static supportedLocalesOf(locales) { return supportedLocalesOf(locales, arguments[1]); }
  select(value) {
    var r = slotOf(this, 'PluralRules'), n = toIntlMathematicalValue(value);
    if (n.kind !== 'finite') return 'other';
    return icu.pluralSelect(r.dataLocale, r.type === 'ordinal', r.skeleton, n.decimal);
  }
  selectRange(start, end) {
    var r = slotOf(this, 'PluralRules'), xy = rangeValues(start, end);
    if (xy[0].kind !== 'finite' || xy[1].kind !== 'finite') return 'other';
    return icu.pluralSelectRange(r.dataLocale, r.type === 'ordinal', r.skeleton, xy[0].decimal, xy[1].decimal);
  }
  resolvedOptions() {
    var r = slotOf(this, 'PluralRules'), out = {}, keywords = icu.pluralKeywords(r.dataLocale, r.type === 'ordinal'), categories = list();
    for (var i = 0; i < PLURAL_ORDER.length; i++) if (contains(keywords, PLURAL_ORDER[i])) push(categories, PLURAL_ORDER[i]);
    var names = ['locale', 'type', 'notation', 'compactDisplay', 'minimumIntegerDigits', 'minimumFractionDigits', 'maximumFractionDigits',
                 'minimumSignificantDigits', 'maximumSignificantDigits', 'pluralCategories', 'roundingIncrement', 'roundingMode', 'roundingPriority',
                 'trailingZeroDisplay'];
    for (var j = 0; j < names.length; j++) {
      var value = names[j] === 'pluralCategories' ? toArray(categories) : r[names[j]];
      if (value !== undefined) createDataProperty(out, names[j], value);
    }
    return out;
  }
}

// Number.prototype.toLocaleString and BigInt.prototype.toLocaleString (20.2.1, 20.3.1), called by the engine
natives.numberToLocaleString = function (x, locales, options) {
  return formatNumeric(slotOf(new NumberFormat(locales, options), 'NumberFormat'), toIntlMathematicalValue(x));
};
