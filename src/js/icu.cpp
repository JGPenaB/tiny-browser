// The bridge to ICU, the Unicode library that ships with Windows 10 (1903) and later as icu.dll. Intl and the
// named time zones of Temporal are written in JavaScript; they get their locale data and time zone rules from the
// natives made here. Only plain data crosses the bridge: strings, numbers and arrays.
#include "builtins.h"
#include "icu_bridge.h"
#include <icu.h>
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
            ures_resetIterator(r);
            while (ures_hasNext(r)) {
                UErrorCode e = U_ZERO_ERROR;
                UResourceBundle* child = ures_getNextResource(r, nullptr, &e);
                if (U_FAILURE(e)) break;
                Value v = dumpResource(vm, child, held);
                held.push_back(v);
                if (table) vm.def(o, ures_getKey(child), v, PF_ALL);
                else ((JSArray*)o)->elems.push_back(v);
                ures_close(child);
            }
            return V(o);
        }
        default: return Value::undef();
    }
}
// resource(name): the whole of one of ICU's data tables, such as "metadata" (locale aliases) or "likelySubtags"
JSFN(resource) {
    Vm& vm = ci.vm;
    UErrorCode err = U_ZERO_ERROR;
    std::string name = toAscii(vm.toString(ci.arg(0))->s);
    UResourceBundle* r = ures_openDirect(nullptr, name.c_str(), &err);
    if (U_FAILURE(err)) return Value::undef();
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
// tzIds(): every zone name ICU knows, with links
JSFN(tzIds) {
    Vm& vm = ci.vm;
    JSArray* a = vm.newArray();
    std::vector<Value> held{V(a)};
    RootScope keep(vm, held);
    UErrorCode err = U_ZERO_ERROR;
    UEnumeration* e = ucal_openTimeZoneIDEnumeration(UCAL_ZONE_TYPE_ANY, nullptr, nullptr, &err);
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
    vm.defFn(o, "resource", resource, 1);
    vm.defFn(o, "likelySubtags", likelySubtags, 2);
    vm.defFn(o, "availableLocales", availableLocales, 0);
    vm.defFn(o, "tzIds", tzIds, 0);
    vm.defFn(o, "tzCanonical", tzCanonical, 1);
    vm.defFn(o, "tzOffset", tzOffset, 2);
    vm.defFn(o, "tzTransition", tzTransition, 3);
    vm.defFn(o, "defaultTimeZone", defaultTimeZone, 0);
    return o;
}

}  // namespace tb::js
