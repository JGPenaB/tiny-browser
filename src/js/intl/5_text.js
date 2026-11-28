// ---------------------------------------------------------------- Intl.Collator (10)
var COLLATOR_DATA = new $Map();
function collatorData(locale) {
  var data = mapGet(COLLATOR_DATA, locale);
  if (data !== undefined) return data;
  var co = list(), collations = collationsOfLocale(record({ locale: locale, collation: undefined }));
  push(co, null);
  for (var i = 0; i < collations.length; i++) if (collations[i] !== 'emoji' && collations[i] !== 'eor') push(co, collations[i]);
  push(co, 'emoji'); push(co, 'eor');
  var defaults = icu.collatorDefaults(locale), kf = list();
  push(kf, defaults[1]);
  var others = ['false', 'lower', 'upper'];
  for (var j = 0; j < others.length; j++) if (others[j] !== defaults[1]) push(kf, others[j]);
  data = record({ co: co, kn: ['false', 'true'], kf: kf, sensitivity: 'variant', ignorePunctuation: defaults[0] });
  mapSet(COLLATOR_DATA, locale, data);
  return data;
}
var CollatorProto;
function Collator() {
  var locales = arguments[0], options = arguments[1];
  var o = create(new.target === undefined ? CollatorProto : protoFor(new.target, CollatorProto));
  var requested = canonicalizeLocaleList(locales);
  options = coerceOptionsToObject(options);
  var usage = getOption(options, 'usage', 'string', ['sort', 'search'], 'sort');
  var resolution = resolveOptions(toArray(requested), options, true,
    [['co', 'collation'], ['kn', 'numeric', 'boolean'], ['kf', 'caseFirst', 'string', ['upper', 'lower', 'false']]], ['co', 'kf', 'kn'], collatorData);
  var resolved = resolution.resolved;
  var r = record({ kind: 'Collator', locale: resolved.locale, dataLocale: resolved.dataLocale, usage: usage,
                   collation: resolved.co === null ? 'default' : resolved.co, numeric: resolved.kn === 'true', caseFirst: resolved.kf });
  r.sensitivity = getOption(options, 'sensitivity', 'string', ['base', 'accent', 'case', 'variant'], usage === 'sort' ? 'variant' : resolved.localeData.sensitivity);
  r.ignorePunctuation = getOption(options, 'ignorePunctuation', 'boolean', undefined, resolved.localeData.ignorePunctuation);
  var co = usage === 'search' ? 'search' : r.collation;
  r.icuLocale = r.dataLocale + (co !== 'default' ? '-u-co-' + co : '');
  wmSet(slots, o, r);
  return o;
}
function compareStrings(r, x, y) {
  var s = r.sensitivity;
  return icu.collatorCompare(x, y, r.icuLocale, s === 'base' || s === 'case' ? 0 : s === 'accent' ? 1 : 2, s === 'case', r.ignorePunctuation,
                             r.numeric, r.caseFirst);
}
CollatorProto = setUpConstructor(Collator, {
  get compare() {
    var r = slotOf(this, 'Collator');
    if (r.boundCompare === undefined) r.boundCompare = builtinFunction('', 2, function (x, y) { return compareStrings(r, toString(x), toString(y)); });
    return r.boundCompare;
  },
  resolvedOptions() {
    return resolvedOptionsObject(slotOf(this, 'Collator'), ['locale', 'usage', 'sensitivity', 'ignorePunctuation', 'collation', 'numeric', 'caseFirst']);
  }
});
defineProperty(Collator, 'supportedLocalesOf', { value: builtinFunction('supportedLocalesOf', 1, supportedLocalesOf), writable: true, enumerable: false, configurable: true });
// String.prototype.localeCompare (20.1.1), called by the engine with both strings
natives.localeCompare = function (string, that, locales, options) { return compareStrings(slotOf(new Collator(locales, options), 'Collator'), string, that); };
// TransformCase (20.1.4): the languages with case rules of their own; any other maps as "und"
natives.changeCase = function (string, locales, upper) {
  var requested = canonicalizeLocaleList(locales);
  var locale = requested.length !== 0 ? requested[0] : defaultLocale();
  var language = parseTag(locale).language;
  return icu.changeCase(string, language === 'az' || language === 'lt' || language === 'tr' || language === 'el' ? language : 'und', upper);
};

// ---------------------------------------------------------------- Intl.RelativeTimeFormat (18)
var RELATIVE_UNITS = ['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'];
function singularRelativeTimeUnit(unit) {
  var i = arrayIndexOf(['seconds', 'minutes', 'hours', 'days', 'weeks', 'months', 'quarters', 'years'], unit);
  if (i >= 0) return RELATIVE_UNITS[i];
  if (!contains(RELATIVE_UNITS, unit)) throwRange(unit + ' is not a unit of relative time');
  return unit;
}
// PartitionPattern (9.2.? ): a CLDR pattern with "{0}" as [literal, placeholder, literal]
function patternPieces(pattern) {
  var i = strIndexOf(pattern, '{0}');
  return i < 0 ? [pattern] : [strSlice(pattern, 0, i), undefined, strSlice(pattern, i + 3)];
}
class RelativeTimeFormat extends $Object {
  constructor() {
    var o = create(protoFor(new.target, RelativeTimeFormat.prototype));
    var resolution = resolveOptions(arguments[0], arguments[1], true, [['nu', 'numberingSystem']], ['nu'], numberingSystemData);
    var options = resolution.options, resolved = resolution.resolved;
    var r = record({ kind: 'RelativeTimeFormat', locale: resolved.locale, dataLocale: resolved.dataLocale, numberingSystem: resolved.nu });
    r.style = getOption(options, 'style', 'string', ['long', 'short', 'narrow'], 'long');
    r.numeric = getOption(options, 'numeric', 'string', ['always', 'auto'], 'always');
    var nfOptions = create(null);
    nfOptions.numberingSystem = r.numberingSystem;
    r.numberFormat = slotOf(new NumberFormat(r.locale, nfOptions), 'NumberFormat');
    r.pluralRules = slotOf(new PluralRules(r.locale), 'PluralRules');
    wmSet(slots, o, r);
    return o;
  }
  static supportedLocalesOf(locales) { return supportedLocalesOf(locales, arguments[1]); }
  format(value, unit) { var r = slotOf(this, 'RelativeTimeFormat'), parts = partitionRelativeTime(r, value, unit), s = ''; for (var i = 0; i < parts.length; i++) s += parts[i].value; return s; }
  formatToParts(value, unit) {
    var parts = partitionRelativeTime(slotOf(this, 'RelativeTimeFormat'), value, unit), out = [];
    for (var i = 0; i < parts.length; i++) {
      var o = {};
      createDataProperty(o, 'type', parts[i].type);
      createDataProperty(o, 'value', parts[i].value);
      if (parts[i].unit !== undefined) createDataProperty(o, 'unit', parts[i].unit);
      createDataProperty(out, i, o);
    }
    return out;
  }
  resolvedOptions() { return resolvedOptionsObject(slotOf(this, 'RelativeTimeFormat'), ['locale', 'style', 'numeric', 'numberingSystem']); }
}
// PartitionRelativeTimePattern (18.5.2), with CLDR's relative time data as ICU has it
function partitionRelativeTime(r, value, unit) {
  value = +value;
  unit = singularRelativeTimeUnit(toString(unit));
  if (value !== value || value === Infinity || value === -Infinity) throwRange('A relative time must be a finite number');
  var locale = toIcuId(r.dataLocale);
  var fields = r.style !== 'long' ? icu.resource('', locale, 'fields/' + unit + '-' + r.style) : undefined;
  if (fields === undefined) fields = icu.resource('', locale, 'fields/' + unit);
  if (r.numeric === 'auto' && fields.relative !== undefined) {
    var literal = fields.relative[`${value}`];
    if (literal !== undefined) return [record({ type: 'literal', value: literal, unit: undefined })];
  }
  var past = value < 0 || (value === 0 && 1 / value < 0), magnitude = past ? -value : value;
  var x = toIntlMathematicalValue(magnitude);
  var numberParts = toParts(icu.numberFormat(r.numberFormat.dataLocale, r.numberFormat.skeleton, x.decimal, true), x, false, 'decimal');
  var category = icu.pluralSelect(r.pluralRules.dataLocale, false, r.pluralRules.skeleton, x.decimal);
  var tense = fields.relativeTime[past ? 'past' : 'future'];
  var pattern = tense[category] !== undefined ? tense[category] : tense.other;
  var pieces = patternPieces(pattern), out = list();
  for (var i = 0; i < pieces.length; i++) {
    if (pieces[i] === undefined) {
      for (var j = 0; j < numberParts.length; j++) push(out, record({ type: numberParts[j].type, value: numberParts[j].value, unit: unit }));
    } else if (pieces[i] !== '') push(out, record({ type: 'literal', value: pieces[i], unit: undefined }));
  }
  return out;
}

// ---------------------------------------------------------------- Intl.ListFormat (14)
var LIST_TYPES = ['conjunction', 'disjunction', 'unit'], LIST_STYLES = ['long', 'short', 'narrow'];
var iteratorSymbol = $Symbol.iterator, invoke = uncurry(fnCall);
// StringListFromIterable (14.5.5)
function stringListFromIterable(iterable) {
  var out = list();
  if (iterable === undefined) return out;
  var method = iterable[iteratorSymbol];
  if (typeof method !== 'function') throwType('The list is not iterable');
  var iterator = invoke(method, iterable);
  if (!isObject(iterator)) throwType('The iterator is not an object');
  var next = iterator.next;
  for (;;) {
    var step = invoke(next, iterator);
    if (!isObject(step)) throwType('The iterator result is not an object');
    if (step.done) return out;
    var value = step.value;
    if (typeof value !== 'string') {
      var ret = iterator['return'];
      if (ret !== undefined && ret !== null) { try { invoke(ret, iterator); } catch (e) {} }
      throwType('A list holds strings only');
    }
    push(out, value);
  }
}
function listParts(r, items) {
  var raw = icu.listFormat(r.dataLocale, arrayIndexOf(LIST_TYPES, r.type), arrayIndexOf(LIST_STYLES, r.style), items, true);
  var text = raw[0], n = text.length, type = list(), i;
  for (i = 0; i < n; i++) type[i] = 'literal';
  for (var k = 1; k < raw.length; k += 4) {
    if (raw[k] !== 3 || raw[k + 1] !== 1) continue;  // UFIELD_CATEGORY_LIST, ULISTFMT_ELEMENT_FIELD
    for (i = raw[k + 2]; i < raw[k + 3]; i++) type[i] = 'element' + raw[k + 2];  // each element is a part of its own
  }
  var parts = list();
  for (i = 0; i < n;) {
    var j = i + 1;
    while (j < n && type[j] === type[i]) j++;
    push(parts, record({ type: type[i] === 'literal' ? 'literal' : 'element', value: strSlice(text, i, j), source: undefined }));
    i = j;
  }
  return parts;
}
class ListFormat extends $Object {
  constructor() {
    var o = create(protoFor(new.target, ListFormat.prototype));
    var resolution = resolveOptions(arguments[0], arguments[1], false, [], [], function () { return record({}); });
    var options = resolution.options;
    var r = record({ kind: 'ListFormat', locale: resolution.resolved.locale, dataLocale: resolution.resolved.dataLocale });
    r.type = getOption(options, 'type', 'string', LIST_TYPES, 'conjunction');
    r.style = getOption(options, 'style', 'string', LIST_STYLES, 'long');
    wmSet(slots, o, r);
    return o;
  }
  static supportedLocalesOf(locales) { return supportedLocalesOf(locales, arguments[1]); }
  format(list) {
    var r = slotOf(this, 'ListFormat'), items = stringListFromIterable(list);
    return icu.listFormat(r.dataLocale, arrayIndexOf(LIST_TYPES, r.type), arrayIndexOf(LIST_STYLES, r.style), items, false);
  }
  formatToParts(list) { var r = slotOf(this, 'ListFormat'); return partsArray(listParts(r, stringListFromIterable(list))); }
  resolvedOptions() { return resolvedOptionsObject(slotOf(this, 'ListFormat'), ['locale', 'type', 'style']); }
}

// ---------------------------------------------------------------- Intl.DisplayNames (12)
var DATE_TIME_FIELDS = ['era', 'year', 'quarter', 'month', 'weekOfYear', 'weekday', 'day', 'dayPeriod', 'hour', 'minute', 'second', 'timeZoneName'];
// CanonicalCodeForDisplayNames (12.5.1)
function canonicalCodeForDisplayNames(type, code) {
  if (type === 'language') {
    var parsed = parseTag(code);
    if (parsed === undefined || parsed.extensions.length !== 0 || parsed.privateUse !== undefined) throwRange(code + ' is not a language identifier');
    return canonicalizeLocaleId(code);
  }
  if (type === 'region') { if (!isRegionSubtag(code)) throwRange(code + ' is not a region code'); return toUpperCase(code); }
  if (type === 'script') { if (!isScriptSubtag(code)) throwRange(code + ' is not a script code'); return titleCase(code); }
  if (type === 'calendar') {
    if (!isUnicodeType(code) || strIndexOf(code, '_') >= 0) throwRange(code + ' is not a calendar code');
    return toLowerCase(code);
  }
  if (type === 'dateTimeField') { if (!contains(DATE_TIME_FIELDS, code)) throwRange(code + ' is not a date and time field'); return code; }
  if (!isWellFormedCurrencyCode(code)) throwRange(code + ' is not a currency code');
  return toUpperCase(code);
}
class DisplayNames extends $Object {
  constructor(locales, options) {
    var o = create(protoFor(new.target, DisplayNames.prototype));
    var resolution = resolveOptions(locales, options, 'require', [], [], function () { return record({}); });
    options = resolution.options;
    var r = record({ kind: 'DisplayNames', locale: resolution.resolved.locale, dataLocale: resolution.resolved.dataLocale });
    r.style = getOption(options, 'style', 'string', ['narrow', 'short', 'long'], 'long');
    r.type = getOption(options, 'type', 'string', ['language', 'region', 'script', 'currency', 'calendar', 'dateTimeField'], REQUIRED);
    r.fallback = getOption(options, 'fallback', 'string', ['code', 'none'], 'code');
    var languageDisplay = getOption(options, 'languageDisplay', 'string', ['dialect', 'standard'], 'dialect');
    if (r.type === 'language') r.languageDisplay = languageDisplay;
    wmSet(slots, o, r);
    return o;
  }
  static supportedLocalesOf(locales) { return supportedLocalesOf(locales, arguments[1]); }
  of(code) {
    var r = slotOf(this, 'DisplayNames');
    code = canonicalCodeForDisplayNames(r.type, toString(code));
    var name = icu.displayName(r.dataLocale, r.type, code, r.style === 'long', r.languageDisplay === 'dialect');
    if (name !== undefined) return name;
    return r.fallback === 'code' ? code : undefined;
  }
  resolvedOptions() { return resolvedOptionsObject(slotOf(this, 'DisplayNames'), ['locale', 'style', 'type', 'fallback', 'languageDisplay']); }
}

// ---------------------------------------------------------------- Intl.Segmenter (19)
var SEGMENT_GRANULARITIES = ['grapheme', 'word', 'sentence'];
class Segmenter extends $Object {
  constructor() {
    var o = create(protoFor(new.target, Segmenter.prototype));
    var resolution = resolveOptions(arguments[0], arguments[1], false, [], [], function () { return record({}); });
    var r = record({ kind: 'Segmenter', locale: resolution.resolved.locale, dataLocale: resolution.resolved.dataLocale });
    r.granularity = getOption(resolution.options, 'granularity', 'string', SEGMENT_GRANULARITIES, 'grapheme');
    wmSet(slots, o, r);
    return o;
  }
  static supportedLocalesOf(locales) { return supportedLocalesOf(locales, arguments[1]); }
  segment(string) {
    var r = slotOf(this, 'Segmenter');
    string = toString(string);
    var segments = create(SegmentsProto);
    wmSet(slots, segments, record({ kind: 'Segments', segmenter: r, string: string, boundaries: undefined }));
    return segments;
  }
  resolvedOptions() { return resolvedOptionsObject(slotOf(this, 'Segmenter'), ['locale', 'granularity']); }
}
// The boundaries of the string, found once: [index, status, index, status, ...] from 0 to its length
function boundariesOf(s) {
  if (s.boundaries === undefined) s.boundaries = icu.segment(s.segmenter.dataLocale, arrayIndexOf(SEGMENT_GRANULARITIES, s.segmenter.granularity), s.string);
  return s.boundaries;
}
// FindBoundary (19.8.1): the position in the boundary list of the last boundary at or before index
function boundaryBefore(b, index) {
  var lo = 0, hi = b.length / 2 - 1;
  while (lo < hi) { var mid = (lo + hi + 1) >> 1; if (b[mid * 2] <= index) lo = mid; else hi = mid - 1; }
  return lo;
}
// CreateSegmentDataObject (19.7.1) for the segment that starts at boundary k
function segmentData(s, k) {
  var b = boundariesOf(s), start = b[k * 2], end = b[k * 2 + 2], result = {};
  createDataProperty(result, 'segment', strSlice(s.string, start, end));
  createDataProperty(result, 'index', start);
  createDataProperty(result, 'input', s.string);
  if (s.segmenter.granularity === 'word') createDataProperty(result, 'isWordLike', b[k * 2 + 3] >= 100);  // not UBRK_WORD_NONE
  return result;
}
var SegmentsProto = create($Object.prototype), SegmentIteratorProto;
defineMethods(SegmentsProto, {
  containing(index) {
    var s = slotOf(this, 'Segments'), n = +index;
    n = n !== n ? 0 : n < 0 ? -floor(-n) : floor(n);
    if (n < 0 || n >= s.string.length) return undefined;
    return segmentData(s, boundaryBefore(boundariesOf(s), n));
  }
});
(function () {  // %IntlSegmentsPrototype%[@@iterator], a method named [Symbol.iterator]
  var f = { [iteratorSymbol]() {
    var s = slotOf(this, 'Segments'), it = create(SegmentIteratorProto);
    wmSet(slots, it, record({ kind: 'SegmentIterator', segments: s, next: 0 }));
    return it;
  } }[iteratorSymbol];
  defineProperty(SegmentsProto, iteratorSymbol, { value: f, writable: true, enumerable: false, configurable: true });
})();
SegmentIteratorProto = create(getPrototypeOf(getPrototypeOf([][iteratorSymbol]())));
defineMethods(SegmentIteratorProto, {
  next() {
    var it = slotOf(this, 'SegmentIterator'), s = it.segments, result = {};
    if (it.next >= s.string.length) {
      createDataProperty(result, 'value', undefined);
      createDataProperty(result, 'done', true);
      return result;
    }
    var b = boundariesOf(s), k = boundaryBefore(b, it.next);
    createDataProperty(result, 'value', segmentData(s, k));
    createDataProperty(result, 'done', false);
    it.next = b[k * 2 + 2];
    return result;
  }
});
defineProperty(SegmentIteratorProto, symToStringTag, { value: 'Segmenter String Iterator', writable: false, enumerable: false, configurable: true });
