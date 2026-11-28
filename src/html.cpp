#include "html.h"

#include <cstring>
#include <initializer_list>

#include "url.h"

namespace tb {

static constexpr auto npos = std::string_view::npos;

const std::string* Node::attr(std::string_view n) const {
    for (auto& a : attrs)
        if (a.first == n) return &a.second;
    return nullptr;
}

int canvas_dimension(const Node* canvas, bool height) {
    int dflt = height ? 150 : 300;
    const std::string* v = canvas->attr(height ? "height" : "width");
    if (!v) return dflt;
    // the rules for parsing non-negative integers (HTML 2.3.4.1): spaces, an optional sign, then digits
    size_t i = 0;
    auto space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r'; };
    while (i < v->size() && space((*v)[i])) ++i;
    bool minus = i < v->size() && (*v)[i] == '-';
    if (i < v->size() && ((*v)[i] == '+' || minus)) ++i;
    if (i >= v->size() || !isdigit((unsigned char)(*v)[i])) return dflt;
    long long n = 0;
    while (i < v->size() && isdigit((unsigned char)(*v)[i]) && n < 100000000) n = n * 10 + ((*v)[i++] - '0');
    if (minus && n != 0) return dflt;
    return (int)std::min<long long>(n, 2147483647);
}

std::string text_content(const Node* n) {
    if (n->type == Node::T::Text) return n->text;
    std::string r;
    for (auto& k : n->kids)
        if (k->type == Node::T::Text || k->type == Node::T::Element) r += text_content(k.get());
    return r;
}

int g_shadowRoots = 0;

Node* shadow_root_of(const Node* n) {
    for (Node* p = n->parent; p; p = p->parent)
        if (p->isShadowRoot()) return p;
    return nullptr;
}
// Slots are found by name (DOM 4.2.2.3 "find a slot"): the first <slot> of the shadow tree, in tree order, whose
// name is the child's slot attribute. Text and elements without the attribute go to the slot without a name.
static Node* find_slot(Node* in, const std::string& name) {
    for (auto& k : in->kids) {
        if (k->type != Node::T::Element) continue;
        if (k->name == "slot") {
            const std::string* own = k->attr("name");
            if ((own ? *own : std::string()) == name) return k.get();
        }
        if (Node* found = find_slot(k.get(), name)) return found;
    }
    return nullptr;
}
static const std::string& slot_name_of(const Node* child) {
    static const std::string none;
    const std::string* a = child->type == Node::T::Element ? child->attr("slot") : nullptr;
    return a ? *a : none;
}
Node* assigned_slot(const Node* n) {
    if (!n->parent || !n->parent->shadow || n->type == Node::T::Comment || n->isShadowRoot()) return nullptr;
    return find_slot(n->parent->shadow.get(), slot_name_of(n));
}
Node* flat_parent(const Node* n) {
    Node* slot = assigned_slot(n);
    return slot ? slot : n->parent;
}
void assigned_nodes(const Node* slot, std::vector<Node*>& out) {
    Node* root = shadow_root_of(slot);
    if (!root || !root->parent) return;
    const std::string* own = slot->attr("name");
    std::string name = own ? *own : std::string();
    if (find_slot(root, name) != slot) return;  // an earlier slot of the same name takes them
    for (auto& k : root->parent->kids)
        if (k->type != Node::T::Comment && slot_name_of(k.get()) == name) out.push_back(k.get());
}
static void flatten_into(const Node* from, std::vector<Node*>& out, int depth) {
    for (auto& k : from->kids) {
        if (k->type == Node::T::Element && k->name == "slot" && depth < 16 && shadow_root_of(k.get())) {
            size_t before = out.size();
            assigned_nodes(k.get(), out);
            if (out.size() == before) flatten_into(k.get(), out, depth + 1);  // nothing assigned: the slot's own content
        } else
            out.push_back(k.get());
    }
}
FlatKids::FlatKids(const Node* n) {
    bool generated = n->before || n->after;
    if (g_shadowRoots == 0 && !generated) {
        plain_ = &n->kids;
        return;
    }
    const Node* from = n->shadow ? n->shadow.get() : n;
    bool hasSlot = false;
    for (auto& k : from->kids)
        if (k->type == Node::T::Element && k->name == "slot") hasSlot = true;
    if (!generated && (!hasSlot || !(n->shadow || shadow_root_of(from->kids[0].get())))) {
        plain_ = &from->kids;
        return;
    }
    if (n->before) mixed_.push_back(n->before.get());
    if (hasSlot && (n->shadow || shadow_root_of(from->kids[0].get()))) flatten_into(from, mixed_, 0);
    else
        for (auto& k : from->kids) mixed_.push_back(k.get());
    if (n->after) mixed_.push_back(n->after.get());
    if (mixed_.empty()) plain_ = nullptr;
}

namespace {

struct Ent {
    const char* n;
    uint32_t cp;
};
const Ent kEnts[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9},
    {"reg", 0xAE}, {"trade", 0x2122}, {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"laquo", 0xAB},
    {"raquo", 0xBB}, {"middot", 0xB7}, {"bull", 0x2022}, {"deg", 0xB0}, {"times", 0xD7}, {"divide", 0xF7},
    {"plusmn", 0xB1}, {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2}, {"sect", 0xA7},
    {"para", 0xB6}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"iexcl", 0xA1}, {"iquest", 0xBF},
    {"shy", 0xAD}, {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"harr", 0x2194},
    {"hearts", 0x2665}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009}, {"zwj", 0x200D},
    {"zwnj", 0x200C}, {"aacute", 0xE1}, {"eacute", 0xE9}, {"iacute", 0xED}, {"oacute", 0xF3},
    {"uacute", 0xFA}, {"ntilde", 0xF1}, {"Aacute", 0xC1}, {"Eacute", 0xC9}, {"Iacute", 0xCD},
    {"Oacute", 0xD3}, {"Uacute", 0xDA}, {"Ntilde", 0xD1}, {"uuml", 0xFC}, {"ouml", 0xF6}, {"auml", 0xE4},
    {"Uuml", 0xDC}, {"Ouml", 0xD6}, {"Auml", 0xC4}, {"szlig", 0xDF}, {"ccedil", 0xE7}, {"Ccedil", 0xC7},
    {"agrave", 0xE0}, {"egrave", 0xE8}, {"igrave", 0xEC}, {"ograve", 0xF2}, {"ugrave", 0xF9},
    {"acirc", 0xE2}, {"ecirc", 0xEA}, {"icirc", 0xEE}, {"ocirc", 0xF4}, {"ucirc", 0xFB}, {"atilde", 0xE3},
    {"otilde", 0xF5}, {"aring", 0xE5}, {"Aring", 0xC5}, {"aelig", 0xE6}, {"oslash", 0xF8}, {"euml", 0xEB},
    {"iuml", 0xEF}, {"yuml", 0xFF}, {"hArr", 0x21D4}, {"rArr", 0x21D2}, {"lArr", 0x21D0}, {"check", 0x2713},
    {"star", 0x2606}, {"starf", 0x2605}, {"prime", 0x2032}, {"minus", 0x2212}, {"le", 0x2264},
    {"ge", 0x2265}, {"ne", 0x2260}, {"infin", 0x221E}, {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3},
    {"delta", 0x3B4}, {"pi", 0x3C0}, {"mu", 0x3BC}, {"lambda", 0x3BB}, {"sigma", 0x3C3}, {"omega", 0x3C9},
    {"theta", 0x3B8}, {"Delta", 0x394}, {"Sigma", 0x3A3}, {"Omega", 0x3A9}, {"sum", 0x2211},
    {"radic", 0x221A}, {"asymp", 0x2248}, {"crarr", 0x21B5}, {"loz", 0x25CA}, {"spades", 0x2660},
    {"clubs", 0x2663}, {"diams", 0x2666}, {"sup1", 0xB9}, {"sup2", 0xB2}, {"sup3", 0xB3}, {"ordm", 0xBA},
    {"ordf", 0xAA}, {"acute", 0xB4}, {"uml", 0xA8}, {"macr", 0xAF}, {"cedil", 0xB8}, {"not", 0xAC},
    {"brvbar", 0xA6}, {"curren", 0xA4}, {"dagger", 0x2020}, {"Dagger", 0x2021}, {"permil", 0x2030},
    {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"oline", 0x203E},
    {"frasl", 0x2044}, {"micro", 0xB5}, {"emsp13", 0x2004}, {"hairsp", 0x200A}, {"NewLine", '\n'},
    {"Tab", '\t'}, {"colon", ':'}, {"comma", ','}, {"period", '.'}, {"excl", '!'}, {"quest", '?'},
    {"num", '#'}, {"dollar", '$'}, {"percnt", '%'}, {"lpar", '('}, {"rpar", ')'}, {"ast", '*'},
    {"plus", '+'}, {"equals", '='}, {"sol", '/'}, {"bsol", '\\'}, {"lsqb", '['}, {"rsqb", ']'},
    {"lcub", '{'}, {"rcub", '}'}, {"vert", '|'}, {"grave", '`'}, {"Hat", '^'}, {"lowbar", '_'},
};
const uint16_t kCp1252[32] = {0x20AC, 0x81,   0x201A, 0x192,  0x201E, 0x2026, 0x2020, 0x2021,
                              0x2C6,  0x2030, 0x160,  0x2039, 0x152,  0x8D,   0x17D,  0x8F,
                              0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                              0x2DC,  0x2122, 0x161,  0x203A, 0x153,  0x9D,   0x17E,  0x178};

bool in(std::string_view n, std::initializer_list<std::string_view> l) {
    for (auto x : l)
        if (x == n) return true;
    return false;
}

bool isVoid(std::string_view n) {
    return in(n, {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source",
                  "track", "wbr", "keygen", "basefont", "bgsound", "frame"});
}

struct Tok {
    enum K { Start, End, Text, Comment, Doctype } k = Text;
    std::string name, data;
    std::vector<std::pair<std::string, std::string>> attrs;
    bool selfClosing = false;
};

constexpr size_t kMaxDepth = 384;

class Parser {
public:
    Parser(std::string_view s, Document& d) : s_(s), d_(d) {}

    void run() {
        Tok t;
        while (next(t)) process(t);
        ensureBody();
    }

private:
    std::string_view s_;
    size_t i_ = 0;
    Document& d_;
    std::string rawEnd_;
    bool rcdata_ = false;
    std::vector<Node*> st_;
    int foreign_ = 0;
    bool skipNL_ = false;
public:
    bool sawDoctype_ = false;
private:

    // ---------------- tokenizer ----------------
    bool next(Tok& t) {
        t = Tok{};
        while (true) {
            if (i_ >= s_.size()) return false;
            if (!rawEnd_.empty()) {
                size_t p = i_, end = npos;
                while ((p = s_.find("</", p)) != npos) {
                    size_t a = p + 2 + rawEnd_.size();
                    if (istarts(s_.substr(p + 2), rawEnd_)) {
                        char after = a < s_.size() ? s_[a] : '>';
                        if (isws(after) || after == '>' || after == '/') {
                            end = p;
                            break;
                        }
                    }
                    p += 2;
                }
                if (end == npos) end = s_.size();
                t.k = Tok::Text;
                if (rcdata_)
                    decode_entities(s_.substr(i_, end - i_), t.data);
                else
                    t.data = std::string(s_.substr(i_, end - i_));
                i_ = end;
                rawEnd_.clear();
                if (t.data.empty()) continue;
                return true;
            }
            if (s_[i_] == '<' && i_ + 1 < s_.size()) {
                char c = s_[i_ + 1];
                if (c == '!') {
                    if (s_.substr(i_, 4) == "<!--") {
                        size_t e = s_.find("-->", i_ + 4);
                        t.k = Tok::Comment;
                        t.data = std::string(s_.substr(i_ + 4, (e == npos ? s_.size() : e) - (i_ + 4)));
                        i_ = e == npos ? s_.size() : e + 3;
                        return true;
                    }
                    if (s_.substr(i_, 9) == "<![CDATA[") {
                        size_t e = s_.find("]]>", i_ + 9);
                        t.k = Tok::Text;
                        t.data = std::string(s_.substr(i_ + 9, (e == npos ? s_.size() : e) - (i_ + 9)));
                        i_ = e == npos ? s_.size() : e + 3;
                        return true;
                    }
                    size_t e = s_.find('>', i_);
                    std::string_view body = s_.substr(i_ + 2, (e == npos ? s_.size() : e) - (i_ + 2));
                    i_ = e == npos ? s_.size() : e + 1;
                    t.k = istarts(body, "doctype") ? Tok::Doctype : Tok::Comment;
                    t.data = std::string(body);
                    return true;
                }
                if (c == '?') {
                    size_t e = s_.find('>', i_);
                    t.k = Tok::Comment;
                    t.data = std::string(s_.substr(i_ + 1, (e == npos ? s_.size() : e) - (i_ + 1)));
                    i_ = e == npos ? s_.size() : e + 1;
                    return true;
                }
                if (c == '/') {
                    if (i_ + 2 < s_.size() && isalpha_(s_[i_ + 2])) {
                        size_t j = i_ + 2;
                        while (j < s_.size() && !isws(s_[j]) && s_[j] != '>' && s_[j] != '/') ++j;
                        t.k = Tok::End;
                        t.name = lower(s_.substr(i_ + 2, j - i_ - 2));
                        size_t e = s_.find('>', j);
                        i_ = e == npos ? s_.size() : e + 1;
                        return true;
                    }
                    // "</>" or bogus end tag: skip to '>'
                    size_t e = s_.find('>', i_);
                    i_ = e == npos ? s_.size() : e + 1;
                    continue;
                }
                if (isalpha_(c)) {
                    parseTag(t);
                    return true;
                }
            }
            size_t start = i_;
            size_t e = s_.find('<', i_ + 1);
            if (e == npos) e = s_.size();
            t.k = Tok::Text;
            decode_entities(s_.substr(start, e - start), t.data);
            i_ = e;
            return true;
        }
    }

    void parseTag(Tok& t) {
        size_t j = i_ + 1;
        while (j < s_.size() && !isws(s_[j]) && s_[j] != '>' && s_[j] != '/') ++j;
        t.k = Tok::Start;
        t.name = lower(s_.substr(i_ + 1, j - i_ - 1));
        while (j < s_.size()) {
            while (j < s_.size() && isws(s_[j])) ++j;
            if (j >= s_.size()) break;
            if (s_[j] == '>') {
                ++j;
                break;
            }
            if (s_[j] == '/') {
                if (j + 1 < s_.size() && s_[j + 1] == '>') {
                    t.selfClosing = true;
                    j += 2;
                    break;
                }
                ++j;
                continue;
            }
            size_t ns = j++;
            while (j < s_.size() && !isws(s_[j]) && s_[j] != '=' && s_[j] != '>' && s_[j] != '/') ++j;
            std::string name = lower(s_.substr(ns, j - ns));
            std::string val;
            size_t k = j;
            while (k < s_.size() && isws(s_[k])) ++k;
            if (k < s_.size() && s_[k] == '=') {
                ++k;
                while (k < s_.size() && isws(s_[k])) ++k;
                if (k < s_.size() && (s_[k] == '"' || s_[k] == '\'')) {
                    char q = s_[k++];
                    size_t e = s_.find(q, k);
                    if (e == npos) e = s_.size();
                    decode_entities(s_.substr(k, e - k), val);
                    j = e < s_.size() ? e + 1 : e;
                } else {
                    size_t e = k;
                    while (e < s_.size() && !isws(s_[e]) && s_[e] != '>') ++e;
                    decode_entities(s_.substr(k, e - k), val);
                    j = e;
                }
            }
            bool dup = false;
            for (auto& a : t.attrs)
                if (a.first == name) dup = true;
            if (!dup && !name.empty()) t.attrs.emplace_back(std::move(name), std::move(val));
        }
        i_ = j;
    }

    // ---------------- tree builder ----------------
    Node* cur() { return st_.empty() ? (d_.html ? d_.html : d_.root.get()) : st_.back(); }
    // Where the next node goes: into the current element, or into the contents of a template (HTML 13.2.6.1)
    Node* place() {
        Node* c = cur();
        return c->content ? c->content.get() : c;
    }

    static std::unique_ptr<Node> mk(std::string_view name) {
        auto n = std::make_unique<Node>();
        n->type = Node::T::Element;
        n->name = std::string(name);
        return n;
    }

    void ensureHtml() {
        if (!d_.html) {
            d_.html = d_.root->append(mk("html"));
            st_.assign(1, d_.html);
        }
    }
    void ensureHead() {
        ensureHtml();
        if (!d_.head) d_.head = d_.html->append(mk("head"));
    }
    void ensureBody() {
        ensureHead();
        if (!d_.body) {
            d_.body = d_.html->append(mk("body"));
            st_.clear();
            st_.push_back(d_.html);
            st_.push_back(d_.body);
            foreign_ = 0;
        }
    }

    void popTo(size_t k) {
        for (size_t m = k; m < st_.size(); ++m)
            if (st_[m]->name == "svg" || st_[m]->name == "math") --foreign_;
        if (foreign_ < 0) foreign_ = 0;
        st_.resize(k);
    }
    void popOne() {
        if (st_.size() > 2) popTo(st_.size() - 1);
    }

    // Finds the topmost open element named any of `names`, stopping at a scope barrier.
    long findInScope(std::initializer_list<std::string_view> names, std::initializer_list<std::string_view> barrier) {
        for (size_t k = st_.size(); k-- > 0;) {
            Node* x = st_[k];
            if (in(x->name, names)) return (long)k;
            if (x == d_.body || x == d_.html || in(x->name, barrier)) return -1;
        }
        return -1;
    }

    void closeP() {
        long k = findInScope({"p"}, {"button", "table", "td", "th", "caption", "marquee", "object", "applet",
                                     "template", "svg", "math"});
        if (k >= 0) popTo((size_t)k);
    }

    void mergeAttrs(Node* n, Tok& t) {
        for (auto& a : t.attrs)
            if (!n->attr(a.first)) n->attrs.push_back(a);
    }

    void insert(Tok& t) {
        // A declarative shadow root (HTML 13.2.6.4.4, a template start tag with shadowrootmode): the template's
        // content becomes the shadow tree of the element it is written in.
        if (t.name == "template" && foreign_ == 0 && cur() == place() && !cur()->shadow && cur()->type == Node::T::Element && st_.size() < kMaxDepth)
            for (auto& a : t.attrs)
                if (a.first == "shadowrootmode" && (a.second == "open" || a.second == "closed")) {
                    Node* host = cur();
                    host->shadow = mk("#shadow-root");
                    host->shadow->parent = host;
                    host->shadow->attrs.push_back({"mode", a.second});
                    ++g_shadowRoots;
                    st_.push_back(host->shadow.get());
                    return;
                }
        auto e = mk(t.name);
        e->attrs = std::move(t.attrs);
        Node* raw = place()->append(std::move(e));
        const std::string& n = raw->name;
        if (n == "template" && foreign_ == 0) raw->content = mk("#document-fragment");
        bool selfClose = isVoid(n) || (t.selfClosing && (foreign_ > 0 || n == "svg" || n == "math"));
        if (selfClose) return;
        if (st_.size() < kMaxDepth) {
            st_.push_back(raw);
            if (n == "svg" || n == "math") ++foreign_;
        }
        if (in(n, {"script", "style", "xmp", "iframe", "noembed", "noframes"})) {
            rawEnd_ = n;
            rcdata_ = false;
        } else if (n == "textarea" || n == "title") {
            rawEnd_ = n;
            rcdata_ = true;
        }
    }

    void process(Tok& t) {
        switch (t.k) {
            case Tok::Doctype:
                sawDoctype_ = true;
                return;
            case Tok::Comment: {
                auto n = std::make_unique<Node>();
                n->type = Node::T::Comment;
                n->text = std::move(t.data);
                place()->append(std::move(n));
                return;
            }
            case Tok::Text: text(t.data); return;
            case Tok::Start: start(t); return;
            case Tok::End: end(t.name); return;
        }
    }

    void text(std::string& data) {
        if (skipNL_) {
            skipNL_ = false;
            if (!data.empty() && data[0] == '\n') data.erase(0, 1);
        }
        if (data.empty()) return;
        Node* c = cur();
        bool rawParent = c->type == Node::T::Element &&
                         in(c->name, {"style", "script", "title", "textarea", "xmp", "iframe", "noembed", "noframes"});
        if (!d_.body && !rawParent) {
            bool allws = true;
            for (char ch : data)
                if (!isws(ch)) allws = false;
            if (allws) return;
            ensureBody();
        }
        c = place();
        if (!c->kids.empty() && c->kids.back()->type == Node::T::Text) {
            c->kids.back()->text += data;
        } else {
            auto n = std::make_unique<Node>();
            n->type = Node::T::Text;
            n->text = std::move(data);
            c->append(std::move(n));
        }
    }

    void start(Tok& t) {
        std::string& n = t.name;
        if (n == "html") {
            ensureHtml();
            mergeAttrs(d_.html, t);
            return;
        }
        if (n == "head") {
            if (!d_.body) {
                ensureHead();
                st_ = {d_.html, d_.head};
            }
            return;
        }
        if (n == "body") {
            ensureBody();
            mergeAttrs(d_.body, t);
            return;
        }
        if (!d_.body && in(n, {"meta", "link", "style", "script", "title", "base", "basefont", "bgsound"})) {
            ensureHead();
            if (st_.empty() || st_.back() != d_.head) st_ = {d_.html, d_.head};
            insert(t);
            return;
        }
        ensureBody();
        if (foreign_ == 0) {
            if (in(n, {"address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div",
                       "dl", "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5",
                       "h6", "header", "hgroup", "hr", "main", "menu", "nav", "ol", "p", "pre", "section", "table",
                       "ul", "li", "dd", "dt", "listing", "plaintext", "search", "summary", "xmp"}))
                closeP();
            if (in(n, {"h1", "h2", "h3", "h4", "h5", "h6"}) &&
                in(cur()->name, {"h1", "h2", "h3", "h4", "h5", "h6"}))
                popOne();
            if (n == "li") {
                long k = findInScope({"li"}, {"ul", "ol", "menu", "dir", "table", "td", "th"});
                if (k >= 0) popTo((size_t)k);
            }
            if (n == "dd" || n == "dt") {
                long k = findInScope({"dd", "dt"}, {"dl", "table", "td", "th"});
                if (k >= 0) popTo((size_t)k);
            }
            if (n == "option" && cur()->name == "option") popOne();
            if (n == "optgroup") {
                if (cur()->name == "option") popOne();
                if (cur()->name == "optgroup") popOne();
            }
            if (n == "tr" || n == "tbody" || n == "thead" || n == "tfoot") {
                long k = findInScope({"td", "th"}, {"table"});
                if (k >= 0) popTo((size_t)k);
                k = findInScope({"tr"}, {"table"});
                if (k >= 0) popTo((size_t)k);
                if (n != "tr") {
                    k = findInScope({"tbody", "thead", "tfoot"}, {"table"});
                    if (k >= 0) popTo((size_t)k);
                }
            }
            if (n == "td" || n == "th") {
                long k = findInScope({"td", "th"}, {"table"});
                if (k >= 0) popTo((size_t)k);
            }
            if (n == "a") {
                long k = findInScope({"a"}, {"table", "td", "th", "caption"});
                if (k >= 0) popTo((size_t)k);
            }
            if (n == "button") {
                long k = findInScope({"button"}, {"table", "td", "th"});
                if (k >= 0) popTo((size_t)k);
            }
            if (n == "nobr") {
                long k = findInScope({"nobr"}, {"table", "td", "th"});
                if (k >= 0) popTo((size_t)k);
            }
            if (n == "image") n = "img";
            if (n == "textarea" || n == "pre" || n == "listing") skipNL_ = true;
        }
        insert(t);
    }

    void end(const std::string& name) {
        if (name == "html" || name == "body") return;
        if (name == "head") {
            if (!d_.body && !st_.empty() && st_.back() == d_.head) st_.pop_back();
            return;
        }
        if (name == "br") {
            Tok t;
            t.k = Tok::Start;
            t.name = "br";
            start(t);
            return;
        }
        if (name == "p") {
            ensureBody();
            if (findInScope({"p"}, {"button", "table", "td", "th", "caption"}) < 0) {
                Tok t;
                t.k = Tok::Start;
                t.name = "p";
                insert(t);
            }
            closeP();
            return;
        }
        bool tableish = in(name, {"table", "tr", "td", "th", "tbody", "thead", "tfoot", "caption"});
        for (size_t k = st_.size(); k-- > 0;) {
            Node* x = st_[k];
            if (x->name == name || (name == "template" && x->isShadowRoot())) {
                popTo(k);
                return;
            }
            if (x == d_.body || x == d_.html || x == d_.head) return;
            if (!tableish && foreign_ == 0 && in(x->name, {"table", "td", "th", "caption"})) return;
            if (tableish && x->name == "table" && name != "table") return;
        }
    }
};

// Ids, classes and child positions for every node, and the document's title, base URL and size. The contents of
// templates are prepared the same way, though they are no part of the document.
void finalize(Document& d) {
    std::vector<Node*> stack{d.root.get()};
    std::vector<Node*> templates;  // contents still to walk, after the document itself
    size_t count = 0;
    bool inDocument = true;
    for (;;) {
        if (stack.empty()) {
            if (templates.empty()) break;
            stack.push_back(templates.back());
            templates.pop_back();
            inDocument = false;
        }
        Node* n = stack.back();
        stack.pop_back();
        if (inDocument) ++count;
        if (n->content) templates.push_back(n->content.get());
        if (n->shadow) stack.push_back(n->shadow.get());
        if (n->type == Node::T::Element) {
            if (auto v = n->attr("id")) n->id = *v;
            if (auto c = n->attr("class")) {
                std::string_view s = *c;
                size_t i = 0;
                while (i < s.size()) {
                    while (i < s.size() && isws(s[i])) ++i;
                    size_t j = i;
                    while (j < s.size() && !isws(s[j])) ++j;
                    if (j > i) n->classes.emplace_back(s.substr(i, j - i));
                    i = j;
                }
            }
            if (inDocument && d.title.empty() && n->name == "title") {
                std::string t = text_content(n), out;
                bool sp = false;
                for (char ch : t) {
                    if (isws(ch)) {
                        sp = !out.empty();
                    } else {
                        if (sp) out += ' ';
                        sp = false;
                        out += ch;
                    }
                }
                d.title = out;
            }
            if (inDocument && n->name == "base" && d.baseUrl.empty())
                if (auto h = n->attr("href")) d.baseUrl = resolve_url(d.url, *h);
        }
        uint32_t ec = 0, idx = 0;
        for (auto& k : n->kids)
            if (k->type == Node::T::Element) ++ec;
        for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) stack.push_back(it->get());
        for (auto& k : n->kids)
            if (k->type == Node::T::Element) {
                k->elemIndex = idx++;
                k->elemCount = ec;
            }
    }
    d.nodeCount = count;
    if (d.baseUrl.empty()) d.baseUrl = d.url;
}

}  // namespace

void decode_entities(std::string_view s, std::string& out) {
    out.reserve(out.size() + s.size());
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c != '&') {
            if (c != '\0') out += c;
            ++i;
            continue;
        }
        size_t j = i + 1;
        if (j < s.size() && s[j] == '#') {
            ++j;
            bool hex = false;
            if (j < s.size() && (s[j] == 'x' || s[j] == 'X')) {
                hex = true;
                ++j;
            }
            uint32_t v = 0;
            size_t d = j;
            while (j < s.size()) {
                char ch = s[j];
                int dv;
                if (ch >= '0' && ch <= '9')
                    dv = ch - '0';
                else if (hex && ch >= 'a' && ch <= 'f')
                    dv = ch - 'a' + 10;
                else if (hex && ch >= 'A' && ch <= 'F')
                    dv = ch - 'A' + 10;
                else
                    break;
                if (v < 0x110000) v = v * (hex ? 16 : 10) + dv;
                ++j;
            }
            if (j == d) {
                out += '&';
                ++i;
                continue;
            }
            if (j < s.size() && s[j] == ';') ++j;
            if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
            if (v >= 0x80 && v <= 0x9F) v = kCp1252[v - 0x80];
            append_utf8(out, v);
            i = j;
            continue;
        }
        size_t k = j;
        while (k < s.size() && k - j < 32 && isalnum_(s[k])) ++k;
        std::string_view name = s.substr(j, k - j);
        bool found = false;
        if (!name.empty()) {
            for (auto& e : kEnts)
                if (name == e.n) {
                    append_utf8(out, e.cp);
                    i = k + ((k < s.size() && s[k] == ';') ? 1 : 0);
                    found = true;
                    break;
                }
            if (!found) {
                // Legacy entities that browsers accept without a terminator, e.g. "&copy2024" or "&ampx".
                static const Ent legacy[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'},
                                             {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE}};
                for (auto& e : legacy) {
                    size_t L = strlen(e.n);
                    if (name.size() > L && name.substr(0, L) == e.n) {
                        append_utf8(out, e.cp);
                        i = j + L;
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) {
            out += '&';
            ++i;
        }
    }
}

std::unique_ptr<Document> parse_html(std::string_view src, const std::string& url) {
    auto d = std::make_unique<Document>();
    d->url = url;
    d->root = std::make_unique<Node>();
    d->root->type = Node::T::Document;
    Parser p(src, *d);
    p.run();
    d->quirks = !p.sawDoctype_;
    finalize(*d);
    return d;
}

}  // namespace tb
