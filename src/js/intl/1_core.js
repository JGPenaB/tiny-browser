// Intl (ECMA-402, https://tc39.es/ecma402/): written in JavaScript on top of the engine, with locale data and the
// formatters themselves from ICU (natives.icu, icu.cpp). tools/gen_js_parts.py turns these files into intl_js.h; the
// engine compiles them the first time the global `Intl` is read (prelude.cpp intlNatives). Section numbers in
// comments are those of ECMA-402.
(function (natives) {
'use strict';
var icu = natives.icu, protoFor = natives.protoFor;
var P = natives.pristine;  // the built-ins as they were before any page script ran (prelude.cpp snapshotIntrinsics)
var $Object = P.Object.value, $TypeError = P.TypeError.value, $RangeError = P.RangeError.value, $Number = P.Number.value, $String = P.String.value,
    $Array = P.Array.value;
var $Symbol = P.Symbol.value, $Map = P.Map.value, $WeakMap = P.WeakMap.value, $Math = P.Math.statics;
var create = P.Object.statics.create, defineProperty = P.Object.statics.defineProperty, getOwnPropertyNames = P.Object.statics.getOwnPropertyNames,
    setPrototypeOf = P.Object.statics.setPrototypeOf, getOwnPropertyDescriptor = P.Object.statics.getOwnPropertyDescriptor, $ObjectCtor = $Object;
var floor = $Math.floor;
var reflectApply = P.Reflect.statics.apply, fnCall = P.Function.proto.call, fnBind = P.Function.proto.bind;
function uncurry(f) { return reflectApply(fnBind, fnCall, [f]); }  // uncurry(f)(self, ...args) is f.call(self, ...args)
var wmGet = uncurry(P.WeakMap.proto.get), wmSet = uncurry(P.WeakMap.proto.set);
var mapGet = uncurry(P.Map.proto.get), mapSet = uncurry(P.Map.proto.set), mapHas = uncurry(P.Map.proto.has);
var strSlice = uncurry(P.String.proto.slice), strIndexOf = uncurry(P.String.proto.indexOf), charCodeAt = uncurry(P.String.proto.charCodeAt),
    toLowerCase = uncurry(P.String.proto.toLowerCase), toUpperCase = uncurry(P.String.proto.toUpperCase),
    strSplit = uncurry(P.String.proto.split);
var arrayPush = uncurry(P.Array.proto.push), arraySort = uncurry(P.Array.proto.sort), arrayIndexOf = uncurry(P.Array.proto.indexOf),
    arrayJoin = uncurry(P.Array.proto.join), arraySlice = uncurry(P.Array.proto.slice);
var symToStringTag = P.Symbol.statics.toStringTag;

function throwRange(message) { throw new $RangeError(message); }
function throwType(message) { throw new $TypeError(message); }
function toObject(v) { if (v === undefined || v === null) throwType('Cannot convert undefined or null to object'); return $ObjectCtor(v); }
function isObject(x) { return (typeof x === 'object' && x !== null) || typeof x === 'function'; }
function toString(x) { if (typeof x === 'symbol') throwType('Cannot convert a Symbol value to a string'); return `${x}`; }
// Records the engine keeps for itself have no prototype, so a setter a page puts on Object.prototype never sees them
function record(o) { setPrototypeOf(o, null); return o; }
// CreateDataPropertyOrThrow, for the objects handed to the page
function createDataProperty(o, key, value) { defineProperty(o, key, { value: value, writable: true, enumerable: true, configurable: true }); }
function contains(list, x) { return arrayIndexOf(list, x) >= 0; }
// An array made by the engine for its own use: index writes cannot reach setters on Array.prototype
function list() { var a = []; setPrototypeOf(a, null); return a; }
function push(a, x) { a[a.length] = x; }
function toArray(items) {  // CreateArrayFromList
  var a = [];
  for (var i = 0; i < items.length; i++) defineProperty(a, i, { value: items[i], writable: true, enumerable: true, configurable: true });
  return a;
}
function sorted(items) { var copy = arraySlice(items); arraySort(copy); return copy; }

// ---------------------------------------------------------------- internal slots
var slots = new $WeakMap();
function slotOf(x, kind) {
  var r = isObject(x) ? wmGet(slots, x) : undefined;
  if (r === undefined || r.kind !== kind) throwType('this is not an Intl.' + kind);
  return r;
}
function kindOf(x) { var r = isObject(x) ? wmGet(slots, x) : undefined; return r === undefined ? undefined : r.kind; }
function copyAccessors(target, source) {
  var names = getOwnPropertyNames(source);
  for (var i = 0; i < names.length; i++) {
    var d = getOwnPropertyDescriptor(source, names[i]);
    defineProperty(target, names[i], { get: d.get, enumerable: false, configurable: true });
  }
}

// ---------------------------------------------------------------- options (9.2.10-9.2.15)
function coerceOptionsToObject(options) { return options === undefined ? create(null) : toObject(options); }
function getOptionsObject(options) {
  if (options === undefined) return create(null);
  if (isObject(options)) return options;
  throwType('Options must be an object');
}
// GetOption: type is 'boolean' or 'string'; fallback REQUIRED throws
var REQUIRED = {};
function getOption(options, property, type, values, fallback) {
  var value = options[property];
  if (value === undefined) {
    if (fallback === REQUIRED) throwType(property + ' is required');
    return fallback;
  }
  value = type === 'boolean' ? !!value : toString(value);
  if (values !== undefined && !contains(values, value)) throwRange(toString(value) + ' is not a valid value for ' + property);
  return value;
}

// ---------------------------------------------------------------- language tags (6.2)
function isAlphaCode(c) { return (c >= 97 && c <= 122) || (c >= 65 && c <= 90); }
function isDigitCode(c) { return c >= 48 && c <= 57; }
function allOf(s, test) { for (var i = 0; i < s.length; i++) if (!test(charCodeAt(s, i))) return false; return true; }
function isAlnumCode(c) { return isAlphaCode(c) || isDigitCode(c); }
function isAlpha(s, lo, hi) { return s.length >= lo && s.length <= hi && allOf(s, isAlphaCode); }
function isAlnum(s, lo, hi) { return s.length >= lo && s.length <= hi && allOf(s, isAlnumCode); }
function isLanguageSubtag(s) { return isAlpha(s, 2, 3) || isAlpha(s, 5, 8); }
function isScriptSubtag(s) { return isAlpha(s, 4, 4); }
function isRegionSubtag(s) { return isAlpha(s, 2, 2) || (s.length === 3 && allOf(s, isDigitCode)); }
function isVariantSubtag(s) { return isAlnum(s, 5, 8) || (s.length === 4 && isDigitCode(charCodeAt(s, 0)) && isAlnum(s, 4, 4)); }
// `type` (3.6.1): one or more alphanum{3,8} separated by "-"
function isUnicodeType(s) {
  var parts = strSplit(s, '-');
  for (var i = 0; i < parts.length; i++) if (!isAlnum(parts[i], 3, 8)) return false;
  return true;
}
function titleCase(s) { return toUpperCase(strSlice(s, 0, 1)) + toLowerCase(strSlice(s, 1)); }

// Parses a language tag that IsWellFormedLanguageTag (6.2.1) accepts into
//   { language, script, region, variants, extensions, privateUse }
// where extensions are { singleton: 'u', attributes, keywords: [[key, value]] }, { singleton: 't', tlang, fields },
// or { singleton, value }. Returns undefined for any other string. Subtags come out lowercase.
function parseLanguageId(parts, i, out) {
  if (i >= parts.length || !isLanguageSubtag(parts[i])) return -1;
  out.language = parts[i++];
  out.script = undefined; out.region = undefined; out.variants = list();
  if (i < parts.length && isScriptSubtag(parts[i])) out.script = parts[i++];
  if (i < parts.length && isRegionSubtag(parts[i])) out.region = parts[i++];
  while (i < parts.length && isVariantSubtag(parts[i])) {
    if (contains(out.variants, parts[i])) return -1;  // a variant twice
    push(out.variants, parts[i++]);
  }
  return i;
}
function parseTag(tag) {
  if (tag.length === 0 || !allOf(tag, function (c) { return isAlnumCode(c) || c === 45; })) return undefined;
  var parts = strSplit(toLowerCase(tag), '-');
  for (var p = 0; p < parts.length; p++) if (parts[p].length === 0 || parts[p].length > 8) return undefined;
  var result = record({ extensions: list(), privateUse: undefined });
  var i = parseLanguageId(parts, 0, result);
  if (i < 0) return undefined;
  var singletons = list();
  while (i < parts.length) {
    var singleton = parts[i];
    if (singleton.length !== 1) return undefined;
    if (singleton === 'x') {
      if (i + 1 >= parts.length) return undefined;
      result.privateUse = arrayJoin(arraySlice(parts, i + 1), '-');
      return result;
    }
    if (contains(singletons, singleton)) return undefined;
    push(singletons, singleton);
    i++;
    var start = i;
    while (i < parts.length && parts[i].length > 1) i++;
    var sub = arraySlice(parts, start, i);
    if (sub.length === 0) return undefined;
    var ext;
    if (singleton === 'u') ext = parseUnicodeExtension(sub);
    else if (singleton === 't') ext = parseTransformedExtension(sub);
    else {
      for (var k = 0; k < sub.length; k++) if (!isAlnum(sub[k], 2, 8)) return undefined;
      ext = record({ singleton: singleton, value: arrayJoin(sub, '-') });
    }
    if (ext === undefined) return undefined;
    push(result.extensions, ext);
  }
  return result;
}
function isUKey(s) { return s.length === 2 && isAlnumCode(charCodeAt(s, 0)) && isAlphaCode(charCodeAt(s, 1)); }
function parseUnicodeExtension(sub) {
  var ext = record({ singleton: 'u', attributes: list(), keywords: list() }), i = 0;
  while (i < sub.length && isAlnum(sub[i], 3, 8)) push(ext.attributes, sub[i++]);
  while (i < sub.length) {
    if (!isUKey(sub[i])) return undefined;
    var key = sub[i++], start = i;
    while (i < sub.length && isAlnum(sub[i], 3, 8)) i++;
    push(ext.keywords, [key, arrayJoin(arraySlice(sub, start, i), '-')]);
  }
  return ext;
}
function isTKey(s) { return s.length === 2 && isAlphaCode(charCodeAt(s, 0)) && isDigitCode(charCodeAt(s, 1)); }
function parseTransformedExtension(sub) {
  var ext = record({ singleton: 't', tlang: undefined, fields: list() }), i = 0;
  if (isLanguageSubtag(sub[0])) {
    ext.tlang = record({});
    i = parseLanguageId(sub, 0, ext.tlang);
    if (i < 0) return undefined;
  }
  while (i < sub.length) {
    if (!isTKey(sub[i])) return undefined;
    var key = sub[i++], start = i;
    while (i < sub.length && isAlnum(sub[i], 3, 8)) i++;
    if (i === start) return undefined;
    push(ext.fields, [key, arrayJoin(arraySlice(sub, start, i), '-')]);
  }
  return ext;
}
function isWellFormedLanguageTag(tag) { return parseTag(tag) !== undefined; }

// ---------------------------------------------------------------- canonicalization (6.2.2, UTS 35 Annex C)
var ALIAS, KEY_TYPES, LANGUAGE_RULES;
function aliasData() {
  if (ALIAS === undefined) ALIAS = icu.resource(undefined, 'metadata', 'alias');
  return ALIAS;
}
function keyTypeData() {
  if (KEY_TYPES === undefined) KEY_TYPES = icu.resource(undefined, 'keyTypeData');
  return KEY_TYPES;
}
// An ICU alias key or replacement such as "sgn_GR" or "und_hepburn_heploc" as a language id, or undefined when it
// has parts a Unicode locale identifier cannot hold (extlangs, private use)
function parseIcuId(id) {
  var parts = strSplit(id, '_'), r = { language: toLowerCase(parts[0]), script: undefined, region: undefined, variants: list() };
  for (var i = 1; i < parts.length; i++) {
    var p = parts[i];
    if (p === 'x') return undefined;
    if (r.script === undefined && r.region === undefined && r.variants.length === 0 && isScriptSubtag(p)) r.script = toLowerCase(p);
    else if (r.region === undefined && r.variants.length === 0 && isRegionSubtag(p)) r.region = toLowerCase(p);
    else if (isVariantSubtag(p)) push(r.variants, toLowerCase(p));
    else return undefined;
  }
  return r;
}
function languageRules() {
  if (LANGUAGE_RULES !== undefined) return LANGUAGE_RULES;
  LANGUAGE_RULES = list();
  var table = aliasData().language, keys = getOwnPropertyNames(table);
  for (var i = 0; i < keys.length; i++) {
    var from = parseIcuId(keys[i]), to = parseIcuId(strSplit(table[keys[i]].replacement, ' ')[0]);
    if (from === undefined || to === undefined) continue;
    // the more a rule's source says, the sooner it is tried (UTS 35 Annex C: the most specific rule wins)
    from.rank = from.variants.length * 8 + (from.region !== undefined ? 4 : 0) + (from.script !== undefined ? 2 : 0) + (from.language !== 'und' ? 1 : 0);
    push(LANGUAGE_RULES, { from: from, to: to });
  }
  arraySort(LANGUAGE_RULES, function (a, b) { return b.from.rank - a.from.rank; });
  return LANGUAGE_RULES;
}
function ruleMatches(from, id) {
  if (from.language !== 'und' && from.language !== id.language) return false;
  if (from.script !== undefined && from.script !== id.script) return false;
  if (from.region !== undefined && from.region !== id.region) return false;
  for (var i = 0; i < from.variants.length; i++) if (!contains(id.variants, from.variants[i])) return false;
  return true;
}
function likelyRegion(language, script) {
  var maximal = addLikelySubtags({ language: language, script: script, region: undefined });
  return maximal === undefined ? undefined : maximal.region;
}
// Add Likely Subtags and Remove Likely Subtags (UTS 35 4.3) on { language, script, region }: ICU looks the subtags up;
// the subtags the id had are kept, as the algorithm says
function addLikelySubtags(id) {
  var found = icu.likelySubtags(languageIdString({ language: id.language, script: id.script, region: id.region, variants: [] }), true);
  var parsed = found === undefined ? undefined : parseTag(found);
  if (parsed === undefined) return undefined;
  return { language: id.language !== 'und' ? id.language : parsed.language, script: id.script !== undefined ? id.script : parsed.script,
           region: id.region !== undefined ? id.region : parsed.region };
}
// Replaces the aliases in a language id { language, script, region, variants } (all lowercase) until none is left
function canonicalizeLanguageId(id) {
  var rules = languageRules(), data = aliasData(), changed = true, guard = 0;
  while (changed && guard++ < 100) {
    changed = false;
    for (var r = 0; r < rules.length; r++) {
      var from = rules[r].from, to = rules[r].to;
      if (!ruleMatches(from, id)) continue;
      var variants = list(), i;
      for (i = 0; i < id.variants.length; i++) if (!contains(from.variants, id.variants[i])) push(variants, id.variants[i]);
      for (i = 0; i < to.variants.length; i++) if (!contains(variants, to.variants[i])) push(variants, to.variants[i]);
      if (to.language !== 'und' || from.language !== 'und') id.language = to.language === 'und' ? id.language : to.language;
      if (to.script !== undefined && (id.script === undefined || from.script !== undefined)) id.script = to.script;
      else if (from.script !== undefined) id.script = undefined;
      if (to.region !== undefined && (id.region === undefined || from.region !== undefined)) id.region = to.region;
      else if (from.region !== undefined) id.region = undefined;
      id.variants = variants;
      changed = true;
      break;
    }
    if (changed) continue;
    if (id.script !== undefined) {
      var script = data.script[titleCase(id.script)];
      if (script !== undefined) { id.script = toLowerCase(script.replacement); changed = true; continue; }
    }
    if (id.region !== undefined) {
      var region = data.territory[toUpperCase(id.region)];
      if (region !== undefined) {
        var choices = strSplit(region.replacement, ' '), pick = choices[0];
        if (choices.length > 1) {
          var likely = likelyRegion(id.language, id.script);
          for (var c = 0; c < choices.length; c++) if (likely !== undefined && toLowerCase(choices[c]) === likely) pick = choices[c];
        }
        id.region = toLowerCase(pick);
        changed = true;
        continue;
      }
    }
    for (var v = 0; v < id.variants.length; v++) {
      var variant = data.variant[id.variants[v]];
      if (variant !== undefined) { id.variants[v] = toLowerCase(variant.replacement); changed = true; break; }
    }
  }
  arraySort(id.variants);
  return id;
}
function languageIdString(id) {
  var s = id.language;
  if (id.script !== undefined) s += '-' + titleCase(id.script);
  if (id.region !== undefined) s += '-' + toUpperCase(id.region);
  for (var i = 0; i < id.variants.length; i++) s += '-' + id.variants[i];
  return s;
}
// The ICU spelling of a BCP 47 key ("ca" is "calendar"), for its tables keyed that way
function legacyKey(key) {
  var map = keyTypeData().keyMap, names = getOwnPropertyNames(map);
  for (var i = 0; i < names.length; i++) if (map[names[i]] === key || (map[names[i]] === '' && names[i] === key)) return names[i];
  return key;
}
// CanonicalizeUValue (9.2.2): the canonical BCP 47 type for a key, through CLDR's aliases ("gregorian" is "gregory",
// "yes" is "true"); "true" itself is left out of a canonical tag, as the empty string
var LOWER_TABLES = new $WeakMap();
function lookupIgnoringCase(table, key) {  // ICU spells legacy types with capitals ("Eire"); BCP 47 types are lowercase
  if (table === undefined) return undefined;
  var lower = wmGet(LOWER_TABLES, table);
  if (lower === undefined) {
    lower = new $Map();
    var names = getOwnPropertyNames(table);
    for (var i = 0; i < names.length; i++) mapSet(lower, toLowerCase(names[i]), table[names[i]]);
    wmSet(LOWER_TABLES, table, lower);
  }
  return mapGet(lower, key);
}
function canonicalizeUValue(key, value) {
  value = toLowerCase(value);
  var data = keyTypeData(), legacy = legacyKey(key);
  var bcpAlias = data.bcpTypeAlias[key];
  if (bcpAlias !== undefined && bcpAlias[value] !== undefined) value = bcpAlias[value];
  if (key === 'tz') value = arrayJoin(strSplit(value, '/'), ':');  // ICU spells zone names with ":" in its tables
  var alias = lookupIgnoringCase(data.typeAlias[legacy], value);
  if (typeof alias === 'string') value = toLowerCase(key === 'tz' ? arrayJoin(strSplit(alias, '/'), ':') : alias);
  var mapped = lookupIgnoringCase(data.typeMap[legacy], value);
  if (typeof mapped === 'string' && mapped !== '' && value !== 'true') value = mapped;
  if ((key === 'rg' || key === 'sd') && aliasData().subdivision[value] !== undefined) value = strSplit(aliasData().subdivision[value].replacement, ' ')[0];
  return value === 'true' ? '' : value;
}
// The canonical Unicode extension: first of each attribute and keyword kept, both sorted (UTS 35 3.2.1)
function canonicalUnicodeExtension(ext) {
  var attributes = list(), keywords = list(), keys = list(), i;
  for (i = 0; i < ext.attributes.length; i++) if (!contains(attributes, ext.attributes[i])) push(attributes, ext.attributes[i]);
  for (i = 0; i < ext.keywords.length; i++) {
    var key = ext.keywords[i][0];
    if (contains(keys, key)) continue;
    push(keys, key);
    push(keywords, [key, canonicalizeUValue(key, ext.keywords[i][1])]);
  }
  arraySort(attributes);
  arraySort(keywords, function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; });
  return { singleton: 'u', attributes: attributes, keywords: keywords };
}
function unicodeExtensionString(ext) {
  var s = 'u', i;
  for (i = 0; i < ext.attributes.length; i++) s += '-' + ext.attributes[i];
  for (i = 0; i < ext.keywords.length; i++) s += '-' + ext.keywords[i][0] + (ext.keywords[i][1] !== '' ? '-' + ext.keywords[i][1] : '');
  return s;
}
function canonicalTransformedExtension(ext) {
  var fields = list(), keys = list();
  for (var i = 0; i < ext.fields.length; i++) {
    var key = ext.fields[i][0];
    if (contains(keys, key)) continue;
    push(keys, key);
    var value = canonicalizeUValue(key, ext.fields[i][1]);
    push(fields, [key, value === '' ? 'true' : value]);
  }
  arraySort(fields, function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; });
  return { singleton: 't', tlang: ext.tlang === undefined ? undefined : canonicalizeLanguageId(ext.tlang), fields: fields };
}
function extensionString(ext) {
  if (ext.singleton === 'u') return unicodeExtensionString(ext);
  if (ext.singleton === 't') {
    var s = 't';
    if (ext.tlang !== undefined) s += '-' + toLowerCase(languageIdString(ext.tlang));
    for (var i = 0; i < ext.fields.length; i++) s += '-' + ext.fields[i][0] + '-' + ext.fields[i][1];
    return s;
  }
  return ext.singleton + '-' + ext.value;
}
function canonicalizeParsed(parsed) {
  canonicalizeLanguageId(parsed);
  var extensions = list();
  for (var i = 0; i < parsed.extensions.length; i++) {
    var ext = parsed.extensions[i];
    if (ext.singleton === 'u') ext = canonicalUnicodeExtension(ext);
    else if (ext.singleton === 't') ext = canonicalTransformedExtension(ext);
    if (ext.singleton === 'u' && ext.attributes.length === 0 && ext.keywords.length === 0) continue;
    push(extensions, ext);
  }
  arraySort(extensions, function (a, b) { return a.singleton < b.singleton ? -1 : 1; });
  parsed.extensions = extensions;
  return parsed;
}
function tagString(parsed) {
  var s = languageIdString(parsed);
  for (var i = 0; i < parsed.extensions.length; i++) s += '-' + extensionString(parsed.extensions[i]);
  if (parsed.privateUse !== undefined) s += '-x-' + parsed.privateUse;
  return s;
}
// CanonicalizeUnicodeLocaleId (6.2.2); tag is well-formed
function canonicalizeLocaleId(tag) { return tagString(canonicalizeParsed(parseTag(tag))); }

// ---------------------------------------------------------------- tag parts (15.5)
function unicodeExtensionOf(parsed) {
  for (var i = 0; i < parsed.extensions.length; i++) if (parsed.extensions[i].singleton === 'u') return parsed.extensions[i];
  return undefined;
}
function withoutUnicodeExtension(parsed) {
  var copy = { language: parsed.language, script: parsed.script, region: parsed.region, variants: parsed.variants, extensions: list(), privateUse: parsed.privateUse };
  for (var i = 0; i < parsed.extensions.length; i++) if (parsed.extensions[i].singleton !== 'u') push(copy.extensions, parsed.extensions[i]);
  return copy;
}
// InsertUnicodeExtensionAndCanonicalize (14.5.? / 9.2.?): keywords are [[key, value]]
function insertUnicodeExtensionAndCanonicalize(locale, attributes, keywords) {
  var parsed = withoutUnicodeExtension(parseTag(locale));
  push(parsed.extensions, { singleton: 'u', attributes: attributes, keywords: keywords });
  return tagString(canonicalizeParsed(parsed));
}
function keywordValue(parsed, key) {
  var ext = unicodeExtensionOf(parsed);
  if (ext === undefined) return undefined;
  for (var i = 0; i < ext.keywords.length; i++) if (ext.keywords[i][0] === key) return ext.keywords[i][1];
  return undefined;
}
function baseNameOf(locale) { return languageIdString(parseTag(locale)); }

// ---------------------------------------------------------------- locale lists and negotiation (9.2)
// CanonicalizeLocaleList (9.2.1)
function canonicalizeLocaleList(locales) {
  var seen = list();
  if (locales === undefined) return seen;
  var o;
  if (typeof locales === 'string' || kindOf(locales) === 'Locale') { o = list(); push(o, locales); }
  else o = toObject(locales);
  var length = toLength(o.length);
  for (var k = 0; k < length; k++) {
    if (!(k in o)) continue;
    var element = o[k], tag;
    if (typeof element !== 'string' && !isObject(element)) throwType('A locale must be a string or an object');
    if (kindOf(element) === 'Locale') tag = slotOf(element, 'Locale').locale;
    else tag = toString(element);
    if (!isWellFormedLanguageTag(tag)) throwRange(tag + ' is not a well-formed language tag');
    var canonical = canonicalizeLocaleId(tag);
    if (!contains(seen, canonical)) push(seen, canonical);
  }
  return seen;
}
function toLength(v) {
  var n = +v;
  if (n !== n || n <= 0) return 0;
  n = floor(n);
  return n > 9007199254740991 ? 9007199254740991 : n;
}
// The locales ICU has data for, with each one's shortest form too ("zh-Hant-TW" is also "zh-TW")
var AVAILABLE;
function availableLocales() {
  if (AVAILABLE !== undefined) return AVAILABLE;
  AVAILABLE = new $Map();
  var tags = icu.availableLocales();
  for (var i = 0; i < tags.length; i++) {
    if (!isWellFormedLanguageTag(tags[i])) continue;
    var tag = canonicalizeLocaleId(tags[i]);
    mapSet(AVAILABLE, tag, true);
    var minimal = icu.likelySubtags(tag, false);
    if (minimal !== undefined && isWellFormedLanguageTag(minimal)) mapSet(AVAILABLE, canonicalizeLocaleId(minimal), true);
  }
  mapSet(AVAILABLE, 'en-US', true);
  return AVAILABLE;
}
function defaultLocale() { return 'en-US'; }
// LookupMatchingLocaleByPrefix (9.2.3); the best-fit matcher (9.2.4) is the same lookup over the same list
function lookupMatchingLocale(requested) {
  var available = availableLocales();
  for (var i = 0; i < requested.length; i++) {
    var parsed = parseTag(requested[i]), extension = unicodeExtensionOf(parsed);
    var prefix = tagString(withoutUnicodeExtension(parsed));
    while (prefix !== '') {
      if (mapHas(available, prefix)) return { locale: prefix, extension: extension };
      var pos = prefix.length - 1;
      while (pos > 0 && prefix[pos] !== '-') pos--;
      while (pos >= 2 && prefix[pos - 2] === '-') pos -= 2;
      prefix = strSlice(prefix, 0, pos);
    }
  }
  return undefined;
}
// ResolveLocale (9.2.7). localeData(locale) gives, for each key, the list of supported values, default first.
function resolveLocale(requested, options, relevantKeys, localeData) {
  var match = lookupMatchingLocale(requested);
  if (match === undefined) match = { locale: defaultLocale(), extension: undefined };
  var foundLocale = match.locale, data = localeData === undefined ? undefined : localeData(foundLocale);
  var ufields = match.extension === undefined ? list() : match.extension.keywords;
  var retained = list(), result = record({ dataLocale: foundLocale, localeData: data });
  for (var i = 0; i < relevantKeys.length; i++) {
    var key = relevantKeys[i], supported = data[key], value = supported[0], retainedField;
    for (var j = 0; j < ufields.length; j++) {
      if (ufields[j][0] !== key) continue;
      var uvalue = ufields[j][1] === '' ? 'true' : ufields[j][1];
      if (contains(supported, uvalue)) { value = uvalue; retainedField = ufields[j]; }
      break;
    }
    var optionsValue = options[key];
    if (typeof optionsValue === 'string') {
      optionsValue = canonicalizeUValue(key, optionsValue);
      if (optionsValue === '') optionsValue = 'true';
    }
    if (optionsValue !== value && contains(supported, optionsValue)) { value = optionsValue; retainedField = undefined; }
    if (retainedField !== undefined) push(retained, retainedField);
    result[key] = value;
  }
  if (retained.length !== 0) foundLocale = insertUnicodeExtensionAndCanonicalize(foundLocale, list(), retained);
  result.locale = foundLocale;
  return result;
}
// FilterLocales (9.2.9), for supportedLocalesOf
function filterLocales(requested, options) {
  options = coerceOptionsToObject(options);
  getOption(options, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit');
  var subset = list();
  for (var i = 0; i < requested.length; i++) {
    var one = list();
    push(one, requested[i]);
    if (lookupMatchingLocale(one) !== undefined) push(subset, requested[i]);
  }
  return toArray(subset);
}
function supportedLocalesOf(locales, options) { return filterLocales(canonicalizeLocaleList(locales), options); }
