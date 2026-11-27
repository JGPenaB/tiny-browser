// Globals the engine builds on first use (Temporal) behave like ordinary data properties before they are built.
test("a write before the first read replaces the lazy value", function () {
  globalThis.Temporal = 5;
  eq(Temporal, 5);
  eq(Object.getOwnPropertyDescriptor(globalThis, "Temporal"), { value: 5, writable: true, enumerable: false, configurable: true });
  ok(delete globalThis.Temporal); ok(!("Temporal" in globalThis));
});
done();
