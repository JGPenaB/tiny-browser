#include <cstdlib>
#include <cwctype>

#include "css.h"
#include "layout.h"
#include "text.h"

namespace tb {

// A colour as an element paints it: its opacity, with that of its ancestors, makes it that much see-through.
static Color faded(Color c, const Style& s) {
    c.a *= s.alpha;
    return c;
}

// ---------------------------------------------------------------- form control helpers (shared with the shell)

WidgetKind widget_kind(const Node* n) {
    if (n->type != Node::T::Element) return WidgetKind::None;
    const std::string& nm = n->name;
    if (nm == "textarea") return WidgetKind::TextArea;
    if (nm == "select") return WidgetKind::Select;
    if (nm == "button") return WidgetKind::Button;
    if (nm != "input") return WidgetKind::None;
    auto t = n->attr("type");
    std::string type = t ? lower(*t) : "text";
    if (type == "hidden") return WidgetKind::None;
    if (type == "checkbox") return WidgetKind::Check;
    if (type == "radio") return WidgetKind::Radio;
    if (type == "submit" || type == "button" || type == "reset" || type == "image" || type == "file") return WidgetKind::Button;
    if (type == "password") return WidgetKind::Password;
    return WidgetKind::Text;
}

static std::string collapse_ws(const std::string& s, size_t maxLen = 200) {
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
        if (out.size() >= maxLen) break;
    }
    return out;
}

std::wstring widget_label(const Node* n) {
    std::string label;
    if (n->name == "input") {
        auto t = n->attr("type");
        std::string type = t ? lower(*t) : "text";
        if (auto v = n->attr("value")) label = *v;
        else if (type == "reset") label = "Reset";
        else if (type == "file") label = "Choose file";
        else if (type == "image") label = n->attr("alt") ? *n->attr("alt") : "Submit";
        else if (type == "submit") label = "Submit";
    } else {
        label = text_content(n);
    }
    return widen(collapse_ws(label));
}

std::vector<Node*> select_options(const Node* n) {
    std::vector<Node*> r;
    for (auto& k : n->kids) {
        if (k->is("option")) r.push_back(k.get());
        else if (k->is("optgroup"))
            for (auto& o : k->kids)
                if (o->is("option")) r.push_back(o.get());
    }
    return r;
}

namespace {

enum class BK : uint8_t { Block, Anon, Table, Row, Cell, Flex, Marker };

struct Mark {
    size_t d = 0, l = 0, b = 0, w = 0;
};

struct Box;

struct Item {
    enum K : uint8_t { Word, Space, Break, Image, Widget, Atomic, Gap } k = Word;
    bool brk = false;  // a line break is allowed before this item
    std::wstring text;
    const Style* st = nullptr;
    Node* link = nullptr;
    Node* inl = nullptr;   // innermost inline element containing the item
    Node* node = nullptr;  // replaced element (img / form control)
    Color bg;
    float w = -1, h = 0, base = 0;
    int img = -1;
    std::shared_ptr<Box> box;  // Atomic: inline-block / inline-flex
    Mark m0, m1;
};

struct Box {
    BK kind = BK::Block;
    Node* node = nullptr;
    const Style* st = nullptr;
    std::vector<std::unique_ptr<Box>> kids;
    std::vector<std::unique_ptr<Box>> abs;  // positioned descendants whose containing block is this box
    std::vector<Box*> inlineAbs;            // (Anon) positioned boxes whose static position is this line box
    Box* cb = nullptr;                      // nearest positioned ancestor
    Box* target = nullptr;                  // Marker: the out-of-flow box it stands in for
    std::vector<Node*> inl;
    std::vector<Item> items;
    bool itemsBuilt = false;
    float x = 0, y = 0, w = 0, h = 0;  // border box
    float m[4] = {}, b[4] = {}, p[4] = {};
    float firstBaseline = -1;
    int ordinal = 0, span = 1;
    bool intrinsicDone = false;
    float minW = 0, maxW = 0;
    size_t paintIdx = SIZE_MAX, clipIdx = SIZE_MAX;
    float forcedW = -1, forcedH = -1;  // border-box size imposed by the parent formatting context
    float staticX = 0, staticY = 0;
    bool fixed = false, oof = false, replaced = false;
    bool fillItem = false;  // (Anon inside a replaced wrapper) the image/control takes the wrapper's width
    float naturalH = 0;  // the height of the content, whatever height the box itself was given
    // A box that clips its content: what it holds in the display list and the place kept for its scroll thumb.
    // placeScrolled() uses these once every box has its final size.
    Mark scroll0, scroll1;
    size_t thumbIdx = SIZE_MAX;
};

struct Ctx {
    const LayoutInput& in;
    LayoutOut& out;
    std::vector<Node*> boxed;
    Box* root = nullptr;
    int layer = 0;
    float maxBottom = 0;
    std::vector<Box*> scrollers;  // the boxes that clip their content
};

Mark mark(Ctx& c) { return {c.out.items.size(), c.out.links.size(), c.boxed.size(), c.out.widgets.size()}; }

void shift(Ctx& c, const Mark& a, const Mark& e, float dx, float dy, size_t skipItem = SIZE_MAX, Node* skipNode = nullptr) {
    if (dx == 0 && dy == 0) return;
    for (size_t i = a.d; i < e.d && i < c.out.items.size(); ++i)
        if (i != skipItem) {
            c.out.items[i].x += dx;
            c.out.items[i].y += dy;
        }
    for (size_t i = a.l; i < e.l && i < c.out.links.size(); ++i) {
        c.out.links[i].x += dx;
        c.out.links[i].y += dy;
    }
    for (size_t i = a.b; i < e.b && i < c.boxed.size(); ++i)
        if (c.boxed[i] != skipNode) {
            c.boxed[i]->bx += dx;
            c.boxed[i]->by += dy;
        }
    for (size_t i = a.w; i < e.w && i < c.out.widgets.size(); ++i) {
        c.out.widgets[i].x += dx;
        c.out.widgets[i].y += dy;
    }
}

void rollback(Ctx& c, const Mark& m) {
    for (size_t i = m.b; i < c.boxed.size(); ++i) c.boxed[i]->hasBox = false;
    c.boxed.resize(m.b);
    c.out.items.resize(m.d);
    c.out.links.resize(m.l);
    c.out.widgets.resize(m.w);
}

size_t push(Ctx& c, DItem&& d) {
    d.layer = (int16_t)std::clamp(c.layer, -30000, 30000);
    c.out.items.push_back(std::move(d));
    return c.out.items.size() - 1;
}

bool blockLevel(Display d) {
    return d == Display::Block || d == Display::ListItem || d == Display::Table || d == Display::RowGroup ||
           d == Display::Row || d == Display::Cell || d == Display::Flex;
}
bool isAbs(const Style* s) { return s->position == 2 || s->position == 3; }

bool containsBlock(const Node* n, int depth = 0) {
    if (depth > 48) return false;
    for (Node* k : FlatKids(n)) {
        if (k->type != Node::T::Element || !k->style) continue;
        Display d = k->style->display;
        if (d == Display::None || isAbs(k->style.get())) continue;
        if (blockLevel(d)) return true;
        if (d == Display::Inline && containsBlock(k, depth + 1)) return true;
    }
    return false;
}

std::unique_ptr<Box> mk(BK k, Node* n) {
    auto b = std::make_unique<Box>();
    b->kind = k;
    b->node = n;
    b->st = n ? n->style.get() : nullptr;
    return b;
}

std::unique_ptr<Box> buildBox(Ctx& c, Node* e, Box* cb);
void flow(Ctx& c, Box& box, Node* e, Box* cb);

// Registers an absolutely/fixed positioned element with its containing block.
Box* addAbs(Ctx& c, Node* n, Box* cb) {
    auto a = buildBox(c, n, cb);
    a->fixed = n->style->position == 3;
    if (a->fixed) c.out.fixed = true;
    a->oof = true;
    Box* raw = a.get();
    Box* owner = (a->fixed || !cb) ? c.root : cb;
    owner->abs.push_back(std::move(a));
    return raw;
}

std::unique_ptr<Box> anonFor(Box& parent, Node* e, std::vector<Node*>&& run, Box* cb) {
    auto a = std::make_unique<Box>();
    a->kind = BK::Anon;
    a->node = e;
    a->st = parent.st;
    a->inl = std::move(run);
    a->cb = cb;
    return a;
}

bool meaningfulRun(const std::vector<Node*>& run, bool pre) {
    for (Node* n : run) {
        if (n->type != Node::T::Text) return true;
        if (pre && !n->text.empty()) return true;
        for (char ch : n->text)
            if (!isws(ch)) return true;
    }
    return false;
}

void addCell(Ctx& c, Box& row, Node* cc, Box* cb) {
    auto cell = mk(BK::Cell, cc);
    cell->cb = cb;
    if (auto cs = cc->attr("colspan")) cell->span = std::clamp(atoi(cs->c_str()), 1, 64);
    flow(c, *cell, cc, cc->style->position != 0 ? cell.get() : cb);
    row.kids.push_back(std::move(cell));
}

void collectRows(Ctx& c, Box& t, Node* n, Box* cb, int depth = 0) {
    Box* anonRow = nullptr;
    for (Node* e : FlatKids(n)) {
        if (e->type != Node::T::Element || !e->style) continue;
        Display d = e->style->display;
        if (d == Display::None || e->name == "caption" || isAbs(e->style.get())) continue;
        if (d == Display::Row) {
            anonRow = nullptr;
            auto r = mk(BK::Row, e);
            for (Node* cc : FlatKids(e)) {
                if (cc->type != Node::T::Element || !cc->style || cc->style->display == Display::None) continue;
                addCell(c, *r, cc, cb);
            }
            t.kids.push_back(std::move(r));
        } else if (d == Display::Cell) {
            if (!anonRow) {
                auto r = mk(BK::Row, nullptr);
                anonRow = r.get();
                t.kids.push_back(std::move(r));
            }
            addCell(c, *anonRow, e, cb);
        } else if (depth < 8) {
            anonRow = nullptr;
            collectRows(c, t, e, cb, depth + 1);
        }
    }
}

// Children of a flex container: every element is blockified into its own item, text runs become anonymous items.
void flexKids(Ctx& c, Box& box, Node* e, Box* cb) {
    std::vector<Node*> run;
    auto flush = [&] {
        if (!run.empty() && meaningfulRun(run, false)) box.kids.push_back(anonFor(box, e, std::move(run), cb));
        run.clear();
    };
    for (Node* n : FlatKids(e)) {
        if (n->type == Node::T::Text) {
            run.push_back(n);
            continue;
        }
        if (n->type != Node::T::Element || !n->style || n->style->display == Display::None) continue;
        flush();
        if (isAbs(n->style.get())) {
            auto mkr = mk(BK::Marker, nullptr);
            mkr->target = addAbs(c, n, cb);
            box.kids.push_back(std::move(mkr));
            continue;
        }
        if (n->name == "br") continue;
        box.kids.push_back(buildBox(c, n, cb));
    }
    flush();
}

std::unique_ptr<Box> buildBox(Ctx& c, Node* e, Box* cb) {
    Display d = e->style->display;
    BK k = d == Display::Table ? BK::Table : (d == Display::Flex || d == Display::InlineFlex) ? BK::Flex : BK::Block;
    bool repl = replaced_like(e);
    if (repl) k = BK::Block;
    auto b = mk(k, e);
    b->cb = cb;
    if (!c.root) c.root = b.get();
    Box* inner = (e->style->position != 0 || c.root == b.get()) ? b.get() : cb;
    if (repl) {
        // A block-level or flex-item replaced element: wrap the element itself in an anonymous line box.
        b->replaced = true;
        std::vector<Node*> run{e};
        auto a = anonFor(*b, e, std::move(run), inner);
        a->st = (e->parent && e->parent->style) ? e->parent->style.get() : e->style.get();
        a->fillItem = true;
        b->kids.push_back(std::move(a));
    } else if (k == BK::Table) {
        collectRows(c, *b, e, inner);
    } else if (k == BK::Flex) {
        flexKids(c, *b, e, inner);
    } else {
        flow(c, *b, e, inner);
    }
    return b;
}

void flow(Ctx& c, Box& box, Node* e, Box* cb) {
    std::vector<Node*> run;
    int ordinal = 1;
    if (e->name == "ol")
        if (auto s = e->attr("start")) ordinal = atoi(s->c_str());
    bool pre = box.st && (box.st->whiteSpace == 1 || box.st->whiteSpace == 3);
    auto flush = [&] {
        if (!run.empty() && meaningfulRun(run, pre)) box.kids.push_back(anonFor(box, e, std::move(run), cb));
        run.clear();
    };
    for (Node* n : FlatKids(e)) {
        if (n->type == Node::T::Text) {
            run.push_back(n);
            continue;
        }
        if (n->type != Node::T::Element || !n->style) continue;
        Display d = n->style->display;
        if (d == Display::None) continue;
        if (isAbs(n->style.get())) {
            auto mkr = mk(BK::Marker, nullptr);
            mkr->target = addAbs(c, n, cb);
            box.kids.push_back(std::move(mkr));
            continue;
        }
        if (blockLevel(d) || (d == Display::Inline && containsBlock(n))) {
            flush();
            auto ch = buildBox(c, n, cb);
            if (d == Display::ListItem) {
                if (auto v = n->attr("value")) ordinal = atoi(v->c_str());
                ch->ordinal = ordinal++;
            }
            box.kids.push_back(std::move(ch));
        } else {
            run.push_back(n);
        }
    }
    flush();
}

void edges(Box& b, float cw) {
    if (b.kind == BK::Anon || b.kind == BK::Marker || !b.st) {
        for (int i = 0; i < 4; ++i) b.m[i] = b.b[i] = b.p[i] = 0;
        return;
    }
    const Style& s = *b.st;
    for (int i = 0; i < 4; ++i) {
        b.m[i] = (b.kind == BK::Cell || b.kind == BK::Row) ? 0 : s.margin[i].resolve(cw);
        b.p[i] = (b.kind == BK::Row || b.replaced) ? 0 : std::max(0.f, s.padding[i].resolve(cw));
        b.b[i] = (b.kind == BK::Row || b.replaced) ? 0 : s.border[i];
    }
}

float hpbOf(const Box& b) { return b.b[1] + b.b[3] + b.p[1] + b.p[3]; }
float vpbOf(const Box& b) { return b.b[0] + b.b[2] + b.p[0] + b.p[2]; }

float collapse(float a, float b) {
    if (a >= 0 && b >= 0) return std::max(a, b);
    if (a < 0 && b < 0) return std::min(a, b);
    return a + b;
}

void setBox(Ctx& c, Node* n, float x, float y, float w, float h) {
    if (!n) return;
    if (!n->hasBox) {
        n->hasBox = true;
        c.boxed.push_back(n);
    }
    n->bx = x;
    n->by = y;
    n->bw = w;
    n->bh = h;
    n->layer = (int16_t)std::clamp(c.layer, -30000, 30000);
}

void unionBox(Ctx& c, Node* n, float x, float y, float w, float h) {
    if (!n->hasBox) {
        setBox(c, n, x, y, w, h);
        return;
    }
    float r = std::max(n->bx + n->bw, x + w), bt = std::max(n->by + n->bh, y + h);
    n->bx = std::min(n->bx, x);
    n->by = std::min(n->by, y);
    n->bw = r - n->bx;
    n->bh = bt - n->by;
}

void setHeight(Ctx& c, Box& b, float h) {
    b.h = h;
    if (b.paintIdx != SIZE_MAX) c.out.items[b.paintIdx].h = h;
    if (b.clipIdx != SIZE_MAX) c.out.items[b.clipIdx].h = std::max(0.f, h - b.b[0] - b.b[2]);
    if (b.node && b.kind != BK::Anon) b.node->bh = h;
}

void moveBox(Ctx& c, Box& b, const Mark& a, const Mark& e, float dx, float dy) {
    if (dx == 0 && dy == 0) return;
    shift(c, a, e, dx, dy);
    b.x += dx;
    b.y += dy;
    if (b.firstBaseline >= 0) b.firstBaseline += dy;
}

// ---------------------------------------------------------------- inline items

bool isCjk(wchar_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3000 && c <= 0x303F);
}

std::wstring transformText(std::wstring s, uint8_t tt) {
    if (tt == 1)
        for (auto& c : s) c = (wchar_t)towupper(c);
    else if (tt == 2)
        for (auto& c : s) c = (wchar_t)towlower(c);
    else if (tt == 3) {
        bool start = true;
        for (auto& c : s) {
            if (iswspace(c))
                start = true;
            else if (start) {
                c = (wchar_t)towupper(c);
                start = false;
            }
        }
    }
    return s;
}

bool atomicLike(Item::K k) { return k == Item::Image || k == Item::Widget || k == Item::Atomic; }

void addText(Box& b, const std::string& utf8, const Style* st, Node* link, Node* inl, Color bg) {
    std::wstring w = widen(utf8);
    if (st->textTransform) w = transformText(std::move(w), st->textTransform);
    uint8_t ws = st->whiteSpace;
    auto pushItem = [&](Item::K k, std::wstring t, bool brk) {
        Item it;
        it.k = k;
        it.text = std::move(t);
        it.st = st;
        it.link = link;
        it.inl = inl;
        it.bg = bg;
        it.brk = brk;
        b.items.push_back(std::move(it));
    };
    if (ws == 1 || ws == 3) {
        std::wstring cur;
        bool brkNext = b.items.empty() || b.items.back().k == Item::Break;
        auto flushw = [&] {
            if (!cur.empty()) {
                pushItem(Item::Word, cur, brkNext || ws == 3);
                brkNext = false;
                cur.clear();
            }
        };
        for (wchar_t ch : w) {
            if (ch == L'\r') continue;
            if (ch == L'\n') {
                flushw();
                pushItem(Item::Break, L"", true);
                brkNext = true;
                continue;
            }
            if (ch == L'\t') {
                cur.append(4, L' ');
                continue;
            }
            cur += ch;
            if (ws == 3 && ch == L' ') flushw();
        }
        flushw();
        return;
    }
    auto isSp = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r' || ch == L'\f'; };
    size_t i = 0, n = w.size();
    while (i < n) {
        wchar_t ch = w[i];
        if (isSp(ch)) {
            if (ws == 4 && ch == L'\n') {
                pushItem(Item::Break, L"", true);
                ++i;
                continue;
            }
            if (!b.items.empty() && b.items.back().k != Item::Space && b.items.back().k != Item::Break)
                pushItem(Item::Space, L" ", false);
            ++i;
            continue;
        }
        size_t j = i;
        if (isCjk(ch))
            j = i + 1;
        else
            while (j < n && !isSp(w[j]) && !isCjk(w[j])) ++j;
        bool brk = b.items.empty() || b.items.back().k == Item::Space || b.items.back().k == Item::Break || isCjk(ch) ||
                   atomicLike(b.items.back().k) || (!b.items.back().text.empty() && isCjk(b.items.back().text.back()));
        pushItem(Item::Word, w.substr(i, j - i), brk);
        i = j;
    }
}

void walk(Ctx& c, Box& b, Node* n, const Style* st, Node* link, Node* inl, Color bg) {
    if (n->type == Node::T::Text) {
        addText(b, n->text, st, link, inl, bg);
        return;
    }
    if (n->type != Node::T::Element || !n->style) return;
    const Style* s = n->style.get();
    if (s->display == Display::None) return;
    if (isAbs(s) && n != b.node) {
        b.inlineAbs.push_back(addAbs(c, n, b.cb));
        return;
    }
    const std::string& nm = n->name;
    if (nm == "br") {
        Item it;
        it.k = Item::Break;
        it.st = s;
        b.items.push_back(it);
        return;
    }
    Item it;
    it.brk = true;
    it.st = s;
    it.link = link;
    it.node = n;
    it.inl = n;
    if (nm == "img") {
        int idx = -1;
        if (c.in.imgNode) {
            auto f = c.in.imgNode->find(n);
            if (f != c.in.imgNode->end()) idx = f->second;
        }
        const ImageRes* im = (idx >= 0 && c.in.images && idx < (int)c.in.images->size()) ? &(*c.in.images)[idx] : nullptr;
        if (im && im->w == 0) im = nullptr;
        float w = s->width.u == Len::Px ? s->width.v : -1, h = s->height.u == Len::Px ? s->height.v : -1;
        if (im) {
            if (w < 0 && h < 0) {
                w = (float)im->w;
                h = (float)im->h;
            } else if (w < 0)
                w = h * im->w / std::max(1, im->h);
            else if (h < 0)
                h = w * im->h / std::max(1, im->w);
        } else if (w < 0 || h < 0) {
            auto alt = n->attr("alt");
            if (alt && !alt->empty()) addText(b, *alt, s, link, n, bg);
            return;
        }
        it.k = Item::Image;
        it.w = w;
        it.h = h;
        it.img = im ? idx : -1;
        b.items.push_back(std::move(it));
        return;
    }
    if (nm == "input" || nm == "button" || nm == "select" || nm == "textarea") {
        WidgetKind wk = widget_kind(n);
        if (wk == WidgetKind::None) return;
        it.k = Item::Widget;
        float lh = s->lh();
        float padH = std::max(0.f, s->padding[1].resolve(0)) + std::max(0.f, s->padding[3].resolve(0)) + s->border[1] + s->border[3];
        float padV = std::max(0.f, s->padding[0].resolve(0)) + std::max(0.f, s->padding[2].resolve(0)) + s->border[0] + s->border[2];
        float cw = 150, ch = lh;
        switch (wk) {
            case WidgetKind::Check:
            case WidgetKind::Radio:
                it.w = it.h = 13;
                b.items.push_back(std::move(it));
                return;
            case WidgetKind::Button: cw = std::max(8.f, c.in.ts->width(*s, widget_label(n))); break;
            case WidgetKind::Select: {
                cw = 30;
                auto opts = select_options(n);
                for (size_t i = 0; i < opts.size() && i < 300; ++i)
                    cw = std::max(cw, c.in.ts->width(*s, widen(collapse_ws(text_content(opts[i])))));
                cw += 20;
                break;
            }
            case WidgetKind::TextArea: {
                int cols = n->attr("cols") ? atoi(n->attr("cols")->c_str()) : 0;
                int rows = n->attr("rows") ? atoi(n->attr("rows")->c_str()) : 0;
                cw = cols > 0 ? cols * s->fontSize * 0.55f : 180.f;
                ch = (rows > 0 ? rows : 2) * lh;
                break;
            }
            default: {
                int sz = n->attr("size") ? atoi(n->attr("size")->c_str()) : 0;
                cw = sz > 0 ? sz * s->fontSize * 0.55f : 150.f;
            }
        }
        if (s->width.u == Len::Px) cw = s->borderBox ? s->width.v - padH : s->width.v;
        if (s->height.u == Len::Px) ch = s->borderBox ? s->height.v - padV : s->height.v;
        it.w = std::max(4.f, cw + padH);
        it.h = std::max(4.f, ch + padV);
        b.items.push_back(std::move(it));
        return;
    }
    if (nm == "svg" || nm == "canvas" || nm == "video" || nm == "iframe" || nm == "object" || nm == "embed" || nm == "audio") {
        if (s->width.u == Len::Px && s->height.u == Len::Px) {
            it.k = Item::Image;
            it.w = s->width.v;
            it.h = s->height.v;
            b.items.push_back(std::move(it));
        }
        return;
    }
    Node* nlink = link;
    if (nm == "a" && n->attr("href")) nlink = n;
    if (n != b.node && (s->display == Display::InlineBlock || s->display == Display::InlineFlex || blockLevel(s->display))) {
        it.k = Item::Atomic;
        it.link = nlink;
        it.box = std::shared_ptr<Box>(buildBox(c, n, b.cb).release());
        it.box->oof = true;
        b.items.push_back(std::move(it));
        return;
    }
    Color nbg = s->bg.transparent() ? bg : s->bg;
    auto gap = [&](int side) {
        float g = std::max(0.f, s->margin[side].resolve(0)) + s->border[side] + std::max(0.f, s->padding[side].resolve(0));
        if (g <= 0.01f) return;
        Item gi;
        gi.k = Item::Gap;
        gi.st = s;
        gi.w = g;
        gi.inl = n;
        gi.bg = nbg;
        b.items.push_back(std::move(gi));
    };
    gap(3);
    for (Node* k : FlatKids(n)) walk(c, b, k, s, nlink, n, nbg);
    gap(1);
}

Node* enclosingLink(Node* n) {
    for (; n; n = n->parent)
        if (n->type == Node::T::Element && n->name == "a" && n->attr("href")) return n;
    return nullptr;
}

void buildItems(Ctx& c, Box& b) {
    if (b.itemsBuilt) return;
    b.itemsBuilt = true;
    for (Node* n : b.inl) {
        // A replaced element wrapped by its own block box lists itself; start from its parent for link lookup.
        Node* from = (n == b.node) ? n->parent : b.node;
        walk(c, b, n, b.st, enclosingLink(from), nullptr, Color{});
    }
}

void ensureW(Ctx& c, Item& it) {
    if (it.w >= 0) return;
    it.w = c.in.ts->width(*it.st, it.text);
}

bool wraps(const Style* s) { return s->whiteSpace == 0 || s->whiteSpace == 3 || s->whiteSpace == 4; }

// ---------------------------------------------------------------- intrinsic sizes

void intrinsic(Ctx& c, Box& b);

struct Cols {
    int n = 0;
    float spacing = 2;
    std::vector<float> mn, mx, fixed, pct;
};

Cols tableCols(Ctx& c, Box& t) {
    Cols cs;
    if (t.node)
        if (auto sp = t.node->attr("cellspacing")) cs.spacing = (float)std::max(0, atoi(sp->c_str()));
    for (auto& r : t.kids) {
        int n = 0;
        for (auto& cell : r->kids) n += cell->span;
        cs.n = std::max(cs.n, n);
    }
    cs.mn.assign(cs.n, 0);
    cs.mx.assign(cs.n, 0);
    cs.fixed.assign(cs.n, 0);
    cs.pct.assign(cs.n, 0);
    for (int pass = 0; pass < 2; ++pass) {
        for (auto& r : t.kids) {
            int col = 0;
            for (auto& cp : r->kids) {
                Box& cell = *cp;
                int span = std::min(cell.span, cs.n - col);
                if (span <= 0) break;
                intrinsic(c, cell);
                if (pass == 0 && span == 1) {
                    cs.mn[col] = std::max(cs.mn[col], cell.minW);
                    cs.mx[col] = std::max(cs.mx[col], cell.maxW);
                    if (cell.st && cell.st->width.u == Len::Px) cs.fixed[col] = std::max(cs.fixed[col], cell.st->width.v);
                    if (cell.st && cell.st->width.u == Len::Pct) cs.pct[col] = std::max(cs.pct[col], cell.st->width.v);
                } else if (pass == 1 && span > 1) {
                    float smn = 0, smx = 0;
                    for (int i = col; i < col + span; ++i) {
                        smn += cs.mn[i];
                        smx += cs.mx[i];
                    }
                    float gap = cs.spacing * (span - 1);
                    if (cell.minW > smn + gap)
                        for (int i = col; i < col + span; ++i) cs.mn[i] += (cell.minW - smn - gap) / span;
                    if (cell.maxW > smx + gap)
                        for (int i = col; i < col + span; ++i) cs.mx[i] += (cell.maxW - smx - gap) / span;
                }
                col += span;
            }
        }
    }
    for (int i = 0; i < cs.n; ++i) cs.mx[i] = std::max(cs.mx[i], cs.mn[i]);
    return cs;
}

void intrinsic(Ctx& c, Box& b) {
    if (b.intrinsicDone) return;
    b.intrinsicDone = true;
    edges(b, 0);
    float hpb = hpbOf(b) + (b.kind == BK::Cell ? 0 : b.m[1] + b.m[3]);
    float mn = 0, mx = 0;
    switch (b.kind) {
        case BK::Marker: break;
        case BK::Anon: {
            buildItems(c, b);
            float line = 0, word = 0;
            for (auto& it : b.items) {
                if (it.k == Item::Break) {
                    mx = std::max(mx, line);
                    line = 0;
                    word = 0;
                    continue;
                }
                if (it.k == Item::Atomic) {
                    intrinsic(c, *it.box);
                    line += it.box->maxW;
                    bool wr = wraps(b.st);
                    word = (wr ? 0 : word) + it.box->minW;
                    mn = std::max(mn, word);
                    if (wr) word = 0;
                    continue;
                }
                ensureW(c, it);
                if (it.k == Item::Space) {
                    if (line > 0) line += it.w;
                    word = wraps(it.st) ? 0 : word + it.w;
                    continue;
                }
                // Percentage-sized replaced content contributes nothing to the minimum.
                bool pct = (it.k == Item::Image || it.k == Item::Widget) && it.st &&
                           (it.st->width.u == Len::Pct || it.st->maxWidth.u == Len::Pct);
                float w = it.w;
                line += w;
                word = (it.brk && wraps(it.st)) ? (pct ? 0 : w) : word + (pct ? 0 : w);
                mn = std::max(mn, word);
            }
            mx = std::max(mx, line);
            break;
        }
        case BK::Table: {
            Cols cs = tableCols(c, b);
            float sp = cs.spacing * (cs.n + 1);
            for (int i = 0; i < cs.n; ++i) {
                mn += cs.mn[i];
                mx += std::max(cs.mx[i], cs.fixed[i]);
            }
            mn += sp;
            mx += sp;
            break;
        }
        case BK::Flex: {
            bool colDir = b.st->flexDir >= 2;
            float sumMin = 0, sumMax = 0;
            int cnt = 0;
            for (auto& k : b.kids) {
                if (k->kind == BK::Marker) continue;
                intrinsic(c, *k);
                float kmn = k->minW, kmx = k->maxW;
                // What an item adds to the container's width (Flexbox 9.9.3) is its own intrinsic width, capped by
                // its flex base size when it cannot grow and floored by it when it cannot shrink.
                if (!colDir && k->kind != BK::Anon && k->st && k->st->flexBasis.u == Len::Px) {
                    float base = k->st->flexBasis.v + (k->st->borderBox ? 0 : hpbOf(*k)) + k->m[1] + k->m[3];
                    if (k->st->flexGrow <= 0) kmn = std::min(kmn, base), kmx = std::min(kmx, base);
                    if (k->st->flexShrink <= 0) kmn = std::max(kmn, base), kmx = std::max(kmx, base);
                }
                if (colDir) {
                    mn = std::max(mn, kmn);
                    mx = std::max(mx, kmx);
                } else {
                    sumMin += kmn;
                    sumMax += kmx;
                    mn = std::max(mn, kmn);
                    ++cnt;
                }
            }
            if (!colDir) {
                float g = b.st->gapCol * std::max(0, cnt - 1);
                mx = sumMax + g;
                if (!b.st->flexWrap) mn = sumMin + g;
            }
            break;
        }
        default:
            for (auto& k : b.kids) {
                if (k->kind == BK::Marker) continue;
                intrinsic(c, *k);
                mn = std::max(mn, k->minW);
                mx = std::max(mx, k->maxW);
            }
    }
    if (b.st && b.kind != BK::Anon && !b.replaced) {
        if (b.st->width.u == Len::Px) {
            float w = b.st->width.v;
            if (b.st->borderBox) w -= hpbOf(b);
            mn = b.st->overflow != 0 ? w : std::max(mn, w);
            mx = std::max(w, mn);
        } else if (b.st->overflow != 0) {
            mn = 0;
        }
        if (b.st->maxWidth.u == Len::Px) {
            float w = b.st->maxWidth.v - (b.st->borderBox ? hpbOf(b) : 0);
            mx = std::min(mx, std::max(mn, w));
        }
        if (b.st->minWidth.u == Len::Px) {
            float w = b.st->minWidth.v - (b.st->borderBox ? hpbOf(b) : 0);
            mn = std::max(mn, w);
        }
    }
    b.minW = mn + hpb;
    b.maxW = std::max(mx, mn) + hpb;
}

// ---------------------------------------------------------------- layout

void layoutBox(Ctx& c, Box& b, float cx, float top, float cw, bool center = false);

std::wstring roman(int n, bool upper) {
    static const std::pair<int, const wchar_t*> t[] = {{1000, L"m"}, {900, L"cm"}, {500, L"d"}, {400, L"cd"}, {100, L"c"}, {90, L"xc"},
                                                        {50, L"l"},   {40, L"xl"},  {10, L"x"},  {9, L"ix"},   {5, L"v"},   {4, L"iv"}, {1, L"i"}};
    std::wstring r;
    for (auto& [v, s] : t)
        while (n >= v) {
            r += s;
            n -= v;
        }
    if (upper)
        for (auto& ch : r) ch = (wchar_t)towupper(ch);
    return r;
}

void emitText(Ctx& c, float x, float y, float w, float h, const Style& st, const std::wstring& txt) {
    DItem d;
    d.k = DItem::K::Text;
    d.x = x;
    d.y = y;
    d.w = w;
    d.h = h;
    d.c = faded(st.color, st);
    d.tl = c.in.ts->layout(st, txt, st.underline, st.strike);
    if (d.tl) push(c, std::move(d));
}

void addLink(Ctx& c, float x, float y, float w, float h, Node* a) {
    c.out.links.push_back({x, y, w, h, a, (int16_t)std::clamp(c.layer, -30000, 30000)});
}

float layoutInline(Ctx& c, Box& b, float x0, float y0, float avail) {
    buildItems(c, b);
    for (Box* a : b.inlineAbs) {
        a->staticX = x0;
        a->staticY = y0;
    }
    const Style& bs = *b.st;
    float sasc, sdesc;
    c.in.ts->metrics(bs, sasc, sdesc);
    struct Placed {
        Item* it;
        float x;
    };
    std::vector<Placed> line;
    float x = 0, y = y0;
    bool first = true;

    auto vshift = [](const Style* s) { return s->vAlign == 4 ? s->fontSize * 0.25f : s->vAlign == 5 ? -s->fontSize * 0.4f : 0.f; };
    auto baseOf = [&](Item& it) -> float {
        if (it.k == Item::Atomic) return it.base;
        if (it.k == Item::Widget) {
            WidgetKind wk = widget_kind(it.node);
            if (wk == WidgetKind::Check || wk == WidgetKind::Radio) return it.h - 2;
            if (wk == WidgetKind::TextArea) return it.h;
            float a, d;
            c.in.ts->metrics(*it.st, a, d);
            return (it.h - (a + d)) / 2 + a;
        }
        return it.h;
    };

    auto commit = [&](bool forced) {
        while (!line.empty() && line.back().it->k == Item::Space) line.pop_back();
        if (line.empty() && !forced) {
            x = 0;
            return;
        }
        float lineW = line.empty() ? 0 : line.back().x + line.back().it->w;
        float lh = bs.lh();
        float A = sasc + (lh - (sasc + sdesc)) / 2, D = lh - A;
        for (auto& p : line) {
            Item& it = *p.it;
            if (it.k == Item::Gap) continue;
            if (atomicLike(it.k)) {
                float bse = baseOf(it);
                A = std::max(A, bse);
                D = std::max(D, it.h - bse);
            } else {
                float a, d;
                c.in.ts->metrics(*it.st, a, d);
                float l = it.st->lh(), half = (l - (a + d)) / 2, sh = vshift(it.st);
                A = std::max(A, a + half - sh);
                D = std::max(D, d + half + sh);
            }
        }
        float baseline = y + A, lineH = A + D;
        float off = 0;
        if (bs.textAlign == 1) off = avail - lineW;
        else if (bs.textAlign == 2) off = (avail - lineW) / 2;
        if (off < 0) off = 0;
        size_t i = 0;
        while (i < line.size()) {
            Item& it = *line[i].it;
            float fx = x0 + off + line[i].x;
            if (it.k == Item::Gap) {
                ++i;
                continue;
            }
            if (it.k == Item::Atomic) {
                float topY = baseline - it.base;
                shift(c, it.m0, it.m1, fx, topY);
                if (it.link) addLink(c, fx, topY, it.w, it.h, it.link);
                for (Node* p = it.inl ? it.inl->parent : nullptr; p && p != b.node; p = p->parent) unionBox(c, p, fx, topY, it.w, it.h);
                ++i;
                continue;
            }
            if (it.k == Item::Image || it.k == Item::Widget) {
                float iy = baseline - baseOf(it);
                if (!it.st->hidden) {
                    if (it.k == Item::Image) {
                        if (!it.st->bg.transparent()) {
                            DItem bgd;
                            bgd.k = DItem::K::Rect;
                            bgd.x = fx;
                            bgd.y = iy;
                            bgd.w = it.w;
                            bgd.h = it.h;
                            bgd.c = faded(it.st->bg, *it.st);
                            push(c, std::move(bgd));
                        }
                        DItem d;
                        d.k = DItem::K::Image;
                        d.x = fx;
                        d.y = iy;
                        d.w = it.w;
                        d.h = it.h;
                        d.img = it.img;
                        push(c, std::move(d));
                    } else {
                        c.out.widgets.push_back({it.node, fx, iy, it.w, it.h, it.st, (int16_t)std::clamp(c.layer, -30000, 30000)});
                        DItem d;
                        d.k = DItem::K::Widget;
                        d.x = fx;
                        d.y = iy;
                        d.w = it.w;
                        d.h = it.h;
                        d.img = (int)c.out.widgets.size() - 1;
                        push(c, std::move(d));
                    }
                }
                if (it.link) addLink(c, fx, iy, it.w, it.h, it.link);
                for (Node* p = it.inl; p && p != b.node; p = p->parent) unionBox(c, p, fx, iy, it.w, it.h);
                if (it.inl == b.node && it.inl) setBox(c, it.inl, fx, iy, it.w, it.h);
                ++i;
                continue;
            }
            std::wstring txt = it.text;
            float wsum = it.w;
            size_t j = i + 1;
            while (j < line.size()) {
                Item& n = *line[j].it;
                if (n.k != Item::Word && n.k != Item::Space) break;
                if (n.st != it.st || n.link != it.link || n.inl != it.inl) break;
                txt += n.text;
                wsum = line[j].x + n.w - line[i].x;
                ++j;
            }
            float a, de;
            c.in.ts->metrics(*it.st, a, de);
            float ty = baseline - a + vshift(it.st);
            if (!it.bg.transparent() && !it.st->hidden) {
                DItem d;
                d.k = DItem::K::Rect;
                d.x = fx;
                d.y = ty;
                d.w = wsum;
                d.h = a + de;
                d.c = faded(it.bg, *it.st);
                push(c, std::move(d));
            }
            if (!it.st->hidden) emitText(c, fx, ty, wsum, a + de, *it.st, txt);
            if (it.link) addLink(c, fx, ty, wsum, a + de, it.link);
            for (Node* p = it.inl; p && p != b.node; p = p->parent) unionBox(c, p, fx, ty, wsum, a + de);
            i = j;
        }
        if (first) {
            b.firstBaseline = baseline;
            first = false;
        }
        y += lineH;
        line.clear();
        x = 0;
    };

    for (auto& it : b.items) {
        switch (it.k) {
            case Item::Break: commit(true); break;
            case Item::Space:
                if (line.empty()) break;
                ensureW(c, it);
                line.push_back({&it, x});
                x += it.w;
                break;
            case Item::Gap:
                line.push_back({&it, x});
                x += it.w;
                break;
            case Item::Atomic: {
                Box& ab = *it.box;
                ab.forcedW = ab.forcedH = -1;
                if (!ab.st->width.definite()) {
                    intrinsic(c, ab);
                    edges(ab, avail);
                    float hm = ab.m[1] + ab.m[3], hpb = hpbOf(ab);
                    float fit = std::min(std::max(ab.minW, avail), ab.maxW) - hm;
                    if (ab.st->maxWidth.definite()) fit = std::min(fit, ab.st->maxWidth.resolve(avail) + (ab.st->borderBox ? 0 : hpb));
                    if (ab.st->minWidth.definite()) fit = std::max(fit, ab.st->minWidth.resolve(avail) + (ab.st->borderBox ? 0 : hpb));
                    ab.forcedW = std::max(0.f, fit);
                } else {
                    edges(ab, avail);
                }
                it.m0 = mark(c);
                layoutBox(c, ab, 0, ab.m[0], avail);
                it.m1 = mark(c);
                it.w = ab.m[3] + ab.w + ab.m[1];
                it.h = ab.y + ab.h + ab.m[2];
                it.base = (ab.firstBaseline >= 0 && ab.st->overflow == 0) ? ab.firstBaseline : it.h;
                if (!line.empty() && x + it.w > avail + 0.01f && wraps(&bs)) commit(false);
                line.push_back({&it, x});
                x += it.w;
                break;
            }
            default: {
                if ((it.k == Item::Image || it.k == Item::Widget) && it.st) {
                    // Percentage sizes and max-width are relative to the line box.
                    float ow = it.w, oh = it.h;
                    if (it.st->width.u == Len::Pct) it.w = it.st->width.resolve(avail);
                    if (it.st->maxWidth.u == Len::Pct || it.st->maxWidth.u == Len::Px) it.w = std::min(it.w, it.st->maxWidth.resolve(avail));
                    if (b.fillItem && avail > 0) {
                        WidgetKind wk = it.k == Item::Widget ? widget_kind(it.node) : WidgetKind::None;
                        if (wk != WidgetKind::Check && wk != WidgetKind::Radio) it.w = avail;
                    }
                    if (it.k == Item::Image && it.w != ow && ow > 0 && it.st->height.u != Len::Px) it.h = oh * it.w / ow;
                }
                ensureW(c, it);
                if (!line.empty() && x + it.w > avail + 0.01f && it.brk && wraps(it.st)) commit(false);
                line.push_back({&it, x});
                x += it.w;
            }
        }
    }
    if (!line.empty()) commit(false);
    return y - y0;
}

float layoutTable(Ctx& c, Box& t, float ix, float iy, float availW) {
    Cols cs = tableCols(c, t);
    int n = cs.n;
    float sp = cs.spacing;
    if (n == 0) return 0;
    float avail = availW - sp * (n + 1);
    std::vector<float> w(n, 0);
    std::vector<int> autos;
    float assigned = 0;
    for (int i = 0; i < n; ++i) {
        if (cs.pct[i] > 0)
            w[i] = std::max(cs.mn[i], cs.pct[i] * avail / 100.f);
        else if (cs.fixed[i] > 0)
            w[i] = std::max(cs.mn[i], cs.fixed[i]);
        else {
            autos.push_back(i);
            continue;
        }
        assigned += w[i];
    }
    float rem = avail - assigned, smx = 0, smn = 0;
    for (int a : autos) {
        smx += cs.mx[a];
        smn += cs.mn[a];
    }
    if (autos.empty()) {
        if (rem > 0 && assigned > 0)
            for (int i = 0; i < n; ++i) w[i] += rem * w[i] / assigned;
    } else if (rem >= smx) {
        float extra = rem - smx;
        for (int a : autos) w[a] = cs.mx[a] + (smx > 0 ? extra * cs.mx[a] / smx : extra / autos.size());
    } else if (rem >= smn) {
        float range = smx - smn;
        for (int a : autos) w[a] = cs.mn[a] + (range > 0 ? (rem - smn) * (cs.mx[a] - cs.mn[a]) / range : 0);
    } else {
        for (int a : autos) w[a] = cs.mn[a];
    }

    float y = iy + sp;
    for (auto& rp : t.kids) {
        Box& r = *rp;
        size_t rowPaint = SIZE_MAX;
        if (r.st && !r.st->bg.transparent() && !r.st->hidden) {
            DItem d;
            d.k = DItem::K::Rect;
            d.c = faded(r.st->bg, *r.st);
            rowPaint = push(c, std::move(d));
        }
        float x = ix + sp, rowH = 0;
        int col = 0;
        struct CI {
            Box* cell;
            Mark m0, m1;
        };
        std::vector<CI> infos;
        for (auto& cp : r.kids) {
            Box& cell = *cp;
            if (col >= n) break;
            int span = std::min(cell.span, n - col);
            float cwid = sp * (span - 1);
            for (int i = col; i < col + span; ++i) cwid += w[i];
            Mark a = mark(c);
            layoutBox(c, cell, x, y, cwid);
            infos.push_back({&cell, a, mark(c)});
            rowH = std::max(rowH, cell.h);
            x += cwid + sp;
            col += span;
        }
        if (r.st && r.st->height.u == Len::Px) rowH = std::max(rowH, r.st->height.v);
        for (auto& ci : infos) {
            Box& cell = *ci.cell;
            float extra = rowH - cell.h;
            if (extra > 0.01f) {
                uint8_t va = cell.st ? cell.st->vAlign : 2;
                float dy = (va == 0 || va == 1) ? 0 : va == 3 ? extra : extra / 2;
                if (dy > 0) shift(c, ci.m0, ci.m1, 0, dy, cell.paintIdx, cell.node);
                setHeight(c, cell, rowH);
            }
        }
        if (rowPaint != SIZE_MAX) {
            DItem& d = c.out.items[rowPaint];
            d.x = ix;
            d.y = y;
            d.w = availW;
            d.h = rowH;
        }
        if (r.node) setBox(c, r.node, ix, y, availW, rowH);
        y += rowH + sp;
    }
    return y - iy;
}

bool hasGrowKid(const Box& b) {
    for (auto& k : b.kids)
        if (k->kind != BK::Anon && k->kind != BK::Marker && k->st && k->st->flexGrow > 0) return true;
    return false;
}

// Flexbox: single- and multi-line rows, columns, grow/shrink, gaps, justify-content, align-items, auto margins.
// `lower` is the main (column) or cross (row) size the container must reach and `upper` the one it may not pass; -1
// stands for none. They are equal when the container's height is definite.
float layoutFlex(Ctx& c, Box& b, float ix, float iy, float W, float lower, float upper) {
    const Style& s = *b.st;
    bool col = s.flexDir >= 2, rev = s.flexDir == 1 || s.flexDir == 3;
    std::vector<Box*> items;
    for (auto& k : b.kids) {
        if (k->kind == BK::Marker) {
            if (k->target) {
                k->target->staticX = ix;
                k->target->staticY = iy;
            }
            continue;
        }
        items.push_back(k.get());
    }
    if (rev) std::reverse(items.begin(), items.end());
    size_t n = items.size();
    if (!n) return std::max(0.f, lower);
    auto fst = [](Box* it) -> const Style* { return it->kind == BK::Anon ? nullptr : it->st; };
    auto alignOf = [&](Box* it) -> uint8_t {
        const Style* st = fst(it);
        return (st && st->alignSelf != 255) ? st->alignSelf : s.alignItems;
    };

    if (col) {
        float gap = s.gapRow, y = iy;
        struct CI {
            Box* b;
            Mark m0, m1;
        };
        std::vector<CI> ci;
        for (Box* it : items) {
            const Style* st = fst(it);
            uint8_t al = alignOf(it);
            it->oof = true;
            it->forcedW = it->forcedH = -1;
            bool defW = st && st->width.definite();
            bool mlA = st && st->margin[3].u == Len::Auto, mrA = st && st->margin[1].u == Len::Auto;
            if (!defW && (al != 0 || mlA || mrA)) {
                intrinsic(c, *it);
                edges(*it, W);
                it->forcedW = std::max(0.f, std::min(std::max(it->minW, W), it->maxW) - it->m[1] - it->m[3]);
            } else if (!defW) {
                edges(*it, W);
                it->forcedW = std::max(0.f, W - it->m[1] - it->m[3]);
            } else {
                edges(*it, W);
            }
            if (st && it->forcedW >= 0 && st->maxWidth.definite())
                it->forcedW = std::min(it->forcedW, st->maxWidth.resolve(W) + (st->borderBox ? 0 : hpbOf(*it)));
            if (st) {
                const Len& hb = st->flexBasis.u == Len::Px ? st->flexBasis : st->height;
                if (hb.u == Len::Px && !it->replaced) it->forcedH = hb.v + (st->borderBox ? 0 : vpbOf(*it));
            }
            Mark a = mark(c);
            layoutBox(c, *it, ix, y + it->m[0], W);
            float freeX = W - (it->m[3] + it->w + it->m[1]), dx = 0;
            if (freeX > 0.01f) {
                if (mlA && mrA) dx = freeX / 2;
                else if (mlA) dx = freeX;
                else if (mrA) dx = 0;
                else if (al == 2) dx = freeX / 2;
                else if (al == 3) dx = freeX;
            }
            moveBox(c, *it, a, mark(c), dx, 0);
            ci.push_back({it, a, mark(c)});
            y = it->y + it->h + it->m[2] + gap;
            if (b.firstBaseline < 0 && it->firstBaseline >= 0) b.firstBaseline = it->firstBaseline;
        }
        float total = y - gap - iy;
        if (lower > total + 0.5f) {
            float freeY = lower - total, sumGrow = 0;
            for (auto& e : ci)
                if (const Style* st = fst(e.b)) sumGrow += st->flexGrow;
            if (sumGrow > 0) {
                float acc = 0;
                for (auto& e : ci) {
                    moveBox(c, *e.b, e.m0, e.m1, 0, acc);
                    const Style* st = fst(e.b);
                    float g = st ? st->flexGrow : 0;
                    if (g > 0) {
                        float extra = freeY * g / std::max(1.f, sumGrow);
                        setHeight(c, *e.b, e.b->h + extra);
                        acc += extra;
                    }
                }
            } else {
                float lead = 0, between = 0;
                switch (s.justify) {
                    case 1: lead = freeY / 2; break;
                    case 2: lead = freeY; break;
                    case 3: between = n > 1 ? freeY / (n - 1) : 0; break;
                    case 4: between = freeY / n; lead = between / 2; break;
                    case 5: between = freeY / (n + 1); lead = between; break;
                }
                float off = lead;
                for (auto& e : ci) {
                    moveBox(c, *e.b, e.m0, e.m1, 0, off);
                    off += between;
                }
            }
            total = lower;
        } else if (upper >= 0 && total > upper + 0.5f) {
            // Negative free space (Flexbox 9.7): items give up height in proportion to flex-shrink times their inner
            // size, each down to its minimum: its min-height, or when that is automatic (4.5) nothing for a scroll
            // container and the height of its content for any other item.
            struct Part {
                float size, target, floor, weight;
                bool frozen;
            };
            std::vector<Part> parts;
            for (auto& e : ci) {
                const Style* st = fst(e.b);
                float size = e.b->h, edge = vpbOf(*e.b);
                float floor = e.b->naturalH + edge;
                if (st && st->minHeight.u == Len::Px) floor = st->minHeight.v + (st->borderBox ? 0 : edge);
                else if (st && st->overflow != 0) floor = edge;
                float weight = (st ? st->flexShrink : 1) * std::max(0.f, size - edge);
                parts.push_back({size, size, std::min(floor, size), weight, weight <= 0});
            }
            float over = total - upper;
            for (size_t round = 0; round <= parts.size(); ++round) {
                float given = 0, weights = 0;
                for (auto& part : parts) {
                    if (part.frozen) given += part.size - part.target;
                    else weights += part.weight;
                }
                if (weights <= 0 || over - given <= 0) break;
                bool violated = false;
                for (auto& part : parts) {
                    if (part.frozen) continue;
                    part.target = part.size - (over - given) * part.weight / weights;
                    if (part.target < part.floor) {
                        part.target = part.floor;
                        part.frozen = violated = true;
                    }
                }
                if (!violated) break;
            }
            float cut = 0;
            for (size_t i = 0; i < ci.size(); ++i) {
                moveBox(c, *ci[i].b, ci[i].m0, ci[i].m1, 0, -cut);
                if (parts[i].target >= parts[i].size) continue;
                setHeight(c, *ci[i].b, parts[i].target);
                cut += parts[i].size - parts[i].target;
            }
            total -= cut;
        }
        return std::max(0.f, total);
    }

    // ---- row direction
    float gap = s.gapCol, gapCross = s.gapRow;
    struct FI {
        Box* b = nullptr;
        const Style* st = nullptr;
        float base = 0, hyp = 0, mn = 0, mx = 0, main = 0, hm = 0, grow = 0, shrink = 1, outerH = 0;
        bool frozen = false, mlA = false, mrA = false;
        Mark m0, m1;
    };
    std::vector<FI> fi(n);
    for (size_t i = 0; i < n; ++i) {
        Box* it = items[i];
        FI& f = fi[i];
        f.b = it;
        f.st = fst(it);
        it->oof = true;
        it->forcedH = -1;
        intrinsic(c, *it);
        edges(*it, W);
        f.hm = it->m[1] + it->m[3];
        float hpb = hpbOf(*it);
        float cMin = std::max(0.f, it->minW - f.hm), cMax = std::max(cMin, it->maxW - f.hm);
        const Style* st = f.st;
        f.grow = st ? st->flexGrow : 0;
        f.shrink = st ? st->flexShrink : 1;
        bool defBasis = st && st->flexBasis.definite(), defW = st && st->width.definite() && !it->replaced;
        if (defBasis) f.base = st->flexBasis.resolve(W) + (st->borderBox ? 0 : hpb);
        else if (defW) f.base = st->width.resolve(W) + (st->borderBox ? 0 : hpb);
        else f.base = cMax;
        f.base = std::max(f.base, hpb);
        if (st && st->minWidth.definite()) f.mn = st->minWidth.resolve(W) + (st->borderBox ? 0 : hpb);
        else if (st && st->overflow != 0) f.mn = hpb;
        else f.mn = (defW || defBasis) ? std::min(cMin, f.base) : cMin;
        f.mx = (st && st->maxWidth.definite()) ? st->maxWidth.resolve(W) + (st->borderBox ? 0 : hpb) : 1e9f;
        if (f.mx < f.mn) f.mx = f.mn;
        f.hyp = std::clamp(f.base, f.mn, f.mx);
        f.mlA = st && st->margin[3].u == Len::Auto;
        f.mrA = st && st->margin[1].u == Len::Auto;
        f.main = f.hyp;
    }
    std::vector<std::pair<size_t, size_t>> lines;
    if (!s.flexWrap) {
        lines.push_back({0, n});
    } else {
        size_t st0 = 0;
        float sum = 0;
        for (size_t i = 0; i < n; ++i) {
            float o = fi[i].hyp + fi[i].hm;
            if (i > st0 && sum + gap + o > W + 0.5f) {
                lines.push_back({st0, i});
                st0 = i;
                sum = o;
            } else {
                sum += (i > st0 ? gap : 0) + o;
            }
        }
        lines.push_back({st0, n});
    }
    bool single = lines.size() == 1;
    float y = iy;
    for (auto [a, e] : lines) {
        float gaps = gap * (float)(e - a - 1);
        float sumHyp = gaps;
        for (size_t i = a; i < e; ++i) sumHyp += fi[i].hyp + fi[i].hm;
        bool growing = sumHyp < W;
        for (size_t i = a; i < e; ++i) {
            FI& f = fi[i];
            f.main = f.hyp;
            f.frozen = (growing ? f.grow : f.shrink) <= 0 || (growing ? f.base > f.hyp : f.base < f.hyp);
        }
        for (int iter = 0; iter < 12; ++iter) {
            float used = gaps, sumF = 0, sumG = 0;
            for (size_t i = a; i < e; ++i) {
                FI& f = fi[i];
                used += (f.frozen ? f.main : f.base) + f.hm;
                if (!f.frozen) {
                    sumF += growing ? f.grow : f.shrink * f.base;
                    sumG += f.grow;
                }
            }
            if (sumF <= 0) break;
            float freeS = W - used;
            if (growing && sumG < 1) freeS *= sumG;
            if ((growing && freeS <= 0) || (!growing && freeS >= 0)) {
                for (size_t i = a; i < e; ++i)
                    if (!fi[i].frozen) fi[i].main = std::clamp(fi[i].base, fi[i].mn, fi[i].mx);
                break;
            }
            bool violated = false;
            for (size_t i = a; i < e; ++i) {
                FI& f = fi[i];
                if (f.frozen) continue;
                float t = f.base + freeS * (growing ? f.grow : f.shrink * f.base) / sumF;
                float cl = std::clamp(t, f.mn, f.mx);
                f.main = cl;
                if (std::fabs(cl - t) > 0.01f) {
                    f.frozen = true;
                    violated = true;
                }
            }
            if (!violated) break;
        }
        float usedMain = gaps;
        int autos = 0;
        for (size_t i = a; i < e; ++i) {
            usedMain += fi[i].main + fi[i].hm;
            autos += (fi[i].mlA ? 1 : 0) + (fi[i].mrA ? 1 : 0);
        }
        float freeM = W - usedMain, lead = 0, between = 0, perAuto = 0;
        size_t cnt = e - a;
        if (freeM > 0.01f) {
            if (autos) perAuto = freeM / autos;
            else
                switch (s.justify) {
                    case 0: lead = rev ? freeM : 0; break;
                    case 1: lead = freeM / 2; break;
                    case 2: lead = rev ? 0 : freeM; break;
                    case 3: between = cnt > 1 ? freeM / (cnt - 1) : 0; break;
                    case 4: between = freeM / cnt; lead = between / 2; break;
                    case 5: between = freeM / (cnt + 1); lead = between; break;
                }
        }

        float lineH = 0;
        std::vector<float> forcedH(cnt, -1.f);
        auto pass = [&] {
            float x = ix + lead;
            lineH = 0;
            for (size_t i = a; i < e; ++i) {
                FI& f = fi[i];
                Box* it = f.b;
                if (f.mlA) x += perAuto;
                edges(*it, W);
                it->forcedW = std::max(0.f, f.main);
                it->forcedH = forcedH[i - a];
                if (it->forcedH < 0 && f.st && f.st->height.u == Len::Px && !it->replaced)
                    it->forcedH = f.st->height.v + (f.st->borderBox ? 0 : vpbOf(*it));
                f.m0 = mark(c);
                layoutBox(c, *it, x, y + it->m[0], W);
                f.m1 = mark(c);
                f.outerH = it->m[0] + it->h + it->m[2];
                lineH = std::max(lineH, f.outerH);
                x += f.hm + it->w + gap + between;
                if (f.mrA) x += perAuto;
            }
            if (single && lower > lineH) lineH = lower;
        };
        pass();
        // A stretched column container that distributes space needs a second pass with its final height.
        bool redo = false;
        for (size_t i = a; i < e; ++i) {
            FI& f = fi[i];
            Box* it = f.b;
            if (lineH - f.outerH <= 0.5f || alignOf(it) != 0 || (f.st && f.st->height.definite())) continue;
            if (it->kind == BK::Flex && it->st && it->st->flexDir >= 2 && (it->st->justify != 0 || hasGrowKid(*it))) {
                forcedH[i - a] = lineH - it->m[0] - it->m[2];
                redo = true;
            }
        }
        if (redo) {
            float keep = lineH;
            rollback(c, fi[a].m0);
            pass();
            lineH = std::max(lineH, keep);
        }
        for (size_t i = a; i < e; ++i) {
            FI& f = fi[i];
            Box* it = f.b;
            float extra = lineH - f.outerH;
            if (b.firstBaseline < 0 && it->firstBaseline >= 0) b.firstBaseline = it->firstBaseline;
            if (extra <= 0.01f) continue;
            const Style* st = f.st;
            bool mtA = st && st->margin[0].u == Len::Auto, mbA = st && st->margin[2].u == Len::Auto;
            uint8_t al = alignOf(it);
            if (mtA || mbA) {
                moveBox(c, *it, f.m0, f.m1, 0, mtA && mbA ? extra / 2 : mtA ? extra : 0);
            } else if (al == 0 && !(st && st->height.definite()) && !it->replaced) {
                setHeight(c, *it, lineH - it->m[0] - it->m[2]);
            } else {
                moveBox(c, *it, f.m0, f.m1, 0, al == 2 ? extra / 2 : al == 3 ? extra : 0);
            }
        }
        y += lineH + gapCross;
    }
    return std::max(0.f, y - gapCross - iy);
}

void layoutAbs(Ctx& c, Box& cbBox, Box& A) {
    const Style* s = A.st;
    float px, py, pw, ph;
    // A fixed box is held by the viewport, wherever the page is scrolled to (CSS Positioned Layout 3, section 2.1).
    float viewportY = A.fixed ? c.in.scrollY : 0;
    if (A.fixed || &cbBox == c.root) {
        px = 0;
        py = viewportY;
        pw = c.in.vw;
        ph = c.in.vh;
    } else {
        px = cbBox.x + cbBox.b[3];
        py = cbBox.y + cbBox.b[0];
        pw = cbBox.w - cbBox.b[1] - cbBox.b[3];
        ph = cbBox.h - cbBox.b[0] - cbBox.b[2];
    }
    edges(A, pw);
    bool L = s->inset[3].definite(), R = s->inset[1].definite(), T = s->inset[0].definite(), Bm = s->inset[2].definite();
    float l = s->inset[3].resolve(pw), r = s->inset[1].resolve(pw), t = s->inset[0].resolve(ph), bo = s->inset[2].resolve(ph);
    A.forcedW = A.forcedH = -1;
    A.oof = true;
    float hm = A.m[1] + A.m[3], hpb = hpbOf(A);
    if (!s->width.definite() || A.replaced) {
        if (L && R && !A.replaced) {
            A.forcedW = std::max(0.f, pw - l - r - hm);
        } else {
            intrinsic(c, A);
            edges(A, pw);
            hm = A.m[1] + A.m[3];
            float avail = std::max(0.f, pw - (L ? l : 0) - (R ? r : 0));
            A.forcedW = std::max(0.f, std::min(std::max(A.minW, avail), A.maxW) - hm);
        }
        if (s->maxWidth.definite()) A.forcedW = std::min(A.forcedW, s->maxWidth.resolve(pw) + (s->borderBox ? 0 : hpb));
        if (s->minWidth.definite()) A.forcedW = std::max(A.forcedW, s->minWidth.resolve(pw) + (s->borderBox ? 0 : hpb));
    }
    if (!s->height.definite() && T && Bm && !A.replaced) A.forcedH = std::max(0.f, ph - t - bo - A.m[0] - A.m[2]);
    else if (s->height.u == Len::Pct && !A.replaced) A.forcedH = std::max(0.f, s->height.resolve(ph) + (s->borderBox ? 0 : vpbOf(A)));
    float bw = A.forcedW >= 0 ? A.forcedW : s->width.resolve(pw) + (s->borderBox ? 0 : hpb);
    float cx = L ? px + l : R ? px + pw - r - bw - hm : A.staticX;
    if (L && R && s->margin[3].u == Len::Auto && s->margin[1].u == Len::Auto) cx = px + l + std::max(0.f, (pw - l - r - bw) / 2);
    float topY = T ? py + t + A.m[0] : (Bm ? 0 : viewportY + A.staticY + A.m[0]);
    Mark a0 = mark(c);
    layoutBox(c, A, cx, topY, pw);
    if (!T && Bm) moveBox(c, A, a0, mark(c), 0, (py + ph - bo - A.m[2] - A.h) - A.y);
    else if (T && Bm && s->height.definite() && s->margin[0].u == Len::Auto && s->margin[2].u == Len::Auto)
        moveBox(c, A, a0, mark(c), 0, std::max(0.f, (ph - t - bo - A.h) / 2));
    if (!s->hidden && A.x + A.w > 0 && A.x < c.in.vw && A.w > 0 && A.h > 0 && !A.fixed) c.maxBottom = std::max(c.maxBottom, A.y + A.h);
}

void layoutBox(Ctx& c, Box& b, float cx, float top, float cw, bool center) {
    edges(b, cw);
    const Style* s = b.st;
    b.paintIdx = b.clipIdx = SIZE_MAX;
    b.firstBaseline = -1;
    bool own = s && b.kind != BK::Anon && b.kind != BK::Marker;
    Mark m0 = mark(c);
    int savedLayer = c.layer;
    if (own && s->position != 0 && &b != c.root) {
        int L = s->zAuto ? 1 : (s->zIndex >= 0 ? s->zIndex + 1 : s->zIndex);
        c.layer = L < 0 ? L : std::max(c.layer, L);
    }
    float hpb = hpbOf(b), vpb = vpbOf(b);
    bool normal = own && b.kind != BK::Cell && !b.oof;
    bool autoW = true;
    float contentW;
    if (b.forcedW >= 0) {
        contentW = b.forcedW - hpb;
        autoW = false;
    } else if (b.kind == BK::Cell) {
        contentW = cw - hpb;
    } else if (own && s->width.definite() && !b.replaced) {
        contentW = s->width.resolve(cw);
        if (s->borderBox) contentW -= hpb;
        autoW = false;
    } else {
        contentW = cw - b.m[1] - b.m[3] - hpb;
    }
    if (own && b.kind != BK::Cell && b.forcedW < 0 && !b.replaced) {
        if (s->maxWidth.definite()) {
            float mx = s->maxWidth.resolve(cw);
            if (s->borderBox) mx -= hpb;
            if (contentW > mx) {
                contentW = mx;
                autoW = false;
            }
        }
        if (s->minWidth.definite()) {
            float mn = s->minWidth.resolve(cw);
            if (s->borderBox) mn -= hpb;
            if (contentW < mn) contentW = mn;
        }
        if (b.kind == BK::Table && !s->width.definite()) {
            intrinsic(c, b);
            edges(b, cw);
            float ownW = hpbOf(b) + b.m[1] + b.m[3];
            float want = std::max(b.minW - ownW, std::min(contentW, b.maxW - ownW));
            if (want < contentW) {
                contentW = want;
                autoW = false;
            }
        }
    }
    if (b.replaced && own && b.forcedW < 0) {
        // Block-level replaced element: the box hugs the image/control so auto margins can centre it.
        intrinsic(c, b);
        edges(b, cw);
        float want = s->width.u == Len::Pct ? s->width.resolve(cw) : b.maxW - b.m[1] - b.m[3];
        if (s->maxWidth.definite()) want = std::min(want, s->maxWidth.resolve(cw));
        if (want < contentW) {
            contentW = want;
            autoW = false;
        }
    }
    contentW = std::max(0.f, contentW);
    if (normal && !autoW) {
        float freeSpace = cw - (contentW + hpb);
        bool ml = s->margin[3].u == Len::Auto, mr = s->margin[1].u == Len::Auto;
        if (ml && mr) b.m[3] = b.m[1] = std::max(0.f, freeSpace / 2);
        else if (ml) b.m[3] = std::max(0.f, freeSpace - b.m[1]);
        else if (center && !mr) b.m[3] += std::max(0.f, freeSpace - b.m[3] - b.m[1]) / 2;
    }
    b.x = cx + b.m[3];
    b.y = top;
    b.w = contentW + hpb;
    float ix = b.x + b.b[3] + b.p[3], iy = top + b.b[0] + b.p[0];

    if (own && !b.replaced && !s->hidden && (!s->bg.transparent() || b.b[0] > 0 || b.b[1] > 0 || b.b[2] > 0 || b.b[3] > 0)) {
        DItem d;
        d.k = DItem::K::Box;
        d.c = faded(s->bg, *s);
        d.radius = s->radius;
        for (int i = 0; i < 4; ++i) {
            d.bw[i] = b.b[i];
            d.bc[i] = faded(s->borderColor[i], *s);
        }
        b.paintIdx = push(c, std::move(d));
    }
    bool clips = own && !b.replaced && s->overflow != 0 && &b != c.root && !(b.node && b.node->name == "body");
    if (clips) {
        DItem d;
        d.k = DItem::K::PushClip;
        b.clipIdx = push(c, std::move(d));
        b.scroll0 = mark(c);
    }

    float defH = -1;
    if (b.forcedH >= 0) defH = std::max(0.f, b.forcedH - vpb);
    else if (own && !b.replaced && s->height.u == Len::Px) defH = std::max(0.f, s->height.v - (s->borderBox ? vpb : 0));
    float minH = (own && !b.replaced && s->minHeight.u == Len::Px) ? std::max(0.f, s->minHeight.v - (s->borderBox ? vpb : 0)) : 0;
    float maxH = (own && !b.replaced && s->maxHeight.u == Len::Px) ? std::max(0.f, s->maxHeight.v - (s->borderBox ? vpb : 0)) : -1;
    // CSS 2 section 10.7: a height is held to max-height first and then raised to min-height.
    auto bounded = [&](float h) { return std::max(maxH >= 0 ? std::min(h, maxH) : h, minH); };

    float contentH = 0;
    switch (b.kind) {
        case BK::Marker: break;
        case BK::Anon: contentH = layoutInline(c, b, ix, iy, contentW); break;
        case BK::Table: contentH = layoutTable(c, b, ix, iy, contentW); break;
        case BK::Flex:
            if (defH >= 0) contentH = layoutFlex(c, b, ix, iy, contentW, bounded(defH), bounded(defH));
            else contentH = layoutFlex(c, b, ix, iy, contentW, minH > 0 ? minH : -1.f, maxH);
            break;
        default: {
            float y = iy, pend = 0;
            bool firstKid = true;
            for (auto& k : b.kids) {
                if (k->kind == BK::Marker) {
                    if (k->target) {
                        k->target->staticX = ix;
                        k->target->staticY = y + (firstKid ? 0 : pend);
                    }
                    continue;
                }
                edges(*k, contentW);
                float mt = k->m[0];
                float gap = firstKid ? mt : collapse(pend, mt);
                layoutBox(c, *k, ix, y + gap, contentW, s && s->centerBlocks);
                y = k->y + k->h;
                pend = k->m[2];
                firstKid = false;
                if (b.firstBaseline < 0 && k->firstBaseline >= 0) b.firstBaseline = k->firstBaseline;
            }
            contentH = firstKid ? 0 : std::max(0.f, y + pend - iy);
        }
    }
    b.naturalH = contentH;
    contentH = bounded(defH >= 0 ? defH : contentH);
    b.h = contentH + vpb;
    if (b.paintIdx != SIZE_MAX) {
        DItem& d = c.out.items[b.paintIdx];
        d.x = b.x;
        d.y = b.y;
        d.w = b.w;
        d.h = b.h;
    }
    if (own && b.node && !b.replaced) setBox(c, b.node, b.x, b.y, b.w, b.h);

    if (b.kind == BK::Block && own && s->display == Display::ListItem && s->listStyle != 4 && !s->hidden) {
        std::wstring m;
        int o = b.ordinal;
        switch (s->listStyle) {
            case 0: m = L"•"; break;
            case 1: m = L"◦"; break;
            case 2: m = L"▪"; break;
            case 3: m = std::to_wstring(o) + L"."; break;
            case 5: m = std::wstring(1, wchar_t(L'a' + (std::max(1, o) - 1) % 26)) + L"."; break;
            case 6: m = std::wstring(1, wchar_t(L'A' + (std::max(1, o) - 1) % 26)) + L"."; break;
            case 7: m = roman(std::max(1, o), false) + L"."; break;
            case 8: m = roman(std::max(1, o), true) + L"."; break;
        }
        float a, d;
        c.in.ts->metrics(*s, a, d);
        float mw = c.in.ts->width(*s, m);
        float base = b.firstBaseline >= 0 ? b.firstBaseline : iy + a;
        Style ms = *s;
        ms.underline = ms.strike = false;
        emitText(c, ix - mw - s->fontSize * 0.45f, base - a, mw, a + d, ms, m);
    }

    for (size_t i = 0; i < b.abs.size(); ++i) layoutAbs(c, b, *b.abs[i]);

    if (clips) {
        b.scroll1 = mark(c);
        DItem thumb;  // empty until placeScrolled() finds that the box scrolls
        thumb.k = DItem::K::Rect;
        b.thumbIdx = push(c, std::move(thumb));
        c.scrollers.push_back(&b);
        DItem& d = c.out.items[b.clipIdx];
        d.x = b.x + b.b[3];
        d.y = b.y + b.b[0];
        d.w = std::max(0.f, b.w - b.b[1] - b.b[3]);
        d.h = std::max(0.f, b.h - b.b[0] - b.b[2]);
        DItem e;
        e.k = DItem::K::PopClip;
        e.x = d.x;
        e.y = d.y;
        push(c, std::move(e));
    }
    if (own && s->position == 1) {
        float dx = s->inset[3].definite() ? s->inset[3].resolve(cw) : s->inset[1].definite() ? -s->inset[1].resolve(cw) : 0;
        float dy = s->inset[0].u == Len::Px ? s->inset[0].v : s->inset[2].u == Len::Px ? -s->inset[2].v : 0;
        shift(c, m0, mark(c), dx, dy);
    }
    c.layer = savedLayer;
}

// Reorders the display list by paint layer. Clip groups (PushClip..PopClip) move as a unit.
void sortRange(std::vector<DItem>& src, size_t a, size_t e, std::vector<DItem>& dst) {
    struct U {
        size_t a, e;
        int layer;
    };
    std::vector<U> units;
    bool mixed = false;
    for (size_t i = a; i < e;) {
        size_t j = i + 1;
        if (src[i].k == DItem::K::PushClip) {
            int depth = 0;
            for (j = i; j < e; ++j) {
                if (src[j].k == DItem::K::PushClip) ++depth;
                else if (src[j].k == DItem::K::PopClip && --depth == 0) break;
            }
            j = j < e ? j + 1 : e;
        }
        if (!units.empty() && units.back().layer != src[i].layer) mixed = true;
        units.push_back({i, j, src[i].layer});
        i = j;
    }
    if (mixed) std::stable_sort(units.begin(), units.end(), [](const U& x, const U& y) { return x.layer < y.layer; });
    for (auto& u : units) {
        if (u.e - u.a < 2 || src[u.a].k != DItem::K::PushClip) {
            for (size_t i = u.a; i < u.e; ++i) dst.push_back(std::move(src[i]));
        } else {
            bool closed = src[u.e - 1].k == DItem::K::PopClip;
            dst.push_back(std::move(src[u.a]));
            sortRange(src, u.a + 1, closed ? u.e - 1 : u.e, dst);
            if (closed) dst.push_back(std::move(src[u.e - 1]));
        }
    }
}

// Scroll containers (CSS Overflow 3). Once every box has its final size, this records how far each clipping box
// can scroll, holds its scroll offset within that and moves its content up by the offset. An `overflow: auto` box
// that can scroll gets a thumb at its right edge that shows where it is.
void placeScrolled(Ctx& c) {
    std::sort(c.scrollers.begin(), c.scrollers.end());  // a box that was laid out twice is listed twice
    c.scrollers.erase(std::unique(c.scrollers.begin(), c.scrollers.end()), c.scrollers.end());
    for (Box* b : c.scrollers) {
        Node* n = b->node;
        n->scrollRange = std::max(0.f, b->naturalH - (b->h - vpbOf(*b)));
        n->scrollTop = std::clamp(n->scrollTop, 0.f, n->scrollRange);
        shift(c, b->scroll0, b->scroll1, 0, -n->scrollTop, SIZE_MAX, n);
        if (n->scrollRange <= 0 || b->st->overflow != 2) continue;
        const DItem& port = c.out.items[b->clipIdx];
        DItem& thumb = c.out.items[b->thumbIdx];
        thumb.w = 5;
        thumb.h = std::max(20.f, port.h * port.h / (port.h + n->scrollRange));
        thumb.x = port.x + port.w - thumb.w - 2;
        thumb.y = port.y + (port.h - thumb.h) * n->scrollTop / n->scrollRange;
        thumb.c = {0, 0, 0, 0.35f};
    }
}

}  // namespace

LayoutOut layout_document(const LayoutInput& in) {
    LayoutOut out;
    if (!in.doc || !in.doc->html || !in.doc->html->style) return out;
    std::vector<Node*> stack{in.doc->root.get()};
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        n->hasBox = false;
        n->scrollRange = 0;
        for (auto& k : n->kids) stack.push_back(k.get());
        if (n->shadow) stack.push_back(n->shadow.get());
    }
    Ctx c{in, out};
    Node* html = in.doc->html;
    const Style* hs = html->style.get();
    if (!hs->bg.transparent())
        out.canvas = hs->bg;
    else if (in.doc->body && in.doc->body->style && !in.doc->body->style->bg.transparent())
        out.canvas = in.doc->body->style->bg;
    if (out.canvas.a < 1) {
        float a = out.canvas.a;
        out.canvas = {out.canvas.r * a + (1 - a), out.canvas.g * a + (1 - a), out.canvas.b * a + (1 - a), 1};
    }
    if (hs->display == Display::None) return out;
    auto root = buildBox(c, html, nullptr);
    edges(*root, in.vw);
    layoutBox(c, *root, 0, root->m[0], in.vw);
    out.height = std::max(root->y + root->h + root->m[2], c.maxBottom);
    placeScrolled(c);

    bool layered = false;
    for (auto& it : out.items)
        if (it.layer != 0) {
            layered = true;
            break;
        }
    if (layered) {
        std::vector<DItem> sorted;
        sorted.reserve(out.items.size());
        sortRange(out.items, 0, out.items.size(), sorted);
        out.items.swap(sorted);
        std::stable_sort(out.links.begin(), out.links.end(), [](const LinkArea& x, const LinkArea& y) { return x.layer < y.layer; });
    }
    return out;
}

}  // namespace tb
