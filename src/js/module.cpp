// ES modules (ECMA-262 16.2.1): module records, their namespace objects, and the loader that fetches, links and
// evaluates module graphs. The embedder supplies two things through the `__modules.host` object: how a specifier
// becomes a URL, and how the text behind a URL is fetched.
#include <algorithm>
#include <unordered_map>

#include "builtins.h"

namespace tb::js {

namespace {

const uint8_t kModuleKind = 200;

// One object is both the module's record (what the natives below work on) and its namespace object (what script
// sees, ECMA-262 10.4.6). Exports are read straight out of the module's top-level scope, so importers always see
// the current value.
struct JSModule : JSHost {
    struct Export {
        JSString* name;
        int slot;              // slot in `scope` for a local export
        int request;           // index into deps for a re-export, else -1
        JSString* importName;  // the name in that module; null re-exports its namespace (export * as x)
    };
    struct Binding {
        JSModule* module = nullptr;
        int slot = -1;  // -1: the binding is `module`'s namespace object
    };
    JSFunction* fn = nullptr;         // the compiled module body
    JSGenerator* activation = nullptr;  // its running state: paused before the first statement once linked
    Context* scope = nullptr;         // the module environment
    std::vector<JSString*> requests;  // [[RequestedModules]]
    std::vector<int> requestSlots;    // where each requested module's namespace goes in `scope`
    std::vector<Export> exports;      // local and indirect export entries
    std::vector<int> stars;           // star export entries
    std::vector<std::pair<int, JSString*>> imports;
    std::vector<JSModule*> deps;  // [[LoadedModules]], in the order of requests
    bool hasTLA = false, namesReady = false;
    std::vector<JSString*> names;  // the namespace's [[Exports]]: unambiguous names in code unit order
    std::unordered_map<JSString*, Binding> bindings;
    JSModule() { hostKind = kModuleKind; }
    void trace(Gc& g) override {
        JSHost::trace(g);
        if (fn) g.mark(fn);
        if (activation) g.mark(activation);
        if (scope) g.mark(scope);
        for (JSString* r : requests) g.mark(r);
        for (auto& e : exports) {
            g.mark(e.name);
            if (e.importName) g.mark(e.importName);
        }
        for (auto& im : imports) g.mark(im.second);
        for (JSModule* d : deps)
            if (d) g.mark(d);
        for (JSString* n : names) g.mark(n);
    }
};

bool isModule(Value v) { return v.isObject() && v.asObject()->cls == Cls::Host && ((JSHost*)v.asObject())->hostKind == kModuleKind; }

enum class Found { No, Yes, Ambiguous };
struct Resolution {
    Found found = Found::No;
    JSModule* module = nullptr;
    int slot = -1;
};
using ResolveSet = std::vector<std::pair<JSModule*, JSString*>>;

JSModule* dep(JSModule* m, int request) { return request >= 0 && (size_t)request < m->deps.size() ? m->deps[request] : nullptr; }

// ECMA-262 16.2.1.7.2.2 ResolveExport
Resolution resolveExport(Vm& vm, JSModule* m, JSString* name, ResolveSet& seen) {
    for (auto& r : seen)
        if (r.first == m && r.second == name) return {};  // a circular import request
    seen.push_back({m, name});
    for (auto& e : m->exports) {
        if (e.name != name) continue;
        if (e.request < 0) return {Found::Yes, m, e.slot};
        JSModule* from = dep(m, e.request);
        if (!from) return {};
        if (!e.importName) return {Found::Yes, from, -1};
        return resolveExport(vm, from, e.importName, seen);
    }
    if (name == vm.a.default_) return {};  // export * never provides a default export
    Resolution star;
    for (int r : m->stars) {
        JSModule* from = dep(m, r);
        if (!from) continue;
        Resolution res = resolveExport(vm, from, name, seen);
        if (res.found == Found::Ambiguous) return res;
        if (res.found == Found::No) continue;
        if (star.found == Found::No) star = res;
        else if (res.module != star.module || res.slot != star.slot) return {Found::Ambiguous};
    }
    return star;
}

// ECMA-262 16.2.1.7.2.1 GetExportedNames
std::vector<JSString*> exportedNames(Vm& vm, JSModule* m, std::vector<JSModule*>& starSet) {
    std::vector<JSString*> names;
    if (std::find(starSet.begin(), starSet.end(), m) != starSet.end()) return names;  // an export * circularity
    starSet.push_back(m);
    for (auto& e : m->exports) names.push_back(e.name);
    for (int r : m->stars) {
        JSModule* from = dep(m, r);
        if (!from) continue;
        for (JSString* n : exportedNames(vm, from, starSet))
            if (n != vm.a.default_ && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    }
    return names;
}

// ECMA-262 16.2.1.13 GetModuleNamespace: the names are worked out once, when the namespace is first used.
void ensureNames(Vm& vm, JSModule* m) {
    if (m->namesReady) return;
    std::vector<JSModule*> starSet;
    for (JSString* n : exportedNames(vm, m, starSet)) {
        ResolveSet seen;
        Resolution r = resolveExport(vm, m, n, seen);
        if (r.found != Found::Yes) continue;
        m->names.push_back(n);
        m->bindings[n] = {r.module, r.slot};
    }
    std::sort(m->names.begin(), m->names.end(), [](JSString* a, JSString* b) { return a->s < b->s; });
    m->namesReady = true;
}

JSString* exportName(Vm& vm, PropKey k) {
    if (k.isIndex()) return vm.atom(std::to_string(k.asIndex()));
    return k.isString() ? k.str() : nullptr;
}
bool nsHas(Vm& vm, JSObject* o, PropKey k) {
    JSString* name = exportName(vm, k);
    if (!name) return false;
    JSModule* m = (JSModule*)o;
    ensureNames(vm, m);
    return m->bindings.count(name) != 0;
}
bool nsGet(Vm& vm, JSObject* o, PropKey k, Value& out) {
    JSString* name = exportName(vm, k);
    if (!name) return false;
    JSModule* m = (JSModule*)o;
    ensureNames(vm, m);
    auto it = m->bindings.find(name);
    if (it == m->bindings.end()) return false;
    const JSModule::Binding& b = it->second;
    if (b.slot < 0) {
        out = V(b.module);
        return true;
    }
    Context* env = b.module->scope;
    Value v = env && (size_t)b.slot < env->slots.size() ? env->slots[b.slot] : Value::empty();
    if (v.isEmpty()) vm.throwError(Err::ReferenceError, "Cannot access '%s' before initialization", utf16to8(name->s).c_str());
    out = v;
    return true;
}
void nsKeys(Vm& vm, JSObject* o, std::vector<PropKey>& out) {
    JSModule* m = (JSModule*)o;
    ensureNames(vm, m);
    for (JSString* n : m->names) out.push_back(vm.toKey(V(n)));
}
// No set hook: [[Set]] on a namespace always fails, because the object is not extensible and has no own data.
const HostHooks kNsHooks{nsGet, nullptr, nullptr, nsKeys, PF_W | PF_E, nsHas};

JSModule* moduleArg(CallInfo& ci, int i) {
    if (!isModule(ci.arg(i))) ci.vm.throwError(Err::TypeError, "Not a module");
    return (JSModule*)ci.arg(i).asObject();
}

// compile(source, file) -> [module, [requested specifiers], has top-level await]   (ParseModule)
JSFN(moduleCompile) {
    Vm& vm = ci.vm;
    auto src = std::make_shared<const std::u16string>(vm.toStr16(ci.arg(0)));
    std::string file = utf16to8(vm.toStr16(ci.arg(1)));
    ModuleInfo info;
    FuncProto* p = compileScript(vm, src, file, false, nullptr, &info);
    JSFunction* outer = vm.makeClosure(p, nullptr, nullptr);
    Value fn = vm.callFunction(outer, V(vm.global), nullptr, 0, Value::undef(), false);
    JSArray* out = vm.newArray();
    JSModule* m = vm.alloc<JSModule>();
    out->elems.push_back(V(m));
    m->hooks = &kNsHooks;
    m->proto = nullptr;
    m->oflags |= OF_NOEXT;
    m->add(PropKey::of(vm.symToStringTag), vm.strv("Module"), 0);
    m->fn = (JSFunction*)fn.asObject();
    m->hasTLA = info.hasTopLevelAwait;
    m->requestSlots = info.requestSlots;
    m->stars = info.stars;
    for (auto& r : info.requests) m->requests.push_back(vm.atom(r));
    for (auto& e : info.exports) m->exports.push_back({vm.atom(e.name), e.slot, e.request, e.named ? vm.atom(e.importName) : nullptr});
    for (auto& im : info.imports) m->imports.push_back({im.first, vm.atom(im.second)});
    JSArray* requests = vm.newArray();
    out->elems.push_back(V(requests));
    for (JSString* r : m->requests) requests->elems.push_back(V(r));
    out->elems.push_back(Value::boolean(m->hasTLA));
    JSArray* types = vm.newArray();
    out->elems.push_back(V(types));
    for (size_t i = 0; i < info.requests.size(); ++i)
        types->elems.push_back(i < info.requestTypes.size() && !info.requestTypes[i].empty() ? vm.strv(info.requestTypes[i]) : Value::undef());
    return V(out);
}

// bind(module, [modules]): the modules its requests were resolved to ([[LoadedModules]])
JSFN(moduleBind) {
    Vm& vm = ci.vm;
    JSModule* m = moduleArg(ci, 0);
    std::vector<JSModule*> deps;
    for (size_t i = 0; i < m->requests.size(); ++i) {
        Value d = vm.get(ci.arg(1), PropKey::index((uint32_t)i));
        if (!isModule(d)) vm.throwError(Err::TypeError, "Module '%s' was not loaded", utf16to8(m->requests[i]->s).c_str());
        deps.push_back((JSModule*)d.asObject());
    }
    m->deps = std::move(deps);
    return Value::undef();
}

[[noreturn]] void linkError(Vm& vm, JSModule* m, int request, JSString* name, Found found) {
    std::string spec = request >= 0 && (size_t)request < m->requests.size() ? utf16to8(m->requests[request]->s) : "";
    if (found == Found::Ambiguous)
        vm.throwError(Err::SyntaxError, "The requested module '%s' contains conflicting star exports for name '%s'", spec.c_str(), utf16to8(name->s).c_str());
    vm.throwError(Err::SyntaxError, "The requested module '%s' does not provide an export named '%s'", spec.c_str(), utf16to8(name->s).c_str());
}

// init(module, meta): ECMA-262 16.2.1.7.3.1 InitializeEnvironment. Checks that every import and re-export can be
// resolved, then creates the module's scope with its function declarations in place. The body does not run yet.
JSFN(moduleInitialize) {
    Vm& vm = ci.vm;
    JSModule* m = moduleArg(ci, 0);
    if (m->deps.size() != m->requests.size()) vm.throwError(Err::TypeError, "Module is not loaded");
    for (auto& e : m->exports) {
        if (e.request < 0) continue;
        ResolveSet seen;
        Resolution r = resolveExport(vm, m, e.name, seen);
        if (r.found != Found::Yes) linkError(vm, m, e.request, e.importName ? e.importName : e.name, r.found);
    }
    for (auto& im : m->imports) {
        ResolveSet seen;
        Resolution r = resolveExport(vm, m->deps[im.first], im.second, seen);
        if (r.found != Found::Yes) linkError(vm, m, im.first, im.second, r.found);
    }
    Value args[2] = {V(m), ci.arg(1)};
    JSGenerator* g = (JSGenerator*)vm.startGenerator(m->fn, Value::undef(), args, 2).asCell();
    g->proto = nullptr;
    g->isAsync = true;
    m->scope = nullptr;
    m->activation = g;
    bool done = false;
    vm.resumeGenerator(g, Value::undef(), 0, done);  // runs up to the pause placed before the first statement
    return Value::undef();
}

// run(module): ECMA-262 16.2.1.7.3.2 ExecuteModule. Without top-level await the body runs to completion here and
// its errors are thrown; with it, the result is a promise that settles when the body finishes.
JSFN(moduleRun) {
    Vm& vm = ci.vm;
    JSModule* m = moduleArg(ci, 0);
    JSGenerator* g = m->activation;
    if (!g) vm.throwError(Err::TypeError, "Module is not linked");
    if (!m->hasTLA) {
        bool done = false;
        vm.resumeGenerator(g, Value::undef(), 0, done);
        return Value::undef();
    }
    JSPromise* p = vm.newPromise();
    g->promise = Value::cell(p);
    vm.asyncStep(g, Value::undef(), 0);
    return Value::cell(p);
}

JSFN(moduleSetImport) {
    ci.vm.importHook = ci.arg(0);
    return Value::undef();
}

// The loader. These are the specification's algorithms, kept in its order and naming so they can be read side by
// side: LoadRequestedModules, Link, Evaluate (with top-level await) and dynamic import (ECMA-262 16.2.1.6, 13.3.10).
const char kModuleLoader[] = R"JS((function (compile, bind, init, run, setImport) {
'use strict';
var G = globalThis, $Promise = G.Promise, $Map = G.Map, $Set = G.Set, $TypeError = G.TypeError, $SyntaxError = G.SyntaxError, $Object = G.Object,
    min = G.Math.min, parseJSON = G.JSON.parse;
// A module request is a specifier with its import attributes (only `type` is known here): both go into its key.
function requestKey(specifier, type) { return type === undefined ? specifier : specifier + '\u0000' + type; }
var fetching = new $Map();  // the module map: URL -> promise of its record
var byFile = new $Map();    // the name a module was compiled under -> record, to find the referrer of import()
var asyncCount = 0;
var host = {
  resolve: function (specifier, referrer) { throw new $TypeError("Cannot resolve module specifier '" + specifier + "'"); },
  fetch: function (url) { return $Promise.reject(new $TypeError('Cannot load module ' + url)); }
};
function capability() {
  var c = {};
  c.promise = new $Promise(function (resolve, reject) { c.resolve = resolve; c.reject = reject; });
  return c;
}

// A Cyclic Module Record. status: new, unlinked, linking, linked, evaluating, evaluating-async, evaluated.
function Record(file, source, base) {
  var parsed = compile(source, file), self = this;
  this.file = file;
  this.base = base;
  this.ns = parsed[0];
  this.specifiers = parsed[1];
  this.types = parsed[3];
  this.requested = [];  // the key of each request
  for (var i = 0; i < parsed[1].length; i++) this.requested[i] = requestKey(parsed[1][i], parsed[3][i]);
  this.hasTLA = parsed[2];
  this.status = 'new';
  this.hasError = false;      // [[EvaluationError]] is set
  this.error = undefined;
  this.ancestor = 0;          // [[DFSAncestorIndex]]
  this.loaded = new $Map();   // [[LoadedModules]]: specifier -> record
  this.cycleRoot = null;
  this.asyncOrder = undefined;  // [[AsyncEvaluationOrder]]: unset, then an integer, then 'done'
  this.capability = null;       // [[TopLevelCapability]]
  this.asyncParents = [];
  this.pendingAsync = 0;
  this.meta = $Object.create(null);
  this.meta.url = base;
  this.meta.resolve = function (specifier) { return host.resolve(`${specifier}`, self.base); };
  byFile.set(file, this);
}

// ---- loading
function loadRequestedModules(module) {
  var state = { isLoading: true, pending: 1, visited: new $Set(), capability: capability() };
  innerModuleLoading(state, module);
  return state.capability.promise;
}
function innerModuleLoading(state, module) {
  if (module.status === 'new' && !state.visited.has(module)) {
    state.visited.add(module);
    state.pending += module.requested.length;
    for (var i = 0; i < module.requested.length; i++) {
      var record = module.loaded.get(module.requested[i]);
      if (record) innerModuleLoading(state, record);
      else loadImportedModule(module, module.specifiers[i], module.types[i], state);
      if (!state.isLoading) return;
    }
  }
  state.pending--;
  if (state.pending === 0) {
    state.isLoading = false;
    state.visited.forEach(function (loaded) {
      if (loaded.status !== 'new') return;
      bind(loaded.ns, loaded.requested.map(function (s) { return loaded.loaded.get(s).ns; }));
      loaded.status = 'unlinked';
    });
    state.capability.resolve(undefined);
  }
}
function continueModuleLoading(state, ok, value) {
  if (!state.isLoading) return;
  if (ok) innerModuleLoading(state, value);
  else { state.isLoading = false; state.capability.reject(value); }
}
function fetchRecord(url, type) {
  var key = requestKey(url, type), p = fetching.get(key);
  if (!p) {
    p = new $Promise(function (resolve) { resolve(host.fetch(url, type)); }).then(function (r) {
      var source = typeof r === 'string' ? r : r.source, base = typeof r === 'string' ? url : r.url || url;
      if (type === undefined) return new Record(url, source, base);
      // A synthetic module (16.2.1.8): its one export, default, is the parsed JSON or the text itself
      if (type !== 'json' && type !== 'text') throw new $TypeError("Module type '" + type + "' is not supported");
      var value = type === 'json' ? parseJSON(source) : source;
      var record = new Record(key, 'export default import.meta.value;', base);
      record.meta = { value: value };
      return record;
    });
    fetching.set(key, p);
  }
  return p;
}
// HostLoadImportedModule, ending in FinishLoadingImportedModule. The referrer is a module record, or the URL of
// a classic script that called import().
function loadImportedModule(referrer, specifier, type, payload) {
  var isRecord = typeof referrer !== 'string', promise, key = requestKey(specifier, type);
  try { promise = fetchRecord(host.resolve(specifier, isRecord ? referrer.base : referrer), type); }
  catch (e) { promise = $Promise.reject(e); }
  promise.then(function (record) {
    if (isRecord && !referrer.loaded.has(key)) referrer.loaded.set(key, record);
    finishLoading(payload, true, record);
  }, function (error) { finishLoading(payload, false, error); });
}
function finishLoading(payload, ok, value) {
  if (payload.visited) continueModuleLoading(payload, ok, value);
  else continueDynamicImport(payload, ok, value);
}

// ---- linking
function link(module) {
  var stack = [];
  try { innerModuleLinking(module, stack, 0); }
  catch (e) {
    for (var i = 0; i < stack.length; i++) stack[i].status = 'unlinked';
    throw e;
  }
}
function innerModuleLinking(module, stack, index) {
  if (module.status !== 'unlinked') return index;  // linking, linked, evaluating-async or evaluated
  module.status = 'linking';
  var moduleIndex = index;
  module.ancestor = index;
  index++;
  stack.push(module);
  for (var i = 0; i < module.requested.length; i++) {
    var required = module.loaded.get(module.requested[i]);
    index = innerModuleLinking(required, stack, index);
    if (required.status === 'linking') module.ancestor = min(module.ancestor, required.ancestor);
  }
  init(module.ns, module.meta);
  if (module.ancestor === moduleIndex) {
    for (;;) {
      var m = stack.pop();
      m.status = 'linked';
      if (m === module) break;
    }
  }
  return index;
}

// ---- evaluation
function evaluate(module) {
  if ((module.status === 'evaluating-async' || module.status === 'evaluated') && module.cycleRoot) module = module.cycleRoot;
  if (module.capability) return module.capability.promise;
  var stack = [], cap = capability(), failed = false, error;
  module.capability = cap;
  try { innerModuleEvaluation(module, stack, 0); } catch (e) { failed = true; error = e; }
  if (failed) {
    for (var i = 0; i < stack.length; i++) {
      stack[i].status = 'evaluated';
      stack[i].hasError = true;
      stack[i].error = error;
    }
    cap.reject(error);
  } else if (module.status === 'evaluated') cap.resolve(undefined);
  return cap.promise;
}
function innerModuleEvaluation(module, stack, index) {
  if (module.status === 'evaluating-async' || module.status === 'evaluated') {
    if (!module.hasError) return index;
    throw module.error;
  }
  if (module.status === 'evaluating') return index;
  module.status = 'evaluating';
  var moduleIndex = index;
  module.ancestor = index;
  module.pendingAsync = 0;
  index++;
  stack.push(module);
  for (var i = 0; i < module.requested.length; i++) {
    var required = module.loaded.get(module.requested[i]);
    index = innerModuleEvaluation(required, stack, index);
    if (required.status === 'evaluating') module.ancestor = min(module.ancestor, required.ancestor);
    else {
      required = required.cycleRoot;
      if (required.hasError) throw required.error;
    }
    if (typeof required.asyncOrder === 'number') {
      module.pendingAsync++;
      required.asyncParents.push(module);
    }
  }
  if (module.hasTLA || module.pendingAsync > 0) {
    module.asyncOrder = asyncCount++;
    if (module.pendingAsync === 0) executeAsyncModule(module);
  } else run(module.ns);
  if (module.ancestor === moduleIndex) {
    for (;;) {
      var m = stack.pop();
      m.status = m.asyncOrder === undefined ? 'evaluated' : 'evaluating-async';
      m.cycleRoot = module;
      if (m === module) break;
    }
  }
  return index;
}
function executeAsyncModule(module) {
  var done;
  try { done = run(module.ns); } catch (e) { done = $Promise.reject(e); }
  done.then(function () { asyncModuleExecutionFulfilled(module); }, function (error) { asyncModuleExecutionRejected(module, error); });
}
function gatherAvailableAncestors(module, execList) {
  for (var i = 0; i < module.asyncParents.length; i++) {
    var m = module.asyncParents[i];
    if (execList.indexOf(m) >= 0 || m.cycleRoot.hasError) continue;
    m.pendingAsync--;
    if (m.pendingAsync === 0) {
      execList.push(m);
      if (!m.hasTLA) gatherAvailableAncestors(m, execList);
    }
  }
}
function asyncModuleExecutionFulfilled(module) {
  if (module.status === 'evaluated') return;
  module.asyncOrder = 'done';
  module.status = 'evaluated';
  if (module.capability) module.capability.resolve(undefined);
  var execList = [];
  gatherAvailableAncestors(module, execList);
  execList.sort(function (a, b) { return a.asyncOrder - b.asyncOrder; });
  for (var i = 0; i < execList.length; i++) {
    var m = execList[i];
    if (m.status === 'evaluated') continue;
    if (m.hasTLA) { executeAsyncModule(m); continue; }
    var failed = false, error;
    try { run(m.ns); } catch (e) { failed = true; error = e; }
    if (failed) asyncModuleExecutionRejected(m, error);
    else {
      m.asyncOrder = 'done';
      m.status = 'evaluated';
      if (m.capability) m.capability.resolve(undefined);
    }
  }
}
function asyncModuleExecutionRejected(module, error) {
  if (module.status === 'evaluated') return;
  module.hasError = true;
  module.error = error;
  module.status = 'evaluated';
  module.asyncOrder = 'done';
  if (module.capability) module.capability.reject(error);
  for (var i = 0; i < module.asyncParents.length; i++) asyncModuleExecutionRejected(module.asyncParents[i], error);
}

// ---- import()
function importCall(specifier, options, file) {
  var cap = capability();
  try {
    var spec = `${specifier}`, type;
    if (options !== undefined) {
      if ($Object(options) !== options) throw new $TypeError('The second argument of import() must be an object');
      var attributes = options.with;
      if (attributes !== undefined) {
        if ($Object(attributes) !== attributes) throw new $TypeError("The 'with' option of import() must be an object");
        var entries = $Object.entries(attributes);
        for (var i = 0; i < entries.length; i++)
          if (typeof entries[i][1] !== 'string') throw new $TypeError('Import attribute values must be strings');
        for (var i = 0; i < entries.length; i++) {
          if (entries[i][0] !== 'type') throw new $SyntaxError("Import attribute '" + entries[i][0] + "' is not supported");
          type = entries[i][1];
        }
      }
    }
    loadImportedModule(byFile.get(file) || `${file}`, spec, type, cap);
  } catch (e) { cap.reject(e); }
  return cap.promise;
}
function continueDynamicImport(cap, ok, value) {
  if (!ok) { cap.reject(value); return; }
  loadRequestedModules(value).then(function () {
    try { link(value); } catch (e) { cap.reject(e); return; }
    evaluate(value).then(function () { cap.resolve(value.ns); }, cap.reject);
  }, cap.reject);
}
setImport(importCall);

return {
  host: host,
  // Fetches a module (or takes its source as given) and loads everything it imports -> promise of a handle
  load: function (file, source, base) {
    var first = source === undefined || source === null ? fetchRecord(file)
      : new $Promise(function (resolve) { resolve(new Record(file, source, base || file)); });
    return first.then(function (record) { return loadRequestedModules(record).then(function () { return record; }); });
  },
  // Links and evaluates a loaded module. Link errors are thrown; the promise follows the evaluation.
  run: function (record) { link(record); return evaluate(record); },
  namespace: function (record) { return record.ns; },
  import: function (specifier, referrer) { return importCall(specifier, undefined, referrer); }
};
}))JS";

}  // namespace

// OP_ModuleInit, the first thing a module body does: its scope becomes the module environment and the
// namespaces of the modules it imports are put where its code expects them.
void moduleInit(Vm& vm, Value nsv, Context* ctx) {
    (void)vm;
    if (!isModule(nsv)) return;
    JSModule* m = (JSModule*)nsv.asObject();
    m->scope = ctx;
    for (size_t i = 0; i < m->requestSlots.size() && i < m->deps.size(); ++i) {
        int slot = m->requestSlots[i];
        if (ctx && slot >= 0 && (size_t)slot < ctx->slots.size()) ctx->slots[slot] = V(m->deps[i]);
    }
}

void setupModules(Vm& vm) {
    auto src = std::make_shared<const std::u16string>(utf8to16(kModuleLoader));
    FuncProto* p = compileScript(vm, src, "<modules>", false, nullptr);
    JSFunction* outer = vm.makeClosure(p, nullptr, nullptr);
    Value make = vm.callFunction(outer, V(vm.global), nullptr, 0, Value::undef(), false);
    Value natives[5] = {V(vm.newNative(moduleCompile, "compile", 2)), V(vm.newNative(moduleBind, "bind", 2)), V(vm.newNative(moduleInitialize, "init", 2)),
                        V(vm.newNative(moduleRun, "run", 1)), V(vm.newNative(moduleSetImport, "setImport", 1))};
    Value api = vm.call(make, Value::undef(), natives, 5);
    vm.def(vm.global, "__modules", api, PF_W | PF_C);
}

}  // namespace tb::js
