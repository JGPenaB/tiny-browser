#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <unordered_map>

#include "css.h"
#include "dom.h"
#include "svg.h"

namespace tb {

class TextSystem;
struct Style;

struct ImageRes {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // premultiplied BGRA
    std::string url;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp;
    uint64_t gen = 0;  // render-target generation the bitmap belongs to
};

// One entry of the display list produced by layout and consumed by the GPU painter.
struct DItem {
    // PushTransform holds a transform for everything up to its PopTransform: a b c d in bw[0..3], e f in w h, and
    // the transform origin in x y (document coordinates), so x' = origin + M (x - origin) + (e, f).
    // Svg draws LayoutOut::svgs[img] into the item's box.
    // Background paints one layer of a background image (bg) in the clip box x y w h.
    enum class K : uint8_t { Box, Text, Image, Rect, Widget, PushClip, PopClip, PushTransform, PopTransform, Svg, Background };
    K k = K::Rect;
    int16_t layer = 0;  // paint layer: 0 = normal flow, >0 positioned (z-index + 1), <0 behind
    float x = 0, y = 0, w = 0, h = 0;
    float radius = 0;
    Color c;
    float bw[4] = {};
    Color bc[4];
    Microsoft::WRL::ComPtr<IDWriteTextLayout> tl;
    std::shared_ptr<const struct BgPaint> bg;
    std::shared_ptr<const ClipShape> shape;  // PushClip of clip-path: the shape, in the reference box x y w h
    int img = -1;  // image index, or widget index for K::Widget; for K::Image -1 is a picture that did not come (a grey
                   // box is shown) and -2 an img that represents nothing (HTML 15.4.2), which shows nothing
};

// One background layer as layout placed it (CSS Backgrounds 3, 3.3-3.9): the image, the first tile and its size, and
// whether tiles repeat across and down. Painted in the item's box, which is the background painting area.
// The item's box is the border box; where the tiles go is worked out when it is painted (render.cpp), because the box
// may still move or change height after layout has emitted the item.
struct BgPaint {
    std::shared_ptr<const Background> bg;
    size_t layer = 0;
    int img = -1;            // the picture of a url() image: an index into the page's images, -1 while it has not come
    float b[4] = {}, p[4] = {};  // the box's border and padding widths, for the origin and clip boxes
    float radius = 0;        // its rounded corners, which the background is clipped to
    // a conic gradient's tile, drawn on the CPU (Direct2D has no conic gradients) and kept while its size stays
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap> conic;
    mutable float conicW = 0, conicH = 0;
    mutable uint64_t conicGen = 0;
};

struct LinkArea {
    float x, y, w, h;
    Node* a;
    int16_t layer = 0;
};

// A form control placed by layout. The shell paints its dynamic content (value, caret, checked state).
struct WidgetArea {
    Node* node;
    float x, y, w, h;
    const Style* st;
    int16_t layer = 0;
};

enum class WidgetKind : uint8_t { None, Text, Password, Button, Check, Radio, Select, TextArea };
WidgetKind widget_kind(const Node* n);
std::wstring widget_label(const Node* n);         // caption of button-like controls
std::vector<Node*> select_options(const Node* n); // <option> elements of a <select>, in order

struct LayoutOut {
    std::vector<DItem> items;
    std::vector<LinkArea> links;
    std::vector<WidgetArea> widgets;
    float height = 0;
    Color canvas{1, 1, 1, 1};
    bool fixed = false;  // the page has fixed boxes: they have to be laid out again when the page scrolls
    std::vector<SvgImage> svgs;      // the svg elements of the page
    std::vector<SvgShape> svgShapes;  // and their shapes
};

struct LayoutInput {
    Document* doc = nullptr;
    TextSystem* ts = nullptr;
    float vw = 800, vh = 600;
    float scrollY = 0;  // how far the page is scrolled: where the viewport is, and with it every fixed box
    const std::vector<ImageRes>* images = nullptr;
    const std::unordered_map<const Node*, int>* imgNode = nullptr;
    const std::unordered_map<std::string, int>* imgByUrl = nullptr;  // every picture by its URL, -1 while on its way
};

LayoutOut layout_document(const LayoutInput& in);

struct WidgetPainter {
    virtual void paintWidget(ID2D1RenderTarget* rt, const DItem& it) = 0;
};

void paint_items(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, LayoutOut& lo, std::vector<ImageRes>& imgs,
                 float y0, float y1, uint64_t gen, WidgetPainter* wp);

}  // namespace tb
