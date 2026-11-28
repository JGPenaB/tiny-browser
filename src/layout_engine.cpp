#include <cstdlib>
#include <deque>

#include "anim.h"
#include <cwctype>

#include "css.h"
#include "layout.h"
#include "url.h"
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

enum class BK : uint8_t { Block, Anon, Table, Row, Cell, Flex, Grid, Marker };

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
    int svg = -1;              // Image: an svg element, LayoutOut::svgs[svg]
    float aspect = 0;          // Image of an svg: its viewBox width / height, 0 when it has none
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
    int span = 1;
    bool intrinsicDone = false;
    float minW = 0, maxW = 0;
    float contentMinW = 0;  // minW as the content alone makes it, before the box's own width and limits count
    size_t paintIdx = SIZE_MAX, clipIdx = SIZE_MAX;
    size_t outlineIdx = SIZE_MAX;  // the outline, drawn round the border box once its size is known
    size_t bgFirst = SIZE_MAX, bgEnd = SIZE_MAX;  // the background image items, then (at bgEnd) the borders over them
    float forcedW = -1, forcedH = -1;  // border-box size imposed by the parent formatting context
    float staticX = 0, staticY = 0;
    bool fixed = false, oof = false, replaced = false;
    bool fillItem = false;  // (Anon inside a replaced wrapper) the image/control takes the wrapper's width
    bool firstLines = false;  // (Anon) its first line is the first line of its block: text-indent applies
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
    // The content height of the block whose lines are being laid out, when it is definite: what a percentage height
    // of a replaced element on those lines is resolved against (CSS 2 section 10.5). -1 when it is not definite.
    float lineBlockH = -1;
    // The floats of the block formatting context being laid out (CSS 2 9.5): their margin boxes, in document
    // coordinates. A box that makes a context of its own lays its floats out in a list of its own.
    struct Float {
        uint8_t side;  // 1 left, 2 right
        float x0, x1, y0, y1;
    };
    std::vector<Float>* floats = nullptr;
    // Stickily positioned boxes (CSS Position 3, 3.4) and what they painted: moved once the whole page is laid out
    struct Sticky {
        Box* b;
        Mark m0, m1;
    };
    std::vector<Sticky> stickies;
};

// The room the floats leave between x0 and x1 for something from y to y + h: [left, right]
std::pair<float, float> floatBand(const Ctx& c, float y, float h, float x0, float x1) {
    float L = x0, R = x1;
    if (!c.floats) return {L, R};
    for (const auto& f : *c.floats) {
        if (f.y1 <= y || f.y0 >= y + std::max(h, 0.01f) || f.x1 <= x0 || f.x0 >= x1) continue;
        if (f.side == 1) L = std::max(L, f.x1);
        else R = std::min(R, f.x0);
    }
    return {L, R};
}
// The first float bottom below y among the floats beside y: where the room can grow. Infinity when none is.
float nextFloatBottom(const Ctx& c, float y, float x0, float x1) {
    float best = INFINITY;
    if (!c.floats) return best;
    for (const auto& f : *c.floats)
        if (f.y1 > y && f.y0 <= y && f.x1 > x0 && f.x0 < x1) best = std::min(best, f.y1);
    return best;
}
// Where clearance puts a box (CSS 2 9.5.2): below the floats of the sides it clears
float clearBelow(const Ctx& c, uint8_t clear) {
    float y = -INFINITY;
    if (!c.floats || !clear) return y;
    for (const auto& f : *c.floats)
        if (f.side & clear) y = std::max(y, f.y1);
    return y;
}

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
    while (!c.stickies.empty() && c.stickies.back().m0.d >= m.d) c.stickies.pop_back();
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
           d == Display::Row || d == Display::Cell || d == Display::Flex || d == Display::Grid;
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
    // A transformed ancestor holds fixed boxes too (CSS Transforms 1, section 2): they are then placed like
    // absolutely positioned ones, inside it.
    for (Box* k = cb; a->fixed && k; k = k->cb)
        if (k->st && k->st->transform) {
            a->fixed = false;
            cb = k;
        }
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
    BK k = d == Display::Table                                   ? BK::Table
           : (d == Display::Flex || d == Display::InlineFlex) ? BK::Flex
           : (d == Display::Grid || d == Display::InlineGrid) ? BK::Grid
                                                              : BK::Block;
    bool repl = replaced_like(e);
    if (repl) k = BK::Block;
    auto b = mk(k, e);
    b->cb = cb;
    if (!c.root) c.root = b.get();
    // a positioned or transformed box holds its absolutely positioned descendants
    Box* inner = (e->style->position != 0 || e->style->transform || c.root == b.get()) ? b.get() : cb;
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
    } else if (k == BK::Flex || k == BK::Grid) {  // flex items and grid items are made the same way
        flexKids(c, *b, e, inner);
    } else {
        flow(c, *b, e, inner);
    }
    return b;
}

void flow(Ctx& c, Box& box, Node* e, Box* cb) {
    std::vector<Node*> run;
    bool pre = box.st && (box.st->whiteSpace == 1 || box.st->whiteSpace == 3);
    auto flush = [&] {
        if (!run.empty() && meaningfulRun(run, pre)) {
            bool firstInBlock = true;
            for (auto& k : box.kids) firstInBlock = firstInBlock && k->kind == BK::Marker;
            box.kids.push_back(anonFor(box, e, std::move(run), cb));
            box.kids.back()->firstLines = firstInBlock;
        }
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
            box.kids.push_back(buildBox(c, n, cb));
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

// The background image items and the borders over them have the box's own geometry
void syncBackground(Ctx& c, Box& b) {
    if (b.bgFirst == SIZE_MAX || b.paintIdx == SIZE_MAX) return;
    const DItem& box = c.out.items[b.paintIdx];
    for (size_t i = b.bgFirst; i <= b.bgEnd && i < c.out.items.size(); ++i) {
        c.out.items[i].x = box.x;
        c.out.items[i].y = box.y;
        c.out.items[i].w = box.w;
        c.out.items[i].h = box.h;
    }
}

void setHeight(Ctx& c, Box& b, float h) {
    if (b.outlineIdx != SIZE_MAX) c.out.items[b.outlineIdx].h += h - b.h;
    b.h = h;
    if (b.paintIdx != SIZE_MAX) c.out.items[b.paintIdx].h = h;
    syncBackground(c, b);
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
            // No picture (HTML 15.4.2): an img whose alternative text says something is that text. Any other is a
            // replaced element with no natural size, so its own width and height (a percentage is resolved on the
            // line) are what it gets, and it shows nothing.
            auto alt = n->attr("alt");
            if (alt && !alt->empty()) {
                addText(b, *alt, s, link, n, bg);
                return;
            }
            it.k = Item::Image;
            it.w = std::max(0.f, w);
            it.h = std::max(0.f, h);
            it.img = -2;
            b.items.push_back(std::move(it));
            return;
        }
        it.k = Item::Image;
        it.w = w;
        it.h = h;
        it.img = im ? idx : -1;
        b.items.push_back(std::move(it));
        return;
    }
    if (nm == "input" || nm == "select" || nm == "textarea") {  // a <button> lays its content out as an inline-block
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
    if (nm == "svg") {
        // An svg element is a replaced element (SVG 2, section 8.2): its width and height come from CSS, which its
        // own width and height attributes feed; auto is 100% for the width and, with a viewBox, follows its aspect
        // ratio for the height, else 150px. Sizes that depend on the line are settled there.
        it.k = Item::Image;
        it.img = -2;
        it.svg = (int)c.out.svgs.size();
        c.out.svgs.push_back(svg_build(n, c.in.doc, c.out.svgShapes));
        it.aspect = svg_aspect(n);
        it.w = s->width.u == Len::Px ? s->width.v : 0;
        it.h = s->height.u == Len::Px ? s->height.v : -1;
        if (it.h < 0 && s->width.u == Len::Px) it.h = it.aspect > 0 ? it.w / it.aspect : 150;
        b.items.push_back(std::move(it));
        return;
    }
    if (nm == "canvas" || nm == "video" || nm == "iframe" || nm == "object" || nm == "embed" || nm == "audio") {
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
    if (n != b.node && (s->display == Display::InlineBlock || s->display == Display::InlineFlex || s->display == Display::InlineGrid ||
                        blockLevel(s->display))) {
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

// ---------------------------------------------------------------- grid (CSS Grid 1)

// Where the items of a grid container go (CSS Grid 1, section 8): the tracks of both axes and each item's lines
struct GridPlan {
    struct Item {
        Box* b;
        int line[2][2];  // [axis][start, end]: axis 0 rows, 1 columns; lines from 0
    };
    std::vector<Item> items;
    std::vector<GridTrack> tracks[2];
};

// An axis of the explicit grid with its auto repeat written out `repeats` times, and the names of its lines.
// [autoFrom, autoTo) are the tracks the repeat made.
void expandTracks(const GridTrackList& L, int repeats, std::vector<GridTrack>& tracks, std::vector<std::vector<std::string>>& names,
                  int& autoFrom, int& autoTo) {
    autoFrom = autoTo = -1;
    if (L.autoAt < 0) {
        tracks = L.tracks;
        names = L.names;
        return;
    }
    tracks.clear();
    names.clear();
    auto add = [](std::vector<std::string>& to, const std::vector<std::string>& from) { to.insert(to.end(), from.begin(), from.end()); };
    for (int i = 0; i < L.autoAt; ++i) {
        names.push_back(L.names[i]);
        tracks.push_back(L.tracks[i]);
    }
    names.push_back(L.names[L.autoAt]);
    autoFrom = (int)tracks.size();
    for (int r = 0; r < repeats; ++r) {
        add(names.back(), L.autoNames[0]);
        for (size_t k = 0; k < L.autoTracks.size(); ++k) {
            tracks.push_back(L.autoTracks[k]);
            names.push_back(L.autoNames[k + 1]);
        }
    }
    autoTo = (int)tracks.size();
    add(names.back(), L.names[L.autoAt + 1]);
    for (size_t i = L.autoAt; i < L.tracks.size(); ++i) {
        tracks.push_back(L.tracks[i]);
        names.push_back(L.names[i + 2]);
    }
}

// How many times repeat(auto-fill | auto-fit) repeats (7.2.3.2): as often as fits in `avail`, at least once; once
// when the size of the container is not known
int autoRepeats(const GridTrackList& L, float avail, float gap) {
    if (L.autoAt < 0) return 0;
    if (avail < 0) return 1;
    auto fixedSize = [&](const GridTrack& t) {
        const GridBreadth& b = t.max.k == GridBreadth::Length ? t.max : t.min;
        return b.k == GridBreadth::Length ? b.len.resolve(avail) : 0.f;
    };
    float other = 0, each = 0;
    for (auto& t : L.tracks) other += fixedSize(t);
    for (auto& t : L.autoTracks) each += fixedSize(t);
    float step = each + gap * (float)L.autoTracks.size();
    if (step <= 0) return 1;
    float room = avail - other - gap * ((float)L.tracks.size() - 1);
    return std::clamp((int)std::floor((room + 0.01f) / step), 1, 10000);
}

GridPlan placeGrid(Box& b, float W, float H) {
    const Style& s = *b.st;
    static const GridTemplate none;
    const GridTemplate& g = s.grid ? *s.grid : none;
    GridPlan plan;
    std::vector<std::vector<std::string>> names[2];
    int autoFrom[2], autoTo[2];
    expandTracks(g.rows, autoRepeats(g.rows, H, s.gapRow), plan.tracks[0], names[0], autoFrom[0], autoTo[0]);
    expandTracks(g.cols, autoRepeats(g.cols, W, s.gapCol), plan.tracks[1], names[1], autoFrom[1], autoTo[1]);
    int templ[2] = {(int)plan.tracks[0].size(), (int)plan.tracks[1].size()};
    int nExplicit[2] = {std::max(templ[0], g.areaRows), std::max(templ[1], g.areaCols)};

    // the line a placement names (8.3), or INT_MIN when it is auto or a span
    auto lineOf = [&](int axis, const GridLine& gl, bool end) -> int {
        if (gl.span || gl.isAuto()) return INT_MIN;
        if (gl.name.empty()) return gl.line > 0 ? gl.line - 1 : std::max(0, nExplicit[axis] + 1 + gl.line);
        if (gl.line == 0)  // an area of that name: its edge on this side
            for (auto& a : g.areas)
                if (a.name == gl.name) return axis == 0 ? (end ? a.row1 : a.row0) : (end ? a.col1 : a.col0);
        std::vector<int> found;  // the lines of that name: named ones, and those the areas name x-start and x-end
        for (int i = 0; i < (int)names[axis].size(); ++i)
            if (std::find(names[axis][i].begin(), names[axis][i].end(), gl.name) != names[axis][i].end()) found.push_back(i);
        for (auto& a : g.areas) {
            int lo = axis == 0 ? a.row0 : a.col0, hi = axis == 0 ? a.row1 : a.col1;
            if (gl.name == a.name + "-start") found.push_back(lo);
            if (gl.name == a.name + "-end") found.push_back(hi);
        }
        std::sort(found.begin(), found.end());
        found.erase(std::unique(found.begin(), found.end()), found.end());
        int n = gl.line ? gl.line : 1;
        // past the lines there are, every implicit line has the name
        if (n > 0) return n <= (int)found.size() ? found[n - 1] : nExplicit[axis] + n - (int)found.size();
        return -n <= (int)found.size() ? found[found.size() + n] : 0;
    };
    struct Pending {
        Box* b;
        int lo[2], span[2];
        bool fixed[2];
    };
    std::vector<Pending> pend;
    for (auto& k : b.kids) {
        if (k->kind == BK::Marker) continue;
        Pending p{k.get(), {0, 0}, {1, 1}, {false, false}};
        const Style* st = k->kind == BK::Anon ? nullptr : k->st;
        for (int axis = 0; axis < 2; ++axis) {
            if (!st) continue;
            const GridLine* gl = axis == 0 ? st->gridRow : st->gridCol;
            int a = lineOf(axis, gl[0], false), e = lineOf(axis, gl[1], true);
            if (a != INT_MIN && e != INT_MIN) {
                if (e < a) std::swap(a, e);
                if (e == a) e = a + 1;
            } else if (a != INT_MIN) {
                e = a + std::max(1, gl[1].span);
            } else if (e != INT_MIN) {
                a = std::max(0, e - std::max(1, gl[0].span));
                if (e <= a) e = a + 1;
            } else {
                p.span[axis] = std::max(1, gl[0].span ? gl[0].span : gl[1].span);
                continue;
            }
            p.lo[axis] = a;
            p.span[axis] = e - a;
            p.fixed[axis] = true;
        }
        pend.push_back(p);
    }

    // Auto-placement (8.5). The major axis is the one auto-flow adds tracks to: rows, or columns for `column`.
    int major = g.flowColumn ? 1 : 0, minor = 1 - major;
    std::vector<std::vector<char>> occ;  // [major][minor]
    auto fits = [&](int a0, int a1, int b0, int b1) {
        for (int a = a0; a < a1 && a < (int)occ.size(); ++a)
            for (int m = b0; m < b1 && m < (int)occ[a].size(); ++m)
                if (occ[a][m]) return false;
        return true;
    };
    auto take = [&](Pending& p) {
        int a1 = p.lo[major] + p.span[major], b1 = p.lo[minor] + p.span[minor];
        if ((int)occ.size() < a1) occ.resize(a1);
        for (int a = p.lo[major]; a < a1; ++a) {
            if ((int)occ[a].size() < b1) occ[a].resize(b1, 0);
            for (int m = p.lo[minor]; m < b1; ++m) occ[a][m] = 1;
        }
        p.fixed[0] = p.fixed[1] = true;
    };
    for (auto& p : pend)  // 1. the items placed in both axes
        if (p.fixed[0] && p.fixed[1]) take(p);
    std::unordered_map<int, int> cursorOf;  // 2. the items locked to a major line, in their own track
    for (auto& p : pend) {
        if (p.fixed[minor] || !p.fixed[major]) continue;
        int m = g.dense ? 0 : cursorOf[p.lo[major]];
        while (!fits(p.lo[major], p.lo[major] + p.span[major], m, m + p.span[minor])) ++m;
        p.lo[minor] = m;
        cursorOf[p.lo[major]] = m + p.span[minor];
        take(p);
    }
    int minorCount = nExplicit[minor];  // 3. the minor tracks of the implicit grid
    for (auto& p : pend) minorCount = std::max(minorCount, p.fixed[minor] ? p.lo[minor] + p.span[minor] : p.span[minor]);
    int curMajor = 0, curMinor = 0;  // 4. the rest, in order
    for (auto& p : pend) {
        if (p.fixed[major] && p.fixed[minor]) continue;
        if (p.fixed[minor]) {
            if (g.dense) curMajor = 0;
            else if (curMinor > p.lo[minor]) ++curMajor;
            curMinor = p.lo[minor];
            while (!fits(curMajor, curMajor + p.span[major], p.lo[minor], p.lo[minor] + p.span[minor])) ++curMajor;
        } else {
            if (g.dense) curMajor = curMinor = 0;
            while (curMinor + p.span[minor] > minorCount ||
                   !fits(curMajor, curMajor + p.span[major], curMinor, curMinor + p.span[minor])) {
                if (++curMinor + p.span[minor] > minorCount) {
                    ++curMajor;
                    curMinor = 0;
                }
            }
            p.lo[minor] = curMinor;
        }
        p.lo[major] = curMajor;
        take(p);
        curMinor = p.lo[minor] + p.span[minor];
    }

    // The tracks: those of the template, then those of grid-auto-rows / -columns over and over (7.6)
    int count[2] = {nExplicit[0], nExplicit[1]};
    for (auto& p : pend)
        for (int axis = 0; axis < 2; ++axis) count[axis] = std::max(count[axis], p.lo[axis] + p.span[axis]);
    for (int axis = 0; axis < 2; ++axis) {
        const auto& autoTracks = axis == 0 ? g.autoRows : g.autoCols;
        for (int i = templ[axis]; i < count[axis]; ++i) plan.tracks[axis].push_back(autoTracks[(i - templ[axis]) % autoTracks.size()]);
    }
    for (auto& p : pend) {
        GridPlan::Item it{p.b, {{p.lo[0], p.lo[0] + p.span[0]}, {p.lo[1], p.lo[1] + p.span[1]}}};
        plan.items.push_back(it);
    }
    // auto-fit: the repeated tracks no item is in collapse away (7.2.3.2)
    for (int axis = 0; axis < 2; ++axis) {
        const GridTrackList& L = axis == 0 ? g.rows : g.cols;
        if (!L.autoFit || autoFrom[axis] < 0) continue;
        for (int t = autoTo[axis] - 1; t >= autoFrom[axis]; --t) {
            bool used = false;
            for (auto& it : plan.items) used = used || (it.line[axis][0] <= t && t < it.line[axis][1]);
            if (used) continue;
            plan.tracks[axis].erase(plan.tracks[axis].begin() + t);
            for (auto& it : plan.items)
                for (int& l : it.line[axis])
                    if (l > t) --l;
        }
    }
    return plan;
}

// The track sizing algorithm (11.3-11.8) for one axis. min and max are each item's minimum and maximum contributions
// along it (outer sizes). avail < 0 when the size of the container along the axis is not known. mode 0 lays out;
// 1 and 2 size the tracks under a min-content or max-content constraint, for the container's intrinsic sizes.
std::vector<float> sizeTracks(const std::vector<GridTrack>& defs, const GridPlan& plan, int axis, const std::vector<float>& minC,
                              const std::vector<float>& maxC, float avail, float gap, bool stretch, int mode) {
    size_t n = defs.size();
    const float inf = INFINITY;
    std::vector<float> base(n, 0), limit(n, inf), flex(n, 0);
    auto fixedOf = [&](const GridBreadth& b, float& out) {
        if (b.k != GridBreadth::Length || (b.len.u == Len::Pct && avail < 0)) return false;
        out = b.len.resolve(avail);
        return true;
    };
    auto intrinsicMin = [&](size_t t) {
        float v;
        return !fixedOf(defs[t].min, v);
    };
    for (size_t t = 0; t < n; ++t) {
        float v;
        if (fixedOf(defs[t].min, v)) base[t] = v;
        if (defs[t].max.k == GridBreadth::Fr) flex[t] = defs[t].max.fr;
        else if (fixedOf(defs[t].max, v)) limit[t] = std::max(v, base[t]);
    }
    float gaps = gap * (float)(n > 0 ? n - 1 : 0);
    // the items' contributions, those spanning one track first, then by span (11.5)
    std::vector<size_t> order(plan.items.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto spanOf = [&](size_t i) { return plan.items[i].line[axis][1] - plan.items[i].line[axis][0]; };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return spanOf(a) < spanOf(b); });
    std::vector<float> grownLimit(n, -1);  // the limits that items set, for tracks with an intrinsic maximum
    for (size_t i : order) {
        int t0 = plan.items[i].line[axis][0], t1 = plan.items[i].line[axis][1];
        bool crossesFlex = false;
        for (int t = t0; t < t1; ++t) crossesFlex = crossesFlex || flex[t] > 0;
        float lo = minC[i], hi = maxC[i];
        if (mode == 2) lo = hi;  // under a max-content constraint an automatic minimum takes the max-content size
        if (t1 - t0 == 1) {
            size_t t = t0;
            if (intrinsicMin(t)) base[t] = std::max(base[t], defs[t].min.k == GridBreadth::MaxContent ? hi : lo);
            if (flex[t] == 0 && limit[t] == inf) {
                float want = defs[t].max.k == GridBreadth::MinContent ? lo : hi;
                if (defs[t].fitContent.definite()) want = std::max(lo, std::min(hi, defs[t].fitContent.resolve(std::max(0.f, avail))));
                grownLimit[t] = std::max(grownLimit[t], want);
            }
            continue;
        }
        // an item across several tracks: what its spanned tracks lack goes to those that can take it, evenly
        float have = gap * (float)(t1 - t0 - 1), haveLimit = have;
        std::vector<size_t> into, intoLimit;
        for (int t = t0; t < t1; ++t) {
            have += base[t];
            haveLimit += limit[t] == inf ? std::max(base[t], grownLimit[t]) : limit[t];
            if (intrinsicMin(t) && (!crossesFlex || flex[t] > 0)) into.push_back(t);
            if (!crossesFlex && limit[t] == inf && flex[t] == 0) intoLimit.push_back(t);
        }
        if (lo > have && !into.empty())
            for (size_t t : into) base[t] += (lo - have) / (float)into.size();
        if (hi > haveLimit && !intoLimit.empty())
            for (size_t t : intoLimit) grownLimit[t] = std::max(grownLimit[t], std::max(base[t], grownLimit[t]) + (hi - haveLimit) / (float)intoLimit.size());
    }
    for (size_t t = 0; t < n; ++t) {
        if (limit[t] == inf && flex[t] == 0) limit[t] = std::max(base[t], grownLimit[t]);
        if (flex[t] == 0) limit[t] = std::max(limit[t], base[t]);
    }
    // maximize the tracks (11.6): up to their limits, as far as the free space goes
    if (mode == 2 || (mode == 0 && avail < 0)) {
        for (size_t t = 0; t < n; ++t)
            if (flex[t] == 0) base[t] = limit[t];
    } else if (mode == 0) {
        for (int round = 0; round < 8; ++round) {
            float used = gaps, open = 0;
            for (size_t t = 0; t < n; ++t) {
                used += base[t];
                if (flex[t] == 0 && limit[t] > base[t] + 0.01f) ++open;
            }
            float freeSpace = avail - used;
            if (freeSpace <= 0.01f || open == 0) break;
            for (size_t t = 0; t < n; ++t)
                if (flex[t] == 0 && limit[t] > base[t] + 0.01f) base[t] = std::min(limit[t], base[t] + freeSpace / open);
        }
    }
    // flexible tracks (11.7): the size of 1fr is what the space left gives, or what the content wants
    bool anyFlex = false;
    for (float f : flex) anyFlex = anyFlex || f > 0;
    if (anyFlex && mode != 1) {
        float fr = 0;
        if (mode == 0 && avail >= 0) {
            std::vector<char> inflexible(n, 0);
            for (int round = 0; round <= (int)n; ++round) {
                float left = avail - gaps, sumFlex = 0;
                for (size_t t = 0; t < n; ++t) {
                    if (flex[t] > 0 && !inflexible[t]) sumFlex += flex[t];
                    else left -= base[t];
                }
                fr = std::max(0.f, left) / std::max(1.f, sumFlex);
                bool again = false;
                for (size_t t = 0; t < n; ++t)
                    if (flex[t] > 0 && !inflexible[t] && fr * flex[t] < base[t]) inflexible[t] = again = true;
                if (!again) break;
            }
        } else {
            for (size_t t = 0; t < n; ++t)
                if (flex[t] > 0) fr = std::max(fr, flex[t] > 1 ? base[t] / flex[t] : base[t]);
            for (size_t i = 0; i < plan.items.size(); ++i) {
                int t0 = plan.items[i].line[axis][0], t1 = plan.items[i].line[axis][1];
                float sumFlex = 0, fixedPart = gap * (float)(t1 - t0 - 1);
                for (int t = t0; t < t1; ++t) {
                    if (flex[t] > 0) sumFlex += flex[t];
                    else fixedPart += base[t];
                }
                if (sumFlex > 0) fr = std::max(fr, (maxC[i] - fixedPart) / std::max(1.f, sumFlex));
            }
        }
        for (size_t t = 0; t < n; ++t)
            if (flex[t] > 0) base[t] = std::max(base[t], fr * flex[t]);
    }
    // auto tracks stretch over what is left (11.8)
    if (mode == 0 && avail >= 0 && stretch) {
        float used = gaps;
        int autos = 0;
        for (size_t t = 0; t < n; ++t) {
            used += base[t];
            if (defs[t].max.k == GridBreadth::Auto && flex[t] == 0) ++autos;
        }
        if (avail > used && autos)
            for (size_t t = 0; t < n; ++t)
                if (defs[t].max.k == GridBreadth::Auto && flex[t] == 0) base[t] += (avail - used) / (float)autos;
    }
    return base;
}

// The start of each track and the end of the last (lines[n]), with justify-content / align-content spreading the
// space that is left (Box Alignment 3, 5.1)
std::vector<float> trackLines(const std::vector<float>& size, float start, float gap, float avail, uint8_t distribute) {
    size_t n = size.size();
    float total = gap * (float)(n > 0 ? n - 1 : 0);
    for (float v : size) total += v;
    float freeSpace = avail >= 0 ? avail - total : 0, lead = 0, between = 0;
    if (freeSpace > 0.01f && n > 0)
        switch (distribute) {
            case 1: lead = freeSpace / 2; break;
            case 2: lead = freeSpace; break;
            case 3: between = n > 1 ? freeSpace / (float)(n - 1) : 0; break;
            case 4: between = freeSpace / (float)n; lead = between / 2; break;
            case 5: between = freeSpace / (float)(n + 1); lead = between; break;
        }
    std::vector<float> lines(n + 1);
    float x = start + lead;
    for (size_t t = 0; t < n; ++t) {
        lines[t] = x;
        x += size[t] + gap + between;
    }
    lines[n] = n ? lines[n - 1] + size[n - 1] : start;
    return lines;
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
        case BK::Grid: {  // the columns sized under a min-content and a max-content constraint (11.1)
            GridPlan plan = placeGrid(b, -1, -1);
            std::vector<float> lo, hi;
            for (auto& it : plan.items) {
                intrinsic(c, *it.b);
                lo.push_back(it.b->minW);
                hi.push_back(it.b->maxW);
            }
            float gaps = b.st->gapCol * (float)(plan.tracks[1].empty() ? 0 : plan.tracks[1].size() - 1);
            mn = mx = gaps;
            for (float v : sizeTracks(plan.tracks[1], plan, 1, lo, hi, -1, b.st->gapCol, false, 1)) mn += v;
            for (float v : sizeTracks(plan.tracks[1], plan, 1, lo, hi, -1, b.st->gapCol, false, 2)) mx += v;
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
    b.contentMinW = mn + hpb;
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

void emitText(Ctx& c, float x, float y, float w, float h, const Style& st, const std::wstring& txt) {
    DItem d;
    d.k = DItem::K::Text;
    if (st.textShadow) {  // CSS Text Decoration 3, 4: the colours resolved against the text's own
        auto list = std::make_shared<Shadows>(*st.textShadow);
        for (auto& sh : *list) sh.c = faded(sh.currentColor ? st.color : sh.c, st);
        d.shadows = std::move(list);
    }
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
    std::deque<Item> pieces;  // items made while laying out: parts of words broken across lines, ellipses
    float indent = b.firstLines ? bs.textIndent.resolve(avail) : 0;  // CSS Text 3, 8.1
    float x = indent, y = y0;
    bool first = true;
    // the room of the current line between the floats (CSS 2 9.5): lineX0 and lineAvail replace x0 and avail
    float lineX0 = x0, lineAvail = avail;
    auto band = [&]() {
        auto [L, R] = floatBand(c, y, bs.lh(), x0, x0 + avail);
        lineX0 = L;
        lineAvail = std::max(0.f, R - L);
    };
    // a line too narrow for what comes next moves down below a float, while one is beside it
    auto makeRoom = [&](float need) {
        for (int guard = 0; guard < 100 && line.empty() && need > lineAvail + 0.01f && lineAvail < avail; ++guard) {
            float next = nextFloatBottom(c, y, x0, x0 + avail);
            if (!std::isfinite(next)) break;
            y = next;
            band();
        }
    };
    band();

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
            x = first ? indent : 0;
            return;
        }
        float lineW = line.empty() ? 0 : line.back().x + line.back().it->w;
        if (bs.textOverflow == 1 && bs.overflow != 0 && lineW > lineAvail + 0.5f) {
            // text-overflow: ellipsis (CSS Overflow 4, 3.1): what does not fit ends in an ellipsis instead
            const Style* last = line.back().it->st;
            float ew = c.in.ts->width(*last, L"\u2026");
            while (!line.empty() && line.back().x + line.back().it->w + ew > lineAvail + 0.5f) {
                Placed p = line.back();
                line.pop_back();
                if (p.it->k != Item::Word || p.x + ew >= lineAvail) continue;
                std::wstring keep = p.it->text;  // as much of the word as fits
                while (!keep.empty() && p.x + c.in.ts->width(*p.it->st, keep) + ew > lineAvail + 0.5f) keep.pop_back();
                if (keep.empty()) continue;
                Item cut = *p.it;
                cut.text = keep;
                cut.w = c.in.ts->width(*cut.st, keep);
                pieces.push_back(std::move(cut));
                line.push_back({&pieces.back(), p.x});
                break;
            }
            Item dots = line.empty() ? Item{} : *line.back().it;
            dots.k = Item::Word;
            dots.st = line.empty() ? last : dots.st;
            dots.text = L"\u2026";
            dots.w = ew;
            pieces.push_back(std::move(dots));
            line.push_back({&pieces.back(), line.empty() ? 0.f : line.back().x + line.back().it->w});
            lineW = line.back().x + ew;
        }
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
        if (bs.textAlign == 1) off = lineAvail - lineW;
        else if (bs.textAlign == 2) off = (lineAvail - lineW) / 2;
        if (off < 0) off = 0;
        size_t i = 0;
        while (i < line.size()) {
            Item& it = *line[i].it;
            float fx = lineX0 + off + line[i].x;
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
                        d.k = it.svg >= 0 ? DItem::K::Svg : DItem::K::Image;
                        d.objectFit = it.st->objectFit;
                        d.objectPos[0] = it.st->objectPos[0];
                        d.objectPos[1] = it.st->objectPos[1];
                        d.x = fx;
                        d.y = iy;
                        d.w = it.w;
                        d.h = it.h;
                        d.img = it.svg >= 0 ? it.svg : it.img;
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
        band();
    };
    // A word too long for any line breaks where it has to (overflow-wrap: anywhere) or anywhere (word-break:
    // break-all): the part that fits goes on this line, the rest on the next ones
    auto breakWord = [&](Item& it) {
        std::wstring rest = it.text;
        while (!rest.empty()) {
            float room = lineAvail - x;
            size_t n = 0;
            while (n < rest.size() && c.in.ts->width(*it.st, rest.substr(0, n + 1)) <= room + 0.01f) ++n;
            if (n > 0 && n < rest.size() && (rest[n - 1] & 0xFC00) == 0xD800) --n;  // not inside a surrogate pair
            if (n == 0 && line.empty()) n = rest.size() > 1 && (rest[0] & 0xFC00) == 0xD800 ? 2 : 1;
            if (n == 0) {
                commit(false);
                continue;
            }
            Item part = it;
            part.text = rest.substr(0, n);
            part.w = c.in.ts->width(*it.st, part.text);
            pieces.push_back(std::move(part));
            line.push_back({&pieces.back(), x});
            x += pieces.back().w;
            rest.erase(0, n);
            if (!rest.empty()) commit(false);
        }
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
                if (!line.empty() && x + it.w > lineAvail + 0.01f && wraps(&bs)) commit(false);
                makeRoom(it.w);
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
                    if (it.k == Item::Image && it.st->height.u == Len::Pct && c.lineBlockH >= 0) it.h = it.st->height.resolve(c.lineBlockH);
                    if (it.svg >= 0) {
                        if (it.st->width.u == Len::Auto && !b.fillItem) it.w = it.st->height.definite() && it.h >= 0 && it.aspect > 0 ? it.h * it.aspect : avail;
                        if (it.h < 0) it.h = it.aspect > 0 ? it.w / it.aspect : 150;
                    }
                }
                ensureW(c, it);
                bool breakable = it.k == Item::Word && wraps(it.st) && it.text.size() > 1;
                if (breakable && it.st->wordBreak == 1 && x + it.w > lineAvail + 0.01f) {  // fills this line first
                    breakWord(it);
                    break;
                }
                if (!line.empty() && x + it.w > lineAvail + 0.01f && it.brk && wraps(it.st)) commit(false);
                if (breakable && it.st->overflowWrap == 1 && line.empty() && x + it.w > lineAvail + 0.01f) {
                    breakWord(it);
                    break;
                }
                if (it.k == Item::Word || atomicLike(it.k)) makeRoom(it.w);
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
        f.mx = (st && st->maxWidth.definite()) ? st->maxWidth.resolve(W) + (st->borderBox ? 0 : hpb) : 1e9f;
        if (st && st->minWidth.definite()) {
            f.mn = st->minWidth.resolve(W) + (st->borderBox ? 0 : hpb);
            f.mx = std::max(f.mx, f.mn);
        } else if (st && st->overflow != 0) {
            f.mn = hpb;
        } else {
            // The automatic minimum (Flexbox 4.5): the width the content needs at least, capped by the item's own
            // width when it has one and by its max-width. A flex-basis does not lower it.
            f.mn = std::max(hpb, it->contentMinW - f.hm);
            if (defW) f.mn = std::min(f.mn, st->width.resolve(W) + (st->borderBox ? 0 : hpb));
            f.mn = std::min(f.mn, f.mx);
        }
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

// A grid container's content (CSS Grid 1, section 11): its columns are sized from the items' widths, the items are
// laid out in their columns, the rows are sized from their heights, and the items are moved into their rows and
// aligned in their areas. H is the content height when it is definite, else -1; minH and maxH bound it.
float layoutGrid(Ctx& c, Box& b, float ix, float iy, float W, float H, float minH, float maxH) {
    const Style& s = *b.st;
    for (auto& k : b.kids)
        if (k->kind == BK::Marker && k->target) {
            k->target->staticX = ix;
            k->target->staticY = iy;
        }
    GridPlan plan = placeGrid(b, W, H);
    size_t n = plan.items.size();
    auto styleOf = [](const Box* it) -> const Style* { return it->kind == BK::Anon ? nullptr : it->st; };

    // columns, from the items' min-content and max-content widths
    std::vector<float> lo(n), hi(n);
    for (size_t i = 0; i < n; ++i) {
        Box* it = plan.items[i].b;
        intrinsic(c, *it);
        lo[i] = it->minW;
        hi[i] = it->maxW;
    }
    std::vector<float> cols = sizeTracks(plan.tracks[1], plan, 1, lo, hi, W, s.gapCol, s.justifyNormal, 0);
    std::vector<float> colX = trackLines(cols, ix, s.gapCol, W, s.justifyNormal ? 0 : s.justify);

    // each item in its columns, at the top for now
    struct Placed {
        Mark m0, m1;
        float areaX, areaW;
    };
    std::vector<Placed> placed(n);
    for (size_t i = 0; i < n; ++i) {
        Box* it = plan.items[i].b;
        const Style* st = styleOf(it);
        int c0 = plan.items[i].line[1][0], c1 = plan.items[i].line[1][1];
        float x0 = colX[c0], areaW = std::max(0.f, colX[c1 - 1] + cols[c1 - 1] - x0);
        uint8_t js = st && st->justifySelf != 255 ? st->justifySelf : s.justifyItems;
        bool autoMargin = st && (st->margin[1].u == Len::Auto || st->margin[3].u == Len::Auto);
        it->oof = true;
        it->forcedW = it->forcedH = -1;
        edges(*it, areaW);
        float hm = it->m[1] + it->m[3], hpb = hpbOf(*it);
        if (!(st && st->width.definite()) && !it->replaced) {
            if (js == 0 && !autoMargin) it->forcedW = std::max(0.f, areaW - hm);  // stretched (Box Alignment 3, 6.1)
            else it->forcedW = std::max(0.f, std::min(std::max(it->minW, areaW), it->maxW) - hm);
            if (st && st->maxWidth.definite()) it->forcedW = std::min(it->forcedW, st->maxWidth.resolve(areaW) + (st->borderBox ? 0 : hpb));
            if (st && st->minWidth.definite()) it->forcedW = std::max(it->forcedW, st->minWidth.resolve(areaW) + (st->borderBox ? 0 : hpb));
        }
        if (st && st->height.u == Len::Px && !it->replaced) it->forcedH = st->height.v + (st->borderBox ? 0 : vpbOf(*it));
        placed[i].m0 = mark(c);
        layoutBox(c, *it, x0, iy + it->m[0], areaW);
        placed[i].m1 = mark(c);
        placed[i].areaX = x0;
        placed[i].areaW = areaW;
        float freeX = areaW - (hm + it->w), dx = 0;
        if (freeX > 0.01f) {
            bool ml = st && st->margin[3].u == Len::Auto, mr = st && st->margin[1].u == Len::Auto;
            if (ml && mr) dx = freeX / 2;
            else if (ml) dx = freeX;
            else if (!mr && js == 2) dx = freeX / 2;
            else if (!mr && js == 3) dx = freeX;
        }
        moveBox(c, *it, placed[i].m0, placed[i].m1, dx, 0);
        lo[i] = hi[i] = it->m[0] + it->h + it->m[2];
    }

    // rows, from the items' heights; a definite min-height is the room auto rows stretch into (11.8)
    float rowAvail = H;
    std::vector<float> rows = sizeTracks(plan.tracks[0], plan, 0, lo, hi, rowAvail, s.gapRow, s.alignContentNormal, 0);
    auto total = [&](const std::vector<float>& v) {
        float t = s.gapRow * (float)(v.empty() ? 0 : v.size() - 1);
        for (float x : v) t += x;
        return t;
    };
    if (H < 0 && minH > total(rows) + 0.5f) rowAvail = minH;
    else if (H < 0 && maxH >= 0 && total(rows) > maxH + 0.5f) rowAvail = maxH;
    if (rowAvail != H) rows = sizeTracks(plan.tracks[0], plan, 0, lo, hi, rowAvail, s.gapRow, s.alignContentNormal, 0);
    std::vector<float> rowY = trackLines(rows, iy, s.gapRow, rowAvail, s.alignContentNormal ? 0 : s.alignContent);

    // each item into its rows, aligned in its area
    for (size_t i = 0; i < n; ++i) {
        Box* it = plan.items[i].b;
        const Style* st = styleOf(it);
        int r0 = plan.items[i].line[0][0], r1 = plan.items[i].line[0][1];
        float y0 = rowY[r0], areaH = std::max(0.f, rowY[r1 - 1] + rows[r1 - 1] - y0);
        moveBox(c, *it, placed[i].m0, placed[i].m1, 0, y0 - iy);
        float extra = areaH - (it->m[0] + it->h + it->m[2]);
        uint8_t as = st && st->alignSelf != 255 ? st->alignSelf : s.alignItems;
        bool mt = st && st->margin[0].u == Len::Auto, mb = st && st->margin[2].u == Len::Auto;
        if (extra > 0.01f) {
            if (mt || mb) moveBox(c, *it, placed[i].m0, placed[i].m1, 0, mt && mb ? extra / 2 : mt ? extra : 0);
            else if (as == 0 && !(st && st->height.definite()) && !it->replaced) setHeight(c, *it, areaH - it->m[0] - it->m[2]);
            else if (as == 2 || as == 3) moveBox(c, *it, placed[i].m0, placed[i].m1, 0, as == 2 ? extra / 2 : extra);
        }
        if (r0 == 0 && b.firstBaseline < 0 && it->firstBaseline >= 0) b.firstBaseline = it->firstBaseline;
    }
    return std::max(0.f, rowY.back() - iy);
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

// The transform of a box that has just been laid out (CSS Transforms 1). A translation moves everything the box
// painted, as relative positioning does, so element boxes and hit testing follow it. Any other transform is left to
// the painter through the PushTransform item; element boxes then keep their untransformed place.
void applyTransform(Ctx& c, Box& b, size_t pushIdx, const Mark& m0) {
    const Style& s = *b.st;
    float m[6];
    transform_matrix(*s.transform, b.w, b.h, m);
    bool translation = m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1;
    if (translation) {
        shift(c, m0, mark(c), m[4], m[5]);
    } else {
        float x = b.node && b.node->hasBox ? b.node->bx : b.x, y = b.node && b.node->hasBox ? b.node->by : b.y;
        DItem& d = c.out.items[pushIdx];
        d.x = x + s.transformOrigin[0].resolve(b.w);
        d.y = y + s.transformOrigin[1].resolve(b.h);
        std::copy(m, m + 4, d.bw);
        d.w = m[4];
        d.h = m[5];
    }
    DItem pop;
    pop.k = DItem::K::PopTransform;
    push(c, std::move(pop));
}

// A float (CSS 2 9.5.1): its width shrinks to fit (10.3.5), then it goes as high as it may and as far to its side as
// the floats already placed let it, and joins them.
void placeFloat(Ctx& c, Box& k, float ix, float contentW, float top) {
    const Style* ks = k.node->style.get();
    float hm = k.m[1] + k.m[3], hpb = hpbOf(k);
    float bw;  // the border box width
    if (ks->width.definite() && !k.replaced) bw = ks->width.resolve(contentW) + (ks->borderBox ? 0 : hpb);
    else {
        intrinsic(c, k);
        bw = std::min(std::max(k.minW, contentW), k.maxW) - hm;
        if (ks->maxWidth.definite()) bw = std::min(bw, ks->maxWidth.resolve(contentW) + (ks->borderBox ? 0 : hpb));
        if (ks->minWidth.definite()) bw = std::max(bw, ks->minWidth.resolve(contentW) + (ks->borderBox ? 0 : hpb));
    }
    bw = std::max(bw, hpb);
    float outer = bw + hm;
    if (c.floats) {  // a float is not higher than an earlier one (9.5.1 rule 5)
        for (const auto& f : *c.floats) top = std::max(top, f.y0);
    }
    if (ks->clear) top = std::max(top, clearBelow(c, ks->clear));
    float x = ix;
    for (int guard = 0; guard < 200; ++guard) {
        auto [L, R] = floatBand(c, top, 1, ix, ix + contentW);
        if (R - L >= outer || (L == ix && R == ix + contentW)) {
            x = ks->floatSide == 1 ? L : R - outer;
            break;
        }
        float next = nextFloatBottom(c, top, ix, ix + contentW);
        if (!std::isfinite(next)) {
            x = ks->floatSide == 1 ? L : R - outer;
            break;
        }
        top = next;
    }
    k.forcedW = bw;
    layoutBox(c, k, x, top + k.m[0], outer);
    k.forcedW = -1;
    if (c.floats) c.floats->push_back({ks->floatSide, x, x + outer, top, k.y + k.h + k.m[2]});
}

void layoutBox(Ctx& c, Box& b, float cx, float top, float cw, bool center) {
    edges(b, cw);
    const Style* s = b.st;
    b.paintIdx = b.clipIdx = SIZE_MAX;
    b.bgFirst = b.bgEnd = SIZE_MAX;
    b.firstBaseline = -1;
    bool own = s && b.kind != BK::Anon && b.kind != BK::Marker;
    Mark m0 = mark(c);
    int savedLayer = c.layer;
    // An element whose opacity or transform animations and transitions run: painting works them out (anim.h), and
    // its own transform is theirs to apply
    bool animated = own && (s->animOpacity || s->animTransform) && &b != c.root && b.node && b.node->anim;
    // A transformed box is painted like a positioned one with z-index auto (CSS Transforms 1, section 2).
    bool transformed = own && s->transform && !(animated && s->animTransform) && &b != c.root;
    if (own && (s->position != 0 || transformed || animated) && &b != c.root) {
        int L = s->position == 0 || s->zAuto ? 1 : (s->zIndex >= 0 ? s->zIndex + 1 : s->zIndex);
        c.layer = L < 0 ? L : std::max(c.layer, L);
    }
    bool filtered = own && s->filter && &b != c.root;
    if (filtered) {  // a filter makes a stacking context (Filter Effects 1, 4)
        if (s->position == 0 && !transformed && !animated) c.layer = std::max(c.layer, 1);
        DItem d;
        d.k = DItem::K::PushFilter;
        d.filter = s->filter;
        push(c, std::move(d));
    }
    size_t animIdx = SIZE_MAX;
    if (animated) {
        auto paint = std::make_shared<AnimPaint>();
        paint->anims = s->animations;
        paint->state = *b.node->anim;
        paint->baseOpacity = s->opacity;
        paint->baseTransform = s->transform;
        paint->origin[0] = s->transformOrigin[0];
        paint->origin[1] = s->transformOrigin[1];
        paint->opacity = s->animOpacity;
        paint->transform = s->animTransform;
        DItem d;
        d.k = DItem::K::PushAnim;
        d.anim = std::move(paint);
        animIdx = push(c, std::move(d));
        c.out.animations = true;
    }
    size_t transformIdx = SIZE_MAX;
    if (transformed) {
        DItem d;
        d.k = DItem::K::PushTransform;
        d.bw[0] = d.bw[3] = 1;  // the identity, which a translation keeps: it moves the items themselves instead
        transformIdx = push(c, std::move(d));
    }
    float hpb = hpbOf(b), vpb = vpbOf(b);
    bool normal = own && b.kind != BK::Cell && !b.oof;
    // A box that makes a block formatting context of its own (CSS 2 9.4.1) has floats of its own
    std::vector<Ctx::Float> ownFloats;
    std::vector<Ctx::Float>* outerFloats = c.floats;
    bool bfcRoot = &b == c.root || b.forcedW >= 0 ||
                   (own && (b.oof || b.kind == BK::Cell || b.kind == BK::Table || b.kind == BK::Flex || b.kind == BK::Grid || b.replaced || s->floatSide ||
                            s->overflow != 0 || s->flowRoot || s->display == Display::InlineBlock));
    if (bfcRoot) c.floats = &ownFloats;
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

    // clip-path (CSS Masking 1): everything the box paints is clipped to the shape; placed once the box's size is known
    size_t shapeIdx = SIZE_MAX;
    if (own && s->clipPath && &b != c.root) {
        DItem d;
        d.k = DItem::K::PushClip;
        d.shape = s->clipPath;
        shapeIdx = push(c, std::move(d));
    }
    bool layers = own && !b.replaced && !s->hidden && s->bgImages && !s->bgImages->images.empty();
    std::shared_ptr<const Shadows> shadows;
    if (own && !s->hidden && s->boxShadow) {  // currentcolor becomes the element's colour
        auto list = std::make_shared<Shadows>(*s->boxShadow);
        for (auto& sh : *list) {
            if (sh.currentColor) sh.c = s->color;
            sh.c = faded(sh.c, *s);
        }
        shadows = std::move(list);
    }
    if (own && !b.replaced && !s->hidden &&
        (!s->bg.transparent() || layers || shadows || b.b[0] > 0 || b.b[1] > 0 || b.b[2] > 0 || b.b[3] > 0)) {
        DItem d;
        d.k = DItem::K::Box;
        d.shadows = shadows;
        d.shadowParts = layers ? 1 : 3;
        d.c = faded(s->bg, *s);
        d.radius = s->radius;
        for (int i = 0; i < 4; ++i) {
            d.bw[i] = layers ? 0 : b.b[i];  // with images over the colour, the borders come after them
            d.bc[i] = faded(s->borderColor[i], *s);
        }
        b.paintIdx = push(c, std::move(d));
        if (layers) {
            // the images, the last layer first (CSS Backgrounds 3, 2.2); placed once the box's height is known
            b.bgFirst = c.out.items.size();
            for (size_t i = s->bgImages->images.size(); i-- > 0;) {
                const BgImage& im = s->bgImages->images[i];
                if (im.k == BgImage::None) continue;
                auto paint = std::make_shared<BgPaint>();
                paint->bg = s->bgImages;
                paint->layer = i;
                paint->radius = s->radius;
                for (int k = 0; k < 4; ++k) {
                    paint->b[k] = b.b[k];
                    paint->p[k] = b.p[k];
                }
                if (im.k == BgImage::Url && c.in.imgByUrl) {
                    auto it = c.in.imgByUrl->find(resolve_url(c.in.doc->baseUrl, im.url));
                    if (it != c.in.imgByUrl->end()) paint->img = it->second;
                    if (paint->img < 0) continue;  // not here yet
                }
                DItem bgItem;
                bgItem.k = DItem::K::Background;
                bgItem.c = Color{0, 0, 0, s->alpha};
                bgItem.bg = std::move(paint);
                push(c, std::move(bgItem));
            }
            b.bgEnd = c.out.items.size();
            DItem borders;  // the borders over the images
            borders.k = DItem::K::Box;
            borders.radius = s->radius;
            borders.shadows = shadows;
            borders.shadowParts = 2;
            for (int i = 0; i < 4; ++i) {
                borders.bw[i] = b.b[i];
                borders.bc[i] = faded(s->borderColor[i], *s);
            }
            push(c, std::move(borders));
        }
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
    if (own && !b.replaced && defH < 0 && s->aspectRatio > 0) {
        // CSS Sizing 4, 5: the height the width and the ratio give (of the box box-sizing names), which the content
        // may still stretch unless the box scrolls
        float h = (s->borderBox ? contentW + hpb : contentW) / s->aspectRatio - (s->borderBox ? vpb : 0);
        if (s->overflow != 0) defH = std::max(0.f, h);
        else minH = std::max(minH, h);
    }
    float maxH = (own && !b.replaced && s->maxHeight.u == Len::Px) ? std::max(0.f, s->maxHeight.v - (s->borderBox ? vpb : 0)) : -1;
    // CSS 2 section 10.7: a height is held to max-height first and then raised to min-height.
    auto bounded = [&](float h) { return std::max(maxH >= 0 ? std::min(h, maxH) : h, minH); };

    float contentH = 0;
    switch (b.kind) {
        case BK::Marker: break;
        case BK::Anon: contentH = layoutInline(c, b, ix, iy, contentW); break;  // lineBlockH was set by the parent
        case BK::Table: contentH = layoutTable(c, b, ix, iy, contentW); break;
        case BK::Flex:
            if (defH >= 0) contentH = layoutFlex(c, b, ix, iy, contentW, bounded(defH), bounded(defH));
            else contentH = layoutFlex(c, b, ix, iy, contentW, minH > 0 ? minH : -1.f, maxH);
            break;
        case BK::Grid: contentH = layoutGrid(c, b, ix, iy, contentW, defH >= 0 ? bounded(defH) : -1.f, minH, maxH); break;
        default: {
            float y = iy, pend = 0;
            bool firstKid = true;
            float outerLineBlockH = c.lineBlockH;
            c.lineBlockH = defH;
            for (auto& k : b.kids) {
                if (k->kind == BK::Marker) {
                    if (k->target) {
                        k->target->staticX = ix;
                        k->target->staticY = y + (firstKid ? 0 : pend);
                    }
                    continue;
                }
                edges(*k, contentW);
                const Style* ks = k->node && k->node->style ? k->node->style.get() : nullptr;
                if (ks && ks->floatSide && k->kind != BK::Anon) {
                    placeFloat(c, *k, ix, contentW, firstKid ? y : y + pend);
                    continue;
                }
                float mt = k->m[0];
                float gap = firstKid ? mt : collapse(pend, mt);
                float top = y + gap, kx = ix, kw = contentW;
                if (ks && ks->clear && k->kind != BK::Anon) {  // clearance (CSS 2 9.5.2): the border edge below the floats
                    float below = clearBelow(c, ks->clear);
                    if (below > top) top = below;
                }
                // a box that makes its own formatting context does not overlap the floats: it narrows or moves down
                bool avoids = k->kind != BK::Anon && (k->kind == BK::Table || k->kind == BK::Flex || k->kind == BK::Grid || k->replaced ||
                                                      (ks && (ks->overflow != 0 || ks->flowRoot)));
                if (avoids && c.floats && !c.floats->empty()) {
                    for (int guard = 0; guard < 100; ++guard) {
                        auto [L, R] = floatBand(c, top, 1, ix, ix + contentW);
                        float need = ks && ks->width.definite() ? ks->width.resolve(contentW) + (ks->borderBox ? 0 : hpbOf(*k)) + k->m[1] + k->m[3] : 0;
                        if (R - L >= need || (L == ix && R == ix + contentW)) {
                            kx = L;
                            kw = R - L;
                            break;
                        }
                        float next = nextFloatBottom(c, top, ix, ix + contentW);
                        if (!std::isfinite(next)) break;
                        top = next;
                    }
                }
                layoutBox(c, *k, kx, top, kw, s && s->centerBlocks);
                y = k->y + k->h;
                pend = k->m[2];
                firstKid = false;
                if (b.firstBaseline < 0 && k->firstBaseline >= 0) b.firstBaseline = k->firstBaseline;
            }
            contentH = firstKid ? 0 : std::max(0.f, y + pend - iy);
            c.lineBlockH = outerLineBlockH;
        }
    }
    if (bfcRoot) {  // the context's floats are inside it: its height takes them in (CSS 2 10.6.7)
        for (const auto& f : ownFloats) contentH = std::max(contentH, f.y1 - iy);
        c.floats = outerFloats;
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
        syncBackground(c, b);
    }
    if (own && b.node && !b.replaced) setBox(c, b.node, b.x, b.y, b.w, b.h);

    if (b.kind == BK::Block && own && s->display == Display::ListItem && s->listStyle != 4 && !s->hidden) {
        // the list-item counter in the list style, with a full stop after a number (CSS Lists 3, 3.1)
        std::wstring m = widen(counter_text(s->listItem, s->listStyle));
        if (s->listStyle > 2) m += L".";
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
    if (own && !s->hidden && s->outlineStyle && s->outlineWidth > 0 && &b != c.root) {  // CSS UI 4, 4
        float w = s->outlineStyle == 2 ? std::max(2.f, s->outlineWidth) : s->outlineWidth, out = s->outlineOffset + w;
        DItem d;
        d.k = DItem::K::Box;
        d.x = b.x - out;
        d.y = b.y - out;
        d.w = std::max(0.f, b.w + 2 * out);
        d.h = std::max(0.f, b.h + 2 * out);
        d.radius = s->radius > 0 ? std::max(0.f, s->radius + out) : 0;
        Color oc = s->outlineColor.a < 0 ? (s->outlineStyle == 2 ? Color{0.1f, 0.45f, 0.9f, 1} : s->color) : s->outlineColor;
        for (int i = 0; i < 4; ++i) {
            d.bw[i] = w;
            d.bc[i] = faded(oc, *s);
        }
        b.outlineIdx = push(c, std::move(d));
    }
    if (shapeIdx != SIZE_MAX) {
        DItem& d = c.out.items[shapeIdx];
        uint8_t box = s->clipPath->box;  // the reference box, from the border box out to the margins or in
        float out[4] = {};
        for (int i = 0; i < 4; ++i) out[i] = box == 3 ? b.m[i] : box == 0 ? 0 : -(b.b[i] + (box == 2 ? b.p[i] : 0));
        d.x = b.x - out[3];
        d.y = b.y - out[0];
        d.w = std::max(0.f, b.w + out[1] + out[3]);
        d.h = std::max(0.f, b.h + out[0] + out[2]);
        DItem e;
        e.k = DItem::K::PopClip;
        e.x = d.x;
        e.y = d.y;
        push(c, std::move(e));
    }
    if (own && s->position == 4 && &b != c.root) c.stickies.push_back({&b, m0, mark(c)});
    if (own && s->position == 1) {
        float dx = s->inset[3].definite() ? s->inset[3].resolve(cw) : s->inset[1].definite() ? -s->inset[1].resolve(cw) : 0;
        float dy = s->inset[0].u == Len::Px ? s->inset[0].v : s->inset[2].u == Len::Px ? -s->inset[2].v : 0;
        shift(c, m0, mark(c), dx, dy);
    }
    if (transformIdx != SIZE_MAX) applyTransform(c, b, transformIdx, m0);
    if (animIdx != SIZE_MAX) {
        DItem& d = c.out.items[animIdx];
        d.x = b.node->hasBox ? b.node->bx : b.x;
        d.y = b.node->hasBox ? b.node->by : b.y;
        d.w = b.w;
        d.h = b.h;
        DItem pop;
        pop.k = DItem::K::PopAnim;
        push(c, std::move(pop));
    }
    if (filtered) {
        DItem pop;
        pop.k = DItem::K::PopFilter;
        push(c, std::move(pop));
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
        auto opens = [&](size_t at) {
            return src[at].k == DItem::K::PushClip || src[at].k == DItem::K::PushTransform || src[at].k == DItem::K::PushAnim || src[at].k == DItem::K::PushFilter;
        };
        auto closes = [&](size_t at) {
            return src[at].k == DItem::K::PopClip || src[at].k == DItem::K::PopTransform || src[at].k == DItem::K::PopAnim || src[at].k == DItem::K::PopFilter;
        };
        if (opens(i)) {  // a clip or a transform and what it holds stay together, sorted inside
            int depth = 0;
            for (j = i; j < e; ++j) {
                if (opens(j)) ++depth;
                else if (closes(j) && --depth == 0) break;
            }
            j = j < e ? j + 1 : e;
        }
        if (!units.empty() && units.back().layer != src[i].layer) mixed = true;
        units.push_back({i, j, src[i].layer});
        i = j;
    }
    if (mixed) std::stable_sort(units.begin(), units.end(), [](const U& x, const U& y) { return x.layer < y.layer; });
    for (auto& u : units) {
        bool group = src[u.a].k == DItem::K::PushClip || src[u.a].k == DItem::K::PushTransform || src[u.a].k == DItem::K::PushAnim ||
                     src[u.a].k == DItem::K::PushFilter;
        if (u.e - u.a < 2 || !group) {
            for (size_t i = u.a; i < u.e; ++i) dst.push_back(std::move(src[i]));
        } else {
            bool closed = src[u.e - 1].k == DItem::K::PopClip || src[u.e - 1].k == DItem::K::PopTransform || src[u.e - 1].k == DItem::K::PopAnim ||
                          src[u.e - 1].k == DItem::K::PopFilter;
            dst.push_back(std::move(src[u.a]));
            sortRange(src, u.a + 1, closed ? u.e - 1 : u.e, dst);
            if (closed) dst.push_back(std::move(src[u.e - 1]));
        }
    }
}

// Scroll containers (CSS Overflow 3). Once every box has its final size, this records how far each clipping box
// can scroll, holds its scroll offset within that and moves its content up by the offset. An `overflow: auto` box
// that can scroll gets a thumb at its right edge that shows where it is.
// A sticky box (CSS Position 3, 3.4) keeps its place in the flow until scrolling would take it past its top (or
// bottom) inset in the viewport; then it stays at the inset, as far as its containing block's content box lets it.
// Scrolling moves it, so the page is laid out again when it scrolls.
void placeStickies(Ctx& c) {
    for (auto& st : c.stickies) {
        Box& b = *st.b;
        const Style* s = b.st;
        const Node* cb = b.node ? b.node->parent : nullptr;
        while (cb && !(cb->type == Node::T::Element && cb->hasBox)) cb = cb->parent;
        if (!cb || !cb->style) continue;
        const Style& cs = *cb->style;
        float cbTop = cb->by + cs.border[0] + cs.padding[0].resolve(cb->bw), cbBottom = cb->by + cb->bh - cs.border[2] - cs.padding[2].resolve(cb->bw);
        float dy = 0;
        if (s->inset[0].definite()) {
            float want = c.in.scrollY + s->inset[0].resolve(c.in.vh);
            if (b.y < want) dy = std::min(want - b.y, std::max(0.f, cbBottom - (b.y + b.h + b.m[2])));
        } else if (s->inset[2].definite()) {
            float want = c.in.scrollY + c.in.vh - s->inset[2].resolve(c.in.vh);
            if (b.y + b.h > want) dy = std::max(want - (b.y + b.h), std::min(0.f, cbTop - (b.y - b.m[0])));
        }
        if (dy != 0) {
            shift(c, st.m0, st.m1, 0, dy);
            b.y += dy;
        }
        c.out.fixed = true;
    }
}

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
    placeStickies(c);
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
