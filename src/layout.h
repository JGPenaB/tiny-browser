#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <unordered_map>

#include "dom.h"

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
    enum class K : uint8_t { Box, Text, Image, Rect, Widget, PushClip, PopClip, PushTransform, PopTransform };
    K k = K::Rect;
    int16_t layer = 0;  // paint layer: 0 = normal flow, >0 positioned (z-index + 1), <0 behind
    float x = 0, y = 0, w = 0, h = 0;
    float radius = 0;
    Color c;
    float bw[4] = {};
    Color bc[4];
    Microsoft::WRL::ComPtr<IDWriteTextLayout> tl;
    int img = -1;  // image index, or widget index for K::Widget; for K::Image -1 is a picture that did not come (a grey
                   // box is shown) and -2 an img that represents nothing (HTML 15.4.2), which shows nothing
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
};

struct LayoutInput {
    Document* doc = nullptr;
    TextSystem* ts = nullptr;
    float vw = 800, vh = 600;
    float scrollY = 0;  // how far the page is scrolled: where the viewport is, and with it every fixed box
    const std::vector<ImageRes>* images = nullptr;
    const std::unordered_map<const Node*, int>* imgNode = nullptr;
};

LayoutOut layout_document(const LayoutInput& in);

struct WidgetPainter {
    virtual void paintWidget(ID2D1RenderTarget* rt, const DItem& it) = 0;
};

void paint_items(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, LayoutOut& lo, std::vector<ImageRes>& imgs,
                 float y0, float y1, uint64_t gen, WidgetPainter* wp);

}  // namespace tb
