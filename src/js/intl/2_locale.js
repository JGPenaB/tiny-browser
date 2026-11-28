// ---------------------------------------------------------------- Intl.Locale (15)
var LocaleProto;
var LOCALE_KEYS = ['ca', 'co', 'fw', 'hc', 'kf', 'kn', 'nu'];
var WEEKDAYS = ['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun'];
function weekdayToUValue(fw) { var n = arrayIndexOf(['0', '1', '2', '3', '4', '5', '6', '7'], fw); return n < 0 ? fw : WEEKDAYS[n]; }
function weekdayUValueToNumber(fw) { var n = arrayIndexOf(WEEKDAYS, fw); return n < 0 ? undefined : n === 0 ? 7 : n; }
function localeOf(x) { return slotOf(x, 'Locale'); }
function checkType(value) { if (value !== undefined && !isUnicodeType(value)) throwRange(value + ' is not a valid Unicode type'); }

// UpdateLanguageId (15.1.2)
function updateLanguageId(tag, options) {
  var parsed = parseTag(tag);
  var language = getOption(options, 'language', 'string', undefined, parsed.language);
  if (!isLanguageSubtag(language)) throwRange(language + ' is not a language subtag');
  var script = getOption(options, 'script', 'string', undefined, parsed.script === undefined ? undefined : titleCase(parsed.script));
  if (script !== undefined && !isScriptSubtag(script)) throwRange(script + ' is not a script subtag');
  var region = getOption(options, 'region', 'string', undefined, parsed.region === undefined ? undefined : toUpperCase(parsed.region));
  if (region !== undefined && !isRegionSubtag(region)) throwRange(region + ' is not a region subtag');
  var variants = getOption(options, 'variants', 'string', undefined, parsed.variants.length === 0 ? undefined : arrayJoin(parsed.variants, '-'));
  if (variants !== undefined) {
    if (variants === '') throwRange('variants cannot be empty');
    var subtags = strSplit(toLowerCase(variants), '-'), seen = list();
    for (var i = 0; i < subtags.length; i++) {
      if (!isVariantSubtag(subtags[i]) || contains(seen, subtags[i])) throwRange(variants + ' are not valid variants');
      push(seen, subtags[i]);
    }
  }
  var rest = { language: '', script: undefined, region: undefined, variants: list(), extensions: parsed.extensions, privateUse: parsed.privateUse };
  var newTag = language + (script !== undefined ? '-' + script : '') + (region !== undefined ? '-' + region : '') + (variants !== undefined ? '-' + variants : '');
  return newTag + strSlice(tagString(rest), 0);
}
// MakeLocaleRecord (15.1.3)
function makeLocaleRecord(tag, opt) {
  var parsed = parseTag(tag), ext = unicodeExtensionOf(parsed);
  var attributes = ext === undefined ? list() : ext.attributes, ufields = list(), result = {}, i;
  if (ext !== undefined) for (i = 0; i < ext.keywords.length; i++) push(ufields, [ext.keywords[i][0], ext.keywords[i][1]]);
  for (i = 0; i < LOCALE_KEYS.length; i++) {
    var key = LOCALE_KEYS[i], field, value;
    for (var j = 0; j < ufields.length; j++) if (ufields[j][0] === key) { field = ufields[j]; break; }
    value = field === undefined ? undefined : field[1];
    var override = opt[key];
    if (override !== undefined) {
      value = canonicalizeUValue(key, override);
      if (field !== undefined) field[1] = value;
      else push(ufields, [key, value]);
    }
    result[key] = value;
    field = undefined;
  }
  var bare = tagString(withoutUnicodeExtension(parsed));
  result.locale = attributes.length !== 0 || ufields.length !== 0 ? insertUnicodeExtensionAndCanonicalize(bare, attributes, ufields) : canonicalizeLocaleId(bare);
  return result;
}
function createLocale(tag, options, newTarget) {
  var o = create(newTarget === undefined ? LocaleProto : protoFor(newTarget, LocaleProto));
  if (typeof tag !== 'string' && !isObject(tag)) throwType('A locale must be a string or an object');
  tag = kindOf(tag) === 'Locale' ? localeOf(tag).locale : toString(tag);
  options = coerceOptionsToObject(options);
  if (!isWellFormedLanguageTag(tag)) throwRange(tag + ' is not a well-formed language tag');
  tag = canonicalizeLocaleId(tag);
  tag = updateLanguageId(tag, options);
  var opt = {};
  opt.ca = getOption(options, 'calendar', 'string', undefined, undefined); checkType(opt.ca);
  opt.co = getOption(options, 'collation', 'string', undefined, undefined); checkType(opt.co);
  var fw = getOption(options, 'firstDayOfWeek', 'string', undefined, undefined);
  if (fw !== undefined) { fw = weekdayToUValue(fw); checkType(fw); }
  opt.fw = fw;
  opt.hc = getOption(options, 'hourCycle', 'string', ['h11', 'h12', 'h23', 'h24'], undefined);
  opt.kf = getOption(options, 'caseFirst', 'string', ['upper', 'lower', 'false'], undefined);
  var kn = getOption(options, 'numeric', 'boolean', undefined, undefined);
  opt.kn = kn === undefined ? undefined : toString(kn);
  opt.nu = getOption(options, 'numberingSystem', 'string', undefined, undefined); checkType(opt.nu);
  var r = makeLocaleRecord(tag, opt);
  wmSet(slots, o, { kind: 'Locale', locale: r.locale, calendar: r.ca, collation: r.co, firstDayOfWeek: r.fw, hourCycle: r.hc,
                    caseFirst: r.kf, numeric: r.kn === undefined ? false : r.kn === 'true' || r.kn === '', numberingSystem: r.nu });
  return o;
}
function newLocale(tag) { return createLocale(tag, undefined, undefined); }
// RegionPreference (15.5.8)
function canonicalUnicodeSubdivision(parsed, key) {
  var value = keywordValue(parsed, key);
  if (value === undefined || value.length < 3) return undefined;
  var region = isDigitCode(charCodeAt(value, 0)) ? strSlice(value, 0, 3) : strSlice(value, 0, 2);
  return isRegionSubtag(region) ? toUpperCase(region) : undefined;
}
function regionPreference(locale) {
  var parsed = parseTag(locale), region = parsed.region === undefined ? undefined : toUpperCase(parsed.region);
  if (region === undefined) {
    region = canonicalUnicodeSubdivision(parsed, 'sd');
    if (region === undefined) {
      var likely = likelyRegion(parsed.language, parsed.script);
      region = likely === undefined ? '001' : toUpperCase(likely);
    }
  }
  return { region: region, override: canonicalUnicodeSubdivision(parsed, 'rg') };
}
function preferredRegions(locale) {
  var p = regionPreference(locale), regions = list();
  if (p.override !== undefined) push(regions, p.override);
  push(regions, p.region);
  return regions;
}
function supplemental(path) { return icu.resource(undefined, 'supplementalData', path); }
// CalendarsOfLocale (15.5.9)
function calendarsOfLocale(r) {
  if (r.calendar !== undefined) return toArray([r.calendar]);
  var regions = preferredRegions(r.locale), inUse, out = list();
  for (var i = 0; i < regions.length && inUse === undefined; i++) inUse = supplemental('calendarPreferenceData/' + regions[i]);
  if (inUse === undefined) inUse = supplemental('calendarPreferenceData/001');
  var available = availableCalendars();
  for (var j = 0; inUse !== undefined && j < inUse.length; j++) {
    var canonical = canonicalizeUValue('ca', inUse[j]);
    if (contains(available, canonical) && !contains(out, canonical)) push(out, canonical);
  }
  if (out.length === 0) push(out, 'gregory');
  return toArray(out);
}
// The collations a locale has in ICU, as BCP 47 types (CollationsOfLocale, 15.5.10)
function collationsOfLocale(r) {
  if (r.collation !== undefined) return toArray([r.collation]);
  var table = icu.resource('coll', toIcuId(baseNameOf(r.locale)), 'collations'), out = list();
  var names = table === undefined ? [] : getOwnPropertyNames(table);
  for (var i = 0; i < names.length; i++) {
    var name = names[i];
    if (name === 'default' || strSlice(name, 0, 8) === 'private-') continue;
    var type = canonicalizeUValue('co', name);
    if (type === 'standard' || type === 'search' || contains(out, type)) continue;
    push(out, type);
  }
  if (!contains(out, 'emoji')) push(out, 'emoji');
  if (!contains(out, 'eor')) push(out, 'eor');
  return toArray(sorted(out));
}
// HourCyclesOfLocale (15.5.11), from CLDR's time data ("h" is h12, "H" h23, "K" h11, "k" h24)
function hourCyclesOfLocale(r) {
  if (r.hourCycle !== undefined) return toArray([r.hourCycle]);
  var regions = preferredRegions(r.locale), language = parseTag(r.locale).language, data;
  for (var i = 0; i < regions.length && data === undefined; i++) {
    data = supplemental('timeData/' + language + '_' + regions[i]);
    if (data === undefined) data = supplemental('timeData/' + regions[i]);
  }
  var out = list();
  if (data !== undefined) {
    var tokens = strSplit(data.preferred + ' ' + data.allowed, ' ');
    for (var t = 0; t < tokens.length; t++) {
      var c = tokens[t][0], cycle = c === 'h' ? 'h12' : c === 'H' ? 'h23' : c === 'K' ? 'h11' : c === 'k' ? 'h24' : undefined;
      if (cycle !== undefined && !contains(out, cycle)) push(out, cycle);
    }
  }
  if (out.length === 0) push(out, 'h23');
  return toArray(out);
}
// NumberingSystemsOfLocale (15.5.12)
function numberingSystemsOfLocale(r) {
  if (r.numberingSystem !== undefined) return toArray([r.numberingSystem]);
  return toArray([defaultNumberingSystem(baseNameOf(r.locale))]);
}
function defaultNumberingSystem(locale) {
  var nu = icu.resource('', toIcuId(locale), 'NumberElements/default');
  return typeof nu === 'string' ? nu : 'latn';
}
// TimeZonesOfLocale (15.5.13)
function timeZonesOfLocale(r) {
  var parsed = parseTag(r.locale);
  if (parsed.region === undefined) return undefined;
  var ids = icu.tzIds(toUpperCase(parsed.region)), out = list();
  for (var i = 0; i < ids.length; i++) {
    var zone = namedZone(ids[i]);
    if (zone !== undefined && zone.primary !== 'UTC' && !contains(out, zone.primary)) push(out, zone.primary);
  }
  return toArray(sorted(out));
}
// TextDirectionOfLocale (15.5.14): from the Unicode script properties ICU has
function textDirectionOfLocale(r) {
  var parsed = parseTag(r.locale), script = parsed.script;
  if (script === undefined) {
    var maximal = addLikelySubtags(parsed);
    if (maximal === undefined || maximal.script === undefined) return undefined;
    script = maximal.script;
  }
  script = titleCase(script);
  if (script === 'Brai') return undefined;  // CLDR's script metadata gives Braille no direction
  return icu.scriptDirection(script);
}
// WeekInfoOfLocale (15.5.17). ICU's week data: first day (1 is Sunday), minimal days, weekend start and end days.
function weekInfoOfLocale(r) {
  var p = regionPreference(r.locale), data;
  if (p.override !== undefined) data = supplemental('weekData/' + p.override);
  if (data === undefined) data = supplemental('weekData/' + p.region);
  if (data === undefined) data = supplemental('weekData/001');
  var iso = function (d) { return d === 1 ? 7 : d - 1; };
  var firstDay = iso(data[0]), weekend = list();
  for (var d = data[2], n = 0; n < 7; n++, d = d % 7 + 1) {
    push(weekend, iso(d));
    if (d === data[4]) break;
  }
  arraySort(weekend, function (a, b) { return a - b; });
  if (r.firstDayOfWeek !== undefined) {
    var fw = weekdayUValueToNumber(r.firstDayOfWeek);
    if (fw !== undefined) firstDay = fw;
  }
  return { firstDay: firstDay, weekend: weekend };
}
// ICU's own spelling of a language id ("zh-Hant-TW" is "zh_Hant_TW")
function toIcuId(baseName) { return arrayJoin(strSplit(baseName, '-'), '_'); }

class Locale extends $Object {
  constructor(tag) { return createLocale(tag, arguments[1], new.target); }
  get baseName() { return baseNameOf(localeOf(this).locale); }
  get calendar() { return localeOf(this).calendar; }
  get caseFirst() { return localeOf(this).caseFirst; }
  get collation() { return localeOf(this).collation; }
  get firstDayOfWeek() { return localeOf(this).firstDayOfWeek; }
  get hourCycle() { return localeOf(this).hourCycle; }
  get language() { return parseTag(localeOf(this).locale).language; }
  get numberingSystem() { return localeOf(this).numberingSystem; }
  get numeric() { return localeOf(this).numeric; }
  get region() { var r = parseTag(localeOf(this).locale).region; return r === undefined ? undefined : toUpperCase(r); }
  get script() { var s = parseTag(localeOf(this).locale).script; return s === undefined ? undefined : titleCase(s); }
  get variants() { var v = parseTag(localeOf(this).locale).variants; return v.length === 0 ? undefined : arrayJoin(v, '-'); }
  maximize() {
    var r = localeOf(this), parsed = parseTag(r.locale), maximal = addLikelySubtags(parsed);
    if (maximal !== undefined) { parsed.language = maximal.language; parsed.script = maximal.script; parsed.region = maximal.region; }
    return newLocale(tagString(parsed));
  }
  minimize() {
    var r = localeOf(this), parsed = parseTag(r.locale), maximal = addLikelySubtags(parsed);
    if (maximal !== undefined) {  // the shortest of language, language-region, language-script that maximizes the same
      var tries = [{ language: maximal.language }, { language: maximal.language, region: maximal.region }, { language: maximal.language, script: maximal.script }];
      for (var i = 0; i < tries.length; i++) {
        var t = addLikelySubtags(tries[i]);
        if (t !== undefined && t.language === maximal.language && t.script === maximal.script && t.region === maximal.region) {
          parsed.language = tries[i].language; parsed.script = tries[i].script; parsed.region = tries[i].region;
          break;
        }
      }
    }
    return newLocale(tagString(parsed));
  }
  toString() { return localeOf(this).locale; }
  getCalendars() { return calendarsOfLocale(localeOf(this)); }
  getCollations() { return collationsOfLocale(localeOf(this)); }
  getHourCycles() { return hourCyclesOfLocale(localeOf(this)); }
  getNumberingSystems() { return numberingSystemsOfLocale(localeOf(this)); }
  getTimeZones() { return timeZonesOfLocale(localeOf(this)); }
  getTextInfo() { var info = {}; info.direction = textDirectionOfLocale(localeOf(this)); return info; }
  getWeekInfo() {
    var w = weekInfoOfLocale(localeOf(this)), info = {};
    info.firstDay = w.firstDay;
    info.weekend = toArray(w.weekend);
    return info;
  }
}
LocaleProto = Locale.prototype;

// ---------------------------------------------------------------- supportedValuesOf (8.3.2)
var UNITS = ['acre', 'bit', 'byte', 'celsius', 'centimeter', 'day', 'degree', 'fahrenheit', 'fluid-ounce', 'foot', 'gallon', 'gigabit',
             'gigabyte', 'gram', 'hectare', 'hour', 'inch', 'kilobit', 'kilobyte', 'kilogram', 'kilometer', 'liter', 'megabit', 'megabyte',
             'meter', 'microsecond', 'mile', 'mile-scandinavian', 'milliliter', 'millimeter', 'millisecond', 'minute', 'month',
             'nanosecond', 'ounce', 'percent', 'petabyte', 'pound', 'second', 'stone', 'terabit', 'terabyte', 'week', 'yard', 'year'];
function typeNames(legacy) {  // the BCP 47 types of a key, as ICU lists them
  var map = keyTypeData().typeMap[legacy], names = getOwnPropertyNames(map), out = list();
  for (var i = 0; i < names.length; i++) {
    var bcp = map[names[i]] !== '' ? map[names[i]] : names[i];
    if (!contains(out, bcp)) push(out, bcp);
  }
  return out;
}
var CALENDARS;
function availableCalendars() {
  if (CALENDARS === undefined) {
    // "islamic" and "islamic-rgsa" are calculated by sighting and have no fixed rules: the Intl era and month code
    // proposal leaves them out
    var names = typeNames('calendar'), out = list();
    for (var i = 0; i < names.length; i++) if (names[i] !== 'islamic' && names[i] !== 'islamic-rgsa') push(out, names[i]);
    CALENDARS = sorted(out);
  }
  return CALENDARS;
}
function availableCollations() {
  var out = list(), names = typeNames('collation');
  for (var i = 0; i < names.length; i++) if (names[i] !== 'standard' && names[i] !== 'search') push(out, names[i]);
  return sorted(out);
}
function availableCurrencies() {
  var names = getOwnPropertyNames(keyTypeData().typeMap.currency), out = list();
  for (var i = 0; i < names.length; i++) if (names[i].length === 3) push(out, toUpperCase(names[i]));
  return sorted(out);
}
var NUMBERING_SYSTEMS;
function availableNumberingSystems() {  // the ones with digits of their own, as Table 30 lists them
  if (NUMBERING_SYSTEMS !== undefined) return NUMBERING_SYSTEMS;
  var table = icu.resource(undefined, 'numberingSystems', 'numberingSystems'), names = getOwnPropertyNames(table), out = list();
  for (var i = 0; i < names.length; i++) if (table[names[i]].algorithmic === 0) push(out, names[i]);
  NUMBERING_SYSTEMS = sorted(out);
  return NUMBERING_SYSTEMS;
}
function supportedValuesOf(key) {
  key = toString(key);
  var values;
  if (key === 'calendar') {
    values = list();
    var calendars = availableCalendars();
    for (var i = 0; i < calendars.length; i++) if (canonicalizeUValue('ca', calendars[i]) === calendars[i]) push(values, calendars[i]);
  } else if (key === 'collation') values = availableCollations();
  else if (key === 'currency') values = availableCurrencies();
  else if (key === 'numberingSystem') values = availableNumberingSystems();
  else if (key === 'timeZone') values = primaryZoneIds();
  else if (key === 'unit') values = UNITS;
  else throwRange(key + ' is not a key of supportedValuesOf');
  return toArray(values);
}
