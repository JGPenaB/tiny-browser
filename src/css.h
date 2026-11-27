#pragma once
#include <unordered_map>

#include "common.h"
#include "dom.h"

namespace tb {

enum class Display : uint8_t { Inline, Block, ListItem, InlineBlock, None, Table, RowGroup, Row, Cell, Flex, InlineFlex };

struct Len {
    enum U : uint8_t { Auto, Px, Pct, None };
    U u = Auto;
    float v = 0;
    float off = 0;  // px offset for calc(% +/- px)
    float resolve(float base) const { return u == Px ? v : u == Pct ? v * base / 100.f + off : 0.f; }
    bool definite() const { return u == Px || u == Pct; }
};

using VarMap = std::unordered_map<std::string, std::string>;

// An SVG paint (SVG 2, section 13.2): none, a colour, or currentcolor, the element's own `color`.
struct Paint {
    enum K : uint8_t { None, Rgb, Current } k = None;
    Color c{0, 0, 0, 1};
};

// One function of a transform list (CSS Transforms 1, section 7.1). Translations keep their lengths, because a
// percentage is of the element's own border box and that is known only after layout; the rest are numbers
// (angles in radians). 3D functions are kept as their 2D part (translate3d, scale3d, rotateZ); the others are
// dropped, so translateZ(0) changes nothing.
struct TransformFn {
    enum K : uint8_t { Matrix, Translate, Scale, Rotate, Skew } k = Matrix;
    float v[6] = {1, 0, 0, 1, 0, 0};
    Len tx, ty;
};
using TransformList = std::vector<TransformFn>;
// The transform of a box of the given size as a 2D matrix a b c d e f: x' = a x + c y + e, y' = b x + d y + f.
void transform_matrix(const TransformList& list, float width, float height, float out[6]);

struct Style {
    Display display = Display::Inline;
    Color color{0, 0, 0, 1};
    Color bg{};
    float fontSize = 16;
    uint16_t weight = 400;
    bool italic = false;
    std::wstring family = L"Times New Roman";
    float lhPx = -1, lhFactor = 1.2f;
    uint8_t textAlign = 0;      // 0 left, 1 right, 2 center
    bool underline = false, strike = false;
    uint8_t whiteSpace = 0;     // 0 normal, 1 pre, 2 nowrap, 3 pre-wrap, 4 pre-line
    uint8_t listStyle = 0;      // 0 disc 1 circle 2 square 3 decimal 4 none 5 lower-alpha 6 upper-alpha 7 lower-roman 8 upper-roman
    uint8_t textTransform = 0;  // 1 upper, 2 lower, 3 capitalize
    uint8_t vAlign = 0;         // 0 baseline 1 top 2 middle 3 bottom 4 sub 5 super
    bool borderBox = false, hidden = false;
    bool centerBlocks = false;  // text-align: -webkit-center (<center>, align=center) also centers blocks
    Len margin[4] = {{Len::Px, 0}, {Len::Px, 0}, {Len::Px, 0}, {Len::Px, 0}};
    Len padding[4] = {{Len::Px, 0}, {Len::Px, 0}, {Len::Px, 0}, {Len::Px, 0}};
    Len width, height, minWidth, minHeight;
    Len maxWidth{Len::None, 0}, maxHeight{Len::None, 0};
    float border[4] = {3, 3, 3, 3};
    uint8_t borderStyle[4] = {0, 0, 0, 0};
    Color borderColor[4] = {{0, 0, 0, -1}, {0, 0, 0, -1}, {0, 0, 0, -1}, {0, 0, 0, -1}};
    std::shared_ptr<const VarMap> vars;
    std::shared_ptr<const TransformList> transform;              // null for `none`
    // SVG painting (SVG 2, section 13), all inherited
    Paint fill{Paint::Rgb, {0, 0, 0, 1}}, stroke;
    Len strokeWidth{Len::Px, 1};
    bool fillEvenOdd = false;
    float fillOpacity = 1, strokeOpacity = 1;
    Len transformOrigin[2] = {{Len::Pct, 50}, {Len::Pct, 50}};  // from the border box's top left corner

    // Positioning
    uint8_t position = 0;  // 0 static, 1 relative, 2 absolute, 3 fixed, 4 sticky
    Len inset[4];          // top right bottom left (Auto = unset)
    int zIndex = 0;
    bool zAuto = true;
    uint8_t overflow = 0;  // 0 visible, 1 hidden/clip, 2 auto/scroll
    uint8_t floatSide = 0; // 1 left, 2 right (parsed; float layout is not implemented yet)
    float radius = 0;      // border-radius in px (uniform)
    bool opacity0 = false, animated = false;
    // opacity: the element's own value, and `alpha`, that value times the ancestors'. There is no compositor yet,
    // so an element is not blended as a group: each thing it paints is made that much see-through instead.
    float opacity = 1, alpha = 1;
    bool masked = false;   // mask-image icons: the solid background would be a filled square without mask support
    // Flex container
    uint8_t flexDir = 0;   // 0 row, 1 row-reverse, 2 column, 3 column-reverse
    bool flexWrap = false;
    uint8_t justify = 0;   // 0 start, 1 center, 2 end, 3 space-between, 4 space-around, 5 space-evenly
    uint8_t alignItems = 0;  // 0 stretch, 1 start, 2 center, 3 end
    uint8_t alignSelf = 255; // 255 = auto
    float gapRow = 0, gapCol = 0;
    // Flex item
    float flexGrow = 0, flexShrink = 1;
    Len flexBasis;

    float lh() const { return lhPx >= 0 ? lhPx : lhFactor * fontSize; }
};

struct AttrSel {
    std::string name, val;
    char op = 0;  // 0 exists, '=', '~', '|', '^', '$', '*'
};
struct Compound;
struct Pseudo {
    enum K : uint8_t { FirstChild, LastChild, OnlyChild, Root, Link, Empty, NthChild, NthLastChild, FirstOfType, LastOfType, Not, Defined, Host, Slotted };
    K k = FirstChild;
    int a = 0, b = 0;
    std::shared_ptr<Compound> inner;
};
struct Compound {
    std::string tag, id;
    std::vector<std::string> classes;
    std::vector<AttrSel> attrs;
    std::vector<Pseudo> pseudos;
    bool never = false;
};
struct Selector {
    std::vector<Compound> parts;
    std::vector<char> combs;  // combs[i] joins parts[i] and parts[i+1]
    uint32_t spec = 0;
};
struct Decl {
    std::string prop, val;
    bool important = false;
};
struct MediaQuery {
    std::vector<std::pair<float, float>> alts;  // (min-width, max-width), -1 = unbounded
};
struct Rule {
    Selector sel;
    std::shared_ptr<std::vector<Decl>> decls;
    int media = -1;
};
struct Sheet {
    std::vector<Rule> rules;
    std::vector<MediaQuery> media;
};
// Selectors of a shadow tree's style sheet are matched with that tree as their scope: nothing above its root is
// seen, except the host through :host. Set while such a rule is matched; null for the document's own rules.
extern const Node* g_selectorScope;

Sheet parse_sheet(std::string_view css);
std::vector<Decl> parse_decls(std::string_view body);
bool parse_color(std::string_view v, Color& out);
const char* display_name(Display d);
// True for an element that layout sizes as one box whatever its `display` says: replaced elements and form controls
// other than <button>, whose content is laid out like any other (HTML 15.5.3, button layout).
bool replaced_like(const Node* n);
std::string color_str(const Color& c);
bool selector_parse(std::string_view text, Selector& out);
bool selector_matches(const Selector& s, const Node* e);

class StyleEngine {
public:
    using FontResolver = std::wstring (*)(std::string_view familyList);
    static FontResolver fontResolver;

    // scope: the shadow root whose <style> the sheet is, or null for a sheet of the document
    void add(const Sheet* s, bool ua, const Node* scope = nullptr) { sheets_.push_back({s, ua, scope}); }
    void compute(Document& d, float vw, float vh);
    bool hasMedia() const { return hasMedia_; }
    size_t ruleCount() const { return all_.size(); }

private:
    struct IR {
        const Rule* r;
        uint32_t order;
        bool ua;
        const Node* scope;
    };
    struct Added {
        const Sheet* sheet;
        bool ua;
        const Node* scope;
    };
    void build(float vw);
    void computeTree(Node* e, const Style& parent);
    void computeElem(Node* e, const Style& parent);

    std::vector<Added> sheets_;
    std::vector<IR> all_;
    std::unordered_map<std::string, std::vector<uint32_t>> byId_, byClass_, byTag_;
    std::vector<uint32_t> uni_;
    std::vector<uint32_t> cand_;
    bool hasMedia_ = false;
};

}  // namespace tb
