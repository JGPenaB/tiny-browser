// The 2D context of <canvas> (HTML 4.12.5.1, CanvasRenderingContext2D), drawn with Direct2D on a Direct3D 11 device
// of its own (the GPU, else WARP). Each canvas is a bitmap; one device context draws on whichever canvas a script
// is using. Paths are kept in canvas coordinates as the methods add them (the transform of the moment applied, as
// the standard says); when they are filled or stroked they are mapped back through the current transform so that
// line widths, dashes and gradients are in user space.
//
// Compositing: source-over is drawn straight onto the canvas, clipped by a layer. Any other operation, and any
// shadow, draws the shape onto a scratch bitmap first, then composites that with the canvas (a Direct2D composite
// mode, or the blend effect for the blend modes) into a second scratch bitmap that replaces the canvas inside the
// clipping region.
#include "canvas.h"

#include <windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include "css.h"
#include "dom.h"
#include "js/builtins.h"
#include "svg.h"
#include "text.h"

#pragma comment(lib, "d3d11.lib")

namespace tb {

using namespace js;
using Microsoft::WRL::ComPtr;
using Mat = D2D1::Matrix3x2F;

bool webgl_canvas_pixels(Vm& vm, Node* canvas, int& w, int& h, std::vector<uint8_t>& rgba);

namespace {

constexpr double kPi = 3.14159265358979323846;
enum : uint8_t { HK_2D = 210, HK_Gradient, HK_Pattern, HK_Path };

// ---------------------------------------------------------------- the device
struct Device {
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1DeviceContext> dc;
    bool tried = false;
};
Device& device() {
    static Device d;
    if (d.tried) return d;
    d.tried = true;
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)d.factory.GetAddressOf())))
        return d;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ComPtr<ID3D11Device> d3d;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
        if (SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                        &d3d, nullptr, nullptr)))
            break;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> dev;
    if (!d3d || FAILED(d3d.As(&dxgi)) || FAILED(d.factory->CreateDevice(dxgi.Get(), &dev)) ||
        FAILED(dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d.dc)))
        d.dc.Reset();
    if (d.dc) {
        d.dc->SetDpi(96, 96);
        d.dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    return d;
}

// ---------------------------------------------------------------- paths
// A path: subpaths of lines and cubic curves (quadratic curves and arcs become cubics), each point as it was given
// after the transform of its moment
struct Seg {
    enum K : uint8_t { Move, Line, Cubic, Close } k;
    float p[6];
};
struct Path {
    std::vector<Seg> segs;
    bool open = false;      // there is a subpath to go on from
    float lx = 0, ly = 0;   // its last point
    float sx = 0, sy = 0;   // its first
    void move(float x, float y) {
        segs.push_back({Seg::Move, {x, y}});
        open = true;
        lx = sx = x;
        ly = sy = y;
    }
    void line(float x, float y) {
        if (!open) return move(x, y);
        segs.push_back({Seg::Line, {x, y}});
        lx = x;
        ly = y;
    }
    void cubic(float x1, float y1, float x2, float y2, float x, float y) {
        if (!open) move(x1, y1);
        segs.push_back({Seg::Cubic, {x1, y1, x2, y2, x, y}});
        lx = x;
        ly = y;
    }
    void close() {  // the subpath is closed, and the next starts where it did
        if (!open) return;
        if (segs.back().k != Seg::Close) segs.push_back({Seg::Close, {}});
        lx = sx;
        ly = sy;
    }
};
D2D1_POINT_2F at(const Mat& m, double x, double y) { return m.TransformPoint(D2D1::Point2F((float)x, (float)y)); }

// An arc of an ellipse (HTML 4.12.5.1.10, ellipse()): a line from the last point to its start, then cubic curves,
// a quarter turn at most each. Angles in radians; the ellipse is rotated by `rot`.
void addArc(Path& p, const Mat& m, double cx, double cy, double rx, double ry, double rot, double a0, double a1, bool ccw) {
    double sweep = ccw ? a0 - a1 : a1 - a0;
    if (sweep >= 2 * kPi) sweep = 2 * kPi;
    else {
        sweep = std::fmod(sweep, 2 * kPi);
        if (sweep < 0) sweep += 2 * kPi;
    }
    if (ccw) sweep = -sweep;
    double cr = std::cos(rot), sr = std::sin(rot);
    auto point = [&](double t, double& x, double& y) {
        double ex = rx * std::cos(t), ey = ry * std::sin(t);
        x = cx + ex * cr - ey * sr;
        y = cy + ex * sr + ey * cr;
    };
    auto slope = [&](double t, double& x, double& y) {
        double ex = -rx * std::sin(t), ey = ry * std::cos(t);
        x = ex * cr - ey * sr;
        y = ex * sr + ey * cr;
    };
    double x, y;
    point(a0, x, y);
    D2D1_POINT_2F s = at(m, x, y);
    p.line(s.x, s.y);
    int pieces = std::max(1, (int)std::ceil(std::abs(sweep) / (kPi / 2) - 1e-9));
    double step = sweep / pieces, k = 4.0 / 3.0 * std::tan(step / 4);
    for (int i = 0; i < pieces; ++i) {
        double t0 = a0 + step * i, t1 = t0 + step, x0, y0, dx0, dy0, x1, y1, dx1, dy1;
        point(t0, x0, y0);
        slope(t0, dx0, dy0);
        point(t1, x1, y1);
        slope(t1, dx1, dy1);
        D2D1_POINT_2F c1 = at(m, x0 + k * dx0, y0 + k * dy0), c2 = at(m, x1 - k * dx1, y1 - k * dy1), e = at(m, x1, y1);
        p.cubic(c1.x, c1.y, c2.x, c2.y, e.x, e.y);
    }
}
// The outline of a path as Direct2D geometry, its points mapped by `m`
ComPtr<ID2D1PathGeometry> geometryOf(const Path& p, const Mat& m, bool evenOdd) {
    ComPtr<ID2D1PathGeometry> g;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(device().factory->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) return nullptr;
    sink->SetFillMode(evenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
    bool figure = false;
    D2D1_POINT_2F start{};
    for (const Seg& s : p.segs) {
        switch (s.k) {
            case Seg::Move:
                if (figure) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                start = m.TransformPoint({s.p[0], s.p[1]});
                sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
                figure = true;
                break;
            case Seg::Line:
            case Seg::Cubic:
                if (!figure) {  // after a closed subpath: a new one from where that one started
                    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
                    figure = true;
                }
                if (s.k == Seg::Line) sink->AddLine(m.TransformPoint({s.p[0], s.p[1]}));
                else sink->AddBezier(D2D1::BezierSegment(m.TransformPoint({s.p[0], s.p[1]}), m.TransformPoint({s.p[2], s.p[3]}), m.TransformPoint({s.p[4], s.p[5]})));
                break;
            case Seg::Close:
                if (figure) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                figure = false;
                break;
        }
    }
    if (figure) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    return g;
}

// ---------------------------------------------------------------- script objects that Direct2D needs (freed by the collector)
struct GradientObj : JSHost {
    uint8_t kind = 0;  // 0 linear (x0 y0 x1 y1), 1 radial (x0 y0 r0 x1 y1 r1), 2 conic (angle x y)
    double p[6] = {};
    std::vector<D2D1_GRADIENT_STOP> stops;  // straight colours, in the order of their offsets
};
struct PatternObj : JSHost {
    ComPtr<ID2D1Bitmap1> bitmap;  // with a transparent pixel around it on an axis that does not repeat
    bool repX = true, repY = true;
    Mat m = Mat::Identity();
};
struct PathObj : JSHost {
    Path path;
};

// ---------------------------------------------------------------- contexts
struct Paint {
    Color c{0, 0, 0, 1};
    Value object = Value::null();  // a CanvasGradient or CanvasPattern instead of the colour
};
struct State {  // the drawing state (4.12.5.1.2)
    Mat m = Mat::Identity();
    ComPtr<ID2D1Geometry> clip;  // in canvas coordinates; null for none
    Paint fill, stroke;
    float alpha = 1;
    uint8_t op = 0;  // index into kOps
    float lineWidth = 1, miter = 10, dashOffset = 0;
    uint8_t cap = 0, join = 0;  // butt round square; miter round bevel
    std::vector<float> dash;
    float shadowX = 0, shadowY = 0, shadowBlur = 0;
    Color shadowColor{0, 0, 0, 0};
    std::string font = "10px sans-serif";
    std::shared_ptr<const Style> fontStyle;
    uint8_t align = 0, baseline = 0, direction = 0;  // kAligns, kBaselines, kDirections
    bool smoothing = true;
    uint8_t quality = 0;
    std::string filter = "none", letterSpacing = "0px", wordSpacing = "0px", kerning = "auto", textRendering = "auto", stretch = "normal",
                caps = "normal";
};
const char* const kOps[] = {"source-over", "source-in", "source-out", "source-atop", "destination-over", "destination-in",
                            "destination-out", "destination-atop", "lighter", "copy", "xor", "multiply", "screen", "overlay",
                            "darken", "lighten", "color-dodge", "color-burn", "hard-light", "soft-light", "difference",
                            "exclusion", "hue", "saturation", "color", "luminosity"};
const D2D1_COMPOSITE_MODE kComposite[] = {D2D1_COMPOSITE_MODE_SOURCE_OVER, D2D1_COMPOSITE_MODE_SOURCE_IN, D2D1_COMPOSITE_MODE_SOURCE_OUT,
                                          D2D1_COMPOSITE_MODE_SOURCE_ATOP, D2D1_COMPOSITE_MODE_DESTINATION_OVER,
                                          D2D1_COMPOSITE_MODE_DESTINATION_IN, D2D1_COMPOSITE_MODE_DESTINATION_OUT,
                                          D2D1_COMPOSITE_MODE_DESTINATION_ATOP, D2D1_COMPOSITE_MODE_PLUS,
                                          D2D1_COMPOSITE_MODE_SOURCE_COPY, D2D1_COMPOSITE_MODE_XOR};
const D2D1_BLEND_MODE kBlend[] = {D2D1_BLEND_MODE_MULTIPLY, D2D1_BLEND_MODE_SCREEN, D2D1_BLEND_MODE_OVERLAY, D2D1_BLEND_MODE_DARKEN,
                                  D2D1_BLEND_MODE_LIGHTEN, D2D1_BLEND_MODE_COLOR_DODGE, D2D1_BLEND_MODE_COLOR_BURN,
                                  D2D1_BLEND_MODE_HARD_LIGHT, D2D1_BLEND_MODE_SOFT_LIGHT, D2D1_BLEND_MODE_DIFFERENCE,
                                  D2D1_BLEND_MODE_EXCLUSION, D2D1_BLEND_MODE_HUE, D2D1_BLEND_MODE_SATURATION, D2D1_BLEND_MODE_COLOR,
                                  D2D1_BLEND_MODE_LUMINOSITY};
const char* const kCaps[] = {"butt", "round", "square"};
const char* const kJoins[] = {"miter", "round", "bevel"};
const char* const kAligns[] = {"start", "end", "left", "right", "center"};
const char* const kBaselines[] = {"alphabetic", "top", "hanging", "middle", "ideographic", "bottom"};
const char* const kDirections[] = {"inherit", "ltr", "rtl"};
const char* const kQualities[] = {"low", "medium", "high"};

struct Ctx2D {
    Node* canvas = nullptr;
    JSObject* self = nullptr;
    int w = 0, h = 0;
    bool alpha = true, dirty = false, clean = true, willRead = false;
    ComPtr<ID2D1Bitmap1> bmp, scratch[5], readback;
    State st;
    std::vector<State> saved;
    Path path;
};
struct VmCanvases {
    CanvasHost* host = nullptr;
    JSObject *ctxProto = nullptr, *gradientProto = nullptr, *patternProto = nullptr, *pathProto = nullptr, *metricsProto = nullptr;
    std::vector<std::unique_ptr<Ctx2D>> contexts;
    std::unordered_map<int, ComPtr<ID2D1Bitmap1>> pictures;  // the page's pictures as bitmaps, by the host's id
};
std::unordered_map<Vm*, VmCanvases>& registry() {
    static std::unordered_map<Vm*, VmCanvases> r;
    return r;
}
VmCanvases& vmc(Vm& vm) { return registry()[&vm]; }
Ctx2D* g_open = nullptr;  // the canvas the device context draws on now, between BeginDraw and EndDraw

void endDraw() {
    if (!g_open) return;
    device().dc->EndDraw();
    device().dc->SetTarget(nullptr);
    g_open = nullptr;
}
ComPtr<ID2D1Bitmap1> newBitmap(int w, int h, D2D1_BITMAP_OPTIONS options, bool opaque = false) {
    ComPtr<ID2D1Bitmap1> b;
    device().dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
                              D2D1::BitmapProperties1(options, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, opaque ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED)),
                              &b);
    return b;
}
// The canvas's bitmap and state as when it was made: transparent black (opaque black without alpha), a fresh state
// and an empty path (4.12.5.1.1). Called when the canvas gets a size, even the one it had.
void reset(Ctx2D& c) {
    if (g_open == &c) endDraw();
    c.w = canvas_dimension(c.canvas, false);
    c.h = canvas_dimension(c.canvas, true);
    c.bmp.Reset();
    for (auto& s : c.scratch) s.Reset();
    c.readback.Reset();
    c.st = State();
    c.saved.clear();
    c.path = Path();
    c.clean = true;
    c.dirty = true;
    if (c.w <= 0 || c.h <= 0 || c.w > 16384 || c.h > 16384 || !device().dc) return;
    c.bmp = newBitmap(c.w, c.h, D2D1_BITMAP_OPTIONS_TARGET, !c.alpha);
    if (!c.bmp) return;
    auto* dc = device().dc.Get();
    dc->SetTarget(c.bmp.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, c.alpha ? 0.f : 1.f));
    dc->EndDraw();
    dc->SetTarget(nullptr);
}
// Ready to draw on this canvas: false when it has no bitmap (a size of 0)
bool begin(Ctx2D& c) {
    if (canvas_dimension(c.canvas, false) != c.w || canvas_dimension(c.canvas, true) != c.h) reset(c);
    if (!c.bmp) return false;
    if (g_open != &c) {
        endDraw();
        device().dc->SetTarget(c.bmp.Get());
        device().dc->BeginDraw();
        device().dc->SetTransform(Mat::Identity());
        g_open = &c;
    }
    return true;
}
ID2D1Bitmap1* scratch(Ctx2D& c, int i) {
    if (!c.scratch[i]) c.scratch[i] = newBitmap(c.w, c.h, D2D1_BITMAP_OPTIONS_TARGET);
    return c.scratch[i].Get();
}
void pushClip(Ctx2D& c) {
    if (c.st.clip)
        device().dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), c.st.clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, Mat::Identity(), 1,
                                                      nullptr, D2D1_LAYER_OPTIONS1_NONE),
                               nullptr);
}
void popClip(Ctx2D& c) {
    if (c.st.clip) device().dc->PopLayer();
}
void drawImageAt(ID2D1Image* image, D2D1_COMPOSITE_MODE mode, D2D1_POINT_2F offset = {0, 0}) {
    device().dc->DrawImage(image, &offset, nullptr, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, mode);
}
// Puts `src` (canvas sized) on the canvas with the composite operation `op`, inside the clipping region (4.12.5.1.21)
void composite(Ctx2D& c, ID2D1Image* src, uint8_t op) {
    auto* dc = device().dc.Get();
    dc->SetTransform(Mat::Identity());
    if (op == 0) {
        dc->SetTarget(c.bmp.Get());
        pushClip(c);
        drawImageAt(src, D2D1_COMPOSITE_MODE_SOURCE_OVER);
        popClip(c);
        return;
    }
    ID2D1Bitmap1* result = scratch(c, 2);
    if (!result) return;
    dc->SetTarget(result);
    dc->Clear(D2D1::ColorF(0, 0));
    if (op < ARRAYSIZE(kComposite)) {
        drawImageAt(c.bmp.Get(), D2D1_COMPOSITE_MODE_SOURCE_COPY);
        drawImageAt(src, kComposite[op]);
    } else {
        ComPtr<ID2D1Effect> blend;
        if (SUCCEEDED(dc->CreateEffect(CLSID_D2D1Blend, &blend))) {
            blend->SetInput(0, c.bmp.Get());  // the destination
            blend->SetInput(1, src);
            blend->SetValue(D2D1_BLEND_PROP_MODE, kBlend[op - ARRAYSIZE(kComposite)]);
            ComPtr<ID2D1Image> out;
            blend->GetOutput(&out);
            drawImageAt(out.Get(), D2D1_COMPOSITE_MODE_SOURCE_COPY);
        }
    }
    dc->SetTarget(c.bmp.Get());
    if (!c.st.clip) {
        drawImageAt(result, D2D1_COMPOSITE_MODE_SOURCE_COPY);
        return;
    }
    // inside the clip only: the clip's area of the canvas is taken out, then the result's same area put in
    ID2D1Bitmap1 *mask = scratch(c, 3), *part = scratch(c, 4);
    if (!mask || !part) return;
    ComPtr<ID2D1SolidColorBrush> white;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    dc->SetTarget(mask);
    dc->Clear(D2D1::ColorF(0, 0));
    dc->FillGeometry(c.st.clip.Get(), white.Get());
    dc->SetTarget(part);
    dc->Clear(D2D1::ColorF(0, 0));
    pushClip(c);
    drawImageAt(result, D2D1_COMPOSITE_MODE_SOURCE_OVER);
    popClip(c);
    dc->SetTarget(c.bmp.Get());
    drawImageAt(mask, D2D1_COMPOSITE_MODE_DESTINATION_OUT);
    drawImageAt(part, D2D1_COMPOSITE_MODE_PLUS);
}
// Draws one operation: draw() paints it with the current transform in place. Shadows and composite operations
// other than source-over go through the scratch bitmaps (4.12.5.1.20, 4.12.5.1.21).
template <class F>
void paint(Ctx2D& c, F&& draw) {
    if (!begin(c)) return;
    auto* dc = device().dc.Get();
    const State& s = c.st;
    bool shadow = s.shadowColor.a > 0 && (s.shadowBlur > 0 || s.shadowX != 0 || s.shadowY != 0);
    c.dirty = true;
    if (s.op == 0 && !shadow) {
        pushClip(c);
        dc->SetTransform(s.m);
        draw(dc);
        dc->SetTransform(Mat::Identity());
        popClip(c);
        return;
    }
    ID2D1Bitmap1* shape = scratch(c, 0);
    if (!shape) return;
    dc->SetTarget(shape);
    dc->Clear(D2D1::ColorF(0, 0));
    dc->SetTransform(s.m);
    draw(dc);
    dc->SetTransform(Mat::Identity());
    ID2D1Image* src = shape;
    if (shadow) {  // the shadow, then the shape over it
        ID2D1Bitmap1* both = scratch(c, 1);
        ComPtr<ID2D1Effect> fx;
        if (both && SUCCEEDED(dc->CreateEffect(CLSID_D2D1Shadow, &fx))) {
            fx->SetInput(0, shape);
            fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, s.shadowBlur / 2);
            fx->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(s.shadowColor.r, s.shadowColor.g, s.shadowColor.b, s.shadowColor.a));
            ComPtr<ID2D1Image> out;
            fx->GetOutput(&out);
            dc->SetTarget(both);
            dc->Clear(D2D1::ColorF(0, 0));
            drawImageAt(out.Get(), D2D1_COMPOSITE_MODE_SOURCE_OVER, {s.shadowX, s.shadowY});
            drawImageAt(shape, D2D1_COMPOSITE_MODE_SOURCE_OVER);
            src = both;
        }
    }
    composite(c, src, s.op);
}

// The context a method was called on
Ctx2D& ctxOf(CallInfo& ci) {
    if (ci.thisv.isObject() && ci.thisv.asObject()->cls == Cls::Host) {
        JSHost* h = (JSHost*)ci.thisv.asObject();
        if (h->hostKind == HK_2D && h->ptr) return *(Ctx2D*)h->ptr;
    }
    ci.vm.throwError(Err::TypeError, "Illegal invocation: not a CanvasRenderingContext2D");
}
template <class T>
T* hostOf(Value v, uint8_t kind) {
    if (!v.isObject() || v.asObject()->cls != Cls::Host || ((JSHost*)v.asObject())->hostKind != kind) return nullptr;
    return (T*)v.asObject();
}
double num(CallInfo& ci, int i) { return ci.vm.toNumber(ci.arg(i)); }
// The numbers of arguments from..from+n-1; false when one is not finite, which makes most methods do nothing
bool finite(CallInfo& ci, int from, int n, double* out) {
    bool ok = true;
    for (int i = 0; i < n; ++i) {
        out[i] = num(ci, from + i);
        ok = ok && std::isfinite(out[i]);
    }
    return ok;
}
Value str(Vm& vm, const std::string& s) { return vm.strv(std::string_view(s)); }
std::string str8(Vm& vm, Value v) { return utf16to8(vm.toStr16(v)); }
[[noreturn]] void domError(CallInfo& ci, const char* name, const char* message) {
    Value ctor = ci.vm.get(Value::cell(ci.vm.global), "DOMException");
    Value args[] = {str(ci.vm, message), str(ci.vm, name)};
    ci.vm.throwValue(ci.vm.construct(ctor, args, 2, ctor));
}
std::string colorText(const Color& c) {  // the serialisation of a colour (4.12.5.1.5)
    int r = (int)std::lround(c.r * 255), g = (int)std::lround(c.g * 255), b = (int)std::lround(c.b * 255);
    char buf[64];
    if (c.a >= 1) {
        snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
        return buf;
    }
    snprintf(buf, sizeof buf, "%.3f", c.a);
    std::string a = buf;
    while (a.back() == '0') a.pop_back();
    if (a.back() == '.') a.pop_back();
    snprintf(buf, sizeof buf, "rgba(%d, %d, %d, %s)", r, g, b, a.c_str());
    return buf;
}

// ---------------------------------------------------------------- brushes
ComPtr<ID2D1Brush> brushOf(Ctx2D& c, const Paint& p) {
    auto* dc = device().dc.Get();
    float alpha = c.st.alpha;
    ComPtr<ID2D1Brush> out;
    if (auto* g = hostOf<GradientObj>(p.object, HK_Gradient)) {
        if (g->stops.empty()) return nullptr;  // paints nothing
        if (g->stops.size() == 1 || g->kind == 2) {  // conic gradients are drawn with their first colour for now
            ComPtr<ID2D1SolidColorBrush> b;
            dc->CreateSolidColorBrush(g->stops[0].color, D2D1::BrushProperties(alpha), &b);
            return b;
        }
        std::vector<D2D1_GRADIENT_STOP> stops = g->stops;
        double* q = g->p;
        if (g->kind == 1 && q[5] > 0 && q[2] > 0) {  // a start circle of its own: the stops move out to it
            double from = std::min(1.0, q[2] / q[5]);
            for (auto& s : stops) s.position = (float)(from + s.position * (1 - from));
        }
        ComPtr<ID2D1GradientStopCollection1> coll;
        dc->CreateGradientStopCollection(stops.data(), (UINT32)stops.size(), D2D1_COLOR_SPACE_SRGB, D2D1_COLOR_SPACE_SRGB,
                                         D2D1_BUFFER_PRECISION_8BPC_UNORM, D2D1_EXTEND_MODE_CLAMP, D2D1_COLOR_INTERPOLATION_MODE_PREMULTIPLIED, &coll);
        if (!coll) return nullptr;
        if (g->kind == 0) {
            if (q[0] == q[2] && q[1] == q[3]) return nullptr;
            ComPtr<ID2D1LinearGradientBrush> b;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F((float)q[0], (float)q[1]), D2D1::Point2F((float)q[2], (float)q[3])),
                                          D2D1::BrushProperties(alpha), coll.Get(), &b);
            return b;
        }
        if (q[0] == q[3] && q[1] == q[4] && q[2] == q[5]) return nullptr;
        ComPtr<ID2D1RadialGradientBrush> b;
        dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F((float)q[3], (float)q[4]),
                                                                          D2D1::Point2F((float)(q[0] - q[3]), (float)(q[1] - q[4])), (float)q[5], (float)q[5]),
                                      D2D1::BrushProperties(alpha), coll.Get(), &b);
        return b;
    }
    if (auto* pt = hostOf<PatternObj>(p.object, HK_Pattern)) {
        if (!pt->bitmap) return nullptr;
        D2D1_SIZE_F size = pt->bitmap->GetSize();
        Mat m = pt->m;
        if (!pt->repX) m = Mat::Translation(-1, 0) * m;  // the transparent border
        if (!pt->repY) m = Mat::Translation(0, -1) * m;
        ComPtr<ID2D1ImageBrush> b;
        dc->CreateImageBrush(pt->bitmap.Get(),
                             D2D1::ImageBrushProperties(D2D1::RectF(0, 0, size.width, size.height), pt->repX ? D2D1_EXTEND_MODE_WRAP : D2D1_EXTEND_MODE_CLAMP,
                                                        pt->repY ? D2D1_EXTEND_MODE_WRAP : D2D1_EXTEND_MODE_CLAMP,
                                                        c.st.smoothing ? D2D1_INTERPOLATION_MODE_LINEAR : D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                             D2D1::BrushProperties(alpha, m), &b);
        return b;
    }
    ComPtr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(D2D1::ColorF(p.c.r, p.c.g, p.c.b, p.c.a * alpha), &b);
    return b;
}
ComPtr<ID2D1StrokeStyle1> strokeStyleOf(const State& s) {
    static const D2D1_CAP_STYLE caps[] = {D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_SQUARE};
    static const D2D1_LINE_JOIN joins[] = {D2D1_LINE_JOIN_MITER_OR_BEVEL, D2D1_LINE_JOIN_ROUND, D2D1_LINE_JOIN_BEVEL};
    std::vector<float> dashes;  // Direct2D counts them in line widths
    for (float d : s.dash) dashes.push_back(d / s.lineWidth);
    ComPtr<ID2D1StrokeStyle1> ss;
    device().factory->CreateStrokeStyle(D2D1::StrokeStyleProperties1(caps[s.cap], caps[s.cap], caps[s.cap], joins[s.join], s.miter,
                                                                     dashes.empty() ? D2D1_DASH_STYLE_SOLID : D2D1_DASH_STYLE_CUSTOM,
                                                                     s.dashOffset / s.lineWidth),
                                        dashes.empty() ? nullptr : dashes.data(), (UINT32)dashes.size(), &ss);
    return ss;
}
// The current transform's inverse, to take canvas coordinates back to user space; false when it has none
bool inverse(const State& s, Mat& out) {
    out = s.m;
    return out.Invert();
}

// ---------------------------------------------------------------- the context's properties
enum Prop : int {
    P_Alpha, P_Op, P_Fill, P_Stroke, P_LineWidth, P_Cap, P_Join, P_Miter, P_DashOffset, P_ShadowX, P_ShadowY, P_ShadowBlur, P_ShadowColor,
    P_Font, P_Align, P_Baseline, P_Direction, P_Smoothing, P_Quality, P_Filter, P_LetterSpacing, P_WordSpacing, P_Kerning, P_TextRendering,
    P_Stretch, P_Caps
};
template <size_t N>
int indexOf(const char* const (&list)[N], const std::string& s) {
    for (size_t i = 0; i < N; ++i)
        if (s == list[i]) return (int)i;
    return -1;
}
Value paintGet(Vm& vm, const Paint& p) { return p.object.isNull() ? str(vm, colorText(p.c)) : p.object; }
void paintSet(Vm& vm, Paint& p, Value v) {
    if (hostOf<GradientObj>(v, HK_Gradient) || hostOf<PatternObj>(v, HK_Pattern)) {
        p.object = v;
        return;
    }
    Color c;
    if (v.isObject() || !parse_color(trim(str8(vm, v)), c)) return;  // not a colour: ignored
    p.c = c;
    p.object = Value::null();
}
Value propGet(CallInfo& ci) {
    Ctx2D& c = ctxOf(ci);
    Vm& vm = ci.vm;
    const State& s = c.st;
    switch (ci.callee->data.asInt()) {
        case P_Alpha: return Value::num(s.alpha);
        case P_Op: return str(vm, kOps[s.op]);
        case P_Fill: return paintGet(vm, s.fill);
        case P_Stroke: return paintGet(vm, s.stroke);
        case P_LineWidth: return Value::num(s.lineWidth);
        case P_Cap: return str(vm, kCaps[s.cap]);
        case P_Join: return str(vm, kJoins[s.join]);
        case P_Miter: return Value::num(s.miter);
        case P_DashOffset: return Value::num(s.dashOffset);
        case P_ShadowX: return Value::num(s.shadowX);
        case P_ShadowY: return Value::num(s.shadowY);
        case P_ShadowBlur: return Value::num(s.shadowBlur);
        case P_ShadowColor: return str(vm, colorText(s.shadowColor));
        case P_Font: return str(vm, s.font);
        case P_Align: return str(vm, kAligns[s.align]);
        case P_Baseline: return str(vm, kBaselines[s.baseline]);
        case P_Direction: return str(vm, kDirections[s.direction]);
        case P_Smoothing: return Value::boolean(s.smoothing);
        case P_Quality: return str(vm, kQualities[s.quality]);
        case P_Filter: return str(vm, s.filter);
        case P_LetterSpacing: return str(vm, s.letterSpacing);
        case P_WordSpacing: return str(vm, s.wordSpacing);
        case P_Kerning: return str(vm, s.kerning);
        case P_TextRendering: return str(vm, s.textRendering);
        case P_Stretch: return str(vm, s.stretch);
        case P_Caps: return str(vm, s.caps);
    }
    return Value::undef();
}
// A canvas's font (4.12.5.1.4): the CSS font shorthand, its relative sizes of the canvas element's own font
bool setFont(Ctx2D& c, const std::string& text) {
    static const Style dflt = [] {
        Style s;
        s.fontSize = 10;
        return s;
    }();
    const Style& parent = c.canvas && c.canvas->style ? *c.canvas->style : dflt;
    auto st = std::make_shared<Style>();
    if (!parse_font(text, parent, *st)) return false;
    c.st.font = std::string(trim(text));
    c.st.fontStyle = std::move(st);
    return true;
}
Value propSet(CallInfo& ci) {
    Ctx2D& c = ctxOf(ci);
    Vm& vm = ci.vm;
    State& s = c.st;
    Value v = ci.arg(0);
    auto positive = [&](float& field) {  // values that are not finite, or not above 0, are ignored
        double d = vm.toNumber(v);
        if (std::isfinite(d) && d > 0) field = (float)d;
    };
    auto finiteSet = [&](float& field) {
        double d = vm.toNumber(v);
        if (std::isfinite(d)) field = (float)d;
    };
    auto keyword = [&](auto& list, uint8_t& field) {
        int i = indexOf(list, str8(vm, v));
        if (i >= 0) field = (uint8_t)i;
    };
    switch (ci.callee->data.asInt()) {
        case P_Alpha: {
            double d = vm.toNumber(v);
            if (std::isfinite(d) && d >= 0 && d <= 1) s.alpha = (float)d;
            break;
        }
        case P_Op: keyword(kOps, s.op); break;
        case P_Fill: paintSet(vm, s.fill, v); break;
        case P_Stroke: paintSet(vm, s.stroke, v); break;
        case P_LineWidth: positive(s.lineWidth); break;
        case P_Cap: keyword(kCaps, s.cap); break;
        case P_Join: keyword(kJoins, s.join); break;
        case P_Miter: positive(s.miter); break;
        case P_DashOffset: finiteSet(s.dashOffset); break;
        case P_ShadowX: finiteSet(s.shadowX); break;
        case P_ShadowY: finiteSet(s.shadowY); break;
        case P_ShadowBlur: {
            double d = vm.toNumber(v);
            if (std::isfinite(d) && d >= 0) s.shadowBlur = (float)d;
            break;
        }
        case P_ShadowColor: {
            Color col;
            if (parse_color(trim(str8(vm, v)), col)) s.shadowColor = col;
            break;
        }
        case P_Font: setFont(c, str8(vm, v)); break;
        case P_Align: keyword(kAligns, s.align); break;
        case P_Baseline: keyword(kBaselines, s.baseline); break;
        case P_Direction: keyword(kDirections, s.direction); break;
        case P_Smoothing: s.smoothing = vm.toBool(v); break;
        case P_Quality: keyword(kQualities, s.quality); break;
        case P_Filter: s.filter = str8(vm, v); break;
        case P_LetterSpacing: s.letterSpacing = str8(vm, v); break;
        case P_WordSpacing: s.wordSpacing = str8(vm, v); break;
        case P_Kerning: s.kerning = str8(vm, v); break;
        case P_TextRendering: s.textRendering = str8(vm, v); break;
        case P_Stretch: s.stretch = str8(vm, v); break;
        case P_Caps: s.caps = str8(vm, v); break;
    }
    return Value::undef();
}

// ---------------------------------------------------------------- state and transforms
JSFN(save) {
    Ctx2D& c = ctxOf(ci);
    c.saved.push_back(c.st);
    return Value::undef();
}
JSFN(restore) {
    Ctx2D& c = ctxOf(ci);
    if (c.saved.empty()) return Value::undef();
    c.st = std::move(c.saved.back());
    c.saved.pop_back();
    return Value::undef();
}
JSFN(resetFn) {  // reset(): as when the context was made, without a new bitmap size
    Ctx2D& c = ctxOf(ci);
    reset(c);
    return Value::undef();
}
void multiply(Ctx2D& c, const Mat& by) { c.st.m = by * c.st.m; }
JSFN(scale) {
    double a[2];
    if (finite(ci, 0, 2, a)) multiply(ctxOf(ci), Mat::Scale((float)a[0], (float)a[1]));
    return Value::undef();
}
JSFN(rotate) {
    double a[1];
    if (finite(ci, 0, 1, a)) multiply(ctxOf(ci), Mat::Rotation((float)(a[0] * 180 / kPi)));
    return Value::undef();
}
JSFN(translate) {
    double a[2];
    if (finite(ci, 0, 2, a)) multiply(ctxOf(ci), Mat::Translation((float)a[0], (float)a[1]));
    return Value::undef();
}
JSFN(transform) {
    double a[6];
    if (finite(ci, 0, 6, a)) multiply(ctxOf(ci), Mat((float)a[0], (float)a[1], (float)a[2], (float)a[3], (float)a[4], (float)a[5]));
    return Value::undef();
}
// A DOMMatrix2DInit (Geometry Interfaces 1, 6.2): a..f or m11..m42, identity where missing
bool matrixInit(CallInfo& ci, Value v, Mat& out) {
    Vm& vm = ci.vm;
    out = Mat::Identity();
    if (v.isNullish()) return true;
    if (!v.isObject()) vm.throwError(Err::TypeError, "The provided value is not of type 'DOMMatrix2DInit'");
    const char* names[6][2] = {{"a", "m11"}, {"b", "m12"}, {"c", "m21"}, {"d", "m22"}, {"e", "m41"}, {"f", "m42"}};
    float* f[6] = {&out._11, &out._12, &out._21, &out._22, &out._31, &out._32};
    for (int i = 0; i < 6; ++i) {
        Value x = vm.get(v, names[i][0]), y = vm.get(v, names[i][1]);
        Value use = x.isUndef() ? y : x;
        if (!use.isUndef()) {
            double d = vm.toNumber(use);
            if (!std::isfinite(d)) return false;
            *f[i] = (float)d;
        }
    }
    return true;
}
JSFN(setTransform) {
    Ctx2D& c = ctxOf(ci);
    if (ci.argc >= 6) {
        double a[6];
        if (finite(ci, 0, 6, a)) c.st.m = Mat((float)a[0], (float)a[1], (float)a[2], (float)a[3], (float)a[4], (float)a[5]);
        return Value::undef();
    }
    Mat m;
    if (matrixInit(ci, ci.arg(0), m)) c.st.m = m;
    return Value::undef();
}
JSFN(resetTransform) {
    ctxOf(ci).st.m = Mat::Identity();
    return Value::undef();
}
Value domMatrix(Vm& vm, const Mat& m) {
    Value ctor = vm.get(Value::cell(vm.global), "DOMMatrix");
    JSArray* a = vm.newArray();
    for (float f : {m._11, m._12, m._21, m._22, m._31, m._32}) a->elems.push_back(Value::num(f));
    Value args[] = {Value::cell(a)};
    return vm.construct(ctor, args, 1, ctor);
}
JSFN(getTransform) { return domMatrix(ci.vm, ctxOf(ci).st.m); }

// ---------------------------------------------------------------- building paths (CanvasPath, 4.12.5.1.10), shared by the context and Path2D
// The path a method adds to and the transform its points go through: the context's current path with its
// current transform, or a Path2D as it is
Path& pathOf(CallInfo& ci, Mat& m) {
    if (auto* p = hostOf<PathObj>(ci.thisv, HK_Path)) {
        m = Mat::Identity();
        return p->path;
    }
    Ctx2D& c = ctxOf(ci);
    m = c.st.m;
    return c.path;
}
JSFN(closePath) {
    Mat m;
    pathOf(ci, m).close();
    return Value::undef();
}
JSFN(moveTo) {
    Mat m;
    Path& p = pathOf(ci, m);
    double a[2];
    if (!finite(ci, 0, 2, a)) return Value::undef();
    D2D1_POINT_2F q = at(m, a[0], a[1]);
    p.move(q.x, q.y);
    return Value::undef();
}
JSFN(lineTo) {
    Mat m;
    Path& p = pathOf(ci, m);
    double a[2];
    if (!finite(ci, 0, 2, a)) return Value::undef();
    D2D1_POINT_2F q = at(m, a[0], a[1]);
    p.line(q.x, q.y);
    return Value::undef();
}
JSFN(quadraticCurveTo) {
    Mat m;
    Path& p = pathOf(ci, m);
    double a[4];
    if (!finite(ci, 0, 4, a)) return Value::undef();
    D2D1_POINT_2F c1 = at(m, a[0], a[1]), e = at(m, a[2], a[3]);
    if (!p.open) p.move(c1.x, c1.y);
    float x0 = p.lx, y0 = p.ly;  // the same curve as a cubic
    p.cubic(x0 + 2.f / 3 * (c1.x - x0), y0 + 2.f / 3 * (c1.y - y0), e.x + 2.f / 3 * (c1.x - e.x), e.y + 2.f / 3 * (c1.y - e.y), e.x, e.y);
    return Value::undef();
}
JSFN(bezierCurveTo) {
    Mat m;
    Path& p = pathOf(ci, m);
    double a[6];
    if (!finite(ci, 0, 6, a)) return Value::undef();
    D2D1_POINT_2F c1 = at(m, a[0], a[1]), c2 = at(m, a[2], a[3]), e = at(m, a[4], a[5]);
    p.cubic(c1.x, c1.y, c2.x, c2.y, e.x, e.y);
    return Value::undef();
}
JSFN(arcTo) {
    Mat m;
    Path& p = pathOf(ci, m);
    double a[5];
    if (!finite(ci, 0, 5, a)) return Value::undef();
    if (a[4] < 0) domError(ci, "IndexSizeError", "The radius provided is negative.");
    D2D1_POINT_2F p1d = at(m, a[0], a[1]);
    if (!p.open) p.move(p1d.x, p1d.y);
    Mat inv = m;
    if (!inv.Invert()) return Value::undef();
    D2D1_POINT_2F p0 = inv.TransformPoint({p.lx, p.ly});  // the last point, in the method's coordinates
    double x0 = p0.x, y0 = p0.y, x1 = a[0], y1 = a[1], x2 = a[2], y2 = a[3], r = a[4];
    double v1x = x0 - x1, v1y = y0 - y1, v2x = x2 - x1, v2y = y2 - y1;
    double l1 = std::hypot(v1x, v1y), l2 = std::hypot(v2x, v2y), cross = v1x * v2y - v1y * v2x;
    if (l1 == 0 || l2 == 0 || r == 0 || std::abs(cross) < 1e-9 * l1 * l2) {
        p.line(p1d.x, p1d.y);
        return Value::undef();
    }
    v1x /= l1, v1y /= l1, v2x /= l2, v2y /= l2;
    double angle = std::acos(std::clamp(v1x * v2x + v1y * v2y, -1.0, 1.0));
    double d = r / std::tan(angle / 2);
    double t1x = x1 + v1x * d, t1y = y1 + v1y * d, t2x = x1 + v2x * d, t2y = y1 + v2y * d;
    double bx = v1x + v2x, by = v1y + v2y, bl = std::hypot(bx, by), h = r / std::sin(angle / 2);
    double cx = x1 + bx / bl * h, cy = y1 + by / bl * h;
    addArc(p, m, cx, cy, r, r, 0, std::atan2(t1y - cy, t1x - cx), std::atan2(t2y - cy, t2x - cx), cross > 0);
    return Value::undef();
}
JSFN(arc) {
    Mat m;
    Path& p = pathOf(ci, m);
    double a[5];
    if (!finite(ci, 0, 5, a)) return Value::undef();
    if (a[2] < 0) domError(ci, "IndexSizeError", "The radius provided is negative.");
    addArc(p, m, a[0], a[1], a[2], a[2], 0, a[3], a[4], ci.vm.toBool(ci.arg(5)));
    return Value::undef();
}
JSFN(ellipse) {
    Mat m;
    Path& p = pathOf(ci, m);
    double a[7];
    if (!finite(ci, 0, 7, a)) return Value::undef();
    if (a[2] < 0 || a[3] < 0) domError(ci, "IndexSizeError", "The radius provided is negative.");
    addArc(p, m, a[0], a[1], a[2], a[3], a[4], a[5], a[6], ci.vm.toBool(ci.arg(7)));
    return Value::undef();
}
void addRect(Path& p, const Mat& m, double x, double y, double w, double h) {
    D2D1_POINT_2F q[4] = {at(m, x, y), at(m, x + w, y), at(m, x + w, y + h), at(m, x, y + h)};
    p.move(q[0].x, q[0].y);
    for (int i = 1; i < 4; ++i) p.line(q[i].x, q[i].y);
    p.close();
    p.move(q[0].x, q[0].y);
}
JSFN(rect) {
    Mat m;
    Path& p = pathOf(ci, m);
    double a[4];
    if (finite(ci, 0, 4, a)) addRect(p, m, a[0], a[1], a[2], a[3]);
    return Value::undef();
}
// roundRect(x, y, w, h, radii) (4.12.5.1.10): radii are 1 to 4 numbers or DOMPointInit, as CSS border-radius takes them
JSFN(roundRect) {
    Mat m;
    Path& p = pathOf(ci, m);
    Vm& vm = ci.vm;
    double a[4];
    if (!finite(ci, 0, 4, a)) return Value::undef();
    std::vector<Value> list;
    Value radii = ci.arg(4);
    if (radii.isUndef()) list.push_back(Value::i32(0));
    else if (radii.isObject() && radii.asObject()->cls == Cls::Array) list = ((JSArray*)radii.asObject())->elems;
    else list.push_back(radii);
    if (list.empty() || list.size() > 4) vm.throwError(Err::RangeError, "The radii must be 1 to 4 values.");
    double rx[4], ry[4], given[4][2];
    for (size_t i = 0; i < list.size(); ++i) {
        Value r = list[i];
        double x, y;
        if (r.isObject()) {
            x = vm.toNumber(vm.get(r, "x"));
            y = vm.toNumber(vm.get(r, "y"));
        } else
            x = y = vm.toNumber(r);
        if (!std::isfinite(x) || !std::isfinite(y)) return Value::undef();
        if (x < 0 || y < 0) vm.throwError(Err::RangeError, "A radius is negative.");
        given[i][0] = x;
        given[i][1] = y;
    }
    static const int pick[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};  // upper-left, upper-right, lower-right, lower-left
    for (int k = 0; k < 4; ++k) {
        rx[k] = given[pick[list.size() - 1][k]][0];
        ry[k] = given[pick[list.size() - 1][k]][1];
    }
    double x = a[0], y = a[1], w = a[2], h = a[3];
    if (w < 0) {  // drawn the other way round, with the corners swapped to match
        x += w;
        w = -w;
        std::swap(rx[0], rx[1]), std::swap(ry[0], ry[1]), std::swap(rx[2], rx[3]), std::swap(ry[2], ry[3]);
    }
    if (h < 0) {
        y += h;
        h = -h;
        std::swap(rx[0], rx[3]), std::swap(ry[0], ry[3]), std::swap(rx[1], rx[2]), std::swap(ry[1], ry[2]);
    }
    double scale = 1;  // radii that do not fit are scaled down together
    auto fit = [&](double sum, double side) {
        if (sum > side && sum > 0) scale = std::min(scale, side / sum);
    };
    fit(rx[0] + rx[1], w), fit(ry[1] + ry[2], h), fit(rx[2] + rx[3], w), fit(ry[3] + ry[0], h);
    for (int k = 0; k < 4; ++k) rx[k] *= scale, ry[k] *= scale;
    D2D1_POINT_2F s = at(m, x + rx[0], y);
    p.move(s.x, s.y);
    D2D1_POINT_2F q = at(m, x + w - rx[1], y);
    p.line(q.x, q.y);
    addArc(p, m, x + w - rx[1], y + ry[1], rx[1], ry[1], 0, -kPi / 2, 0, false);
    q = at(m, x + w, y + h - ry[2]);
    p.line(q.x, q.y);
    addArc(p, m, x + w - rx[2], y + h - ry[2], rx[2], ry[2], 0, 0, kPi / 2, false);
    q = at(m, x + rx[3], y + h);
    p.line(q.x, q.y);
    addArc(p, m, x + rx[3], y + h - ry[3], rx[3], ry[3], 0, kPi / 2, kPi, false);
    q = at(m, x, y + ry[0]);
    p.line(q.x, q.y);
    addArc(p, m, x + rx[0], y + ry[0], rx[0], ry[0], 0, kPi, 3 * kPi / 2, false);
    p.close();
    p.move(at(m, x, y).x, at(m, x, y).y);
    return Value::undef();
}

// ---------------------------------------------------------------- drawing paths (4.12.5.1.14)
JSFN(beginPath) {
    ctxOf(ci).path = Path();
    return Value::undef();
}
bool evenOddArg(CallInfo& ci, int i) {
    Value v = ci.arg(i);
    if (v.isUndef()) return false;
    std::string r = str8(ci.vm, v);
    if (r == "evenodd") return true;
    if (r == "nonzero") return false;
    ci.vm.throwError(Err::TypeError, "The provided value '%s' is not a valid enum value of type CanvasFillRule.", r.c_str());
}
// fill(), stroke() and clip() take either their path or none (the current one): the path, in canvas coordinates,
// and the index of the argument after it
const Path& pathArg(CallInfo& ci, Ctx2D& c, int& next, Path& scratchPath) {
    next = 0;
    if (auto* p = hostOf<PathObj>(ci.arg(0), HK_Path)) {
        next = 1;
        scratchPath = Path();  // a Path2D's points go through the current transform now
        for (const Seg& s : p->path.segs) {
            Seg t = s;
            for (int k = 0; k < 6; k += 2) {
                if ((s.k == Seg::Move || s.k == Seg::Line) && k > 0) break;
                if (s.k == Seg::Close) break;
                D2D1_POINT_2F q = c.st.m.TransformPoint({s.p[k], s.p[k + 1]});
                t.p[k] = q.x;
                t.p[k + 1] = q.y;
            }
            scratchPath.segs.push_back(t);
        }
        return scratchPath;
    }
    if (ci.arg(0).isObject()) ci.vm.throwError(Err::TypeError, "parameter 1 is not of type 'Path2D'.");
    return c.path;
}
JSFN(fill) {
    Ctx2D& c = ctxOf(ci);
    int next;
    Path tmp;
    const Path& p = pathArg(ci, c, next, tmp);
    bool evenOdd = evenOddArg(ci, next);
    Mat inv;
    if (!inverse(c.st, inv)) return Value::undef();
    auto g = geometryOf(p, inv, evenOdd);
    auto br = brushOf(c, c.st.fill);
    if (g && br) paint(c, [&](ID2D1DeviceContext* dc) { dc->FillGeometry(g.Get(), br.Get()); });
    return Value::undef();
}
JSFN(stroke) {
    Ctx2D& c = ctxOf(ci);
    int next;
    Path tmp;
    const Path& p = pathArg(ci, c, next, tmp);
    Mat inv;
    if (!inverse(c.st, inv)) return Value::undef();
    auto g = geometryOf(p, inv, false);
    auto br = brushOf(c, c.st.stroke);
    auto ss = strokeStyleOf(c.st);
    float width = c.st.lineWidth;
    if (g && br) paint(c, [&](ID2D1DeviceContext* dc) { dc->DrawGeometry(g.Get(), br.Get(), width, ss.Get()); });
    return Value::undef();
}
JSFN(clip) {
    Ctx2D& c = ctxOf(ci);
    int next;
    Path tmp;
    const Path& p = pathArg(ci, c, next, tmp);
    auto g = geometryOf(p, Mat::Identity(), evenOddArg(ci, next));
    if (!g) return Value::undef();
    if (!c.st.clip) {
        c.st.clip = g;
        return Value::undef();
    }
    ComPtr<ID2D1PathGeometry> both;  // the new clipping region is where both are
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(device().factory->CreatePathGeometry(&both)) || FAILED(both->Open(&sink))) return Value::undef();
    c.st.clip->CombineWithGeometry(g.Get(), D2D1_COMBINE_MODE_INTERSECT, nullptr, sink.Get());
    sink->Close();
    c.st.clip = both;
    return Value::undef();
}
JSFN(isPointInPath) {
    Ctx2D& c = ctxOf(ci);
    int next;
    Path tmp;
    const Path& p = pathArg(ci, c, next, tmp);
    double a[2];
    if (!finite(ci, next, 2, a)) return Value::boolean(false);
    auto g = geometryOf(p, Mat::Identity(), evenOddArg(ci, next + 2));
    BOOL in = FALSE;
    if (g) g->FillContainsPoint(D2D1::Point2F((float)a[0], (float)a[1]), nullptr, &in);
    return Value::boolean(in != FALSE);
}
JSFN(isPointInStroke) {
    Ctx2D& c = ctxOf(ci);
    int next;
    Path tmp;
    const Path& p = pathArg(ci, c, next, tmp);
    double a[2];
    if (!finite(ci, next, 2, a)) return Value::boolean(false);
    Mat inv;
    if (!inverse(c.st, inv)) return Value::boolean(false);
    auto g = geometryOf(p, inv, false);
    auto ss = strokeStyleOf(c.st);
    BOOL in = FALSE;
    if (g) g->StrokeContainsPoint(D2D1::Point2F((float)a[0], (float)a[1]), c.st.lineWidth, ss.Get(), &c.st.m, &in);
    return Value::boolean(in != FALSE);
}

// ---------------------------------------------------------------- rectangles (4.12.5.1.13)
bool rectArgs(CallInfo& ci, double a[4]) { return finite(ci, 0, 4, a); }
JSFN(clearRect) {
    Ctx2D& c = ctxOf(ci);
    double a[4];
    if (!rectArgs(ci, a) || !begin(c)) return Value::undef();
    auto* dc = device().dc.Get();
    const Mat& m = c.st.m;
    c.dirty = true;
    if (!c.st.clip && m._12 == 0 && m._21 == 0) {  // a rectangle of the canvas: cleared at once
        D2D1_POINT_2F p0 = at(m, a[0], a[1]), p1 = at(m, a[0] + a[2], a[1] + a[3]);
        dc->PushAxisAlignedClip(D2D1::RectF(std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::max(p0.x, p1.x), std::max(p0.y, p1.y)),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        dc->Clear(D2D1::ColorF(0, 0, 0, c.alpha ? 0.f : 1.f));
        dc->PopAxisAlignedClip();
        return Value::undef();
    }
    ID2D1Bitmap1* shape = scratch(c, 0);
    if (!shape) return Value::undef();
    ComPtr<ID2D1SolidColorBrush> black;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &black);
    dc->SetTarget(shape);
    dc->Clear(D2D1::ColorF(0, 0));
    dc->SetTransform(m);
    dc->FillRectangle(D2D1::RectF((float)a[0], (float)a[1], (float)(a[0] + a[2]), (float)(a[1] + a[3])), black.Get());
    composite(c, shape, 6);  // destination-out: what the rectangle covers becomes transparent
    return Value::undef();
}
JSFN(fillRect) {
    Ctx2D& c = ctxOf(ci);
    double a[4];
    if (!rectArgs(ci, a)) return Value::undef();
    auto br = brushOf(c, c.st.fill);
    D2D1_RECT_F r = D2D1::RectF((float)a[0], (float)a[1], (float)(a[0] + a[2]), (float)(a[1] + a[3]));
    if (br && a[2] != 0 && a[3] != 0) paint(c, [&](ID2D1DeviceContext* dc) { dc->FillRectangle(r, br.Get()); });
    return Value::undef();
}
JSFN(strokeRect) {
    Ctx2D& c = ctxOf(ci);
    double a[4];
    if (!rectArgs(ci, a)) return Value::undef();
    Path p;
    if (a[2] == 0 && a[3] == 0) return Value::undef();
    addRect(p, Mat::Identity(), a[0], a[1], a[2], a[3]);
    if (a[2] == 0 || a[3] == 0) {  // a line, not a closed rectangle
        p = Path();
        p.move((float)a[0], (float)a[1]);
        p.line((float)(a[0] + a[2]), (float)(a[1] + a[3]));
    }
    auto g = geometryOf(p, Mat::Identity(), false);
    auto br = brushOf(c, c.st.stroke);
    auto ss = strokeStyleOf(c.st);
    float width = c.st.lineWidth;
    if (g && br) paint(c, [&](ID2D1DeviceContext* dc) { dc->DrawGeometry(g.Get(), br.Get(), width, ss.Get()); });
    return Value::undef();
}

// ---------------------------------------------------------------- gradients and patterns (4.12.5.1.5)
Value newGradient(CallInfo& ci, uint8_t kind, int n) {
    double a[6];
    if (!finite(ci, 0, n, a)) ci.vm.throwError(Err::TypeError, "The provided double value is non-finite.");
    if (kind == 1 && (a[2] < 0 || a[5] < 0)) domError(ci, "IndexSizeError", "The radius provided is negative.");
    auto* g = ci.vm.alloc<GradientObj>();
    g->proto = vmc(ci.vm).gradientProto;
    g->hostKind = HK_Gradient;
    g->kind = kind;
    for (int i = 0; i < n; ++i) g->p[i] = a[i];
    return Value::cell(g);
}
JSFN(createLinearGradient) { ctxOf(ci); return newGradient(ci, 0, 4); }
JSFN(createRadialGradient) { ctxOf(ci); return newGradient(ci, 1, 6); }
JSFN(createConicGradient) { ctxOf(ci); return newGradient(ci, 2, 3); }
JSFN(addColorStop) {
    auto* g = hostOf<GradientObj>(ci.thisv, HK_Gradient);
    if (!g) ci.vm.throwError(Err::TypeError, "Illegal invocation: not a CanvasGradient");
    double offset = num(ci, 0);
    if (!std::isfinite(offset)) ci.vm.throwError(Err::TypeError, "The provided double value is non-finite.");
    if (offset < 0 || offset > 1) domError(ci, "IndexSizeError", "The provided value is outside the range [0, 1].");
    Color col;
    std::string text = str8(ci.vm, ci.arg(1));
    if (!parse_color(trim(text), col)) domError(ci, "SyntaxError", "The value provided could not be parsed as a color.");
    D2D1_GRADIENT_STOP stop{(float)offset, D2D1::ColorF(col.r, col.g, col.b, col.a)};
    auto it = std::upper_bound(g->stops.begin(), g->stops.end(), stop, [](auto& a, auto& b) { return a.position < b.position; });
    g->stops.insert(it, stop);  // after any of the same offset (4.12.5.1.5)
    return Value::undef();
}
// The picture of an image argument: a bitmap on the device, its size, and whether it came from another origin.
// Null with ok = true when there is nothing to draw yet (a picture still loading).
struct Source {
    ComPtr<ID2D1Bitmap1> bitmap;
    int w = 0, h = 0;
    bool foreign = false;
};
ComPtr<ID2D1Bitmap1> bitmapFromBGRA(int w, int h, const uint8_t* px) {
    ComPtr<ID2D1Bitmap1> b;
    device().dc->CreateBitmap(D2D1::SizeU(w, h), px, w * 4,
                              D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
    return b;
}
std::vector<uint8_t> bgraFromRGBA(const std::vector<uint8_t>& rgba) {  // straight RGBA to premultiplied BGRA
    std::vector<uint8_t> out(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        uint8_t a = rgba[i + 3];
        out[i] = (uint8_t)((rgba[i + 2] * a + 127) / 255);
        out[i + 1] = (uint8_t)((rgba[i + 1] * a + 127) / 255);
        out[i + 2] = (uint8_t)((rgba[i] * a + 127) / 255);
        out[i + 3] = a;
    }
    return out;
}
Ctx2D* contextOf(Vm& vm, Node* canvas) {
    for (auto& c : vmc(vm).contexts)
        if (c->canvas == canvas) return c.get();
    return nullptr;
}
// The image argument of drawImage() and createPattern() (4.12.5.1.15, CanvasImageSource): false when it cannot be
// drawn (yet); errors for what the standard makes errors
bool sourceOf(CallInfo& ci, Value v, Source& out, Ctx2D* drawingOn) {
    Vm& vm = ci.vm;
    VmCanvases& vc = vmc(vm);
    Node* n = vc.host ? vc.host->nodeOf(v) : nullptr;
    if (!n || (n->name != "img" && n->name != "canvas" && n->name != "video")) {
        vm.throwError(Err::TypeError, "The provided value is not of type '(HTMLImageElement or HTMLCanvasElement or HTMLVideoElement or ImageBitmap)'.");
    }
    if (n->name == "video") return false;  // no frames to show
    if (n->name == "img") {
        const uint8_t* px = nullptr;
        int id = 0;
        int got = vc.host->imageBGRA(n, out.w, out.h, px, id);
        if (got == 0) return false;
        out.foreign = got == 2;
        auto& cached = vc.pictures[id];
        if (!cached) cached = bitmapFromBGRA(out.w, out.h, px);
        out.bitmap = cached;
        return out.bitmap != nullptr;
    }
    int w = canvas_dimension(n, false), h = canvas_dimension(n, true);
    if (w == 0 || h == 0) domError(ci, "InvalidStateError", "The image argument is a canvas element with a width or height of 0.");
    if (Ctx2D* other = contextOf(vm, n)) {
        if (!begin(*other)) return false;
        endDraw();  // its pending drawing is done before it is read
        out.w = other->w;
        out.h = other->h;
        out.foreign = !other->clean;
        if (other != drawingOn) {
            out.bitmap = other->bmp;
            return true;
        }
        out.bitmap = newBitmap(out.w, out.h, D2D1_BITMAP_OPTIONS_NONE);  // drawn on itself: a copy first
        if (out.bitmap) out.bitmap->CopyFromBitmap(nullptr, other->bmp.Get(), nullptr);
        return out.bitmap != nullptr;
    }
    std::vector<uint8_t> rgba;
    if (webgl_canvas_pixels(vm, n, out.w, out.h, rgba)) {
        auto bgra = bgraFromRGBA(rgba);
        out.bitmap = bitmapFromBGRA(out.w, out.h, bgra.data());
        return out.bitmap != nullptr;
    }
    out.w = w;  // a canvas with no context is transparent black
    out.h = h;
    std::vector<uint8_t> clear((size_t)w * h * 4, 0);
    out.bitmap = bitmapFromBGRA(w, h, clear.data());
    return out.bitmap != nullptr;
}
JSFN(createPattern) {
    Ctx2D& c = ctxOf(ci);
    Source src;
    if (!sourceOf(ci, ci.arg(0), src, nullptr)) return Value::null();
    std::string rep = ci.arg(1).isNull() ? "" : str8(ci.vm, ci.arg(1));
    if (rep.empty()) rep = "repeat";
    bool rx = rep == "repeat" || rep == "repeat-x", ry = rep == "repeat" || rep == "repeat-y";
    if (!rx && !ry && rep != "no-repeat") domError(ci, "SyntaxError", "The provided type is not one of 'repeat', 'repeat-x', 'repeat-y', or 'no-repeat'.");
    if (src.foreign) c.clean = false;
    auto* pt = ci.vm.alloc<PatternObj>();
    pt->proto = vmc(ci.vm).patternProto;
    pt->hostKind = HK_Pattern;
    pt->repX = rx;
    pt->repY = ry;
    // a copy of the picture as it is now, with a transparent pixel on each side that does not repeat
    int bw = src.w + (rx ? 0 : 2), bh = src.h + (ry ? 0 : 2);
    pt->bitmap = newBitmap(bw, bh, D2D1_BITMAP_OPTIONS_TARGET);
    if (pt->bitmap) {
        endDraw();
        auto* dc = device().dc.Get();
        dc->SetTarget(pt->bitmap.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0));
        dc->DrawBitmap(src.bitmap.Get(), D2D1::RectF(rx ? 0.f : 1.f, ry ? 0.f : 1.f, (rx ? 0.f : 1.f) + src.w, (ry ? 0.f : 1.f) + src.h));
        dc->EndDraw();
        dc->SetTarget(nullptr);
    }
    return Value::cell(pt);
}
JSFN(patternSetTransform) {
    auto* pt = hostOf<PatternObj>(ci.thisv, HK_Pattern);
    if (!pt) ci.vm.throwError(Err::TypeError, "Illegal invocation: not a CanvasPattern");
    Mat m;
    if (matrixInit(ci, ci.arg(0), m)) pt->m = m;
    return Value::undef();
}

// ---------------------------------------------------------------- images (4.12.5.1.15)
JSFN(drawImage) {
    Ctx2D& c = ctxOf(ci);
    if (ci.argc != 3 && ci.argc != 5 && ci.argc != 9) ci.vm.throwError(Err::TypeError, "Valid arities are: [3, 5, 9], but %d arguments provided.", ci.argc);
    double a[8];
    if (!finite(ci, 1, ci.argc - 1, a)) return Value::undef();
    Source src;
    if (!sourceOf(ci, ci.arg(0), src, &c)) return Value::undef();
    double sx = 0, sy = 0, sw = src.w, sh = src.h, dx, dy, dw, dh;
    if (ci.argc == 3) dx = a[0], dy = a[1], dw = sw, dh = sh;
    else if (ci.argc == 5) dx = a[0], dy = a[1], dw = a[2], dh = a[3];
    else sx = a[0], sy = a[1], sw = a[2], sh = a[3], dx = a[4], dy = a[5], dw = a[6], dh = a[7];
    if (sw < 0) sx += sw, sw = -sw;
    if (sh < 0) sy += sh, sh = -sh;
    if (dw < 0) dx += dw, dw = -dw;
    if (dh < 0) dy += dh, dh = -dh;
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return Value::undef();
    // a source rectangle partly outside the picture: both rectangles are cut down alike
    double fx = dw / sw, fy = dh / sh;
    if (sx < 0) dx -= sx * fx, dw += sx * fx, sw += sx, sx = 0;
    if (sy < 0) dy -= sy * fy, dh += sy * fy, sh += sy, sy = 0;
    if (sx + sw > src.w) dw -= (sx + sw - src.w) * fx, sw = src.w - sx;
    if (sy + sh > src.h) dh -= (sy + sh - src.h) * fy, sh = src.h - sy;
    if (sw <= 0 || sh <= 0) return Value::undef();
    if (src.foreign) c.clean = false;
    float opacity = c.st.alpha;
    D2D1_INTERPOLATION_MODE interp = !c.st.smoothing ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                     : c.st.quality == 2  ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                                                         : D2D1_INTERPOLATION_MODE_LINEAR;
    D2D1_RECT_F dst = D2D1::RectF((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh)), from = D2D1::RectF((float)sx, (float)sy, (float)(sx + sw), (float)(sy + sh));
    paint(c, [&](ID2D1DeviceContext* dc) { dc->DrawBitmap(src.bitmap.Get(), dst, opacity, interp, from); });
    return Value::undef();
}

// The canvas's bitmap copied where the processor can read it, and mapped: premultiplied BGRA rows from the top
bool mapCanvas(Ctx2D& c, D2D1_MAPPED_RECT& mapped) {
    if (!begin(c)) return false;
    endDraw();
    if (!c.readback) c.readback = newBitmap(c.w, c.h, D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
    return c.readback && SUCCEEDED(c.readback->CopyFromBitmap(nullptr, c.bmp.Get(), nullptr)) && SUCCEEDED(c.readback->Map(D2D1_MAP_OPTIONS_READ, &mapped));
}
// The canvas's pixels as straight RGBA, for the rectangle x y w h (parts outside the canvas are transparent black)
bool readPixels(Ctx2D& c, int x, int y, int w, int h, uint8_t* rgba) {
    D2D1_MAPPED_RECT mapped;
    if (!mapCanvas(c, mapped)) return false;
    memset(rgba, 0, (size_t)w * h * 4);
    for (int row = 0; row < h; ++row) {
        int cy = y + row;
        if (cy < 0 || cy >= c.h) continue;
        for (int col = 0; col < w; ++col) {
            int cx = x + col;
            if (cx < 0 || cx >= c.w) continue;
            const uint8_t* s = mapped.bits + (size_t)cy * mapped.pitch + (size_t)cx * 4;
            uint8_t* d = rgba + ((size_t)row * w + col) * 4;
            uint8_t a = c.alpha ? s[3] : 255;
            d[0] = a ? (uint8_t)std::min(255, (s[2] * 255 + a / 2) / a) : 0;
            d[1] = a ? (uint8_t)std::min(255, (s[1] * 255 + a / 2) / a) : 0;
            d[2] = a ? (uint8_t)std::min(255, (s[0] * 255 + a / 2) / a) : 0;
            d[3] = a;
        }
    }
    c.readback->Unmap();
    return true;
}
Value newImageData(CallInfo& ci, int w, int h, JSTypedArray*& data) {
    Vm& vm = ci.vm;
    if ((double)w * h * 4 > 1 << 30) vm.throwError(Err::RangeError, "Out of memory at ImageData creation");
    data = newTypedArray(vm, TA::U8C, (uint32_t)w * h * 4);
    Value ctor = vm.get(Value::cell(vm.global), "ImageData");
    Value args[] = {Value::cell(data), Value::i32(w), Value::i32(h)};
    return vm.construct(ctor, args, 3, ctor);
}
JSFN(createImageData) {
    ctxOf(ci);
    Vm& vm = ci.vm;
    int w, h;
    Value a0 = ci.arg(0);
    if (a0.isObject()) {
        w = vm.toInt32(vm.get(a0, "width"));
        h = vm.toInt32(vm.get(a0, "height"));
    } else {
        double a[2] = {num(ci, 0), num(ci, 1)};
        if (!std::isfinite(a[0]) || !std::isfinite(a[1])) vm.throwError(Err::TypeError, "The provided double value is non-finite.");
        w = std::abs((int)a[0]);
        h = std::abs((int)a[1]);
        if (!w || !h) domError(ci, "IndexSizeError", "The source width or height is 0.");
    }
    JSTypedArray* data;
    return newImageData(ci, w, h, data);
}
JSFN(getImageData) {
    Ctx2D& c = ctxOf(ci);
    Vm& vm = ci.vm;
    int x = vm.toInt32(ci.arg(0)), y = vm.toInt32(ci.arg(1)), w = vm.toInt32(ci.arg(2)), h = vm.toInt32(ci.arg(3));
    if (!w || !h) domError(ci, "IndexSizeError", "The source width or height is 0.");
    if (!c.clean) vmc(vm).host->securityError("The canvas has been tainted by cross-origin data.");
    if (w < 0) x += w, w = -w;
    if (h < 0) y += h, h = -h;
    JSTypedArray* data;
    Value out = newImageData(ci, w, h, data);
    readPixels(c, x, y, w, h, data->buffer->data() + data->offset);
    return out;
}
JSFN(putImageData) {
    Ctx2D& c = ctxOf(ci);
    Vm& vm = ci.vm;
    Value img = ci.arg(0);
    if (!img.isObject() || !vm.instanceOf(img, vm.get(Value::cell(vm.global), "ImageData")))
        vm.throwError(Err::TypeError, "parameter 1 is not of type 'ImageData'.");
    int iw = vm.toInt32(vm.get(img, "width")), ih = vm.toInt32(vm.get(img, "height"));
    Value dv = vm.get(img, "data");
    auto* data = dv.isObject() && dv.asObject()->cls == Cls::TypedArray ? (JSTypedArray*)dv.asObject() : nullptr;
    if (!data || typedOutOfBounds(data)) domError(ci, "InvalidStateError", "The source data has been detached.");
    int dx = vm.toInt32(ci.arg(1)), dy = vm.toInt32(ci.arg(2));
    int rx = 0, ry = 0, rw = iw, rh = ih;  // the dirty rectangle (4.12.5.1.16)
    if (ci.argc >= 7) {
        rx = vm.toInt32(ci.arg(3)), ry = vm.toInt32(ci.arg(4)), rw = vm.toInt32(ci.arg(5)), rh = vm.toInt32(ci.arg(6));
        if (rw < 0) rx += rw, rw = -rw;
        if (rh < 0) ry += rh, rh = -rh;
    }
    if (rx < 0) rw += rx, rx = 0;
    if (ry < 0) rh += ry, ry = 0;
    rw = std::min(rw, iw - rx);
    rh = std::min(rh, ih - ry);
    // and only where it lands on the canvas
    if (!begin(c)) return Value::undef();
    int x0 = std::max(rx, -dx), y0 = std::max(ry, -dy), x1 = std::min(rx + rw, c.w - dx), y1 = std::min(ry + rh, c.h - dy);
    if (x1 <= x0 || y1 <= y0) return Value::undef();
    int w = x1 - x0, h = y1 - y0;
    const uint8_t* src = data->buffer->data() + data->offset;
    std::vector<uint8_t> bgra((size_t)w * h * 4);
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col) {
            const uint8_t* s = src + ((size_t)(y0 + row) * iw + (x0 + col)) * 4;
            uint8_t* d = &bgra[((size_t)row * w + col) * 4];
            uint8_t a = s[3];
            d[0] = (uint8_t)((s[2] * a + 127) / 255);
            d[1] = (uint8_t)((s[1] * a + 127) / 255);
            d[2] = (uint8_t)((s[0] * a + 127) / 255);
            d[3] = a;
        }
    endDraw();
    D2D1_RECT_U r{(UINT32)(dx + x0), (UINT32)(dy + y0), (UINT32)(dx + x1), (UINT32)(dy + y1)};
    c.bmp->CopyFromMemory(&r, bgra.data(), w * 4);
    c.dirty = true;
    return Value::undef();
}

// ---------------------------------------------------------------- text (4.12.5.1.4, 4.12.5.1.12)
// What a text method draws: the layout of the text in the context's font with its whitespace as spaces, where its
// origin goes so that x y is at the alignment point and the baseline asked for, and its metrics
struct TextRun {
    ComPtr<IDWriteTextLayout> layout;
    float width = 0, baseline = 0, asc = 0, desc = 0, anchor = 0, shift = 0;  // shift: the chosen baseline above the alphabetic one
};
bool textRun(Ctx2D& c, Vm& vm, Value text, TextRun& run) {
    if (!c.st.fontStyle) setFont(c, c.st.font);
    if (!c.st.fontStyle || !TextSystem::inst) return false;
    std::u16string t = vm.toStr16(text);
    for (auto& ch : t)
        if (ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r') ch = ' ';
    std::wstring w(t.begin(), t.end());
    run.layout = TextSystem::inst->layout(*c.st.fontStyle, w, false, false);
    if (!run.layout) return false;
    run.layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m;
    run.layout->GetMetrics(&m);
    run.width = m.widthIncludingTrailingWhitespace;
    DWRITE_LINE_METRICS line;
    UINT32 lines = 0;
    run.layout->GetLineMetrics(&line, 1, &lines);
    run.baseline = lines ? line.baseline : 0;
    TextSystem::inst->metrics(*c.st.fontStyle, run.asc, run.desc);
    bool rtl = c.st.direction == 2;  // "inherit" would take the canvas element's direction, which styles do not keep yet
    switch (c.st.align) {
        case 0: run.anchor = rtl ? run.width : 0; break;  // start
        case 1: run.anchor = rtl ? 0 : run.width; break;  // end
        case 2: run.anchor = 0; break;
        case 3: run.anchor = run.width; break;
        case 4: run.anchor = run.width / 2; break;
    }
    float em = c.st.fontStyle->fontSize;
    switch (c.st.baseline) {
        case 0: run.shift = 0; break;
        case 1: run.shift = run.asc; break;                     // top: the em box's top, taken as the ascent
        case 2: run.shift = 0.8f * em; break;                   // hanging
        case 3: run.shift = (run.asc - run.desc) / 2; break;    // middle of the em box
        case 4: case 5: run.shift = -run.desc; break;           // ideographic, bottom
    }
    return true;
}
void drawText(CallInfo& ci, bool fillIt) {
    Ctx2D& c = ctxOf(ci);
    double a[2];
    if (!finite(ci, 1, 2, a)) return;
    double maxWidth = ci.arg(3).isUndef() ? INFINITY : num(ci, 3);  // an optional argument: undefined is none
    if (std::isnan(maxWidth) || maxWidth <= 0) return;
    TextRun run;
    if (!textRun(c, ci.vm, ci.arg(0), run) || run.width == 0) return;
    // the layout's top left corner, with the text squeezed to maxWidth around its alignment point
    float sx = run.width > maxWidth ? (float)(maxWidth / run.width) : 1.f;
    Mat place = Mat::Translation(-run.anchor, -run.baseline + run.shift) * Mat::Scale(sx, 1) * Mat::Translation((float)a[0], (float)a[1]);
    auto br = brushOf(c, fillIt ? c.st.fill : c.st.stroke);
    if (!br) return;
    if (fillIt) {
        paint(c, [&](ID2D1DeviceContext* dc) {
            Mat m;
            dc->GetTransform(&m);
            dc->SetTransform(place * m);
            // the brush is in user space: undo the placement for it
            Mat undo = place;
            if (undo.Invert()) br->SetTransform(undo);
            dc->DrawTextLayout(D2D1::Point2F(0, 0), run.layout.Get(), br.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            dc->SetTransform(m);
        });
        return;
    }
    // strokeText: the glyph outlines, stroked
    struct Outline : IDWriteTextRenderer {
        std::vector<ComPtr<ID2D1Geometry>> parts;
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
            if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteTextRenderer) || id == __uuidof(IDWritePixelSnapping)) {
                *out = this;
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* no) override { return *no = TRUE, S_OK; }
        HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override { return *m = DWRITE_MATRIX{1, 0, 0, 1, 0, 0}, S_OK; }
        HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override { return *p = 1, S_OK; }
        HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                               const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
            ComPtr<ID2D1PathGeometry> g;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(device().factory->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) return S_OK;
            run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets, run->glyphCount,
                                              run->isSideways, run->bidiLevel % 2, sink.Get());
            sink->Close();
            ComPtr<ID2D1TransformedGeometry> moved;
            device().factory->CreateTransformedGeometry(g.Get(), Mat::Translation(x, y), &moved);
            if (moved) parts.push_back(moved);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
    } outline;
    run.layout->Draw(nullptr, &outline, 0, 0);
    if (outline.parts.empty()) return;
    std::vector<ID2D1Geometry*> raw;
    for (auto& p : outline.parts) raw.push_back(p.Get());
    ComPtr<ID2D1GeometryGroup> group;
    device().factory->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(), (UINT32)raw.size(), &group);
    ComPtr<ID2D1TransformedGeometry> placed;
    if (group) device().factory->CreateTransformedGeometry(group.Get(), place, &placed);
    auto ss = strokeStyleOf(c.st);
    float width = c.st.lineWidth;
    if (placed) paint(c, [&](ID2D1DeviceContext* dc) { dc->DrawGeometry(placed.Get(), br.Get(), width, ss.Get()); });
}
JSFN(fillText) {
    drawText(ci, true);
    return Value::undef();
}
JSFN(strokeText) {
    drawText(ci, false);
    return Value::undef();
}
JSFN(measureText) {
    Ctx2D& c = ctxOf(ci);
    Vm& vm = ci.vm;
    TextRun run;
    JSObject* o = vm.newObject(vmc(vm).metricsProto);
    bool ok = textRun(c, vm, ci.arg(0), run);
    DWRITE_OVERHANG_METRICS over{};
    if (ok) run.layout->GetOverhangMetrics(&over);
    float lineH = run.asc + run.desc;
    float inkTop = -over.top, inkBottom = lineH + over.bottom;  // from the layout's top
    auto put = [&](const char* name, double v) { vm.def(o, name, Value::num(ok ? v : 0), PF_E); };
    put("width", run.width);
    put("actualBoundingBoxLeft", run.anchor + over.left);
    put("actualBoundingBoxRight", run.width + over.right - run.anchor);
    put("actualBoundingBoxAscent", run.baseline - inkTop - run.shift);
    put("actualBoundingBoxDescent", inkBottom - run.baseline + run.shift);
    put("fontBoundingBoxAscent", run.asc - run.shift);
    put("fontBoundingBoxDescent", run.desc + run.shift);
    put("emHeightAscent", run.asc - run.shift);
    put("emHeightDescent", run.desc + run.shift);
    put("hangingBaseline", 0.8 * (c.st.fontStyle ? c.st.fontStyle->fontSize : 10) - run.shift);
    put("alphabeticBaseline", -run.shift);
    put("ideographicBaseline", -run.desc - run.shift);
    return Value::cell(o);
}

// ---------------------------------------------------------------- the rest of the context
JSFN(setLineDash) {
    Ctx2D& c = ctxOf(ci);
    Vm& vm = ci.vm;
    Value v = ci.arg(0);
    if (!v.isObject()) vm.throwError(Err::TypeError, "The provided value cannot be converted to a sequence.");
    std::vector<float> dash;
    int64_t n = (int64_t)vm.toNumber(vm.get(v, "length"));
    for (int64_t i = 0; i < n && i < 1 << 16; ++i) {
        double d = vm.toNumber(vm.get(v, PropKey::index((uint32_t)i)));
        if (!std::isfinite(d) || d < 0) return Value::undef();  // the whole list is ignored
        dash.push_back((float)d);
    }
    if (dash.size() % 2) dash.insert(dash.end(), dash.begin(), dash.end());  // an odd list counts twice
    bool all0 = std::all_of(dash.begin(), dash.end(), [](float d) { return d == 0; });
    c.st.dash = all0 ? std::vector<float>{} : dash;
    if (all0 && !dash.empty()) c.st.dash.clear();
    return Value::undef();
}
JSFN(getLineDash) {
    Ctx2D& c = ctxOf(ci);
    JSArray* a = ci.vm.newArray();
    for (float d : c.st.dash) a->elems.push_back(Value::num(d));
    return Value::cell(a);
}
JSFN(canvasGet) {
    Ctx2D& c = ctxOf(ci);
    VmCanvases& v = vmc(ci.vm);
    return v.host ? v.host->wrapNode(c.canvas) : Value::null();
}
JSFN(getContextAttributes) {
    Ctx2D& c = ctxOf(ci);
    Vm& vm = ci.vm;
    JSObject* o = vm.newObject();
    vm.def(o, "alpha", Value::boolean(c.alpha), PF_ALL);
    vm.def(o, "colorSpace", str(vm, "srgb"), PF_ALL);
    vm.def(o, "desynchronized", Value::boolean(false), PF_ALL);
    vm.def(o, "willReadFrequently", Value::boolean(c.willRead), PF_ALL);
    return Value::cell(o);
}
JSFN(isContextLost) {
    ctxOf(ci);
    return Value::boolean(false);
}
JSFN(nothing) { return Value::undef(); }

// ---------------------------------------------------------------- Path2D
JSFN(path2dCtor) {
    Vm& vm = ci.vm;
    if (!ci.isConstruct()) vm.throwError(Err::TypeError, "Failed to construct 'Path2D': Please use the 'new' operator.");
    auto* p = vm.alloc<PathObj>();
    Value proto = ci.newTarget.isObject() ? vm.get(ci.newTarget, "prototype") : Value::undef();
    p->proto = proto.isObject() ? proto.asObject() : vmc(vm).pathProto;
    p->hostKind = HK_Path;
    Value a = ci.arg(0);
    if (auto* other = hostOf<PathObj>(a, HK_Path)) {
        p->path = other->path;
    } else if (!a.isUndef()) {  // SVG path data (SVG 2, 9.3)
        SvgShape shape;
        svg_path_data(str8(vm, a), shape);
        Path& path = p->path;
        size_t k = 0;
        float x = 0, y = 0;
        for (auto op : shape.ops) {
            const float* v = &shape.args[k];
            switch (op) {
                case SvgShape::Move: path.move(v[0], v[1]), x = v[0], y = v[1], k += 2; break;
                case SvgShape::Line: path.line(v[0], v[1]), x = v[0], y = v[1], k += 2; break;
                case SvgShape::Cubic: path.cubic(v[0], v[1], v[2], v[3], v[4], v[5]), x = v[4], y = v[5], k += 6; break;
                case SvgShape::Quad:
                    path.cubic(x + 2.f / 3 * (v[0] - x), y + 2.f / 3 * (v[1] - y), v[2] + 2.f / 3 * (v[0] - v[2]), v[3] + 2.f / 3 * (v[1] - v[3]), v[2], v[3]);
                    x = v[2], y = v[3], k += 4;
                    break;
                case SvgShape::Arc: {  // the endpoint form (SVG 2, B.2.4) as the centre form
                    double rx = std::abs(v[0]), ry = std::abs(v[1]), phi = v[2] * kPi / 180, x2 = v[5], y2 = v[6];
                    bool large = v[3] != 0, sweep = v[4] != 0;
                    k += 7;
                    if ((x == x2 && y == y2)) break;
                    if (rx == 0 || ry == 0) {
                        path.line((float)x2, (float)y2);
                        x = (float)x2, y = (float)y2;
                        break;
                    }
                    double cp = std::cos(phi), sp = std::sin(phi), dx = (x - x2) / 2, dy = (y - y2) / 2;
                    double x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
                    double lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
                    if (lambda > 1) rx *= std::sqrt(lambda), ry *= std::sqrt(lambda);
                    double num2 = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p, den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
                    double co = std::sqrt(std::max(0.0, num2 / den)) * (large == sweep ? -1 : 1);
                    double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
                    double cx = cp * cxp - sp * cyp + (x + x2) / 2, cy = sp * cxp + cp * cyp + (y + y2) / 2;
                    auto angle = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
                    double t1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
                    double dt = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
                    if (!sweep && dt > 0) dt -= 2 * kPi;
                    else if (sweep && dt < 0) dt += 2 * kPi;
                    addArc(path, Mat::Identity(), cx, cy, rx, ry, phi, t1, t1 + dt, dt < 0);
                    x = (float)x2, y = (float)y2;
                    break;
                }
                case SvgShape::Close: path.close(), x = path.lx, y = path.ly; break;
            }
        }
    }
    return Value::cell(p);
}
JSFN(addPath) {
    auto* p = hostOf<PathObj>(ci.thisv, HK_Path);
    auto* other = hostOf<PathObj>(ci.arg(0), HK_Path);
    if (!p) ci.vm.throwError(Err::TypeError, "Illegal invocation: not a Path2D");
    if (!other) ci.vm.throwError(Err::TypeError, "parameter 1 is not of type 'Path2D'.");
    Mat m;
    if (!matrixInit(ci, ci.arg(1), m)) return Value::undef();
    std::vector<Seg> segs = other->path.segs;
    for (const Seg& s : segs) {
        D2D1_POINT_2F q[3];
        int n = s.k == Seg::Cubic ? 3 : s.k == Seg::Close ? 0 : 1;
        for (int i = 0; i < n; ++i) q[i] = m.TransformPoint({s.p[2 * i], s.p[2 * i + 1]});
        if (s.k == Seg::Move) p->path.move(q[0].x, q[0].y);
        else if (s.k == Seg::Line) p->path.line(q[0].x, q[0].y);
        else if (s.k == Seg::Cubic) p->path.cubic(q[0].x, q[0].y, q[1].x, q[1].y, q[2].x, q[2].y);
        else p->path.close();
    }
    return Value::undef();
}

// ---------------------------------------------------------------- the canvas element's side
// __canvas2d(canvas, attributes): its 2D context, made the first time (4.12.5.1, getContext)
JSFN(canvas2dContext) {
    Vm& vm = ci.vm;
    VmCanvases& v = vmc(vm);
    Node* canvas = v.host ? v.host->nodeOf(ci.arg(0)) : nullptr;
    if (!canvas || !device().dc) return Value::null();
    if (Ctx2D* c = contextOf(vm, canvas)) return Value::cell(c->self);
    auto c = std::make_unique<Ctx2D>();
    c->canvas = canvas;
    Value attrs = ci.arg(1);
    if (attrs.isObject()) {
        Value a = vm.get(attrs, "alpha");
        if (!a.isUndef()) c->alpha = vm.toBool(a);
        c->willRead = vm.toBool(vm.get(attrs, "willReadFrequently"));
    }
    JSHost* h = vm.alloc<JSHost>();
    h->proto = v.ctxProto;
    h->hostKind = HK_2D;
    h->ptr = c.get();
    c->self = h;
    reset(*c);
    v.contexts.push_back(std::move(c));
    return Value::cell(h);
}
// __canvasReset(canvas): its width or height was set (4.12.5): the bitmap and the context start afresh
JSFN(canvasReset) {
    VmCanvases& v = vmc(ci.vm);
    Node* canvas = v.host ? v.host->nodeOf(ci.arg(0)) : nullptr;
    if (Ctx2D* c = canvas ? contextOf(ci.vm, canvas) : nullptr) reset(*c);
    return Value::undef();
}
// __canvasDataURL(canvas, type, quality) (4.12.5, toDataURL): the canvas as a PNG or JPEG data: URL
JSFN(canvasDataURL) {
    Vm& vm = ci.vm;
    VmCanvases& v = vmc(vm);
    Node* canvas = v.host ? v.host->nodeOf(ci.arg(0)) : nullptr;
    if (!canvas) vm.throwError(Err::TypeError, "Illegal invocation");
    int w = canvas_dimension(canvas, false), h = canvas_dimension(canvas, true);
    std::vector<uint8_t> rgba;
    if (Ctx2D* c = contextOf(vm, canvas)) {
        if (!c->clean) v.host->securityError("Tainted canvases may not be exported.");
        if (begin(*c)) {
            rgba.resize((size_t)c->w * c->h * 4);
            readPixels(*c, 0, 0, c->w, c->h, rgba.data());
        }
    } else if (!webgl_canvas_pixels(vm, canvas, w, h, rgba))
        rgba.assign((size_t)w * h * 4, 0);
    if (w == 0 || h == 0 || rgba.size() != (size_t)w * h * 4) return str(vm, "data:,");
    std::string type = ci.arg(1).isUndef() ? "image/png" : lower(str8(vm, ci.arg(1)));
    bool jpeg = type == "image/jpeg";
    double quality = ci.arg(2).isNumber() ? vm.toNumber(ci.arg(2)) : 0.92;
    if (!(quality >= 0 && quality <= 1)) quality = 0.92;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, &props)))
        return str(vm, "data:,");
    if (jpeg && props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT val;
        VariantInit(&val);
        val.vt = VT_R4;
        val.fltVal = (float)quality;
        props->Write(1, &opt, &val);
    }
    frame->Initialize(props.Get());
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    std::vector<uint8_t> px;
    if (jpeg) {  // no alpha in a JPEG: what is transparent becomes black
        px.resize((size_t)w * h * 3);
        for (size_t i = 0, j = 0; i < rgba.size(); i += 4, j += 3) {
            px[j] = (uint8_t)(rgba[i + 2] * rgba[i + 3] / 255);
            px[j + 1] = (uint8_t)(rgba[i + 1] * rgba[i + 3] / 255);
            px[j + 2] = (uint8_t)(rgba[i] * rgba[i + 3] / 255);
        }
    } else {
        px = rgba;
        for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    }
    UINT stride = (UINT)w * (jpeg ? 3 : 4);
    frame->WritePixels(h, stride, stride * h, px.data());
    frame->Commit();
    enc->Commit();
    HGLOBAL mem = nullptr;
    GetHGlobalFromStream(stream.Get(), &mem);
    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    std::string bytes((size_t)stat.cbSize.QuadPart, '\0');
    if (void* p = GlobalLock(mem)) {
        memcpy(bytes.data(), p, bytes.size());
        GlobalUnlock(mem);
    }
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out = std::string("data:") + (jpeg ? "image/jpeg" : "image/png") + ";base64,";
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t n = (uint8_t)bytes[i] << 16 | (i + 1 < bytes.size() ? (uint8_t)bytes[i + 1] << 8 : 0) | (i + 2 < bytes.size() ? (uint8_t)bytes[i + 2] : 0);
        out += b64[n >> 18 & 63];
        out += b64[n >> 12 & 63];
        out += i + 1 < bytes.size() ? b64[n >> 6 & 63] : '=';
        out += i + 2 < bytes.size() ? b64[n & 63] : '=';
    }
    return str(vm, out);
}

}  // namespace

void canvas2d_install(Vm& vm, JSObject* G, CanvasHost& host) {
    VmCanvases& v = vmc(vm);
    v.host = &host;
    vm.rootTracers.push_back([&vm](Gc& g) {
        auto it = registry().find(&vm);
        if (it == registry().end()) return;
        VmCanvases& v = it->second;
        for (JSObject* p : {v.ctxProto, v.gradientProto, v.patternProto, v.pathProto, v.metricsProto})
            if (p) g.mark(p);
        for (auto& c : v.contexts) {
            g.mark(c->self);
            g.mark(c->st.fill.object);
            g.mark(c->st.stroke.object);
            for (auto& s : c->saved) {
                g.mark(s.fill.object);
                g.mark(s.stroke.object);
            }
        }
    });
    auto iface = [&](const char* name, NativeFn ctor = nullptr) {
        JSObject* proto = vm.newObject();
        makeCtor(vm, name, ctor ? ctor : [](CallInfo& ci) -> Value { ci.vm.throwError(Err::TypeError, "Illegal constructor"); }, 0, proto);
        return proto;
    };
    v.ctxProto = iface("CanvasRenderingContext2D");
    v.gradientProto = iface("CanvasGradient");
    v.patternProto = iface("CanvasPattern");
    v.pathProto = iface("Path2D", path2dCtor);
    v.metricsProto = iface("TextMetrics");
    vm.defFn(v.gradientProto, "addColorStop", addColorStop, 2);
    vm.defFn(v.patternProto, "setTransform", patternSetTransform, 0);
    struct M {
        const char* name;
        NativeFn fn;
        int len;
    };
    static const M pathMethods[] = {{"closePath", closePath, 0}, {"moveTo", moveTo, 2}, {"lineTo", lineTo, 2},
                                    {"quadraticCurveTo", quadraticCurveTo, 4}, {"bezierCurveTo", bezierCurveTo, 6}, {"arcTo", arcTo, 5},
                                    {"rect", rect, 4}, {"roundRect", roundRect, 4}, {"arc", arc, 5}, {"ellipse", ellipse, 7}};
    for (const M& m : pathMethods) {
        vm.defFn(v.ctxProto, m.name, m.fn, m.len);
        vm.defFn(v.pathProto, m.name, m.fn, m.len);
    }
    vm.defFn(v.pathProto, "addPath", addPath, 1);
    static const M methods[] = {
        {"save", save, 0}, {"restore", restore, 0}, {"reset", resetFn, 0}, {"scale", scale, 2}, {"rotate", rotate, 1},
        {"translate", translate, 2}, {"transform", transform, 6}, {"setTransform", setTransform, 0}, {"getTransform", getTransform, 0},
        {"resetTransform", resetTransform, 0}, {"createLinearGradient", createLinearGradient, 4},
        {"createRadialGradient", createRadialGradient, 6}, {"createConicGradient", createConicGradient, 3},
        {"createPattern", createPattern, 2}, {"clearRect", clearRect, 4}, {"fillRect", fillRect, 4}, {"strokeRect", strokeRect, 4},
        {"beginPath", beginPath, 0}, {"fill", fill, 0}, {"stroke", stroke, 0}, {"clip", clip, 0}, {"isPointInPath", isPointInPath, 2},
        {"isPointInStroke", isPointInStroke, 2}, {"fillText", fillText, 3}, {"strokeText", strokeText, 3}, {"measureText", measureText, 1},
        {"drawImage", drawImage, 3}, {"createImageData", createImageData, 1}, {"getImageData", getImageData, 4},
        {"putImageData", putImageData, 3}, {"setLineDash", setLineDash, 1}, {"getLineDash", getLineDash, 0},
        {"getContextAttributes", getContextAttributes, 0}, {"isContextLost", isContextLost, 0}, {"drawFocusIfNeeded", nothing, 1},
        {"scrollPathIntoView", nothing, 0}};
    for (const M& m : methods) vm.defFn(v.ctxProto, m.name, m.fn, m.len);
    static const char* const props[] = {"globalAlpha", "globalCompositeOperation", "fillStyle", "strokeStyle", "lineWidth", "lineCap",
                                        "lineJoin", "miterLimit", "lineDashOffset", "shadowOffsetX", "shadowOffsetY", "shadowBlur",
                                        "shadowColor", "font", "textAlign", "textBaseline", "direction", "imageSmoothingEnabled",
                                        "imageSmoothingQuality", "filter", "letterSpacing", "wordSpacing", "fontKerning", "textRendering",
                                        "fontStretch", "fontVariantCaps"};
    for (int i = 0; i < (int)ARRAYSIZE(props); ++i) {
        JSNative* get = vm.newNative(propGet, props[i], 0);
        JSNative* set = vm.newNative(propSet, props[i], 1);
        get->data = set->data = Value::i32(i);
        vm.defineAccessor(v.ctxProto, PropKey::of(vm.atom(props[i])), Value::cell(get), Value::cell(set), PF_C | PF_E);
    }
    vm.defGetter(v.ctxProto, "canvas", canvasGet);
    vm.defFn(G, "__canvas2d", canvas2dContext, 2);
    vm.defFn(G, "__canvasReset", canvasReset, 1);
    vm.defFn(G, "__canvasDataURL", canvasDataURL, 3);
}

void canvas2d_present(Vm& vm) {
    auto it = registry().find(&vm);
    if (it == registry().end() || !it->second.host) return;
    for (auto& c : it->second.contexts) {
        if (!c->dirty) continue;
        c->dirty = false;
        if (!c->bmp) continue;  // a size of 0: nothing to show
        D2D1_MAPPED_RECT mapped;
        if (!mapCanvas(*c, mapped)) continue;
        std::vector<uint8_t> bgra((size_t)c->w * c->h * 4);
        size_t row = (size_t)c->w * 4;
        for (int y = 0; y < c->h; ++y) memcpy(bgra.data() + y * row, mapped.bits + (size_t)y * mapped.pitch, row);
        c->readback->Unmap();
        if (!c->alpha)
            for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
        it->second.host->canvasPresented(c->canvas, c->w, c->h, std::move(bgra));
    }
}

bool canvas2d_pixels(Vm& vm, Node* canvas, int& w, int& h, std::vector<uint8_t>& rgba, bool& clean) {
    Ctx2D* c = contextOf(vm, canvas);
    if (!c) return false;
    clean = c->clean;
    if (!begin(*c)) {
        w = h = 0;
        rgba.clear();
        return true;
    }
    w = c->w;
    h = c->h;
    rgba.resize((size_t)w * h * 4);
    return readPixels(*c, 0, 0, w, h, rgba.data());
}

void canvas2d_release(Vm& vm) {
    auto it = registry().find(&vm);
    if (it == registry().end()) return;
    for (auto& c : it->second.contexts)
        if (g_open == c.get()) endDraw();
    registry().erase(it);
}

}  // namespace tb
