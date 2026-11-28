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
// A brush that paints the gradient of one tile whose top left corner is at the origin
Microsoft::WRL::ComPtr<ID2D1Brush> gradientBrush(ID2D1RenderTarget* rt, const BgImage& g, float w, float h, float alpha, Color& solid) {
    Microsoft::WRL::ComPtr<ID2D1Brush> brush;
    if (g.stops.empty()) return brush;
    if (g.k == BgImage::Conic || (g.stops.size() == 1)) {
        solid = g.stops[0].c;
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
        auto brush = gradientBrush(rt, image, tw, th, alpha, solid);
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
                        brush->SetTransform(D2D1::Matrix3x2F::Translation(xx, yy));
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

void paint_items(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, LayoutOut& lo, std::vector<ImageRes>& imgs,
                 float y0, float y1, uint64_t gen, WidgetPainter* wp) {
    int clipDepth = 0;
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
            rt->PushAxisAlignedClip(D2D1::RectF(it.x, it.y, it.x + it.w, it.y + it.h), D2D1_ANTIALIAS_MODE_ALIASED);
            ++clipDepth;
            continue;
        }
        if (it.k == DItem::K::PopClip) {
            if (clipDepth > 0) {
                rt->PopAxisAlignedClip();
                --clipDepth;
            }
            continue;
        }
        if (outer.empty() && (it.y > y1 || it.y + it.h < y0)) continue;  // inside a transform, positions say little
        switch (it.k) {
            case DItem::K::Box: {
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
                        break;
                    }
                    if (uniform) break;
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
                break;
            }
            case DItem::K::Rect:
                br->SetColor(cf(it.c));
                rt->FillRectangle(D2D1::RectF(it.x, it.y, it.x + it.w, it.y + it.h), br);
                break;
            case DItem::K::Text:
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
    while (clipDepth-- > 0) rt->PopAxisAlignedClip();
    if (!outer.empty()) rt->SetTransform(outer.front());
}

}  // namespace tb
