// DOM bindings: exposes a page's document to the JavaScript engine and runs its scripts, events and timers.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "css.h"
#include "html.h"
#include "js/builtins.h"
#include "net.h"
#include "script.h"
#include "script_prelude.h"
#include "url.h"

namespace tb {

using namespace js;

namespace {
enum : uint8_t { HK_Node = 1, HK_ClassList, HK_Style, HK_Dataset, HK_Event, HK_Storage, HK_Computed, HK_Observer };

struct Listener {
    std::string type;
    Value fn;
    bool capture = false, once = false;
};
struct Timer {
    int id = 0;
    double due = 0, interval = -1;
    Value fn;
    std::vector<Value> args;
    bool frame = false;  // requestAnimationFrame: the callback receives a timestamp
};
using Store = std::vector<std::pair<std::u16string, std::u16string>>;

// Results of network requests come back from worker threads through this; it outlives the page if needed.
struct NetShared {
    std::mutex mu;
    std::vector<std::pair<int, FetchResult>> done;
    std::atomic<int> inflight{0};
    std::function<void()> wake;
    bool alive = true;
};
struct ObserveTarget {
    Node* node;
    bool childList, attributes, characterData, subtree;
    bool filtered = false;            // attributeFilter was given: only these attributes are reported
    std::vector<std::string> filter;
};
struct Observer {  // a MutationObserver
    Value object, callback;
    std::vector<ObserveTarget> targets;
    std::vector<Value> records;
};

std::string headerValue(const std::string& raw, const char* lowerName) {
    size_t i = 0;
    while (i < raw.size()) {
        size_t e = raw.find("\r\n", i);
        if (e == std::string::npos) e = raw.size();
        std::string_view line(raw.data() + i, e - i);
        size_t c = line.find(':');
        if (c != std::string_view::npos && lower(trim(line.substr(0, c))) == lowerName) return std::string(trim(line.substr(c + 1)));
        i = e + 2;
    }
    return "";
}
// A request made by script: adds the cross-origin (CORS) checks on top of a plain fetch. Runs on a worker thread.
FetchResult corsFetch(const std::string& url, FetchOpts o, bool crossOrigin, const std::string& origin, bool credentials) {
    o.wantHeaders = true;
    auto refuse = [&](const char* why) {
        FetchResult bad;
        bad.error = why;
        bad.finalUrl = url;
        return bad;
    };
    if (crossOrigin) {
        o.sendOrigin = true;
        bool simpleMethod = o.method == "GET" || o.method == "HEAD" || o.method == "POST";
        std::string custom;
        for (auto& h : o.headers) {
            std::string n = lower(h.first);
            if (n != "accept" && n != "accept-language" && n != "content-language") custom += (custom.empty() ? "" : ",") + n;
        }
        std::string ct = lower(o.contentType);
        if (!(ct.empty() || istarts(ct, "text/plain") || istarts(ct, "application/x-www-form-urlencoded") || istarts(ct, "multipart/form-data")))
            custom += (custom.empty() ? "" : ",") + std::string("content-type");
        if (!simpleMethod || !custom.empty()) {  // ask the server first (preflight)
            FetchOpts pf;
            pf.purpose = FetchOpts::For::Other;
            pf.method = "OPTIONS";
            pf.referrer = o.referrer;
            pf.firstPartySite = o.firstPartySite;
            pf.wantHeaders = true;
            pf.headers.push_back({"Access-Control-Request-Method", o.method});
            if (!custom.empty()) pf.headers.push_back({"Access-Control-Request-Headers", custom});
            FetchResult p = fetch(url, pf);
            std::string allow = headerValue(p.rawHeaders, "access-control-allow-origin");
            if (!p.error.empty() || p.status < 200 || p.status >= 300 || !(allow == "*" || allow == origin))
                return refuse("the server did not allow this cross-origin request (CORS preflight)");
            auto listed = [](const std::string& list, const std::string& item) {
                size_t i = 0;
                while (i <= list.size()) {
                    size_t e = list.find(',', i);
                    if (e == std::string::npos) e = list.size();
                    std::string_view one = trim(std::string_view(list).substr(i, e - i));
                    if (one == "*" || one == item) return true;
                    i = e + 1;
                }
                return false;
            };
            std::string methods = lower(headerValue(p.rawHeaders, "access-control-allow-methods"));
            std::string allowedHeaders = lower(headerValue(p.rawHeaders, "access-control-allow-headers"));
            if (!simpleMethod && !listed(methods, lower(o.method))) return refuse("the server does not allow this method from other sites (CORS preflight)");
            size_t at = 0;
            while (at < custom.size()) {
                size_t e = custom.find(',', at);
                if (e == std::string::npos) e = custom.size();
                if (!listed(allowedHeaders, custom.substr(at, e - at))) return refuse("the server does not allow this request header from other sites (CORS preflight)");
                at = e + 1;
            }
        }
    }
    FetchResult r = fetch(url, o);
    if (crossOrigin && r.error.empty()) {
        std::string allow = headerValue(r.rawHeaders, "access-control-allow-origin");
        if (!(allow == origin || (allow == "*" && !credentials))) return refuse("blocked by the same-origin policy: the response has no matching Access-Control-Allow-Origin header");
    }
    return r;
}
// What a <script> element holds, going by its type attribute; -1 for data blocks the browser does not run.
int scriptKind(const Node* n) {
    const std::string* type = n->attr("type");
    std::string ty = type ? lower(std::string(trim(*type))) : "";
    if (ty == "module") return ScriptSource::Module;
    if (ty == "importmap") return ScriptSource::ImportMap;
    bool classic = ty.empty() || ty == "text/javascript" || ty == "application/javascript" || ty == "application/x-javascript" || ty == "text/ecmascript" ||
                   ty == "application/ecmascript" || ty == "text/jscript" || ty == "javascript";
    return classic ? ScriptSource::Classic : -1;
}
}  // namespace

namespace {
void setAttr(Node* n, const std::string& name, const std::string& value);  // the element's own bookkeeping, defined below
void removeAttr(Node* n, const std::string& name);
}  // namespace

struct ScriptHost::Impl {
    Document& doc;
    std::string url;
    Client& client;
    Vm vm;
    std::unordered_map<Node*, JSHost*> wrappers;               // one JavaScript object per node, kept for the page's life
    std::unordered_map<Node*, std::unique_ptr<Node>> orphans;  // nodes that are not in the tree (new or removed)
    std::unordered_map<const void*, std::vector<Listener>> listeners;  // keyed by node; the window uses `this`
    std::vector<Timer> timers;
    std::unordered_map<std::string, std::vector<Selector>> selectorCache;
    Store localStore, sessionStore;
    JSObject *nodeProto = nullptr, *elementProto = nullptr, *documentProto = nullptr, *textProto = nullptr, *eventProto = nullptr,
             *classListProto = nullptr, *styleProto = nullptr, *computedProto = nullptr, *nodeListProto = nullptr, *storageProto = nullptr;
    Value currentScript = Value::null();
    // The session history entries that belong to this document (HTML 7.4.1), each with its URL and its classic
    // history API state. The shell keeps the tab's whole history and says which of these is the current one.
    struct SessionEntry {
        std::string url;
        Value state = Value::null();
    };
    std::vector<SessionEntry> session;
    size_t sessionAt = 0;
    bool manualScrollRestoration = false;
    std::vector<std::pair<std::string, std::string>> hashChanges;  // old and new URL of hashchange events still owed
    JSObject* historyObject = nullptr;
    JSNative* allCollection = nullptr;  // document.all
    // The prototype of each event interface, by its name ("MouseEvent"). Every interface has one of its own that
    // inherits from its parent interface's (DOM 2.2, UI Events 3, HTML 8.1.8.3).
    std::unordered_map<std::string, JSObject*> eventProtos;
    Value domException, cloneFn;  // the prelude's DOMException class and structuredClone
    std::string readyState = "loading";
    double startMs = now_ms();
    int nextTimer = 1, depth = 0;
    bool dead = false;
    std::shared_ptr<NetShared> net = std::make_shared<NetShared>();
    std::unordered_map<int, std::function<void(FetchResult&)>> netCallbacks;
    std::unordered_map<int, Value> netJs;  // JavaScript callbacks of requests in flight
    int nextNet = 1;
    std::unordered_set<Node*> startedScripts;
    std::unordered_map<std::string, JSObject*> tagProtos;  // HTMLInputElement.prototype for <input>, and so on
    JSObject* fragmentProto = nullptr;
    JSObject* shadowProto = nullptr;  // ShadowRoot.prototype
    std::vector<std::unique_ptr<Observer>> observers;
    bool observersScheduled = false;
    Value deliverFn;
    JSObject* observerProto = nullptr;
    Value pageScripts;                  // the prelude's object that runs module scripts
    std::vector<ScriptSource> deferred;  // scripts that wait for the document to be parsed
    bool loadPending = false;           // deferred scripts are still running; the load events come after them
    bool loadDue = false;               // they are done: fire the load events on the next pump
    // Custom elements (HTML 4.13). The registry and the callbacks are in the prelude. This side knows which
    // elements are custom (Node::ceState), notes what happens to them while the DOM changes, and hands the notes
    // to the prelude once the operation that caused them is over ([CEReactions], 4.13.6).
    struct Reaction {
        Node* node;
        int kind;  // 0 connected, 1 disconnected, 2 attribute changed, 3 an element that may be waiting for its definition
        std::string name, oldValue, newValue;
        bool hadOld = false, hasNew = false;
    };
    std::deque<Reaction> reactions;
    std::unordered_map<Node*, std::vector<std::string>> adopted;  // adoptedStyleSheets of the document node and of shadow roots, as text
    Value ceHook;  // the prelude's function that carries a reaction out; set when the first element is defined

    Impl(Document& d, const std::string& u, Client& c) : doc(d), url(u), client(c) {}

    Value wrap(Node* n) {
        if (!n) return Value::null();
        auto it = wrappers.find(n);
        if (it != wrappers.end()) return V(it->second);
        JSHost* h = vm.alloc<JSHost>();
        h->hostKind = HK_Node;
        h->ptr = n;
        if (n->type == Node::T::Element) {
            auto tp = tagProtos.find(n->name);
            h->proto = tp != tagProtos.end() ? tp->second : n->isShadowRoot() ? shadowProto : isFragment(n) ? fragmentProto : elementProto;
        } else
            h->proto = n->type == Node::T::Document ? documentProto : textProto;
        wrappers[n] = h;
        return V(h);
    }
    Value list(const std::vector<Node*>& nodes) {
        JSArray* a = vm.newArray();
        a->proto = nodeListProto;
        for (Node* n : nodes) a->elems.push_back(wrap(n));
        return V(a);
    }
    Node* docNode() { return doc.root.get(); }
    void changed() { client.domChanged(); }
    void reportError() {
        Value e = vm.pending;
        vm.pending = Value::undef();
        if (vm.terminated) return;
        std::u16string text = vm.describeError(e);
        if (text.rfind(u"Uncaught", 0) != 0) text = u"Uncaught " + text;
        client.consoleMessage(3, std::wstring(text.begin(), text.end()));
    }

    // ---- tree surgery
    static void reindex(Node* p) {
        uint32_t count = 0, idx = 0;
        for (auto& k : p->kids)
            if (k->type == Node::T::Element) ++count;
        for (auto& k : p->kids)
            if (k->type == Node::T::Element) {
                k->elemIndex = idx++;
                k->elemCount = count;
            }
    }
    Node* adopt(std::unique_ptr<Node> n) {
        Node* raw = n.get();
        raw->parent = nullptr;
        orphans[raw] = std::move(n);
        return raw;
    }
    Node* newElement(const std::string& tag) {
        auto n = std::make_unique<Node>();
        n->type = Node::T::Element;
        n->name = lower(tag);
        if (n->name == "template") n->content = fragment();
        return adopt(std::move(n));
    }
    static std::unique_ptr<Node> fragment() {
        auto f = std::make_unique<Node>();
        f->type = Node::T::Element;
        f->name = "#document-fragment";
        return f;
    }
    Node* newText(const std::string& text, Node::T type = Node::T::Text) {
        auto n = std::make_unique<Node>();
        n->type = type;
        n->text = text;
        return adopt(std::move(n));
    }
    std::unique_ptr<Node> take(Node* n) {
        if (Node* p = n->parent) {
            for (size_t i = 0; i < p->kids.size(); ++i)
                if (p->kids[i].get() == n) {
                    std::unique_ptr<Node> u = std::move(p->kids[i]);
                    p->kids.erase(p->kids.begin() + i);
                    n->parent = nullptr;
                    reindex(p);
                    return u;
                }
        }
        auto it = orphans.find(n);
        if (it == orphans.end()) return nullptr;
        std::unique_ptr<Node> u = std::move(it->second);
        orphans.erase(it);
        return u;
    }
    static bool isFragment(const Node* n) { return n->type == Node::T::Element && n->name == "#document-fragment"; }
    void insert(Node* parent, Node* child, Node* before) {
        for (Node* a = parent; a; a = a->parent)
            if (a == child) vm.throwError(Err::Error, "HierarchyRequestError: the new child contains the parent");
        if (isFragment(child)) {
            std::vector<Node*> kids;
            for (auto& k : child->kids) kids.push_back(k.get());
            for (Node* k : kids) insert(parent, k, before);
            return;
        }
        if (child == before) {  // inserting a node before itself leaves it where it is
            if (child->parent == parent) return;
            before = nullptr;
        }
        bool wasConnected = connected(child);
        std::unique_ptr<Node> u = take(child);
        if (!u) return;
        if (wasConnected) noteTree(child, false);
        u->parent = parent;
        size_t pos = parent->kids.size();
        if (before)
            for (size_t i = 0; i < parent->kids.size(); ++i)
                if (parent->kids[i].get() == before) {
                    pos = i;
                    break;
                }
        parent->kids.insert(parent->kids.begin() + pos, std::move(u));
        reindex(parent);
        changed();
        record("childList", parent, child, nullptr, nullptr);
        if (connected(parent)) {
            std::vector<Node*> scripts;
            collectScripts(child, scripts);
            for (Node* sc : scripts) startScript(sc);
            noteTree(child, true);
        }
    }
    void remove(Node* n) {
        Node* parent = n->parent;
        if (!parent) return;
        bool wasConnected = connected(n);
        std::unique_ptr<Node> u = take(n);
        if (u) orphans[n] = std::move(u);
        changed();
        record("childList", parent, nullptr, n, nullptr);
        if (wasConnected) noteTree(n, false);
    }
    void clearChildren(Node* el) {
        bool wasConnected = connected(el);
        std::vector<Node*> gone;
        for (auto& k : el->kids) {
            Node* raw = k.get();
            raw->parent = nullptr;
            gone.push_back(raw);
            orphans[raw] = std::move(k);
        }
        el->kids.clear();
        for (Node* g : gone) {
            record("childList", el, nullptr, g, nullptr);
            if (wasConnected) noteTree(g, false);
        }
    }
    static bool connected(const Node* n) {
        while (n->parent) n = n->parent;
        return n->type == Node::T::Document;
    }
    static void collectScripts(Node* n, std::vector<Node*>& out) {
        if (n->type != Node::T::Element) return;
        if (n->name == "script") out.push_back(n);
        else
            for (auto& k : n->kids) collectScripts(k.get(), out);
    }

    // ---- custom element reactions
    static bool maybeCustom(const Node* n) { return n->type == Node::T::Element && n->name.find('-') != std::string::npos; }
    template <class F>
    static void eachElementOf(Node* root, F&& fn) {  // the elements below root, in tree order
        std::vector<Node*> st;
        for (auto it = root->kids.rbegin(); it != root->kids.rend(); ++it) st.push_back(it->get());
        while (!st.empty()) {
            Node* n = st.back();
            st.pop_back();
            if (n->type != Node::T::Element) continue;
            fn(n);
            for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) st.push_back(it->get());
        }
    }
    // A subtree has entered or left the document: its custom elements hear of it, and on the way in those that
    // may be waiting for a definition are looked at.
    void noteTree(Node* root, bool entering) {
        if (!ceHook.isObject()) return;
        std::vector<Node*> st{root};
        while (!st.empty()) {
            Node* n = st.back();
            st.pop_back();
            if (n->type != Node::T::Element) continue;
            if (n->isShadowRoot()) {
                for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) st.push_back(it->get());
                continue;
            }
            if (n->ceState == 1) reactions.push_back({n, entering ? 0 : 1});
            else if (entering && n->ceState == 0 && maybeCustom(n)) reactions.push_back({n, 3});
            for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) st.push_back(it->get());
            if (n->shadow) st.push_back(n->shadow.get());  // a shadow tree enters and leaves with its host
        }
    }
    // Elements are served in the order they were first noted, and each gets everything noted for it at once:
    // that is how the standard's element queue with a reaction queue per element comes out (4.13.6).
    void flushReactions() {
        while (!reactions.empty() && !dead) {
            Node* n = reactions.front().node;
            for (;;) {
                auto it = std::find_if(reactions.begin(), reactions.end(), [n](const Reaction& x) { return x.node == n; });
                if (it == reactions.end() || dead) break;
                Reaction r = std::move(*it);
                reactions.erase(it);
                carryOut(r);
            }
        }
    }
    void carryOut(const Reaction& r) {
        std::vector<Value> args{Value::i32(r.kind), wrap(r.node)};
        RootScope keep(vm, args);
        args.push_back(val8(r.name));
        args.push_back(r.hadOld ? val8(r.oldValue) : Value::null());
        args.push_back(r.hasNew ? val8(r.newValue) : Value::null());
        try {
            vm.call(ceHook, Value::undef(), args.data(), (int)args.size());
        } catch (JSThrow&) {
            reportError();
        }
    }
    Value val8(const std::string& s) { return vm.strv(utf8to16(s)); }
    // Every change of an attribute made by a script goes through here. value: null removes the attribute.
    void changeAttribute(Node* n, const std::string& name, const std::string* value) {
        const std::string* now = n->attr(name);
        if (!now && !value) return;
        Reaction r{n, 2, name};
        if (now) r.hadOld = true, r.oldValue = *now;
        if (value) r.hasNew = true, r.newValue = *value;
        if (value) setAttr(n, name, *value);
        else removeAttr(n, name);
        attrChanged(n, name);
        if (n->name == "img" && (name == "src" || name == "srcset" || name == "data-src")) client.imageSourceChanged(n);
        if (n->ceState == 1) reactions.push_back(std::move(r));
    }

    // ---- MutationObserver
    void record(const char* type, Node* target, Node* added, Node* removed, const std::string* attrName) {
        if (observers.empty()) return;
        for (auto& ob : observers) {
            bool hit = false;
            for (auto& t : ob->targets) {
                bool wants = !strcmp(type, "childList") ? t.childList : !strcmp(type, "attributes") ? t.attributes : t.characterData;
                if (!wants) continue;
                if (attrName && t.filtered && std::find(t.filter.begin(), t.filter.end(), *attrName) == t.filter.end()) continue;
                if (t.node == target) hit = true;
                else if (t.subtree && !target->isShadowRoot())
                    for (Node* a = target->parent; a && !hit; a = a->parent) {
                        hit = a == t.node;
                        if (a->isShadowRoot()) break;  // a shadow tree is a tree of its own: observers above it do not see inside
                    }
                if (hit) break;
            }
            if (!hit) continue;
            JSObject* r = vm.newObject();
            vm.def(r, "type", vm.strv(type), PF_ALL);
            vm.def(r, "target", wrap(target), PF_ALL);
            std::vector<Node*> a, d;
            if (added) a.push_back(added);
            if (removed) d.push_back(removed);
            vm.def(r, "addedNodes", list(a), PF_ALL);
            vm.def(r, "removedNodes", list(d), PF_ALL);
            vm.def(r, "attributeName", attrName ? vm.strv(utf8to16(*attrName)) : Value::null(), PF_ALL);
            vm.def(r, "oldValue", Value::null(), PF_ALL);
            vm.def(r, "previousSibling", Value::null(), PF_ALL);
            vm.def(r, "nextSibling", Value::null(), PF_ALL);
            ob->records.push_back(Value::cell(r));
            if (!observersScheduled) {
                observersScheduled = true;
                vm.enqueueJob({3, deliverFn, Value::undef(), Value::undef()});
            }
        }
    }
    void attrChanged(Node* n, const std::string& name) {
        changed();
        record("attributes", n, nullptr, nullptr, &name);
        if (name == "src" && n->name == "script" && connected(n)) startScript(n);
    }

    // ---- scripts added after the page loaded, and network requests made by script
    void runCode(Node* script, const std::string& code, const std::string& file) {
        Value prev = currentScript;
        currentScript = wrap(script);
        try {
            vm.eval(utf8to16(code), file);
        } catch (JSThrow&) {
            reportError();
        }
        currentScript = prev;
    }
    void fireSimple(Node* target, const char* type) {
        try {
            fire(target, makeEvent(type, false, false));
        } catch (JSThrow&) {
            reportError();
        }
    }
    int startFetch(const std::string& u, const FetchOpts& opts, bool crossOrigin, const std::string& origin, bool credentials, std::function<void(FetchResult&)> cb) {
        int id = nextNet++;
        netCallbacks[id] = std::move(cb);
        net->inflight++;
        std::shared_ptr<NetShared> sh = net;
        std::thread([sh, id, u, opts, crossOrigin, origin, credentials] {
            FetchResult r = corsFetch(u, opts, crossOrigin, origin, credentials);
            {
                std::lock_guard<std::mutex> lk(sh->mu);
                if (sh->alive) sh->done.push_back({id, std::move(r)});
            }
            sh->inflight--;
            if (sh->wake) sh->wake();
        }).detach();
        return id;
    }
    std::string firstParty() const {
        Url page = Url::parse(url);
        return page.valid && !page.host.empty() ? site_of(page.host) : std::string("local");
    }
    void callPage(const char* method, Value* args, int n) {
        if (!pageScripts.isObject()) return;
        try {
            vm.call(vm.get(pageScripts, method), pageScripts, args, n);
        } catch (JSThrow&) {
            reportError();
        }
    }
    void registerImportMap(const std::string& text) {
        Value arg = vm.strv(utf8to16(text));
        callPage("importMap", &arg, 1);
    }
    void startScript(Node* sc) {
        if (dead || !connected(sc)) return;
        int kind = scriptKind(sc);
        const std::string* src = sc->attr("src");
        if (kind == ScriptSource::Module) {  // a module script added by script: runs as soon as it is loaded
            bool external = src && !src->empty();
            std::string code = external ? std::string() : text_content(sc);
            if ((!external && (src || trim(code).empty())) || !startedScripts.insert(sc).second) return;
            Value args[3] = {wrap(sc), external ? vm.strv(utf8to16(resolve_url(doc.baseUrl, *src))) : Value::null(),
                             external ? Value::null() : vm.strv(utf8to16(code))};
            callPage("dynamic", args, 3);
            return;
        }
        if (kind == ScriptSource::ImportMap) {
            if (!src && startedScripts.insert(sc).second) registerImportMap(text_content(sc));
            return;
        }
        if (kind != ScriptSource::Classic || sc->attr("nomodule")) return;
        if (src && !src->empty()) {
            if (!startedScripts.insert(sc).second) return;
            std::string u = resolve_url(doc.baseUrl, *src);
            if (url.rfind("https:", 0) == 0 && u.rfind("http:", 0) == 0) {
                client.consoleMessage(2, L"Blocked insecure script on a secure page: " + widen(u));
                fireSimple(sc, "error");
                return;
            }
            FetchOpts o;
            o.purpose = FetchOpts::For::Other;
            o.referrer = url;
            o.firstPartySite = firstParty();
            startFetch(u, o, false, "", false, [this, sc, u](FetchResult& r) {
                if (!r.error.empty() || r.status >= 400) {
                    client.consoleMessage(2, L"Could not load script " + widen(u));
                    fireSimple(sc, "error");
                    return;
                }
                runCode(sc, bytes_to_utf8(r.body, r.contentType), u);
                fireSimple(sc, "load");
            });
        } else if (!src) {
            std::string code = text_content(sc);
            if (trim(code).empty() || !startedScripts.insert(sc).second) return;
            runCode(sc, code, url);
        }
    }
    void finishNet(int id, bool binary, FetchResult& r) {
        auto it = netJs.find(id);
        if (it == netJs.end()) return;
        Value cb = it->second;
        netJs.erase(it);
        Value args[6];
        netResult(r, binary, args);
        try {
            vm.call(cb, Value::undef(), args, 6);
        } catch (JSThrow&) {
            reportError();
        }
    }
    // error, status, statusText, final URL, response headers, body
    void netResult(FetchResult& r, bool binary, Value out[6]) {
        std::string statusText, headers = r.rawHeaders;
        size_t eol = headers.find("\r\n");
        if (eol != std::string::npos) {
            std::string first = headers.substr(0, eol);
            headers.erase(0, eol + 2);
            size_t sp = first.find(' ');
            sp = sp == std::string::npos ? sp : first.find(' ', sp + 1);
            if (sp != std::string::npos) statusText = first.substr(sp + 1);
        }
        if (headers.empty() && !r.contentType.empty()) headers = "content-type: " + r.contentType + "\r\n";
        out[0] = r.error.empty() ? Value::null() : vm.strv(utf8to16(r.error));
        out[1] = Value::i32(r.status);
        out[2] = vm.strv(utf8to16(statusText));
        out[3] = vm.strv(utf8to16(r.finalUrl));
        out[4] = vm.strv(utf8to16(headers));
        if (binary) {
            JSArrayBuffer* b = vm.alloc<JSArrayBuffer>();
            b->proto = vm.arrayBufferProto;
            b->bytes.assign(r.body.begin(), r.body.end());
            out[5] = Value::cell(b);
        } else
            out[5] = vm.strv(utf8to16(bytes_to_utf8(r.body, r.contentType)));
    }
    void setInner(Node* el, const std::string& html) {
        std::string pre = "<!doctype html><body>";
        std::vector<const char*> path;
        if (el->name == "tr") pre += "<table><tbody><tr>", path = {"table", "tbody", "tr"};
        else if (el->name == "tbody" || el->name == "thead" || el->name == "tfoot") pre += "<table><tbody>", path = {"table", "tbody"};
        else if (el->name == "table") pre += "<table>", path = {"table"};
        else if (el->name == "select") pre += "<select>", path = {"select"};
        else if (el->name == "ul" || el->name == "ol") pre += "<ul>", path = {"ul"};
        std::unique_ptr<Document> d = parse_html(pre + html, doc.url);
        bool inert = el->content != nullptr;
        if (inert) el = el->content.get();  // a template: the markup becomes its contents, which belong to no page
        Node* ctx = d->body;
        for (const char* name : path) {
            Node* next = nullptr;
            if (ctx)
                for (auto& k : ctx->kids)
                    if (k->is(name)) {
                        next = k.get();
                        break;
                    }
            ctx = next;
        }
        // elements made from markup are upgraded once the operation is over, wherever they end up: they are noted
        // as they are made, before anything is taken out (HTML 13.2.6.1, an element for a token of a fragment)
        if (ctx && ceHook.isObject() && !inert)
            eachElementOf(ctx, [&](Node* e) {
                if (maybeCustom(e)) reactions.push_back({e, 3});
            });
        clearChildren(el);
        if (ctx) {
            for (auto& k : ctx->kids) {
                k->parent = el;
                std::vector<Node*> inert;  // scripts that arrive through innerHTML never run
                collectScripts(k.get(), inert);
                for (Node* sc : inert) startedScripts.insert(sc);
                el->kids.push_back(std::move(k));
            }
            ctx->kids.clear();
        }
        reindex(el);
        changed();
        bool isConnected = connected(el);
        for (auto& k : el->kids) {
            record("childList", el, k.get(), nullptr, nullptr);
            if (isConnected) noteTree(k.get(), true);
        }
    }
    void setText(Node* n, const std::string& text) {
        if (n->type == Node::T::Text || n->type == Node::T::Comment) {
            n->text = text;
        } else if (n->type == Node::T::Element) {
            clearChildren(n);
            if (!text.empty()) {
                auto t = std::make_unique<Node>();
                t->type = Node::T::Text;
                t->text = text;
                n->append(std::move(t));
            }
        }
        changed();
        record(n->type == Node::T::Element ? "childList" : "characterData", n, nullptr, nullptr, nullptr);
    }
    Node* clone(const Node* src, bool deep) {
        auto n = std::make_unique<Node>();
        n->type = src->type == Node::T::Document ? Node::T::Element : src->type;
        n->name = src->name;
        n->text = src->text;
        n->id = src->id;
        n->attrs = src->attrs;
        n->classes = src->classes;
        Node* raw = adopt(std::move(n));
        if (ceHook.isObject() && maybeCustom(raw)) reactions.push_back({raw, 3});
        if (src->content) raw->content = deep ? take(clone(src->content.get(), true)) : fragment();
        if (deep)
            for (auto& k : src->kids) {
                Node* c = clone(k.get(), true);
                std::unique_ptr<Node> u = take(c);
                u->parent = raw;
                raw->kids.push_back(std::move(u));
            }
        reindex(raw);
        return raw;
    }

    // ---- selectors
    const std::vector<Selector>& selectors(const std::string& text) {
        auto it = selectorCache.find(text);
        if (it != selectorCache.end()) return it->second;
        if (selectorCache.size() > 600) selectorCache.clear();
        std::vector<Selector> out;
        bool bad = false;
        size_t start = 0;
        int nest = 0;
        char quote = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            char c = i < text.size() ? text[i] : ',';
            if (quote && i < text.size()) {
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '(' || c == '[') ++nest;
            else if (c == ')' || c == ']') --nest;
            else if (c == ',' && (nest <= 0 || i == text.size())) {
                std::string_view part = trim(std::string_view(text).substr(start, i - start));
                Selector s;
                if (!part.empty() && selector_parse(part, s)) out.push_back(std::move(s));
                else bad = true;
                start = i + 1;
            }
        }
        if (bad) vm.throwError(Err::SyntaxError, "'%s' is not a valid selector", text.c_str());
        return selectorCache.emplace(text, std::move(out)).first->second;
    }
    static bool matches(const std::vector<Selector>& sels, const Node* e) {
        for (auto& s : sels)
            if (selector_matches(s, e)) return true;
        return false;
    }

    // The document's current session history entry changed without a load (HTML 7.4.6.2, update document for
    // history step application): popstate at once, and hashchange in a task of its own when the fragment changed.
    void sessionEntryChanged(const std::string& oldUrl) {
        url = session[sessionAt].url;
        if (Url::parse(oldUrl).fragment != Url::parse(url).fragment) {
            hashChanges.push_back({oldUrl, url});
            client.wake(0);
        }
        Value ev = makeEvent("popstate", false, false);
        ((JSHost*)ev.asObject())->slotA = session[sessionAt].state;
        fire(nullptr, ev);
    }

    // ---- events
    // The interface of an event that the browser itself fires, from its type (UI Events 3, Pointer Events, HTML).
    JSObject* eventProtoFor(const std::string& type) {
        static const std::pair<const char*, const char*> kinds[] = {
            {"click", "PointerEvent"},      {"auxclick", "PointerEvent"},   {"contextmenu", "PointerEvent"}, {"dblclick", "MouseEvent"},
            {"mousedown", "MouseEvent"},    {"mouseup", "MouseEvent"},      {"mousemove", "MouseEvent"},     {"mouseover", "MouseEvent"},
            {"mouseout", "MouseEvent"},     {"mouseenter", "MouseEvent"},   {"mouseleave", "MouseEvent"},    {"wheel", "WheelEvent"},
            {"keydown", "KeyboardEvent"},   {"keyup", "KeyboardEvent"},     {"keypress", "KeyboardEvent"},   {"input", "InputEvent"},
            {"beforeinput", "InputEvent"},  {"focus", "FocusEvent"},        {"blur", "FocusEvent"},          {"focusin", "FocusEvent"},
            {"focusout", "FocusEvent"},     {"popstate", "PopStateEvent"},  {"hashchange", "HashChangeEvent"}, {"submit", "SubmitEvent"},
            {"pageshow", "PageTransitionEvent"}, {"pagehide", "PageTransitionEvent"}, {"beforeunload", "BeforeUnloadEvent"},
        };
        for (auto& [name, kind] : kinds)
            if (type == name)
                if (auto it = eventProtos.find(kind); it != eventProtos.end()) return it->second;
        return eventProto;
    }
    Value makeEvent(const std::string& type, bool bubbles, bool cancelable) {
        JSHost* e = vm.alloc<JSHost>();
        e->hostKind = HK_Event;
        e->proto = eventProtoFor(type);
        vm.def(e, "type", vm.strv(utf8to16(type)), PF_ALL);
        vm.def(e, "bubbles", Value::boolean(bubbles), PF_ALL);
        vm.def(e, "cancelable", Value::boolean(cancelable), PF_ALL);
        vm.def(e, "timeStamp", Value::num(now_ms() - startMs), PF_ALL);
        vm.def(e, "isTrusted", Value::boolean(true), PF_ALL);
        vm.def(e, "composed", Value::boolean(true), PF_ALL);
        vm.def(e, "eventPhase", Value::i32(0), PF_ALL);
        vm.def(e, "target", Value::null(), PF_ALL);
        vm.def(e, "currentTarget", Value::null(), PF_ALL);
        return V(e);
    }
    void callListener(Value fn, Value thisv, Value ev) {
        try {
            if (vm.isCallable(fn)) {
                vm.call(fn, thisv, &ev, 1);
            } else if (fn.isObject()) {
                Value h = vm.get(fn, "handleEvent");
                if (vm.isCallable(h)) vm.call(h, fn, &ev, 1);
            }
        } catch (JSThrow&) {
            reportError();
        }
    }
    // The handler set as el.onclick = fn, or written as an onclick="..." attribute.
    Value handlerFor(const void* key, Value cur, const std::string& type) {
        std::string name = "on" + type;
        PropKey k = PropKey::of(vm.atom(name));
        JSObject* o = cur.asObject();
        if (Prop* p = o->find(k)) return (p->flags & PF_ACC) ? Value::undef() : p->val;
        const Node* n = key == this ? doc.body : (const Node*)key;
        if (key == this && type != "load" && type != "unload" && type != "resize" && type != "scroll" && type != "beforeunload") return Value::undef();
        if (!n || n->type != Node::T::Element) return Value::undef();
        const std::string* code = n->attr(name);
        if (!code) return Value::undef();
        Value f = Value::null();
        try {
            f = vm.compileFunction(u"event", utf8to16(*code), name);
        } catch (JSThrow&) {
            reportError();
        }
        o->add(k, f, PF_W | PF_C);
        return f;
    }
    bool fire(Node* target, Value ev) {
        JSHost* e = (JSHost*)ev.asObject();
        std::string type = utf16to8(vm.toStr16(vm.get(ev, "type")));
        bool bubbles = vm.toBool(vm.get(ev, "bubbles"));
        Value window = V(vm.global), targetV = target ? wrap(target) : window;
        vm.set(ev, "target", targetV);
        vm.set(ev, "srcElement", targetV);
        std::vector<Node*> path;
        bool composed = vm.toBool(vm.get(ev, "composed"));
        for (Node* n = target; n; n = n->parent) {
            path.push_back(n);
            if (n->isShadowRoot() && !composed) break;  // the event stays inside its shadow tree
        }
        bool leaves = !path.empty() && !path.back()->parent && path.back()->type == Node::T::Document;
        e->flagA = e->flagB = false;
        // What a listener on `at` (null: the window) sees as the target: the target itself, or the host of the
        // outermost shadow tree around it that `at` is not inside of (DOM 2.5, retargeting).
        auto seenFrom = [&](Node* at) {
            Node* t = target;
            while (t) {
                Node* root = shadow_root_of(t);
                if (!root) break;
                bool inside = false;
                for (Node* a = at; a && !inside; a = a->parent) inside = a == root;
                if (inside) break;
                t = root->parent;
            }
            return t;
        };
        auto run = [&](const void* key, Value cur, int phase) {
            if (e->flagA) return;
            if (g_shadowRoots && target) {
                Value seen = wrap(seenFrom(key == (const void*)this ? nullptr : (Node*)key));
                vm.set(ev, "target", seen);
                vm.set(ev, "srcElement", seen);
            }
            vm.set(ev, "currentTarget", cur);
            vm.set(ev, "eventPhase", Value::i32(phase));
            if (phase != 1) {
                Value h = handlerFor(key, cur, type);
                if (vm.isCallable(h)) {
                    try {
                        Value r = vm.call(h, cur, &ev, 1);
                        if (r.isBool() && !r.isTrue()) e->flagC = true;
                    } catch (JSThrow&) {
                        reportError();
                    }
                }
            }
            auto it = listeners.find(key);
            if (it == listeners.end()) return;
            std::vector<Value> fns;
            RootScope keep(vm, fns);
            std::vector<Listener> snapshot = it->second;
            for (auto& l : snapshot) fns.push_back(l.fn);
            for (auto& l : snapshot) {
                if (l.type != type || (phase == 1 && !l.capture) || (phase == 3 && l.capture)) continue;
                if (l.once) removeListener(key, l.type, l.fn, l.capture);
                callListener(l.fn, cur, ev);
                if (e->flagB) break;
            }
        };
        if (target && leaves) run(this, window, 1);
        for (size_t i = path.size(); i-- > 1;) run(path[i], wrap(path[i]), 1);
        run(target ? (const void*)target : (const void*)this, targetV, 2);
        if (bubbles && target) {
            for (size_t i = 1; i < path.size(); ++i) run(path[i], wrap(path[i]), 3);
            if (leaves) run(this, window, 3);
        }
        vm.set(ev, "currentTarget", Value::null());
        vm.set(ev, "eventPhase", Value::i32(0));
        return !e->flagC;
    }
    void removeListener(const void* key, const std::string& type, Value fn, bool capture) {
        auto it = listeners.find(key);
        if (it == listeners.end()) return;
        auto& v = it->second;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].type == type && v[i].fn == fn && v[i].capture == capture) {
                v.erase(v.begin() + i);
                return;
            }
    }

    void setup();
};

namespace {

using Impl = ScriptHost::Impl;

// Every entry from the browser into page script goes through one of these: it arms the time limit, and when
// the outermost one ends it runs promise callbacks and gives the garbage collector a chance.
struct Entry {
    Impl& im;
    explicit Entry(Impl& i) : im(i) {
        if (im.depth++ == 0) im.vm.deadlineTick = GetTickCount64() + 8000;
    }
    ~Entry() {
        if (--im.depth) return;
        im.vm.runMicrotasks();
        im.vm.deadlineTick = 0;
        if (im.vm.terminated && !im.dead) {
            im.dead = true;
            im.timers.clear();
            im.client.consoleMessage(3, L"Scripts on this page were stopped: one of them ran for more than 8 seconds.");
        }
        if (im.vm.gc.shouldCollect()) im.vm.gc.collect();
    }
};

Impl& I(Vm& vm) { return *(Impl*)vm.hostData; }
std::string str8(Vm& vm, Value v) { return utf16to8(vm.toStr16(v)); }
Value val8(Vm& vm, std::string_view s) { return vm.strv(utf8to16(s)); }

[[noreturn]] void illegal(CallInfo& ci, const char* want) {
    std::string name = ci.callee && ci.callee->name ? utf16to8(ci.callee->name->s) : "method";
    ci.vm.throwError(Err::TypeError, "Illegal invocation: %s was called on %s, which is not %s", name.c_str(), utf16to8(ci.vm.inspect(ci.thisv, 0)).c_str(), want);
}
Node* toNode(Value v) {
    if (v.isObject() && v.asObject()->cls == Cls::Host) {
        JSHost* h = (JSHost*)v.asObject();
        if (h->hostKind == HK_Node) return (Node*)h->ptr;
    }
    return nullptr;
}
Node* thisNode(CallInfo& ci) {
    Node* n = toNode(ci.thisv);
    if (!n) illegal(ci, "a node");
    return n;
}
Node* thisElem(CallInfo& ci) {
    Node* n = thisNode(ci);
    if (n->type != Node::T::Element) illegal(ci, "an element");
    return n;
}
Node* argNode(CallInfo& ci, int i, const char* what) {
    Node* n = toNode(ci.arg(i));
    if (!n) ci.vm.throwError(Err::TypeError, "%s: parameter %d is not a Node", what, i + 1);
    return n;
}
// Methods like append() take nodes or strings; strings become text nodes.
Node* nodeOrText(CallInfo& ci, Value v) {
    if (Node* n = toNode(v)) return n;
    return I(ci.vm).newText(str8(ci.vm, v));
}
JSHost* thisHost(CallInfo& ci, uint8_t kind) {
    if (ci.thisv.isObject() && ci.thisv.asObject()->cls == Cls::Host && ((JSHost*)ci.thisv.asObject())->hostKind == kind) return (JSHost*)ci.thisv.asObject();
    illegal(ci, "the right kind of object");
}

void setAttr(Node* n, const std::string& name, const std::string& value) {
    bool found = false;
    for (auto& a : n->attrs)
        if (a.first == name) {
            a.second = value;
            found = true;
            break;
        }
    if (!found) n->attrs.push_back({name, value});
    if (name == "id") {
        n->id = value;
    } else if (name == "class") {
        n->classes.clear();
        size_t i = 0;
        while (i < value.size()) {
            while (i < value.size() && isws(value[i])) ++i;
            size_t st = i;
            while (i < value.size() && !isws(value[i])) ++i;
            if (i > st) n->classes.push_back(value.substr(st, i - st));
        }
    }
}
void removeAttr(Node* n, const std::string& name) {
    for (size_t i = 0; i < n->attrs.size(); ++i)
        if (n->attrs[i].first == name) {
            n->attrs.erase(n->attrs.begin() + i);
            break;
        }
    if (name == "id") n->id.clear();
    if (name == "class") n->classes.clear();
}

// A DOM operation after which the reactions of custom elements run ([CEReactions], HTML 4.13.6)
template <NativeFn fn>
Value ce(CallInfo& ci) {
    Impl& im = I(ci.vm);
    if (!im.ceHook.isObject()) return fn(ci);
    std::vector<Value> keep(1);
    RootScope roots(ci.vm, keep);
    try {
        keep[0] = fn(ci);
    } catch (JSThrow&) {
        keep[0] = ci.vm.pending;
        im.flushReactions();
        ci.vm.pending = keep[0];
        throw;
    }
    im.flushReactions();
    return keep[0];
}
JSFN(ceHookSet) {  // __ceHook(fn): the prelude's function for reactions; from now on they are noted
    I(ci.vm).ceHook = ci.arg(0);
    return Value::undef();
}
JSFN(isConstructorFn) { return Value::boolean(ci.vm.isConstructor(ci.arg(0))); }  // __isConstructor(value)
JSFN(ceStateFn) {  // __ceState(element[, state]): reads or sets the custom element state
    Node* n = toNode(ci.arg(0));
    if (!n) return Value::i32(0);
    if (ci.argc > 1) {
        n->ceState = (uint8_t)ci.vm.toInt32(ci.args[1]);
        I(ci.vm).changed();  // :defined may match differently now
    }
    return Value::i32(n->ceState);
}
JSFN(htmlElementCtor) {  // [HTMLConstructor] (HTML 3.2.3): only a custom element's constructor gets through
    Impl& im = I(ci.vm);
    if (!ci.isConstruct() || ci.newTarget == V(ci.callee) || !im.ceHook.isObject()) ci.vm.throwError(Err::TypeError, "Illegal constructor");
    Value args[2] = {Value::i32(4), ci.newTarget};
    return ci.vm.call(im.ceHook, Value::undef(), args, 2);
}

template <class F>
void eachElement(Node* root, F&& fn) {  // document order, excluding root; fn returns false to stop
    std::vector<Node*> st;
    for (auto it = root->kids.rbegin(); it != root->kids.rend(); ++it) st.push_back(it->get());
    while (!st.empty()) {
        Node* n = st.back();
        st.pop_back();
        if (n->type != Node::T::Element) continue;
        if (!fn(n)) return;
        for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) st.push_back(it->get());
    }
}
bool isConnected(const Node* n) {
    while (n->parent) n = n->parent;
    return n->type == Node::T::Document;
}
int indexInParent(const Node* n) {
    if (!n->parent) return -1;
    auto& k = n->parent->kids;
    for (size_t i = 0; i < k.size(); ++i)
        if (k[i].get() == n) return (int)i;
    return -1;
}
Node* sibling(const Node* n, int dir, bool elementsOnly) {
    int i = indexInParent(n);
    if (i < 0) return nullptr;
    auto& k = n->parent->kids;
    for (i += dir; i >= 0 && i < (int)k.size(); i += dir)
        if (!elementsOnly || k[i]->type == Node::T::Element) return k[i].get();
    return nullptr;
}
Node* edgeChild(const Node* n, bool last, bool elementsOnly) {
    auto& k = n->kids;
    for (size_t j = 0; j < k.size(); ++j) {
        Node* c = k[last ? k.size() - 1 - j : j].get();
        if (!elementsOnly || c->type == Node::T::Element) return c;
    }
    return nullptr;
}

bool isVoid(const std::string& n) {
    static const char* v[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"};
    for (const char* s : v)
        if (n == s) return true;
    return false;
}
void escapeInto(const std::string& s, std::string& out, bool attr) {
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"' && attr) out += "&quot;";
        else out += c;
    }
}
void serialize(const Node* n, std::string& out) {
    switch (n->type) {
        case Node::T::Text:
            if (n->parent && (n->parent->name == "script" || n->parent->name == "style")) out += n->text;
            else escapeInto(n->text, out, false);
            break;
        case Node::T::Comment: out += "<!--" + n->text + "-->"; break;
        case Node::T::Document:
            for (auto& k : n->kids) serialize(k.get(), out);
            break;
        case Node::T::Element: {
            bool frag = Impl::isFragment(n);
            if (!frag) {
                out += "<" + n->name;
                for (auto& a : n->attrs) {
                    out += " " + a.first + "=\"";
                    escapeInto(a.second, out, true);
                    out += "\"";
                }
                out += ">";
                if (isVoid(n->name)) return;
            }
            for (auto& k : (n->content ? n->content.get() : n)->kids) serialize(k.get(), out);  // a template: its contents
            if (!frag) out += "</" + n->name + ">";
        }
    }
}
std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c -= 32;
    return s;
}

// ---------------------------------------------------------------------------- Node
JSFN(nodeType) {
    Node* n = thisNode(ci);
    int t = n->type == Node::T::Document ? 9 : n->type == Node::T::Text ? 3 : n->type == Node::T::Comment ? 8 : Impl::isFragment(n) || n->isShadowRoot() ? 11 : 1;
    return Value::i32(t);
}
JSFN(nodeName) {
    Node* n = thisNode(ci);
    switch (n->type) {
        case Node::T::Document: return ci.vm.strv("#document");
        case Node::T::Text: return ci.vm.strv("#text");
        case Node::T::Comment: return ci.vm.strv("#comment");
        default: return val8(ci.vm, Impl::isFragment(n) ? n->name : upper(n->name));
    }
}
JSFN(nodeValueGet) {
    Node* n = thisNode(ci);
    return n->type == Node::T::Text || n->type == Node::T::Comment ? val8(ci.vm, n->text) : Value::null();
}
JSFN(nodeValueSet) {
    Node* n = thisNode(ci);
    if (n->type == Node::T::Text || n->type == Node::T::Comment) I(ci.vm).setText(n, ci.arg(0).isNull() ? "" : str8(ci.vm, ci.arg(0)));
    return Value::undef();
}
JSFN(textContentGet) {
    Node* n = thisNode(ci);
    if (n->type == Node::T::Document) return Value::null();
    return val8(ci.vm, n->type == Node::T::Comment ? n->text : text_content(n));
}
JSFN(textContentSet) {
    Node* n = thisNode(ci);
    I(ci.vm).setText(n, ci.arg(0).isNullish() ? "" : str8(ci.vm, ci.arg(0)));
    return Value::undef();
}
JSFN(parentNode) {  // a shadow root has a host, not a parent
    Node* n = thisNode(ci);
    return I(ci.vm).wrap(n->isShadowRoot() ? nullptr : n->parent);
}
JSFN(parentElement) {
    Node* n = thisNode(ci);
    Node* p = n->parent;
    return I(ci.vm).wrap(p && p->type == Node::T::Element && !n->isShadowRoot() && !p->isShadowRoot() && !Impl::isFragment(p) ? p : nullptr);
}
JSFN(childNodes) {
    Node* n = thisNode(ci);
    std::vector<Node*> out;
    for (auto& k : n->kids) out.push_back(k.get());
    return I(ci.vm).list(out);
}
JSFN(children) {
    Node* n = thisNode(ci);
    std::vector<Node*> out;
    for (auto& k : n->kids)
        if (k->type == Node::T::Element) out.push_back(k.get());
    return I(ci.vm).list(out);
}
JSFN(childElementCount) {
    int c = 0;
    for (auto& k : thisNode(ci)->kids)
        if (k->type == Node::T::Element) ++c;
    return Value::i32(c);
}
// One native serves firstChild, lastElementChild, nextSibling...: data holds which one.
JSFN(relative) {
    Node* n = thisNode(ci);
    int w = ci.callee->data.asInt();
    Node* r = nullptr;
    switch (w) {
        case 0: r = edgeChild(n, false, false); break;
        case 1: r = edgeChild(n, true, false); break;
        case 2: r = edgeChild(n, false, true); break;
        case 3: r = edgeChild(n, true, true); break;
        case 4: r = sibling(n, 1, false); break;
        case 5: r = sibling(n, -1, false); break;
        case 6: r = sibling(n, 1, true); break;
        default: r = sibling(n, -1, true);
    }
    return I(ci.vm).wrap(r);
}
JSFN(ownerDocument) {
    Node* n = thisNode(ci);
    return n->type == Node::T::Document ? Value::null() : I(ci.vm).wrap(I(ci.vm).docNode());
}
JSFN(nodeIsConnected) { return Value::boolean(isConnected(thisNode(ci))); }
JSFN(appendChild) {
    Node* p = thisNode(ci);
    Node* c = argNode(ci, 0, "appendChild");
    I(ci.vm).insert(p, c, nullptr);
    return ci.arg(0);
}
JSFN(insertBefore) {
    Node* p = thisNode(ci);
    Node* c = argNode(ci, 0, "insertBefore");
    Node* ref = toNode(ci.arg(1));
    if (ref && ref->parent != p) ci.vm.throwError(Err::Error, "NotFoundError: the reference node is not a child of this node");
    I(ci.vm).insert(p, c, ref);
    return ci.arg(0);
}
JSFN(removeChild) {
    Node* p = thisNode(ci);
    Node* c = argNode(ci, 0, "removeChild");
    if (c->parent != p) ci.vm.throwError(Err::Error, "NotFoundError: the node to be removed is not a child of this node");
    I(ci.vm).remove(c);
    return ci.arg(0);
}
JSFN(replaceChild) {
    Node* p = thisNode(ci);
    Node* fresh = argNode(ci, 0, "replaceChild");
    Node* old = argNode(ci, 1, "replaceChild");
    if (old->parent != p) ci.vm.throwError(Err::Error, "NotFoundError: the node to be replaced is not a child of this node");
    if (fresh != old) {
        I(ci.vm).insert(p, fresh, old);
        I(ci.vm).remove(old);
    }
    return ci.arg(1);
}
JSFN(cloneNode) {
    Impl& im = I(ci.vm);
    return im.wrap(im.clone(thisNode(ci), ci.vm.toBool(ci.arg(0))));
}
JSFN(nodeContains) {
    Node* n = thisNode(ci);
    for (Node* o = toNode(ci.arg(0)); o; o = o->parent)
        if (o == n) return Value::boolean(true);
    return Value::boolean(false);
}
JSFN(hasChildNodes) { return Value::boolean(!thisNode(ci)->kids.empty()); }
JSFN(nodeRemove) {
    I(ci.vm).remove(thisNode(ci));
    return Value::undef();
}
JSFN(getRootNode) {  // (options): the shadow root a node is under, or with composed: true the root above all hosts
    Node* n = thisNode(ci);
    bool composed = ci.arg(0).isObject() && ci.vm.toBool(ci.vm.get(ci.arg(0), "composed"));
    while (n->parent && (composed || !n->isShadowRoot())) n = n->parent;
    return I(ci.vm).wrap(n);
}
// ---- shadow trees (DOM 4.8)
JSFN(attachShadow) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    Node* host = thisElem(ci);
    static const char* allowed[] = {"article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3", "h4", "h5", "h6", "header", "main", "nav", "p", "section", "span"};
    bool ok = Impl::maybeCustom(host);
    for (const char* a : allowed) ok = ok || host->name == a;
    if (!ok) vm.throwError(Err::Error, "NotSupportedError: a <%s> element cannot have a shadow root", host->name.c_str());
    if (host->shadow) vm.throwError(Err::Error, "NotSupportedError: the element already has a shadow root");
    std::string mode = ci.arg(0).isObject() ? str8(vm, vm.get(ci.arg(0), "mode")) : std::string();
    if (mode != "open" && mode != "closed") vm.throwError(Err::TypeError, "attachShadow: the mode must be 'open' or 'closed'");
    host->shadow = std::make_unique<Node>();
    host->shadow->type = Node::T::Element;
    host->shadow->name = "#shadow-root";
    host->shadow->parent = host;
    host->shadow->attrs.push_back({"mode", mode});
    ++g_shadowRoots;
    im.changed();
    return im.wrap(host->shadow.get());
}
JSFN(shadowRootGet) {  // data: 1 = whatever the mode (for the prelude)
    Node* shadow = thisElem(ci)->shadow.get();
    bool open = shadow && (ci.callee->data.asInt() == 1 || *shadow->attr("mode") == "open");
    return I(ci.vm).wrap(open ? shadow : nullptr);
}
Node* thisShadow(CallInfo& ci) {
    Node* n = thisNode(ci);
    if (!n->isShadowRoot()) illegal(ci, "a ShadowRoot");
    return n;
}
JSFN(adoptFn) {  // __adopt(document or shadow root, [css text...]): the sheets that tree has adopted
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    Node* root = toNode(ci.arg(0));
    if (!root || !ci.arg(1).isObject()) return Value::undef();
    std::vector<std::string>& list = im.adopted[root];
    list.clear();
    uint32_t n = vm.lengthOf(ci.arg(1));
    for (uint32_t i = 0; i < n; ++i) list.push_back(str8(vm, vm.get(ci.arg(1), PropKey::index(i))));
    im.changed();
    return Value::undef();
}
JSFN(shadowHost) { return I(ci.vm).wrap(thisShadow(ci)->parent); }
JSFN(shadowMode) { return val8(ci.vm, *thisShadow(ci)->attr("mode")); }
JSFN(slotAssigned) {  // assignedNodes(options) and, with data 1, assignedElements(options)
    Vm& vm = ci.vm;
    Node* slot = thisElem(ci);
    bool flatten = ci.arg(0).isObject() && vm.toBool(vm.get(ci.arg(0), "flatten"));
    std::vector<Node*> found, out;
    assigned_nodes(slot, found);
    if (found.empty() && flatten)
        for (Node* k : FlatKids(slot)) found.push_back(k);
    for (Node* n : found)
        if (ci.callee->data.asInt() != 1 || n->type == Node::T::Element) out.push_back(n);
    JSArray* a = vm.newArray();
    for (Node* n : out) a->elems.push_back(I(vm).wrap(n));
    return V(a);
}
JSFN(assignedSlotGet) {  // null for a closed shadow tree, as its slots are not for outsiders
    Node* slot = assigned_slot(thisNode(ci));
    Node* root = slot ? shadow_root_of(slot) : nullptr;
    return I(ci.vm).wrap(root && *root->attr("mode") == "open" ? slot : nullptr);
}
JSFN(compareDocumentPosition) {
    Node* a = thisNode(ci);
    Node* b = toNode(ci.arg(0));
    if (!b || a == b) return Value::i32(0);
    for (Node* p = b->parent; p; p = p->parent)
        if (p == a) return Value::i32(20);  // contained by, following
    for (Node* p = a->parent; p; p = p->parent)
        if (p == b) return Value::i32(10);  // contains, preceding
    return Value::i32(isConnected(a) && isConnected(b) ? 4 : 1);
}
// append / prepend / before / after / replaceWith / replaceChildren: data holds which one.
JSFN(insertMany) {
    Impl& im = I(ci.vm);
    Node* n = thisNode(ci);
    int w = ci.callee->data.asInt();
    Node* parent = w <= 1 || w == 5 ? n : n->parent;
    if (!parent) return Value::undef();
    Node* before = w == 0 || w == 5 ? nullptr : w == 1 ? edgeChild(n, false, false) : w == 2 ? n : w == 3 ? sibling(n, 1, false) : n;
    if (w == 5) im.clearChildren(n);
    for (int i = 0; i < ci.argc; ++i) im.insert(parent, nodeOrText(ci, ci.args[i]), before);
    if (w == 4) im.remove(n);
    im.changed();
    return Value::undef();
}

// ---------------------------------------------------------------------------- queries
JSFN(querySelector) {
    Impl& im = I(ci.vm);
    Node* root = thisNode(ci);
    const auto& sels = im.selectors(str8(ci.vm, ci.arg(0)));
    Node* found = nullptr;
    eachElement(root, [&](Node* e) {
        if (!Impl::matches(sels, e)) return true;
        found = e;
        return false;
    });
    return im.wrap(found);
}
JSFN(querySelectorAll) {
    Impl& im = I(ci.vm);
    Node* root = thisNode(ci);
    const auto& sels = im.selectors(str8(ci.vm, ci.arg(0)));
    std::vector<Node*> out;
    eachElement(root, [&](Node* e) {
        if (Impl::matches(sels, e)) out.push_back(e);
        return true;
    });
    return im.list(out);
}
JSFN(elementMatches) {
    Impl& im = I(ci.vm);
    Node* e = thisElem(ci);
    return Value::boolean(Impl::matches(im.selectors(str8(ci.vm, ci.arg(0))), e));
}
JSFN(elementClosest) {
    Impl& im = I(ci.vm);
    const auto& sels = im.selectors(str8(ci.vm, ci.arg(0)));
    for (Node* e = thisElem(ci); e && e->type == Node::T::Element; e = e->parent)
        if (Impl::matches(sels, e)) return im.wrap(e);
    return Value::null();
}
JSFN(getElementById) {
    Impl& im = I(ci.vm);
    std::string id = str8(ci.vm, ci.arg(0));
    Node* found = nullptr;
    eachElement(thisNode(ci), [&](Node* e) {
        if (e->id != id) return true;
        found = e;
        return false;
    });
    return im.wrap(found);
}
JSFN(getElementsByTagName) {
    std::string tag = lower(str8(ci.vm, ci.arg(0)));
    std::vector<Node*> out;
    eachElement(thisNode(ci), [&](Node* e) {
        if (tag == "*" || e->name == tag) out.push_back(e);
        return true;
    });
    return I(ci.vm).list(out);
}
// ---- document.all (HTML 2.6.3.1, HTMLAllCollection). It has [[IsHTMLDDA]]: typeof says "undefined" and it counts
// as false, yet it is an object, and pages tell it from undefined with ===. It can be called like its item().
// Not done: reading it by index or name as a property (all[0], all.name), and a collection for a name that
// several elements share: the first of them is returned.
JSFN(allItem) {  // data: 0 for all(x) and item(x), which also take an index; 1 for namedItem(x)
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    if (ci.argc == 0 || ci.arg(0).isUndef()) return Value::null();
    std::string key = str8(vm, ci.arg(0));
    bool index = ci.callee->data.asInt() == 0 && !key.empty() && key.size() < 10 && key.find_first_not_of("0123456789") == std::string::npos;
    if (key.empty()) return Value::null();
    size_t at = index ? (size_t)atoll(key.c_str()) : 0, seen = 0;
    static const char* named[] = {"a", "button", "embed", "form", "frame", "frameset", "iframe", "img", "input", "map", "meta", "object", "select", "textarea"};
    Node* found = nullptr;
    eachElement(im.doc.root.get(), [&](Node* e) {
        if (index) {
            if (seen++ == at) found = e;
        } else if (e->id == key) {
            found = e;
        } else if (const std::string* name = e->attr("name"); name && *name == key) {
            for (const char* tag : named)
                if (e->name == tag) found = e;
        }
        return !found;
    });
    return im.wrap(found);
}
JSFN(allLength) {
    int count = 0;
    eachElement(I(ci.vm).doc.root.get(), [&](Node*) {
        ++count;
        return true;
    });
    return Value::i32(count);
}
JSFN(documentAll) { return V(I(ci.vm).allCollection); }
JSFN(getElementsByClassName) {
    std::string names = str8(ci.vm, ci.arg(0));
    std::vector<std::string> want;
    size_t i = 0;
    while (i < names.size()) {
        while (i < names.size() && isws(names[i])) ++i;
        size_t st = i;
        while (i < names.size() && !isws(names[i])) ++i;
        if (i > st) want.push_back(names.substr(st, i - st));
    }
    std::vector<Node*> out;
    eachElement(thisNode(ci), [&](Node* e) {
        for (auto& w : want)
            if (std::find(e->classes.begin(), e->classes.end(), w) == e->classes.end()) return true;
        if (!want.empty()) out.push_back(e);
        return true;
    });
    return I(ci.vm).list(out);
}
JSFN(getElementsByName) {
    std::string name = str8(ci.vm, ci.arg(0));
    std::vector<Node*> out;
    eachElement(thisNode(ci), [&](Node* e) {
        const std::string* a = e->attr("name");
        if (a && *a == name) out.push_back(e);
        return true;
    });
    return I(ci.vm).list(out);
}
JSFN(nodeListNamedItem) {
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != Cls::Array) return Value::null();
    std::string name = str8(ci.vm, ci.arg(0));
    for (Value v : ((JSArray*)ci.thisv.asObject())->elems) {
        Node* n = toNode(v);
        if (!n || n->type != Node::T::Element) continue;
        const std::string* a = n->attr("name");
        if (n->id == name || (a && *a == name)) return v;
    }
    return Value::null();
}
JSFN(nodeListItem) {
    if (!ci.thisv.isObject() || ci.thisv.asObject()->cls != Cls::Array) return Value::null();
    JSArray* a = (JSArray*)ci.thisv.asObject();
    double i = ci.vm.toInteger(ci.arg(0));
    return i >= 0 && i < (double)a->elems.size() ? a->elems[(size_t)i] : Value::null();
}

// ---------------------------------------------------------------------------- Element
JSFN(tagName) { return val8(ci.vm, upper(thisElem(ci)->name)); }
JSFN(localName) { return val8(ci.vm, thisElem(ci)->name); }
JSFN(getAttribute) {
    const std::string* v = thisElem(ci)->attr(lower(str8(ci.vm, ci.arg(0))));
    return v ? val8(ci.vm, *v) : Value::null();
}
JSFN(setAttribute) {
    Node* n = thisElem(ci);
    std::string value = str8(ci.vm, ci.arg(1));
    I(ci.vm).changeAttribute(n, lower(str8(ci.vm, ci.arg(0))), &value);
    return Value::undef();
}
JSFN(removeAttribute) {
    I(ci.vm).changeAttribute(thisElem(ci), lower(str8(ci.vm, ci.arg(0))), nullptr);
    return Value::undef();
}
JSFN(hasAttribute) { return Value::boolean(thisElem(ci)->attr(lower(str8(ci.vm, ci.arg(0)))) != nullptr); }
JSFN(toggleAttribute) {
    Node* n = thisElem(ci);
    std::string name = lower(str8(ci.vm, ci.arg(0)));
    bool on = ci.argc > 1 ? ci.vm.toBool(ci.args[1]) : !n->attr(name);
    std::string empty;
    if (on != (n->attr(name) != nullptr)) I(ci.vm).changeAttribute(n, name, on ? &empty : nullptr);
    return Value::boolean(on);
}
JSFN(getAttributeNames) {
    JSArray* a = ci.vm.newArray();
    for (auto& at : thisElem(ci)->attrs) a->elems.push_back(val8(ci.vm, at.first));
    return V(a);
}
JSFN(attributesGet) {
    Vm& vm = ci.vm;
    JSArray* a = vm.newArray();
    for (auto& at : thisElem(ci)->attrs) {
        JSObject* o = vm.newObject();
        vm.def(o, "name", val8(vm, at.first), PF_ALL);
        vm.def(o, "value", val8(vm, at.second), PF_ALL);
        a->elems.push_back(V(o));
    }
    return V(a);
}
// Properties that mirror an attribute. data: attribute name, data2: 0 text, 1 boolean, 2 URL, 3 number.
JSFN(reflectGet) {
    Vm& vm = ci.vm;
    Node* n = thisElem(ci);
    std::string attr = utf16to8(ci.callee->data.asString()->s);
    const std::string* v = n->attr(attr);
    switch (ci.callee->data2.asInt()) {
        case 1: return Value::boolean(v != nullptr);
        case 2: return v ? val8(vm, resolve_url(I(vm).doc.baseUrl, *v)) : V(vm.a.empty);
        case 3: return Value::num(v ? atof(v->c_str()) : (attr == "tabindex" ? -1.0 : 0.0));
        default: return v ? val8(vm, *v) : V(vm.a.empty);
    }
}
JSFN(reflectSet) {
    Vm& vm = ci.vm;
    Node* n = thisElem(ci);
    std::string attr = utf16to8(ci.callee->data.asString()->s);
    std::string value;
    if (ci.callee->data2.asInt() != 1) value = str8(vm, ci.arg(0));
    I(vm).changeAttribute(n, attr, ci.callee->data2.asInt() != 1 || vm.toBool(ci.arg(0)) ? &value : nullptr);
    return Value::undef();
}
JSFN(innerHTMLGet) {
    std::string out;
    const Node* n = thisNode(ci);
    for (auto& k : (n->content ? n->content.get() : n)->kids) serialize(k.get(), out);
    return val8(ci.vm, out);
}
JSFN(imageInfo) {  // HTMLImageElement: data 0 naturalWidth, 1 naturalHeight, 2 complete
    int w = 0, h = 0;
    bool complete = I(ci.vm).client.imageSize(thisElem(ci), w, h);
    int which = ci.callee->data.asInt();
    return which == 2 ? Value::boolean(complete) : Value::i32(which == 0 ? w : h);
}
JSFN(templateContent) { return I(ci.vm).wrap(thisElem(ci)->content.get()); }  // HTMLTemplateElement.prototype.content
JSFN(innerHTMLSet) {
    I(ci.vm).setInner(thisElem(ci), ci.arg(0).isNull() ? "" : str8(ci.vm, ci.arg(0)));
    return Value::undef();
}
JSFN(outerHTMLGet) {
    std::string out;
    serialize(thisElem(ci), out);
    return val8(ci.vm, out);
}
JSFN(outerHTMLSet) {
    Impl& im = I(ci.vm);
    Node* n = thisElem(ci);
    if (!n->parent) return Value::undef();
    Node* tmp = im.newElement("div");
    im.setInner(tmp, str8(ci.vm, ci.arg(0)));
    std::vector<Node*> kids;
    for (auto& k : tmp->kids) kids.push_back(k.get());
    for (Node* k : kids) im.insert(n->parent, k, n);
    im.remove(n);
    return Value::undef();
}
JSFN(insertAdjacent) {  // data: 0 HTML, 1 element, 2 text
    Impl& im = I(ci.vm);
    Node* n = thisElem(ci);
    std::string where = lower(str8(ci.vm, ci.arg(0)));
    int kind = ci.callee->data.asInt();
    std::vector<Node*> items;
    if (kind == 0) {
        Node* tmp = im.newElement(n->name == "tr" || n->name == "tbody" || n->name == "table" ? n->name : std::string("div"));
        im.setInner(tmp, str8(ci.vm, ci.arg(1)));
        for (auto& k : tmp->kids) items.push_back(k.get());
    } else if (kind == 1) {
        items.push_back(argNode(ci, 1, "insertAdjacentElement"));
    } else
        items.push_back(im.newText(str8(ci.vm, ci.arg(1))));
    Node* parent = where == "afterbegin" || where == "beforeend" ? n : n->parent;
    if (!parent) return Value::null();
    Node* before = where == "beforebegin" ? n : where == "afterbegin" ? edgeChild(n, false, false) : where == "afterend" ? sibling(n, 1, false) : nullptr;
    for (Node* it : items) im.insert(parent, it, before);
    return kind == 1 ? ci.arg(1) : Value::undef();
}

struct Box {
    float x = 0, y = 0, w = 0, h = 0;
};
Box boxOf(Impl& im, Node* n) {
    im.client.ensureLayout();
    Box b;
    if (n->hasBox) b = {n->bx, n->by, n->bw, n->bh};
    return b;
}
JSFN(getBoundingClientRect) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    Box b = boxOf(im, thisElem(ci));
    float vw, vh, sy;
    im.client.viewport(vw, vh, sy);
    JSObject* r = vm.newObject();
    double y = b.y - sy;
    const char* names[] = {"x", "y", "width", "height", "left", "top", "right", "bottom"};
    double vals[] = {b.x, y, b.w, b.h, b.x, y, b.x + b.w, y + b.h};
    for (int i = 0; i < 8; ++i) vm.def(r, names[i], Value::num(vals[i]), PF_ALL);
    return V(r);
}
JSFN(getClientRects) {  // one rectangle per box; none for an element that is not rendered (CSSOM View 6)
    Node* n = thisElem(ci);
    I(ci.vm).client.ensureLayout();
    JSArray* a = ci.vm.newArray();
    Value keep = V(a);
    if (n->hasBox) a->elems.push_back(getBoundingClientRect(ci));
    return keep;
}
JSFN(metric) {  // data: 0 width, 1 height, 2 left, 3 top, 4 scrollLeft and other zeros, 5 scrollHeight
    Impl& im = I(ci.vm);
    Node* n = thisElem(ci);
    int w = ci.callee->data.asInt();
    if (w == 4) return Value::i32(0);
    Box b = boxOf(im, n);
    if (w == 5) return Value::num(std::round(b.h + n->scrollRange));
    if ((n->name == "html" || n->name == "body") && w <= 1) {
        float vw, vh, sy;
        im.client.viewport(vw, vh, sy);
        if (w == 0) return Value::num(std::round(vw));
        if (ci.callee->data2.asInt() == 1) return Value::num(std::round(vh));  // clientHeight of the root is the viewport
    }
    return Value::num(std::round(w == 0 ? b.w : w == 1 ? b.h : w == 2 ? b.x : b.y));
}
// __scrollTop(element[, offset]) (CSSOM View, scrollTop): how far the element is scrolled down. With an offset it
// scrolls the element there, as far as it can go, and says whether it moved, which is when the caller owes the
// element a scroll event. The root element stands for the page, and the shell tells the document about that.
JSFN(scrollTopFn) {
    Impl& im = I(ci.vm);
    Node* n = toNode(ci.arg(0));
    if (!n || n->type != Node::T::Element) ci.vm.throwError(Err::TypeError, "scrollTop: not an Element");
    im.client.ensureLayout();
    bool page = n == im.doc.html;
    float vw, vh, pageY;
    im.client.viewport(vw, vh, pageY);
    float was = page ? pageY : n->scrollTop;
    if (ci.argc < 2) return Value::num(was);
    float to = (float)ci.vm.toNumber(ci.arg(1));
    if (page) {
        im.client.scrollTo(to);
        return Value::boolean(false);
    }
    to = std::clamp(to, 0.f, n->scrollRange);
    if (to == was) return Value::boolean(false);
    n->scrollTop = to;
    im.client.elementScrolled();
    return Value::boolean(true);
}
JSFN(offsetParent) {
    Node* n = thisElem(ci);
    return I(ci.vm).wrap(n->name == "body" || n->name == "html" ? nullptr : I(ci.vm).doc.body);
}
JSFN(scrollIntoView) {
    Impl& im = I(ci.vm);
    Box b = boxOf(im, thisElem(ci));
    im.client.scrollTo(b.y);
    return Value::undef();
}
JSFN(elementFocus) {
    I(ci.vm).client.focusNode(thisElem(ci));
    return Value::undef();
}
JSFN(elementBlur) {
    Impl& im = I(ci.vm);
    if (im.client.focusedNode() == thisElem(ci)) im.client.focusNode(nullptr);
    return Value::undef();
}
Node* formOf(Node* n) {
    for (Node* p = n; p; p = p->parent)
        if (p->is("form")) return p;
    return nullptr;
}
bool isSubmitButton(const Node* n) {
    std::string type = n->attr("type") ? lower(*n->attr("type")) : "";
    return (n->name == "button" && (type.empty() || type == "submit")) || (n->name == "input" && (type == "submit" || type == "image"));
}
JSFN(reportErrorFn) {  // prints an error the way an uncaught exception is printed
    Impl& im = I(ci.vm);
    ci.vm.pending = ci.arg(0);
    im.reportError();
    return Value::undef();
}
JSFN(deferredRun) {  // runs one deferred classic script; the callee's data is its index
    Impl& im = I(ci.vm);
    size_t i = (size_t)ci.callee->data.asInt();
    if (i < im.deferred.size() && !im.dead) im.runCode(im.deferred[i].node, im.deferred[i].code, im.deferred[i].url);
    return Value::undef();
}
JSFN(deferredDone) {  // every deferred script has run
    Impl& im = I(ci.vm);
    if (im.loadPending) {
        im.loadPending = false;
        im.loadDue = true;
        im.client.wake(0);
    }
    return Value::undef();
}
JSFN(elementClick) {
    Impl& im = I(ci.vm);
    Node* n = thisElem(ci);
    if (n->attr("disabled")) return Value::undef();
    Value ev = im.makeEvent("click", true, true);
    if (!im.fire(n, ev)) return Value::undef();
    for (Node* a = n; a; a = a->parent)  // a click inside a link follows it
        if (a->is("a") && a->attr("href")) {
            std::string href = *a->attr("href");
            if (!istarts(href, "javascript:")) im.client.navigate(resolve_url(im.doc.baseUrl, href), false);
            return Value::undef();
        }
    if (isSubmitButton(n)) {
        if (Node* form = formOf(n)) {
            if (im.fire(form, im.makeEvent("submit", true, true))) im.client.submitForm(form, n);
        }
    } else if (n->name == "input" && n->attr("type") && (lower(*n->attr("type")) == "checkbox" || lower(*n->attr("type")) == "radio") && Impl::connected(n)) {
        bool radio = lower(*n->attr("type")) == "radio";
        im.client.setFormChecked(n, radio ? true : !im.client.formChecked(n));
        im.fire(n, im.makeEvent("input", true, false));
        im.fire(n, im.makeEvent("change", true, false));
        im.changed();
    }
    return Value::undef();
}
JSFN(formSubmit) {  // data: 0 submit(), 1 requestSubmit(submitter)
    Impl& im = I(ci.vm);
    Node* form = thisElem(ci);
    if (!form->is("form")) illegal(ci, "a form");
    Node* submitter = toNode(ci.arg(0));
    if (ci.callee->data.asInt() == 1 && !im.fire(form, im.makeEvent("submit", true, true))) return Value::undef();
    im.client.submitForm(form, submitter);
    return Value::undef();
}
bool isControl(const Node* n) { return n->name == "input" || n->name == "textarea" || n->name == "select"; }
JSFN(valueGet) {
    Vm& vm = ci.vm;
    Node* n = thisElem(ci);
    if (isControl(n) && isConnected(n)) {
        std::wstring v = I(vm).client.formValue(n);
        return vm.strv(std::u16string(v.begin(), v.end()));
    }
    const std::string* a = n->attr("value");
    if (!a && n->name == "textarea") return val8(vm, text_content(n));
    if (!a && n->name == "option") return val8(vm, std::string(trim(text_content(n))));
    return a ? val8(vm, *a) : V(vm.a.empty);
}
JSFN(valueSet) {
    Vm& vm = ci.vm;
    Node* n = thisElem(ci);
    const std::u16string& s = vm.toStr16(ci.arg(0));
    if (isControl(n) && isConnected(n)) I(vm).client.setFormValue(n, std::wstring(s.begin(), s.end()));
    else {
        std::string value = utf16to8(s);
        I(vm).changeAttribute(n, "value", &value);
    }
    I(vm).changed();
    return Value::undef();
}
JSFN(checkedGet) {
    Node* n = thisElem(ci);
    if (n->name == "input" && isConnected(n)) return Value::boolean(I(ci.vm).client.formChecked(n));
    return Value::boolean(n->attr("checked") != nullptr);
}
JSFN(checkedSet) {
    Node* n = thisElem(ci);
    bool on = ci.vm.toBool(ci.arg(0));
    if (n->name == "input" && isConnected(n)) I(ci.vm).client.setFormChecked(n, on);
    else {
        std::string empty;
        I(ci.vm).changeAttribute(n, "checked", on ? &empty : nullptr);
    }
    I(ci.vm).changed();
    return Value::undef();
}
JSFN(formGet) {
    for (Node* p = thisElem(ci)->parent; p; p = p->parent)
        if (p->is("form")) return I(ci.vm).wrap(p);
    return Value::null();
}

// ---- classList
std::string joinClasses(const Node* n) {
    std::string s;
    for (auto& c : n->classes) s += (s.empty() ? "" : " ") + c;
    return s;
}
bool classListHookGet(Vm& vm, JSObject* o, PropKey k, Value& out) {
    if (!k.isIndex()) return false;
    Node* n = (Node*)((JSHost*)o)->ptr;
    if (k.asIndex() >= n->classes.size()) return false;
    out = vm.strv(utf8to16(n->classes[k.asIndex()]));
    return true;
}
void classListHookKeys(Vm&, JSObject* o, std::vector<PropKey>& out) {
    Node* n = (Node*)((JSHost*)o)->ptr;
    for (uint32_t i = 0; i < n->classes.size(); ++i) out.push_back(PropKey::index(i));
}
const HostHooks kClassListHooks{classListHookGet, nullptr, nullptr, classListHookKeys};
JSFN(classListGet) {
    Vm& vm = ci.vm;
    Node* n = thisElem(ci);
    JSHost* h = vm.alloc<JSHost>();
    h->hostKind = HK_ClassList;
    h->ptr = n;
    h->proto = I(vm).classListProto;
    h->hooks = &kClassListHooks;
    return V(h);
}
JSFN(classListOp) {  // data: 0 add, 1 remove, 2 toggle, 3 contains, 4 replace
    Vm& vm = ci.vm;
    Node* n = (Node*)thisHost(ci, HK_ClassList)->ptr;
    int op = ci.callee->data.asInt();
    auto has = [&](const std::string& c) { return std::find(n->classes.begin(), n->classes.end(), c) != n->classes.end(); };
    auto drop = [&](const std::string& c) { n->classes.erase(std::remove(n->classes.begin(), n->classes.end(), c), n->classes.end()); };
    Value result = Value::undef();
    if (op == 3) return Value::boolean(has(str8(vm, ci.arg(0))));
    if (op == 2) {
        std::string c = str8(vm, ci.arg(0));
        bool on = ci.argc > 1 ? vm.toBool(ci.args[1]) : !has(c);
        if (on && !has(c)) n->classes.push_back(c);
        if (!on) drop(c);
        result = Value::boolean(on);
    } else if (op == 4) {
        std::string from = str8(vm, ci.arg(0)), to = str8(vm, ci.arg(1));
        bool had = has(from);
        if (had) {
            drop(from);
            if (!has(to)) n->classes.push_back(to);
        }
        result = Value::boolean(had);
    } else {
        for (int i = 0; i < ci.argc; ++i) {
            std::string c = str8(vm, ci.args[i]);
            if (c.empty()) continue;
            if (op == 0 && !has(c)) n->classes.push_back(c);
            if (op == 1) drop(c);
        }
    }
    std::string value = joinClasses(n);
    I(vm).changeAttribute(n, "class", &value);
    return result;
}
JSFN(classListLength) { return Value::num((double)((Node*)thisHost(ci, HK_ClassList)->ptr)->classes.size()); }
JSFN(classListValue) { return val8(ci.vm, joinClasses((Node*)thisHost(ci, HK_ClassList)->ptr)); }
JSFN(classListItem) {
    Node* n = (Node*)thisHost(ci, HK_ClassList)->ptr;
    double i = ci.vm.toInteger(ci.arg(0));
    return i >= 0 && i < (double)n->classes.size() ? val8(ci.vm, n->classes[(size_t)i]) : Value::null();
}

// ---- style and dataset: objects whose properties live in an attribute
std::string kebab(const std::u16string& name) {
    std::string out;
    for (char16_t c : name) {
        if (c >= 'A' && c <= 'Z') {
            out += '-';
            out += (char)(c + 32);
        } else
            out += (char)c;
    }
    return out;
}
std::string cssName(const std::u16string& prop) {
    if (prop == u"cssFloat") return "float";
    if (prop.find(u'-') != std::u16string::npos) return utf16to8(prop);
    std::string k = kebab(prop);
    if (k.rfind("webkit-", 0) == 0 || k.rfind("moz-", 0) == 0 || k.rfind("ms-", 0) == 0) k = "-" + k;
    return k;
}
std::string styleValue(const Node* n, const std::string& name) {
    std::string val;
    if (const std::string* st = n->attr("style"))
        for (auto& d : parse_decls(*st))
            if (d.prop == name) val = d.val;
    return val;
}
void setStyleProp(Impl& im, Node* n, const std::string& name, const std::string& value, bool important = false) {
    std::vector<Decl> decls;
    if (const std::string* st = n->attr("style")) decls = parse_decls(*st);
    decls.erase(std::remove_if(decls.begin(), decls.end(), [&](const Decl& d) { return d.prop == name; }), decls.end());
    if (!value.empty()) decls.push_back({name, value, important});
    std::string out;
    for (auto& d : decls) out += d.prop + ": " + d.val + (d.important ? " !important" : "") + "; ";
    if (!out.empty()) out.pop_back();
    im.changeAttribute(n, "style", &out);
}
bool ownsKey(JSObject* o, PropKey k) { return !k.isString() || (o->proto && o->proto->find(k)); }
bool styleHookGet(Vm& vm, JSObject* o, PropKey k, Value& out) {
    if (ownsKey(o, k)) return false;
    out = val8(vm, styleValue((Node*)((JSHost*)o)->ptr, cssName(k.str()->s)));
    return true;
}
bool styleHookSet(Vm& vm, JSObject* o, PropKey k, Value v) {
    if (ownsKey(o, k)) return false;
    setStyleProp(I(vm), (Node*)((JSHost*)o)->ptr, cssName(k.str()->s), v.isNullish() ? "" : str8(vm, v));
    I(vm).flushReactions();  // an assignment is no call of a native function: the reactions it caused run here
    return true;
}
const HostHooks kStyleHooks{styleHookGet, styleHookSet, nullptr, nullptr};
JSFN(styleGet) {
    Vm& vm = ci.vm;
    Node* n = thisElem(ci);
    JSHost* h = vm.alloc<JSHost>();
    h->hostKind = HK_Style;
    h->ptr = n;
    h->proto = I(vm).styleProto;
    h->hooks = &kStyleHooks;
    return V(h);
}
JSFN(styleSet) {  // el.style = "color: red"
    std::string value = str8(ci.vm, ci.arg(0));
    I(ci.vm).changeAttribute(thisElem(ci), "style", &value);
    return Value::undef();
}
JSFN(styleSetProperty) {
    Node* n = (Node*)thisHost(ci, HK_Style)->ptr;
    setStyleProp(I(ci.vm), n, cssName(ci.vm.toStr16(ci.arg(0))), ci.arg(1).isNullish() ? "" : str8(ci.vm, ci.arg(1)), str8(ci.vm, ci.arg(2)) == "important");
    return Value::undef();
}
JSFN(styleGetPropertyValue) { return val8(ci.vm, styleValue((Node*)thisHost(ci, HK_Style)->ptr, cssName(ci.vm.toStr16(ci.arg(0))))); }
JSFN(styleRemoveProperty) {
    Node* n = (Node*)thisHost(ci, HK_Style)->ptr;
    std::string name = cssName(ci.vm.toStr16(ci.arg(0)));
    std::string old = styleValue(n, name);
    setStyleProp(I(ci.vm), n, name, "");
    return val8(ci.vm, old);
}
JSFN(styleCssTextGet) {
    const std::string* st = ((Node*)thisHost(ci, HK_Style)->ptr)->attr("style");
    return st ? val8(ci.vm, *st) : V(ci.vm.a.empty);
}
JSFN(styleCssTextSet) {
    std::string value = str8(ci.vm, ci.arg(0));
    I(ci.vm).changeAttribute((Node*)thisHost(ci, HK_Style)->ptr, "style", &value);
    return Value::undef();
}

std::string dataAttr(PropKey k) { return "data-" + kebab(k.str()->s); }
bool dataHookGet(Vm& vm, JSObject* o, PropKey k, Value& out) {
    if (!k.isString()) return false;
    const std::string* v = ((Node*)((JSHost*)o)->ptr)->attr(dataAttr(k));
    if (!v) return false;
    out = val8(vm, *v);
    return true;
}
bool dataHookSet(Vm& vm, JSObject* o, PropKey k, Value v) {
    if (!k.isString()) return false;
    std::string value = str8(vm, v);
    I(vm).changeAttribute((Node*)((JSHost*)o)->ptr, dataAttr(k), &value);
    I(vm).flushReactions();
    return true;
}
bool dataHookDel(Vm& vm, JSObject* o, PropKey k) {
    if (!k.isString()) return false;
    I(vm).changeAttribute((Node*)((JSHost*)o)->ptr, dataAttr(k), nullptr);
    I(vm).flushReactions();
    return true;
}
void dataHookKeys(Vm& vm, JSObject* o, std::vector<PropKey>& out) {
    for (auto& a : ((Node*)((JSHost*)o)->ptr)->attrs) {
        if (a.first.rfind("data-", 0) != 0) continue;
        std::u16string name;
        bool up = false;
        for (size_t i = 5; i < a.first.size(); ++i) {
            char c = a.first[i];
            if (c == '-') up = true;
            else name += (char16_t)(up && c >= 'a' && c <= 'z' ? c - 32 : c), up = false;
        }
        out.push_back(PropKey::of(vm.atom(std::u16string_view(name))));
    }
}
const HostHooks kDataHooks{dataHookGet, dataHookSet, dataHookDel, dataHookKeys};
JSFN(datasetGet) {
    JSHost* h = ci.vm.alloc<JSHost>();
    h->hostKind = HK_Dataset;
    h->ptr = thisElem(ci);
    h->proto = ci.vm.objectProto;
    h->hooks = &kDataHooks;
    return V(h);
}

// ---- getComputedStyle
// The element whose box holds this element's box: its parent in the flat tree, since shadow roots and slots have no box.
const Node* boxParent(const Node* n) {
    const Node* p = flat_parent(n);
    while (p && (p->isShadowRoot() || p->name == "slot")) p = flat_parent(p);
    return p;
}
// The resolved value of a property (CSSOM 9, "Resolved Values") for the properties the style engine keeps. A property
// it does not keep reads as the element's inline declaration, or as nothing.
// Layout records an element's border box and nothing more, so width and height are the only used values there are.
// Margins, paddings and insets report their computed value instead: right for a length in px, while a percentage or
// `auto` reads as it was written, not in px.
// A <number> as CSSOM 6.7.2 serializes it: in decimal, rounded to six places, with no exponent and no trailing zeros.
std::string cssNumber(double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.7g", v);  // values come from floats: drop the digits a float does not have first
    v = strtod(buf, nullptr);
    snprintf(buf, sizeof buf, "%.6f", v);
    std::string out = buf;
    out.erase(out.find_last_not_of('0') + 1);
    if (out.back() == '.') out.pop_back();
    return out == "-0" ? "0" : out;
}
std::string resolvedValue(const Node* n, const Style& s, const std::string& name) {
    char buf[64];
    auto number = [&](float v, const char* unit) { return cssNumber(v) + unit; };
    // `keyword` is the name this property gives to Len::Auto or Len::None
    auto length = [&](const Len& l, const char* keyword) -> std::string {
        if (l.u == Len::Px) return number(l.v, "px");
        if (l.u != Len::Pct) return keyword;
        if (l.off == 0) return number(l.v, "%");
        snprintf(buf, sizeof buf, "calc(%g%% + %gpx)", l.v, l.off);
        return buf;
    };
    auto rgb = [&](const Color& c) {  // rgb(r, g, b), or rgba() when not opaque
        int r = int(c.r * 255 + .5f), g = int(c.g * 255 + .5f), bl = int(c.b * 255 + .5f);
        if (c.a >= 0.999f) snprintf(buf, sizeof buf, "rgb(%d, %d, %d)", r, g, bl);
        else snprintf(buf, sizeof buf, "rgba(%d, %d, %d, %g)", r, g, bl, c.a < 0 ? 0.0 : (double)c.a);
        return std::string(buf);
    };
    auto oneOf = [](size_t i, std::initializer_list<const char*> keywords) { return std::string(i < keywords.size() ? keywords.begin()[i] : ""); };

    if (name.rfind("--", 0) == 0) {  // a custom property
        if (s.vars)
            if (auto it = s.vars->find(name); it != s.vars->end()) return it->second;
        return {};
    }
    static const char* sides[] = {"top", "right", "bottom", "left"};
    for (int i = 0; i < 4; ++i) {
        if (name == sides[i]) return length(s.inset[i], "auto");
        if (name == std::string("margin-") + sides[i]) return length(s.margin[i], "auto");
        if (name == std::string("padding-") + sides[i]) return length(s.padding[i], "0px");
        if (name == std::string("border-") + sides[i] + "-width") return number(s.border[i], "px");
        if (name == std::string("border-") + sides[i] + "-color") return rgb(s.borderColor[i]);
    }
    if (name == "width" || name == "height") {
        bool horizontal = name == "width";
        // the property does not apply to an inline box that is not replaced, and without a box there is no used value
        if (!n->hasBox || (s.display == Display::Inline && !replaced_like(n))) return length(horizontal ? s.width : s.height, "auto");
        float used = horizontal ? n->bw : n->bh;
        if (!s.borderBox && !replaced_like(n))  // layout gives a replaced element neither padding nor border
            for (int side : {horizontal ? 1 : 0, horizontal ? 3 : 2}) used -= s.border[side] + s.padding[side].resolve(0);
        return number(std::max(0.f, used), "px");
    }
    if (name == "min-width" || name == "min-height") {
        // An automatic minimum size resolves to zero where there is no box and on a box laid out the CSS 2 way,
        // which leaves flex items (CSS Sizing 3, 3.1.2).
        const Node* holder = n->hasBox ? boxParent(n) : nullptr;
        bool flexItem = holder && holder->style && (holder->style->display == Display::Flex || holder->style->display == Display::InlineFlex);
        return length(name == "min-width" ? s.minWidth : s.minHeight, flexItem ? "auto" : "0px");
    }
    if (name == "max-width") return length(s.maxWidth, "none");
    if (name == "max-height") return length(s.maxHeight, "none");
    if (name == "display") return display_name(s.display);
    if (name == "visibility") return s.hidden ? "hidden" : "visible";
    if (name == "color") return rgb(s.color);
    if (name == "background-color") return rgb(s.bg);
    if (name == "opacity") return number(s.opacity, "");
    if (name == "position") return oneOf(s.position, {"static", "relative", "absolute", "fixed", "sticky"});
    if (name == "z-index") return s.zAuto ? "auto" : std::to_string(s.zIndex);
    if (name == "box-sizing") return s.borderBox ? "border-box" : "content-box";
    if (name == "overflow" || name == "overflow-x" || name == "overflow-y") return oneOf(s.overflow, {"visible", "hidden", "auto"});
    if (name == "float") return oneOf(s.floatSide, {"none", "left", "right"});
    if (name == "font-size") return number(s.fontSize, "px");
    if (name == "font-weight") return std::to_string(s.weight);
    if (name == "font-style") return s.italic ? "italic" : "normal";
    // `normal` is kept as the factor 1.2, so a line-height of exactly 1.2 reads as normal too
    if (name == "line-height") return s.lhPx < 0 && s.lhFactor == 1.2f ? "normal" : number(s.lh(), "px");
    // the style keeps `left` and the initial `start` as one value
    if (name == "text-align") return oneOf(s.textAlign, {"start", "right", "center"});
    if (name == "white-space") return oneOf(s.whiteSpace, {"normal", "pre", "nowrap", "pre-wrap", "pre-line"});
    if (name == "text-transform") return oneOf(s.textTransform, {"none", "uppercase", "lowercase", "capitalize"});
    if (name == "vertical-align") return oneOf(s.vAlign, {"baseline", "top", "middle", "bottom", "sub", "super"});
    if (name == "text-decoration-line" || name == "text-decoration")
        return s.underline && s.strike ? "underline line-through" : s.underline ? "underline" : s.strike ? "line-through" : "none";
    if (name == "flex-direction") return oneOf(s.flexDir, {"row", "row-reverse", "column", "column-reverse"});
    if (name == "flex-wrap") return s.flexWrap ? "wrap" : "nowrap";
    if (name == "transform") {  // CSS Transforms 1, section 9: the matrix of the used value
        if (!s.transform) return "none";
        float m[6];
        transform_matrix(*s.transform, n->hasBox ? n->bw : 0, n->hasBox ? n->bh : 0, m);
        std::string out = "matrix(";
        for (int i = 0; i < 6; ++i) out += cssNumber(m[i]) + (i < 5 ? ", " : ")");
        return out;
    }
    if (name == "flex-grow") return number(s.flexGrow, "");
    if (name == "flex-shrink") return number(s.flexShrink, "");
    return styleValue(n, name);
}
bool computedHookGet(Vm& vm, JSObject* o, PropKey k, Value& out) {
    if (ownsKey(o, k)) return false;
    Node* n = (Node*)((JSHost*)o)->ptr;
    out = val8(vm, n->style ? resolvedValue(n, *n->style, cssName(k.str()->s)) : std::string());
    return true;
}
const HostHooks kComputedHooks{computedHookGet, nullptr, nullptr, nullptr};
JSFN(getComputedStyle) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    Node* n = toNode(ci.arg(0));
    if (!n || n->type != Node::T::Element) vm.throwError(Err::TypeError, "getComputedStyle: parameter 1 is not an Element");
    im.client.ensureLayout();
    JSHost* h = vm.alloc<JSHost>();
    h->hostKind = HK_Computed;
    h->ptr = n;
    h->proto = im.computedProto;
    h->hooks = &kComputedHooks;
    return V(h);
}
JSFN(computedGetPropertyValue) {
    Value out;
    JSHost* h = thisHost(ci, HK_Computed);
    PropKey k = PropKey::of(ci.vm.atom(std::u16string_view(ci.vm.toStr16(ci.arg(0)))));
    return computedHookGet(ci.vm, h, k, out) ? out : V(ci.vm.a.empty);
}

// ---------------------------------------------------------------------------- events
const void* targetKey(CallInfo& ci, Node*& node) {
    node = toNode(ci.thisv);
    if (node) return node;
    if (ci.thisv.isNullish() || (ci.thisv.isObject() && ci.thisv.asObject() == ci.vm.global)) return &I(ci.vm);
    illegal(ci, "the right kind of object");
}
JSFN(addEventListener) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    Node* node = nullptr;
    const void* key = targetKey(ci, node);
    Value fn = ci.arg(1), opts = ci.arg(2);
    if (!fn.isObject()) return Value::undef();
    Listener l;
    l.type = str8(vm, ci.arg(0));
    l.fn = fn;
    if (opts.isObject()) {
        l.capture = vm.toBool(vm.get(opts, "capture"));
        l.once = vm.toBool(vm.get(opts, "once"));
    } else
        l.capture = vm.toBool(opts);
    auto& list = im.listeners[key];
    for (auto& e : list)
        if (e.type == l.type && e.fn == l.fn && e.capture == l.capture) return Value::undef();
    list.push_back(l);
    return Value::undef();
}
JSFN(removeEventListener) {
    Vm& vm = ci.vm;
    Node* node = nullptr;
    const void* key = targetKey(ci, node);
    Value opts = ci.arg(2);
    bool capture = opts.isObject() ? vm.toBool(vm.get(opts, "capture")) : vm.toBool(opts);
    I(vm).removeListener(key, str8(vm, ci.arg(0)), ci.arg(1), capture);
    return Value::undef();
}
JSFN(dispatchEventFn) {
    Vm& vm = ci.vm;
    Node* node = nullptr;
    targetKey(ci, node);
    Value ev = ci.arg(0);
    if (!ev.isObject() || ev.asObject()->cls != Cls::Host || ((JSHost*)ev.asObject())->hostKind != HK_Event)
        vm.throwError(Err::TypeError, "dispatchEvent: parameter 1 is not an Event");
    return Value::boolean(I(vm).fire(node, ev));
}
JSFN(eventCtor) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Event constructor requires 'new'");
    Value init = ci.arg(1);
    bool bubbles = init.isObject() && vm.toBool(vm.get(init, "bubbles")), cancelable = init.isObject() && vm.toBool(vm.get(init, "cancelable"));
    Value ev = im.makeEvent(str8(vm, ci.arg(0)), bubbles, cancelable);
    JSObject* e = ev.asObject();
    e->proto = vm.protoFromNewTarget(ci.newTarget, im.eventProto);
    vm.def(e, "isTrusted", Value::boolean(false), PF_ALL);
    vm.def(e, "composed", Value::boolean(false), PF_ALL);
    vm.def(e, "detail", Value::null(), PF_ALL);
    if (init.isObject()) vm.copyDataProps(e, init, nullptr);
    // PopStateEvent and HashChangeEvent keep their attributes inside the event; the prototype's getters read them.
    auto is = [&](const char* kind) {
        JSObject* wanted = im.eventProtos[kind];
        for (JSObject* p = e->proto; p; p = p->proto)
            if (p == wanted) return true;
        return false;
    };
    auto take = [&](const char* name, Value fallback) {  // the member of the init dictionary, which is not left on the event
        PropKey key = PropKey::of(vm.atom(name));
        Value v = init.isObject() ? vm.get(init, key) : Value::undef();
        vm.del(e, key);
        return v.isUndef() ? fallback : v;
    };
    JSHost* host = (JSHost*)e;
    if (is("PopStateEvent")) host->slotA = take("state", Value::null());
    if (is("HashChangeEvent")) {
        host->slotA = vm.strv(vm.toStr16(take("oldURL", V(vm.a.empty))));
        host->slotB = vm.strv(vm.toStr16(take("newURL", V(vm.a.empty))));
    }
    return ev;
}
JSFN(eventSlotGet) {  // data: 0 for the first value kept inside the event, 1 for the second
    JSHost* e = thisHost(ci, HK_Event);
    return ci.callee->data.asInt() == 0 ? e->slotA : e->slotB;
}
JSFN(eventPreventDefault) {
    JSHost* e = thisHost(ci, HK_Event);
    if (ci.vm.toBool(ci.vm.get(ci.thisv, "cancelable"))) e->flagC = true;
    return Value::undef();
}
JSFN(eventStopPropagation) {
    thisHost(ci, HK_Event)->flagA = true;
    return Value::undef();
}
JSFN(eventStopImmediate) {
    JSHost* e = thisHost(ci, HK_Event);
    e->flagA = e->flagB = true;
    return Value::undef();
}
JSFN(eventDefaultPrevented) { return Value::boolean(thisHost(ci, HK_Event)->flagC); }
JSFN(eventInit) {
    Vm& vm = ci.vm;
    thisHost(ci, HK_Event);
    vm.set(ci.thisv, "type", V(vm.toString(ci.arg(0))));
    vm.set(ci.thisv, "bubbles", Value::boolean(vm.toBool(ci.arg(1))));
    vm.set(ci.thisv, "cancelable", Value::boolean(vm.toBool(ci.arg(2))));
    if (ci.argc > 3) vm.set(ci.thisv, "detail", ci.args[3]);
    return Value::undef();
}
JSFN(eventComposedPath) {  // the nodes the event visits: all the way up when composed, else only to its shadow root
    Vm& vm = ci.vm;
    JSArray* a = vm.newArray();
    bool composed = vm.toBool(vm.get(ci.thisv, "composed"));
    Node* n = toNode(vm.get(ci.thisv, "target"));
    for (; n; n = n->parent) {
        a->elems.push_back(I(vm).wrap(n));
        if (n->isShadowRoot() && !composed) return V(a);
    }
    a->elems.push_back(V(vm.global));
    return V(a);
}
JSFN(createEvent) {
    Value ev = I(ci.vm).makeEvent("", false, false);
    ci.vm.def(ev.asObject(), "detail", Value::null(), PF_ALL);
    return ev;
}

// ---------------------------------------------------------------------------- timers and window
Value addTimer(CallInfo& ci, int kind) {  // 0 timeout, 1 interval, 2 animation frame
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    Value fn = ci.arg(0);
    if (fn.isString()) fn = vm.compileFunction(u"", fn.asString()->s, "timer");
    if (!vm.isCallable(fn) || im.dead) return Value::i32(0);
    double ms = kind == 2 ? 16 : ci.argc > 1 ? vm.toNumber(ci.args[1]) : 0;
    if (!(ms > 0)) ms = 0;
    if (kind == 1 && ms < 4) ms = 4;
    Timer t;
    t.id = im.nextTimer++;
    t.due = now_ms() + ms;
    t.interval = kind == 1 ? ms : -1;
    t.fn = fn;
    t.frame = kind == 2;
    for (int i = 2; i < ci.argc && kind != 2; ++i) t.args.push_back(ci.args[i]);
    im.timers.push_back(std::move(t));
    im.client.wake(ms);
    return Value::i32(im.timers.back().id);
}
JSFN(setTimeoutFn) { return addTimer(ci, 0); }
JSFN(setIntervalFn) { return addTimer(ci, 1); }
JSFN(requestAnimationFrameFn) { return addTimer(ci, 2); }
JSFN(clearTimerFn) {
    Impl& im = I(ci.vm);
    int id = ci.vm.toInt32(ci.arg(0));
    im.timers.erase(std::remove_if(im.timers.begin(), im.timers.end(), [id](const Timer& t) { return t.id == id; }), im.timers.end());
    return Value::undef();
}
JSFN(alertFn) {
    I(ci.vm).client.consoleMessage(1, L"alert: " + std::wstring(ci.vm.toStr16(ci.arg(0)).begin(), ci.vm.toStr16(ci.arg(0)).end()));
    return Value::undef();
}
JSFN(confirmFn) { return Value::boolean(true); }
JSFN(promptFn) { return Value::null(); }
JSFN(noopFn) { return Value::undef(); }
JSFN(performanceNow) { return Value::dbl(now_ms() - I(ci.vm).startMs); }
JSFN(viewportGet) {  // data: 0 innerWidth, 1 innerHeight, 2 scrollY, 3 scrollX
    float w, h, sy;
    I(ci.vm).client.viewport(w, h, sy);
    int which = ci.callee->data.asInt();
    return Value::num(std::round(which == 0 ? w : which == 1 ? h : which == 2 ? sy : 0));
}
JSFN(scrollToFn) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    Value a0 = ci.arg(0);
    double y = a0.isObject() ? vm.toNumber(vm.get(a0, "top")) : vm.toNumber(ci.arg(1));
    if (ci.callee->data.asInt() == 1) {  // scrollBy
        float w, h, sy;
        im.client.viewport(w, h, sy);
        y += sy;
    }
    if (y == y) im.client.scrollTo((float)y);
    return Value::undef();
}
JSFN(matchMediaFn) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    std::string q = lower(str8(vm, ci.arg(0)));
    float w, h, sy;
    im.client.viewport(w, h, sy);
    bool ok = !contains(q, "print") && !contains(q, "prefers-color-scheme: dark") && !contains(q, "prefers-reduced-motion: reduce") && !contains(q, "hover: none") && !contains(q, "pointer: coarse");
    auto bound = [&](const char* key, bool isMin) {
        size_t p = q.find(key);
        if (p == std::string::npos) return;
        double v = atof(q.c_str() + p + strlen(key));
        if (contains(std::string_view(q).substr(p, 40), "em")) v *= 16;
        if (isMin ? w < v : w > v) ok = false;
    };
    bound("min-width:", true);
    bound("max-width:", false);
    JSObject* o = vm.newObject();
    vm.def(o, "matches", Value::boolean(ok), PF_ALL);
    vm.def(o, "media", ci.arg(0), PF_ALL);
    for (const char* n : {"addListener", "removeListener", "addEventListener", "removeEventListener"}) vm.defFn(o, n, noopFn, 1);
    return V(o);
}
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
JSFN(btoaFn) {
    Vm& vm = ci.vm;
    const std::u16string& s = vm.toStr16(ci.arg(0));
    std::string out;
    for (size_t i = 0; i < s.size(); i += 3) {
        uint32_t b[3] = {0, 0, 0};
        size_t n = std::min<size_t>(3, s.size() - i);
        for (size_t k = 0; k < n; ++k) {
            if (s[i + k] > 255) vm.throwError(Err::Error, "InvalidCharacterError: btoa: the string contains characters outside of the Latin1 range");
            b[k] = s[i + k];
        }
        uint32_t v = (b[0] << 16) | (b[1] << 8) | b[2];
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += n > 1 ? kB64[(v >> 6) & 63] : '=';
        out += n > 2 ? kB64[v & 63] : '=';
    }
    return vm.strv(out);
}
JSFN(atobFn) {
    Vm& vm = ci.vm;
    std::string in = str8(vm, ci.arg(0));
    std::u16string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (isws(c) || c == '=') continue;
        const char* p = strchr(kB64, c);
        if (!p || !c) vm.throwError(Err::Error, "InvalidCharacterError: atob: the string is not correctly encoded");
        acc = (acc << 6) | (uint32_t)(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char16_t)((acc >> bits) & 255);
        }
    }
    return vm.strv(std::move(out));
}

// ---- location
JSFN(locationGet) {  // data: which part
    Vm& vm = ci.vm;
    Url u = Url::parse(I(vm).url);
    std::string v;
    switch (ci.callee->data.asInt()) {
        case 0: v = I(vm).url; break;
        case 1: v = u.scheme + ":"; break;
        case 2: v = u.hostport(); break;
        case 3: v = u.host; break;
        case 4: v = u.port; break;
        case 5: v = u.path.empty() ? "/" : u.path; break;
        case 6: v = u.hasQuery && !u.query.empty() ? "?" + u.query : ""; break;
        case 7: v = u.hasFragment && !u.fragment.empty() ? "#" + u.fragment : ""; break;
        default: v = u.hierarchical && !u.host.empty() ? u.origin() : "null";
    }
    return val8(vm, v);
}
JSFN(locationSet) {  // href, hash, search, pathname assignments and assign()/replace(): data2 = 1 for replace
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    std::string v = str8(vm, ci.arg(0));
    int which = ci.callee->data.asInt();
    if (which == 7 && !v.empty() && v[0] != '#') v = "#" + v;
    if (which == 6 && !v.empty() && v[0] != '?') v = "?" + v;
    im.client.navigate(resolve_url(im.url, v), ci.callee->data2.asInt() == 1);
    return Value::undef();
}
JSFN(locationReload) {
    I(ci.vm).client.navigate(I(ci.vm).url, true);
    return Value::undef();
}
JSFN(locationToString) { return val8(ci.vm, I(ci.vm).url); }
// ---- history (HTML 7.2.5, the History interface)
[[noreturn]] void throwDom(Impl& im, const char* name, const char* message) {
    Value args[] = {val8(im.vm, message), val8(im.vm, name)};
    im.vm.throwValue(im.vm.construct(im.domException, args, 2, im.domException));
}
JSFN(domExceptionSet) {  // __domException(class): the prelude hands over its DOMException
    I(ci.vm).domException = ci.arg(0);
    return Value::undef();
}
Impl& historyOf(CallInfo& ci) {  // the members of History work on the history object and nothing else
    Impl& im = I(ci.vm);
    if (!ci.thisv.isObject() || ci.thisv.asObject() != im.historyObject) ci.vm.throwError(Err::TypeError, "Illegal invocation");
    return im;
}
// Whether a document at `from` can have its URL rewritten to `to` (HTML 7.2.5).
bool canRewriteUrl(const std::string& from, const std::string& to) {
    Url a = Url::parse(from), b = Url::parse(to);
    if (!b.valid || a.scheme != b.scheme || a.host != b.host || a.port != b.port) return false;
    if (b.scheme == "http" || b.scheme == "https") return true;
    if (a.path != b.path) return false;
    return b.scheme == "file" || a.query == b.query;
}
JSFN(historyPushState) {  // data: 0 pushState, 1 replaceState (the shared history push/replace state steps)
    Vm& vm = ci.vm;
    Impl& im = historyOf(ci);
    bool push = ci.callee->data.asInt() == 0;
    Value state = ci.arg(0).isUndef() ? Value::null() : vm.call(im.cloneFn, Value::undef(), ci.arg(0));
    std::vector<Value> held{state};
    RootScope keep(vm, held);
    std::string next = im.url;
    Value to = ci.arg(2);
    if (!to.isNullish() && !str8(vm, to).empty()) {
        next = resolve_url(im.url, str8(vm, to));
        if (!canRewriteUrl(im.url, next)) throwDom(im, "SecurityError", "The URL of a history state must stay within the document's origin");
    }
    // the URL and history update steps (HTML 7.4.4): no event is fired
    if (push) {
        im.session.resize(im.sessionAt + 1);
        im.session.push_back({next, state});
        ++im.sessionAt;
    } else {
        im.session[im.sessionAt] = {next, state};
    }
    im.url = next;
    im.client.urlChanged(next, push);
    return Value::undef();
}
JSFN(historyGet) {  // data: 0 length, 1 state, 2 scrollRestoration
    Impl& im = historyOf(ci);
    switch (ci.callee->data.asInt()) {
        case 0: return Value::i32(im.client.historyLength());
        case 1: return im.session[im.sessionAt].state;
        default: return val8(ci.vm, im.manualScrollRestoration ? "manual" : "auto");
    }
}
JSFN(historySetScrollRestoration) {
    Impl& im = historyOf(ci);
    std::string mode = str8(ci.vm, ci.arg(0));
    if (mode == "auto" || mode == "manual") im.manualScrollRestoration = mode == "manual";  // other values are ignored
    return Value::undef();
}
JSFN(historyGo) {  // data: the delta of back() and forward(), 0 for go(delta)
    Impl& im = historyOf(ci);
    int delta = ci.callee->data.asInt();
    if (delta == 0) delta = ci.vm.toInt32(ci.arg(0));
    if (delta == 0) im.client.navigate(im.url, true);  // go(0) reloads
    else im.client.traverse(delta);
    return Value::undef();
}

// ---- storage
Store& storeOf(JSObject* o) { return *(Store*)((JSHost*)o)->ptr; }
bool storageHookGet(Vm& vm, JSObject* o, PropKey k, Value& out) {
    if (ownsKey(o, k)) return false;
    for (auto& e : storeOf(o))
        if (e.first == k.str()->s) {
            out = vm.strv(e.second);
            return true;
        }
    return false;
}
void storePut(Store& s, const std::u16string& key, const std::u16string& val) {
    for (auto& e : s)
        if (e.first == key) {
            e.second = val;
            return;
        }
    s.push_back({key, val});
}
bool storageHookSet(Vm& vm, JSObject* o, PropKey k, Value v) {
    if (ownsKey(o, k)) return false;
    storePut(storeOf(o), k.str()->s, vm.toStr16(v));
    return true;
}
bool storeErase(Store& s, const std::u16string& key) {
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i].first == key) {
            s.erase(s.begin() + i);
            return true;
        }
    return false;
}
bool storageHookDel(Vm&, JSObject* o, PropKey k) { return k.isString() && storeErase(storeOf(o), k.str()->s); }
void storageHookKeys(Vm& vm, JSObject* o, std::vector<PropKey>& out) {
    for (auto& e : storeOf(o)) out.push_back(PropKey::of(vm.atom(std::u16string_view(e.first))));
}
const HostHooks kStorageHooks{storageHookGet, storageHookSet, storageHookDel, storageHookKeys};
JSFN(storageOp) {  // data: 0 getItem, 1 setItem, 2 removeItem, 3 clear, 4 key, 5 length
    Vm& vm = ci.vm;
    Store& s = storeOf(thisHost(ci, HK_Storage));
    switch (ci.callee->data.asInt()) {
        case 0: {
            std::u16string key = vm.toStr16(ci.arg(0));
            for (auto& e : s)
                if (e.first == key) return vm.strv(e.second);
            return Value::null();
        }
        case 1: {
            std::u16string key = vm.toStr16(ci.arg(0));
            storePut(s, key, vm.toStr16(ci.arg(1)));
            return Value::undef();
        }
        case 2: storeErase(s, vm.toStr16(ci.arg(0))); return Value::undef();
        case 3: s.clear(); return Value::undef();
        case 4: {
            double i = vm.toInteger(ci.arg(0));
            return i >= 0 && i < (double)s.size() ? vm.strv(s[(size_t)i].first) : Value::null();
        }
        default: return Value::num((double)s.size());
    }
}

// ---------------------------------------------------------------------------- document
JSFN(docElementGet) {  // data: 0 documentElement, 1 head, 2 body
    Impl& im = I(ci.vm);
    thisNode(ci);
    int w = ci.callee->data.asInt();
    return im.wrap(w == 0 ? im.doc.html : w == 1 ? im.doc.head : im.doc.body);
}
JSFN(docTitleGet) { return val8(ci.vm, I(ci.vm).doc.title); }
JSFN(docTitleSet) {
    Impl& im = I(ci.vm);
    im.doc.title = str8(ci.vm, ci.arg(0));
    im.client.titleChanged();
    return Value::undef();
}
JSFN(docUrlGet) { return val8(ci.vm, I(ci.vm).url); }
JSFN(docReadyState) { return val8(ci.vm, I(ci.vm).readyState); }
JSFN(docCurrentScript) { return I(ci.vm).currentScript; }
JSFN(docActiveElement) {
    Impl& im = I(ci.vm);
    Node* f = im.client.focusedNode();
    return im.wrap(f ? f : im.doc.body);
}
JSFN(docDomain) { return val8(ci.vm, Url::parse(I(ci.vm).url).host); }
JSFN(createElement) {
    Impl& im = I(ci.vm);
    return im.wrap(im.newElement(str8(ci.vm, ci.arg(ci.callee->data.asInt()))));  // createElementNS passes the name second
}
JSFN(createTextNode) {
    Impl& im = I(ci.vm);
    return im.wrap(im.newText(str8(ci.vm, ci.arg(0)), ci.callee->data.asInt() == 1 ? Node::T::Comment : Node::T::Text));
}
JSFN(createDocumentFragment) {
    Impl& im = I(ci.vm);
    Node* f = im.newElement("#document-fragment");
    return im.wrap(f);
}
JSFN(docCollection) {  // data: 0 forms, 1 images, 2 links, 3 scripts
    static const char* tags[] = {"form", "img", "a", "script"};
    const char* tag = tags[ci.callee->data.asInt()];
    std::vector<Node*> out;
    eachElement(thisNode(ci), [&](Node* e) {
        if (e->name == tag && (e->name != "a" || e->attr("href"))) out.push_back(e);
        return true;
    });
    return I(ci.vm).list(out);
}
JSFN(docWrite) {  // late document.write: the text is parsed and appended to the body
    Impl& im = I(ci.vm);
    std::string html;
    for (int i = 0; i < ci.argc; ++i) html += str8(ci.vm, ci.args[i]);
    if (!im.doc.body) return Value::undef();
    Node* tmp = im.newElement("div");
    im.setInner(tmp, html);
    std::vector<Node*> kids;
    for (auto& k : tmp->kids) kids.push_back(k.get());
    for (Node* k : kids) im.insert(im.doc.body, k, nullptr);
    return Value::undef();
}
JSFN(trueFn) { return Value::boolean(true); }
JSFN(emptyStringFn) { return V(ci.vm.a.empty); }
JSFN(illegalCtor) { ci.vm.throwError(Err::TypeError, "Illegal constructor"); }
JSFN(observerCtor) {  // MutationObserver and friends exist so that feature checks pass; they never fire yet
    Vm& vm = ci.vm;
    JSObject* o = vm.newObject(vm.protoFromNewTarget(ci.newTarget, ci.callee->data2.asObject()));
    return V(o);
}
JSFN(emptyArrayFn) { return V(ci.vm.newArray()); }

// ---- MutationObserver
JSFN(mutationObserverCtor) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    if (!ci.isConstruct() || !vm.isCallable(ci.arg(0))) vm.throwError(Err::TypeError, "MutationObserver: a callback function and 'new' are required");
    JSHost* h = vm.alloc<JSHost>();
    h->hostKind = HK_Observer;
    h->proto = vm.protoFromNewTarget(ci.newTarget, im.observerProto);
    auto ob = std::make_unique<Observer>();
    ob->object = V(h);
    ob->callback = ci.arg(0);
    h->ptr = ob.get();
    im.observers.push_back(std::move(ob));
    return V(h);
}
JSFN(mutationObserve) {
    Vm& vm = ci.vm;
    Observer* ob = (Observer*)thisHost(ci, HK_Observer)->ptr;
    Node* n = argNode(ci, 0, "MutationObserver.observe");
    Value o = ci.arg(1);
    ObserveTarget t{n, false, false, false, false};
    if (o.isObject()) {
        t.childList = vm.toBool(vm.get(o, "childList"));
        Value filter = vm.get(o, "attributeFilter");
        t.attributes = vm.toBool(vm.get(o, "attributes")) || !filter.isUndef() || vm.toBool(vm.get(o, "attributeOldValue"));
        if (filter.isObject()) {
            t.filtered = true;
            uint32_t count = vm.lengthOf(filter);
            for (uint32_t i = 0; i < count; ++i) t.filter.push_back(str8(vm, vm.get(filter, PropKey::index(i))));
        }
        t.characterData = vm.toBool(vm.get(o, "characterData")) || vm.toBool(vm.get(o, "characterDataOldValue"));
        t.subtree = vm.toBool(vm.get(o, "subtree"));
    }
    for (auto& e : ob->targets)
        if (e.node == n) {
            e = t;
            return Value::undef();
        }
    ob->targets.push_back(t);
    return Value::undef();
}
JSFN(mutationDisconnect) {
    Observer* ob = (Observer*)thisHost(ci, HK_Observer)->ptr;
    ob->targets.clear();
    ob->records.clear();
    return Value::undef();
}
JSFN(mutationTakeRecords) {
    Observer* ob = (Observer*)thisHost(ci, HK_Observer)->ptr;
    JSArray* a = ci.vm.newArray();
    a->elems.swap(ob->records);
    return V(a);
}
JSFN(deliverMutations) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    im.observersScheduled = false;
    for (size_t i = 0; i < im.observers.size(); ++i) {
        Observer* ob = im.observers[i].get();
        if (ob->records.empty()) continue;
        JSArray* a = vm.newArray();
        a->elems.swap(ob->records);
        Value args[2] = {V(a), ob->object};
        try {
            vm.call(ob->callback, ob->object, args, 2);
        } catch (JSThrow&) {
            im.reportError();
        }
    }
    return Value::undef();
}

// ---- cookies as script sees them (never HttpOnly ones)
JSFN(docCookieGet) {
    Url u = Url::parse(I(ci.vm).url);
    if (!u.valid || (u.scheme != "http" && u.scheme != "https")) return V(ci.vm.a.empty);
    return val8(ci.vm, CookieJar::get().header(u, false, false, true));
}
JSFN(docCookieSet) {
    Url u = Url::parse(I(ci.vm).url);
    if (u.valid && (u.scheme == "http" || u.scheme == "https")) CookieJar::get().store(u, str8(ci.vm, ci.arg(0)), true);
    return Value::undef();
}

// ---- network primitives behind fetch() and XMLHttpRequest (see script_prelude.h)
struct NetRequest {
    std::string url, origin, error;
    FetchOpts opts{.purpose = FetchOpts::For::Other};  // fetch() and XMLHttpRequest ask for anything unless the script says otherwise
    bool crossOrigin = false, credentials = false, binary = false;
};
// arguments: method, url, [[name, value]...], body, flags (1 = send credentials, 2 = binary response)
void buildRequest(CallInfo& ci, NetRequest& rq) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    rq.opts.method = upper(str8(vm, ci.arg(0)));
    rq.url = resolve_url(im.doc.baseUrl, str8(vm, ci.arg(1)));
    Url page = Url::parse(im.url), target = Url::parse(rq.url);
    rq.origin = page.valid && page.hierarchical ? page.origin() : "null";
    int flags = vm.toInt32(ci.arg(4));
    rq.credentials = (flags & 1) != 0;
    rq.binary = (flags & 2) != 0;
    bool web = target.scheme == "http" || target.scheme == "https";
    if (!target.valid || !(web || target.scheme == "data" || (target.scheme == "file" && page.scheme == "file"))) rq.error = "this kind of address cannot be requested from a page";
    else if (page.scheme == "https" && target.scheme == "http") rq.error = "blocked: an insecure request from a secure page (mixed content)";
    rq.crossOrigin = web && rq.origin != target.origin();
    rq.opts.referrer = im.url;
    rq.opts.firstPartySite = im.firstParty();
    Value hs = ci.arg(2);
    if (hs.isObject()) {
        uint32_t n = vm.lengthOf(hs);
        for (uint32_t i = 0; i < n; ++i) {
            Value pair = vm.get(hs, PropKey::index(i));
            std::string name = str8(vm, vm.get(pair, PropKey::index(0))), value = str8(vm, vm.get(pair, PropKey::index(1)));
            std::string ln = lower(name);
            static const char* forbidden[] = {"cookie", "cookie2", "host", "origin", "referer", "user-agent", "content-length", "connection", "accept-encoding",
                                              "accept-charset", "date", "expect", "te", "trailer", "transfer-encoding", "upgrade", "via", "keep-alive", "dnt"};
            bool skip = ln.rfind("sec-", 0) == 0 || ln.rfind("proxy-", 0) == 0 || ln.empty();
            for (const char* f : forbidden) skip |= ln == f;
            if (skip) continue;
            if (ln == "content-type") rq.opts.contentType = value;
            else rq.opts.headers.push_back({name, value});
        }
    }
    Value body = ci.arg(3);
    if (body.isString()) {
        rq.opts.body = utf16to8(body.asString()->s);
    } else if (body.isObject()) {
        JSObject* o = body.asObject();
        if (o->cls == Cls::ArrayBuffer) {
            JSArrayBuffer* b = (JSArrayBuffer*)o;
            rq.opts.body.assign(b->data(), b->data() + b->size());
        } else if (o->cls == Cls::TypedArray || o->cls == Cls::DataView) {
            JSTypedArray* t = (JSTypedArray*)o;
            size_t n = (size_t)typedLength(t) * kTaSize[(int)t->type];
            const uint8_t* p = t->buffer->data() + t->offset;
            rq.opts.body.assign(p, p + n);
        }
    }
    if (rq.opts.method == "GET" || rq.opts.method == "HEAD") rq.opts.body.clear();
}
JSFN(netStart) {
    Vm& vm = ci.vm;
    Impl& im = I(vm);
    NetRequest rq;
    buildRequest(ci, rq);
    if (!vm.isCallable(ci.arg(5))) return Value::i32(0);
    bool binary = rq.binary;
    Impl* self = &im;
    if (!rq.error.empty()) {  // refused before it starts: report it like any other failure, later
        int id = im.nextNet++;
        im.netJs[id] = ci.arg(5);
        im.netCallbacks[id] = [self, id, binary](FetchResult& r) { self->finishNet(id, binary, r); };
        FetchResult bad;
        bad.error = rq.error;
        bad.finalUrl = rq.url;
        {
            std::lock_guard<std::mutex> lk(im.net->mu);
            im.net->done.push_back({id, std::move(bad)});
        }
        im.client.wake(0);
        return Value::i32(id);
    }
    int id = im.nextNet;
    im.netJs[id] = ci.arg(5);
    im.startFetch(rq.url, rq.opts, rq.crossOrigin, rq.origin, rq.credentials, [self, id, binary](FetchResult& r) { self->finishNet(id, binary, r); });
    return Value::i32(id);
}
JSFN(netSyncFn) {
    Vm& vm = ci.vm;
    NetRequest rq;
    buildRequest(ci, rq);
    FetchResult r;
    if (!rq.error.empty()) r.error = rq.error;
    else r = corsFetch(rq.url, rq.opts, rq.crossOrigin, rq.origin, rq.credentials);
    JSArray* out = vm.newArray(6);
    Value vals[6];
    I(vm).netResult(r, rq.binary, vals);
    for (int i = 0; i < 6; ++i) out->elems[i] = vals[i];
    return V(out);
}
JSFN(netAbortFn) {
    Impl& im = I(ci.vm);
    int id = ci.vm.toInt32(ci.arg(0));
    im.netJs.erase(id);
    im.netCallbacks.erase(id);
    return Value::undef();
}
// [href, protocol, host, hostname, port, pathname, search, hash, origin] or null when the address is not valid
JSFN(urlParse) {
    Vm& vm = ci.vm;
    std::string text = std::string(trim(str8(vm, ci.arg(0))));
    if (!ci.arg(1).isUndef()) {
        std::string base = str8(vm, ci.arg(1));
        if (!Url::parse(base).valid) return Value::null();  // a base that is not a URL fails even for an absolute input
        text = resolve_url(base, text);
    }
    Url u = Url::parse(text);
    if (!u.valid) return Value::null();
    JSArray* a = vm.newArray();
    std::string path = u.hierarchical && u.path.empty() ? "/" : u.path;
    const std::string parts[] = {u.str(), u.scheme + ":", u.hostport(), u.host, u.port, path, u.hasQuery && !u.query.empty() ? "?" + u.query : "",
                                 u.hasFragment && !u.fragment.empty() ? "#" + u.fragment : "", u.hierarchical && !u.host.empty() ? u.origin() : "null"};
    for (auto& p : parts) a->elems.push_back(val8(vm, p));
    return V(a);
}
void drainNet(Impl& im) {
    std::vector<std::pair<int, FetchResult>> done;
    {
        std::lock_guard<std::mutex> lk(im.net->mu);
        done.swap(im.net->done);
    }
    for (auto& d : done) {
        if (im.dead) break;
        auto it = im.netCallbacks.find(d.first);
        if (it == im.netCallbacks.end()) continue;
        std::function<void(FetchResult&)> cb = std::move(it->second);
        im.netCallbacks.erase(it);
        Entry entry(im);
        cb(d.second);
    }
}

}  // namespace

// ---------------------------------------------------------------------------- setup
void ScriptHost::Impl::setup() {
    vm.hostData = this;
    vm.traceThrows = getenv("TB_TRACE_THROWS") != nullptr;
    vm.onConsole = [this](int level, const std::u16string& s) { client.consoleMessage(level, std::wstring(s.begin(), s.end())); };
    vm.rootTracers.push_back([this](Gc& g) {
        for (auto& w : wrappers) g.mark(w.second);
        for (auto& l : listeners)
            for (auto& e : l.second) g.mark(e.fn);
        for (auto& t : timers) {
            g.mark(t.fn);
            for (Value v : t.args) g.mark(v);
        }
        g.mark(currentScript);
        for (auto& e : session) g.mark(e.state);
        if (historyObject) g.mark(V(historyObject));
        if (allCollection) g.mark(V(allCollection));
        for (auto& ep : eventProtos) g.mark(V(ep.second));
        g.mark(domException);
        g.mark(cloneFn);
        g.mark(deliverFn);
        g.mark(pageScripts);
        g.mark(ceHook);
        g.mark(fragmentProto);
        g.mark(shadowProto);
        for (auto& tp : tagProtos) g.mark(tp.second);
        g.mark(observerProto);
        for (auto& n : netJs) g.mark(n.second);
        for (auto& ob : observers) {
            g.mark(ob->object);
            g.mark(ob->callback);
            for (Value v : ob->records) g.mark(v);
        }
        for (JSObject* p : {nodeProto, elementProto, documentProto, textProto, eventProto, classListProto, styleProto, computedProto, nodeListProto, storageProto}) g.mark(p);
    });
    JSObject* G = vm.global;
    auto getter = [&](JSObject* o, const char* name, NativeFn get, NativeFn set = nullptr, int data = 0, int data2 = 0) {
        JSNative* g = vm.newNative(get, name, 0);
        g->data = Value::i32(data);
        g->data2 = Value::i32(data2);
        Value s = Value::undef();
        if (set) {
            JSNative* sn = vm.newNative(set, name, 1);
            sn->data = Value::i32(data);
            sn->data2 = Value::i32(data2);
            s = V(sn);
        }
        vm.defineAccessor(o, PropKey::of(vm.atom(name)), V(g), s, PF_C | PF_E);
    };
    auto method = [&](JSObject* o, const char* name, NativeFn fn, int len, int data = 0) {
        JSNative* n = vm.defFn(o, name, fn, len);
        n->data = Value::i32(data);
        return n;
    };

    // ---- EventTarget + Node
    nodeProto = vm.newObject();
    for (JSObject* o : {nodeProto, G}) {
        method(o, "addEventListener", addEventListener, 2);
        method(o, "removeEventListener", removeEventListener, 2);
        method(o, "dispatchEvent", dispatchEventFn, 1);
    }
    getter(nodeProto, "nodeType", nodeType);
    getter(nodeProto, "nodeName", nodeName);
    getter(nodeProto, "nodeValue", nodeValueGet, nodeValueSet);
    getter(nodeProto, "textContent", textContentGet, ce<textContentSet>);
    getter(nodeProto, "parentNode", parentNode);
    getter(nodeProto, "parentElement", parentElement);
    getter(nodeProto, "childNodes", childNodes);
    getter(nodeProto, "children", children);
    getter(nodeProto, "childElementCount", childElementCount);
    const char* rel[] = {"firstChild", "lastChild", "firstElementChild", "lastElementChild", "nextSibling", "previousSibling", "nextElementSibling", "previousElementSibling"};
    for (int i = 0; i < 8; ++i) getter(nodeProto, rel[i], relative, nullptr, i);
    getter(nodeProto, "ownerDocument", ownerDocument);
    getter(nodeProto, "isConnected", nodeIsConnected);
    getter(nodeProto, "baseURI", docUrlGet);
    method(nodeProto, "appendChild", ce<appendChild>, 1);
    method(nodeProto, "insertBefore", ce<insertBefore>, 2);
    method(nodeProto, "removeChild", ce<removeChild>, 1);
    method(nodeProto, "replaceChild", ce<replaceChild>, 2);
    method(nodeProto, "cloneNode", ce<cloneNode>, 1);
    method(nodeProto, "contains", nodeContains, 1);
    method(nodeProto, "hasChildNodes", hasChildNodes, 0);
    method(nodeProto, "remove", ce<nodeRemove>, 0);
    method(nodeProto, "getRootNode", getRootNode, 0);
    method(nodeProto, "compareDocumentPosition", compareDocumentPosition, 1);
    const char* many[] = {"append", "prepend", "before", "after", "replaceWith", "replaceChildren"};
    for (int i = 0; i < 6; ++i) method(nodeProto, many[i], ce<insertMany>, 0, i);
    method(nodeProto, "querySelector", querySelector, 1);
    method(nodeProto, "querySelectorAll", querySelectorAll, 1);
    method(nodeProto, "getElementById", getElementById, 1);
    method(nodeProto, "getElementsByTagName", getElementsByTagName, 1);
    method(nodeProto, "getElementsByClassName", getElementsByClassName, 1);
    const char* consts[] = {"ELEMENT_NODE", "TEXT_NODE", "COMMENT_NODE", "DOCUMENT_NODE", "DOCUMENT_FRAGMENT_NODE"};
    int constVals[] = {1, 3, 8, 9, 11};
    for (int i = 0; i < 5; ++i) vm.def(nodeProto, consts[i], Value::i32(constVals[i]), 0);

    nodeListProto = vm.newObject(vm.arrayProto);
    method(nodeListProto, "item", nodeListItem, 1);
    method(nodeListProto, "namedItem", nodeListNamedItem, 1);

    // ---- Element
    elementProto = vm.newObject(nodeProto);
    getter(elementProto, "tagName", tagName);
    getter(elementProto, "localName", localName);
    method(elementProto, "getAttribute", getAttribute, 1);
    method(elementProto, "setAttribute", ce<setAttribute>, 2);
    method(elementProto, "removeAttribute", ce<removeAttribute>, 1);
    method(elementProto, "hasAttribute", hasAttribute, 1);
    method(elementProto, "toggleAttribute", ce<toggleAttribute>, 1);
    method(elementProto, "getAttributeNames", getAttributeNames, 0);
    getter(elementProto, "attributes", attributesGet);
    getter(elementProto, "innerHTML", innerHTMLGet, ce<innerHTMLSet>);
    getter(elementProto, "outerHTML", outerHTMLGet, ce<outerHTMLSet>);
    getter(elementProto, "innerText", textContentGet, ce<textContentSet>);
    method(elementProto, "insertAdjacentHTML", ce<insertAdjacent>, 2, 0);
    method(elementProto, "insertAdjacentElement", ce<insertAdjacent>, 2, 1);
    method(elementProto, "insertAdjacentText", ce<insertAdjacent>, 2, 2);
    method(elementProto, "matches", elementMatches, 1);
    method(elementProto, "webkitMatchesSelector", elementMatches, 1);
    method(elementProto, "closest", elementClosest, 1);
    method(elementProto, "getBoundingClientRect", getBoundingClientRect, 0);
    method(elementProto, "getClientRects", getClientRects, 0);
    struct {
        const char* name;
        int which, root;
    } metrics[] = {{"offsetWidth", 0, 0}, {"offsetHeight", 1, 0}, {"offsetLeft", 2, 0}, {"offsetTop", 3, 0}, {"clientWidth", 0, 0},
                   {"clientHeight", 1, 1}, {"scrollWidth", 0, 0}, {"scrollHeight", 5, 0}, {"clientLeft", 4, 0}, {"clientTop", 4, 0},
                   {"scrollLeft", 4, 0}};
    for (auto& m : metrics) getter(elementProto, m.name, metric, m.which == 4 ? noopFn : nullptr, m.which, m.root);
    getter(elementProto, "offsetParent", offsetParent);
    method(elementProto, "scrollIntoView", scrollIntoView, 0);
    method(elementProto, "focus", elementFocus, 0);
    method(elementProto, "blur", elementBlur, 0);
    method(elementProto, "click", elementClick, 0);
    getter(elementProto, "classList", classListGet);
    getter(elementProto, "style", styleGet, ce<styleSet>);
    getter(elementProto, "dataset", datasetGet);
    struct RP {
        const char* prop;
        const char* attr;
        int kind;  // 0 text, 1 boolean, 2 URL, 3 number
    };
    auto reflect = [&](JSObject* proto, std::initializer_list<RP> props) {
        for (auto& r : props) {
            JSNative* g = vm.newNative(reflectGet, r.prop, 0);
            JSNative* s = vm.newNative(ce<reflectSet>, r.prop, 1);
            g->data = s->data = V(vm.atom(r.attr));
            g->data2 = s->data2 = Value::i32(r.kind);
            vm.defineAccessor(proto, PropKey::of(vm.atom(r.prop)), V(g), V(s), PF_C | PF_E);
        }
    };
    reflect(elementProto, {{"id", "id", 0}, {"className", "class", 0}, {"title", "title", 0}, {"lang", "lang", 0}, {"dir", "dir", 0},
                           {"hidden", "hidden", 1}, {"tabIndex", "tabindex", 3}, {"draggable", "draggable", 1}, {"slot", "slot", 0},
                           {"role", "role", 0}, {"nonce", "nonce", 0}, {"accessKey", "accesskey", 0}});

    classListProto = vm.newObject();
    const char* clOps[] = {"add", "remove", "toggle", "contains", "replace"};
    for (int i = 0; i < 5; ++i) method(classListProto, clOps[i], ce<classListOp>, 1, i);
    method(classListProto, "item", classListItem, 1);
    method(classListProto, "toString", classListValue, 0);
    getter(classListProto, "length", classListLength);
    getter(classListProto, "value", classListValue);
    for (const char* n : {"forEach", "keys", "values", "entries"}) vm.def(classListProto, n, vm.get(V(vm.arrayProto), n));
    vm.defineOwn(classListProto, PropKey::of(vm.symIterator), vm.get(V(vm.arrayProto), "values"), PF_W | PF_C);

    styleProto = vm.newObject();
    method(styleProto, "setProperty", ce<styleSetProperty>, 2);
    method(styleProto, "getPropertyValue", styleGetPropertyValue, 1);
    method(styleProto, "removeProperty", ce<styleRemoveProperty>, 1);
    getter(styleProto, "cssText", styleCssTextGet, ce<styleCssTextSet>);
    computedProto = vm.newObject();
    method(computedProto, "getPropertyValue", computedGetPropertyValue, 1);

    // ---- Document and text nodes
    documentProto = vm.newObject(nodeProto);
    textProto = vm.newObject(nodeProto);
    getter(documentProto, "documentElement", docElementGet, nullptr, 0);
    getter(documentProto, "head", docElementGet, nullptr, 1);
    getter(documentProto, "body", docElementGet, nullptr, 2);
    JSObject* allProto = vm.newObject();
    makeCtor(vm, "HTMLAllCollection", illegalCtor, 0, allProto);
    allCollection = vm.newNative(allItem, "all", 1);
    allCollection->data = Value::i32(0);
    allCollection->htmlDda = true;
    allCollection->proto = allProto;
    for (const char* own : {"length", "name"}) vm.del(allCollection, PropKey::of(vm.atom(own)));  // a function's own, which would hide the collection's
    getter(allProto, "length", allLength);
    method(allProto, "item", allItem, 0, 0);
    method(allProto, "namedItem", allItem, 1, 1);
    getter(documentProto, "all", documentAll);
    getter(documentProto, "title", docTitleGet, docTitleSet);
    getter(documentProto, "URL", docUrlGet);
    getter(documentProto, "documentURI", docUrlGet);
    getter(documentProto, "readyState", docReadyState);
    getter(documentProto, "currentScript", docCurrentScript);
    getter(documentProto, "activeElement", docActiveElement);
    getter(documentProto, "domain", docDomain);
    getter(documentProto, "referrer", emptyStringFn);
    getter(documentProto, "cookie", docCookieGet, docCookieSet);
    const char* colls[] = {"forms", "images", "links", "scripts"};
    for (int i = 0; i < 4; ++i) getter(documentProto, colls[i], docCollection, nullptr, i);
    method(documentProto, "createElement", createElement, 1, 0);
    method(documentProto, "createElementNS", createElement, 2, 1);
    method(documentProto, "createTextNode", createTextNode, 1, 0);
    method(documentProto, "createComment", createTextNode, 1, 1);
    method(documentProto, "createDocumentFragment", createDocumentFragment, 0);
    method(documentProto, "createEvent", createEvent, 1);
    method(documentProto, "getElementsByName", getElementsByName, 1);
    method(documentProto, "write", ce<docWrite>, 1);
    method(documentProto, "writeln", docWrite, 1);
    method(documentProto, "hasFocus", trueFn, 0);
    vm.def(documentProto, "characterSet", vm.strv("UTF-8"), PF_ALL);
    vm.def(documentProto, "compatMode", vm.strv(doc.quirks ? "BackCompat" : "CSS1Compat"), PF_ALL);
    vm.def(documentProto, "visibilityState", vm.strv("visible"), PF_ALL);
    vm.def(documentProto, "hidden", Value::boolean(false), PF_ALL);
    vm.def(documentProto, "defaultView", V(G), PF_ALL);

    // ---- Event
    eventProto = vm.newObject();
    JSNative* eventC = makeCtor(vm, "Event", eventCtor, 1, eventProto);
    method(eventProto, "preventDefault", eventPreventDefault, 0);
    method(eventProto, "stopPropagation", eventStopPropagation, 0);
    method(eventProto, "stopImmediatePropagation", eventStopImmediate, 0);
    method(eventProto, "initEvent", eventInit, 3);
    method(eventProto, "initCustomEvent", eventInit, 4);
    method(eventProto, "composedPath", eventComposedPath, 0);
    getter(eventProto, "defaultPrevented", eventDefaultPrevented);
    // The other event interfaces, each after the one it inherits from. Apart from the two below, their attributes
    // are still plain properties of each event object, not getters on these prototypes.
    struct {
        const char* name;
        const char* parent;
    } eventKinds[] = {{"UIEvent", "Event"},          {"MouseEvent", "UIEvent"},        {"KeyboardEvent", "UIEvent"},   {"FocusEvent", "UIEvent"},
                      {"InputEvent", "UIEvent"},     {"CompositionEvent", "UIEvent"},  {"TouchEvent", "UIEvent"},      {"PointerEvent", "MouseEvent"},
                      {"WheelEvent", "MouseEvent"},  {"DragEvent", "MouseEvent"},      {"CustomEvent", "Event"},       {"ErrorEvent", "Event"},
                      {"MessageEvent", "Event"},     {"ProgressEvent", "Event"},       {"HashChangeEvent", "Event"},   {"PopStateEvent", "Event"},
                      {"StorageEvent", "Event"},     {"PageTransitionEvent", "Event"}, {"TransitionEvent", "Event"},   {"AnimationEvent", "Event"},
                      {"BeforeUnloadEvent", "Event"}, {"PromiseRejectionEvent", "Event"}, {"ClipboardEvent", "Event"}, {"SubmitEvent", "Event"},
                      {"ToggleEvent", "Event"}};
    std::unordered_map<std::string, JSNative*> eventCtors{{"Event", eventC}};
    eventProtos["Event"] = eventProto;
    for (auto& kind : eventKinds) {
        JSObject* proto = vm.newObject();
        proto->proto = eventProtos[kind.parent];
        JSNative* c = makeCtor(vm, kind.name, eventCtor, 1, proto);
        c->proto = eventCtors[kind.parent];
        eventProtos[kind.name] = proto;
        eventCtors[kind.name] = c;
    }
    getter(eventProtos["PopStateEvent"], "state", eventSlotGet, nullptr, 0);
    getter(eventProtos["HashChangeEvent"], "oldURL", eventSlotGet, nullptr, 0);
    getter(eventProtos["HashChangeEvent"], "newURL", eventSlotGet, nullptr, 1);

    // ---- one constructor and prototype per kind of node, so properties such as `value` or `async` exist only
    // on the elements that have them
    fragmentProto = vm.newObject(nodeProto);
    shadowProto = vm.newObject(fragmentProto);
    makeCtor(vm, "ShadowRoot", illegalCtor, 0, shadowProto);
    getter(shadowProto, "host", shadowHost);
    getter(shadowProto, "mode", shadowMode);
    getter(shadowProto, "innerHTML", innerHTMLGet, ce<innerHTMLSet>);
    method(elementProto, "attachShadow", attachShadow, 1);
    getter(elementProto, "shadowRoot", shadowRootGet);
    getter(elementProto, "assignedSlot", assignedSlotGet);
    getter(textProto, "assignedSlot", assignedSlotGet);
    vm.defFn(G, "__shadowOf", shadowRootGet, 0)->data = Value::i32(1);
    vm.defFn(G, "__adopt", adoptFn, 2);
    struct {
        const char* name;
        JSObject* proto;
    } ctors[] = {{"Node", nodeProto}, {"Element", elementProto}, {"HTMLElement", elementProto}, {"Document", documentProto},
                 {"HTMLDocument", documentProto}, {"Text", textProto}, {"Comment", textProto}, {"CharacterData", textProto},
                 {"DocumentFragment", fragmentProto}, {"NodeList", nodeListProto}, {"HTMLCollection", nodeListProto}};
    for (auto& c : ctors) {
        JSNative* made = makeCtor(vm, c.name, std::string(c.name) == "HTMLElement" ? htmlElementCtor : illegalCtor, 0, c.proto);
        if (std::string(c.name) != "Node") continue;
        for (int i = 0; i < 5; ++i) vm.def(made, consts[i], Value::i32(constVals[i]), 0);
        const char* positions[] = {"DOCUMENT_POSITION_DISCONNECTED", "DOCUMENT_POSITION_PRECEDING", "DOCUMENT_POSITION_FOLLOWING", "DOCUMENT_POSITION_CONTAINS", "DOCUMENT_POSITION_CONTAINED_BY"};
        int bits[] = {1, 2, 4, 8, 16};
        for (int i = 0; i < 5; ++i) {
            vm.def(made, positions[i], Value::i32(bits[i]), 0);
            vm.def(nodeProto, positions[i], Value::i32(bits[i]), 0);
        }
    }
    getter(textProto, "data", nodeValueGet, nodeValueSet);
    auto iface = [&](const char* name, JSObject* parent, const std::vector<const char*>& tags, std::initializer_list<RP> props) {
        JSObject* proto = vm.newObject(parent);
        makeCtor(vm, name, illegalCtor, 0, proto);
        for (const char* t : tags) tagProtos[t] = proto;
        reflect(proto, props);
        return proto;
    };
    auto control = [&](JSObject* proto, bool checkable) {
        getter(proto, "value", valueGet, ce<valueSet>);
        getter(proto, "form", formGet);
        if (checkable) getter(proto, "checked", checkedGet, ce<checkedSet>);
    };
    iface("HTMLAnchorElement", elementProto, {"a"}, {{"href", "href", 2}, {"target", "target", 0}, {"rel", "rel", 0}, {"download", "download", 0}, {"hreflang", "hreflang", 0}, {"type", "type", 0}, {"referrerPolicy", "referrerpolicy", 0}, {"name", "name", 0}});
    iface("HTMLAreaElement", elementProto, {"area"}, {{"href", "href", 2}, {"target", "target", 0}, {"alt", "alt", 0}});
    iface("HTMLImageElement", elementProto, {"img"}, {{"src", "src", 2}, {"alt", "alt", 0}, {"width", "width", 3}, {"height", "height", 3}, {"srcset", "srcset", 0}, {"sizes", "sizes", 0}, {"loading", "loading", 0}, {"decoding", "decoding", 0}, {"crossOrigin", "crossorigin", 0}, {"referrerPolicy", "referrerpolicy", 0}, {"useMap", "usemap", 0}, {"name", "name", 0}});
    iface("HTMLScriptElement", elementProto, {"script"}, {{"src", "src", 2}, {"type", "type", 0}, {"async", "async", 1}, {"defer", "defer", 1}, {"charset", "charset", 0}, {"crossOrigin", "crossorigin", 0}, {"integrity", "integrity", 0}, {"noModule", "nomodule", 1}, {"referrerPolicy", "referrerpolicy", 0}, {"htmlFor", "for", 0}, {"event", "event", 0}});
    iface("HTMLLinkElement", elementProto, {"link"}, {{"href", "href", 2}, {"rel", "rel", 0}, {"media", "media", 0}, {"type", "type", 0}, {"as", "as", 0}, {"crossOrigin", "crossorigin", 0}, {"integrity", "integrity", 0}, {"sizes", "sizes", 0}, {"disabled", "disabled", 1}, {"hreflang", "hreflang", 0}, {"referrerPolicy", "referrerpolicy", 0}});
    iface("HTMLMetaElement", elementProto, {"meta"}, {{"name", "name", 0}, {"content", "content", 0}, {"httpEquiv", "http-equiv", 0}, {"charset", "charset", 0}});
    iface("HTMLStyleElement", elementProto, {"style"}, {{"media", "media", 0}, {"type", "type", 0}, {"disabled", "disabled", 1}});
    iface("HTMLBaseElement", elementProto, {"base"}, {{"href", "href", 2}, {"target", "target", 0}});
    control(iface("HTMLInputElement", elementProto, {"input"}, {{"type", "type", 0}, {"name", "name", 0}, {"placeholder", "placeholder", 0}, {"disabled", "disabled", 1}, {"required", "required", 1}, {"readOnly", "readonly", 1}, {"maxLength", "maxlength", 3}, {"minLength", "minlength", 3}, {"size", "size", 3}, {"multiple", "multiple", 1}, {"autofocus", "autofocus", 1}, {"min", "min", 0}, {"max", "max", 0}, {"step", "step", 0}, {"pattern", "pattern", 0}, {"accept", "accept", 0}, {"autocomplete", "autocomplete", 0}, {"alt", "alt", 0}, {"src", "src", 2}, {"defaultValue", "value", 0}, {"defaultChecked", "checked", 1}, {"inputMode", "inputmode", 0}}), true);
    control(iface("HTMLTextAreaElement", elementProto, {"textarea"}, {{"name", "name", 0}, {"placeholder", "placeholder", 0}, {"disabled", "disabled", 1}, {"required", "required", 1}, {"readOnly", "readonly", 1}, {"rows", "rows", 3}, {"cols", "cols", 3}, {"maxLength", "maxlength", 3}, {"wrap", "wrap", 0}, {"autofocus", "autofocus", 1}, {"autocomplete", "autocomplete", 0}}), false);
    control(iface("HTMLSelectElement", elementProto, {"select"}, {{"name", "name", 0}, {"disabled", "disabled", 1}, {"required", "required", 1}, {"multiple", "multiple", 1}, {"size", "size", 3}, {"autofocus", "autofocus", 1}, {"autocomplete", "autocomplete", 0}}), false);
    control(iface("HTMLOptionElement", elementProto, {"option"}, {{"selected", "selected", 1}, {"defaultSelected", "selected", 1}, {"disabled", "disabled", 1}, {"label", "label", 0}}), false);
    control(iface("HTMLButtonElement", elementProto, {"button"}, {{"type", "type", 0}, {"name", "name", 0}, {"disabled", "disabled", 1}, {"autofocus", "autofocus", 1}, {"formAction", "formaction", 2}, {"formMethod", "formmethod", 0}}), false);
    JSObject* formProto = iface("HTMLFormElement", elementProto, {"form"}, {{"action", "action", 2}, {"method", "method", 0}, {"enctype", "enctype", 0}, {"encoding", "enctype", 0}, {"target", "target", 0}, {"name", "name", 0}, {"noValidate", "novalidate", 1}, {"acceptCharset", "accept-charset", 0}, {"autocomplete", "autocomplete", 0}});
    method(formProto, "submit", formSubmit, 0, 0);
    method(formProto, "requestSubmit", formSubmit, 0, 1);
    iface("HTMLLabelElement", elementProto, {"label"}, {{"htmlFor", "for", 0}});
    iface("HTMLFieldSetElement", elementProto, {"fieldset"}, {{"name", "name", 0}, {"disabled", "disabled", 1}});
    iface("HTMLIFrameElement", elementProto, {"iframe", "frame"}, {{"src", "src", 2}, {"name", "name", 0}, {"width", "width", 0}, {"height", "height", 0}, {"allow", "allow", 0}, {"sandbox", "sandbox", 0}, {"srcdoc", "srcdoc", 0}, {"loading", "loading", 0}, {"referrerPolicy", "referrerpolicy", 0}, {"allowFullscreen", "allowfullscreen", 1}, {"frameBorder", "frameborder", 0}, {"scrolling", "scrolling", 0}});
    iface("HTMLCanvasElement", elementProto, {"canvas"}, {{"width", "width", 3}, {"height", "height", 3}});
    JSObject* media = iface("HTMLMediaElement", elementProto, {}, {{"src", "src", 2}, {"controls", "controls", 1}, {"autoplay", "autoplay", 1}, {"loop", "loop", 1}, {"muted", "muted", 1}, {"defaultMuted", "muted", 1}, {"preload", "preload", 0}, {"crossOrigin", "crossorigin", 0}});
    iface("HTMLVideoElement", media, {"video"}, {{"poster", "poster", 2}, {"width", "width", 3}, {"height", "height", 3}, {"playsInline", "playsinline", 1}});
    iface("HTMLAudioElement", media, {"audio"}, {});
    iface("HTMLSourceElement", elementProto, {"source"}, {{"src", "src", 2}, {"type", "type", 0}, {"srcset", "srcset", 0}, {"sizes", "sizes", 0}, {"media", "media", 0}});
    iface("HTMLTrackElement", elementProto, {"track"}, {{"src", "src", 2}, {"kind", "kind", 0}, {"srclang", "srclang", 0}, {"label", "label", 0}, {"default", "default", 1}});
    iface("HTMLEmbedElement", elementProto, {"embed"}, {{"src", "src", 2}, {"type", "type", 0}, {"width", "width", 0}, {"height", "height", 0}});
    iface("HTMLObjectElement", elementProto, {"object"}, {{"data", "data", 2}, {"type", "type", 0}, {"name", "name", 0}, {"width", "width", 0}, {"height", "height", 0}});
    iface("HTMLTableCellElement", elementProto, {"td", "th"}, {{"colSpan", "colspan", 3}, {"rowSpan", "rowspan", 3}, {"headers", "headers", 0}, {"scope", "scope", 0}});
    iface("HTMLDetailsElement", elementProto, {"details"}, {{"open", "open", 1}});
    iface("HTMLDialogElement", elementProto, {"dialog"}, {{"open", "open", 1}});
    iface("HTMLOListElement", elementProto, {"ol"}, {{"start", "start", 3}, {"reversed", "reversed", 1}, {"type", "type", 0}});
    iface("HTMLLIElement", elementProto, {"li"}, {{"value", "value", 3}});
    iface("HTMLProgressElement", elementProto, {"progress", "meter"}, {{"value", "value", 3}, {"max", "max", 3}});
    iface("HTMLSlotElement", elementProto, {"slot"}, {{"name", "name", 0}});
    iface("HTMLTimeElement", elementProto, {"time"}, {{"dateTime", "datetime", 0}});
    iface("HTMLDataElement", elementProto, {"data"}, {{"value", "value", 0}});
    iface("HTMLQuoteElement", elementProto, {"blockquote", "q"}, {{"cite", "cite", 2}});
    struct {
        const char* name;
        std::vector<const char*> tags;
    } plain[] = {
        {"HTMLDivElement", {"div"}}, {"HTMLSpanElement", {"span"}}, {"HTMLParagraphElement", {"p"}},
        {"HTMLHeadingElement", {"h1", "h2", "h3", "h4", "h5", "h6"}}, {"HTMLUListElement", {"ul"}}, {"HTMLBodyElement", {"body"}},
        {"HTMLHeadElement", {"head"}}, {"HTMLHtmlElement", {"html"}}, {"HTMLBRElement", {"br"}}, {"HTMLHRElement", {"hr"}},
        {"HTMLPreElement", {"pre"}}, {"HTMLTitleElement", {"title"}}, {"HTMLTemplateElement", {"template"}},
        {"HTMLTableElement", {"table"}}, {"HTMLTableRowElement", {"tr"}}, {"HTMLTableSectionElement", {"tbody", "thead", "tfoot"}},
        {"HTMLPictureElement", {"picture"}}, {"HTMLDataListElement", {"datalist"}}, {"HTMLLegendElement", {"legend"}},
        {"HTMLOptGroupElement", {"optgroup"}}, {"HTMLMenuElement", {"menu"}}, {"HTMLUnknownElement", {}},
    };
    for (auto& pl : plain) iface(pl.name, elementProto, pl.tags, {});
    getter(tagProtos["template"], "content", templateContent);
    getter(tagProtos["img"], "naturalWidth", imageInfo, nullptr, 0);
    getter(tagProtos["img"], "naturalHeight", imageInfo, nullptr, 1);
    getter(tagProtos["img"], "complete", imageInfo, nullptr, 2);
    method(tagProtos["slot"], "assignedNodes", slotAssigned, 0, 0);
    method(tagProtos["slot"], "assignedElements", slotAssigned, 0, 1);
    JSObject* svg = iface("SVGElement", elementProto, {"svg", "g", "path", "circle", "rect", "line", "polyline", "polygon", "ellipse", "text", "tspan", "use", "defs", "symbol", "clippath", "lineargradient", "radialgradient", "stop", "mask", "pattern", "image", "foreignobject"}, {});
    for (const char* n : {"SVGSVGElement", "SVGGraphicsElement", "SVGPathElement"}) {
        JSNative* c = vm.newNative(illegalCtor, n, 0);
        c->oflags |= OF_CTOR;
        c->add(PropKey::of(vm.a.prototype), V(svg), 0);
        vm.def(G, n, V(c));
    }
    makeCtor(vm, "DOMTokenList", illegalCtor, 0, classListProto);
    makeCtor(vm, "CSSStyleDeclaration", illegalCtor, 0, styleProto);
    vm.def(elementProto, "constructor", vm.get(V(G), "HTMLElement"));
    observerProto = vm.newObject();
    makeCtor(vm, "MutationObserver", mutationObserverCtor, 1, observerProto);
    vm.def(G, "WebKitMutationObserver", vm.get(V(G), "MutationObserver"));
    vm.defFn(observerProto, "observe", mutationObserve, 2);
    vm.defFn(observerProto, "disconnect", mutationDisconnect, 0);
    vm.defFn(observerProto, "takeRecords", mutationTakeRecords, 0);
    deliverFn = V(vm.newNative(deliverMutations, "deliverMutations", 0));
    for (const char* n : {"PerformanceObserver"}) {  // exists so that feature checks pass; it never reports anything
        JSObject* p = vm.newObject();
        for (const char* m : {"observe", "unobserve", "disconnect"}) vm.defFn(p, m, noopFn, 1);
        vm.defFn(p, "takeRecords", emptyArrayFn, 0);
        makeCtor(vm, n, observerCtor, 1, p)->data2 = V(p);
    }

    // ---- window
    for (const char* n : {"window", "self", "top", "parent", "frames"}) vm.def(G, n, V(G), PF_W | PF_C | PF_E);
    vm.def(G, "document", wrap(docNode()), PF_C | PF_E);
    method(G, "setTimeout", setTimeoutFn, 2);
    method(G, "setInterval", setIntervalFn, 2);
    method(G, "requestAnimationFrame", requestAnimationFrameFn, 1);
    method(G, "requestIdleCallback", setTimeoutFn, 1);
    for (const char* n : {"clearTimeout", "clearInterval", "cancelAnimationFrame", "cancelIdleCallback"}) method(G, n, clearTimerFn, 1);
    method(G, "alert", alertFn, 1);
    method(G, "confirm", confirmFn, 1);
    method(G, "prompt", promptFn, 1);
    method(G, "getComputedStyle", getComputedStyle, 1);
    method(G, "matchMedia", matchMediaFn, 1);
    method(G, "scrollTo", scrollToFn, 2, 0);
    method(G, "scroll", scrollToFn, 2, 0);
    method(G, "scrollBy", scrollToFn, 2, 1);
    method(G, "btoa", btoaFn, 1);
    method(G, "atob", atobFn, 1);
    for (const char* n : {"focus", "blur", "close", "stop", "print", "postMessage"}) method(G, n, noopFn, 0);
    getter(G, "innerWidth", viewportGet, nullptr, 0);
    getter(G, "innerHeight", viewportGet, nullptr, 1);
    getter(G, "outerWidth", viewportGet, nullptr, 0);
    getter(G, "outerHeight", viewportGet, nullptr, 1);
    getter(G, "scrollY", viewportGet, nullptr, 2);
    getter(G, "pageYOffset", viewportGet, nullptr, 2);
    getter(G, "scrollX", viewportGet, nullptr, 3);
    getter(G, "pageXOffset", viewportGet, nullptr, 3);
    vm.def(G, "devicePixelRatio", Value::i32(1), PF_ALL);
    vm.def(G, "name", V(vm.a.empty), PF_ALL);
    vm.def(G, "isSecureContext", Value::boolean(url.rfind("https:", 0) == 0), PF_ALL);

    JSObject* perf = vm.newObject();
    vm.def(G, "performance", V(perf), PF_ALL);
    method(perf, "now", performanceNow, 0);
    vm.def(perf, "timeOrigin", Value::num(startMs), PF_ALL);
    for (const char* n : {"mark", "measure", "clearMarks", "clearMeasures"}) method(perf, n, noopFn, 1);
    for (const char* n : {"getEntries", "getEntriesByName", "getEntriesByType"}) method(perf, n, emptyArrayFn, 1);

    float vw = 1280, vh = 720, sy = 0;
    client.viewport(vw, vh, sy);
    JSObject* screen = vm.newObject();
    vm.def(G, "screen", V(screen), PF_ALL);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    vm.def(screen, "width", Value::i32(sw), PF_ALL);
    vm.def(screen, "height", Value::i32(sh), PF_ALL);
    vm.def(screen, "availWidth", Value::i32(sw), PF_ALL);
    vm.def(screen, "availHeight", Value::i32(sh), PF_ALL);
    vm.def(screen, "colorDepth", Value::i32(24), PF_ALL);
    vm.def(screen, "pixelDepth", Value::i32(24), PF_ALL);

    JSObject* nav = vm.newObject();
    vm.def(G, "navigator", V(nav), PF_ALL);
    vm.def(nav, "userAgent", vm.strv("Mozilla/5.0 (Windows NT 10.0; Win64; x64) TinyBrowser/0.3"), PF_ALL);
    vm.def(nav, "appName", vm.strv("Netscape"), PF_ALL);
    vm.def(nav, "appVersion", vm.strv("5.0 (Windows)"), PF_ALL);
    vm.def(nav, "platform", vm.strv("Win32"), PF_ALL);
    vm.def(nav, "vendor", V(vm.a.empty), PF_ALL);
    vm.def(nav, "product", vm.strv("Gecko"), PF_ALL);
    vm.def(nav, "language", vm.strv("en-US"), PF_ALL);
    JSArray* langs = vm.newArray();
    langs->elems = {vm.strv("en-US"), vm.strv("en")};
    vm.def(nav, "languages", V(langs), PF_ALL);
    vm.def(nav, "cookieEnabled", Value::boolean(true), PF_ALL);
    vm.def(nav, "onLine", Value::boolean(true), PF_ALL);
    vm.def(nav, "hardwareConcurrency", Value::i32(4), PF_ALL);
    vm.def(nav, "maxTouchPoints", Value::i32(0), PF_ALL);
    vm.def(nav, "doNotTrack", vm.strv("1"), PF_ALL);
    method(nav, "sendBeacon", trueFn, 2);
    method(nav, "javaEnabled", confirmFn, 0)->fn = [](CallInfo&) { return Value::boolean(false); };

    // location: one object for window.location and document.location; assigning to either navigates
    JSObject* loc = vm.newObject();
    const char* parts[] = {"href", "protocol", "host", "hostname", "port", "pathname", "search", "hash", "origin"};
    for (int i = 0; i < 9; ++i) getter(loc, parts[i], locationGet, i == 0 || i >= 5 && i <= 7 ? locationSet : nullptr, i);
    method(loc, "assign", locationSet, 1, 0);
    method(loc, "replace", locationSet, 1, 0)->data2 = Value::i32(1);
    method(loc, "reload", locationReload, 0);
    method(loc, "toString", locationToString, 0);
    for (JSObject* o : {G, documentProto}) {
        JSNative* g = vm.newNative([](CallInfo& ci) { return ci.callee->data; }, "location", 0);
        g->data = V(loc);
        JSNative* s = vm.newNative(locationSet, "location", 1);
        s->data = Value::i32(0);
        s->data2 = Value::i32(0);
        vm.defineAccessor(o, PropKey::of(vm.atom("location")), V(g), V(s), PF_C | PF_E);
    }

    JSObject* historyProto = vm.newObject();
    makeCtor(vm, "History", illegalCtor, 0, historyProto);
    historyObject = vm.newObject();
    historyObject->proto = historyProto;
    vm.def(G, "history", V(historyObject), PF_ALL);
    session.push_back({url, Value::null()});
    getter(historyProto, "length", historyGet, nullptr, 0);
    getter(historyProto, "state", historyGet, nullptr, 1);
    getter(historyProto, "scrollRestoration", historyGet, historySetScrollRestoration, 2);
    method(historyProto, "pushState", historyPushState, 2, 0);
    method(historyProto, "replaceState", historyPushState, 2, 1);
    method(historyProto, "go", historyGo, 0, 0);
    method(historyProto, "back", historyGo, 0, -1);
    method(historyProto, "forward", historyGo, 0, 1);

    storageProto = vm.newObject();
    makeCtor(vm, "Storage", illegalCtor, 0, storageProto);
    const char* stOps[] = {"getItem", "setItem", "removeItem", "clear", "key"};
    for (int i = 0; i < 5; ++i) method(storageProto, stOps[i], storageOp, 1, i);
    getter(storageProto, "length", storageOp, nullptr, 5);
    struct {
        const char* name;
        Store* store;
    } stores[] = {{"localStorage", &localStore}, {"sessionStorage", &sessionStore}};
    for (auto& s : stores) {
        JSHost* h = vm.alloc<JSHost>();
        h->hostKind = HK_Storage;
        h->ptr = s.store;
        h->proto = storageProto;
        h->hooks = &kStorageHooks;
        vm.def(G, s.name, V(h), PF_ALL);
    }

    // What only nodes with children have (DOM 4.2.6 ParentNode, and the lookups of documents and elements) was
    // registered on Node.prototype above. Text and comment nodes must not have it: pages tell the kinds of node
    // apart by asking for querySelectorAll.
    struct {
        const char* name;
        bool onElement, onDocument, onFragment;
    } parentOnly[] = {{"querySelector", true, true, true},       {"querySelectorAll", true, true, true},     {"children", true, true, true},
                      {"childElementCount", true, true, true},   {"firstElementChild", true, true, true},    {"lastElementChild", true, true, true},
                      {"append", true, true, true},              {"prepend", true, true, true},              {"replaceChildren", true, true, true},
                      {"getElementById", false, true, true},     {"getElementsByTagName", true, true, false}, {"getElementsByClassName", true, true, false}};
    for (auto& m : parentOnly) {
        PropKey key = PropKey::of(vm.atom(m.name));
        PropDesc d;
        if (!vm.getOwnDesc(nodeProto, key, d)) continue;
        if (m.onElement) vm.defineOwnDesc(elementProto, key, d);
        if (m.onDocument) vm.defineOwnDesc(documentProto, key, d);
        if (m.onFragment) vm.defineOwnDesc(fragmentProto, key, d);
        vm.del(nodeProto, key);
    }
    vm.def(vm.get(V(G), "PerformanceObserver").asObject(), "supportedEntryTypes", V(vm.newArray()), PF_E);

    net->wake = client.threadWake();
    vm.defFn(G, "__net", netStart, 6);
    vm.defFn(G, "__netSync", netSyncFn, 5);
    vm.defFn(G, "__netAbort", netAbortFn, 1);
    vm.defFn(G, "__url", urlParse, 2);
    vm.defFn(G, "__reportError", reportErrorFn, 1);
    vm.defFn(G, "__ceHook", ceHookSet, 1);
    vm.defFn(G, "__ceState", ceStateFn, 2);
    vm.defFn(G, "__isConstructor", isConstructorFn, 1);
    vm.defFn(G, "__scrollTop", scrollTopFn, 2);
    vm.defFn(G, "__domException", domExceptionSet, 1);
    try {
        vm.eval(utf8to16(kScriptPrelude), "browser:prelude");
        cloneFn = vm.get(V(G), "structuredClone");
    } catch (JSThrow&) {
        reportError();
    }
    pageScripts = vm.get(V(G), "__pageScripts");
    vm.del(G, PropKey::of(vm.atom("__pageScripts")));
}

// ---------------------------------------------------------------------------- public interface
ScriptHost::ScriptHost(Document& doc, const std::string& url, Client& client) : impl_(std::make_unique<Impl>(doc, url, client)) { impl_->setup(); }
ScriptHost::~ScriptHost() {
    std::lock_guard<std::mutex> lk(impl_->net->mu);
    impl_->net->alive = false;  // results still on their way are dropped
}

bool ScriptHost::busy() const {
    if (impl_->dead) return false;
    if (impl_->loadPending || impl_->loadDue) return true;
    if (impl_->net->inflight.load() > 0) return true;
    std::lock_guard<std::mutex> lk(impl_->net->mu);
    return !impl_->net->done.empty();
}

bool ScriptHost::stopped() const { return impl_->dead; }
std::vector<std::pair<const Node*, std::string>> ScriptHost::adoptedStyles() const {
    std::vector<std::pair<const Node*, std::string>> out;
    for (auto& [root, list] : impl_->adopted)
        for (auto& css : list) out.push_back({root->isShadowRoot() ? root : nullptr, css});
    return out;
}

void ScriptHost::runScripts(const std::vector<ScriptSource>& scripts) {
    Impl& im = *impl_;
    for (auto& s : scripts)
        if (s.node) im.startedScripts.insert(s.node);
    // First the scripts that run while the document is being parsed, in document order.
    for (auto& s : scripts) {
        if (im.dead) break;
        if (s.deferred) {
            im.deferred.push_back(s);
            continue;
        }
        Entry entry(im);
        if (s.kind == ScriptSource::ImportMap) {
            im.registerImportMap(s.code);
            continue;
        }
        im.currentScript = im.wrap(s.node);
        try {
            im.vm.eval(utf8to16(s.code), s.url);
        } catch (JSThrow&) {
            im.reportError();
        }
        im.currentScript = Value::null();
    }
    im.readyState = "interactive";  // the document is parsed; deferred scripts see this state
    dispatch(im.docNode(), "readystatechange", false, false);
    if (im.deferred.empty() || im.dead || !im.pageScripts.isObject()) {
        fireLoadEvents();
        return;
    }
    // Then defer scripts and modules. Modules load in the background, so the prelude runs this list and calls
    // back when it is through; DOMContentLoaded and load are fired after that.
    im.loadPending = true;
    Entry entry(im);
    Vm& vm = im.vm;
    JSArray* items = vm.newArray();
    for (size_t i = 0; i < im.deferred.size(); ++i) {
        const ScriptSource& s = im.deferred[i];
        bool module = s.kind == ScriptSource::Module;
        JSArray* it = vm.newArray();
        items->elems.push_back(V(it));
        it->elems.push_back(Value::boolean(module));
        it->elems.push_back(im.wrap(s.node));
        it->elems.push_back(module && s.external ? val8(vm, s.url) : Value::null());
        it->elems.push_back(module && !s.external ? val8(vm, s.code) : Value::null());
        JSNative* run = nullptr;
        if (!module) {
            run = vm.newNative(deferredRun, "", 0);
            run->data = Value::i32((int32_t)i);
        }
        it->elems.push_back(run ? V(run) : Value::null());
        it->elems.push_back(Value::boolean(s.async));
    }
    Value args[2] = {V(items), V(vm.newNative(deferredDone, "", 0))};
    im.callPage("deferred", args, 2);
}

void ScriptHost::fireLoadEvents() {
    Impl& im = *impl_;
    im.loadPending = im.loadDue = false;
    dispatch(im.docNode(), "DOMContentLoaded", true, false);
    im.readyState = "complete";
    dispatch(im.docNode(), "readystatechange", false, false);
    dispatch(nullptr, "load", false, false);
}

bool ScriptHost::dispatch(Node* target, const std::string& type, bool bubbles, bool cancelable) {
    Impl& im = *impl_;
    if (im.dead) return true;
    Entry entry(im);
    try {
        return im.fire(target, im.makeEvent(type, bubbles, cancelable));
    } catch (JSThrow&) {
        im.reportError();
        return true;
    }
}

void ScriptHost::traversed(size_t index) {
    Impl& im = *impl_;
    if (im.dead || index >= im.session.size() || index == im.sessionAt) return;
    Entry entry(im);
    std::string old = im.url;
    im.sessionAt = index;
    try {
        im.sessionEntryChanged(old);
    } catch (JSThrow&) {
        im.reportError();
    }
}

void ScriptHost::fragmentNavigated(const std::string& url, bool push) {
    Impl& im = *impl_;
    if (im.dead) return;
    Entry entry(im);
    std::string old = im.url;
    if (push) {
        im.session.resize(im.sessionAt + 1);
        im.session.push_back({url, Value::null()});  // the classic history API state is never carried over
        ++im.sessionAt;
    } else {
        im.session[im.sessionAt] = {url, Value::null()};
    }
    try {
        im.sessionEntryChanged(old);
    } catch (JSThrow&) {
        im.reportError();
    }
}

bool ScriptHost::restoresScroll() const { return !impl_->manualScrollRestoration; }

void ScriptHost::pump() {
    Impl& im = *impl_;
    if (im.dead || im.depth) return;
    drainNet(im);
    if (im.loadDue) fireLoadEvents();
    while (!im.hashChanges.empty() && !im.dead) {
        auto [oldUrl, newUrl] = im.hashChanges.front();
        im.hashChanges.erase(im.hashChanges.begin());
        Entry entry(im);
        try {
            Value ev = im.makeEvent("hashchange", false, false);
            ((JSHost*)ev.asObject())->slotA = val8(im.vm, oldUrl);
            ((JSHost*)ev.asObject())->slotB = val8(im.vm, newUrl);
            im.fire(nullptr, ev);
        } catch (JSThrow&) {
            im.reportError();
        }
    }
    for (int budget = 200; budget > 0 && !im.dead; --budget) {
        double now = now_ms();
        int best = -1;
        for (int i = 0; i < (int)im.timers.size(); ++i)
            if (im.timers[i].due <= now && (best < 0 || im.timers[i].due < im.timers[best].due)) best = i;
        if (best < 0) break;
        Timer t = im.timers[best];
        if (t.interval < 0) im.timers.erase(im.timers.begin() + best);
        else im.timers[best].due = now + t.interval;
        t.args.push_back(t.fn);
        RootScope keep(im.vm, t.args);
        Entry entry(im);
        try {
            if (t.frame) {
                Value stamp = Value::dbl(now - im.startMs);
                im.vm.call(t.fn, Value::undef(), &stamp, 1);
            } else
                im.vm.call(t.fn, Value::undef(), t.args.data(), (int)t.args.size() - 1);
        } catch (JSThrow&) {
            im.reportError();
        }
    }
    if (!im.timers.empty() && !im.dead) {
        double next = im.timers[0].due;
        for (auto& t : im.timers) next = std::min(next, t.due);
        im.client.wake(std::max(0.0, next - now_ms()));
    }
}

bool ScriptHost::eval(const std::wstring& source, std::wstring& out) {
    Impl& im = *impl_;
    Entry entry(im);
    im.vm.terminated = false;
    std::u16string result, error;
    bool ok = im.vm.run(std::u16string(source.begin(), source.end()), "console", &result, &error);
    const std::u16string& shown = ok ? result : error;
    out.assign(shown.begin(), shown.end());
    return ok;
}

}  // namespace tb
