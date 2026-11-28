// GPU painter: walks the display list and issues Direct2D draw calls.
#include <cmath>

#include "layout.h"

namespace tb {

static inline D2D1_COLOR_F cf(const Color& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }

// ---------------------------------------------------------------- background layers (CSS Backgrounds 3, 3.3-3.9)
namespace {

struct RectF4 {
    float x, y, w, h;
};
RectF4 inset(RectF4 r, const float* e) { return {r.x + e[3], r.y + e[0], std::max(0.f, r.w - e[1] - e[3]), std::max(0.f, r.h - e[0] - e[2])}; }
RectF4 boxOf(RectF4 border, const BgPaint& p, int which) {  // 0 border-box, 1 padding-box, 2 content-box
    if (which == 0) return border;
    RectF4 pad = inset(border, p.b);
    return which == 1 ? pad : inset(pad, p.p);
}
template <class T>
const T& cyclic(const std::vector<T>& v, size_t i, const T& fallback) { return v.empty() ? fallback : v[i % v.size()]; }

// The gradient stops of a gradient line of the given length, as positions in pixels (CSS Images 3, 3.5.3)
std::vector<std::pair<float, Color>> resolveStops(const std::vector<GradientStop>& stops, float length) {
    std::vector<std::pair<float, Color>> out;
    size_t n = stops.size();
    std::vector<float> pos(n, NAN);
    for (size_t i = 0; i < n; ++i)
        if (stops[i].pos.u == Len::Px || stops[i].pos.u == Len::Pct) pos[i] = stops[i].pos.resolve(length);
    if (n && std::isnan(pos[0])) pos[0] = 0;
    if (n > 1 && std::isnan(pos[n - 1])) pos[n - 1] = length;
    float most = n ? pos[0] : 0;
    for (size_t i = 0; i < n; ++i)
        if (!std::isnan(pos[i])) {
            if (pos[i] < most) pos[i] = most;  // a stop before an earlier one moves up to it
            most = pos[i];
        }
    for (size_t i = 0; i < n;) {  // the stops without a position share the space between their neighbours evenly
        if (!std::isnan(pos[i])) { ++i; continue; }
        size_t j = i;
        while (j < n && std::isnan(pos[j])) ++j;
        float a = pos[i - 1], b = pos[j];
        for (size_t k = i; k < j; ++k) pos[k] = a + (b - a) * (float)(k - i + 1) / (float)(j - i + 1);
        i = j;
    }
    for (size_t i = 0; i < n; ++i) out.push_back({pos[i], stops[i].c});
    return out;
}
Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stopCollection(ID2D1RenderTarget* rt, const std::vector<std::pair<float, Color>>& stops,
                                                                 float from, float to, bool repeating, float alpha) {
    std::vector<D2D1_GRADIENT_STOP> s;
    float span = to - from;
    for (auto& [p, c] : stops) s.push_back({span > 0 ? (p - from) / span : 0.f, D2D1::ColorF(c.r, c.g, c.b, c.a * alpha)});
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> coll;
    rt->CreateGradientStopCollection(s.data(), (UINT32)s.size(), D2D1_GAMMA_2_2, repeating ? D2D1_EXTEND_MODE_WRAP : D2D1_EXTEND_MODE_CLAMP, &coll);
    return coll;
}
// A conic gradient's tile of w by h (CSS Images 4, 3.3) as a bitmap: each pixel takes the colour at its angle around
// the centre, clockwise from the start angle, the stops mixed with premultiplied alpha
Microsoft::WRL::ComPtr<ID2D1Bitmap> conicBitmap(ID2D1RenderTarget* rt, const BgImage& g, float w, float h, int pw, int ph) {
    auto stops = resolveStops(g.stops, 360);
    float first = stops.front().first, last = stops.back().first, period = last - first;
    float cx = g.cx.resolve(w), cy = g.cy.resolve(h);
    std::vector<uint8_t> px((size_t)pw * ph * 4);
    const double kPi = 3.14159265358979323846;
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
            double dx = (x + 0.5) * w / pw - cx, dy = (y + 0.5) * h / ph - cy;
            float a = (float)std::fmod(std::atan2(dx, -dy) * 180 / kPi - g.angle + 720.0, 360.0);
            if (g.repeating && period > 1e-3f) a = first + std::fmod(std::fmod(a - first, period) + period, period);
            Color c0 = stops.front().second, c1 = c0;
            float t = 0;
            if (a >= last) c0 = c1 = stops.back().second;
            else if (a > first)
                for (size_t i = 1; i < stops.size(); ++i)
                    if (a <= stops[i].first) {
                        c0 = stops[i - 1].second;
                        c1 = stops[i].second;
                        float span = stops[i].first - stops[i - 1].first;
                        t = span > 0 ? (a - stops[i - 1].first) / span : 1;
                        break;
                    }
            float al = c0.a + (c1.a - c0.a) * t;
            auto channel = [&](float v0, float v1) { return (uint8_t)std::lround(std::clamp(v0 * c0.a + (v1 * c1.a - v0 * c0.a) * t, 0.f, 1.f) * 255); };
            uint8_t* p = &px[((size_t)y * pw + x) * 4];
            p[0] = channel(c0.b, c1.b);
            p[1] = channel(c0.g, c1.g);
            p[2] = channel(c0.r, c1.r);
            p[3] = (uint8_t)std::lround(std::clamp(al, 0.f, 1.f) * 255);
        }
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    rt->CreateBitmap(D2D1::SizeU(pw, ph), px.data(), pw * 4, bp, &bmp);
    return bmp;
}

// A brush that paints the gradient of one tile whose top left corner is at the origin; `base` is the transform the
// brush needs before it is moved to a tile
Microsoft::WRL::ComPtr<ID2D1Brush> gradientBrush(ID2D1RenderTarget* rt, const BgImage& g, float w, float h, float alpha, Color& solid,
                                                 const BgPaint& paint, uint64_t gen, D2D1::Matrix3x2F& base) {
    Microsoft::WRL::ComPtr<ID2D1Brush> brush;
    base = D2D1::Matrix3x2F::Identity();
    if (g.stops.empty()) return brush;
    if (g.stops.size() == 1) {
        solid = g.stops[0].c;
        return brush;
    }
    if (g.k == BgImage::Conic) {
        float dpiX, dpiY;
        rt->GetDpi(&dpiX, &dpiY);
        int pw = std::clamp((int)std::ceil(w * dpiX / 96), 1, 1024), ph = std::clamp((int)std::ceil(h * dpiY / 96), 1, 1024);
        if (!paint.conic || paint.conicGen != gen || paint.conicW != w || paint.conicH != h) {
            paint.conic = conicBitmap(rt, g, w, h, pw, ph);
            paint.conicW = w;
            paint.conicH = h;
            paint.conicGen = gen;
        }
        if (!paint.conic) return brush;
        Microsoft::WRL::ComPtr<ID2D1BitmapBrush> bb;
        rt->CreateBitmapBrush(paint.conic.Get(),
                              D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                              &bb);
        if (!bb) return brush;
        bb->SetOpacity(alpha);
        base = D2D1::Matrix3x2F::Scale(w / pw, h / ph);
        brush = bb;
        return brush;
    }
    const double kPi = 3.14159265358979323846;
    if (g.k == BgImage::Linear) {
        double deg = g.angle, corner = std::atan2(h, w) * 180 / kPi;
        if (g.toCorner == (1 | 2)) deg = corner;
        else if (g.toCorner == (4 | 2)) deg = 180 - corner;
        else if (g.toCorner == (4 | 8)) deg = 180 + corner;
        else if (g.toCorner == (1 | 8)) deg = 360 - corner;
        double t = deg * kPi / 180, sx = std::sin(t), sy = -std::cos(t);
        float length = (float)(std::fabs(w * std::sin(t)) + std::fabs(h * std::cos(t)));
        auto stops = resolveStops(g.stops, length);
        // the line the brush spans: the whole gradient line, or past it to the outer stops; a repeating gradient
        // repeats the span from its first stop to its last
        float from = g.repeating ? stops.front().first : std::min(0.f, stops.front().first);
        float to = g.repeating ? stops.back().first : std::max(length, stops.back().first);
        if (to - from < 1e-3f) {  // a repeating gradient of no length: its average colour, here its last
            solid = stops.back().second;
            return brush;
        }
        float cx = w / 2, cy = h / 2;
        D2D1_POINT_2F start = D2D1::Point2F((float)(cx - sx * length / 2 + sx * from), (float)(cy - sy * length / 2 + sy * from));
        D2D1_POINT_2F end = D2D1::Point2F((float)(cx - sx * length / 2 + sx * to), (float)(cy - sy * length / 2 + sy * to));
        auto coll = stopCollection(rt, stops, from, to, g.repeating, alpha);
        if (!coll) return brush;
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> lb;
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(start, end), coll.Get(), &lb);
        brush = lb;
        return brush;
    }
    // radial (CSS Images 3, 3.2): the ending shape from the extent keyword
    float cx = g.cx.resolve(w), cy = g.cy.resolve(h), rx, ry;
    float dl = std::fabs(cx), dr = std::fabs(w - cx), dt = std::fabs(cy), db = std::fabs(h - cy);
    float sideX = 0, sideY = 0;
    switch (g.extent) {
        case 1: sideX = std::min(dl, dr); sideY = std::min(dt, db); break;  // closest-side
        case 3: sideX = std::max(dl, dr); sideY = std::max(dt, db); break;  // farthest-side
        case 2: sideX = std::min(dl, dr); sideY = std::min(dt, db); break;  // closest-corner
        default: sideX = std::max(dl, dr); sideY = std::max(dt, db); break; // farthest-corner
    }
    if (g.extent == 4) {
        rx = g.rx.resolve(w);
        ry = g.circle ? rx : g.ry.resolve(h);
    } else if (g.circle) {
        if (g.extent == 1) rx = ry = std::min(sideX, sideY);
        else if (g.extent == 3) rx = ry = std::max(sideX, sideY);
        else rx = ry = std::sqrt(sideX * sideX + sideY * sideY);
    } else if (g.extent == 0 || g.extent == 2) {  // an ellipse through the corner, of the side ellipse's proportions
        rx = sideX * 1.41421356f;
        ry = sideY * 1.41421356f;
    } else {
        rx = sideX;
        ry = sideY;
    }
    if (rx <= 0 || ry <= 0) {
        solid = g.stops.back().c;
        return brush;
    }
    auto stops = resolveStops(g.stops, rx);
    float to = g.repeating ? std::max(stops.back().first, 1e-3f) : std::max(rx, stops.back().first);
    for (auto& s : stops) s.first = std::max(0.f, s.first);
    auto coll = stopCollection(rt, stops, 0, to, g.repeating, alpha);
    if (!coll) return brush;
    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> rb;
    float scale = to / rx;
    rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rx * scale, ry * scale), coll.Get(), &rb);
    brush = rb;
    return brush;
}
// The shape of a clip-path in its reference box x y w h (CSS Shapes 1, 3.1)
Microsoft::WRL::ComPtr<ID2D1Geometry> clipGeometry(ID2D1RenderTarget* rt, const ClipShape& s, float x, float y, float w, float h) {
    Microsoft::WRL::ComPtr<ID2D1Factory> f;
    rt->GetFactory(&f);
    Microsoft::WRL::ComPtr<ID2D1Geometry> out;
    switch (s.k) {
        case ClipShape::Inset: {
            float t = s.len[0].resolve(h), r = s.len[1].resolve(w), b = s.len[2].resolve(h), l = s.len[3].resolve(w);
            D2D1_RECT_F rect = D2D1::RectF(x + l, y + t, std::max(x + l, x + w - r), std::max(y + t, y + h - b));
            float rad = std::min(s.round.resolve(w), std::min(rect.right - rect.left, rect.bottom - rect.top) / 2);
            Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> g;
            f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, rad, rad), &g);
            out = g;
            break;
        }
        case ClipShape::Circle:
        case ClipShape::Ellipse: {
            float cx = s.cx.resolve(w), cy = s.cy.resolve(h);
            float sideX[2] = {std::min(cx, w - cx), std::max(cx, w - cx)}, sideY[2] = {std::min(cy, h - cy), std::max(cy, h - cy)};
            float rx, ry;
            if (s.k == ClipShape::Circle) {
                // a percentage is of the box's diagonal over the square root of two
                if (s.radiusKw[0] == 1) rx = std::min(sideX[0], sideY[0]);
                else if (s.radiusKw[0] == 2) rx = std::max(sideX[1], sideY[1]);
                else rx = s.len[0].resolve(std::sqrt(w * w + h * h) / 1.41421356f);
                ry = rx;
            } else {
                rx = s.radiusKw[0] ? sideX[s.radiusKw[0] - 1] : s.len[0].resolve(w);
                ry = s.radiusKw[1] ? sideY[s.radiusKw[1] - 1] : s.len[1].resolve(h);
            }
            Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> g;
            f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(x + cx, y + cy), std::max(0.f, rx), std::max(0.f, ry)), &g);
            out = g;
            break;
        }
        case ClipShape::Polygon: {
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> g;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(f->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) return out;
            sink->SetFillMode(s.evenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
            for (size_t i = 0; i < s.points.size(); ++i) {
                D2D1_POINT_2F p = D2D1::Point2F(x + s.points[i].first.resolve(w), y + s.points[i].second.resolve(h));
                if (i == 0) sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
                else sink->AddLine(p);
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            out = g;
            break;
        }
    }
    return out;
}
}  // namespace

void paint_background(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const DItem& it, std::vector<ImageRes>& imgs, uint64_t gen) {
    const BgPaint& p = *it.bg;
    const Background& bg = *p.bg;
    const BgImage& image = bg.images[p.layer];
    RectF4 border{it.x, it.y, it.w, it.h};
    RectF4 area = boxOf(border, p, cyclic(bg.origin, p.layer, (uint8_t)1));
    int clipWhich = cyclic(bg.clip, p.layer, (uint8_t)0);
    RectF4 clip = boxOf(border, p, clipWhich == 3 ? 0 : clipWhich);
    if (clip.w <= 0 || clip.h <= 0 || area.w <= 0 || area.h <= 0) return;
    // the tile size (3.9): an image's own size, a gradient's the positioning area's
    float iw = area.w, ih = area.h;
    ImageRes* picture = nullptr;
    if (image.k == BgImage::Url) {
        if (p.img < 0 || p.img >= (int)imgs.size() || imgs[p.img].w <= 0) return;
        picture = &imgs[p.img];
        iw = (float)picture->w;
        ih = (float)picture->h;
    }
    static const BgSize autoSize;
    const BgSize& size = cyclic(bg.size, p.layer, autoSize);
    float tw = iw, th = ih;
    if (size.mode == 1 || size.mode == 2) {
        float k = size.mode == 1 ? std::max(area.w / iw, area.h / ih) : std::min(area.w / iw, area.h / ih);
        tw = iw * k;
        th = ih * k;
    } else {
        bool wAuto = !size.w.definite(), hAuto = !size.h.definite();
        if (!wAuto) tw = size.w.resolve(area.w);
        if (!hAuto) th = size.h.resolve(area.h);
        if (wAuto && !hAuto) tw = picture ? th * iw / ih : area.w;
        if (hAuto && !wAuto) th = picture ? tw * ih / iw : area.h;
    }
    if (tw <= 0 || th <= 0) return;
    uint16_t rep = cyclic(bg.repeat, p.layer, (uint16_t)0);
    int rx = rep & 0xff, ry = rep >> 8;
    if (rx == 3) tw = area.w / std::max(1.f, std::round(area.w / tw));  // round: a whole number of tiles (3.4)
    if (ry == 3) th = area.h / std::max(1.f, std::round(area.h / th));
    static const BgPos origin0;
    const BgPos& pos = cyclic(bg.pos, p.layer, origin0);
    auto place = [](const Len& l, float room) { return l.u == Len::Pct ? room * l.v / 100.f + l.off : l.resolve(room); };
    float tx = area.x + place(pos.x, area.w - tw), ty = area.y + place(pos.y, area.h - th);
    bool repeatX = rx != 1, repeatY = ry != 1;
    // what the tiles may cover: the clip box, and the one row or column of a direction that does not repeat
    float x0 = clip.x, y0 = clip.y, x1 = clip.x + clip.w, y1 = clip.y + clip.h;
    if (!repeatX) { x0 = std::max(x0, tx); x1 = std::min(x1, tx + tw); }
    if (!repeatY) { y0 = std::max(y0, ty); y1 = std::min(y1, ty + th); }
    if (x1 <= x0 || y1 <= y0) return;
    Microsoft::WRL::ComPtr<ID2D1Layer> layer;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> rounded;
    float rad = std::min(p.radius, std::min(it.w, it.h) / 2);
    if (rad > 0.5f) {
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        rt->GetFactory(&factory);
        factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(it.x, it.y, it.x + it.w, it.y + it.h), rad, rad), &rounded);
        if (rounded) {
            rt->CreateLayer(&layer);
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rounded.Get()), layer.Get());
        }
    }
    float alpha = it.c.a;
    if (picture) {
        if (!picture->bmp || picture->gen != gen) {
            picture->bmp.Reset();
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            rt->CreateBitmap(D2D1::SizeU(picture->w, picture->h), picture->px.data(), picture->w * 4, bp, &picture->bmp);
            picture->gen = gen;
        }
        if (picture->bmp) {
            Microsoft::WRL::ComPtr<ID2D1BitmapBrush> brush;
            rt->CreateBitmapBrush(picture->bmp.Get(),
                                  D2D1::BitmapBrushProperties(repeatX ? D2D1_EXTEND_MODE_WRAP : D2D1_EXTEND_MODE_CLAMP,
                                                              repeatY ? D2D1_EXTEND_MODE_WRAP : D2D1_EXTEND_MODE_CLAMP,
                                                              D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                  &brush);
            if (brush) {
                brush->SetTransform(D2D1::Matrix3x2F::Scale(tw / iw, th / ih) * D2D1::Matrix3x2F::Translation(tx, ty));
                brush->SetOpacity(alpha);
                rt->FillRectangle(D2D1::RectF(x0, y0, x1, y1), brush.Get());
            }
        }
    } else {
        Color solid{};
        D2D1::Matrix3x2F base;
        auto brush = gradientBrush(rt, image, tw, th, alpha, solid, p, gen, base);
        if (!brush && solid.a > 0) {
            br->SetColor(D2D1::ColorF(solid.r, solid.g, solid.b, solid.a * alpha));
            rt->FillRectangle(D2D1::RectF(x0, y0, x1, y1), br);
        } else if (brush) {
            // the tiles the covered area meets, each painted with the brush moved to its corner
            float fx = repeatX ? tx - std::ceil((tx - x0) / tw) * tw : tx, fy = repeatY ? ty - std::ceil((ty - y0) / th) * th : ty;
            int count = 0;
            for (float yy = fy; yy < y1 && count < 4000; yy += th) {
                for (float xx = fx; xx < x1 && count < 4000; xx += tw, ++count) {
                    float a0 = std::max(x0, xx), b0 = std::max(y0, yy), a1 = std::min(x1, xx + tw), b1 = std::min(y1, yy + th);
                    if (a1 > a0 && b1 > b0) {
                        brush->SetTransform(base * D2D1::Matrix3x2F::Translation(xx, yy));
                        rt->FillRectangle(D2D1::RectF(a0, b0, a1, b1), brush.Get());
                    }
                    if (!repeatX) break;
                }
                if (!repeatY) break;
            }
        }
    }
    if (layer) rt->PopLayer();
}

void paintBox(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const DItem& it) {
    D2D1_RECT_F r = D2D1::RectF(it.x, it.y, it.x + it.w, it.y + it.h);
    const float* w = it.bw;
    float rad = std::min(it.radius, std::min(it.w, it.h) / 2);
    if (rad > 0.5f) {
        bool uniform = w[0] == w[1] && w[1] == w[2] && w[2] == w[3];
        if (!it.c.transparent()) {
            br->SetColor(cf(it.c));
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
        }
        if (uniform && w[0] > 0 && !it.bc[0].transparent()) {
            float h = w[0] / 2;
            br->SetColor(cf(it.bc[0]));
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), std::max(0.f, rad - h), std::max(0.f, rad - h)),
                br, w[0]);
            return;
        }
        if (uniform) return;
    } else if (!it.c.transparent()) {
        br->SetColor(cf(it.c));
        rt->FillRectangle(r, br);
    }
    if (w[0] > 0 && !it.bc[0].transparent()) {
        br->SetColor(cf(it.bc[0]));
        rt->FillRectangle(D2D1::RectF(it.x, it.y, it.x + it.w, it.y + w[0]), br);
    }
    if (w[2] > 0 && !it.bc[2].transparent()) {
        br->SetColor(cf(it.bc[2]));
        rt->FillRectangle(D2D1::RectF(it.x, it.y + it.h - w[2], it.x + it.w, it.y + it.h), br);
    }
    if (w[3] > 0 && !it.bc[3].transparent()) {
        br->SetColor(cf(it.bc[3]));
        rt->FillRectangle(D2D1::RectF(it.x, it.y, it.x + w[3], it.y + it.h), br);
    }
    if (w[1] > 0 && !it.bc[1].transparent()) {
        br->SetColor(cf(it.bc[1]));
        rt->FillRectangle(D2D1::RectF(it.x + it.w - w[1], it.y, it.x + it.w, it.y + it.h), br);
    }
    return;
}

// ---- shadows (CSS Backgrounds 3, 7.1; CSS Text Decoration 3, 4). The blur is approximated by layers: the shape
// drawn a few times, grown and shrunk across the blur radius, each layer faint, so that the edge fades from the full
// colour inside to nothing outside as a Gaussian blur of half the radius roughly does.
int blurLayers(float blur) { return blur < 0.5f ? 1 : std::clamp((int)std::ceil(blur * 0.75f), 2, 24); }
// the alpha of each layer, so that where all of them overlap the shadow has its own colour
float layerAlpha(float a, int n) { return n == 1 ? a : 1 - std::pow(1 - std::min(a, 0.999f), 1.f / (float)n); }
// how far layer i of n is grown: from +blur (the faintest edge, outside) to -blur (inside)
float layerGrowth(float blur, int i, int n) { return n == 1 ? 0 : blur * (1 - 2 * ((float)i + 0.5f) / (float)n); }

Microsoft::WRL::ComPtr<ID2D1Geometry> roundedGeometry(ID2D1Factory* f, D2D1_RECT_F r, float rad) {
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> g;
    f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, std::max(0.f, rad), std::max(0.f, rad)), &g);
    return g;
}
// a outside b: a path geometry of the area of a that b does not cover
Microsoft::WRL::ComPtr<ID2D1Geometry> without(ID2D1Factory* f, ID2D1Geometry* a, ID2D1Geometry* b) {
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    a->CombineWithGeometry(b, D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
    sink->Close();
    return path;
}
D2D1_RECT_F grow(D2D1_RECT_F r, float e) { return D2D1::RectF(r.left - e, r.top - e, r.right + e, r.bottom + e); }

void paintBoxShadows(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const DItem& it, bool inset) {
    Microsoft::WRL::ComPtr<ID2D1Factory> f;
    rt->GetFactory(&f);
    float rad = std::min(it.radius, std::min(it.w, it.h) / 2);
    D2D1_RECT_F box = D2D1::RectF(it.x, it.y, it.x + it.w, it.y + it.h);
    if (!inset) {
        // an outer shadow shows only outside the border box (7.1.1): masked off where the box's own fill does not hide it
        float reach = 0;
        for (auto& s : *it.shadows)
            if (!s.inset) reach = std::max(reach, std::fabs(s.dx) + std::fabs(s.dy) + std::max(0.f, s.spread) + 2 * s.blur + 2);
        Microsoft::WRL::ComPtr<ID2D1Geometry> mask;
        if (it.c.a < 0.999f) {
            auto outer = roundedGeometry(f.Get(), grow(box, reach), 0);
            auto inner = roundedGeometry(f.Get(), box, rad);
            if (outer && inner) mask = without(f.Get(), outer.Get(), inner.Get());
        }
        if (mask) rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), mask.Get()), nullptr);
        for (size_t k = it.shadows->size(); k-- > 0;) {  // the first shadow is on top
            const Shadow& s = (*it.shadows)[k];
            if (s.inset || s.c.a <= 0) continue;
            D2D1_RECT_F shape = grow(D2D1::RectF(box.left + s.dx, box.top + s.dy, box.right + s.dx, box.bottom + s.dy), s.spread);
            if (shape.right <= shape.left || shape.bottom <= shape.top) continue;
            int n = blurLayers(s.blur);
            br->SetColor(D2D1::ColorF(s.c.r, s.c.g, s.c.b, layerAlpha(s.c.a, n)));
            for (int i = 0; i < n; ++i) {
                float e = layerGrowth(s.blur, i, n);
                D2D1_RECT_F r = grow(shape, e);
                if (r.right <= r.left || r.bottom <= r.top) continue;
                float rr = rad > 0 ? std::max(0.f, rad + s.spread + e) : std::max(0.f, e);
                if (rr > 0.5f) rt->FillRoundedRectangle(D2D1::RoundedRect(r, rr, rr), br);
                else rt->FillRectangle(r, br);
            }
        }
        if (mask) rt->PopLayer();
        return;
    }
    // inset shadows (7.1.2): inside the padding box, the colour everywhere but a hole the shadow's shape makes
    D2D1_RECT_F pad = D2D1::RectF(box.left + it.bw[3], box.top + it.bw[0], box.right - it.bw[1], box.bottom - it.bw[2]);
    if (pad.right <= pad.left || pad.bottom <= pad.top) return;
    float padRad = std::max(0.f, rad - std::max(std::max(it.bw[0], it.bw[1]), std::max(it.bw[2], it.bw[3])));
    auto clip = roundedGeometry(f.Get(), pad, padRad);
    if (!clip) return;
    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), nullptr);
    for (size_t k = it.shadows->size(); k-- > 0;) {
        const Shadow& s = (*it.shadows)[k];
        if (!s.inset || s.c.a <= 0) continue;
        D2D1_RECT_F hole = grow(D2D1::RectF(pad.left + s.dx, pad.top + s.dy, pad.right + s.dx, pad.bottom + s.dy), -s.spread);
        float reach = std::fabs(s.dx) + std::fabs(s.dy) + std::fabs(s.spread) + 2 * s.blur + 2;
        auto outer = roundedGeometry(f.Get(), grow(pad, reach), 0);
        int n = blurLayers(s.blur);
        br->SetColor(D2D1::ColorF(s.c.r, s.c.g, s.c.b, layerAlpha(s.c.a, n)));
        for (int i = 0; i < n; ++i) {
            float e = -layerGrowth(s.blur, i, n);  // the faintest layer has the largest hole
            D2D1_RECT_F h = grow(hole, -e);
            if (h.right <= h.left || h.bottom <= h.top) {
                rt->FillRectangle(grow(pad, 1), br);
                continue;
            }
            auto holeGeo = roundedGeometry(f.Get(), h, std::max(0.f, padRad - s.spread + e));
            auto area = outer && holeGeo ? without(f.Get(), outer.Get(), holeGeo.Get()) : nullptr;
            if (area) rt->FillGeometry(area.Get(), br);
        }
    }
    rt->PopLayer();
}

void paintTextShadows(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const DItem& it) {
    for (size_t k = it.shadows->size(); k-- > 0;) {
        const Shadow& s = (*it.shadows)[k];
        if (s.c.a <= 0) continue;
        // a blurred shadow: the text drawn faintly in the middle and round two circles inside the blur radius, which
        // spreads it about as far as the blur does and fades it outwards
        int n = s.blur < 0.5f ? 1 : 17;
        br->SetColor(D2D1::ColorF(s.c.r, s.c.g, s.c.b, layerAlpha(s.c.a, n) * (n > 1 ? 0.6f : 1.f)));
        for (int i = 0; i < n; ++i) {
            float ox = 0, oy = 0;
            if (i > 0) {
                float t = (float)((i - 1) % 8) * 3.14159265f / 4 + (i > 8 ? 0.39f : 0), r = s.blur * (i > 8 ? 0.6f : 0.3f);
                ox = std::cos(t) * r;
                oy = std::sin(t) * r;
            }
            rt->DrawTextLayout(D2D1::Point2F(it.x + s.dx + ox, it.y + s.dy + oy), it.tl.Get(), br, D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }
}

void paint_items(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, LayoutOut& lo, std::vector<ImageRes>& imgs,
                 float y0, float y1, uint64_t gen, WidgetPainter* wp) {
    std::vector<bool> clips;  // the open clips, innermost last: a clip-path layer (true) or a rectangle
    std::vector<D2D1_MATRIX_3X2_F> outer;  // the transforms in force around each open PushTransform
    for (auto& it : lo.items) {
        if (it.k == DItem::K::PushTransform) {
            D2D1_MATRIX_3X2_F now;
            rt->GetTransform(&now);
            outer.push_back(now);
            D2D1::Matrix3x2F m(it.bw[0], it.bw[1], it.bw[2], it.bw[3], it.w, it.h);
            D2D1::Matrix3x2F local = D2D1::Matrix3x2F::Translation(-it.x, -it.y) * m * D2D1::Matrix3x2F::Translation(it.x, it.y);
            rt->SetTransform(local * *D2D1::Matrix3x2F::ReinterpretBaseType(&now));
            continue;
        }
        if (it.k == DItem::K::PopTransform) {
            if (!outer.empty()) {
                rt->SetTransform(outer.back());
                outer.pop_back();
            }
            continue;
        }
        if (it.k == DItem::K::PushClip) {
            Microsoft::WRL::ComPtr<ID2D1Geometry> geometry = it.shape ? clipGeometry(rt, *it.shape, it.x, it.y, it.w, it.h) : nullptr;
            if (geometry) {
                rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.Get()), nullptr);
            } else {
                rt->PushAxisAlignedClip(D2D1::RectF(it.x, it.y, it.x + it.w, it.y + it.h), D2D1_ANTIALIAS_MODE_ALIASED);
            }
            clips.push_back(geometry != nullptr);
            continue;
        }
        if (it.k == DItem::K::PopClip) {
            if (!clips.empty()) {
                if (clips.back()) rt->PopLayer();
                else rt->PopAxisAlignedClip();
                clips.pop_back();
            }
            continue;
        }
        if (outer.empty() && (it.y > y1 || it.y + it.h < y0)) continue;  // inside a transform, positions say little
        switch (it.k) {
            case DItem::K::Box:
                if (it.shadows && (it.shadowParts & 1)) paintBoxShadows(rt, br, it, false);
                if (it.shadows && (it.shadowParts & 2)) {  // the inset shadows go between the background and the borders
                    DItem fill = it;
                    for (Color& bc : fill.bc) bc = Color{};
                    paintBox(rt, br, fill);
                    paintBoxShadows(rt, br, it, true);
                    DItem borders = it;
                    borders.c = Color{};
                    paintBox(rt, br, borders);
                    break;
                }
                paintBox(rt, br, it);
                break;
            case DItem::K::Rect:
                br->SetColor(cf(it.c));
                rt->FillRectangle(D2D1::RectF(it.x, it.y, it.x + it.w, it.y + it.h), br);
                break;
            case DItem::K::Text:
                if (it.shadows) paintTextShadows(rt, br, it);
                br->SetColor(cf(it.c));
                rt->DrawTextLayout(D2D1::Point2F(it.x, it.y), it.tl.Get(), br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                break;
            case DItem::K::Widget:
                if (wp) wp->paintWidget(rt, it);
                break;
            case DItem::K::Background:
                if (it.bg) paint_background(rt, br, it, imgs, gen);
                break;
            case DItem::K::Svg:
                if (it.img >= 0 && it.img < (int)lo.svgs.size()) svg_paint(rt, br, lo.svgs[it.img], lo.svgShapes, it.x, it.y, it.w, it.h);
                break;
            case DItem::K::Image: {
                D2D1_RECT_F r = D2D1::RectF(it.x, it.y, it.x + it.w, it.y + it.h);
                if (it.img >= 0 && it.img < (int)imgs.size() && imgs[it.img].w > 0) {
                    ImageRes& im = imgs[it.img];
                    if (!im.bmp || im.gen != gen) {
                        im.bmp.Reset();
                        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                        rt->CreateBitmap(D2D1::SizeU(im.w, im.h), im.px.data(), im.w * 4, bp, &im.bmp);
                        im.gen = gen;
                    }
                    if (im.bmp) rt->DrawBitmap(im.bmp.Get(), r, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                } else if (it.img == -1 && it.w > 2 && it.h > 2) {
                    br->SetColor(D2D1::ColorF(0xf1f3f4));
                    rt->FillRectangle(r, br);
                    br->SetColor(D2D1::ColorF(0xc4c7c5));
                    rt->DrawRectangle(D2D1::RectF(r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f), br, 1.f);
                }
                break;
            }
            default: break;
        }
    }
    for (; !clips.empty(); clips.pop_back()) {
        if (clips.back()) rt->PopLayer();
        else rt->PopAxisAlignedClip();
    }
    if (!outer.empty()) rt->SetTransform(outer.front());
}

}  // namespace tb
