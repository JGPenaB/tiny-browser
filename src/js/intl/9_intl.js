// ---------------------------------------------------------------- the Intl object (8)
var Intl = {};
function hidden(target, name, value) { defineProperty(target, name, { value: value, writable: true, enumerable: false, configurable: true }); }
function tag(target, name) { defineProperty(target, symToStringTag, { value: name, writable: false, enumerable: false, configurable: true }); }
function method(name, fn, length) {  // a built-in function: no prototype, not a constructor
  var f = { [name](a, b) { return fn(a, b); } }[name];
  defineProperty(f, 'length', { value: length, writable: false, enumerable: false, configurable: true });
  return f;
}
var classes = { Locale: Locale };
var classNames = getOwnPropertyNames(classes);
for (var ci = 0; ci < classNames.length; ci++) {
  var cls = classes[classNames[ci]];
  setPrototypeOf(cls, Function.prototype);  // `extends Object` only spares the constructors a `this` they never use
  tag(cls.prototype, 'Intl.' + classNames[ci]);
  hidden(Intl, classNames[ci], cls);
}
hidden(Intl, 'getCanonicalLocales', method('getCanonicalLocales', function (locales) { return toArray(canonicalizeLocaleList(locales)); }, 1));
hidden(Intl, 'supportedValuesOf', method('supportedValuesOf', supportedValuesOf, 1));
tag(Intl, 'Intl');
natives.intl = Intl;
})
