#pragma once
#include <functional>
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

// Background images (CSS Backgrounds 3, section 2), one entry per layer in each list; a shorter list repeats.
struct GradientStop {
    Color c;
    Len pos;  // Len::Auto: between the stops around it (CSS Images 3, 3.5.3)
};
struct BgImage {  // none, a picture, or a gradient (CSS Images 3, section 3)
    enum K : uint8_t { None, Url, Linear, Radial, Conic } k = None;
    bool repeating = false;
    std::string url;
    float angle = 180;     // linear: degrees clockwise from "to top"
    uint8_t toCorner = 0;  // linear "to" a corner: 1 top, 2 right, 4 bottom, 8 left (0: the angle says)
    bool circle = false;   // radial
    uint8_t extent = 0;    // radial: 0 farthest-corner, 1 closest-side, 2 closest-corner, 3 farthest-side, 4 rx and ry
    Len rx, ry, cx{Len::Pct, 50}, cy{Len::Pct, 50};
    std::vector<GradientStop> stops;
};
struct BgPos {
    Len x{Len::Pct, 0}, y{Len::Pct, 0};  // a percentage aligns that point of the image with that point of the area
};
struct BgSize {
    uint8_t mode = 0;  // 0 the lengths (Len::Auto: auto), 1 cover, 2 contain
    Len w, h;
};
struct Background {
    std::vector<BgImage> images;  // the first is painted on top
    std::vector<BgPos> pos;
    std::vector<BgSize> size;
    std::vector<uint16_t> repeat;        // low byte x, high byte y: 0 repeat, 1 no-repeat, 2 space, 3 round
    std::vector<uint8_t> origin, clip;   // 0 border-box, 1 padding-box, 2 content-box, 3 text (clip only)
};

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
    std::shared_ptr<const Background> bgImages;  // null: no background-image
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
    uint8_t floatSide = 0; // 1 left, 2 right (CSS 2 9.5)
    uint8_t clear = 0;     // 1 left, 2 right, 3 both (CSS 2 9.5.2)
    bool flowRoot = false; // display: flow-root: the box makes a block formatting context of its own
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

    // `content` as written (CSS Generated Content 3); it applies to ::before and ::after, which layout makes boxes for
    std::string content;

    float lh() const { return lhPx >= 0 ? lhPx : lhFactor * fontSize; }
};

struct AttrSel {
    std::string name, val;
    char op = 0;  // 0 exists, '=', '~', '|', '^', '$', '*'
};
struct Compound;
struct Selector;
struct Pseudo {
    enum K : uint8_t {
        FirstChild, LastChild, OnlyChild, Root, Link, Empty, NthChild, NthLastChild, FirstOfType, LastOfType, Not, Defined, Host, Slotted,
        // Selectors 4: of-type counting (14.4), selector lists (4), user actions (10), input states (13), the target (9.2)
        NthOfType, NthLastOfType, OnlyOfType, Is, Has, Hover, Active, Focus, FocusWithin, FocusVisible, Checked, Disabled, Enabled,
        Required, Optional, ReadOnly, ReadWrite, PlaceholderShown, Target, Lang, Never, HasAnchor
    };
    K k = FirstChild;
    int a = 0, b = 0;
    std::shared_ptr<Compound> inner;
    // :is(), :where(), :not() and :has(). A relative selector of :has() starts with a HasAnchor compound, which only
    // the element :has() is tried on matches.
    std::shared_ptr<std::vector<Selector>> list;
    std::string arg;                              // :lang()
};
struct Compound {
    std::string tag, id;
    std::vector<std::string> classes;
    std::vector<AttrSel> attrs;
    std::vector<Pseudo> pseudos;
    bool never = false;
    uint8_t pseudoElement = 0;  // ::before 1, ::after 2 (CSS Pseudo-Elements 4): only on the last compound of a selector
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
// What the selectors of user actions, of the target and of form control states see (Selectors 4, 9.2, 10 and 13).
// The shell keeps it for the page being styled. The nodes are only compared, never followed: one a script has
// removed since may still be named here until the next update.
struct UserState {
    std::vector<const Node*> hover;   // the element under the pointer, then its ancestors
    std::vector<const Node*> active;  // the element being pressed, then its ancestors
    std::vector<const Node*> focus;   // the focused element, then its ancestors
    bool focusVisible = false;        // the focus came from the keyboard, or is in a text control
    const Node* target = nullptr;     // the element the URL's fragment names
    // the live checkedness and emptiness of a form control, which can differ from its attributes
    std::function<bool(const Node*)> checked;
    std::function<bool(const Node*)> valueEmpty;
};
extern UserState g_userState;
// Selectors of a shadow tree's style sheet are matched with that tree as their scope: nothing above its root is
// seen, except the host through :host. Set while such a rule is matched; null for the document's own rules.
extern const Node* g_selectorScope;

// base: the URL of the sheet, which the url()s in it are relative to (empty for a sheet in the page itself)
Sheet parse_sheet(std::string_view css, const std::string& base = {});
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
    // Which kinds of UserState the rules in force look at, so that the shell restyles only when one of them changes
    enum : uint8_t { UsesHover = 1, UsesActive = 2, UsesFocus = 4, UsesFormState = 8, UsesTarget = 16 };
    uint8_t userActions() const { return userActions_; }

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
    using Cascade = std::vector<std::pair<uint64_t, const Decl*>>;
    void computePseudo(Node* e, uint8_t kind, Cascade& L);

    std::vector<Added> sheets_;
    std::vector<IR> all_;
    std::unordered_map<std::string, std::vector<uint32_t>> byId_, byClass_, byTag_;
    std::vector<uint32_t> uni_;
    // the rules for ::before and ::after, indexed the same way
    std::unordered_map<std::string, std::vector<uint32_t>> pseudoById_, pseudoByClass_, pseudoByTag_;
    std::vector<uint32_t> pseudoUni_;
    std::vector<uint32_t> cand_;
    bool hasMedia_ = false;
    uint8_t userActions_ = 0;
};

}  // namespace tb
