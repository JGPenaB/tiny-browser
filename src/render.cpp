// GPU painter: walks the display list and issues Direct2D draw calls.
#include "layout.h"

namespace tb {

static inline D2D1_COLOR_F cf(const Color& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }

void paint_items(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, LayoutOut& lo, std::vector<ImageRes>& imgs,
                 float y0, float y1, uint64_t gen, WidgetPainter* wp) {
    int clipDepth = 0;
    for (auto& it : lo.items) {
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
        if (it.y > y1 || it.y + it.h < y0) continue;
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
}

}  // namespace tb
