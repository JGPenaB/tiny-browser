// Inline SVG: see svg.h for what is covered.
#include "svg.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "css.h"

namespace tb {

namespace {

using Matrix = std::array<float, 6>;  // a b c d e f

Matrix multiply(const Matrix& p, const Matrix& q) {  // p * q: q is applied first
    return {p[0] * q[0] + p[2] * q[1], p[1] * q[0] + p[3] * q[1], p[0] * q[2] + p[2] * q[3],
            p[1] * q[2] + p[3] * q[3], p[0] * q[4] + p[2] * q[5] + p[4], p[1] * q[4] + p[3] * q[5] + p[5]};
}
const Matrix kIdentity{1, 0, 0, 1, 0, 0};

const std::string* attrOf(const Node* n, const char* lowerName, const char* svgName = nullptr) {
    // The HTML parser lowers attribute names, while scripts may set them with the SVG spelling (viewBox).
    const std::string* v = n->attr(lowerName);
    return v || !svgName ? v : n->attr(svgName);
}

// A run of numbers as SVG writes them (SVG 2, path data and number lists): separated by whitespace and/or one
// comma, or by nothing where a sign or a second decimal point starts the next one.
struct Numbers {  // reads from a string that must outlive it
    const char* p;
    const char* end;
    explicit Numbers(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}
    explicit Numbers(std::string&&) = delete;
    void skip() {
        while (p < end && (std::isspace((unsigned char)*p) || *p == ',')) ++p;
    }
    bool number(float& v) {
        skip();
        if (p >= end) return false;
        const char* q = p;
        if (*q == '+' || *q == '-') ++q;
        bool digits = false;
        while (q < end && std::isdigit((unsigned char)*q)) ++q, digits = true;
        if (q < end && *q == '.') {
            ++q;
            while (q < end && std::isdigit((unsigned char)*q)) ++q, digits = true;
        }
        if (!digits) return false;
        if (q < end && (*q == 'e' || *q == 'E')) {
            const char* r = q + 1;
            if (r < end && (*r == '+' || *r == '-')) ++r;
            if (r < end && std::isdigit((unsigned char)*r)) {
                q = r;
                while (q < end && std::isdigit((unsigned char)*q)) ++q;
            }
        }
        v = (float)std::strtod(std::string(p, q).c_str(), nullptr);
        p = q;
        return std::isfinite(v);
    }
    bool flag(float& v) {  // an arc flag is one digit, and may touch what follows
        skip();
        if (p >= end || (*p != '0' && *p != '1')) return false;
        v = (float)(*p++ - '0');
        return true;
    }
    bool atNumber() {
        skip();
        return p < end && (std::isdigit((unsigned char)*p) || *p == '.' || *p == '-' || *p == '+');
    }
};

// Path data (SVG 2, section 9.3) made absolute. On an error the path keeps what came before it.
void parsePath(const std::string& d, SvgShape& s) {
    Numbers in(d);
    char cmd = 0;
    float cx = 0, cy = 0, startX = 0, startY = 0, lastCtrlX = 0, lastCtrlY = 0;
    char lastCmd = 0;
    bool open = false;
    auto put = [&](SvgShape::Op op, std::initializer_list<float> a) {
        s.ops.push_back(op);
        s.args.insert(s.args.end(), a);
    };
    for (;;) {
        in.skip();
        if (in.p >= in.end) break;
        if (std::isalpha((unsigned char)*in.p)) {
            cmd = *in.p++;
        } else if (!cmd || cmd == 'Z' || cmd == 'z') {
            break;
        } else if (cmd == 'M') {
            cmd = 'L';  // more pairs after a moveto are linetos
        } else if (cmd == 'm') {
            cmd = 'l';
        }
        bool rel = std::islower((unsigned char)cmd);
        char up = (char)std::toupper((unsigned char)cmd);
        float v[7];
        auto read = [&](int count) {
            for (int i = 0; i < count; ++i)
                if (!in.number(v[i])) return false;
            return true;
        };
        if (up == 'Z') {
            if (open) put(SvgShape::Close, {});
            open = false;
            cx = startX, cy = startY;
            lastCmd = 'Z';
            continue;
        }
        if (!open && up != 'M') {  // drawing after a closepath starts again where the subpath began
            put(SvgShape::Move, {cx, cy});
            open = true;
        }
        switch (up) {
            case 'M':
                if (!read(2)) return;
                cx = rel ? cx + v[0] : v[0], cy = rel ? cy + v[1] : v[1];
                startX = cx, startY = cy;
                put(SvgShape::Move, {cx, cy});
                open = true;
                break;
            case 'L':
                if (!read(2)) return;
                cx = rel ? cx + v[0] : v[0], cy = rel ? cy + v[1] : v[1];
                put(SvgShape::Line, {cx, cy});
                break;
            case 'H':
                if (!read(1)) return;
                cx = rel ? cx + v[0] : v[0];
                put(SvgShape::Line, {cx, cy});
                break;
            case 'V':
                if (!read(1)) return;
                cy = rel ? cy + v[0] : v[0];
                put(SvgShape::Line, {cx, cy});
                break;
            case 'C':
            case 'S': {
                float x1, y1;
                if (up == 'C') {
                    if (!read(6)) return;
                    x1 = rel ? cx + v[0] : v[0], y1 = rel ? cy + v[1] : v[1];
                    std::memmove(v, v + 2, 4 * sizeof(float));
                } else {
                    if (!read(4)) return;
                    bool smooth = lastCmd == 'C' || lastCmd == 'S';
                    x1 = smooth ? 2 * cx - lastCtrlX : cx, y1 = smooth ? 2 * cy - lastCtrlY : cy;
                }
                float x2 = rel ? cx + v[0] : v[0], y2 = rel ? cy + v[1] : v[1], x = rel ? cx + v[2] : v[2], y = rel ? cy + v[3] : v[3];
                put(SvgShape::Cubic, {x1, y1, x2, y2, x, y});
                lastCtrlX = x2, lastCtrlY = y2, cx = x, cy = y;
                break;
            }
            case 'Q':
            case 'T': {
                float x1, y1;
                if (up == 'Q') {
                    if (!read(4)) return;
                    x1 = rel ? cx + v[0] : v[0], y1 = rel ? cy + v[1] : v[1];
                    v[0] = v[2], v[1] = v[3];
                } else {
                    if (!read(2)) return;
                    bool smooth = lastCmd == 'Q' || lastCmd == 'T';
                    x1 = smooth ? 2 * cx - lastCtrlX : cx, y1 = smooth ? 2 * cy - lastCtrlY : cy;
                }
                float x = rel ? cx + v[0] : v[0], y = rel ? cy + v[1] : v[1];
                put(SvgShape::Quad, {x1, y1, x, y});
                lastCtrlX = x1, lastCtrlY = y1, cx = x, cy = y;
                break;
            }
            case 'A': {
                if (!in.number(v[0]) || !in.number(v[1]) || !in.number(v[2]) || !in.flag(v[3]) || !in.flag(v[4]) || !in.number(v[5]) ||
                    !in.number(v[6]))
                    return;
                float x = rel ? cx + v[5] : v[5], y = rel ? cy + v[6] : v[6];
                if (x == cx && y == cy) break;  // an arc to where it starts draws nothing (SVG 2, B.2.5)
                if (v[0] == 0 || v[1] == 0) put(SvgShape::Line, {x, y});
                else put(SvgShape::Arc, {std::fabs(v[0]), std::fabs(v[1]), v[2], v[3], v[4], x, y});
                cx = x, cy = y;
                break;
            }
            default: return;
        }
        lastCmd = up;
    }
}

float lengthAttr(const Node* n, const char* name, float percentOf, float fallback = 0) {
    const std::string* a = n->attr(name);
    if (!a) return fallback;
    const char* start = a->c_str();
    while (std::isspace((unsigned char)*start)) ++start;
    char* end = nullptr;
    double v = std::strtod(start, &end);
    if (end == start || !std::isfinite(v)) return fallback;
    if (*end == '%') return (float)v * percentOf / 100;
    return (float)v;  // a number or px; other units are not converted
}

// The transform attribute (CSS Transforms 1, section 12, the SVG syntax): unitless numbers, rotate about a point.
Matrix transformAttr(const Node* n) {
    const std::string* a = n->attr("transform");
    Matrix m = kIdentity;
    if (!a) return m;
    std::string s = lower(*a);
    size_t i = 0;
    const float pi = 3.14159265358979f;
    while (i < s.size()) {
        size_t open = s.find('(', i), close = s.find(')', i);
        if (open == std::string::npos || close == std::string::npos || close < open) break;
        std::string name(trim(std::string_view(s).substr(i, open - i)));
        while (!name.empty() && (name[0] == ',' || std::isspace((unsigned char)name[0]))) name.erase(0, 1);
        std::string argText = s.substr(open + 1, close - open - 1);  // Numbers points into it
        Numbers in(argText);
        std::vector<float> v;
        float x;
        while (in.number(x)) v.push_back(x);
        i = close + 1;
        Matrix f = kIdentity;
        if (name == "matrix" && v.size() == 6) f = {v[0], v[1], v[2], v[3], v[4], v[5]};
        else if (name == "translate" && !v.empty()) f = {1, 0, 0, 1, v[0], v.size() > 1 ? v[1] : 0};
        else if (name == "scale" && !v.empty()) f = {v[0], 0, 0, v.size() > 1 ? v[1] : v[0], 0, 0};
        else if (name == "rotate" && !v.empty()) {
            float r = v[0] * pi / 180, c = std::cos(r), sn = std::sin(r);
            f = {c, sn, -sn, c, 0, 0};
            if (v.size() == 3) f = multiply(multiply(Matrix{1, 0, 0, 1, v[1], v[2]}, f), Matrix{1, 0, 0, 1, -v[1], -v[2]});
        } else if (name == "skewx" && v.size() == 1) f = {1, 0, std::tan(v[0] * pi / 180), 1, 0, 0};
        else if (name == "skewy" && v.size() == 1) f = {1, std::tan(v[0] * pi / 180), 0, 1, 0, 0};
        else break;  // an error ends the list (and SVG 2 says the attribute is then ignored as a whole)
        m = multiply(m, f);
    }
    return m;
}

struct Viewport {
    float w = 300, h = 150;  // what percentages of shape attributes refer to
};

void parseViewBox(const Node* n, SvgImage& img) {
    const std::string* a = attrOf(n, "viewbox", "viewBox");
    if (!a) return;
    Numbers in(*a);
    float v[4];
    for (float& x : v)
        if (!in.number(x)) return;
    if (v[2] <= 0 || v[3] <= 0) return;  // a negative or zero size turns the viewBox off
    img.hasViewBox = true;
    std::copy(v, v + 4, img.vb);
    if (const std::string* p = attrOf(n, "preserveaspectratio", "preserveAspectRatio")) {
        std::string s = lower(*p);
        if (s.find("none") != std::string::npos) img.stretch = true;
        if (s.find("xmin") != std::string::npos) img.alignX = 0;
        if (s.find("xmax") != std::string::npos) img.alignX = 2;
        if (s.find("ymin") != std::string::npos) img.alignY = 0;
        if (s.find("ymax") != std::string::npos) img.alignY = 2;
        img.slice = s.find("slice") != std::string::npos;
    }
}

// How a viewBox is fitted into a box of the given size (SVG 2, section 8.2).
Matrix viewBoxMatrix(const SvgImage& img, float w, float h) {
    if (!img.hasViewBox) return kIdentity;
    float sx = w / img.vb[2], sy = h / img.vb[3];
    if (!img.stretch) sx = sy = img.slice ? std::max(sx, sy) : std::min(sx, sy);
    float tx = -img.vb[0] * sx, ty = -img.vb[1] * sy;
    float freeX = w - img.vb[2] * sx, freeY = h - img.vb[3] * sy;
    if (!img.stretch) {
        tx += img.alignX == 1 ? freeX / 2 : img.alignX == 2 ? freeX : 0;
        ty += img.alignY == 1 ? freeY / 2 : img.alignY == 2 ? freeY : 0;
    }
    return {sx, 0, 0, sy, tx, ty};
}

const Node* byId(const Node* n, const std::string& id, int depth = 0) {
    if (n->type == Node::T::Element && n->id == id) return n;
    if (depth > 200) return nullptr;
    for (auto& k : n->kids)
        if (const Node* f = byId(k.get(), id, depth + 1)) return f;
    return nullptr;
}

bool samePaint(const Paint& a, const Paint& b) {
    return a.k == b.k && (a.k != Paint::Rgb || (a.c.r == b.c.r && a.c.g == b.c.g && a.c.b == b.c.b && a.c.a == b.c.a));
}

struct Builder {
    const Document* doc;
    std::vector<SvgShape>& shapes;
    int uses = 0;
    // While drawing what a use element refers to: the use element's style, and the style the referenced element's
    // content inherited from where it really is. The content of a use inherits from the use (SVG 2, 5.6.3); this
    // engine styles the referenced elements in place, so a paint equal to the one inherited there is taken as
    // inherited and replaced by the use element's.
    const Style* useStyle = nullptr;
    const Style* placeStyle = nullptr;

    void paintOf(const Node* n, SvgShape& s) {
        const Style* st = n->style.get();
        if (!st) return;
        Paint fill = st->fill, stroke = st->stroke;
        if (useStyle && placeStyle) {
            if (samePaint(fill, placeStyle->fill)) fill = useStyle->fill;
            if (samePaint(stroke, placeStyle->stroke)) stroke = useStyle->stroke;
        }
        const Style& colorFrom = useStyle && fill.k == Paint::Current && st->color.r == placeStyle->color.r ? *useStyle : *st;
        auto resolve = [&](const Paint& p, Color& out, float opacity) {
            if (p.k == Paint::None) return false;
            out = p.k == Paint::Current ? colorFrom.color : p.c;
            out.a *= opacity * st->alpha;
            return out.a > 0;
        };
        s.fill = resolve(fill, s.fillColor, st->fillOpacity);
        s.stroke = resolve(stroke, s.strokeColor, st->strokeOpacity);
        s.strokeWidth = st->strokeWidth.resolve(100);
        if (s.strokeWidth <= 0) s.stroke = false;
        s.evenOdd = st->fillEvenOdd;
    }

    void shape(const Node* n, const Matrix& m, const Viewport& vp) {
        SvgShape s;
        const std::string& name = n->name;
        auto px = [&](const char* a, bool horizontal) { return lengthAttr(n, a, horizontal ? vp.w : vp.h); };
        auto arcTo = [&](float rx, float ry, float x, float y) {
            s.ops.push_back(SvgShape::Arc);
            s.args.insert(s.args.end(), {rx, ry, 0, 0, 1, x, y});
        };
        auto moveTo = [&](float x, float y, SvgShape::Op op = SvgShape::Move) {
            s.ops.push_back(op);
            s.args.insert(s.args.end(), {x, y});
        };
        if (name == "path") {
            if (const std::string* d = n->attr("d")) parsePath(*d, s);
        } else if (name == "rect") {
            float x = px("x", true), y = px("y", false), w = px("width", true), h = px("height", false);
            if (w <= 0 || h <= 0) return;
            bool hasRx = n->attr("rx"), hasRy = n->attr("ry");
            float rx = px("rx", true), ry = px("ry", false);
            if (!hasRx) rx = ry;
            if (!hasRy) ry = rx;
            rx = std::clamp(rx, 0.f, w / 2), ry = std::clamp(ry, 0.f, h / 2);
            if (rx > 0 && ry > 0) {
                moveTo(x + rx, y);
                moveTo(x + w - rx, y, SvgShape::Line);
                arcTo(rx, ry, x + w, y + ry);
                moveTo(x + w, y + h - ry, SvgShape::Line);
                arcTo(rx, ry, x + w - rx, y + h);
                moveTo(x + rx, y + h, SvgShape::Line);
                arcTo(rx, ry, x, y + h - ry);
                moveTo(x, y + ry, SvgShape::Line);
                arcTo(rx, ry, x + rx, y);
            } else {
                moveTo(x, y);
                moveTo(x + w, y, SvgShape::Line);
                moveTo(x + w, y + h, SvgShape::Line);
                moveTo(x, y + h, SvgShape::Line);
            }
            s.ops.push_back(SvgShape::Close);
        } else if (name == "circle" || name == "ellipse") {
            float cx = px("cx", true), cy = px("cy", false), rx, ry;
            if (name == "circle") rx = ry = lengthAttr(n, "r", std::sqrt((vp.w * vp.w + vp.h * vp.h) / 2));
            else rx = px("rx", true), ry = px("ry", false);
            if (rx <= 0 || ry <= 0) return;
            moveTo(cx + rx, cy);
            arcTo(rx, ry, cx - rx, cy);
            arcTo(rx, ry, cx + rx, cy);
            s.ops.push_back(SvgShape::Close);
        } else if (name == "line") {
            moveTo(px("x1", true), px("y1", false));
            moveTo(px("x2", true), px("y2", false), SvgShape::Line);
        } else if (name == "polyline" || name == "polygon") {
            const std::string* pts = n->attr("points");
            if (!pts) return;
            Numbers in(*pts);
            float x, y;
            bool first = true;
            while (in.number(x) && in.number(y)) {
                moveTo(x, y, first ? SvgShape::Move : SvgShape::Line);
                first = false;
            }
            if (first) return;
            if (name == "polygon") s.ops.push_back(SvgShape::Close);
        }
        if (s.ops.empty()) return;
        paintOf(n, s);
        if (!s.fill && !s.stroke) return;
        std::copy(m.begin(), m.end(), s.m);
        shapes.push_back(std::move(s));
    }

    void children(const Node* n, const Matrix& m, const Viewport& vp, int depth) {
        for (auto& k : n->kids) element(k.get(), m, vp, depth + 1);
    }

    void element(const Node* n, const Matrix& outer, const Viewport& vp, int depth) {
        if (n->type != Node::T::Element || depth > 64 || shapes.size() > 20000) return;
        const Style* st = n->style.get();
        if (st && st->display == Display::None) return;
        const std::string& name = n->name;
        Matrix m = multiply(outer, transformAttr(n));
        if (name == "g" || name == "a" || name == "switch") {
            children(n, m, vp, depth);
        } else if (name == "svg") {  // a nested viewport, placed at x y and sized by width and height
            SvgImage inner;
            parseViewBox(n, inner);
            Viewport v{lengthAttr(n, "width", vp.w, vp.w), lengthAttr(n, "height", vp.h, vp.h)};
            Matrix place{1, 0, 0, 1, lengthAttr(n, "x", vp.w), lengthAttr(n, "y", vp.h)};
            Viewport content = inner.hasViewBox ? Viewport{inner.vb[2], inner.vb[3]} : v;
            children(n, multiply(multiply(outer, place), viewBoxMatrix(inner, v.w, v.h)), content, depth);
        } else if (name == "use") {
            const std::string* href = attrOf(n, "href", "xlink:href");
            if (!href || href->empty() || (*href)[0] != '#' || ++uses > 2000 || !doc || !doc->root) return;
            const Node* target = byId(doc->root.get(), href->substr(1));
            if (!target || target == n) return;
            Matrix at = multiply(m, Matrix{1, 0, 0, 1, lengthAttr(n, "x", vp.w), lengthAttr(n, "y", vp.h)});
            const Style* outerUse = useStyle;
            const Style* outerPlace = placeStyle;
            useStyle = n->style.get();
            placeStyle = target->parent && target->parent->style ? target->parent->style.get() : nullptr;
            if (target->name == "symbol") {
                SvgImage inner;
                parseViewBox(target, inner);
                Viewport v{lengthAttr(n, "width", vp.w, vp.w), lengthAttr(n, "height", vp.h, vp.h)};
                Viewport content = inner.hasViewBox ? Viewport{inner.vb[2], inner.vb[3]} : v;
                children(target, multiply(at, viewBoxMatrix(inner, v.w, v.h)), content, depth);
            } else {
                element(target, at, vp, depth);
            }
            useStyle = outerUse;
            placeStyle = outerPlace;
        } else {
            shape(n, m, vp);
        }
    }
};

}  // namespace

void svg_path_data(const std::string& d, SvgShape& out) { parsePath(d, out); }

float svg_aspect(const Node* svg) {
    SvgImage img;
    parseViewBox(svg, img);
    return img.hasViewBox ? img.vb[2] / img.vb[3] : 0;
}

SvgImage svg_build(const Node* svg, const Document* doc, std::vector<SvgShape>& shapes) {
    SvgImage img;
    parseViewBox(svg, img);
    img.first = shapes.size();
    Builder b{doc, shapes};
    Viewport vp;
    if (img.hasViewBox) vp = {img.vb[2], img.vb[3]};
    b.children(svg, kIdentity, vp, 0);
    img.count = shapes.size() - img.first;
    return img;
}

void svg_paint(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const SvgImage& img, std::vector<SvgShape>& shapes, float x, float y,
               float w, float h) {
    if (!img.count || w <= 0 || h <= 0) return;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    rt->GetFactory(&factory);
    D2D1_MATRIX_3X2_F around;
    rt->GetTransform(&around);
    Matrix fit = viewBoxMatrix(img, w, h);
    fit[4] += x, fit[5] += y;
    rt->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);  // svg:not(:root) clips
    for (size_t i = img.first; i < img.first + img.count && i < shapes.size(); ++i) {
        SvgShape& s = shapes[i];
        if (!s.geometry) {
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> g;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) continue;
            sink->SetFillMode(s.evenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
            bool open = false;
            size_t a = 0;
            for (SvgShape::Op op : s.ops) {
                const float* v = s.args.data() + a;
                switch (op) {
                    case SvgShape::Move:
                        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->BeginFigure(D2D1::Point2F(v[0], v[1]), D2D1_FIGURE_BEGIN_FILLED);
                        open = true;
                        a += 2;
                        break;
                    case SvgShape::Line:
                        if (open) sink->AddLine(D2D1::Point2F(v[0], v[1]));
                        a += 2;
                        break;
                    case SvgShape::Cubic:
                        if (open) sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(v[0], v[1]), D2D1::Point2F(v[2], v[3]), D2D1::Point2F(v[4], v[5])));
                        a += 6;
                        break;
                    case SvgShape::Quad:
                        if (open) sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(v[0], v[1]), D2D1::Point2F(v[2], v[3])));
                        a += 4;
                        break;
                    case SvgShape::Arc:
                        if (open)
                            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(v[5], v[6]), D2D1::SizeF(v[0], v[1]), v[2],
                                                          v[4] != 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                                          v[3] != 0 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                        a += 7;
                        break;
                    case SvgShape::Close:
                        if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        open = false;
                        break;
                }
            }
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (FAILED(sink->Close())) continue;
            s.geometry = g;
        }
        Matrix m = multiply(fit, Matrix{s.m[0], s.m[1], s.m[2], s.m[3], s.m[4], s.m[5]});
        D2D1::Matrix3x2F local(m[0], m[1], m[2], m[3], m[4], m[5]);
        rt->SetTransform(local * *D2D1::Matrix3x2F::ReinterpretBaseType(&around));
        if (s.fill) {
            br->SetColor(D2D1::ColorF(s.fillColor.r, s.fillColor.g, s.fillColor.b, s.fillColor.a));
            rt->FillGeometry(s.geometry.Get(), br);
        }
        if (s.stroke) {
            br->SetColor(D2D1::ColorF(s.strokeColor.r, s.strokeColor.g, s.strokeColor.b, s.strokeColor.a));
            rt->DrawGeometry(s.geometry.Get(), br, s.strokeWidth);
        }
    }
    rt->SetTransform(around);
    rt->PopAxisAlignedClip();
}

}  // namespace tb
