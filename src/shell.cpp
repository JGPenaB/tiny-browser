// Browser shell: window, tabs, address bar, bookmarks bar, forms, password prompts, devtools, input, frame drawing.
#include <windows.h>
#include <windowsx.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <psapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <optional>
#include <thread>

#include "css.h"
#include "gpu.h"
#include "layout.h"
#include "net.h"
#include "storage.h"
#include "text.h"
#include "url.h"
#include "js/js.h"
#include "script.h"

using Microsoft::WRL::ComPtr;

namespace tb {

// ---------------------------------------------------------------------------- UA stylesheet
static const char* kUACss = R"CSS(
html,address,blockquote,body,dd,div,dl,dt,fieldset,form,frame,frameset,h1,h2,h3,h4,h5,h6,noframes,ol,p,ul,center,dir,hr,menu,pre,
article,aside,footer,header,hgroup,main,nav,section,figure,figcaption,details,summary,legend,search,dialog,optgroup,address,listing,xmp,plaintext{display:block}
head,script,style,title,meta,link,template,base,datalist,param,noembed,area,map,source,track,option,svg,math,col,colgroup{display:none}
dialog:not([open]),audio:not([controls]),input[type=hidden]{display:none}
li{display:list-item}
table{display:table}
thead,tbody,tfoot{display:table-row-group}
tr{display:table-row}
td,th{display:table-cell;vertical-align:middle;padding:1px}
th{font-weight:bold;text-align:center}
caption{display:block;text-align:center}
body{margin:8px}
h1{font-size:2em;margin:.67em 0;font-weight:bold}
h2{font-size:1.5em;margin:.83em 0;font-weight:bold}
h3{font-size:1.17em;margin:1em 0;font-weight:bold}
h4{margin:1.33em 0;font-weight:bold}
h5{font-size:.83em;margin:1.67em 0;font-weight:bold}
h6{font-size:.67em;margin:2.33em 0;font-weight:bold}
p,dl,pre,menu,ol,ul{margin:1em 0}
blockquote,figure{margin:1em 40px}
ul,ol,menu,dir{padding-left:40px}
li ul,li ol,ul ul,ol ul,ul ol,ol ol{margin:0}
ol{list-style-type:decimal}
ul ul,ol ul{list-style-type:circle}
ul ul ul,ol ul ul,ul ol ul{list-style-type:square}
dd{margin-left:40px}
b,strong{font-weight:bold}
i,em,cite,var,dfn,address{font-style:italic}
pre,code,kbd,samp,tt,listing,xmp,plaintext{font-family:monospace}
code,kbd,samp,tt{font-size:.875em}
pre{font-size:.8125em;white-space:pre}
pre code,pre kbd,pre samp{font-size:1em}
listing,xmp,plaintext{white-space:pre}
a:link{color:#0000ee;text-decoration:underline}
u,ins{text-decoration:underline}
s,strike,del{text-decoration:line-through}
hr{margin:.5em auto;border-style:inset;border-width:1px;color:#9a9a9a}
center{text-align:-webkit-center}
small{font-size:smaller}
big{font-size:larger}
sub{vertical-align:sub;font-size:smaller}
sup{vertical-align:super;font-size:smaller}
mark{background-color:yellow;color:black}
nobr{white-space:nowrap}
input,button,select,textarea{display:inline-block;font-family:Arial;font-size:13.33px;line-height:normal;color:#000;
  font-weight:400;font-style:normal;text-transform:none;text-decoration:none;white-space:normal;text-align:left}
input,select,textarea{border:1px solid #767676;padding:1px 4px;background-color:#fff}
textarea{white-space:pre-wrap;padding:2px}
select{padding:1px 2px}
button,input[type=submit],input[type=button],input[type=reset],input[type=file],input[type=image]{border:1px solid #767676;padding:1px 6px;background-color:#efefef;text-align:center}
input[type=checkbox],input[type=radio]{border-style:none;padding:0;background-color:transparent}
fieldset{margin:0 2px;padding:.35em .75em .625em;border:2px groove #c0c0c0}
legend{padding:0 2px}
iframe{border:2px inset #ccc}
)CSS";

static const Sheet& quirks_sheet() {
    static Sheet s = parse_sheet("table{text-align:left;white-space:normal;line-height:normal;font-weight:normal;font-style:normal;font-size:medium}");
    return s;
}

static const Sheet& ua_sheet() {
    static Sheet s = parse_sheet(kUACss);
    return s;
}
static const Sheet& scripting_sheet() {  // applied to pages whose scripts run
    static Sheet s = parse_sheet("noscript{display:none!important}");
    return s;
}

constexpr UINT WM_APP_LOADED = WM_APP + 1;
constexpr UINT WM_APP_SCRIPT = WM_APP + 2;  // a worker thread has results for a page script
constexpr float kTabH = 38, kToolH = 44, kBmH = 30, kDevW = 500, kSbW = 10, kLine = 18, kInfoH = 42, kRowH = 22;

static inline D2D1_COLOR_F hexc(uint32_t v, float a = 1) { return D2D1::ColorF(v, a); }
static inline D2D1_COLOR_F colf(const Color& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }

struct RectF {
    float l = 0, t = 0, r = 0, b = 0;
    bool hit(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
    D2D1_RECT_F d() const { return D2D1::RectF(l, t, r, b); }
    float w() const { return r - l; }
    float h() const { return b - t; }
};

struct DevLine {
    int depth;
    std::wstring text;
    Node* node;
    uint8_t kind;  // 0 element, 1 text, 2 comment
};

struct FormState {
    std::wstring value;
    bool checked = false;
    int sel = 0;
};

struct Page {
    std::string url;
    std::unique_ptr<Document> doc;
    std::vector<Sheet> sheets;
    std::vector<ImageRes> images;
    std::unordered_map<const Node*, int> imgNode;
    std::vector<NetEntry> net;
    LayoutOut lo;
    float layoutW = -1;
    bool hasMedia = false;
    size_t rules = 0;
    double tFetch = 0, tParse = 0, tSub = 0, tStyle = 0, tLayout = 0;
    std::vector<DevLine> devLines;
    std::unordered_map<const Node*, FormState> fs;  // live values of form controls
    bool domDirty = false;                            // scripts changed the tree since the devtools view was built
    std::unique_ptr<ScriptHost::Client> sclient;      // declared before `script` so it outlives it
    std::unique_ptr<ScriptHost> script;               // null for pages without scripts
};

struct Tab {
    int id = 0;
    std::vector<std::string> hist;
    int hi = -1;
    std::unique_ptr<Page> page;
    std::wstring title = L"New Tab";
    std::wstring addr;
    bool loading = false;
    uint64_t gen = 0;
    float scroll = 0;
    std::string refreshUrl;  // pending <meta http-equiv=refresh>
    float refreshDelay = 0;
    uint64_t refreshGen = 0;
    int refreshCount = 0;
    double scriptDue = -1;  // when the page's script timers next need service (-1: nothing scheduled)
    bool navPending = false, navReplace = false;
    Node* submitForm = nullptr;  // form a script asked to submit
    Node* submitBy = nullptr;
    std::string navRequest;  // navigation asked for by script; done once the script has returned
};

struct PendingSave {
    std::string origin, user, pass;
    bool update = false;
};

static std::string collapse(const std::string& s) {
    std::string out;
    bool sp = false;
    for (char ch : s) {
        if (isws(ch)) {
            sp = !out.empty();
            continue;
        }
        if (sp) out += ' ';
        sp = false;
        out += ch;
    }
    return out;
}

static Node* form_of(Node* n) {
    for (Node* p = n; p; p = p->parent)
        if (p->is("form")) return p;
    return nullptr;
}

// Depth-first, document-order traversal of the element subtree under `root` (excluding root).
template <class F>
static void for_each_element(Node* root, F&& fn) {
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

class App : public WidgetPainter {
public:
    HWND hwnd = nullptr;
    bool headless = false;
    Gpu gpu;
    TextSystem ts;
    Bookmarks bm;
    PasswordVault vault{app_data_dir() + L"\\passwords.dat"};
    std::vector<std::unique_ptr<Tab>> tabs;
    int active = 0, nextId = 1;
    float W = 1280, H = 860, scale = 1;

    bool addrFocus = false, addrSel = false;
    std::wstring addrText;
    size_t caret = 0;

    // Focused form control in the active page.
    Node* focus = nullptr;
    size_t fCaret = 0, fAnchor = 0;
    float fScroll = 0;
    int popup = -1;  // widget index of the open <select> list
    int popupScroll = 0;
    bool hoverText = false, hoverClick = false;
    std::optional<PendingSave> pendSave;
    Page* paintPage = nullptr;

    bool devOpen = false;
    int devTab = 0;
    float devScroll = 0;

    // Devtools console: one JavaScript engine for the browser until pages get their own (milestone 4).
    struct ConLine {
        int kind;  // 0 log, 1 info, 2 warning, 3 error, 4 what you typed, 5 result
        std::wstring text;
    };
    std::unique_ptr<js::Vm> conVm;
    std::vector<ConLine> conLines;
    std::wstring conInput;
    size_t conCaret = 0;
    bool conFocus = false;
    std::vector<std::wstring> conHist;
    int conHistPos = -1;
    Node* devHover = nullptr;
    Node* devSel = nullptr;

    Node* hoverLink = nullptr;
    std::string hoverHref;
    bool dragSb = false;
    float dragOff = 0;
    std::vector<std::pair<RectF, int>> chips;

    ComPtr<IDWriteTextFormat> fUi, fUiC, fIcon, fIconS, fMono, fSmall, fBold;
    ComPtr<ID2D1SolidColorBrush> brush;
    uint64_t brushGen = 0;
    double lastFrameMs = 0;

    ~App() {
        if (pendSave) secure_wipe(pendSave->pass);
    }

    // ------------------------------------------------------------------ setup
    void initFormats() {
        auto* f = ts.factory();
        auto mkf = [&](const wchar_t* fam, float size, DWRITE_FONT_WEIGHT w, bool center, ComPtr<IDWriteTextFormat>& out) {
            f->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &out);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (center) out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ell;
            f->CreateEllipsisTrimmingSign(out.Get(), &ell);
            out->SetTrimming(&tr, ell.Get());
        };
        mkf(L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, false, fUi);
        mkf(L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true, fUiC);
        mkf(L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, false, fSmall);
        mkf(L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, fBold);
        mkf(L"Segoe MDL2 Assets", 14.f, DWRITE_FONT_WEIGHT_NORMAL, true, fIcon);
        mkf(L"Segoe MDL2 Assets", 9.f, DWRITE_FONT_WEIGHT_NORMAL, true, fIconS);
        mkf(L"Consolas", 12.f, DWRITE_FONT_WEIGHT_NORMAL, false, fMono);
    }

    Tab* cur() { return (active >= 0 && active < (int)tabs.size()) ? tabs[active].get() : nullptr; }
    Page* curPage() {
        Tab* t = cur();
        return t ? t->page.get() : nullptr;
    }
    void invalidate() {
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }
    void resetHover() {
        devHover = devSel = hoverLink = nullptr;
        hoverHref.clear();
        devScroll = 0;
        focus = nullptr;
        popup = -1;
        hoverText = hoverClick = false;
    }

    // ------------------------------------------------------------------ geometry
    float chromeH() const { return kTabH + kToolH + kBmH; }
    RectF contentRect() const { return {0, chromeH(), W - (devOpen ? kDevW : 0), H}; }
    float tabW() const {
        float n = (float)std::max<size_t>(1, tabs.size());
        return std::min(240.f, std::max(60.f, (W - 60) / n));
    }
    RectF tabRect(int i) const {
        float w = tabW(), l = 8 + i * w;
        return {l, 6, l + w, kTabH};
    }
    RectF tabClose(int i) const {
        RectF r = tabRect(i);
        return {r.r - 28, r.t + 6, r.r - 8, r.b - 6};
    }
    RectF newTabRect() const {
        float l = 8 + tabs.size() * tabW() + 4;
        return {l, 8, l + 28, kTabH - 2};
    }
    RectF btn(int i) const {
        float l = 8 + i * 34.f;
        return {l, kTabH + 6, l + 32, kTabH + kToolH - 6};
    }
    RectF rbtn(int i) const {  // from the right
        float r = W - 8 - i * 34.f;
        return {r - 32, kTabH + 6, r, kTabH + kToolH - 6};
    }
    RectF addrRect() const { return {8 + 3 * 34.f + 6, kTabH + 6, W - 8 - 3 * 34.f - 6, kTabH + kToolH - 6}; }
    RectF devRect() const { return {W - kDevW, chromeH(), W, H}; }
    RectF infoRect() const {
        RectF c = contentRect();
        return {c.l, c.t, c.r, c.t + kInfoH};
    }
    RectF infoBtn(int i) const {  // 0 = primary, 1 = dismiss
        RectF r = infoRect();
        float right = r.r - 16 - i * 104.f;
        return {right - 96, r.t + 7, right, r.b - 7};
    }

    // ------------------------------------------------------------------ tabs & navigation
    Tab& newTab(const std::string& url, bool activate = true) {
        auto t = std::make_unique<Tab>();
        t->id = nextId++;
        tabs.push_back(std::move(t));
        Tab& tab = *tabs.back();
        if (activate) {
            active = (int)tabs.size() - 1;
            resetHover();
        }
        if (!url.empty()) navigate(tab, url);
        return tab;
    }

    void closeTab(int i) {
        if (i < 0 || i >= (int)tabs.size()) return;
        if (hwnd) KillTimer(hwnd, 1000 + tabs[i]->id);
        tabs.erase(tabs.begin() + i);
        if (tabs.empty()) newTab("about:home");
        if (active >= (int)tabs.size()) active = (int)tabs.size() - 1;
        else if (i < active) --active;
        resetHover();
        addrFocus = false;
        updateTitle();
        invalidate();
    }

    void activate(int i) {
        if (i < 0 || i >= (int)tabs.size() || i == active) return;
        active = i;
        addrFocus = false;
        resetHover();
        updateTitle();
        invalidate();
    }

    static std::string fixup(const std::wstring& in) {
        std::string s(trim(narrow(in)));
        if (s.empty()) return "about:home";
        if (istarts(s, "about:") || istarts(s, "http://") || istarts(s, "https://") || istarts(s, "file:") || istarts(s, "data:"))
            return s;
        if (s.size() > 2 && isalpha_(s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/')) {
            for (auto& c : s)
                if (c == '\\') c = '/';
            return "file:///" + s;
        }
        bool space = s.find(' ') != std::string::npos, dot = s.find('.') != std::string::npos;
        if (istarts(s, "localhost") || istarts(s, "127.0.0.1")) return "http://" + s;
        if (!space && dot) return "https://" + s;
        return "https://html.duckduckgo.com/html/?q=" + form_encode(s);
    }

    // `req` carries POST data; `fromPage` marks navigations started by page content (links, forms, refresh).
    void navigate(Tab& t, const std::string& input, bool push = true, const FetchOpts* req = nullptr, bool fromPage = false) {
        std::string url = input;
        if (istarts(url, "javascript:")) return;  // javascript: links are not run
        if (istarts(url, "about:bookmarks?remove=")) {
            bm.removeAt((size_t)atoi(url.c_str() + 23));
            url = "about:bookmarks";
        }
        if (istarts(url, "about:passwords?remove=")) {
            if (vault.load() && vault.removeAt((size_t)atoi(url.c_str() + 23))) vault.save();
            vault.lock();
            url = "about:passwords";
        }
        if (push) t.refreshCount = 0;
        // Same-document fragment navigation: just scroll.
        if (!req && t.page && t.page->doc && url.find('#') != std::string::npos && strip_fragment(url) == strip_fragment(t.page->url)) {
            if (push) {
                if (t.hi + 1 < (int)t.hist.size()) t.hist.resize(t.hi + 1);
                t.hist.push_back(url);
                t.hi = (int)t.hist.size() - 1;
            }
            t.page->url = url;
            t.addr = widen(url);
            scrollToFragment(t);
            invalidate();
            return;
        }
        if (push) {
            if (t.hi + 1 < (int)t.hist.size()) t.hist.resize(t.hi + 1);
            t.hist.push_back(url);
            t.hi = (int)t.hist.size() - 1;
        }
        startLoad(t, url, req, fromPage);
    }

    void startLoad(Tab& t, const std::string& url, const FetchOpts* req = nullptr, bool fromPage = false) {
        FetchOpts o = req ? *req : FetchOpts{};
        o.topLevel = true;
        if (fromPage && t.page) {
            Url pu = Url::parse(t.page->url);
            if (pu.valid && pu.hierarchical && !pu.host.empty()) {
                o.referrer = t.page->url;
                o.initiatorSite = site_of(pu.host);
            }
        }
        t.loading = true;
        t.gen++;
        t.refreshUrl.clear();
        t.addr = widen(url);
        if (&t == cur()) addrFocus = false;
        std::string html;
        bool preset = istarts(url, "about:");
        if (preset) html = aboutHtml(url);
        if (headless) {
            auto r = load_page(url, preset ? &html : nullptr, &o);
            installPage(t, std::move(r));
            return;
        }
        int id = t.id;
        uint64_t gen = t.gen;
        HWND h = hwnd;
        std::thread([=, html = std::move(html)] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto r = load_page(url, preset ? &html : nullptr, &o);
            r->tabId = id;
            r->gen = gen;
            if (PostMessageW(h, WM_APP_LOADED, 0, (LPARAM)r.get())) r.release();
            CoUninitialize();
        }).detach();
        updateTitle();
        invalidate();
    }

    void goBack() {
        Tab* t = cur();
        if (t && t->hi > 0) {
            t->hi--;
            navigate(*t, t->hist[t->hi], false);
        }
    }
    void goFwd() {
        Tab* t = cur();
        if (t && t->hi + 1 < (int)t->hist.size()) {
            t->hi++;
            navigate(*t, t->hist[t->hi], false);
        }
    }
    void reload() {
        Tab* t = cur();
        if (t && t->hi >= 0) startLoad(*t, t->hist[t->hi]);
    }

    void onLoaded(LoadResult* raw) {
        std::unique_ptr<LoadResult> r(raw);
        CookieJar::get().save();
        for (auto& t : tabs)
            if (t->id == r->tabId) {
                if (t->gen != r->gen) return;  // superseded navigation
                installPage(*t, std::move(r));
                return;
            }
    }

    void installPage(Tab& t, std::unique_ptr<LoadResult> r) {
        auto p = std::make_unique<Page>();
        p->url = r->url;
        p->doc = std::move(r->doc);
        p->sheets = std::move(r->sheets);
        p->images = std::move(r->images);
        p->imgNode = std::move(r->imgNode);
        p->net = std::move(r->net);
        p->tFetch = r->tFetch;
        p->tParse = r->tParse;
        p->tSub = r->tSub;
        if (&t == cur()) resetHover();
        t.page = std::move(p);
        t.loading = false;
        t.scroll = 0;
        t.scriptDue = -1;
        t.navPending = false;
        t.submitForm = t.submitBy = nullptr;
        if (hwnd) KillTimer(hwnd, kScriptTimerBase + t.id);
        startScripts(t, r->scripts);
        if (!t.page) return;
        bool internal = t.hi >= 0 && t.hi < (int)t.hist.size() && istarts(t.hist[t.hi], "about:");
        if (t.hi >= 0 && t.hi < (int)t.hist.size() && !internal) t.hist[t.hi] = t.page->url;
        std::string title = t.page->doc->title;
        t.title = widen(title.empty() ? t.page->url : title);
        t.addr = widen(internal ? t.hist[t.hi] : t.page->url);
        buildDevLines(*t.page);
        autofill(*t.page);
        ensureLayout(t);
        scrollToFragment(t);
        scheduleRefresh(t);
        updateTitle();
        invalidate();
    }

    // ------------------------------------------------------------------ page scripts
    static constexpr UINT_PTR kScriptTimerBase = 0x40000000;

    struct PageClient : ScriptHost::Client {
        App& app;
        Tab& tab;
        PageClient(App& a, Tab& t) : app(a), tab(t) {}
        Page* page() { return tab.page.get(); }
        void domChanged() override {
            if (Page* p = page()) {
                p->layoutW = -1;
                p->domDirty = true;
            }
            app.invalidate();
        }
        void consoleMessage(int level, const std::wstring& text) override { app.conAdd(level, text); }
        void navigate(const std::string& url, bool replace) override {
            tab.navRequest = url;
            tab.navReplace = replace;
            tab.navPending = true;
        }
        void submitForm(Node* form, Node* submitter) override {
            tab.submitForm = form;
            tab.submitBy = submitter;
        }
        void wake(double delayMs) override { app.scheduleScript(tab, delayMs); }
        std::function<void()> threadWake() override {
            HWND target = app.hwnd;
            int id = tab.id;
            return [target, id] {
                if (target) PostMessageW(target, WM_APP_SCRIPT, (WPARAM)id, 0);
            };
        }
        void ensureLayout() override { app.ensureLayout(tab); }
        void viewport(float& w, float& h, float& sy) override {
            w = app.viewportW();
            h = app.contentRect().h();
            sy = tab.scroll;
        }
        void scrollTo(float y) override {
            app.ensureLayout(tab);
            tab.scroll = y;
            app.clampScroll(tab);
            app.invalidate();
        }
        std::wstring formValue(Node* n) override {
            Page* p = page();
            if (!p) return L"";
            FormState& s = app.stateOf(*p, n);
            if (n->name != "select") return s.value;
            auto opts = select_options(n);
            if (s.sel < 0 || s.sel >= (int)opts.size()) return L"";
            Node* o = opts[s.sel];
            return widen(o->attr("value") ? *o->attr("value") : std::string(trim(text_content(o))));
        }
        void setFormValue(Node* n, const std::wstring& v) override {
            Page* p = page();
            if (!p) return;
            FormState& s = app.stateOf(*p, n);
            if (n->name == "select") {
                auto opts = select_options(n);
                std::string want = narrow(v);
                for (int i = 0; i < (int)opts.size(); ++i)
                    if ((opts[i]->attr("value") ? *opts[i]->attr("value") : std::string(trim(text_content(opts[i])))) == want) s.sel = i;
                return;
            }
            s.value = v;
            if (&tab == app.cur() && app.focus == n) app.fCaret = app.fAnchor = v.size();
        }
        bool formChecked(Node* n) override {
            Page* p = page();
            return p && app.stateOf(*p, n).checked;
        }
        void setFormChecked(Node* n, bool on) override {
            if (Page* p = page()) app.stateOf(*p, n).checked = on;
        }
        void focusNode(Node* n) override {
            if (&tab != app.cur()) return;
            if (n && (n->name == "input" || n->name == "textarea" || n->name == "select" || n->name == "button")) app.focusWidget(n, false);
            else app.focus = nullptr;
            app.invalidate();
        }
        Node* focusedNode() override { return &tab == app.cur() ? app.focus : nullptr; }
        void titleChanged() override {
            Page* p = page();
            if (!p) return;
            tab.title = widen(p->doc->title.empty() ? p->url : p->doc->title);
            app.updateTitle();
            app.invalidate();
        }
        void urlChanged(const std::string& url) override {
            Page* p = page();
            if (!p) return;
            p->url = url;
            tab.addr = widen(url);
            if (tab.hi >= 0 && tab.hi < (int)tab.hist.size()) tab.hist[tab.hi] = url;
            app.invalidate();
        }
    };

    void startScripts(Tab& t, const std::vector<ScriptSource>& scripts) {
        if (scripts.empty() || !t.page) return;
        Page& p = *t.page;
        p.sclient = std::make_unique<PageClient>(*this, t);
        p.script = std::make_unique<ScriptHost>(*p.doc, p.url, *p.sclient);
        if (const char* inject = getenv("TB_INJECT_JS")) {  // debugging aid: a script to run before the page's own
            std::vector<ScriptSource> all{{nullptr, inject, "injected"}};
            all.insert(all.end(), scripts.begin(), scripts.end());
            p.script->runScripts(all);
        } else
            p.script->runScripts(scripts);
        afterScript(t);
    }
    // Called whenever control comes back from page script: carries out a navigation the script asked for.
    void afterScript(Tab& t) {
        if (t.submitForm) {
            Node* form = t.submitForm;
            Node* by = t.submitBy;
            t.submitForm = t.submitBy = nullptr;
            if (&t == cur() && t.page) submitForm(form, by, false);
        }
        if (t.navPending) {
            t.navPending = false;
            std::string url = t.navRequest;
            navigate(t, url, !t.navReplace, nullptr, true);
        }
        invalidate();
    }
    void scheduleScript(Tab& t, double delayMs) {
        double due = now_ms() + std::max(0.0, delayMs);
        t.scriptDue = t.scriptDue < 0 ? due : std::min(t.scriptDue, due);
        if (hwnd) SetTimer(hwnd, kScriptTimerBase + t.id, (UINT)std::max(1.0, t.scriptDue - now_ms()), nullptr);
    }
    void runScriptTimers(Tab& t) {
        t.scriptDue = -1;
        if (!t.page || !t.page->script) return;
        t.page->script->pump();
        afterScript(t);
    }
    // Headless mode has no message loop: serve timers here for a bounded time.
    void pumpScripts(Tab& t, double budgetMs) {
        double end = now_ms() + budgetMs;
        while (t.page && t.page->script && now_ms() < end) {
            bool busy = t.page->script->busy();
            if (t.scriptDue < 0 && !busy) break;
            double wait = t.scriptDue < 0 ? 5 : t.scriptDue - now_ms();
            if (busy) wait = std::min(wait, 5.0);
            if (now_ms() + std::max(0.0, wait) > end) break;
            if (wait > 0) Sleep((DWORD)wait + 1);
            runScriptTimers(t);
        }
    }
    // Sends a DOM event to the page's scripts. Returns false when a listener cancelled the default action.
    bool fireEvent(Tab& t, Node* target, const char* type, bool bubbles = true, bool cancelable = true) {
        if (!t.page || !t.page->script) return true;
        bool go = t.page->script->dispatch(target, type, bubbles, cancelable);
        afterScript(t);
        return go;
    }
    // The innermost element whose box contains a document point.
    Node* nodeAt(Page& p, float dx, float dy) {
        Node* best = p.doc->body;
        std::vector<Node*> st{p.doc->root.get()};
        while (!st.empty()) {
            Node* n = st.back();
            st.pop_back();
            if (n->type == Node::T::Element && n->hasBox && dx >= n->bx && dx < n->bx + n->bw && dy >= n->by && dy < n->by + n->bh) best = n;
            for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) st.push_back(it->get());
        }
        return best;
    }

    float viewportW() const { return std::max(200.f, contentRect().w() - kSbW); }

    void restyle(Page& p, float vw) {
        double t0 = now_ms();
        StyleEngine se;
        se.add(&ua_sheet(), true);
        if (p.doc->quirks) se.add(&quirks_sheet(), true);
        for (auto& s : p.sheets) se.add(&s, false);
        if (p.script) se.add(&scripting_sheet(), true);
        se.compute(*p.doc, vw, contentRect().h());
        p.hasMedia = se.hasMedia();
        p.rules = se.ruleCount();
        p.tStyle = now_ms() - t0;
    }

    void ensureLayout(Tab& t) {
        if (!t.page || !t.page->doc) return;
        Page& p = *t.page;
        float vw = viewportW();
        if (std::fabs(p.layoutW - vw) < 0.5f) return;
        if (p.layoutW < 0 || p.hasMedia) restyle(p, vw);
        double t0 = now_ms();
        LayoutInput in;
        in.doc = p.doc.get();
        in.ts = &ts;
        in.vw = vw;
        in.vh = contentRect().h();
        in.images = &p.images;
        in.imgNode = &p.imgNode;
        p.lo = layout_document(in);
        p.tLayout = now_ms() - t0;
        p.layoutW = vw;
        if (p.domDirty) {
            p.domDirty = false;
            buildDevLines(p);
            if (&t == cur()) devHover = devSel = nullptr;
        }
        if (&t == cur()) popup = -1;
        clampScroll(t);
    }

    float maxScroll(Tab& t) {
        if (!t.page) return 0;
        return std::max(0.f, t.page->lo.height - contentRect().h());
    }
    void clampScroll(Tab& t) { t.scroll = std::clamp(t.scroll, 0.f, maxScroll(t)); }

    void scrollToFragment(Tab& t) {
        if (!t.page || !t.page->doc) return;
        size_t h = t.page->url.find('#');
        if (h == std::string::npos) return;
        std::string frag = percent_decode(t.page->url.substr(h + 1));
        if (frag.empty()) {
            t.scroll = 0;
            return;
        }
        std::vector<Node*> st{t.page->doc->root.get()};
        while (!st.empty()) {
            Node* n = st.back();
            st.pop_back();
            if (n->type == Node::T::Element && (n->id == frag || (n->name == "a" && n->attr("name") && *n->attr("name") == frag))) {
                if (n->hasBox) {
                    t.scroll = n->by;
                    clampScroll(t);
                }
                return;
            }
            for (auto& k : n->kids) st.push_back(k.get());
        }
    }

    // <meta http-equiv="refresh" content="N; url=...">: the no-script way sites redirect.
    void scheduleRefresh(Tab& t) {
        if (!t.page || !t.page->doc || t.refreshCount > 8) return;
        std::string content;
        for_each_element(t.page->doc->root.get(), [&](Node* n) {
            if (n->name == "meta" && n->attr("http-equiv") && ieq(*n->attr("http-equiv"), "refresh") && n->attr("content")) {
                content = *n->attr("content");
                return false;
            }
            return true;
        });
        if (content.empty()) return;
        float delay = (float)atof(content.c_str());
        size_t sep = content.find_first_of(";,");
        if (sep == std::string::npos) return;  // plain reload timers are ignored
        std::string_view rest = trim(std::string_view(content).substr(sep + 1));
        if (istarts(rest, "url")) {
            rest = trim(rest.substr(3));
            if (!rest.empty() && rest[0] == '=') rest = trim(rest.substr(1));
        }
        if (rest.size() >= 2 && (rest[0] == '"' || rest[0] == '\'')) rest = rest.substr(1, rest.size() - 2);
        if (rest.empty()) return;
        std::string target = resolve_url(t.page->doc->baseUrl, rest);
        if (target == t.page->url || istarts(target, "javascript:")) return;
        t.refreshUrl = target;
        t.refreshDelay = std::clamp(delay, 0.f, 60.f);
        t.refreshGen = t.gen;
        t.refreshCount++;
        if (hwnd) SetTimer(hwnd, 1000 + t.id, (UINT)(t.refreshDelay * 1000) + 10, nullptr);
    }

    void onTimer(UINT_PTR id) {
        if (hwnd) KillTimer(hwnd, id);
        if (id >= kScriptTimerBase) {
            for (auto& t : tabs)
                if (kScriptTimerBase + t->id == id) {
                    runScriptTimers(*t);
                    return;
                }
            return;
        }
        for (auto& t : tabs)
            if (1000 + t->id == (int)id && t->gen == t->refreshGen && !t->refreshUrl.empty()) {
                std::string u = t->refreshUrl;
                t->refreshUrl.clear();
                navigate(*t, u, false, nullptr, true);
                return;
            }
    }

    void updateTitle() {
        if (!hwnd) return;
        Tab* t = cur();
        std::wstring title = t ? (t->loading && !t->page ? L"Loading…" : t->title) : L"";
        title += title.empty() ? L"Tiny Browser" : L" — Tiny Browser";
        SetWindowTextW(hwnd, title.c_str());
    }

    // ------------------------------------------------------------------ internal pages
    std::string aboutHtml(const std::string& url) {
        static const std::string css =
            "<style>body{font-family:Segoe UI,sans-serif;margin:0;background:#f6f7f9;color:#1f2328}"
            "main{max-width:780px;margin:0 auto;padding:36px 24px}h1{font-weight:600;font-size:30px;margin:0 0 4px}"
            "p.sub{color:#59636e;margin:0 0 24px}.card{background:#fff;border:1px solid #d8dee4;border-radius:10px;padding:14px 20px;margin:0 0 16px}"
            "h2{font-size:16px;margin:4px 0 10px}a{color:#0969da;text-decoration:none}li{margin:5px 0}"
            "table{width:100%}td{padding:5px 6px;border-bottom:1px solid #eaeef2}th{text-align:left;padding:5px 6px;color:#59636e}"
            "code,kbd{background:#eff1f3;padding:1px 5px;font-size:13px;border-radius:4px}.muted{color:#6e7781}.ok{color:#1a7f37}.todo{color:#9a6700}"
            ".search{display:flex;gap:8px;margin:0 0 20px}.search input[type=text]{flex:1;font-size:15px;padding:9px 14px;border:1px solid #c5ccd3;border-radius:20px}"
            ".search input[type=submit]{font-size:14px;padding:9px 18px;border:1px solid #0969da;border-radius:20px;background:#0969da;color:#fff}</style>";
        if (url == "about:blank") return "<!doctype html><html><head><title>about:blank</title></head><body></body></html>";
        if (istarts(url, "about:bookmarks")) {
            std::string h = "<!doctype html><html><head><title>Bookmarks</title>" + css + "</head><body><main><h1>Bookmarks</h1><p class=sub>" +
                            std::to_string(bm.items.size()) + " saved. Stored in <code>%APPDATA%\\TinyBrowser\\bookmarks.tsv</code>.</p><div class=card>";
            if (bm.items.empty()) h += "<p class=muted>No bookmarks yet. Press <kbd>Ctrl+D</kbd> or click the star to add the current page.</p>";
            else {
                h += "<table>";
                for (size_t i = 0; i < bm.items.size(); ++i)
                    h += "<tr><td><a href=\"" + escape_html(bm.items[i].url) + "\">" + escape_html(bm.items[i].title) +
                         "</a><br><span class=muted style=\"font-size:12px\">" + escape_html(bm.items[i].url) +
                         "</span></td><td style=\"text-align:right\"><a href=\"about:bookmarks?remove=" + std::to_string(i) + "\">Remove</a></td></tr>";
                h += "</table>";
            }
            return h + "</div></main></body></html>";
        }
        if (istarts(url, "about:passwords")) {
            bool ok = vault.load();
            std::string h = "<!doctype html><html><head><title>Passwords</title>" + css +
                            "</head><body><main><h1>Saved passwords</h1><p class=sub>Encrypted with Windows DPAPI for your Windows account. "
                            "File: <code>%APPDATA%\\TinyBrowser\\passwords.dat</code></p><div class=card>";
            if (!ok) h += "<p class=muted>The password file could not be decrypted with this Windows account.</p>";
            else if (vault.all().empty())
                h += "<p class=muted>No saved passwords yet. Sign in to a site and choose Save when asked.</p>";
            else {
                h += "<table><tr><th>Site</th><th>Username</th><th>Password</th><th></th></tr>";
                for (size_t i = 0; i < vault.all().size(); ++i) {
                    auto& c = vault.all()[i];
                    h += "<tr><td>" + escape_html(c.origin) + "</td><td>" + escape_html(c.user) +
                         "</td><td>\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2</td><td style=\"text-align:right\"><a href=\"about:passwords?remove=" +
                         std::to_string(i) + "\">Remove</a></td></tr>";
                }
                h += "</table>";
            }
            vault.lock();
            return h + "</div></main></body></html>";
        }
        std::string links;
        const char* demo[][2] = {{"https://www.google.com/", "Google"},
                                 {"https://news.ycombinator.com/", "Hacker News"},
                                 {"https://en.wikipedia.org/wiki/Web_browser", "Wikipedia: Web browser"},
                                 {"https://text.npr.org/", "NPR text edition"},
                                 {"https://lite.cnn.com/", "CNN lite"},
                                 {"https://example.com/", "example.com"}};
        for (auto& d : demo) links += "<li><a href=\"" + std::string(d[0]) + "\">" + d[1] + "</a></li>";
        std::string bms;
        for (size_t i = 0; i < bm.items.size() && i < 8; ++i)
            bms += "<li><a href=\"" + escape_html(bm.items[i].url) + "\">" + escape_html(bm.items[i].title) + "</a></li>";
        if (bms.empty()) bms = "<li class=muted>Nothing yet. Press Ctrl+D on any page.</li>";
        return "<!doctype html><html><head><title>New Tab</title>" + css +
               "</head><body><main><h1>Tiny Browser</h1><p class=sub>A from-scratch browser engine: own HTML parser, CSS cascade, layout and a "
               "Direct2D GPU painter. No Chromium, WebKit, Gecko or third-party JS engine.</p>"
               "<form class=search action=\"https://html.duckduckgo.com/html/\" method=get><input type=text name=q placeholder=\"Search the web\">"
               "<input type=submit value=Search></form>"
               "<div class=card><h2>Try a page</h2><ul>" + links + "</ul></div>"
               "<div class=card><h2>Bookmarks</h2><ul>" + bms + "</ul><p><a href=\"about:bookmarks\">Manage bookmarks</a> &middot; <a href=\"about:passwords\">Saved passwords</a></p></div>"
               "<div class=card><h2>Keyboard</h2><table>"
               "<tr><td><kbd>Ctrl+L</kbd></td><td>Address bar (type a URL or search words)</td></tr>"
               "<tr><td><kbd>Ctrl+T</kbd> / <kbd>Ctrl+W</kbd></td><td>New tab / close tab</td></tr>"
               "<tr><td><kbd>Ctrl+Tab</kbd></td><td>Next tab</td></tr>"
               "<tr><td><kbd>Ctrl+D</kbd></td><td>Bookmark this page</td></tr>"
               "<tr><td><kbd>Tab</kbd></td><td>Move between form fields</td></tr>"
               "<tr><td><kbd>F12</kbd></td><td>Developer tools (elements, computed styles, network, performance)</td></tr>"
               "<tr><td><kbd>Alt+Left</kbd> / <kbd>Alt+Right</kbd></td><td>Back / forward</td></tr>"
               "<tr><td><kbd>F5</kbd></td><td>Reload</td></tr></table></div>"
               "<div class=card><h2>Engine status</h2><ul>"
               "<li class=ok>HTML5 parsing, CSS cascade, block / inline / table / flexbox layout, positioning, clipping, rounded corners</li>"
               "<li class=ok>Forms with GET and POST, cookies, saved passwords (DPAPI), HTTPS + HTTP/2</li>"
               "<li class=ok>GPU rendering (Direct2D on Direct3D 11)</li>"
               "<li class=todo>Next: our own JavaScript engine and DOM bindings; floats, grid, SVG</li></ul></div>"
               "</main></body></html>";
    }

    // ------------------------------------------------------------------ devtools data
    void buildDevLines(Page& p) {
        p.devLines.clear();
        struct F {
            Node* n;
            int depth;
        };
        std::vector<F> st{{p.doc->root.get(), -1}};
        while (!st.empty() && p.devLines.size() < 60000) {
            F f = st.back();
            st.pop_back();
            Node* n = f.n;
            if (n->type == Node::T::Element) {
                std::string s = "<" + n->name;
                int shown = 0;
                for (auto& a : n->attrs) {
                    if (shown >= 4) {
                        s += " \xE2\x80\xA6";
                        break;
                    }
                    std::string v = a.second.size() > 60 ? a.second.substr(0, 60) + "\xE2\x80\xA6" : a.second;
                    s += " " + a.first + "=\"" + v + "\"";
                    ++shown;
                }
                s += ">";
                p.devLines.push_back({f.depth, widen(s), n, 0});
            } else if (n->type == Node::T::Text) {
                std::string out = collapse(n->text.substr(0, 400));
                if (out.size() > 90) out.resize(90);
                if (!out.empty()) p.devLines.push_back({f.depth, widen("\"" + out + "\""), n->parent, 1});
            } else if (n->type == Node::T::Comment) {
                std::string c = n->text.substr(0, 80);
                for (auto& ch : c)
                    if (ch == '\n' || ch == '\r') ch = ' ';
                p.devLines.push_back({f.depth, widen("<!--" + c + "-->"), nullptr, 2});
            }
            if (n->type == Node::T::Element || n->type == Node::T::Document)
                for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) st.push_back({it->get(), f.depth + 1});
        }
    }

    // ------------------------------------------------------------------ forms
    FormState& stateOf(Page& p, Node* n) {
        auto it = p.fs.find(n);
        if (it != p.fs.end()) return it->second;
        FormState s;
        WidgetKind k = widget_kind(n);
        if (k == WidgetKind::TextArea) s.value = widen(text_content(n));
        else if (auto v = n->attr("value")) s.value = widen(*v);
        s.checked = n->attr("checked") != nullptr;
        if (k == WidgetKind::Select) {
            auto opts = select_options(n);
            for (size_t i = 0; i < opts.size(); ++i)
                if (opts[i]->attr("selected")) s.sel = (int)i;
        }
        return p.fs.emplace(n, std::move(s)).first->second;
    }

    std::wstring* focusText() {
        Page* p = curPage();
        if (!focus || !p) return nullptr;
        WidgetKind k = widget_kind(focus);
        if (k != WidgetKind::Text && k != WidgetKind::Password && k != WidgetKind::TextArea) return nullptr;
        std::wstring* v = &stateOf(*p, focus).value;
        fCaret = std::min(fCaret, v->size());
        fAnchor = std::min(fAnchor, v->size());
        return v;
    }

    int widgetIndex(Node* n) {
        Page* p = curPage();
        if (!p || !n) return -1;
        for (size_t i = 0; i < p->lo.widgets.size(); ++i)
            if (p->lo.widgets[i].node == n) return (int)i;
        return -1;
    }

    int widgetAt(float dx, float dy) {
        Page* p = curPage();
        if (!p) return -1;
        int best = -1;
        for (size_t i = 0; i < p->lo.widgets.size(); ++i) {
            auto& w = p->lo.widgets[i];
            if (dx >= w.x && dx < w.x + w.w && dy >= w.y && dy < w.y + w.h && !w.st->hidden)
                if (best < 0 || w.layer >= p->lo.widgets[best].layer) best = (int)i;
        }
        return best;
    }

    void focusWidget(Node* n, bool selectAll) {
        focus = n;
        conFocus = false;
        fScroll = 0;
        popup = -1;
        addrFocus = false;
        if (std::wstring* v = focusText()) {
            fCaret = v->size();
            fAnchor = selectAll ? 0 : fCaret;
        }
        // Keep the control on screen.
        Tab* t = cur();
        int wi = widgetIndex(n);
        if (t && wi >= 0) {
            auto& w = t->page->lo.widgets[wi];
            float viewH = contentRect().h();
            if (w.y < t->scroll + 8) t->scroll = w.y - 40;
            else if (w.y + w.h > t->scroll + viewH - 8) t->scroll = w.y + w.h - viewH + 40;
            clampScroll(*t);
        }
    }

    bool deleteSelection(std::wstring& v) {
        if (fAnchor == fCaret) return false;
        size_t a = std::min(fAnchor, fCaret), b = std::max(fAnchor, fCaret);
        v.erase(a, b - a);
        fCaret = fAnchor = a;
        return true;
    }

    void insertText(const std::wstring& s) {
        std::wstring* v = focusText();
        if (!v || focus->attr("readonly") || focus->attr("disabled")) return;
        deleteSelection(*v);
        std::wstring add = s;
        if (auto ml = focus->attr("maxlength")) {
            int m = atoi(ml->c_str());
            if (m > 0 && v->size() + add.size() > (size_t)m) add.resize(m > (int)v->size() ? m - v->size() : 0);
        }
        v->insert(fCaret, add);
        fCaret += add.size();
        fAnchor = fCaret;
    }

    Node* defaultSubmit(Node* form) {
        Node* found = nullptr;
        for_each_element(form, [&](Node* e) {
            if (e->attr("disabled")) return true;
            std::string type = e->attr("type") ? lower(*e->attr("type")) : "";
            if ((e->name == "input" && (type == "submit" || type == "image")) || (e->name == "button" && (type.empty() || type == "submit"))) {
                found = e;
                return false;
            }
            return true;
        });
        return found;
    }

    void submitForm(Node* from, Node* submitter, bool notify = true) {
        Tab* t = cur();
        if (!t || !t->page) return;
        Page& p = *t->page;
        Node* form = form_of(from);
        if (!form) return;
        if (notify && !fireEvent(*t, form, "submit")) return;  // a script handled the form itself
        if (t->page.get() != &p) return;
        if (!submitter) submitter = defaultSubmit(form);
        std::string body, passVal, userVal, lastText;
        bool sawPass = false;
        auto add = [&](const std::string& name, const std::string& value) {
            if (!body.empty()) body += '&';
            body += form_encode(name) + "=" + form_encode(value);
        };
        for_each_element(form, [&](Node* e) {
            if (e->attr("disabled")) return true;
            const std::string* name = e->attr("name");
            bool named = name && !name->empty();
            auto fsIt = p.fs.find(e);
            if (e->name == "input") {
                std::string type = e->attr("type") ? lower(*e->attr("type")) : "text";
                std::string value = fsIt != p.fs.end() ? narrow(fsIt->second.value) : (e->attr("value") ? *e->attr("value") : "");
                if (type == "password") {
                    if (!value.empty() && !sawPass) {
                        sawPass = true;
                        passVal = value;
                        userVal = lastText;
                    }
                } else if (type == "text" || type == "email" || type == "tel") {
                    if (!sawPass && !value.empty()) lastText = value;
                }
                if (!named) return true;
                if (type == "checkbox" || type == "radio") {
                    bool on = fsIt != p.fs.end() ? fsIt->second.checked : e->attr("checked") != nullptr;
                    if (on) add(*name, e->attr("value") ? *e->attr("value") : "on");
                } else if (type == "submit") {
                    if (e == submitter) add(*name, e->attr("value") ? *e->attr("value") : "Submit");
                } else if (type == "image") {
                    if (e == submitter) {
                        add(*name + ".x", "0");
                        add(*name + ".y", "0");
                    }
                } else if (type != "button" && type != "reset" && type != "file") {
                    add(*name, value);
                }
            } else if (e->name == "textarea" && named) {
                std::string v = fsIt != p.fs.end() ? narrow(fsIt->second.value) : text_content(e), out;
                for (char ch : v) {
                    if (ch == '\r') continue;
                    if (ch == '\n') out += "\r\n";
                    else out += ch;
                }
                add(*name, out);
            } else if (e->name == "select" && named) {
                auto opts = select_options(e);
                int sel = fsIt != p.fs.end() ? fsIt->second.sel : stateOf(p, e).sel;
                if (sel >= 0 && sel < (int)opts.size())
                    add(*name, opts[sel]->attr("value") ? *opts[sel]->attr("value") : collapse(text_content(opts[sel])));
            } else if (e->name == "button" && named && e == submitter) {
                add(*name, e->attr("value") ? *e->attr("value") : "");
            }
            return true;
        });
        auto pick = [&](const char* own, const char* onForm) -> std::string {
            if (submitter)
                if (auto a = submitter->attr(own)) return *a;
            if (auto a = form->attr(onForm)) return *a;
            return {};
        };
        std::string method = lower(pick("formmethod", "method"));
        std::string action = pick("formaction", "action");
        std::string url = resolve_url(p.doc->baseUrl, action.empty() ? strip_fragment(p.url) : action);
        if (method == "dialog" || istarts(url, "javascript:")) return;

        if (sawPass) offerSave(p.url, userVal, passVal);
        secure_wipe(passVal);

        if (method == "post") {
            FetchOpts o;
            o.method = "POST";
            o.body = body;
            o.contentType = "application/x-www-form-urlencoded";
            navigate(*t, url, true, &o, true);
            secure_wipe(o.body);
        } else {
            Url u = Url::parse(url);
            if (!u.valid) return;
            u.query = body;
            u.hasQuery = true;
            u.hasFragment = false;
            navigate(*t, u.str(), true, nullptr, true);
        }
        secure_wipe(body);
    }

    static bool savableOrigin(const Url& u) {
        return u.valid && (u.scheme == "https" || u.host == "localhost" || u.host == "127.0.0.1");
    }

    // After a login form is submitted: offer to remember the password (never saved without a click).
    void offerSave(const std::string& pageUrl, const std::string& user, const std::string& pass) {
        Url u = Url::parse(pageUrl);
        if (!savableOrigin(u) || pass.empty()) return;
        std::string origin = u.origin();
        bool same = false, update = false;
        if (vault.load()) {
            if (const Credential* c = vault.find(origin, user)) {
                same = c->pass == pass;
                update = !same;
            }
        }
        vault.lock();
        if (pendSave) secure_wipe(pendSave->pass);
        pendSave.reset();
        if (!same) pendSave = PendingSave{origin, user, pass, update};
    }

    void answerSave(bool yes) {
        if (!pendSave) return;
        if (yes && vault.load()) {
            vault.put(pendSave->origin, pendSave->user, pendSave->pass);
            vault.save();
        }
        vault.lock();
        secure_wipe(pendSave->pass);
        pendSave.reset();
        invalidate();
    }

    // Fills a saved login into the first password form of a freshly loaded page.
    void autofill(Page& p) {
        Url u = Url::parse(p.url);
        if (!savableOrigin(u)) return;
        Node* pass = nullptr;
        Node* user = nullptr;
        Node* lastText = nullptr;
        for_each_element(p.doc->root.get(), [&](Node* e) {
            if (e->name != "input") return true;
            std::string type = e->attr("type") ? lower(*e->attr("type")) : "text";
            if (type == "password") {
                pass = e;
                user = lastText;
                return false;
            }
            if (type == "text" || type == "email" || type == "tel") lastText = e;
            return true;
        });
        if (!pass) return;
        if (vault.load()) {
            auto creds = vault.forOrigin(u.origin());
            if (!creds.empty()) {
                stateOf(p, pass).value = widen(creds[0]->pass);
                if (user && form_of(user) == form_of(pass)) stateOf(p, user).value = widen(creds[0]->user);
            }
        }
        vault.lock();
    }

    void activateWidget(int wi, float dx, float dy) {
        Page* p = curPage();
        if (!p || wi < 0 || wi >= (int)p->lo.widgets.size()) return;
        WidgetArea w = p->lo.widgets[wi];
        Node* n = w.node;
        if (n->attr("disabled")) return;
        WidgetKind k = widget_kind(n);
        switch (k) {
            case WidgetKind::Text:
            case WidgetKind::Password:
            case WidgetKind::TextArea: {
                bool was = focus == n;
                focusWidget(n, false);
                if (dx >= 0) {
                    fCaret = fAnchor = textHit(w, dx, dy, was ? fScroll : 0);
                }
                break;
            }
            case WidgetKind::Check:
                focus = n;
                stateOf(*p, n).checked = !stateOf(*p, n).checked;
                if (Tab* t = cur()) {
                    fireEvent(*t, n, "input", true, false);
                    fireEvent(*t, n, "change", true, false);
                }
                break;
            case WidgetKind::Radio: {
                focus = n;
                Node* form = form_of(n);
                const std::string* name = n->attr("name");
                if (name)
                    for (auto& o : p->lo.widgets)
                        if (o.node != n && widget_kind(o.node) == WidgetKind::Radio && form_of(o.node) == form && o.node->attr("name") &&
                            *o.node->attr("name") == *name)
                            stateOf(*p, o.node).checked = false;
                stateOf(*p, n).checked = true;
                if (Tab* t = cur()) {
                    fireEvent(*t, n, "input", true, false);
                    fireEvent(*t, n, "change", true, false);
                }
                break;
            }
            case WidgetKind::Select:
                focus = n;
                popup = popup == wi ? -1 : wi;
                popupScroll = std::max(0, stateOf(*p, n).sel - 5);
                break;
            case WidgetKind::Button: {
                focus = n;
                std::string type = n->attr("type") ? lower(*n->attr("type")) : (n->name == "button" ? "submit" : "");
                if (type == "submit" || type == "image") submitForm(n, n);
                else if (type == "reset") {
                    if (Node* form = form_of(n))
                        for_each_element(form, [&](Node* e) {
                            p->fs.erase(e);
                            return true;
                        });
                }
                break;
            }
            default: break;
        }
        invalidate();
    }

    ComPtr<IDWriteTextLayout> widgetTextLayout(const WidgetArea& w, const std::wstring& text, bool area) {
        ComPtr<IDWriteTextLayout> L = ts.layout(*w.st, text, false, false);
        if (L && area) {
            float padH = std::max(0.f, w.st->padding[1].resolve(0)) + std::max(0.f, w.st->padding[3].resolve(0)) + w.st->border[1] + w.st->border[3];
            L->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            L->SetMaxWidth(std::max(10.f, w.w - padH));
        }
        return L;
    }

    std::wstring shownValue(Page& p, Node* n) {
        FormState& s = stateOf(p, n);
        return widget_kind(n) == WidgetKind::Password ? std::wstring(s.value.size(), L'•') : s.value;
    }

    size_t textHit(const WidgetArea& w, float dx, float dy, float scrollX) {
        Page* p = curPage();
        if (!p) return 0;
        bool area = widget_kind(w.node) == WidgetKind::TextArea;
        std::wstring shown = shownValue(*p, w.node);
        auto L = widgetTextLayout(w, shown, area);
        if (!L) return shown.size();
        float padL = std::max(0.f, w.st->padding[3].resolve(0)) + w.st->border[3];
        float padT = std::max(0.f, w.st->padding[0].resolve(0)) + w.st->border[0];
        float a, d;
        ts.metrics(*w.st, a, d);
        float lx = dx - (w.x + padL) + scrollX;
        float ly = area ? dy - (w.y + padT) : (a + d) / 2;
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS m{};
        L->HitTestPoint(lx, ly, &trailing, &inside, &m);
        return std::min(shown.size(), (size_t)m.textPosition + (trailing ? 1 : 0));
    }

    void paintWidget(ID2D1RenderTarget* rt, const DItem& it) override {
        Page* pp = paintPage;
        if (!pp || it.img < 0 || it.img >= (int)pp->lo.widgets.size()) return;
        const WidgetArea& w = pp->lo.widgets[it.img];
        Node* n = w.node;
        const Style& st = *w.st;
        WidgetKind k = widget_kind(n);
        bool focused = n == focus && pp == curPage();
        D2D1_RECT_F r = D2D1::RectF(it.x, it.y, it.x + it.w, it.y + it.h);
        if (k == WidgetKind::Check || k == WidgetKind::Radio) {
            bool on = stateOf(*pp, n).checked;
            D2D1_RECT_F b = D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f);
            D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2), it.w / 2 - 0.5f, it.h / 2 - 0.5f);
            brush->SetColor(hexc(on ? 0x1a73e8 : 0xffffff));
            if (k == WidgetKind::Check) rt->FillRoundedRectangle(D2D1::RoundedRect(b, 2, 2), brush.Get());
            else rt->FillEllipse(el, brush.Get());
            brush->SetColor(hexc(on ? 0x1a73e8 : 0x767676));
            if (k == WidgetKind::Check) rt->DrawRoundedRectangle(D2D1::RoundedRect(b, 2, 2), brush.Get(), 1);
            else rt->DrawEllipse(el, brush.Get(), 1);
            if (on) {
                brush->SetColor(hexc(0xffffff));
                if (k == WidgetKind::Check) {
                    rt->DrawLine(D2D1::Point2F(r.left + 3, r.top + 6.8f), D2D1::Point2F(r.left + 5.4f, r.top + 9.4f), brush.Get(), 1.7f);
                    rt->DrawLine(D2D1::Point2F(r.left + 5.4f, r.top + 9.4f), D2D1::Point2F(r.left + 10, r.top + 3.8f), brush.Get(), 1.7f);
                } else {
                    rt->FillEllipse(D2D1::Ellipse(el.point, 2.6f, 2.6f), brush.Get());
                }
            }
            if (focused) {
                brush->SetColor(hexc(0x1a73e8, 0.55f));
                rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 2, r.top - 2, r.right + 2, r.bottom + 2), 4, 4), brush.Get(), 1.5f);
            }
            return;
        }
        float bw = st.border[0];
        float rad = std::min(st.radius, std::min(it.w, it.h) / 2);
        if (!st.bg.transparent()) {
            brush->SetColor(colf(st.bg));
            if (rad > 0.5f) rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush.Get());
            else rt->FillRectangle(r, brush.Get());
        }
        if (bw > 0 && !st.borderColor[0].transparent()) {
            float h = bw / 2;
            D2D1_RECT_F b = D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h);
            brush->SetColor(colf(st.borderColor[0]));
            if (rad > 0.5f) rt->DrawRoundedRectangle(D2D1::RoundedRect(b, std::max(0.f, rad - h), std::max(0.f, rad - h)), brush.Get(), bw);
            else rt->DrawRectangle(b, brush.Get(), bw);
        }
        if (focused) {
            brush->SetColor(hexc(0x1a73e8));
            D2D1_RECT_F b = D2D1::RectF(r.left - 1, r.top - 1, r.right + 1, r.bottom + 1);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(b, rad + 1, rad + 1), brush.Get(), 2);
        }
        float padL = std::max(0.f, st.padding[3].resolve(0)) + st.border[3], padR = std::max(0.f, st.padding[1].resolve(0)) + st.border[1];
        float padT = std::max(0.f, st.padding[0].resolve(0)) + st.border[0];
        float il = it.x + padL, ir = it.x + it.w - padR;
        float a, d;
        ts.metrics(st, a, d);
        float th = a + d, ty = it.y + (it.h - th) / 2;
        const auto opt = D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT;
        rt->PushAxisAlignedClip(D2D1::RectF(std::min(il, ir), it.y, std::max(il, ir), it.y + it.h), D2D1_ANTIALIAS_MODE_ALIASED);
        switch (k) {
            case WidgetKind::Button: {
                std::wstring label = widget_label(n);
                float tw = ts.width(st, label);
                if (auto L = ts.layout(st, label, false, false)) {
                    brush->SetColor(colf(st.color));
                    rt->DrawTextLayout(D2D1::Point2F(std::max(il, il + ((ir - il) - tw) / 2), ty), L.Get(), brush.Get(), opt);
                }
                break;
            }
            case WidgetKind::Select: {
                auto opts = select_options(n);
                int sel = stateOf(*pp, n).sel;
                std::wstring t = (sel >= 0 && sel < (int)opts.size()) ? widen(collapse(text_content(opts[sel]))) : L"";
                brush->SetColor(colf(st.color));
                if (auto L = ts.layout(st, t, false, false)) rt->DrawTextLayout(D2D1::Point2F(il, ty), L.Get(), brush.Get(), opt);
                if (auto A = ts.layout(st, L"▾", false, false)) rt->DrawTextLayout(D2D1::Point2F(ir - 11, ty), A.Get(), brush.Get(), opt);
                break;
            }
            case WidgetKind::TextArea: {
                FormState& s = stateOf(*pp, n);
                auto L = widgetTextLayout(w, s.value, true);
                if (L) {
                    brush->SetColor(colf(st.color));
                    rt->DrawTextLayout(D2D1::Point2F(il, it.y + padT), L.Get(), brush.Get(), opt);
                    if (focused) {
                        float cx = 0, cy = 0;
                        DWRITE_HIT_TEST_METRICS m{};
                        L->HitTestTextPosition((UINT32)std::min(fCaret, s.value.size()), FALSE, &cx, &cy, &m);
                        rt->FillRectangle(D2D1::RectF(il + cx, it.y + padT + cy, il + cx + 1.2f, it.y + padT + cy + th), brush.Get());
                    }
                }
                break;
            }
            default: {
                std::wstring shown = shownValue(*pp, n);
                bool ph = false;
                if (shown.empty())
                    if (auto p = n->attr("placeholder")) {
                        shown = widen(*p);
                        ph = true;
                    }
                auto L = ts.layout(st, shown, false, false);
                float caretX = 0, scroll = 0;
                if (focused && L && !ph) {
                    float cy = 0, ax = 0;
                    DWRITE_HIT_TEST_METRICS m{};
                    L->HitTestTextPosition((UINT32)std::min(fCaret, shown.size()), FALSE, &caretX, &cy, &m);
                    float innerW = ir - il;
                    if (caretX - fScroll > innerW - 2) fScroll = caretX - innerW + 2;
                    if (caretX - fScroll < 0) fScroll = caretX;
                    if (fScroll < 0) fScroll = 0;
                    scroll = fScroll;
                    if (fAnchor != fCaret) {
                        L->HitTestTextPosition((UINT32)std::min(fAnchor, shown.size()), FALSE, &ax, &cy, &m);
                        brush->SetColor(hexc(0xb4d5fe));
                        rt->FillRectangle(D2D1::RectF(il - scroll + std::min(ax, caretX), ty, il - scroll + std::max(ax, caretX), ty + th), brush.Get());
                    }
                }
                if (L) {
                    brush->SetColor(ph ? hexc(0x757575) : colf(st.color));
                    rt->DrawTextLayout(D2D1::Point2F(il - scroll, ty), L.Get(), brush.Get(), opt);
                }
                if (focused) {
                    brush->SetColor(colf(st.color));
                    rt->FillRectangle(D2D1::RectF(il - scroll + caretX, ty, il - scroll + caretX + 1.2f, ty + th), brush.Get());
                }
            }
        }
        rt->PopAxisAlignedClip();
    }

    RectF popupRect(const WidgetArea& w, size_t nOpts) const {
        float h = std::min<size_t>(nOpts, 12) * kRowH + 2;
        return {w.x, w.y + w.h, w.x + std::max(w.w, 140.f), w.y + w.h + h};
    }

    // ------------------------------------------------------------------ drawing
    void text(ID2D1RenderTarget* rt, std::wstring_view s, IDWriteTextFormat* f, RectF r, uint32_t color, float alpha = 1) {
        brush->SetColor(hexc(color, alpha));
        rt->DrawText(s.data(), (UINT32)s.size(), f, r.d(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void fill(ID2D1RenderTarget* rt, RectF r, uint32_t color, float alpha = 1) {
        brush->SetColor(hexc(color, alpha));
        rt->FillRectangle(r.d(), brush.Get());
    }
    void rfill(ID2D1RenderTarget* rt, RectF r, float rad, uint32_t color, float alpha = 1) {
        brush->SetColor(hexc(color, alpha));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r.d(), rad, rad), brush.Get());
    }

    void drawFrame(ID2D1RenderTarget* rt, uint64_t gen) {
        if (!brush || brushGen != gen) {
            brush.Reset();
            rt->CreateSolidColorBrush(hexc(0), &brush);
            brushGen = gen;
        }
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(hexc(0xffffff));
        Tab* t = cur();
        if (t) ensureLayout(*t);
        drawContent(rt, gen);
        drawChrome(rt);
        if (devOpen) drawDev(rt);
    }

    void drawContent(ID2D1RenderTarget* rt, uint64_t gen) {
        RectF cr = contentRect();
        Tab* t = cur();
        rt->PushAxisAlignedClip(cr.d(), D2D1_ANTIALIAS_MODE_ALIASED);
        if (t && t->page) {
            Page& p = *t->page;
            const Color& c = p.lo.canvas;
            brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, 1));
            rt->FillRectangle(cr.d(), brush.Get());
            rt->SetTransform(D2D1::Matrix3x2F::Translation(cr.l, cr.t - t->scroll));
            paintPage = &p;
            paint_items(rt, brush.Get(), p.lo, p.images, t->scroll, t->scroll + cr.h(), gen, this);
            paintPage = nullptr;
            if (popup >= 0 && popup < (int)p.lo.widgets.size()) {
                const WidgetArea& w = p.lo.widgets[popup];
                auto opts = select_options(w.node);
                RectF pr = popupRect(w, opts.size());
                fill(rt, {pr.l + 2, pr.t + 2, pr.r + 2, pr.b + 2}, 0x000000, 0.12f);
                fill(rt, pr, 0xffffff);
                brush->SetColor(hexc(0x8a8d91));
                rt->DrawRectangle(D2D1::RectF(pr.l + .5f, pr.t + .5f, pr.r - .5f, pr.b - .5f), brush.Get(), 1);
                int sel = stateOf(p, w.node).sel;
                for (int row = 0; row < 12 && popupScroll + row < (int)opts.size(); ++row) {
                    int i = popupScroll + row;
                    RectF rr{pr.l + 1, pr.t + 1 + row * kRowH, pr.r - 1, pr.t + 1 + (row + 1) * kRowH};
                    if (i == sel) fill(rt, rr, 0x1a73e8);
                    text(rt, widen(collapse(text_content(opts[i]))), fUi.Get(), {rr.l + 8, rr.t, rr.r - 4, rr.b}, i == sel ? 0xffffff : 0x202124);
                }
            }
            for (Node* hn : {devSel, devHover}) {
                if (!hn || !hn->hasBox || !devOpen) continue;
                brush->SetColor(hexc(0x6fa8dc, hn == devHover ? 0.35f : 0.22f));
                rt->FillRectangle(D2D1::RectF(hn->bx, hn->by, hn->bx + hn->bw, hn->by + hn->bh), brush.Get());
                brush->SetColor(hexc(0x1a73e8, 0.9f));
                rt->DrawRectangle(D2D1::RectF(hn->bx, hn->by, hn->bx + hn->bw, hn->by + hn->bh), brush.Get(), 1.f);
            }
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
            float docH = p.lo.height, viewH = cr.h();
            if (docH > viewH + 1) {
                float th = std::max(30.f, viewH * viewH / docH);
                float ms = docH - viewH;
                float ty = cr.t + (t->scroll / ms) * (viewH - th);
                rfill(rt, {cr.r - kSbW + 2, ty + 2, cr.r - 2, ty + th - 2}, 3, 0x000000, dragSb ? 0.45f : 0.28f);
            }
        }
        if (t && t->loading) fill(rt, {cr.l, cr.t, cr.l + cr.w() * 0.35f, cr.t + 2.5f}, 0x1a73e8);
        if (pendSave) {
            RectF ir = infoRect();
            fill(rt, ir, 0xe8f0fe);
            fill(rt, {ir.l, ir.b - 1, ir.r, ir.b}, 0xb6c7e3);
            std::string host = pendSave->origin.substr(pendSave->origin.find("//") == std::string::npos ? 0 : pendSave->origin.find("//") + 2);
            std::wstring msg = (pendSave->update ? L"Update the saved password for " : L"Save the password for ") + widen(host) +
                               (pendSave->user.empty() ? L"?" : L" (" + widen(pendSave->user) + L")?") + L"  Stored encrypted on this PC.";
            text(rt, L"", fIcon.Get(), {ir.l + 10, ir.t, ir.l + 34, ir.b}, 0x1a73e8);
            text(rt, msg, fUi.Get(), {ir.l + 40, ir.t, infoBtn(1).l - 12, ir.b}, 0x202124);
            rfill(rt, infoBtn(0), 6, 0x1a73e8);
            text(rt, pendSave->update ? L"Update" : L"Save", fUiC.Get(), infoBtn(0), 0xffffff);
            rfill(rt, infoBtn(1), 6, 0xffffff);
            text(rt, L"Not now", fUiC.Get(), infoBtn(1), 0x1a73e8);
        }
        if (!hoverHref.empty()) {
            std::wstring s = widen(hoverHref);
            float w = std::min(cr.w() * 0.6f, ts.width(Style{}, s) * 0.8f + 24);
            RectF r{cr.l, H - 24, cr.l + std::max(120.f, w), H};
            fill(rt, r, 0xf1f3f4);
            fill(rt, {r.l, r.t, r.r, r.t + 1}, 0xdadce0);
            fill(rt, {r.r - 1, r.t, r.r, r.b}, 0xdadce0);
            text(rt, s, fSmall.Get(), {r.l + 8, r.t, r.r - 6, r.b}, 0x3c4043);
        }
        rt->PopAxisAlignedClip();
    }

    void drawChrome(ID2D1RenderTarget* rt) {
        fill(rt, {0, 0, W, kTabH}, 0xdee1e6);
        for (int i = 0; i < (int)tabs.size(); ++i) {
            RectF r = tabRect(i);
            bool act = i == active;
            if (act) {
                rfill(rt, {r.l, r.t, r.r, r.b + 8}, 8, 0xffffff);
            } else if (i + 1 != active) {
                fill(rt, {r.r - 1, r.t + 9, r.r, r.b - 7}, 0x9aa0a6);
            }
            Tab& t = *tabs[i];
            float tl = r.l + 12;
            if (t.loading) {
                brush->SetColor(hexc(0x1a73e8));
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.l + 16, (r.t + r.b) / 2), 4, 4), brush.Get());
                tl += 14;
            }
            std::wstring title = t.loading && !t.page ? L"Loading…" : t.title;
            text(rt, title, fUi.Get(), {tl, r.t, r.r - 30, r.b}, act ? 0x202124 : 0x45494d);
            text(rt, L"", fIconS.Get(), tabClose(i), 0x5f6368);
        }
        text(rt, L"", fIcon.Get(), newTabRect(), 0x3c4043);

        fill(rt, {0, kTabH, W, kTabH + kToolH}, 0xffffff);
        Tab* t = cur();
        bool canBack = t && t->hi > 0, canFwd = t && t->hi + 1 < (int)t->hist.size();
        text(rt, L"", fIcon.Get(), btn(0), canBack ? 0x3c4043 : 0xbdc1c6);
        text(rt, L"", fIcon.Get(), btn(1), canFwd ? 0x3c4043 : 0xbdc1c6);
        text(rt, t && t->loading ? L"" : L"", fIcon.Get(), btn(2), 0x3c4043);

        RectF a = addrRect();
        if (addrFocus) {
            rfill(rt, a, 16, 0x1a73e8);
            rfill(rt, {a.l + 2, a.t + 2, a.r - 2, a.b - 2}, 14, 0xffffff);
        } else {
            rfill(rt, a, 16, 0xf1f3f4);
        }
        std::wstring shown = addrFocus ? addrText : (t ? t->addr : L"");
        float tx = a.l + 16;
        if (!addrFocus && istarts(narrow(shown), "https://")) {
            text(rt, L"", fIconS.Get(), {a.l + 8, a.t, a.l + 26, a.b}, 0x5f6368);
            tx += 12;
        }
        if (shown.empty() && !addrFocus) {
            text(rt, L"Search or type a URL", fUi.Get(), {tx, a.t, a.r - 12, a.b}, 0x80868b);
        } else {
            ComPtr<IDWriteTextLayout> L;
            ts.factory()->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), fUi.Get(), 100000.f, a.h(), &L);
            if (L) {
                float caretX = 0, scrollX = 0;
                if (addrFocus) {
                    float cy;
                    DWRITE_HIT_TEST_METRICS m;
                    L->HitTestTextPosition((UINT32)caret, FALSE, &caretX, &cy, &m);
                    float avail = a.r - 16 - tx;
                    if (caretX > avail) scrollX = caretX - avail;
                }
                rt->PushAxisAlignedClip(D2D1::RectF(tx - 2, a.t, a.r - 12, a.b), D2D1_ANTIALIAS_MODE_ALIASED);
                if (addrFocus && addrSel && !shown.empty()) {
                    DWRITE_TEXT_METRICS tm;
                    L->GetMetrics(&tm);
                    fill(rt, {tx - scrollX, a.t + 8, tx - scrollX + tm.widthIncludingTrailingWhitespace, a.b - 8}, 0xc7dbfb);
                }
                brush->SetColor(hexc(addrFocus ? 0x202124 : 0x3c4043));
                rt->DrawTextLayout(D2D1::Point2F(tx - scrollX, a.t), L.Get(), brush.Get());
                if (addrFocus && !addrSel) fill(rt, {tx - scrollX + caretX, a.t + 8, tx - scrollX + caretX + 1.2f, a.b - 8}, 0x202124);
                rt->PopAxisAlignedClip();
            }
        }
        bool marked = t && t->page && bm.find(t->page->url) >= 0;
        text(rt, marked ? L"" : L"", fIcon.Get(), rbtn(2), marked ? 0x1a73e8 : 0x3c4043);
        text(rt, L"", fIcon.Get(), rbtn(1), 0x3c4043);
        if (devOpen) rfill(rt, rbtn(0), 6, 0xe8f0fe);
        text(rt, L"", fIcon.Get(), rbtn(0), devOpen ? 0x1a73e8 : 0x3c4043);

        float by = kTabH + kToolH;
        fill(rt, {0, by, W, by + kBmH}, 0xffffff);
        fill(rt, {0, by + kBmH - 1, W, by + kBmH}, 0xdadce0);
        chips.clear();
        float x = 10;
        if (bm.items.empty()) {
            text(rt, L"Bookmarks appear here. Press Ctrl+D or click the star to add one.", fSmall.Get(), {x, by, W - 10, by + kBmH - 1}, 0x80868b);
        }
        for (size_t i = 0; i < bm.items.size(); ++i) {
            std::wstring s = widen(bm.items[i].title);
            ComPtr<IDWriteTextLayout> L;
            ts.factory()->CreateTextLayout(s.c_str(), (UINT32)s.size(), fSmall.Get(), 1000.f, 20.f, &L);
            DWRITE_TEXT_METRICS m{};
            if (L) L->GetMetrics(&m);
            float w = std::min(170.f, m.widthIncludingTrailingWhitespace + 22);
            if (x + w > W - 10) break;
            RectF r{x, by + 3, x + w, by + kBmH - 4};
            chips.push_back({r, (int)i});
            text(rt, L"", fIconS.Get(), {r.l + 2, r.t, r.l + 16, r.b}, 0x5f6368);
            text(rt, s, fSmall.Get(), {r.l + 17, r.t, r.r - 4, r.b}, 0x3c4043);
            x += w + 4;
        }
    }

    // ------------------------------------------------------------------ devtools console
    static constexpr float kConInputH = 28, kMonoAdvance = 6.6f;

    void conAdd(int kind, const std::wstring& text) {
        conLines.push_back({kind, text.size() > 20000 ? text.substr(0, 20000) + L"..." : text});
        if (conLines.size() > 1000) conLines.erase(conLines.begin(), conLines.begin() + 200);
        devScroll = 0;
        invalidate();
    }
    void conRun() {
        std::wstring src = conInput;
        if (src.find_first_not_of(L" \t\r\n") == std::wstring::npos) return;
        if (conHist.empty() || conHist.back() != src) conHist.push_back(src);
        conHistPos = -1;
        conInput.clear();
        conCaret = 0;
        conAdd(4, src);
        if (Tab* t = cur(); t && t->page && t->page->script) {
            std::wstring out;
            bool ok = t->page->script->eval(src, out);
            conAdd(ok ? 5 : 3, out);
            afterScript(*t);
            return;
        }
        if (!conVm) {
            conVm = std::make_unique<js::Vm>();
            conVm->onConsole = [this](int level, const std::u16string& s) { conAdd(level, std::wstring(s.begin(), s.end())); };
        }
        conVm->terminated = false;
        conVm->deadlineTick = GetTickCount64() + 5000;  // a runaway loop must not freeze the window
        std::u16string result, error;
        bool ok = conVm->run(std::u16string(src.begin(), src.end()), "console", &result, &error);
        conVm->deadlineTick = 0;
        const std::u16string& shown = ok ? result : error;
        conAdd(ok ? 5 : 3, std::wstring(shown.begin(), shown.end()));
    }
    void conChar(wchar_t ch) {
        switch (ch) {
            case 13: conRun(); break;
            case 8:
                if (conCaret > 0) conInput.erase(--conCaret, 1);
                break;
            case 3: setClipboard(conInput); break;
            case 22: {
                std::wstring clip = clipboard();
                conInput.insert(conCaret, clip);
                conCaret += clip.size();
                break;
            }
            case 12:  // Ctrl+L clears the log
                conLines.clear();
                break;
            case 27: conFocus = false; break;
            default:
                if (ch >= 32) conInput.insert(conCaret++, 1, ch);
        }
        invalidate();
    }
    bool conKey(WPARAM vk) {
        switch (vk) {
            case VK_LEFT:
                if (conCaret > 0) --conCaret;
                break;
            case VK_RIGHT:
                if (conCaret < conInput.size()) ++conCaret;
                break;
            case VK_HOME: conCaret = 0; break;
            case VK_END: conCaret = conInput.size(); break;
            case VK_DELETE:
                if (conCaret < conInput.size()) conInput.erase(conCaret, 1);
                break;
            case VK_UP:
            case VK_DOWN: {
                if (conHist.empty()) break;
                int n = (int)conHist.size();
                if (vk == VK_UP) conHistPos = conHistPos < 0 ? n - 1 : std::max(0, conHistPos - 1);
                else conHistPos = conHistPos < 0 || conHistPos + 1 >= n ? -1 : conHistPos + 1;
                conInput = conHistPos < 0 ? L"" : conHist[conHistPos];
                conCaret = conInput.size();
                break;
            }
            default: return false;
        }
        invalidate();
        return true;
    }
    void drawConsole(ID2D1RenderTarget* rt, RectF body) {
        RectF in{body.l, body.b - kConInputH, body.r, body.b};
        int cols = std::max(20, (int)((body.w() - 34) / kMonoAdvance));
        rt->PushAxisAlignedClip(D2D1::RectF(body.l, body.t, body.r, in.t), D2D1_ANTIALIAS_MODE_ALIASED);
        float y = in.t - 4 + devScroll;
        if (conLines.empty())
            text(rt, L"Type JavaScript below and press Enter. Pages are not connected to it yet.", fUi.Get(), {body.l + 12, body.t + 8, body.r - 8, body.t + 30}, 0x6e7781);
        for (size_t li = conLines.size(); li-- > 0 && y > body.t;) {
            const ConLine& L = conLines[li];
            // break the entry into rows: at line ends, and wherever a row would be wider than the panel
            std::vector<std::wstring_view> rows;
            std::wstring_view rest = L.text;
            for (;;) {
                size_t nl = rest.find(L'\n');
                std::wstring_view line = rest.substr(0, nl);
                if (line.empty()) rows.push_back(line);
                for (size_t at = 0; at < line.size(); at += cols) rows.push_back(line.substr(at, cols));
                if (nl == std::wstring_view::npos || rows.size() > 400) break;
                rest = rest.substr(nl + 1);
            }
            float h = rows.size() * kLine + 4, top = y - h;
            uint32_t ink = L.kind == 2 ? 0x7a4b00 : L.kind == 3 ? 0xc5221f : L.kind == 4 ? 0x1a56c4 : L.kind == 5 ? 0x5f6368 : 0x1f2328;
            if (L.kind == 2) fill(rt, {body.l, top, body.r, y}, 0xfff8e1);
            if (L.kind == 3) fill(rt, {body.l, top, body.r, y}, 0xfdecea);
            fill(rt, {body.l, y - 1, body.r, y}, 0xf0f0f0);
            if (L.kind >= 4) text(rt, L.kind == 4 ? L">" : L"<", fMono.Get(), {body.l + 8, top + 2, body.l + 22, top + 2 + kLine}, L.kind == 4 ? 0x1a56c4 : 0x9aa0a6);
            for (size_t r = 0; r < rows.size(); ++r) {
                float ry = top + 2 + r * kLine;
                if (ry + kLine > body.t && ry < in.t) text(rt, rows[r], fMono.Get(), {body.l + 24, ry, body.r - 4, ry + kLine}, ink);
            }
            y = top;
        }
        rt->PopAxisAlignedClip();
        fill(rt, in, 0xffffff);
        fill(rt, {in.l, in.t, in.r, in.t + 1}, conFocus ? 0x1a73e8 : 0xd0d0d0);
        text(rt, L">", fMono.Get(), {in.l + 8, in.t, in.l + 22, in.b}, 0x1a73e8);
        // one visible row of input; long input scrolls so the caret stays in view
        size_t start = conCaret > (size_t)cols - 2 ? conCaret - (cols - 2) : 0;
        std::wstring shown = conInput.substr(start, cols);
        for (auto& c : shown)
            if (c == L'\n' || c == L'\r' || c == L'\t') c = L' ';
        if (shown.empty() && !conFocus) text(rt, L"click here, type an expression, press Enter", fMono.Get(), {in.l + 24, in.t, in.r - 4, in.b}, 0x9aa0a6);
        text(rt, shown, fMono.Get(), {in.l + 24, in.t, in.r - 4, in.b}, 0x1f2328);
        if (conFocus) {
            float cx = in.l + 24 + (conCaret - start) * kMonoAdvance;
            fill(rt, {cx, in.t + 6, cx + 1.2f, in.b - 6}, 0x202124);
        }
    }

    void drawDev(ID2D1RenderTarget* rt) {
        RectF p = devRect();
        fill(rt, p, 0xffffff);
        fill(rt, {p.l, p.t, p.l + 1, p.b}, 0xcbcdd1);
        const wchar_t* names[] = {L"Elements", L"Network", L"Performance", L"Console"};
        fill(rt, {p.l + 1, p.t, p.r, p.t + 30}, 0xf3f3f3);
        fill(rt, {p.l + 1, p.t + 29, p.r, p.t + 30}, 0xd0d0d0);
        for (int i = 0; i < 4; ++i) {
            RectF r{p.l + 8 + i * 96.f, p.t, p.l + 8 + i * 96.f + 92, p.t + 30};
            text(rt, names[i], fUiC.Get(), r, devTab == i ? 0x1a73e8 : 0x45494d);
            if (devTab == i) fill(rt, {r.l + 6, r.b - 3, r.r - 6, r.b - 1}, 0x1a73e8);
        }
        RectF body{p.l + 1, p.t + 30, p.r, p.b};
        if (devTab == 3) {
            drawConsole(rt, body);
            return;
        }
        Tab* t = cur();
        Page* pg = t ? t->page.get() : nullptr;
        if (!pg) {
            text(rt, L"No page loaded.", fUi.Get(), {body.l + 12, body.t + 8, body.r, body.t + 30}, 0x6e7781);
            return;
        }
        if (devTab == 0) {
            float treeB = body.t + body.h() * 0.58f;
            rt->PushAxisAlignedClip(D2D1::RectF(body.l, body.t, body.r, treeB), D2D1_ANTIALIAS_MODE_ALIASED);
            size_t first = (size_t)std::max(0.f, devScroll / kLine);
            for (size_t i = first; i < pg->devLines.size(); ++i) {
                float y = body.t + 4 + i * kLine - devScroll;
                if (y > treeB) break;
                auto& L = pg->devLines[i];
                RectF row{body.l, y, body.r, y + kLine};
                if (L.node && L.node == devSel && L.kind == 0) fill(rt, row, 0xcfe3fc);
                else if (L.node && L.node == devHover && L.kind == 0) fill(rt, row, 0xeef3fb);
                uint32_t col = L.kind == 0 ? 0x881280 : L.kind == 1 ? 0x1f2328 : 0x236e25;
                text(rt, L.text, fMono.Get(), {body.l + 8 + L.depth * 14.f, y, body.r - 4, y + kLine}, col);
            }
            rt->PopAxisAlignedClip();
            fill(rt, {body.l, treeB, body.r, treeB + 1}, 0xd0d0d0);
            fill(rt, {body.l, treeB + 1, body.r, treeB + 26}, 0xf3f3f3);
            Node* n = devSel ? devSel : devHover;
            std::wstring hdr = L"Computed";
            if (n) hdr += L"  —  <" + widen(n->name) + (n->id.empty() ? L"" : L"#" + widen(n->id)) + L">";
            text(rt, hdr, fBold.Get(), {body.l + 10, treeB + 1, body.r, treeB + 26}, 0x3c4043);
            if (!n || !n->style) {
                text(rt, n ? L"(not rendered)" : L"Hover or click an element above.", fUi.Get(), {body.l + 10, treeB + 30, body.r, treeB + 50}, 0x6e7781);
                return;
            }
            const Style& s = *n->style;
            char buf[256];
            std::vector<std::pair<std::string, std::string>> rows;
            auto len = [&](const Len& l) -> std::string {
                if (l.u == Len::Auto) return "auto";
                if (l.u == Len::None) return "none";
                snprintf(buf, sizeof buf, l.u == Len::Pct ? "%.4g%%" : "%.4gpx", l.v);
                return buf;
            };
            auto four = [&](const Len* v) { return len(v[0]) + " " + len(v[1]) + " " + len(v[2]) + " " + len(v[3]); };
            if (n->hasBox) {
                snprintf(buf, sizeof buf, "%.1f \xC3\x97 %.1f at (%.1f, %.1f)", n->bw, n->bh, n->bx, n->by);
                rows.push_back({"box", buf});
            }
            rows.push_back({"display", display_name(s.display)});
            static const char* posn[] = {"static", "relative", "absolute", "fixed", "sticky"};
            rows.push_back({"position", posn[std::min<int>(s.position, 4)]});
            rows.push_back({"color", color_str(s.color)});
            rows.push_back({"background-color", color_str(s.bg)});
            snprintf(buf, sizeof buf, "%.4gpx %s, weight %d%s", s.fontSize, narrow(s.family).c_str(), s.weight, s.italic ? ", italic" : "");
            rows.push_back({"font", buf});
            snprintf(buf, sizeof buf, "%.4gpx", s.lh());
            rows.push_back({"line-height", buf});
            rows.push_back({"margin", four(s.margin)});
            rows.push_back({"padding", four(s.padding)});
            snprintf(buf, sizeof buf, "%.3g %.3g %.3g %.3g  %s  radius %.3g", s.border[0], s.border[1], s.border[2], s.border[3],
                     color_str(s.borderColor[0]).c_str(), s.radius);
            rows.push_back({"border", buf});
            rows.push_back({"width / height", len(s.width) + " / " + len(s.height)});
            rows.push_back({"max-width", len(s.maxWidth)});
            if (s.display == Display::Flex || s.display == Display::InlineFlex) {
                static const char* dirn[] = {"row", "row-reverse", "column", "column-reverse"};
                static const char* jn[] = {"flex-start", "center", "flex-end", "space-between", "space-around", "space-evenly"};
                static const char* an[] = {"stretch", "flex-start", "center", "flex-end"};
                snprintf(buf, sizeof buf, "%s %s, justify %s, align %s, gap %.3g", dirn[s.flexDir & 3], s.flexWrap ? "wrap" : "nowrap",
                         jn[std::min<int>(s.justify, 5)], an[std::min<int>(s.alignItems, 3)], s.gapCol);
                rows.push_back({"flex container", buf});
            }
            snprintf(buf, sizeof buf, "%.3g %.3g %s", s.flexGrow, s.flexShrink, len(s.flexBasis).c_str());
            rows.push_back({"flex", buf});
            static const char* ta[] = {"left", "right", "center"};
            rows.push_back({"text-align", ta[std::min<int>(s.textAlign, 2)]});
            if (s.vars) rows.push_back({"custom properties", std::to_string(s.vars->size())});
            float y = treeB + 30;
            for (auto& [k, v] : rows) {
                if (y > body.b - 18) break;
                text(rt, widen(k), fMono.Get(), {body.l + 10, y, body.l + 150, y + kLine}, 0xc80000);
                text(rt, widen(v), fMono.Get(), {body.l + 154, y, body.r - 6, y + kLine}, 0x1f2328);
                y += kLine;
            }
        } else if (devTab == 1) {
            float y = body.t + 6;
            text(rt, L"Status", fBold.Get(), {body.l + 8, y, body.l + 60, y + kLine}, 0x45494d);
            text(rt, L"Size", fBold.Get(), {body.l + 60, y, body.l + 120, y + kLine}, 0x45494d);
            text(rt, L"Time", fBold.Get(), {body.l + 120, y, body.l + 176, y + kLine}, 0x45494d);
            text(rt, L"URL", fBold.Get(), {body.l + 180, y, body.r, y + kLine}, 0x45494d);
            y += kLine + 4;
            rt->PushAxisAlignedClip(D2D1::RectF(body.l, y, body.r, body.b), D2D1_ANTIALIAS_MODE_ALIASED);
            float y0 = y - devScroll;
            char buf[64];
            for (auto& e : pg->net) {
                if (y0 > body.b) break;
                if (y0 + kLine >= y) {
                    bool bad = !e.error.empty() || e.status >= 400;
                    std::string st = e.error.empty() ? std::to_string(e.status) : "ERR";
                    text(rt, widen(st), fMono.Get(), {body.l + 8, y0, body.l + 60, y0 + kLine}, bad ? 0xc5221f : 0x1a7f37);
                    snprintf(buf, sizeof buf, e.bytes >= 10240 ? "%.0f KB" : "%.1f KB", e.bytes / 1024.0);
                    text(rt, widen(buf), fMono.Get(), {body.l + 60, y0, body.l + 120, y0 + kLine}, 0x3c4043);
                    snprintf(buf, sizeof buf, "%.0f ms", e.ms);
                    text(rt, widen(buf), fMono.Get(), {body.l + 120, y0, body.l + 176, y0 + kLine}, 0x3c4043);
                    std::string u = e.url.size() > 300 ? e.url.substr(0, 300) : e.url;
                    if (!e.error.empty()) u += "  (" + e.error + ")";
                    text(rt, widen(u), fMono.Get(), {body.l + 180, y0, body.r - 4, y0 + kLine}, 0x1f2328);
                }
                y0 += kLine;
            }
            rt->PopAxisAlignedClip();
        } else {
            PROCESS_MEMORY_COUNTERS pmc{};
            pmc.cb = sizeof pmc;
            GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
            size_t bytes = 0;
            for (auto& e : pg->net) bytes += e.bytes;
            char buf[256];
            std::vector<std::string> L;
            snprintf(buf, sizeof buf, "Fetch document      %8.1f ms", pg->tFetch); L.push_back(buf);
            snprintf(buf, sizeof buf, "Parse HTML          %8.1f ms   (%zu nodes)", pg->tParse, pg->doc->nodeCount); L.push_back(buf);
            snprintf(buf, sizeof buf, "Subresources        %8.1f ms   (%zu requests, %.0f KB)", pg->tSub, pg->net.size(), bytes / 1024.0); L.push_back(buf);
            snprintf(buf, sizeof buf, "Style (cascade)     %8.1f ms   (%zu rules)", pg->tStyle, pg->rules); L.push_back(buf);
            snprintf(buf, sizeof buf, "Layout              %8.1f ms   (%zu display items)", pg->tLayout, pg->lo.items.size()); L.push_back(buf);
            snprintf(buf, sizeof buf, "Last frame (GPU)    %8.2f ms", lastFrameMs); L.push_back(buf);
            L.push_back("");
            snprintf(buf, sizeof buf, "Document height     %8.0f px", pg->lo.height); L.push_back(buf);
            snprintf(buf, sizeof buf, "Images decoded      %8zu", pg->images.size()); L.push_back(buf);
            snprintf(buf, sizeof buf, "Form controls       %8zu", pg->lo.widgets.size()); L.push_back(buf);
            snprintf(buf, sizeof buf, "Cookies stored      %8zu", CookieJar::get().count()); L.push_back(buf);
            snprintf(buf, sizeof buf, "Word-width cache    %8zu entries", ts.cacheSize()); L.push_back(buf);
            snprintf(buf, sizeof buf, "Process memory      %8.1f MB (working set)", pmc.WorkingSetSize / 1048576.0); L.push_back(buf);
            L.push_back("");
            std::string gpuLine = headless ? "Renderer            Direct2D (offscreen WIC target)"
                                           : std::string("Renderer            Direct2D 1.1 on D3D11 ") + (gpu.warp ? "(WARP software)" : "(hardware)");
            L.push_back(gpuLine);
            if (!headless && !gpu.adapter.empty()) L.push_back("Adapter             " + narrow(gpu.adapter));
            float y = body.t + 10;
            for (auto& s : L) {
                text(rt, widen(s), fMono.Get(), {body.l + 12, y, body.r - 6, y + kLine}, 0x1f2328);
                y += kLine + 2;
            }
        }
    }

    void paint() {
        if (W < 1 || H < 1) return;
        double t0 = now_ms();
        ID2D1DeviceContext* dc = gpu.begin();
        if (!dc) return;
        drawFrame(dc, gpu.gen);
        gpu.end();
        lastFrameMs = now_ms() - t0;
    }

    // ------------------------------------------------------------------ hit testing
    bool toDoc(float x, float y, float& dx, float& dy) {
        Tab* t = cur();
        RectF cr = contentRect();
        if (!t || !t->page || !cr.hit(x, y) || x > cr.r - kSbW) return false;
        dx = x - cr.l;
        dy = y - cr.t + t->scroll;
        return true;
    }

    Node* linkAt(float x, float y) {
        float dx, dy;
        if (!toDoc(x, y, dx, dy)) return nullptr;
        auto& links = cur()->page->lo.links;
        for (size_t i = links.size(); i-- > 0;) {
            auto& l = links[i];
            if (dx >= l.x && dx < l.x + l.w && dy >= l.y && dy < l.y + l.h) return l.a;
        }
        return nullptr;
    }

    std::string hrefOf(Node* a) {
        Tab* t = cur();
        if (!a || !t || !t->page) return {};
        auto h = a->attr("href");
        if (!h) return {};
        return resolve_url(t->page->doc->baseUrl, *h);
    }

    int devLineAt(float y) {
        RectF p = devRect();
        Tab* t = cur();
        if (!t || !t->page) return -1;
        float top = p.t + 30 + 4, treeB = p.t + 30 + (p.h() - 30) * 0.58f;
        if (y < top || y > treeB) return -1;
        int i = (int)((y - top + devScroll) / kLine);
        return i < (int)t->page->devLines.size() ? i : -1;
    }

    size_t addrHit(float x) {
        RectF a = addrRect();
        float tx = a.l + 16;
        ComPtr<IDWriteTextLayout> L;
        ts.factory()->CreateTextLayout(addrText.c_str(), (UINT32)addrText.size(), fUi.Get(), 100000.f, a.h(), &L);
        if (!L) return addrText.size();
        float caretX, cy;
        DWRITE_HIT_TEST_METRICS m;
        L->HitTestTextPosition((UINT32)caret, FALSE, &caretX, &cy, &m);
        float avail = a.r - 16 - tx, scrollX = caretX > avail ? caretX - avail : 0;
        BOOL trailing, inside;
        L->HitTestPoint(x - tx + scrollX, a.h() / 2, &trailing, &inside, &m);
        return std::min(addrText.size(), (size_t)m.textPosition + (trailing ? 1 : 0));
    }

    void focusAddr() {
        Tab* t = cur();
        addrFocus = true;
        conFocus = false;
        addrSel = true;
        focus = nullptr;
        popup = -1;
        addrText = t ? t->addr : L"";
        caret = addrText.size();
        invalidate();
    }

    void scrollBy(float dy) {
        Tab* t = cur();
        if (!t) return;
        t->scroll += dy;
        clampScroll(*t);
        invalidate();
    }

    void toggleBookmark() {
        Tab* t = cur();
        if (!t || !t->page || istarts(t->page->url, "about:")) return;
        bm.toggle(t->page->url, narrow(t->title));
        invalidate();
    }

    // ------------------------------------------------------------------ input
    void onMouseDown(float x, float y, bool middle, bool ctrl) {
        bool wasFocus = addrFocus;
        if (!addrRect().hit(x, y)) addrFocus = false;
        conFocus = false;
        if (y < kTabH) {
            for (int i = 0; i < (int)tabs.size(); ++i) {
                if (tabClose(i).hit(x, y) || (middle && tabRect(i).hit(x, y))) {
                    closeTab(i);
                    return;
                }
                if (tabRect(i).hit(x, y)) {
                    activate(i);
                    return;
                }
            }
            if (newTabRect().hit(x, y)) {
                newTab("about:home");
                focusAddr();
                addrText.clear();
                caret = 0;
            }
            invalidate();
            return;
        }
        if (y < kTabH + kToolH) {
            if (btn(0).hit(x, y)) goBack();
            else if (btn(1).hit(x, y)) goFwd();
            else if (btn(2).hit(x, y)) {
                Tab* t = cur();
                if (t && t->loading) {
                    t->gen++;
                    t->loading = false;
                } else
                    reload();
            } else if (addrRect().hit(x, y)) {
                if (!wasFocus) focusAddr();
                else {
                    addrSel = false;
                    caret = addrHit(x);
                }
            } else if (rbtn(2).hit(x, y)) toggleBookmark();
            else if (rbtn(1).hit(x, y)) {
                if (Tab* t = cur()) navigate(*t, "about:bookmarks");
            } else if (rbtn(0).hit(x, y)) {
                devOpen = !devOpen;
            }
            invalidate();
            return;
        }
        if (y < chromeH()) {
            for (auto& [r, i] : chips)
                if (r.hit(x, y) && i < (int)bm.items.size()) {
                    if (middle || ctrl) newTab(bm.items[i].url, false);
                    else if (Tab* t = cur()) navigate(*t, bm.items[i].url);
                    break;
                }
            invalidate();
            return;
        }
        if (devOpen && devRect().hit(x, y)) {
            RectF p = devRect();
            if (y < p.t + 30) {
                int i = (int)((x - p.l - 8) / 96);
                if (i >= 0 && i < 4) {
                    devTab = i;
                    devScroll = 0;
                }
            }
            if (devTab == 3) {
                conFocus = true;
                focus = nullptr;
                popup = -1;
            } else if (y < p.t + 30) {
            } else if (devTab == 0) {
                int i = devLineAt(y);
                Tab* t = cur();
                if (i >= 0 && t) devSel = t->page->devLines[i].node;
            }
            invalidate();
            return;
        }
        if (pendSave && infoRect().hit(x, y)) {
            if (infoBtn(0).hit(x, y)) answerSave(true);
            else if (infoBtn(1).hit(x, y)) answerSave(false);
            return;
        }
        RectF cr = contentRect();
        Tab* t = cur();
        if (t && t->page && x >= cr.r - kSbW && x < cr.r) {
            float viewH = cr.h(), docH = t->page->lo.height;
            if (docH > viewH) {
                float th = std::max(30.f, viewH * viewH / docH), ms = docH - viewH;
                float ty = cr.t + (t->scroll / ms) * (viewH - th);
                if (y < ty || y > ty + th) {
                    t->scroll = (y - cr.t - th / 2) / (viewH - th) * ms;
                    clampScroll(*t);
                    ty = cr.t + (t->scroll / ms) * (viewH - th);
                }
                dragSb = true;
                dragOff = y - ty;
                if (hwnd) SetCapture(hwnd);
            }
            invalidate();
            return;
        }
        float dx, dy;
        if (!toDoc(x, y, dx, dy)) {
            invalidate();
            return;
        }
        Page& p = *t->page;
        if (popup >= 0 && popup < (int)p.lo.widgets.size()) {
            const WidgetArea& w = p.lo.widgets[popup];
            auto opts = select_options(w.node);
            RectF pr = popupRect(w, opts.size());
            int was = popup;
            popup = -1;
            if (pr.hit(dx, dy)) {
                int i = popupScroll + (int)((dy - pr.t - 1) / kRowH);
                if (i >= 0 && i < (int)opts.size()) {
                    Node* chosen = w.node;
                    stateOf(p, chosen).sel = i;
                    fireEvent(*t, chosen, "input", true, false);
                    fireEvent(*t, chosen, "change", true, false);
                }
                invalidate();
                return;
            }
            if (widgetAt(dx, dy) == was) {
                invalidate();
                return;
            }
        }
        int wi = middle ? -1 : widgetAt(dx, dy);
        if (p.script && !middle) {
            Node* widgetNode = wi >= 0 ? p.lo.widgets[wi].node : nullptr;
            Node* linkNode = linkAt(x, y);
            Node* target = widgetNode ? widgetNode : nodeAt(p, dx, dy);
            if (linkNode && !widgetNode) {
                bool inside = false;
                for (Node* n = target; n; n = n->parent) inside |= n == linkNode;
                if (!inside) target = linkNode;
            }
            bool go = fireEvent(*t, target, "click");
            if (t->page.get() != &p || t->loading) return;  // the script navigated away
            if (!go) {
                invalidate();
                return;
            }
            ensureLayout(*t);
            wi = widgetAt(dx, dy);
            if ((wi >= 0 ? p.lo.widgets[wi].node : nullptr) != widgetNode || linkAt(x, y) != linkNode) {
                invalidate();  // the page under the pointer changed: no default action
                return;
            }
        }
        if (wi >= 0) {
            activateWidget(wi, dx, dy);
            return;
        }
        focus = nullptr;
        if (Node* a = linkAt(x, y)) {
            std::string href = hrefOf(a);
            if (!href.empty()) {
                auto target = a->attr("target");
                if (middle || ctrl || (target && *target == "_blank")) newTab(href, !(middle || ctrl));
                else navigate(*t, href, true, nullptr, true);
            }
        }
        invalidate();
    }

    void onMouseMove(float x, float y) {
        Tab* t = cur();
        if (dragSb && t && t->page) {
            RectF cr = contentRect();
            float viewH = cr.h(), docH = t->page->lo.height;
            float th = std::max(30.f, viewH * viewH / docH), ms = std::max(1.f, docH - viewH);
            t->scroll = (y - dragOff - cr.t) / std::max(1.f, viewH - th) * ms;
            clampScroll(*t);
            invalidate();
            return;
        }
        Node* l = linkAt(x, y);
        Node* dh = devHover;
        if (devOpen && devTab == 0 && devRect().hit(x, y)) {
            int i = devLineAt(y);
            devHover = (i >= 0 && t) ? t->page->devLines[i].node : nullptr;
        } else if (devOpen) {
            devHover = nullptr;
        }
        float dx, dy;
        hoverText = hoverClick = false;
        if (toDoc(x, y, dx, dy)) {
            int wi = widgetAt(dx, dy);
            if (wi >= 0) {
                WidgetKind k = widget_kind(t->page->lo.widgets[wi].node);
                hoverText = k == WidgetKind::Text || k == WidgetKind::Password || k == WidgetKind::TextArea;
                hoverClick = !hoverText;
                l = nullptr;
            }
        }
        if (l != hoverLink || dh != devHover) {
            hoverLink = l;
            hoverHref = hrefOf(l);
            invalidate();
        }
    }

    void onWheel(float x, float y, int delta) {
        if (devOpen && devRect().hit(x, y)) {
            devScroll = std::max(0.f, devScroll + (devTab == 3 ? 1 : -1) * delta / 120.f * kLine * 3);
            invalidate();
            return;
        }
        Page* p = curPage();
        if (p && popup >= 0 && popup < (int)p->lo.widgets.size()) {
            int n = (int)select_options(p->lo.widgets[popup].node).size();
            popupScroll = std::clamp(popupScroll - delta / 120 * 3, 0, std::max(0, n - 12));
            invalidate();
            return;
        }
        scrollBy(-delta / 120.f * 100.f);
    }

    std::wstring clipboard() {
        std::wstring r;
        if (OpenClipboard(hwnd)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto p = (const wchar_t*)GlobalLock(h)) {
                    r = p;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
        return r;
    }
    void setClipboard(const std::wstring& s) {
        if (!OpenClipboard(hwnd)) return;
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
        if (g) {
            memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
        }
        CloseClipboard();
    }
    static std::wstring oneLine(std::wstring s) {
        for (auto& c : s)
            if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        return s;
    }

    void addrInsert(const std::wstring& s) {
        if (addrSel) {
            addrText.clear();
            caret = 0;
            addrSel = false;
        }
        addrText.insert(caret, s);
        caret += s.size();
    }

    void moveFocus(int dir) {
        Page* p = curPage();
        if (!p || p->lo.widgets.empty()) return;
        int n = (int)p->lo.widgets.size();
        int i = widgetIndex(focus);
        for (int step = 0; step < n; ++step) {
            i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
            Node* c = p->lo.widgets[i].node;
            if (!c->attr("disabled") && !p->lo.widgets[i].st->hidden) {
                focusWidget(c, true);
                break;
            }
        }
        invalidate();
    }

    void onFormChar(wchar_t ch) {
        Page* p = curPage();
        if (!p || !focus) return;
        WidgetKind k = widget_kind(focus);
        std::wstring* v = focusText();
        if (!v) {
            if (ch == L' ' || ch == 13) {
                int wi = widgetIndex(focus);
                if (wi >= 0) activateWidget(wi, -1, -1);
            }
            return;
        }
        bool secret = k == WidgetKind::Password;
        size_t a = std::min(fAnchor, fCaret), b = std::max(fAnchor, fCaret);
        switch (ch) {
            case 1:  // Ctrl+A
                fAnchor = 0;
                fCaret = v->size();
                break;
            case 3:  // Ctrl+C
                if (!secret && a != b) setClipboard(v->substr(a, b - a));
                break;
            case 24:  // Ctrl+X
                if (!secret && a != b) {
                    setClipboard(v->substr(a, b - a));
                    deleteSelection(*v);
                }
                break;
            case 22: insertText(k == WidgetKind::TextArea ? clipboard() : oneLine(clipboard())); break;
            case 8:
                if (!deleteSelection(*v) && fCaret > 0) {
                    v->erase(--fCaret, 1);
                    fAnchor = fCaret;
                }
                break;
            case 13:
                if (k == WidgetKind::TextArea) insertText(L"\n");
                else submitForm(focus, nullptr);
                break;
            case 27:
                focus = nullptr;
                break;
            case 9: break;
            default:
                if (ch >= 32) insertText(std::wstring(1, ch));
        }
        invalidate();
    }

    void onChar(wchar_t ch) {
        if (conFocus && devOpen && devTab == 3) {
            conChar(ch);
            return;
        }
        if (!addrFocus) {
            Node* typedInto = focus;
            Tab* typedTab = cur();
            onFormChar(ch);
            if (typedInto && typedTab && typedTab == cur() && focus == typedInto && ch != 13 && ch != 27 && ch != 9 && ch != 3)
                fireEvent(*typedTab, typedInto, "input", true, false);
            return;
        }
        switch (ch) {
            case 1: addrSel = true; break;  // Ctrl+A
            case 3: setClipboard(addrText); break;
            case 24:
                setClipboard(addrText);
                addrText.clear();
                caret = 0;
                addrSel = false;
                break;
            case 22: addrInsert(oneLine(clipboard())); break;
            case 8:
                if (addrSel) {
                    addrText.clear();
                    caret = 0;
                    addrSel = false;
                } else if (caret > 0) {
                    addrText.erase(--caret, 1);
                }
                break;
            case 13: {
                addrFocus = false;
                if (Tab* t = cur()) navigate(*t, fixup(addrText));
                break;
            }
            case 27: addrFocus = false; break;
            default:
                if (ch >= 32) addrInsert(std::wstring(1, ch));
        }
        invalidate();
    }

    bool onFormKey(WPARAM vk, bool shift, bool ctrl) {
        Page* p = curPage();
        if (!p || !focus) return false;
        WidgetKind k = widget_kind(focus);
        if (k == WidgetKind::Select) {
            FormState& s = stateOf(*p, focus);
            int n = (int)select_options(focus).size();
            if (vk == VK_DOWN || vk == VK_UP) {
                s.sel = std::clamp(s.sel + (vk == VK_DOWN ? 1 : -1), 0, std::max(0, n - 1));
                invalidate();
                return true;
            }
            return false;
        }
        std::wstring* v = focusText();
        if (!v) return false;
        auto isWord = [](wchar_t c) { return iswalnum(c) != 0; };
        switch (vk) {
            case VK_LEFT:
                if (!shift && fAnchor != fCaret) fCaret = std::min(fAnchor, fCaret);
                else if (fCaret > 0) {
                    --fCaret;
                    if (ctrl)
                        while (fCaret > 0 && isWord((*v)[fCaret - 1])) --fCaret;
                }
                break;
            case VK_RIGHT:
                if (!shift && fAnchor != fCaret) fCaret = std::max(fAnchor, fCaret);
                else if (fCaret < v->size()) {
                    ++fCaret;
                    if (ctrl)
                        while (fCaret < v->size() && isWord((*v)[fCaret])) ++fCaret;
                }
                break;
            case VK_HOME: fCaret = 0; break;
            case VK_END: fCaret = v->size(); break;
            case VK_DELETE:
                if (!deleteSelection(*v) && fCaret < v->size()) v->erase(fCaret, 1);
                fAnchor = fCaret;
                invalidate();
                return true;
            default: return false;
        }
        if (!shift) fAnchor = fCaret;
        invalidate();
        return true;
    }

    bool onKey(WPARAM vk, bool alt) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        Tab* t = cur();
        if (alt) {
            if (vk == VK_LEFT) goBack();
            else if (vk == VK_RIGHT) goFwd();
            else if (vk == 'D') focusAddr();
            else return false;
            return true;
        }
        if (ctrl) {
            switch (vk) {
                case 'T':
                    newTab("about:home");
                    focusAddr();
                    addrText.clear();
                    caret = 0;
                    return true;
                case 'W':
                case VK_F4: closeTab(active); return true;
                case 'L': focusAddr(); return true;
                case 'D': toggleBookmark(); return true;
                case 'R': reload(); return true;
                case VK_TAB:
                    if (!tabs.empty()) activate((active + (shift ? (int)tabs.size() - 1 : 1)) % (int)tabs.size());
                    return true;
                default:
                    if (vk >= '1' && vk <= '9' && !focus && !addrFocus) {
                        int i = vk == '9' ? (int)tabs.size() - 1 : (int)(vk - '1');
                        activate(std::min(i, (int)tabs.size() - 1));
                        return true;
                    }
            }
            if (!addrFocus && focus && (vk == VK_LEFT || vk == VK_RIGHT)) return onFormKey(vk, shift, true);
            if (!addrFocus && !focus) {
                if (vk == VK_HOME && t) { t->scroll = 0; invalidate(); return true; }
                if (vk == VK_END && t) { t->scroll = maxScroll(*t); invalidate(); return true; }
            }
            return false;  // let WM_CHAR handle Ctrl+A/C/V/X
        }
        if (vk == VK_F5) {
            reload();
            return true;
        }
        if (vk == VK_F12) {
            devOpen = !devOpen;
            invalidate();
            return true;
        }
        if (vk == VK_BROWSER_BACK) {
            goBack();
            return true;
        }
        if (vk == VK_BROWSER_FORWARD) {
            goFwd();
            return true;
        }
        if (conFocus && devOpen && devTab == 3) return conKey(vk);
        if (addrFocus) {
            switch (vk) {
                case VK_LEFT:
                    if (addrSel) caret = 0;
                    else if (caret > 0) --caret;
                    addrSel = false;
                    break;
                case VK_RIGHT:
                    if (addrSel) caret = addrText.size();
                    else if (caret < addrText.size()) ++caret;
                    addrSel = false;
                    break;
                case VK_HOME: caret = 0; addrSel = false; break;
                case VK_END: caret = addrText.size(); addrSel = false; break;
                case VK_DELETE:
                    if (addrSel) {
                        addrText.clear();
                        caret = 0;
                        addrSel = false;
                    } else if (caret < addrText.size())
                        addrText.erase(caret, 1);
                    break;
                default: return false;
            }
            invalidate();
            return true;
        }
        if (vk == VK_TAB) {
            moveFocus(shift ? -1 : 1);
            return true;
        }
        if (focus) {
            if (onFormKey(vk, shift, false)) return true;
            if (focusText()) return false;  // typing: keep page-scroll keys out of text fields
        }
        float page = contentRect().h() - 60;
        switch (vk) {
            case VK_DOWN: scrollBy(40); return true;
            case VK_UP: scrollBy(-40); return true;
            case VK_NEXT: scrollBy(page); return true;
            case VK_PRIOR: scrollBy(-page); return true;
            case VK_SPACE:
                if (focus) return false;
                scrollBy(shift ? -page : page);
                return true;
            case VK_HOME: if (t) { t->scroll = 0; invalidate(); } return true;
            case VK_END: if (t) { t->scroll = maxScroll(*t); invalidate(); } return true;
            case VK_BACK: if (shift) goFwd(); else goBack(); return true;
        }
        return false;
    }

    void updateSize() {
        RECT rc;
        GetClientRect(hwnd, &rc);
        scale = GetDpiForWindow(hwnd) / 96.f;
        W = (rc.right - rc.left) / scale;
        H = (rc.bottom - rc.top) / scale;
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        auto mx = [&] { return GET_X_LPARAM(lp) / scale; };
        auto my = [&] { return GET_Y_LPARAM(lp) / scale; };
        switch (msg) {
            case WM_PAINT:
                paint();
                ValidateRect(hwnd, nullptr);
                return 0;
            case WM_ERASEBKGND: return 1;
            case WM_SIZE:
                if (wp == SIZE_MINIMIZED) return 0;
                updateSize();
                gpu.resize();
                paint();
                return 0;
            case WM_DPICHANGED: {
                RECT* r = (RECT*)lp;
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                updateSize();
                gpu.resize();
                invalidate();
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto* mm = (MINMAXINFO*)lp;
                mm->ptMinTrackSize = {560, 360};
                return 0;
            }
            case WM_LBUTTONDOWN:
                SetFocus(hwnd);
                onMouseDown(mx(), my(), false, (wp & MK_CONTROL) != 0);
                return 0;
            case WM_MBUTTONDOWN: onMouseDown(mx(), my(), true, false); return 0;
            case WM_XBUTTONDOWN:
                if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) goBack();
                else goFwd();
                return TRUE;
            case WM_LBUTTONUP:
                if (dragSb) {
                    dragSb = false;
                    ReleaseCapture();
                    invalidate();
                }
                return 0;
            case WM_MOUSEMOVE: onMouseMove(mx(), my()); return 0;
            case WM_MOUSEWHEEL: {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                onWheel(pt.x / scale, pt.y / scale, GET_WHEEL_DELTA_WPARAM(wp));
                return 0;
            }
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(hwnd, &pt);
                    float x = pt.x / scale, y = pt.y / scale;
                    LPCWSTR c = IDC_ARROW;
                    bool inContent = contentRect().hit(x, y) && !(pendSave && infoRect().hit(x, y));
                    if (addrRect().hit(x, y)) c = IDC_IBEAM;
                    else if (inContent && hoverText) c = IDC_IBEAM;
                    else if (inContent && (hoverLink || hoverClick)) c = IDC_HAND;
                    SetCursor(LoadCursor(nullptr, c));
                    return TRUE;
                }
                break;
            case WM_KEYDOWN:
                if (onKey(wp, false)) return 0;
                break;
            case WM_SYSKEYDOWN:
                if (onKey(wp, true)) return 0;
                break;
            case WM_CHAR: onChar((wchar_t)wp); return 0;
            case WM_TIMER: onTimer((UINT_PTR)wp); return 0;
            case WM_APP_LOADED: onLoaded((LoadResult*)lp); return 0;
            case WM_APP_SCRIPT:
                for (auto& t : tabs)
                    if (t->id == (int)wp) {
                        runScriptTimers(*t);
                        break;
                    }
                return 0;
            case WM_DESTROY:
                CookieJar::get().save();
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        App* app = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            app = (App*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)app);
            app->hwnd = h;
        }
        return app ? app->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    }
};

int run_browser(HINSTANCE hi, const std::string& start) {
    static App app;  // lives for the whole process
    if (!app.ts.init()) return 1;
    app.initFormats();
    app.bm.load();
    CookieJar::get().load(app_data_dir() + L"\\cookies.dat");
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = App::WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"TinyBrowserWindow";
    RegisterClassExW(&wc);
    float s = GetDpiForSystem() / 96.f;
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Tiny Browser", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             int(1280 * s), int(860 * s), nullptr, nullptr, hi, &app);
    if (!h) return 1;
    app.updateSize();
    if (!app.gpu.init(h)) {
        MessageBoxW(h, L"Could not initialise Direct3D 11 / Direct2D.", L"Tiny Browser", MB_ICONERROR);
        return 1;
    }
    app.newTab(App::fixup(widen(start)));
    ShowWindow(h, SW_SHOWDEFAULT);
    UpdateWindow(h);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

// Headless render of one page (plus chrome) to a PNG. `script` drives the page for tests, e.g.
// "click:300,400;type:hello;key:tab;key:enter;scroll:200". Cookies stay in memory only.
int run_screenshot(const std::string& url, const std::wstring& out, int w, int h, bool dev, float scroll, int devTab,
                   const std::wstring& script, const std::wstring& vaultPath) {
    App app;
    app.headless = true;
    app.W = (float)w;
    app.H = (float)h;
    app.devOpen = dev;
    app.devTab = devTab;
    if (!vaultPath.empty()) app.vault = PasswordVault(vaultPath);
    if (!app.ts.init()) return 2;
    app.initFormats();
    app.bm.load();
    double t0 = now_ms();
    Tab& t = app.newTab(App::fixup(widen(url)));
    double tLoad = now_ms() - t0;
    auto followRefresh = [&] {
        for (int i = 0; i < 5; ++i) {
            Tab* c = app.cur();
            if (!c || c->refreshUrl.empty() || c->refreshDelay > 5) break;
            std::string u = c->refreshUrl;
            c->refreshUrl.clear();
            app.navigate(*c, u, false, nullptr, true);
        }
    };
    followRefresh();
    if (Tab* c = app.cur()) app.pumpScripts(*c, 1500);
    app.cur()->scroll = scroll;
    app.ensureLayout(t);
    app.clampScroll(t);
    // Scripted input.
    size_t i = 0;
    while (i < script.size()) {
        size_t e = script.find(L';', i);
        if (e == std::wstring::npos) e = script.size();
        std::wstring cmd = script.substr(i, e - i);
        i = e + 1;
        size_t c = cmd.find(L':');
        std::wstring op = cmd.substr(0, c), arg = c == std::wstring::npos ? L"" : cmd.substr(c + 1);
        if (Tab* ct = app.cur()) app.ensureLayout(*ct);
        if (op == L"click") {
            float x = 0, y = 0;
            swscanf_s(arg.c_str(), L"%f,%f", &x, &y);
            app.onMouseDown(x, y, false, false);
        } else if (op == L"type") {
            for (wchar_t ch : arg) app.onChar(ch);
        } else if (op == L"key") {
            if (arg == L"enter") app.onChar(13);
            else if (arg == L"tab") app.moveFocus(1);
            else if (arg == L"backspace") app.onChar(8);
            else if (arg == L"save") app.answerSave(true);
            else if (arg == L"dismiss") app.answerSave(false);
        } else if (op == L"scroll") {
            app.scrollBy((float)_wtof(arg.c_str()));
        } else if (op == L"wait") {
            Sleep((DWORD)_wtoi(arg.c_str()));
        }
        followRefresh();
        if (Tab* c = app.cur()) app.pumpScripts(*c, 800);
    }
    Tab& ft = *app.cur();
    app.ensureLayout(ft);
    if (dev && devTab == 0 && ft.page) {
        for (auto& L : ft.page->devLines)
            if (L.node && L.node->name == "body") {
                app.devSel = L.node;
                break;
            }
    }
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 3;
    ComPtr<IWICBitmap> bmp;
    wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ComPtr<ID2D1Factory> f;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf());
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(f->CreateWicBitmapRenderTarget(bmp.Get(), D2D1::RenderTargetProperties(), &rt))) return 4;
    double t1 = now_ms();
    rt->BeginDraw();
    app.drawFrame(rt.Get(), 7);
    rt->EndDraw();
    app.lastFrameMs = now_ms() - t1;
    ComPtr<IWICStream> st;
    wic->CreateStream(&st);
    if (FAILED(st->InitializeFromFilename(out.c_str(), GENERIC_WRITE))) return 5;
    ComPtr<IWICBitmapEncoder> enc;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(st.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> fr;
    enc->CreateNewFrame(&fr, nullptr);
    fr->Initialize(nullptr);
    fr->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    fr->WriteSource(bmp.Get(), nullptr);
    fr->Commit();
    enc->Commit();
    if (ft.page) {
        std::wstring txt = out + L".txt";
        FILE* fp = _wfopen(txt.c_str(), L"w");
        if (fp) {
            fprintf(fp, "url=%s\ntitle=%s\nload_total_ms=%.1f\nfetch_ms=%.1f\nparse_ms=%.1f\nsubresources_ms=%.1f\nstyle_ms=%.1f\nlayout_ms=%.1f\npaint_ms=%.1f\nnodes=%zu\nrules=%zu\nitems=%zu\nwidgets=%zu\nheight=%.0f\nrequests=%zu\nimages=%zu\ncookies=%zu\nsave_prompt=%d\n",
                    ft.page->url.c_str(), narrow(ft.title).c_str(), tLoad, ft.page->tFetch, ft.page->tParse, ft.page->tSub, ft.page->tStyle,
                    ft.page->tLayout, app.lastFrameMs, ft.page->doc->nodeCount, ft.page->rules, ft.page->lo.items.size(),
                    ft.page->lo.widgets.size(), ft.page->lo.height, ft.page->net.size(), ft.page->images.size(), CookieJar::get().count(),
                    app.pendSave ? 1 : 0);
            for (auto& wd : ft.page->lo.widgets) {
                const std::string* nm = wd.node->attr("name");
                fprintf(fp, "widget %s name=%s at %.0f,%.0f %0.fx%.0f\n", wd.node->name.c_str(), nm ? nm->c_str() : "", wd.x, wd.y, wd.w, wd.h);
            }
            for (auto& e : ft.page->net) fprintf(fp, "net %d %zu %s %s\n", e.status, e.bytes, e.url.c_str(), e.error.c_str());
            fprintf(fp, "scripts=%d stopped=%d\n", ft.page->script ? 1 : 0, ft.page->script && ft.page->script->stopped() ? 1 : 0);
            static const char* kinds[] = {"log", "info", "warn", "error", "input", "result"};
            for (auto& line : app.conLines) fprintf(fp, "console %s %s\n", kinds[line.kind < 0 || line.kind > 5 ? 0 : line.kind], narrow(line.text).c_str());
            fclose(fp);
        }
    }
    return 0;
}

}  // namespace tb
