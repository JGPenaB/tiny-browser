// The bridge to ICU, the Unicode library that ships with Windows 10 (1903) and later as icu.dll. Intl and the
// named time zones of Temporal are written in JavaScript; they get their locale data and time zone rules from the
// natives made here. Only plain data crosses the bridge: strings, numbers and arrays.
#include "builtins.h"
#include "icu_bridge.h"
#include <windows.h>
#include <icu.h>
#include <cstring>
#include <unordered_map>

namespace tb::js {
namespace {

std::u16string fromUChars(const UChar* s, int32_t n) { return std::u16string((const char16_t*)s, (size_t)n); }
std::u16string fromAsciiKey(const std::string& s) { return std::u16string(s.begin(), s.end()); }
std::string toAscii(const std::u16string& s) {  // locale ids and resource keys are ASCII
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) out += c < 128 ? (char)c : '?';
    return out;
}
// A resource bundle as plain values: tables become objects, arrays arrays, strings strings, integers numbers.
Value dumpResource(Vm& vm, UResourceBundle* r, std::vector<Value>& held) {
    UErrorCode err = U_ZERO_ERROR;
    switch (ures_getType(r)) {
        case URES_STRING: case URES_ALIAS: {
            int32_t n = 0;
            const UChar* s = ures_getString(r, &n, &err);
            return U_FAILURE(err) ? Value::undef() : V(vm.str(fromUChars(s, n)));
        }
        case URES_INT: return Value::num(ures_getInt(r, &err));
        case URES_INT_VECTOR: {
            int32_t n = 0;
            const int32_t* v = ures_getIntVector(r, &n, &err);
            JSArray* a = vm.newArray();
            held.push_back(V(a));
            for (int32_t i = 0; U_SUCCESS(err) && i < n; i++) a->elems.push_back(Value::num(v[i]));
            return V(a);
        }
        case URES_TABLE: case URES_ARRAY: {
            bool table = ures_getType(r) == URES_TABLE;
            JSObject* o = table ? vm.newObject() : vm.newArray();
            held.push_back(V(o));
            int32_t n = ures_getSize(r);  // by index: the iterator can revisit an alias without end
            for (int32_t i = 0; i < n; i++) {
                UErrorCode e = U_ZERO_ERROR;
                UResourceBundle* child = ures_getByIndex(r, i, nullptr, &e);
                if (U_FAILURE(e)) continue;
                const char* key = ures_getKey(child);
                Value v = dumpResource(vm, child, held);
                held.push_back(v);
                if (!table) ((JSArray*)o)->elems.push_back(v);
                else if (key) {  // through toKey: a key such as "810" is an index
                    PropDesc d;
                    d.value = v;
                    d.hasValue = d.hasWritable = d.hasEnumerable = d.hasConfigurable = true;
                    d.writable = d.enumerable = d.configurable = true;
                    vm.defineOwnDesc(o, vm.toKey(V(vm.str(std::string_view(key)))), d);
                }
                ures_close(child);
            }
            return V(o);
        }
        default: return Value::undef();
    }
}
// resource(pkg, bundle, path): one of ICU's data tables as plain values. pkg is undefined for the root tables (such
// as "metadata", "supplementalData", "keyTypeData") or names a locale data tree ("coll", "curr", "lang", "region",
// "unit", "zone"; bundle is then a locale id, with fallback to its parents). path is "/"-separated keys into it.
JSFN(resource) {
    Vm& vm = ci.vm;
    UErrorCode err = U_ZERO_ERROR;
    std::string bundle = toAscii(vm.toString(ci.arg(1))->s);
    UResourceBundle* r;
    if (ci.arg(0).isUndef()) r = ures_openDirect(nullptr, bundle.c_str(), &err);
    else {
        UVersionInfo version;
        u_getVersion(version);
        std::string pkg = "icudt" + std::to_string(version[0]) + "l-" + toAscii(vm.toString(ci.arg(0))->s);
        if (pkg.back() == '-') pkg.pop_back();  // "": the main locale tree
        r = ures_open(pkg.c_str(), bundle.c_str(), &err);
        // ICU falls back to the computer's own locale for a locale it has no data for: use the root data instead
        UErrorCode e = U_ZERO_ERROR;
        const char* actual = U_SUCCESS(err) ? ures_getLocaleByType(r, ULOC_ACTUAL_LOCALE, &e) : nullptr;
        if (actual && strcmp(actual, "root") != 0 && bundle.compare(0, strlen(actual), actual) != 0) {
            ures_close(r);
            err = U_ZERO_ERROR;
            r = ures_open(pkg.c_str(), "root", &err);
        }
    }
    if (U_FAILURE(err)) return Value::undef();
    if (!ci.arg(2).isUndef()) {
        std::string path = toAscii(vm.toString(ci.arg(2))->s);
        size_t start = 0;
        while (start <= path.size()) {
            size_t end = path.find('/', start);
            if (end == std::string::npos) end = path.size();
            std::string key = path.substr(start, end - start);
            UResourceBundle* child = ures_getByKey(r, key.c_str(), nullptr, &err);
            ures_close(r);
            r = child;
            if (U_FAILURE(err)) {
                ures_close(r);
                return Value::undef();
            }
            start = end + 1;
        }
    }
    std::vector<Value> held;
    RootScope keep(vm, held);
    Value v = dumpResource(vm, r, held);
    ures_close(r);
    return v;
}

// likelySubtags(tag, maximize): Add Likely Subtags or Remove Likely Subtags (UTS 35) on a language-script-region tag
JSFN(likelySubtags) {
    Vm& vm = ci.vm;
    std::string tag = toAscii(vm.toString(ci.arg(0))->s);
    char id[ULOC_FULLNAME_CAPACITY], out[ULOC_FULLNAME_CAPACITY], bcp[ULOC_FULLNAME_CAPACITY];
    UErrorCode err = U_ZERO_ERROR;
    int32_t parsed = 0;
    uloc_forLanguageTag(tag.c_str(), id, sizeof id, &parsed, &err);
    if (vm.toBool(ci.arg(1))) uloc_addLikelySubtags(id, out, sizeof out, &err);
    else uloc_minimizeSubtags(id, out, sizeof out, &err);
    uloc_toLanguageTag(out, bcp, sizeof bcp, true, &err);
    if (U_FAILURE(err)) return Value::undef();
    return V(vm.str(std::string_view(bcp)));
}

// availableLocales(): the locales ICU has data for, as BCP 47 tags
JSFN(availableLocales) {
    Vm& vm = ci.vm;
    JSArray* a = vm.newArray();
    std::vector<Value> held{V(a)};
    RootScope keep(vm, held);
    int32_t n = uloc_countAvailable();
    for (int32_t i = 0; i < n; i++) {
        char bcp[ULOC_FULLNAME_CAPACITY];
        UErrorCode err = U_ZERO_ERROR;
        uloc_toLanguageTag(uloc_getAvailable(i), bcp, sizeof bcp, true, &err);
        if (U_SUCCESS(err)) a->elems.push_back(V(vm.str(std::string_view(bcp))));
    }
    return V(a);
}

// scriptDirection(code): "rtl" or "ltr" for a script that has a direction of its own, undefined for any other
JSFN(scriptDirection) {
    Vm& vm = ci.vm;
    std::string name = toAscii(vm.toString(ci.arg(0))->s);
    UScriptCode codes[4];
    UErrorCode err = U_ZERO_ERROR;
    int32_t n = uscript_getCode(name.c_str(), codes, 4, &err);
    if (U_FAILURE(err) || n != 1 || name.size() != 4) return Value::undef();
    UScriptCode c = codes[0];
    if (c == USCRIPT_COMMON || c == USCRIPT_INHERITED || c == USCRIPT_UNKNOWN || c == USCRIPT_UNWRITTEN_LANGUAGES ||
        c == USCRIPT_MATHEMATICAL_NOTATION || c == USCRIPT_SYMBOLS || c == USCRIPT_SYMBOLS_EMOJI || c == USCRIPT_INVALID_CODE)
        return Value::undef();
    return V(vm.str(std::string_view(uscript_isRightToLeft(c) ? "rtl" : "ltr")));
}

// ---------------------------------------------------------------- time zones
// One calendar per zone, kept for the thread: opening one reads the zone's rules.
UCalendar* zoneCalendar(const std::u16string& id) {
    thread_local std::unordered_map<std::u16string, UCalendar*> calendars;
    auto it = calendars.find(id);
    if (it != calendars.end()) return it->second;
    UErrorCode err = U_ZERO_ERROR;
    UCalendar* cal = ucal_open((const UChar*)id.data(), (int32_t)id.size(), "en", UCAL_GREGORIAN, &err);
    if (U_FAILURE(err)) cal = nullptr;
    else ucal_setGregorianChange(cal, -8.64e15, &err);  // proleptic: the rules alone decide, not the calendar reform
    calendars.emplace(id, cal);
    return cal;
}
// tzIds(region): every zone name ICU knows, with links; or with a region, the zones of that region
JSFN(tzIds) {
    Vm& vm = ci.vm;
    JSArray* a = vm.newArray();
    std::vector<Value> held{V(a)};
    RootScope keep(vm, held);
    UErrorCode err = U_ZERO_ERROR;
    std::string region = ci.arg(0).isUndef() ? std::string() : toAscii(vm.toString(ci.arg(0))->s);
    UEnumeration* e = region.empty() ? ucal_openTimeZoneIDEnumeration(UCAL_ZONE_TYPE_ANY, nullptr, nullptr, &err)
                                     : ucal_openTimeZoneIDEnumeration(UCAL_ZONE_TYPE_CANONICAL_LOCATION, region.c_str(), nullptr, &err);
    if (U_FAILURE(err)) return V(a);
    int32_t n = 0;
    while (const UChar* s = uenum_unext(e, &n, &err)) {
        if (U_FAILURE(err)) break;
        a->elems.push_back(V(vm.str(fromUChars(s, n))));
    }
    uenum_close(e);
    return V(a);
}
// tzCanonical(id): the zone a name stands for in CLDR, or undefined for a name ICU does not know
JSFN(tzCanonical) {
    Vm& vm = ci.vm;
    std::u16string id = vm.toString(ci.arg(0))->s;
    UChar out[128];
    UBool isSystem = false;
    UErrorCode err = U_ZERO_ERROR;
    int32_t n = ucal_getCanonicalTimeZoneID((const UChar*)id.data(), (int32_t)id.size(), out, 128, &isSystem, &err);
    if (U_FAILURE(err) || !isSystem) return Value::undef();
    return V(vm.str(fromUChars(out, n)));
}
// tzOffset(id, epochMs): the zone's UTC offset in milliseconds at that time
JSFN(tzOffset) {
    Vm& vm = ci.vm;
    UCalendar* cal = zoneCalendar(vm.toString(ci.arg(0))->s);
    if (!cal) return Value::undef();
    UErrorCode err = U_ZERO_ERROR;
    ucal_setMillis(cal, vm.toNumber(ci.arg(1)), &err);
    int32_t offset = ucal_get(cal, UCAL_ZONE_OFFSET, &err) + ucal_get(cal, UCAL_DST_OFFSET, &err);
    return U_FAILURE(err) ? Value::undef() : Value::num(offset);
}
// tzTransition(id, epochMs, next): the first rule change strictly after (next) or before that time, or null
JSFN(tzTransition) {
    Vm& vm = ci.vm;
    UCalendar* cal = zoneCalendar(vm.toString(ci.arg(0))->s);
    if (!cal) return Value::null();
    UErrorCode err = U_ZERO_ERROR;
    ucal_setMillis(cal, vm.toNumber(ci.arg(1)), &err);
    UDate when = 0;
    bool found = ucal_getTimeZoneTransitionDate(cal, vm.toBool(ci.arg(2)) ? UCAL_TZ_TRANSITION_NEXT : UCAL_TZ_TRANSITION_PREVIOUS, &when, &err);
    return found && U_SUCCESS(err) ? Value::num(when) : Value::null();
}
// defaultTimeZone(): the zone of this computer
JSFN(defaultTimeZone) {
    UChar out[128];
    UErrorCode err = U_ZERO_ERROR;
    int32_t n = ucal_getDefaultTimeZone(out, 128, &err);
    return U_FAILURE(err) ? Value::undef() : V(ci.vm.str(fromUChars(out, n)));
}

// ---------------------------------------------------------------- formatted values
// ICU functions newer than the Windows SDK's icu.h and icu.lib, looked up in icu.dll by name (ICU 68 and later)
struct UNumberRangeFormatter;
struct UFormattedNumberRange;
template <class F>
F icuFunction(const char* name) {
    static HMODULE icu = GetModuleHandleW(L"icu.dll");
    return icu ? (F)GetProcAddress(icu, name) : nullptr;
}
std::u16string ustring(Vm& vm, Value v) { return vm.toString(v)->s; }
// A formatted string as text, or with parts: [text, category, field, start, end, ...] for every field ICU reports
Value formattedValue(Vm& vm, const UFormattedValue* value, bool parts, UErrorCode& err) {
    int32_t n = 0;
    const UChar* s = ufmtval_getString(value, &n, &err);
    if (U_FAILURE(err)) return Value::undef();
    JSString* text = vm.str(fromUChars(s, n));
    if (!parts) return V(text);
    JSArray* a = vm.newArray();
    std::vector<Value> held{V(a)};
    RootScope keep(vm, held);
    a->elems.push_back(V(text));
    UConstrainedFieldPosition* pos = ucfpos_open(&err);
    while (U_SUCCESS(err) && ufmtval_nextPosition(value, pos, &err)) {
        int32_t start = 0, end = 0;
        ucfpos_getIndexes(pos, &start, &end, &err);
        a->elems.push_back(Value::num(ucfpos_getCategory(pos, &err)));
        a->elems.push_back(Value::num(ucfpos_getField(pos, &err)));
        a->elems.push_back(Value::num(start));
        a->elems.push_back(Value::num(end));
    }
    ucfpos_close(pos);
    return V(a);
}
// Formatters are slow to make and pages make the same few again and again: they are kept, by locale and skeleton.
template <class T, void (*Close)(T*)>
struct FormatterCache {
    std::unordered_map<std::u16string, T*> map;
    ~FormatterCache() { for (auto& e : map) if (e.second) Close(e.second); }
    T* find(const std::u16string& key) { auto it = map.find(key); return it == map.end() ? nullptr : it->second; }
    void put(const std::u16string& key, T* f) {
        if (map.size() >= 256) {  // a page that makes formatters without end: start over
            for (auto& e : map) if (e.second) Close(e.second);
            map.clear();
        }
        map[key] = f;
    }
};
void closeNumberRangeFormatter(UNumberRangeFormatter* f) {
    if (auto close = icuFunction<void (*)(UNumberRangeFormatter*)>("unumrf_close")) close(f);
}
UNumberFormatter* numberFormatter(const std::string& locale, const std::u16string& skeleton, UErrorCode& err) {
    thread_local FormatterCache<UNumberFormatter, unumf_close> cache;
    std::u16string key = fromAsciiKey(locale) + u'|' + skeleton;
    if (UNumberFormatter* f = cache.find(key)) return f;
    UNumberFormatter* f = unumf_openForSkeletonAndLocale((const UChar*)skeleton.data(), (int32_t)skeleton.size(), locale.c_str(), &err);
    if (U_FAILURE(err)) return nullptr;
    cache.put(key, f);
    return f;
}
// numberFormat(locale, skeleton, decimal, parts): decimal is a number as a decimal string ("-1.5e+21", "NaN", "-Infinity")
JSFN(numberFormat) {
    Vm& vm = ci.vm;
    UErrorCode err = U_ZERO_ERROR;
    UNumberFormatter* f = numberFormatter(toAscii(ustring(vm, ci.arg(0))), ustring(vm, ci.arg(1)), err);
    if (!f) vm.throwError(Err::RangeError, "The number format cannot be made");
    std::string decimal = toAscii(ustring(vm, ci.arg(2)));
    UFormattedNumber* result = unumf_openResult(&err);
    unumf_formatDecimal(f, decimal.c_str(), (int32_t)decimal.size(), result, &err);
    Value v = formattedValue(vm, unumf_resultAsValue(result, &err), vm.toBool(ci.arg(3)), err);
    unumf_closeResult(result);
    if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The number cannot be formatted");
    return v;
}
// numberFormatRange(locale, skeleton, start, end, parts): as numberFormat, for a range; with parts, the range spans are
// fields of category UFIELD_CATEGORY_NUMBER_RANGE_SPAN
JSFN(numberFormatRange) {
    Vm& vm = ci.vm;
    using Open = UNumberRangeFormatter* (*)(const UChar*, int32_t, int, int, const char*, UParseError*, UErrorCode*);
    using OpenResult = UFormattedNumberRange* (*)(UErrorCode*);
    using Format = void (*)(const UNumberRangeFormatter*, const char*, int32_t, const char*, int32_t, UFormattedNumberRange*, UErrorCode*);
    using AsValue = const UFormattedValue* (*)(const UFormattedNumberRange*, UErrorCode*);
    using CloseResult = void (*)(UFormattedNumberRange*);
    static Open open = icuFunction<Open>("unumrf_openForSkeletonWithCollapseAndIdentityFallback");
    static OpenResult openResult = icuFunction<OpenResult>("unumrf_openResult");
    static Format format = icuFunction<Format>("unumrf_formatDecimalRange");
    static AsValue asValue = icuFunction<AsValue>("unumrf_resultAsValue");
    static CloseResult closeResult = icuFunction<CloseResult>("unumrf_closeResult");
    if (!open || !openResult || !format || !asValue || !closeResult) vm.throwError(Err::RangeError, "This version of Windows cannot format number ranges");
    thread_local FormatterCache<UNumberRangeFormatter, closeNumberRangeFormatter> cache;
    std::string locale = toAscii(ustring(vm, ci.arg(0)));
    std::u16string skeleton = ustring(vm, ci.arg(1));
    std::u16string key = fromAsciiKey(locale) + u'|' + skeleton;
    UErrorCode err = U_ZERO_ERROR;
    UNumberRangeFormatter* f = cache.find(key);
    if (!f) {
        UParseError perr;
        f = open((const UChar*)skeleton.data(), (int32_t)skeleton.size(), 0 /* UNUM_RANGE_COLLAPSE_AUTO */,
                 2 /* UNUM_IDENTITY_FALLBACK_APPROXIMATELY */, locale.c_str(), &perr, &err);
        if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The number format cannot be made");
        cache.put(key, f);
    }
    std::string start = toAscii(ustring(vm, ci.arg(2))), end = toAscii(ustring(vm, ci.arg(3)));
    UFormattedNumberRange* result = openResult(&err);
    format(f, start.c_str(), (int32_t)start.size(), end.c_str(), (int32_t)end.size(), result, &err);
    Value v = formattedValue(vm, asValue(result, &err), vm.toBool(ci.arg(4)), err);
    closeResult(result);
    if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The numbers cannot be formatted");
    return v;
}
// currencyDigits(code): the minor unit digits of an ISO 4217 currency
JSFN(currencyDigits) {
    std::u16string code = ustring(ci.vm, ci.arg(0));
    UErrorCode err = U_ZERO_ERROR;
    int32_t digits = ucurr_getDefaultFractionDigits((const UChar*)code.c_str(), &err);
    return Value::num(U_FAILURE(err) ? 2 : digits);
}
// ---------------------------------------------------------------- plural rules
UPluralRules* pluralRules(const std::string& locale, bool ordinal) {
    thread_local FormatterCache<UPluralRules, uplrules_close> cache;
    std::u16string key = fromAsciiKey(locale) + (ordinal ? u"|o" : u"|c");
    if (UPluralRules* r = cache.find(key)) return r;
    UErrorCode err = U_ZERO_ERROR;
    UPluralRules* r = uplrules_openForType(locale.c_str(), ordinal ? UPLURAL_TYPE_ORDINAL : UPLURAL_TYPE_CARDINAL, &err);
    if (U_FAILURE(err)) return nullptr;
    cache.put(key, r);
    return r;
}
// pluralSelect(locale, ordinal, skeleton, decimal): the plural category of the number as the skeleton formats it
JSFN(pluralSelect) {
    Vm& vm = ci.vm;
    std::string locale = toAscii(ustring(vm, ci.arg(0)));
    UPluralRules* rules = pluralRules(locale, vm.toBool(ci.arg(1)));
    UErrorCode err = U_ZERO_ERROR;
    UNumberFormatter* f = numberFormatter(locale, ustring(vm, ci.arg(2)), err);
    if (!rules || !f) vm.throwError(Err::RangeError, "The plural rules cannot be made");
    std::string decimal = toAscii(ustring(vm, ci.arg(3)));
    UFormattedNumber* result = unumf_openResult(&err);
    unumf_formatDecimal(f, decimal.c_str(), (int32_t)decimal.size(), result, &err);
    UChar keyword[32];
    int32_t n = uplrules_selectFormatted(rules, result, keyword, 32, &err);
    unumf_closeResult(result);
    return U_FAILURE(err) ? V(vm.str(std::string_view("other"))) : V(vm.str(fromUChars(keyword, n)));
}
// pluralSelectRange(locale, ordinal, skeleton, start, end)
JSFN(pluralSelectRange) {
    Vm& vm = ci.vm;
    using SelectForRange = int32_t (*)(const UPluralRules*, const UFormattedNumberRange*, UChar*, int32_t, UErrorCode*);
    static SelectForRange select = icuFunction<SelectForRange>("uplrules_selectForRange");
    using Open = UNumberRangeFormatter* (*)(const UChar*, int32_t, int, int, const char*, UParseError*, UErrorCode*);
    using OpenResult = UFormattedNumberRange* (*)(UErrorCode*);
    using Format = void (*)(const UNumberRangeFormatter*, const char*, int32_t, const char*, int32_t, UFormattedNumberRange*, UErrorCode*);
    using CloseResult = void (*)(UFormattedNumberRange*);
    static Open open = icuFunction<Open>("unumrf_openForSkeletonWithCollapseAndIdentityFallback");
    static OpenResult openResult = icuFunction<OpenResult>("unumrf_openResult");
    static Format format = icuFunction<Format>("unumrf_formatDecimalRange");
    static CloseResult closeResult = icuFunction<CloseResult>("unumrf_closeResult");
    std::string locale = toAscii(ustring(vm, ci.arg(0)));
    UPluralRules* rules = pluralRules(locale, vm.toBool(ci.arg(1)));
    if (!select || !open || !openResult || !format || !closeResult || !rules) return V(vm.str(std::string_view("other")));
    std::u16string skeleton = ustring(vm, ci.arg(2));
    UErrorCode err = U_ZERO_ERROR;
    UParseError perr;
    UNumberRangeFormatter* f = open((const UChar*)skeleton.data(), (int32_t)skeleton.size(), 0, 2, locale.c_str(), &perr, &err);
    std::string start = toAscii(ustring(vm, ci.arg(3))), end = toAscii(ustring(vm, ci.arg(4)));
    UFormattedNumberRange* result = openResult(&err);
    if (f) format(f, start.c_str(), (int32_t)start.size(), end.c_str(), (int32_t)end.size(), result, &err);
    UChar keyword[32];
    int32_t n = U_SUCCESS(err) ? select(rules, result, keyword, 32, &err) : 0;
    closeResult(result);
    if (f) closeNumberRangeFormatter(f);
    return U_FAILURE(err) ? V(vm.str(std::string_view("other"))) : V(vm.str(fromUChars(keyword, n)));
}
// pluralKeywords(locale, ordinal): the categories the locale's rules use
JSFN(pluralKeywords) {
    Vm& vm = ci.vm;
    UPluralRules* rules = pluralRules(toAscii(ustring(vm, ci.arg(0))), vm.toBool(ci.arg(1)));
    JSArray* a = vm.newArray();
    std::vector<Value> held{V(a)};
    RootScope keep(vm, held);
    if (!rules) return V(a);
    UErrorCode err = U_ZERO_ERROR;
    UEnumeration* e = uplrules_getKeywords(rules, &err);
    int32_t n = 0;
    while (U_SUCCESS(err)) {
        const char* s = uenum_next(e, &n, &err);
        if (!s) break;
        a->elems.push_back(V(vm.str(std::string_view(s, n))));
    }
    uenum_close(e);
    return V(a);
}

// ---------------------------------------------------------------- dates
// A BCP 47 tag as an ICU locale id ("ja-JP-u-ca-japanese" is "ja_JP@calendar=japanese")
std::string icuLocale(const std::string& tag) {
    char id[ULOC_FULLNAME_CAPACITY];
    UErrorCode err = U_ZERO_ERROR;
    int32_t parsed = 0;
    uloc_forLanguageTag(tag.c_str(), id, sizeof id, &parsed, &err);
    return U_FAILURE(err) ? tag : std::string(id);
}
UDateTimePatternGenerator* patternGenerator(const std::string& locale, UErrorCode& err) {
    thread_local FormatterCache<UDateTimePatternGenerator, udatpg_close> cache;
    std::u16string key = fromAsciiKey(locale);
    if (auto* g = cache.find(key)) return g;
    UDateTimePatternGenerator* g = udatpg_open(icuLocale(locale).c_str(), &err);
    if (U_FAILURE(err)) return nullptr;
    cache.put(key, g);
    return g;
}
// dateBestPattern(locale, skeleton): the locale's pattern for those fields, with the field widths asked for
JSFN(dateBestPattern) {
    Vm& vm = ci.vm;
    UErrorCode err = U_ZERO_ERROR;
    UDateTimePatternGenerator* g = patternGenerator(toAscii(ustring(vm, ci.arg(0))), err);
    if (!g) vm.throwError(Err::RangeError, "The date format cannot be made");
    std::u16string skeleton = ustring(vm, ci.arg(1));
    UChar out[256];
    int32_t n = udatpg_getBestPatternWithOptions(g, (const UChar*)skeleton.data(), (int32_t)skeleton.size(), UDATPG_MATCH_ALL_FIELDS_LENGTH, out, 256, &err);
    if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The date format cannot be made");
    return V(vm.str(fromUChars(out, n)));
}
// dateSkeleton(pattern): the fields of a pattern, as a skeleton
JSFN(dateSkeleton) {
    Vm& vm = ci.vm;
    std::u16string pattern = ustring(vm, ci.arg(0));
    UChar out[256];
    UErrorCode err = U_ZERO_ERROR;
    int32_t n = udatpg_getSkeleton(nullptr, (const UChar*)pattern.data(), (int32_t)pattern.size(), out, 256, &err);
    return U_FAILURE(err) ? Value::undef() : V(vm.str(fromUChars(out, n)));
}
// dateStylePattern(locale, dateStyle, timeStyle): the pattern of a style (0 full to 3 short, -1 none)
JSFN(dateStylePattern) {
    Vm& vm = ci.vm;
    std::string locale = icuLocale(toAscii(ustring(vm, ci.arg(0))));
    int dateStyle = (int)vm.toNumber(ci.arg(1)), timeStyle = (int)vm.toNumber(ci.arg(2));
    UErrorCode err = U_ZERO_ERROR;
    UDateFormat* f = udat_open(timeStyle < 0 ? UDAT_NONE : (UDateFormatStyle)timeStyle, dateStyle < 0 ? UDAT_NONE : (UDateFormatStyle)dateStyle,
                               locale.c_str(), u"UTC", 3, nullptr, 0, &err);
    if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The date format cannot be made");
    UChar out[256];
    int32_t n = udat_toPattern(f, false, out, 256, &err);
    udat_close(f);
    return V(vm.str(fromUChars(out, n)));
}
UDateFormat* dateFormatter(const std::string& locale, const std::u16string& timeZone, const std::u16string& pattern, UErrorCode& err) {
    thread_local FormatterCache<UDateFormat, udat_close> cache;
    std::u16string key = fromAsciiKey(locale) + u'|' + timeZone + u'|' + pattern;
    if (UDateFormat* f = cache.find(key)) return f;
    UDateFormat* f = udat_open(UDAT_PATTERN, UDAT_PATTERN, icuLocale(locale).c_str(), (const UChar*)timeZone.data(), (int32_t)timeZone.size(),
                               (const UChar*)pattern.data(), (int32_t)pattern.size(), &err);
    if (U_FAILURE(err)) return nullptr;
    // proleptic Gregorian, as ECMA-262 dates are
    UCalendar* cal = (UCalendar*)udat_getCalendar(f);
    UCalendar* copy = ucal_clone(cal, &err);
    if (U_SUCCESS(err) && copy) {
        ucal_setGregorianChange(copy, -8.64e15, &err);
        udat_setCalendar(f, copy);
        ucal_close(copy);
    }
    err = U_ZERO_ERROR;
    cache.put(key, f);
    return f;
}
// dateFormat(locale, timeZone, pattern, epochMs, parts): with parts, [text, 1, field, start, end, ...] (category 1: dates)
JSFN(dateFormat) {
    Vm& vm = ci.vm;
    UErrorCode err = U_ZERO_ERROR;
    UDateFormat* f = dateFormatter(toAscii(ustring(vm, ci.arg(0))), ustring(vm, ci.arg(1)), ustring(vm, ci.arg(2)), err);
    if (!f) vm.throwError(Err::RangeError, "The date format cannot be made");
    double ms = vm.toNumber(ci.arg(3));
    bool parts = vm.toBool(ci.arg(4));
    UFieldPositionIterator* it = parts ? ufieldpositer_open(&err) : nullptr;
    UChar buf[512];
    int32_t n = udat_formatForFields(f, ms, buf, 512, it, &err);
    std::u16string text;
    if (err == U_BUFFER_OVERFLOW_ERROR) {
        text.resize(n);
        err = U_ZERO_ERROR;
        if (it) { ufieldpositer_close(it); it = ufieldpositer_open(&err); }
        udat_formatForFields(f, ms, (UChar*)text.data(), n, it, &err);
    } else text = fromUChars(buf, n);
    if (U_FAILURE(err)) {
        if (it) ufieldpositer_close(it);
        vm.throwError(Err::RangeError, "The date cannot be formatted");
    }
    if (!parts) return V(vm.str(std::move(text)));
    JSArray* a = vm.newArray();
    std::vector<Value> held{V(a)};
    RootScope keep(vm, held);
    a->elems.push_back(V(vm.str(std::move(text))));
    int32_t start = 0, end = 0, field;
    while ((field = ufieldpositer_next(it, &start, &end)) >= 0) {
        a->elems.push_back(Value::num(1));
        a->elems.push_back(Value::num(field));
        a->elems.push_back(Value::num(start));
        a->elems.push_back(Value::num(end));
    }
    ufieldpositer_close(it);
    return V(a);
}
// dateFormatRange(locale, timeZone, skeleton, startMs, endMs, parts): with parts, the fields as for formattedValue
// (category 1 dates, category 0x1005 the range spans)
JSFN(dateFormatRange) {
    Vm& vm = ci.vm;
    using FormatToResult = void (*)(const UDateIntervalFormat*, UDate, UDate, UFormattedDateInterval*, UErrorCode*);
    static FormatToResult formatToResult = icuFunction<FormatToResult>("udtitvfmt_formatToResult");
    if (!formatToResult) vm.throwError(Err::RangeError, "This version of Windows cannot format date ranges");
    thread_local FormatterCache<UDateIntervalFormat, udtitvfmt_close> cache;
    std::string locale = toAscii(ustring(vm, ci.arg(0)));
    std::u16string timeZone = ustring(vm, ci.arg(1)), skeleton = ustring(vm, ci.arg(2));
    std::u16string key = fromAsciiKey(locale) + u'|' + timeZone + u'|' + skeleton;
    UErrorCode err = U_ZERO_ERROR;
    UDateIntervalFormat* f = cache.find(key);
    if (!f) {
        f = udtitvfmt_open(icuLocale(locale).c_str(), (const UChar*)skeleton.data(), (int32_t)skeleton.size(), (const UChar*)timeZone.data(),
                           (int32_t)timeZone.size(), &err);
        if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The date range format cannot be made");
        cache.put(key, f);
    }
    UFormattedDateInterval* result = udtitvfmt_openResult(&err);
    formatToResult(f, vm.toNumber(ci.arg(3)), vm.toNumber(ci.arg(4)), result, &err);
    Value v = formattedValue(vm, udtitvfmt_resultAsValue(result, &err), vm.toBool(ci.arg(5)), err);
    udtitvfmt_closeResult(result);
    if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The dates cannot be formatted");
    return v;
}

// ---------------------------------------------------------------- collation, case, lists, names, segments
// collator(locale, strength, caseLevel, shifted, numeric, caseFirst): a collator by its settings, kept per thread.
// strength 0 primary to 2 tertiary; caseFirst "upper", "lower" or "false".
UCollator* collator(Vm& vm, CallInfo& ci, int first) {
    thread_local FormatterCache<UCollator, ucol_close> cache;
    std::string locale = toAscii(ustring(vm, ci.arg(first)));
    int strength = (int)vm.toNumber(ci.arg(first + 1));
    bool caseLevel = vm.toBool(ci.arg(first + 2)), shifted = vm.toBool(ci.arg(first + 3)), numeric = vm.toBool(ci.arg(first + 4));
    std::string caseFirst = toAscii(ustring(vm, ci.arg(first + 5)));
    std::u16string key = fromAsciiKey(locale + "|" + std::to_string(strength) + (caseLevel ? "c" : "-") + (shifted ? "s" : "-") + (numeric ? "n" : "-") + caseFirst);
    if (UCollator* c = cache.find(key)) return c;
    UErrorCode err = U_ZERO_ERROR;
    UCollator* c = ucol_open(icuLocale(locale).c_str(), &err);
    if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The collator cannot be made");
    ucol_setAttribute(c, UCOL_NORMALIZATION_MODE, UCOL_ON, &err);  // canonically equivalent strings compare equal
    ucol_setAttribute(c, UCOL_STRENGTH, strength == 0 ? UCOL_PRIMARY : strength == 1 ? UCOL_SECONDARY : UCOL_TERTIARY, &err);
    ucol_setAttribute(c, UCOL_CASE_LEVEL, caseLevel ? UCOL_ON : UCOL_OFF, &err);
    ucol_setAttribute(c, UCOL_ALTERNATE_HANDLING, shifted ? UCOL_SHIFTED : UCOL_NON_IGNORABLE, &err);
    ucol_setAttribute(c, UCOL_NUMERIC_COLLATION, numeric ? UCOL_ON : UCOL_OFF, &err);
    ucol_setAttribute(c, UCOL_CASE_FIRST, caseFirst == "upper" ? UCOL_UPPER_FIRST : caseFirst == "lower" ? UCOL_LOWER_FIRST : UCOL_OFF, &err);
    cache.put(key, c);
    return c;
}
// collatorCompare(a, b, locale, strength, caseLevel, shifted, numeric, caseFirst): -1, 0 or 1
JSFN(collatorCompare) {
    Vm& vm = ci.vm;
    UCollator* c = collator(vm, ci, 2);
    std::u16string a = ustring(vm, ci.arg(0)), b = ustring(vm, ci.arg(1));
    UCollationResult r = ucol_strcoll(c, (const UChar*)a.data(), (int32_t)a.size(), (const UChar*)b.data(), (int32_t)b.size());
    return Value::num(r == UCOL_LESS ? -1 : r == UCOL_GREATER ? 1 : 0);
}
// collatorDefaults(locale): [punctuation ignored, case first ("upper", "lower" or "false")] by the locale's own tailoring
JSFN(collatorDefaults) {
    Vm& vm = ci.vm;
    UErrorCode err = U_ZERO_ERROR;
    UCollator* c = ucol_open(icuLocale(toAscii(ustring(vm, ci.arg(0)))).c_str(), &err);
    JSArray* a = vm.newArray();
    std::vector<Value> held{V(a)};
    RootScope keep(vm, held);
    bool shifted = U_SUCCESS(err) && ucol_getAttribute(c, UCOL_ALTERNATE_HANDLING, &err) == UCOL_SHIFTED;
    UColAttributeValue caseFirst = U_SUCCESS(err) ? ucol_getAttribute(c, UCOL_CASE_FIRST, &err) : UCOL_OFF;
    a->elems.push_back(Value::boolean(shifted));
    a->elems.push_back(vm.strv(std::string_view(caseFirst == UCOL_UPPER_FIRST ? "upper" : caseFirst == UCOL_LOWER_FIRST ? "lower" : "false")));
    if (c) ucol_close(c);
    return V(a);
}
// changeCase(text, language, upper): the full case mapping of the language's rules (Turkish dotted i, Lithuanian ...)
JSFN(changeCase) {
    Vm& vm = ci.vm;
    std::u16string text = ustring(vm, ci.arg(0));
    std::string language = toAscii(ustring(vm, ci.arg(1)));
    bool upper = vm.toBool(ci.arg(2));
    std::u16string out(text.size() + 16, u'\0');
    UErrorCode err = U_ZERO_ERROR;
    auto run = [&] {
        return upper ? u_strToUpper((UChar*)out.data(), (int32_t)out.size(), (const UChar*)text.data(), (int32_t)text.size(), language.c_str(), &err)
                     : u_strToLower((UChar*)out.data(), (int32_t)out.size(), (const UChar*)text.data(), (int32_t)text.size(), language.c_str(), &err);
    };
    int32_t n = run();
    if (err == U_BUFFER_OVERFLOW_ERROR) {
        out.assign(n, u'\0');
        err = U_ZERO_ERROR;
        n = run();
    }
    if (U_FAILURE(err)) return V(vm.str(std::move(text)));
    out.resize(n);
    return V(vm.str(std::move(out)));
}
// listFormat(locale, type, width, items, parts): type 0 and, 1 or, 2 unit; width 0 long, 1 short, 2 narrow.
// With parts, the fields of category UFIELD_CATEGORY_LIST (0 literal, 1 element).
JSFN(listFormat) {
    Vm& vm = ci.vm;
    thread_local FormatterCache<UListFormatter, ulistfmt_close> cache;
    std::string locale = toAscii(ustring(vm, ci.arg(0)));
    int type = (int)vm.toNumber(ci.arg(1)), width = (int)vm.toNumber(ci.arg(2));
    std::u16string key = fromAsciiKey(locale + "|" + std::to_string(type) + std::to_string(width));
    UErrorCode err = U_ZERO_ERROR;
    UListFormatter* f = cache.find(key);
    if (!f) {
        f = ulistfmt_openForType(icuLocale(locale).c_str(), (UListFormatterType)type, (UListFormatterWidth)width, &err);
        if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The list format cannot be made");
        cache.put(key, f);
    }
    std::vector<std::u16string> items;
    Value list = ci.arg(3);
    uint32_t n = vm.lengthOf(list);
    for (uint32_t i = 0; i < n; i++) items.push_back(vm.toString(vm.get(list, PropKey::index(i)))->s);
    std::vector<const UChar*> ptrs;
    std::vector<int32_t> lens;
    for (auto& s : items) { ptrs.push_back((const UChar*)s.data()); lens.push_back((int32_t)s.size()); }
    UFormattedList* result = ulistfmt_openResult(&err);
    ulistfmt_formatStringsToResult(f, ptrs.data(), lens.data(), (int32_t)items.size(), result, &err);
    Value v = formattedValue(vm, ulistfmt_resultAsValue(result, &err), vm.toBool(ci.arg(4)), err);
    ulistfmt_closeResult(result);
    if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The list cannot be formatted");
    return v;
}
// displayName(locale, kind, code, long, dialect): the name of a language, region, script, currency, calendar or date
// field in the locale, or undefined when ICU has none of its own (it would only echo the code)
JSFN(displayName) {
    Vm& vm = ci.vm;
    std::string locale = icuLocale(toAscii(ustring(vm, ci.arg(0))));
    std::string kind = toAscii(ustring(vm, ci.arg(1)));
    std::u16string code16 = ustring(vm, ci.arg(2));
    std::string code = toAscii(code16);
    bool full = vm.toBool(ci.arg(3)), dialect = vm.toBool(ci.arg(4));
    UErrorCode err = U_ZERO_ERROR;
    UChar out[512];
    int32_t n = 0;
    if (kind == "currency") {
        UBool isChoice = false;
        int32_t len = 0;
        const UChar* name = ucurr_getName((const UChar*)code16.c_str(), locale.c_str(), full ? UCURR_LONG_NAME : UCURR_SYMBOL_NAME, &isChoice, &len, &err);
        if (U_FAILURE(err) || err == U_USING_DEFAULT_WARNING) return Value::undef();
        return V(vm.str(fromUChars(name, len)));
    }
    if (kind == "dateTimeField") {
        static const struct { const char* name; UDateTimePatternField field; } fields[] = {
            {"era", UDATPG_ERA_FIELD}, {"year", UDATPG_YEAR_FIELD}, {"quarter", UDATPG_QUARTER_FIELD}, {"month", UDATPG_MONTH_FIELD},
            {"weekOfYear", UDATPG_WEEK_OF_YEAR_FIELD}, {"weekday", UDATPG_WEEKDAY_FIELD}, {"day", UDATPG_DAY_FIELD},
            {"dayPeriod", UDATPG_DAYPERIOD_FIELD}, {"hour", UDATPG_HOUR_FIELD}, {"minute", UDATPG_MINUTE_FIELD}, {"second", UDATPG_SECOND_FIELD},
            {"timeZoneName", UDATPG_ZONE_FIELD}};
        UDateTimePatternGenerator* g = patternGenerator(toAscii(ustring(vm, ci.arg(0))), err);
        for (auto& f : fields) {
            if (code != f.name || !g) continue;
            n = udatpg_getFieldDisplayName(g, f.field, full ? UDATPG_WIDE : UDATPG_ABBREVIATED, out, 512, &err);
            return U_FAILURE(err) ? Value::undef() : V(vm.str(fromUChars(out, n)));
        }
        return Value::undef();
    }
    UDisplayContext contexts[] = {dialect ? UDISPCTX_DIALECT_NAMES : UDISPCTX_STANDARD_NAMES, full ? UDISPCTX_LENGTH_FULL : UDISPCTX_LENGTH_SHORT,
                                  UDISPCTX_NO_SUBSTITUTE};
    ULocaleDisplayNames* names = uldn_openForContext(locale.c_str(), contexts, 3, &err);
    if (U_FAILURE(err)) return Value::undef();
    if (kind == "language") n = uldn_localeDisplayName(names, icuLocale(code).c_str(), out, 512, &err);
    else if (kind == "region") n = uldn_regionDisplayName(names, code.c_str(), out, 512, &err);
    else if (kind == "script") n = uldn_scriptDisplayName(names, code.c_str(), out, 512, &err);
    else if (kind == "calendar") n = uldn_keyValueDisplayName(names, "calendar", code.c_str(), out, 512, &err);
    else err = U_ILLEGAL_ARGUMENT_ERROR;
    uldn_close(names);
    if (U_FAILURE(err) || n <= 0) return Value::undef();
    return V(vm.str(fromUChars(out, n)));
}
// segment(locale, granularity, text): the boundaries of the text, as [index, ruleStatus, index, ruleStatus, ...]
// beginning with 0; granularity 0 grapheme, 1 word, 2 sentence. A word segment is word-like when its status is not
// UBRK_WORD_NONE (below 100).
JSFN(segment) {
    Vm& vm = ci.vm;
    std::string locale = icuLocale(toAscii(ustring(vm, ci.arg(0))));
    int granularity = (int)vm.toNumber(ci.arg(1));
    std::u16string text = ustring(vm, ci.arg(2));
    UErrorCode err = U_ZERO_ERROR;
    UBreakIterator* it = ubrk_open(granularity == 0 ? UBRK_CHARACTER : granularity == 1 ? UBRK_WORD : UBRK_SENTENCE, locale.c_str(),
                                   (const UChar*)text.data(), (int32_t)text.size(), &err);
    if (U_FAILURE(err)) vm.throwError(Err::RangeError, "The text cannot be segmented");
    JSArray* a = vm.newArray();
    std::vector<Value> held{V(a)};
    RootScope keep(vm, held);
    for (int32_t pos = ubrk_first(it); pos != UBRK_DONE; pos = ubrk_next(it)) {
        a->elems.push_back(Value::num(pos));
        a->elems.push_back(Value::num(ubrk_getRuleStatus(it)));
    }
    ubrk_close(it);
    return V(a);
}

}  // namespace

JSObject* icuNatives(Vm& vm) {
    JSObject* o = vm.newObject();
    std::vector<Value> held{V(o)};
    RootScope keep(vm, held);
    vm.defFn(o, "resource", resource, 3);
    vm.defFn(o, "likelySubtags", likelySubtags, 2);
    vm.defFn(o, "availableLocales", availableLocales, 0);
    vm.defFn(o, "scriptDirection", scriptDirection, 1);
    vm.defFn(o, "tzIds", tzIds, 1);
    vm.defFn(o, "tzCanonical", tzCanonical, 1);
    vm.defFn(o, "tzOffset", tzOffset, 2);
    vm.defFn(o, "tzTransition", tzTransition, 3);
    vm.defFn(o, "defaultTimeZone", defaultTimeZone, 0);
    vm.defFn(o, "numberFormat", numberFormat, 4);
    vm.defFn(o, "numberFormatRange", numberFormatRange, 5);
    vm.defFn(o, "currencyDigits", currencyDigits, 1);
    vm.defFn(o, "pluralSelect", pluralSelect, 4);
    vm.defFn(o, "pluralSelectRange", pluralSelectRange, 5);
    vm.defFn(o, "pluralKeywords", pluralKeywords, 2);
    vm.defFn(o, "dateBestPattern", dateBestPattern, 2);
    vm.defFn(o, "dateSkeleton", dateSkeleton, 1);
    vm.defFn(o, "dateStylePattern", dateStylePattern, 3);
    vm.defFn(o, "dateFormat", dateFormat, 5);
    vm.defFn(o, "dateFormatRange", dateFormatRange, 6);
    vm.defFn(o, "collatorCompare", collatorCompare, 8);
    vm.defFn(o, "collatorDefaults", collatorDefaults, 1);
    vm.defFn(o, "changeCase", changeCase, 3);
    vm.defFn(o, "listFormat", listFormat, 5);
    vm.defFn(o, "displayName", displayName, 5);
    vm.defFn(o, "segment", segment, 3);
    return o;
}

}  // namespace tb::js
