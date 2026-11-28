// Shared by Temporal and Intl (tools/gen_js_parts.py puts it after each one's first part, whose helpers it uses:
// icu, $Map, mapGet, mapSet, mapHas, toLowerCase, strSlice, arrayIndexOf, arrayPush, arraySort, getOwnPropertyNames).
//
// The available named time zones are those of the IANA database as ICU has them, minus ICU's own legacy names.
// Each record is { id, primary }: id is the name with its IANA case, primary the zone it is a link to (11.1.1).
var ZONES, ZONE_LIST = list();
var UTC_NAMES = ['UTC', 'Etc/UTC', 'Etc/GMT', 'GMT', 'Etc/Universal', 'Etc/Zulu', 'Etc/UCT', 'UCT', 'Universal', 'Zulu', 'Etc/Greenwich',
                 'Greenwich', 'GMT0', 'GMT+0', 'GMT-0', 'Etc/GMT0', 'Etc/GMT+0', 'Etc/GMT-0'];
// Java's three-letter names, which ICU accepts but the IANA database does not have
var NOT_IANA = ['ACT', 'AET', 'AGT', 'ART', 'AST', 'BET', 'BST', 'CAT', 'CNT', 'CST', 'CTT', 'EAT', 'ECT', 'IET', 'IST', 'JST', 'MIT', 'NET',
                'NST', 'PLT', 'PNT', 'PRT', 'PST', 'SST', 'VST'];
// Where CLDR (and so ICU) still calls a zone by a name that the IANA database has since made a link
var IANA_PRIMARY = { 'Africa/Asmera': 'Africa/Asmara', 'America/Buenos_Aires': 'America/Argentina/Buenos_Aires',
  'America/Catamarca': 'America/Argentina/Catamarca', 'America/Coral_Harbour': 'America/Atikokan', 'America/Cordoba': 'America/Argentina/Cordoba',
  'America/Godthab': 'America/Nuuk', 'America/Indianapolis': 'America/Indiana/Indianapolis', 'America/Jujuy': 'America/Argentina/Jujuy',
  'America/Louisville': 'America/Kentucky/Louisville', 'America/Mendoza': 'America/Argentina/Mendoza', 'Asia/Calcutta': 'Asia/Kolkata',
  'Asia/Katmandu': 'Asia/Kathmandu', 'Asia/Rangoon': 'Asia/Yangon', 'Asia/Saigon': 'Asia/Ho_Chi_Minh', 'Atlantic/Faeroe': 'Atlantic/Faroe',
  'Europe/Kiev': 'Europe/Kyiv', 'Pacific/Enderbury': 'Pacific/Kanton', 'Pacific/Ponape': 'Pacific/Pohnpei', 'Pacific/Truk': 'Pacific/Chuuk' };
function zoneTable() {
  if (ZONES !== undefined) return ZONES;
  ZONES = new $Map();
  var ids = icu.tzIds(), i;
  for (i = 0; i < ids.length; i++) {
    var id = ids[i];
    if (strSlice(id, 0, 8) === 'SystemV/' || arrayIndexOf(NOT_IANA, id) >= 0) continue;
    var record = { id: id, primary: undefined };
    mapSet(ZONES, toLowerCase(id), record);
    arrayPush(ZONE_LIST, record);
  }
  for (i = 0; i < UTC_NAMES.length; i++) mapSet(ZONES, toLowerCase(UTC_NAMES[i]), { id: UTC_NAMES[i], primary: 'UTC' });
  var keys = getOwnPropertyNames(IANA_PRIMARY);  // the IANA names, in case ICU lacks one
  for (i = 0; i < keys.length; i++) {
    var name = IANA_PRIMARY[keys[i]];
    if (!mapHas(ZONES, toLowerCase(name))) mapSet(ZONES, toLowerCase(name), { id: name, primary: name });
  }
  return ZONES;
}
// GetAvailableNamedTimeZoneIdentifier (11.1.1)
function namedZone(name) {
  var record = mapGet(zoneTable(), toLowerCase(name));
  if (record !== undefined && record.primary === undefined) {
    var canonical = icu.tzCanonical(record.id);
    if (canonical === undefined) canonical = record.id;
    if (canonical === 'Etc/UTC' || canonical === 'Etc/GMT') canonical = 'UTC';
    var renamed = IANA_PRIMARY[canonical];
    record.primary = renamed !== undefined ? renamed : canonical;
  }
  return record;
}
// The name to ask ICU about: an IANA name that ICU only knows by its older one is asked by that
function icuZone(timeZone) {
  var primary = namedZone(timeZone).primary;
  var keys = getOwnPropertyNames(IANA_PRIMARY);
  for (var i = 0; i < keys.length; i++) if (IANA_PRIMARY[keys[i]] === primary) return keys[i];
  return primary;
}
// AvailablePrimaryTimeZoneIdentifiers (ECMA-402 6.5.3), sorted
function primaryZoneIds() {
  var seen = new $Map(), out = list();
  zoneTable();
  for (var i = 0; i < ZONE_LIST.length; i++) {
    var primary = namedZone(ZONE_LIST[i].id).primary;
    if (!mapHas(seen, primary)) { mapSet(seen, primary, true); arrayPush(out, primary); }
  }
  return arraySort(out);
}
