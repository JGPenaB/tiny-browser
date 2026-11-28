#pragma once
#include <functional>
#include <unordered_map>

#include "common.h"
#include "dom.h"

namespace tb {

enum class Display : uint8_t { Inline, Block, ListItem, InlineBlock, None, Table, RowGroup, Row, Cell, Flex, InlineFlex, Grid, InlineGrid };

struct Len {
    enum U : uint8_t { Auto, Px, Pct, None };
    U u = Auto;
    float v = 0;
    float off = 0;  // px offset for calc(% +/- px)
    float resolve(float base) const { return u == Px ? v : u == Pct ? v * base / 100.f + off : 0.f; }
    bool definite() const { return u == Px || u == Pct; }
};

// An element's custom properties (CSS Variables 1): those it sets itself, over those of the nearest ancestor that
// set any, which it shares rather than copies. A long chain is folded into one map.
struct VarMap {
    std::unordered_map<std::string, std::string> own;
    std::shared_ptr<const VarMap> parent;
    int depth = 0;
    const std::string* find(const std::string& name) const {
        for (const VarMap* m = this; m; m = m->parent.get()) {
            auto it = m->own.find(name);
            if (it != m->own.end()) return &it->second;
        }
        return nullptr;
    }
    size_t size() const {  // how many names have a value
        std::unordered_map<std::string, bool> seen;
        for (const VarMap* m = this; m; m = m->parent.get())
            for (auto& kv : m->own) seen.emplace(kv.first, true);
        return seen.size();
    }
    // A map over `under` with these declarations
    static std::shared_ptr<const VarMap> over(std::shared_ptr<const VarMap> under, std::unordered_map<std::string, std::string> set) {
        auto m = std::make_shared<VarMap>();
        m->own = std::move(set);
        if (under && under->depth >= 24) {  // fold the chain, the nearest values winning
            for (const VarMap* u = under.get(); u; u = u->parent.get())
                for (auto& [k, v] : u->own) m->own.emplace(k, v);
        } else if (under) {
            m->parent = std::move(under);
            m->depth = m->parent->depth + 1;
        }
        return m;
    }
};

// Background images (CSS Backgrounds 3, section 2), one entry per layer in each list; a shorter list repeats.
struct GradientStop {
    Color c;
    Len pos;  // Len::Auto: between the stops around it (CSS Images 3, 3.5.3)
};
struct BgImage {  // none, a picture, or a gradient (CSS Images 3, section 3)
    enum K : uint8_t { None, Url, Linear, Radial, Conic } k = None;
    bool repeating = false;
    std::string url;
    float angle = 180;     // linear: degrees clockwise from "to top"; conic: where it starts, clockwise from the top
    uint8_t toCorner = 0;  // linear "to" a corner: 1 top, 2 right, 4 bottom, 8 left (0: the angle says)
    bool circle = false;   // radial
    uint8_t extent = 0;    // radial: 0 farthest-corner, 1 closest-side, 2 closest-corner, 3 farthest-side, 4 rx and ry
    Len rx, ry, cx{Len::Pct, 50}, cy{Len::Pct, 50};  // radial and conic: the centre
    std::vector<GradientStop> stops;  // conic: positions as percentages of a turn
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

// Grid containers (CSS Grid 1, section 7). A track's minimum and maximum sizing functions; fit-content(limit) is
// auto / max-content held to the limit.
struct GridBreadth {
    enum K : uint8_t { Auto, Length, Fr, MinContent, MaxContent } k = Auto;
    Len len;     // Length: a length or a percentage of the grid container's content box
    float fr = 0;
};
struct GridTrack {
    GridBreadth min, max;
    Len fitContent{Len::None, 0};
};
// grid-template-columns or -rows. names holds the names of each line: one more than there are tracks, and one more
// again when a repeat(auto-fill | auto-fit, ...) group stands between line autoAt and line autoAt + 1.
struct GridTrackList {
    std::vector<GridTrack> tracks;
    std::vector<std::vector<std::string>> names{1};
    int autoAt = -1;
    bool autoFit = false;
    std::vector<GridTrack> autoTracks;
    std::vector<std::vector<std::string>> autoNames;
};
struct GridArea {
    std::string name;
    int row0, col0, row1, col1;  // lines, from 0; the ends are after the area
};
struct GridTemplate {
    GridTrackList cols, rows;
    std::vector<GridTrack> autoCols{GridTrack{}}, autoRows{GridTrack{}};  // grid-auto-columns and -rows
    std::vector<GridArea> areas;
    int areaRows = 0, areaCols = 0;
    bool flowColumn = false, dense = false;  // grid-auto-flow
};
// One end of a grid item's place (CSS Grid 1, 8.3): auto, a line number, a span, a name, or a name with a number
struct GridLine {
    int line = 0;  // not 0: the nth line, from the end when negative (with `name`: the nth line of that name)
    int span = 0;  // not 0: span that many tracks (or lines of that name)
    std::string name;
    bool isAuto() const { return line == 0 && span == 0 && name.empty(); }
};

// One shadow of box-shadow (CSS Backgrounds 3, 7.1) or text-shadow (CSS Text Decoration 3, 4)
struct Shadow {
    float dx = 0, dy = 0, blur = 0, spread = 0;
    Color c;
    bool currentColor = true;  // no colour given: the element's color
    bool inset = false;
};
using Shadows = std::vector<Shadow>;  // the first is painted on top

// clip-path: a basic shape (CSS Shapes 1, 3.1) in a reference box (CSS Masking 1, 3.1)
struct ClipShape {
    enum K : uint8_t { Inset, Circle, Ellipse, Polygon } k = Inset;
    Len len[4];               // inset: top right bottom left; circle: the radius in len[0]; ellipse: rx, ry
    uint8_t radiusKw[2] = {}; // circle and ellipse: 0 a length, 1 closest-side, 2 farthest-side
    Len cx{Len::Pct, 50}, cy{Len::Pct, 50};
    Len round;                // inset: the corners' radius
    std::vector<std::pair<Len, Len>> points;  // polygon
    bool evenOdd = false;
    uint8_t box = 0;          // 0 border-box, 1 padding-box, 2 content-box, 3 margin-box
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
    // 0 disc 1 circle 2 square 3 decimal 4 none 5 lower-alpha 6 upper-alpha 7 lower-roman 8 upper-roman
    // 9 decimal-leading-zero 10 lower-greek
    uint8_t listStyle = 0;
    uint8_t textTransform = 0;  // 1 upper, 2 lower, 3 capitalize
    // CSS Text 3 (inherited): letter-spacing and word-spacing in px (normal is 0), text-indent, overflow-wrap
    // (1 break-word or anywhere: a word too long for its line may break anywhere), word-break (1 break-all)
    float letterSpacing = 0, wordSpacing = 0;
    Len textIndent{Len::Px, 0};
    uint8_t overflowWrap = 0, wordBreak = 0;
    uint8_t textOverflow = 0;  // CSS Overflow 4: 1 ellipsis, on a block that clips
    float aspectRatio = 0;     // CSS Sizing 4: width / height, 0 for auto
    // CSS Images 3, 5.5-5.6: how a picture fills its box: 0 fill, 1 contain, 2 cover, 3 none, 4 scale-down
    uint8_t objectFit = 0;
    Len objectPos[2] = {{Len::Pct, 50}, {Len::Pct, 50}};
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
    bool justifyNormal = true;     // justify-content: normal or stretch, which a grid's auto tracks stretch to
    uint8_t alignContent = 0;      // as `justify`, for the rows of a grid
    bool alignContentNormal = true;
    uint8_t justifyItems = 0;      // as alignItems, along the inline axis of grid areas
    uint8_t justifySelf = 255;     // 255 = auto
    // Grid container and grid item
    std::shared_ptr<const GridTemplate> grid;  // null: no templates, auto tracks, rows filled first
    std::shared_ptr<const ClipShape> clipPath;  // null: none
    std::shared_ptr<const Shadows> boxShadow, textShadow;  // null: none; text-shadow is inherited
    // outline (CSS UI 4, 4): style 0 none, 1 drawn (solid, dashed and the rest are drawn solid), 2 auto
    uint8_t outlineStyle = 0;
    float outlineWidth = 3, outlineOffset = 0;
    Color outlineColor{0, 0, 0, -1};  // alpha < 0: currentcolor
    GridLine gridRow[2], gridCol[2];           // grid-row-start / -end, grid-column-start / -end
    float gapRow = 0, gapCol = 0;
    // Flex item
    float flexGrow = 0, flexShrink = 1;
    Len flexBasis;

    // `content` as written (CSS Generated Content 3); it applies to ::before and ::after, which layout makes boxes for
    std::string content;
    // counter-reset, counter-increment and counter-set as written (CSS Lists 3, 4), which styling applies in tree order
    std::string counterReset, counterIncrement, counterSet;
    int listItem = 0;  // a list item's value of the list-item counter, which its marker shows

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
    std::string text;  // a <media-query-list> as written, evaluated against the viewport each time styles are computed
};
// What media queries are evaluated against: the viewport in CSS pixels and the device pixel ratio
struct MediaEnv {
    float w = 0, h = 0, dpr = 1;
};
// Whether a <media-query-list> matches (Media Queries 4); an empty list matches
bool media_matches(std::string_view list, const MediaEnv& env);
struct Rule {
    Selector sel;
    std::shared_ptr<std::vector<Decl>> decls;
    int media = -1;
    int layer = -1;  // the cascade layer (CSS Cascade 5, 6.4): an index into Sheet::layers, -1 when unlayered
};
// @import (CSS Cascade 5, 2): another sheet whose rules come before this one's
struct SheetImport {
    std::string url;    // resolved against the sheet's own URL where it has one
    int layer = -1;     // layer or layer(name): the layer the imported rules go in, an index into Sheet::layers
    int media = -1;     // the media the import is for, an index into Sheet::media
};
struct Sheet {
    std::vector<Rule> rules;
    std::vector<MediaQuery> media;
    std::vector<std::string> layers;  // the layer names, dotted from the outermost, in the order they first appear
    std::vector<SheetImport> imports;
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
// A counter value in a counter style (CSS Counter Styles 3, 6): `style` is a Style::listStyle code. Values a style
// cannot show fall back to decimal.
std::string counter_text(int value, uint8_t style);
bool selector_parse(std::string_view text, Selector& out);
bool selector_matches(const Selector& s, const Node* e);

class StyleEngine {
public:
    using FontResolver = std::wstring (*)(std::string_view familyList);
    static FontResolver fontResolver;

    // scope: the shadow root whose <style> the sheet is, or null for a sheet of the document
    // layer: the cascade layer an imported sheet goes in ("" for none); media: what an import's media must match
    void add(const Sheet* s, bool ua, const Node* scope = nullptr, std::string layer = {}, const MediaQuery* media = nullptr) {
        sheets_.push_back({s, ua, scope, std::move(layer), media});
    }
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
        uint8_t layerRank;  // where the rule's layer comes in the layer order; unlayered rules last
    };
    struct Added {
        const Sheet* sheet;
        bool ua;
        const Node* scope;
        std::string layer;
        const MediaQuery* media;
    };
    void build(float vw, float vh);
    void computeTree(Node* e, const Style& parent);
    void computeElem(Node* e, const Style& parent);
    using Cascade = std::vector<std::pair<uint64_t, const Decl*>>;
    void computePseudo(Node* e, uint8_t kind, Cascade& L);
    // CSS Lists 3, 4.5: the counters in scope where styling has got to in tree order, innermost last. A counter lives
    // while styling is inside the parent of the element that made it; `depth` is that element's depth.
    struct Counter {
        std::string name;
        int value;
        int depth;
    };
    void applyCounters(Style& s, bool listItem, int depth);
    bool generatedText(Node* e, const Style& s, std::string& out);
    std::vector<Counter> counters_;
    int depth_ = 0;

    std::vector<Added> sheets_;
    std::vector<IR> all_;
    std::unordered_map<std::string, std::vector<uint32_t>> byId_, byClass_, byTag_, byAttr_;
    std::vector<uint32_t> uni_, root_;  // the rules every element is tried against; those only the root is
    // the rules for ::before and ::after, indexed the same way
    std::unordered_map<std::string, std::vector<uint32_t>> pseudoById_, pseudoByClass_, pseudoByTag_;
    std::vector<uint32_t> pseudoUni_;
    std::vector<uint32_t> cand_;
    bool hasMedia_ = false;
    uint8_t userActions_ = 0;
};

}  // namespace tb
