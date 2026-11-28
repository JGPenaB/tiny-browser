// The bridge to ICU, the Unicode library that ships with Windows 10 (1903) and later as icu.dll. Intl and the
// named time zones of Temporal are written in JavaScript; they get their locale data and time zone rules from the
// natives made here. Only plain data crosses the bridge: strings, numbers and arrays.
#include "builtins.h"
#include "icu_bridge.h"
#include <icu.h>
#include <cstring>
#include <unordered_map>

namespace tb::js {
namespace {

std::u16string fromUChars(const UChar* s, int32_t n) { return std::u16string((const char16_t*)s, (size_t)n); }
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
    return o;
}

}  // namespace tb::js
