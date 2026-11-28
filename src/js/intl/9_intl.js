// ---------------------------------------------------------------- the Intl object (8)
var Intl = {};
function hidden(target, name, value) { defineProperty(target, name, { value: value, writable: true, enumerable: false, configurable: true }); }
function tag(target, name) { defineProperty(target, symToStringTag, { value: name, writable: false, enumerable: false, configurable: true }); }
function method(name, fn, length) {  // a built-in function: no prototype, not a constructor
  var f = { [name](a, b) { return fn(a, b); } }[name];
  defineProperty(f, 'length', { value: length, writable: false, enumerable: false, configurable: true });
  return f;
}
var classes = { DisplayNames: DisplayNames, DurationFormat: DurationFormat, ListFormat: ListFormat, Locale: Locale, PluralRules: PluralRules, RelativeTimeFormat: RelativeTimeFormat,
                Segmenter: Segmenter };
var classNames = getOwnPropertyNames(classes);
for (var ci = 0; ci < classNames.length; ci++) {
  var cls = classes[classNames[ci]];
  setPrototypeOf(cls, P.Function.value.prototype);  // `extends Object` only spares the constructors a `this` they never use
  tag(cls.prototype, 'Intl.' + classNames[ci]);
  hidden(Intl, classNames[ci], cls);
}
// the constructors written as functions, because they may also be called as functions (4.3 Note 1)
var functions = { Collator: Collator, DateTimeFormat: DateTimeFormat, NumberFormat: NumberFormat };
var functionNames = getOwnPropertyNames(functions);
for (var fi = 0; fi < functionNames.length; fi++) {
  tag(functions[functionNames[fi]].prototype, 'Intl.' + functionNames[fi]);
  hidden(Intl, functionNames[fi], functions[functionNames[fi]]);
}
hidden(Intl, 'getCanonicalLocales', method('getCanonicalLocales', function (locales) { return toArray(canonicalizeLocaleList(locales)); }, 1));
hidden(Intl, 'supportedValuesOf', method('supportedValuesOf', supportedValuesOf, 1));
tag(Intl, 'Intl');
natives.protos = create(null);  // for protoFor when a constructor of another realm has no prototype object
for (var pi = 0; pi < classNames.length; pi++) natives.protos[classNames[pi]] = classes[classNames[pi]].prototype;
for (var pf = 0; pf < functionNames.length; pf++) natives.protos[functionNames[pf]] = functions[functionNames[pf]].prototype;
natives.intl = Intl;
})
