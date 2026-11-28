#include "css.h"
#include "url.h"
#include "anim.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace tb {

Node::Node() = default;
Node::~Node() = default;

StyleEngine::FontResolver StyleEngine::fontResolver = nullptr;

static constexpr auto npos = std::string_view::npos;
static float g_vw = 1024, g_vh = 768, g_rootFont = 16;

namespace {

bool in(std::string_view n, std::initializer_list<std::string_view> l) {
    for (auto x : l)
        if (x == n) return true;
    return false;
}

// Splits at `sep` outside of (), [] and quotes.
std::vector<std::string_view> split_top(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    int depth = 0;
    char q = 0;
    size_t st = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q) {
            if (c == '\\')
                ++i;
            else if (c == q)
                q = 0;
            continue;
        }
        if (c == '"' || c == '\'')
            q = c;
        else if (c == '(' || c == '[')
            ++depth;
        else if (c == ')' || c == ']') {
            if (depth > 0) --depth;
        } else if (c == sep && depth == 0) {
            out.push_back(s.substr(st, i - st));
            st = i + 1;
        }
    }
    out.push_back(s.substr(st));
    return out;
}

// Whitespace-separated component values, respecting parentheses and quotes.
std::vector<std::string> tokens(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char q = 0;
    for (char c : s) {
        if (q) {
            cur += c;
            if (c == q) q = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            q = c;
            cur += c;
            continue;
        }
        if (c == '(') ++depth;
        if (c == ')' && depth) --depth;
        if (isws(c) && depth == 0) {
            if (!cur.empty()) out.push_back(std::move(cur)), cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

bool parse_num(std::string_view s, float& v, std::string_view& unit) {
    std::string tmp(s);
    if (tmp.empty()) return false;
    char c0 = tmp[0];
    if (!(isdigit_(c0) || c0 == '.' || c0 == '-' || c0 == '+')) return false;
    char* end = nullptr;
    v = std::strtof(tmp.c_str(), &end);
    if (end == tmp.c_str()) return false;
    unit = s.substr(end - tmp.c_str());
    return std::isfinite(v);
}

bool parse_len(std::string_view sv, float fs, Len& out, bool allowAuto);

// <angle> (CSS Values 4, 7.1) in radians; a plain 0 is allowed where the grammar says <zero>.
bool parse_angle(std::string_view t, double& rad) {  // double: cos(90deg) must come out as 6e-17, not 4e-8
    float v;
    std::string_view u;
    if (!parse_num(t, v, u)) return false;
    const double pi = 3.14159265358979323846;
    if (u == "deg") rad = v * pi / 180;
    else if (u == "rad") rad = v;
    else if (u == "grad") rad = v * pi / 200;
    else if (u == "turn") rad = v * 2 * pi;
    else if (u.empty() && v == 0) rad = 0;
    else return false;
    return true;
}

// <transform-list> (CSS Transforms 1, section 7; CSS Transforms 2 for the 3D functions). False when any function
// is malformed, which makes the whole declaration invalid.
bool parse_transform(std::string_view lv, float fs, TransformList& out) {
    size_t i = 0;
    while (i < lv.size()) {
        while (i < lv.size() && isws(lv[i])) ++i;
        if (i >= lv.size()) break;
        size_t open = lv.find('(', i), close = lv.find(')', i);
        if (open == npos || close == npos || close < open) return false;
        std::string name(trim(lv.substr(i, open - i)));
        std::vector<std::string> args;
        for (auto a : split_top(lv.substr(open + 1, close - open - 1), ',')) args.emplace_back(trim(a));
        i = close + 1;
        auto number = [&](size_t k, float& v) {
            std::string_view u;
            return k < args.size() && parse_num(args[k], v, u) && u.empty();
        };
        auto length = [&](size_t k, Len& l) { return k < args.size() && parse_len(args[k], fs, l, false) && (l.u == Len::Px || l.u == Len::Pct); };
        TransformFn f;
        size_t n = args.size();
        if (name == "matrix") {
            if (n != 6) return false;
            for (size_t k = 0; k < 6; ++k)
                if (!number(k, f.v[k])) return false;
        } else if (name == "translate" || name == "translatex" || name == "translatey" || name == "translate3d") {
            f.k = TransformFn::Translate;
            f.tx = f.ty = {Len::Px, 0};
            if (name == "translatey") {
                if (n != 1 || !length(0, f.ty)) return false;
            } else if (!length(0, f.tx) || (name == "translatex" && n != 1) || (name == "translate" && (n > 2 || (n == 2 && !length(1, f.ty)))) ||
                       (name == "translate3d" && (n != 3 || !length(1, f.ty)))) {
                return false;
            }
        } else if (name == "scale" || name == "scalex" || name == "scaley" || name == "scale3d") {
            f.k = TransformFn::Scale;
            float sx = 1, sy = 1;
            if (name == "scaley") {
                if (n != 1 || !number(0, sy)) return false;
            } else {
                if (!number(0, sx)) return false;
                sy = name == "scalex" ? 1 : sx;
                if ((name == "scalex" && n != 1) || (name == "scale" && n > 2) || (name == "scale3d" && n != 3)) return false;
                if (n >= 2 && name != "scalex" && !number(1, sy)) return false;
            }
            f.set_scale(sx, sy);
        } else if (name == "rotate" || name == "rotatez") {
            double a;
            if (n != 1 || !parse_angle(args[0], a)) return false;
            f.k = TransformFn::Rotate;
            f.set_rotate((float)a);
        } else if (name == "skew" || name == "skewx" || name == "skewy") {
            double ax = 0, ay = 0;
            if (name == "skewy") {
                if (n != 1 || !parse_angle(args[0], ay)) return false;
            } else if (n < 1 || n > (name == "skew" ? 2u : 1u) || !parse_angle(args[0], ax) || (n == 2 && !parse_angle(args[1], ay))) {
                return false;
            }
            f.k = TransformFn::Skew;
            f.set_skew((float)ax, (float)ay);
        } else if (name == "translatez" || name == "scalez" || name == "rotatex" || name == "rotatey" || name == "rotate3d" ||
                   name == "perspective" || name == "matrix3d") {
            continue;  // no 2D part that this engine draws
        } else {
            return false;
        }
        out.push_back(f);
    }
    return true;
}

}  // namespace

void transform_matrix(const TransformList& list, float width, float height, float out[6]) {
    float m[6] = {1, 0, 0, 1, 0, 0};
    for (const TransformFn& f : list) {
        float n[6];
        std::copy(f.v, f.v + 6, n);
        if (f.k == TransformFn::Translate) n[4] = f.tx.resolve(width), n[5] = f.ty.resolve(height);
        float r[6] = {m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3],
                      m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
        std::copy(r, r + 6, m);
    }
    std::copy(m, m + 6, out);
}

// ---- CSS Values 4, 10: math functions. An expression is parsed into a tree whose leaves are numbers, lengths in px
// (every absolute and relative unit is converted when the declaration is applied) and percentages. One without
// percentages is worked out at once; one whose percentages only add up (calc(100% - 2em)) becomes a percentage and an
// offset; any other keeps its tree, which Len::resolve evaluates against the percentage's basis.
struct MathExpr {
    enum K : uint8_t { Num, Px, Pct, Add, Sub, Mul, Div, Neg, Fn } k = Num;
    double v = 0;
    std::string fn;  // Fn: the function; for round() the rounding strategy goes in `arg`
    std::string arg;
    std::vector<std::shared_ptr<const MathExpr>> args;
};

namespace {
using MathPtr = std::shared_ptr<const MathExpr>;
MathPtr leaf(MathExpr::K k, double v) {
    auto e = std::make_shared<MathExpr>();
    e->k = k;
    e->v = v;
    return e;
}
MathPtr node(MathExpr::K k, MathPtr a, MathPtr b = nullptr) {
    auto e = std::make_shared<MathExpr>();
    e->k = k;
    e->args.push_back(std::move(a));
    if (b) e->args.push_back(std::move(b));
    return e;
}
bool is_math_function(std::string_view name) {
    static const char* const names[] = {"calc", "-webkit-calc", "min", "max", "clamp", "round", "mod", "rem", "abs", "sign", "sin",
                                        "cos", "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp"};
    for (const char* n : names)
        if (name == n) return true;
    return false;
}

struct MathParser {
    std::string_view s;
    size_t i = 0;
    float fs;
    bool pct = false;  // a percentage was met
    void ws() {
        while (i < s.size() && isws(s[i])) ++i;
    }
    MathPtr sum() {
        MathPtr a = product();
        while (a) {
            size_t save = i;
            bool spaced = i < s.size() && isws(s[i]);
            ws();
            if (i < s.size() && (s[i] == '+' || s[i] == '-') && spaced && i + 1 < s.size() && isws(s[i + 1])) {
                char op = s[i++];
                MathPtr b = product();
                if (!b) return nullptr;
                a = node(op == '+' ? MathExpr::Add : MathExpr::Sub, a, b);
            } else {
                i = save;
                break;
            }
        }
        return a;
    }
    MathPtr product() {
        MathPtr a = value();
        while (a) {
            size_t save = i;
            ws();
            if (i < s.size() && (s[i] == '*' || s[i] == '/')) {
                char op = s[i++];
                MathPtr b = value();
                if (!b) return nullptr;
                a = node(op == '*' ? MathExpr::Mul : MathExpr::Div, a, b);
            } else {
                i = save;
                break;
            }
        }
        return a;
    }
    MathPtr value() {
        ws();
        if (i >= s.size()) return nullptr;
        if (s[i] == '(') {
            ++i;
            MathPtr e = sum();
            ws();
            if (!e || i >= s.size() || s[i] != ')') return nullptr;
            ++i;
            return e;
        }
        size_t st = i;
        if (isalpha((unsigned char)s[i]) || (s[i] == '-' && i + 1 < s.size() && isalpha((unsigned char)s[i + 1]))) {
            while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_')) ++i;
            std::string name = lower(s.substr(st, i - st));
            if (i < s.size() && s[i] == '(') {
                ++i;
                if (!is_math_function(name)) return nullptr;
                auto f = std::make_shared<MathExpr>();
                f->k = MathExpr::Fn;
                f->fn = name == "-webkit-calc" ? "calc" : name;
                ws();
                if (name == "round") {  // an optional rounding strategy first
                    size_t k = i;
                    while (k < s.size() && isalpha((unsigned char)s[k]) || (k < s.size() && s[k] == '-')) ++k;
                    std::string word = lower(s.substr(i, k - i));
                    if (word == "nearest" || word == "up" || word == "down" || word == "to-zero") {
                        f->arg = word;
                        i = k;
                        ws();
                        if (i < s.size() && s[i] == ',') ++i;
                    }
                }
                for (;;) {
                    MathPtr a = sum();
                    if (!a) return nullptr;
                    f->args.push_back(a);
                    ws();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == ')') {
                        ++i;
                        break;
                    }
                    return nullptr;
                }
                return f;
            }
            if (name == "e") return leaf(MathExpr::Num, 2.718281828459045);
            if (name == "pi") return leaf(MathExpr::Num, 3.141592653589793);
            if (name == "infinity") return leaf(MathExpr::Num, INFINITY);
            if (name == "-infinity") return leaf(MathExpr::Num, -INFINITY);
            if (name == "nan") return leaf(MathExpr::Num, NAN);
            return nullptr;
        }
        // a number, with a unit or a percent sign
        if (s[i] == '+' || s[i] == '-') ++i;
        bool digits = false;
        while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.')) ++i, digits = true;
        if (!digits) return nullptr;
        if (i + 1 < s.size() && (s[i] == 'e' || s[i] == 'E') && (isdigit((unsigned char)s[i + 1]) || ((s[i + 1] == '-' || s[i + 1] == '+') && i + 2 < s.size() && isdigit((unsigned char)s[i + 2])))) {
            i += 2;
            while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
        }
        double n = atof(std::string(s.substr(st, i - st)).c_str());
        size_t u0 = i;
        if (i < s.size() && s[i] == '%') {
            ++i;
            pct = true;
            return leaf(MathExpr::Pct, n);
        }
        while (i < s.size() && isalpha((unsigned char)s[i])) ++i;
        std::string unit = lower(s.substr(u0, i - u0));
        if (unit.empty()) return leaf(MathExpr::Num, n);
        static const double pi = 3.141592653589793;
        if (unit == "deg") return leaf(MathExpr::Num, n * pi / 180);  // angles as radians, for the trigonometric functions
        if (unit == "rad") return leaf(MathExpr::Num, n);
        if (unit == "grad") return leaf(MathExpr::Num, n * pi / 200);
        if (unit == "turn") return leaf(MathExpr::Num, n * 2 * pi);
        Len l;
        if (!parse_len(std::to_string(n) + unit, fs, l, false) || l.u != Len::Px) return nullptr;
        return leaf(MathExpr::Px, l.v);
    }
};

double eval_node(const MathExpr& e, double base) {
    auto a = [&](size_t k) { return k < e.args.size() ? eval_node(*e.args[k], base) : 0.0; };
    switch (e.k) {
        case MathExpr::Num:
        case MathExpr::Px: return e.v;
        case MathExpr::Pct: return e.v * base / 100;
        case MathExpr::Add: return a(0) + a(1);
        case MathExpr::Sub: return a(0) - a(1);
        case MathExpr::Mul: return a(0) * a(1);
        case MathExpr::Div: return a(0) / a(1);
        case MathExpr::Neg: return -a(0);
        case MathExpr::Fn: break;
    }
    const std::string& f = e.fn;
    size_t n = e.args.size();
    if (f == "calc") return a(0);
    if (f == "min" || f == "max") {
        double r = a(0);
        for (size_t k = 1; k < n; ++k) r = f == "min" ? std::min(r, a(k)) : std::max(r, a(k));
        return r;
    }
    if (f == "clamp") return std::max(a(0), std::min(a(1), a(2)));
    if (f == "round") {
        double v = a(0), step = n > 1 ? a(1) : 1;
        if (step == 0) return NAN;
        double q = v / step;
        double r = e.arg == "up" ? std::ceil(q) : e.arg == "down" ? std::floor(q) : e.arg == "to-zero" ? std::trunc(q) : std::floor(q + 0.5);
        return r * step;
    }
    if (f == "mod") {  // the sign of the divisor
        double b = a(1), r = std::fmod(a(0), b);
        return r != 0 && (r < 0) != (b < 0) ? r + b : r;
    }
    if (f == "rem") return std::fmod(a(0), a(1));
    if (f == "abs") return std::fabs(a(0));
    if (f == "sign") return a(0) > 0 ? 1 : a(0) < 0 ? -1 : 0;
    if (f == "sin") return std::sin(a(0));
    if (f == "cos") return std::cos(a(0));
    if (f == "tan") return std::tan(a(0));
    if (f == "asin") return std::asin(a(0));
    if (f == "acos") return std::acos(a(0));
    if (f == "atan") return std::atan(a(0));
    if (f == "atan2") return std::atan2(a(0), a(1));
    if (f == "pow") return std::pow(a(0), a(1));
    if (f == "sqrt") return std::sqrt(a(0));
    if (f == "hypot") {
        double r = 0;
        for (size_t k = 0; k < n; ++k) r += a(k) * a(k);
        return std::sqrt(r);
    }
    if (f == "log") return n > 1 ? std::log(a(0)) / std::log(a(1)) : std::log(a(0));
    if (f == "exp") return std::exp(a(0));
    return NAN;
}
// The expression as percentage + offset, when it is that simple: sums, and products and quotients by plain numbers
bool linear(const MathExpr& e, double& pct, double& px, bool& constant) {
    switch (e.k) {
        case MathExpr::Num:
        case MathExpr::Px: pct = 0, px = e.v, constant = true; return true;
        case MathExpr::Pct: pct = e.v, px = 0, constant = false; return true;
        case MathExpr::Add:
        case MathExpr::Sub:
        case MathExpr::Mul:
        case MathExpr::Div: {
            double p1, x1, p2, x2;
            bool c1, c2;
            if (!linear(*e.args[0], p1, x1, c1) || !linear(*e.args[1], p2, x2, c2)) return false;
            double sign = e.k == MathExpr::Sub ? -1 : 1;
            if (e.k == MathExpr::Add || e.k == MathExpr::Sub) {
                pct = p1 + sign * p2, px = x1 + sign * x2, constant = c1 && c2;
                return true;
            }
            if (e.k == MathExpr::Mul && (c1 || c2)) {
                double k = c1 ? x1 : x2;
                pct = (c1 ? p2 : p1) * k, px = (c1 ? x2 : x1) * k, constant = c1 && c2;
                return true;
            }
            if (e.k == MathExpr::Div && c2 && x2 != 0) {
                pct = p1 / x2, px = x1 / x2, constant = c1;
                return true;
            }
            return false;
        }
        case MathExpr::Fn:
            if (e.fn == "calc" && e.args.size() == 1) return linear(*e.args[0], pct, px, constant);
            return false;
        default: return false;
    }
}
// A clamp to what a float holds: infinities in lengths become very large ones (CSS Values 4, 10.9)
float finite(double v) { return std::isnan(v) ? 0.f : (float)std::clamp(v, -1e7, 1e7); }
}  // namespace

double eval_math(const MathExpr& e, float base) { return finite(eval_node(e, base)); }

// A math function as a length: worked out now, a percentage and an offset, or kept to be evaluated later
bool parse_math_len(std::string_view v, float fs, Len& out) {
    MathParser p{v, 0, fs};
    MathPtr e = p.value();
    p.ws();
    if (!e || p.i != v.size() || e->k != MathExpr::Fn) return false;
    double pct, px;
    bool constant;
    if (!p.pct) out = {Len::Px, finite(eval_node(*e, 0))};
    else if (linear(*e, pct, px, constant)) out = {Len::Pct, finite(pct), finite(px)};
    else {
        out = {Len::Pct, 0, 0};
        out.expr = e;
    }
    return true;
}

namespace {

bool parse_len(std::string_view sv, float fs, Len& out, bool allowAuto) {
    std::string s = lower(trim(sv));
    if (s.empty()) return false;
    if (s == "auto" || s == "fit-content" || s == "max-content" || s == "min-content") {
        if (!allowAuto) return false;
        out = {Len::Auto, 0};
        return true;
    }
    if (s == "none") {
        out = {Len::None, 0};
        return true;
    }
    if (size_t open = s.find('('); open != std::string::npos && is_math_function(std::string_view(s).substr(0, open)))
        return parse_math_len(s, fs, out);
    float v;
    std::string_view u;
    if (!parse_num(s, v, u)) return false;
    if (u.empty()) {
        if (v == 0) {
            out = {Len::Px, 0};
            return true;
        }
        return false;
    }
    if (u == "px")
        out = {Len::Px, v};
    else if (u == "%")
        out = {Len::Pct, v};
    else if (u == "em")
        out = {Len::Px, v * fs};
    else if (u == "rem")
        out = {Len::Px, v * g_rootFont};
    else if (u == "pt")
        out = {Len::Px, v * 4.f / 3.f};
    else if (u == "pc")
        out = {Len::Px, v * 16.f};
    else if (u == "in")
        out = {Len::Px, v * 96.f};
    else if (u == "cm")
        out = {Len::Px, v * 37.795f};
    else if (u == "mm")
        out = {Len::Px, v * 3.7795f};
    else if (u == "vw" || u == "dvw" || u == "svw" || u == "lvw")
        out = {Len::Px, v * g_vw / 100.f};
    else if (u == "vh" || u == "dvh" || u == "svh" || u == "lvh")
        out = {Len::Px, v * g_vh / 100.f};
    else if (u == "vmin")
        out = {Len::Px, v * std::min(g_vw, g_vh) / 100.f};
    else if (u == "vmax")
        out = {Len::Px, v * std::max(g_vw, g_vh) / 100.f};
    else if (u == "ex" || u == "ch")
        out = {Len::Px, v * fs * 0.5f};
    else if (u == "q")
        out = {Len::Px, v * 37.795f / 40.f};
    else if (u == "lh" || u == "rlh")
        out = {Len::Px, v * fs * 1.2f};
    else
        return false;
    return true;
}

struct NamedColor {
    const char* n;
    uint32_t v;
};
const NamedColor kColors[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4},
    {"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000},
    {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
    {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
    {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc},
    {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b},
    {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1},
    {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969},
    {"dimgrey", 0x696969}, {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0},
    {"forestgreen", 0x228b22}, {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff},
    {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080}, {"grey", 0x808080}, {"green", 0x008000},
    {"greenyellow", 0xadff2f}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa},
    {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6},
    {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2},
    {"lightgray", 0xd3d3d3}, {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1},
    {"lightsalmon", 0xffa07a}, {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa},
    {"lightslategray", 0x778899}, {"lightslategrey", 0x778899}, {"lightsteelblue", 0xb0c4de},
    {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32}, {"linen", 0xfaf0e6},
    {"magenta", 0xff00ff}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd},
    {"mediumorchid", 0xba55d3}, {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371},
    {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa},
    {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead}, {"navy", 0x000080},
    {"oldlace", 0xfdf5e6}, {"olive", 0x808000}, {"olivedrab", 0x6b8e23}, {"orange", 0xffa500},
    {"orangered", 0xff4500}, {"orchid", 0xda70d6}, {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98},
    {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5},
    {"peachpuff", 0xffdab9}, {"peru", 0xcd853f}, {"pink", 0xffc0cb}, {"plum", 0xdda0dd},
    {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xff0000},
    {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072},
    {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee}, {"sienna", 0xa0522d},
    {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd}, {"slategray", 0x708090},
    {"slategrey", 0x708090}, {"snow", 0xfffafa}, {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4},
    {"tan", 0xd2b48c}, {"teal", 0x008080}, {"thistle", 0xd8bfd8}, {"tomato", 0xff6347},
    {"turquoise", 0x40e0d0}, {"violet", 0xee82ee}, {"wheat", 0xf5deb3}, {"white", 0xffffff},
    {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
    // CSS system colors
    {"canvas", 0xffffff}, {"canvastext", 0x000000}, {"linktext", 0x0000ee}, {"visitedtext", 0x551a8b},
    {"buttonface", 0xefefef}, {"buttontext", 0x000000}, {"buttonborder", 0x767676}, {"field", 0xffffff},
    {"fieldtext", 0x000000}, {"graytext", 0x808080}, {"highlight", 0x0078d7}, {"highlighttext", 0xffffff},
    {"window", 0xffffff}, {"windowtext", 0x000000},
};

float hue2rgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}

}  // namespace

// ---- CSS Color 4 and 5: the colour spaces beyond sRGB, all brought to sRGB in the end (clipped to its gamut)
namespace {
struct Vec3 {
    double x, y, z;
};
Vec3 mul(const double m[9], Vec3 v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z};
}
double srgbToLinear(double c) {
    double a = std::fabs(c);
    return (a <= 0.04045 ? c / 12.92 : std::copysign(std::pow((a + 0.055) / 1.055, 2.4), c));
}
double linearToSrgb(double c) {
    double a = std::fabs(c);
    return a <= 0.0031308 ? c * 12.92 : std::copysign(1.055 * std::pow(a, 1 / 2.4) - 0.055, c);
}
// the matrices of CSS Color 4, section 18 (sample code)
const double kLinSrgbToXyz65[9] = {0.41239079926595934, 0.357584339383878, 0.1804807884018343, 0.21263900587151027, 0.715168678767756,
                                   0.07219231536073371, 0.01933081871559182, 0.11919477979462598, 0.9505321522496607};
const double kXyz65ToLinSrgb[9] = {3.2409699419045226, -1.537383177570094, -0.4986107602930034, -0.9692436362808796, 1.8759675015077202,
                                   0.04155505740717559, 0.05563007969699366, -0.20397695888897652, 1.0569715142428786};
const double kD50ToD65[9] = {0.955473421488075, -0.02309845494876471, 0.06325924320057072, -0.0283697093338637, 1.0099953980813041,
                             0.021041441191917323, 0.012314014864481998, -0.020507649298898964, 1.330365926242124};
const double kD65ToD50[9] = {1.0479297925449969, 0.022946870601609652, -0.05019226628920524, 0.02962780877005599, 0.9904344267538799,
                             -0.017073799063418826, -0.009243040646204504, 0.015055191490298152, 0.7518742814281371};
const double kLinP3ToXyz65[9] = {0.4865709486482162, 0.26566769316909306, 0.1982172852343625, 0.2289745640697488, 0.6917385218365064,
                                 0.079286914093745, 0.0, 0.04511338185890264, 1.043944368900976};
const double kLin2020ToXyz65[9] = {0.6369580483012914, 0.14461690358620832, 0.1688809751641721, 0.2627002120112671, 0.6779980715188708,
                                   0.05930171646986196, 0.0, 0.028072693049087428, 1.060985057710791};
const double kOklabToLms[9] = {1.0, 0.3963377773761749, 0.2158037573099136, 1.0, -0.1055613458156586, -0.0638541728258133,
                               1.0, -0.0894841775298119, -1.2914855480194092};
const double kLmsToXyz65[9] = {1.2268798758459243, -0.5578149944602171, 0.2813910456659647, -0.0405757452148008, 1.1122868032803170,
                               -0.0717110580655164, -0.0763729366746601, -0.4214933324022432, 1.5869240198367816};
const double kXyz65ToLms[9] = {0.8190224379967030, 0.3619062600528904, -0.1288737815209879, 0.0329836539323885, 0.9292868615863434,
                               0.0361446663506424, 0.0481771893596242, 0.2642395317527308, 0.6335478284694309};
const double kLmsToOklab[9] = {0.2104542683093140, 0.7936177747023054, -0.0040720430116193, 1.9779985324311684, -2.4285922420485799,
                               0.4505937096174110, 0.0259040424655478, 0.7827717124575296, -0.8086757549230774};
const double kD50White[3] = {0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};

Vec3 labToXyz50(Vec3 lab) {
    const double k = 24389.0 / 27, e = 216.0 / 24389;
    double f1 = (lab.x + 16) / 116, f0 = lab.y / 500 + f1, f2 = f1 - lab.z / 200;
    double x = std::pow(f0, 3) > e ? std::pow(f0, 3) : (116 * f0 - 16) / k;
    double y = lab.x > k * e ? std::pow((lab.x + 16) / 116, 3) : lab.x / k;
    double z = std::pow(f2, 3) > e ? std::pow(f2, 3) : (116 * f2 - 16) / k;
    return {x * kD50White[0], y * kD50White[1], z * kD50White[2]};
}
Vec3 xyz50ToLab(Vec3 xyz) {
    const double k = 24389.0 / 27, e = 216.0 / 24389;
    double v[3] = {xyz.x / kD50White[0], xyz.y / kD50White[1], xyz.z / kD50White[2]};
    double f[3];
    for (int i = 0; i < 3; ++i) f[i] = v[i] > e ? std::cbrt(v[i]) : (k * v[i] + 16) / 116;
    return {116 * f[1] - 16, 500 * (f[0] - f[1]), 200 * (f[1] - f[2])};
}
Vec3 oklabToXyz65(Vec3 lab) {
    Vec3 lms = mul(kOklabToLms, lab);
    return mul(kLmsToXyz65, {lms.x * lms.x * lms.x, lms.y * lms.y * lms.y, lms.z * lms.z * lms.z});
}
Vec3 xyz65ToOklab(Vec3 xyz) {
    Vec3 lms = mul(kXyz65ToLms, xyz);
    return mul(kLmsToOklab, {std::cbrt(lms.x), std::cbrt(lms.y), std::cbrt(lms.z)});
}
Vec3 polarToLab(Vec3 lch) {  // L, C, H in degrees to L, a, b
    double h = lch.z * 3.14159265358979323846 / 180;
    return {lch.x, lch.y * std::cos(h), lch.y * std::sin(h)};
}
Vec3 labToPolar(Vec3 lab) {
    double h = std::atan2(lab.z, lab.y) * 180 / 3.14159265358979323846;
    return {lab.x, std::sqrt(lab.y * lab.y + lab.z * lab.z), h < 0 ? h + 360 : h};
}
Vec3 xyz65FromSrgb(const Color& c) { return mul(kLinSrgbToXyz65, {srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)}); }
Color srgbFromXyz65(Vec3 xyz, float alpha) {
    Vec3 lin = mul(kXyz65ToLinSrgb, xyz);
    auto ch = [](double v) { return (float)std::clamp(linearToSrgb(v), 0.0, 1.0); };
    return {ch(lin.x), ch(lin.y), ch(lin.z), alpha};
}

// A hue: a number of degrees or an angle
bool parse_hue(const std::string& t, double& deg) {
    float v;
    std::string_view u;
    if (!parse_num(t, v, u)) return false;
    if (u.empty() || u == "deg") deg = v;
    else if (u == "grad") deg = v * 0.9;
    else if (u == "rad") deg = v * 180 / 3.14159265358979323846;
    else if (u == "turn") deg = v * 360;
    else return false;
    return true;
}
// The channels of a colour function: up to three values (none is 0), and the alpha after a slash
bool color_channels(std::string_view inner, std::vector<std::string>& ch, float& alpha) {
    std::string s(inner);
    size_t slash = s.find('/');
    std::string a = slash == std::string::npos ? std::string() : s.substr(slash + 1);
    if (slash != std::string::npos) s = s.substr(0, slash);
    for (auto& c : s)
        if (c == ',') c = ' ';
    ch = tokens(s);
    alpha = 1;
    if (!trim(a).empty()) {
        float v;
        std::string_view u;
        std::string at = lower(trim(a));
        if (at == "none") alpha = 0;
        else if (!parse_num(at, v, u)) return false;
        else alpha = std::clamp(u == "%" ? v / 100 : v, 0.f, 1.f);
    }
    return true;
}
// A channel as a number: a percentage maps 100% to `full`
bool channel(const std::string& t, double full, double& out) {
    if (t == "none") {
        out = 0;
        return true;
    }
    float v;
    std::string_view u;
    if (!parse_num(t, v, u)) return false;
    if (u == "%") out = v / 100.0 * full;
    else if (u.empty()) out = v;
    else return false;
    return true;
}

// The colour spaces color-mix() can interpolate in
enum class MixSpace { Srgb, SrgbLinear, Lab, Oklab, Xyz, Lch, Oklch, Hsl, Hwb };
Vec3 toSpace(MixSpace sp, const Color& c);
Color fromSpace(MixSpace sp, Vec3 v, float alpha);
}  // namespace

// lab(), lch(), oklab(), oklch(), hwb() and color() (CSS Color 4, 8-10), color-mix() (CSS Color 5, 2)
bool parse_color_function(const std::string& s, Color& out) {
    size_t open = s.find('(');
    if (open == std::string::npos || s.back() != ')') return false;
    std::string fn = s.substr(0, open), inner = s.substr(open + 1, s.size() - open - 2);
    if (fn == "color-mix") {
        auto args = split_top(inner, ',');
        if (args.size() != 3) return false;
        auto how = tokens(lower(trim(args[0])));
        if (how.size() < 2 || how[0] != "in") return false;
        static const std::pair<const char*, MixSpace> spaces[] = {
            {"srgb", MixSpace::Srgb}, {"srgb-linear", MixSpace::SrgbLinear}, {"lab", MixSpace::Lab}, {"oklab", MixSpace::Oklab},
            {"xyz", MixSpace::Xyz}, {"xyz-d65", MixSpace::Xyz}, {"xyz-d50", MixSpace::Xyz}, {"lch", MixSpace::Lch},
            {"oklch", MixSpace::Oklch}, {"hsl", MixSpace::Hsl}, {"hwb", MixSpace::Hwb}};
        MixSpace sp{};
        bool found = false;
        for (auto& [n, v] : spaces)
            if (how[1] == n) sp = v, found = true;
        if (!found) return false;
        std::string hueMethod = how.size() >= 4 ? how[2] : "shorter";
        Color c[2];
        double p[2] = {-1, -1};
        for (int i = 0; i < 2; ++i) {  // <color> && <percentage>?
            auto T = tokens(trim(args[i + 1]));
            std::string color;
            for (auto& t : T) {
                float v;
                std::string_view u;
                if (parse_num(t, v, u) && u == "%") p[i] = v;
                else color += (color.empty() ? "" : " ") + t;
            }
            if (!parse_color(color, c[i])) return false;
        }
        if (p[0] < 0 && p[1] < 0) p[0] = p[1] = 50;
        else if (p[0] < 0) p[0] = 100 - p[1];
        else if (p[1] < 0) p[1] = 100 - p[0];
        double sum = p[0] + p[1];
        if (sum <= 0) return false;
        double t = p[1] / sum, alphaScale = std::min(1.0, sum / 100);
        Vec3 a = toSpace(sp, c[0]), b = toSpace(sp, c[1]);
        bool polar = sp == MixSpace::Lch || sp == MixSpace::Oklch || sp == MixSpace::Hsl || sp == MixSpace::Hwb;
        // premultiplied by alpha (CSS Color 4, 12.3); the hue is not
        auto pre = [&](Vec3 v, float al) {
            if (polar) return sp == MixSpace::Hsl || sp == MixSpace::Hwb ? Vec3{v.x, v.y * al, v.z * al} : Vec3{v.x * al, v.y * al, v.z};
            return Vec3{v.x * al, v.y * al, v.z * al};
        };
        float al = (float)(c[0].a * (1 - t) + c[1].a * t);
        Vec3 pa = pre(a, c[0].a), pb = pre(b, c[1].a);
        Vec3 m{pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t, pa.z + (pb.z - pa.z) * t};
        if (polar) {  // the hue (x for hsl and hwb, z for lch and oklch) goes the way the method says (12.4)
            double h1 = sp == MixSpace::Hsl || sp == MixSpace::Hwb ? a.x : a.z, h2 = sp == MixSpace::Hsl || sp == MixSpace::Hwb ? b.x : b.z;
            double d = h2 - h1;
            if (hueMethod == "shorter") {
                if (d > 180) h1 += 360;
                else if (d < -180) h2 += 360;
            } else if (hueMethod == "longer") {
                if (0 < d && d < 180) h1 += 360;
                else if (-180 < d && d <= 0) h2 += 360;
            } else if (hueMethod == "increasing" && d < 0) h2 += 360;
            else if (hueMethod == "decreasing" && d > 0) h1 += 360;
            double h = std::fmod(h1 + (h2 - h1) * t, 360.0);
            if (sp == MixSpace::Hsl || sp == MixSpace::Hwb) m.x = h;
            else m.z = h;
        }
        if (al > 0) {
            if (polar && (sp == MixSpace::Hsl || sp == MixSpace::Hwb)) m = {m.x, m.y / al, m.z / al};
            else if (polar) m = {m.x / al, m.y / al, m.z};
            else m = {m.x / al, m.y / al, m.z / al};
        }
        out = fromSpace(sp, m, (float)(al * alphaScale));
        return true;
    }
    std::vector<std::string> ch;
    float alpha;
    if (!color_channels(inner, ch, alpha)) return false;
    if (fn == "color") {  // color(<space> c1 c2 c3 [/ alpha]?)
        if (ch.size() != 4) return false;
        double v[3];
        for (int i = 0; i < 3; ++i)
            if (!channel(ch[i + 1], 1, v[i])) return false;
        const std::string& space = ch[0];
        Vec3 xyz;
        if (space == "srgb" || space == "srgb-linear" || space == "display-p3" || space == "rec2020") {
            Vec3 lin = space == "srgb-linear" ? Vec3{v[0], v[1], v[2]} : Vec3{srgbToLinear(v[0]), srgbToLinear(v[1]), srgbToLinear(v[2])};
            if (space == "rec2020") {  // its own transfer function
                auto f = [](double c) {
                    const double a = 1.09929682680944, b = 0.018053968510807;
                    double x = std::fabs(c);
                    return std::copysign(x < b * 4.5 ? x / 4.5 : std::pow((x + a - 1) / a, 1 / 0.45), c);
                };
                lin = {f(v[0]), f(v[1]), f(v[2])};
            }
            xyz = space == "display-p3" ? mul(kLinP3ToXyz65, lin) : space == "rec2020" ? mul(kLin2020ToXyz65, lin) : mul(kLinSrgbToXyz65, lin);
        } else if (space == "xyz" || space == "xyz-d65") {
            xyz = {v[0], v[1], v[2]};
        } else if (space == "xyz-d50") {
            xyz = mul(kD50ToD65, {v[0], v[1], v[2]});
        } else
            return false;
        out = srgbFromXyz65(xyz, alpha);
        return true;
    }
    if (ch.size() != 3) return false;
    double a, b, c;
    if (fn == "lab" || fn == "lch") {
        if (!channel(ch[0], 100, a)) return false;
        if (fn == "lab") {
            if (!channel(ch[1], 125, b) || !channel(ch[2], 125, c)) return false;
        } else if (!channel(ch[1], 150, b) || !parse_hue(ch[2] == "none" ? "0" : ch[2], c))
            return false;
        Vec3 lab = fn == "lab" ? Vec3{a, b, c} : polarToLab({a, b, c});
        out = srgbFromXyz65(mul(kD50ToD65, labToXyz50(lab)), alpha);
        return true;
    }
    if (fn == "oklab" || fn == "oklch") {
        if (!channel(ch[0], 1, a)) return false;
        if (fn == "oklab") {
            if (!channel(ch[1], 0.4, b) || !channel(ch[2], 0.4, c)) return false;
        } else if (!channel(ch[1], 0.4, b) || !parse_hue(ch[2] == "none" ? "0" : ch[2], c))
            return false;
        Vec3 lab = fn == "oklab" ? Vec3{a, b, c} : polarToLab({a, b, c});
        out = srgbFromXyz65(oklabToXyz65(lab), alpha);
        return true;
    }
    if (fn == "hwb") {
        double h, w, bl;
        if (!parse_hue(ch[0] == "none" ? "0" : ch[0], h) || !channel(ch[1], 100, w) || !channel(ch[2], 100, bl)) return false;
        out = fromSpace(MixSpace::Hwb, {h, w, bl}, alpha);
        return true;
    }
    return false;
}

namespace {
Vec3 toSpace(MixSpace sp, const Color& c) {
    switch (sp) {
        case MixSpace::Srgb: return {c.r, c.g, c.b};
        case MixSpace::SrgbLinear: return {srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)};
        case MixSpace::Xyz: return xyz65FromSrgb(c);
        case MixSpace::Lab: return xyz50ToLab(mul(kD65ToD50, xyz65FromSrgb(c)));
        case MixSpace::Lch: return labToPolar(xyz50ToLab(mul(kD65ToD50, xyz65FromSrgb(c))));
        case MixSpace::Oklab: return xyz65ToOklab(xyz65FromSrgb(c));
        case MixSpace::Oklch: return labToPolar(xyz65ToOklab(xyz65FromSrgb(c)));
        case MixSpace::Hsl:
        case MixSpace::Hwb: {
            double mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn, h = 0;
            if (d > 0) {
                if (mx == c.r) h = std::fmod((c.g - c.b) / d, 6.0);
                else if (mx == c.g) h = (c.b - c.r) / d + 2;
                else h = (c.r - c.g) / d + 4;
                h *= 60;
                if (h < 0) h += 360;
            }
            if (sp == MixSpace::Hwb) return {h, mn * 100, (1 - mx) * 100};
            double l = (mx + mn) / 2, s = d == 0 ? 0 : d / (1 - std::fabs(2 * l - 1));
            return {h, s * 100, l * 100};
        }
    }
    return {};
}
Color fromSpace(MixSpace sp, Vec3 v, float alpha) {
    auto clip = [](double x) { return (float)std::clamp(x, 0.0, 1.0); };
    switch (sp) {
        case MixSpace::Srgb: return {clip(v.x), clip(v.y), clip(v.z), alpha};
        case MixSpace::SrgbLinear: return {clip(linearToSrgb(v.x)), clip(linearToSrgb(v.y)), clip(linearToSrgb(v.z)), alpha};
        case MixSpace::Xyz: return srgbFromXyz65(v, alpha);
        case MixSpace::Lab: return srgbFromXyz65(mul(kD50ToD65, labToXyz50(v)), alpha);
        case MixSpace::Lch: return srgbFromXyz65(mul(kD50ToD65, labToXyz50(polarToLab(v))), alpha);
        case MixSpace::Oklab: return srgbFromXyz65(oklabToXyz65(v), alpha);
        case MixSpace::Oklch: return srgbFromXyz65(oklabToXyz65(polarToLab(v)), alpha);
        case MixSpace::Hsl:
        case MixSpace::Hwb: {
            double h = std::fmod(v.x, 360.0);
            if (h < 0) h += 360;
            double w = v.y / 100, bl = v.z / 100, s = v.y / 100, l = v.z / 100;
            auto hslRgb = [&](double s, double l, double& r, double& g, double& b) {  // CSS Color 4, 7.1
                auto f = [&](double n) {
                    double k = std::fmod(n + h / 30, 12.0), a = s * std::min(l, 1 - l);
                    return l - a * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
                };
                r = f(0), g = f(8), b = f(4);
            };
            double r, g, b;
            if (sp == MixSpace::Hsl) hslRgb(s, l, r, g, b);
            else if (w + bl >= 1) r = g = b = w / (w + bl);
            else {  // CSS Color 4, 8.1
                hslRgb(1, 0.5, r, g, b);
                r = r * (1 - w - bl) + w;
                g = g * (1 - w - bl) + w;
                b = b * (1 - w - bl) + w;
            }
            return {clip(r), clip(g), clip(b), alpha};
        }
    }
    return {};
}
}  // namespace

bool parse_color(std::string_view sv, Color& out) {
    std::string s = lower(trim(sv));
    if (s.empty()) return false;
    if (s == "transparent") {
        out = {0, 0, 0, 0};
        return true;
    }
    if (s[0] == '#') {
        std::string h = s.substr(1);
        for (char c : h)
            if (!isdigit_(c) && !(c >= 'a' && c <= 'f')) return false;
        auto hv = [](char c) { return isdigit_(c) ? c - '0' : c - 'a' + 10; };
        if (h.size() == 3 || h.size() == 4) {
            out.r = hv(h[0]) * 17 / 255.f;
            out.g = hv(h[1]) * 17 / 255.f;
            out.b = hv(h[2]) * 17 / 255.f;
            out.a = h.size() == 4 ? hv(h[3]) * 17 / 255.f : 1.f;
            return true;
        }
        if (h.size() == 6 || h.size() == 8) {
            out.r = (hv(h[0]) * 16 + hv(h[1])) / 255.f;
            out.g = (hv(h[2]) * 16 + hv(h[3])) / 255.f;
            out.b = (hv(h[4]) * 16 + hv(h[5])) / 255.f;
            out.a = h.size() == 8 ? (hv(h[6]) * 16 + hv(h[7])) / 255.f : 1.f;
            return true;
        }
        return false;
    }
    if (istarts(s, "lab(") || istarts(s, "lch(") || istarts(s, "oklab(") || istarts(s, "oklch(") || istarts(s, "hwb(") ||
        istarts(s, "color(") || istarts(s, "color-mix("))
        return parse_color_function(s, out);
    if (istarts(s, "light-dark(")) {
        auto args = split_top(std::string_view(s).substr(11, s.size() > 12 ? s.size() - 12 : 0), ',');
        return !args.empty() && parse_color(args[0], out);
    }
    if (istarts(s, "rgb") || istarts(s, "hsl")) {
        size_t a = s.find('('), b = s.rfind(')');
        if (a == std::string::npos) return false;
        std::string inner = s.substr(a + 1, (b == std::string::npos ? s.size() : b) - a - 1);
        for (auto& c : inner)
            if (c == ',' || c == '/') c = ' ';
        auto T = tokens(inner);
        if (T.size() < 3) return false;
        float v[4] = {0, 0, 0, 1};
        bool pct[4] = {};
        for (size_t i = 0; i < T.size() && i < 4; ++i) {
            std::string_view u;
            if (T[i] == "none") {
                v[i] = 0;
                continue;
            }
            if (i == 0 && s[0] == 'h') {  // a hue: degrees, or an angle (CSS Color 4, 7.1)
                double deg;
                if (!parse_hue(T[i], deg)) return false;
                v[i] = (float)deg;
                continue;
            }
            if (!parse_num(T[i], v[i], u)) return false;
            pct[i] = u == "%";
        }
        if (T.size() >= 4) out.a = std::clamp(pct[3] ? v[3] / 100.f : v[3], 0.f, 1.f);
        else out.a = 1;
        if (s[0] == 'r') {
            for (int i = 0; i < 3; ++i) v[i] = std::clamp(pct[i] ? v[i] / 100.f : v[i] / 255.f, 0.f, 1.f);
            out.r = v[0];
            out.g = v[1];
            out.b = v[2];
        } else {
            float h = std::fmod(v[0], 360.f) / 360.f;
            if (h < 0) h += 1;
            float sa = std::clamp(v[1] / 100.f, 0.f, 1.f), l = std::clamp(v[2] / 100.f, 0.f, 1.f);
            if (sa == 0) {
                out.r = out.g = out.b = l;
            } else {
                float q = l < 0.5f ? l * (1 + sa) : l + sa - l * sa, p = 2 * l - q;
                out.r = hue2rgb(p, q, h + 1.f / 3);
                out.g = hue2rgb(p, q, h);
                out.b = hue2rgb(p, q, h - 1.f / 3);
            }
        }
        return true;
    }
    for (auto& c : kColors)
        if (s == c.n) {
            out = rgb(c.v);
            return true;
        }
    // system colours (CSS Color 4, 6.2), as a light page shows them
    static const std::pair<const char*, uint32_t> systemColors[] = {
        {"canvas", 0xffffff}, {"canvastext", 0x000000}, {"linktext", 0x0000ee}, {"visitedtext", 0x551a8b},
        {"activetext", 0xff0000}, {"buttonface", 0xefefef}, {"buttontext", 0x000000}, {"buttonborder", 0x767676},
        {"field", 0xffffff}, {"fieldtext", 0x000000}, {"highlight", 0x3390ff}, {"highlighttext", 0xffffff},
        {"selecteditem", 0x3390ff}, {"selecteditemtext", 0xffffff}, {"mark", 0xffff00}, {"marktext", 0x000000},
        {"graytext", 0x808080}, {"accentcolor", 0x0075ff}, {"accentcolortext", 0xffffff}};
    for (auto& [n, v] : systemColors)
        if (s == n) {
            out = rgb(v);
            return true;
        }
    return false;
}

bool replaced_like(const Node* n) {
    const std::string& nm = n->name;
    return nm == "img" || nm == "input" || nm == "select" || nm == "textarea" || nm == "svg" ||
           nm == "canvas" || nm == "video" || nm == "iframe" || nm == "object" || nm == "embed" || nm == "audio";
}

const char* display_name(Display d) {
    switch (d) {
        case Display::Inline: return "inline";
        case Display::Block: return "block";
        case Display::ListItem: return "list-item";
        case Display::InlineBlock: return "inline-block";
        case Display::None: return "none";
        case Display::Table: return "table";
        case Display::RowGroup: return "table-row-group";
        case Display::Row: return "table-row";
        case Display::Cell: return "table-cell";
        case Display::Flex: return "flex";
        case Display::InlineFlex: return "inline-flex";
        case Display::Grid: return "grid";
        case Display::InlineGrid: return "inline-grid";
    }
    return "?";
}

std::string color_str(const Color& c) {
    char buf[64];
    if (c.a >= 0.999f)
        snprintf(buf, sizeof buf, "#%02x%02x%02x", int(c.r * 255 + .5f), int(c.g * 255 + .5f), int(c.b * 255 + .5f));
    else
        snprintf(buf, sizeof buf, "rgba(%d,%d,%d,%.2f)", int(c.r * 255 + .5f), int(c.g * 255 + .5f), int(c.b * 255 + .5f), c.a);
    return buf;
}

// ============================================================================
// Stylesheet parsing
// ============================================================================
namespace {

std::string strip_comments(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    char q = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q) {
            r += c;
            if (c == '\\' && i + 1 < s.size()) {
                r += s[++i];
            } else if (c == q)
                q = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            q = c;
            r += c;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            size_t e = s.find("*/", i + 2);
            if (e == npos) break;
            i = e + 1;
            r += ' ';
            continue;
        }
        r += c;
    }
    return r;
}

size_t block_end(std::string_view s, size_t open) {
    int depth = 0;
    char q = 0;
    for (size_t i = open; i < s.size(); ++i) {
        char c = s[i];
        if (q) {
            if (c == '\\')
                ++i;
            else if (c == q)
                q = 0;
            continue;
        }
        if (c == '"' || c == '\'')
            q = c;
        else if (c == '{')
            ++depth;
        else if (c == '}') {
            if (--depth == 0) return i;
        }
    }
    return npos;
}

bool ident_char(char c) { return isalnum_(c) || c == '-' || c == '_' || (unsigned char)c >= 0x80; }

std::string read_ident(std::string_view s, size_t& i) {
    std::string r;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            r += s[i + 1];
            i += 2;
            continue;
        }
        if (!ident_char(c)) break;
        r += c;
        ++i;
    }
    return r;
}

bool parse_nth(std::string_view arg, int& a, int& b) {
    std::string s;
    for (char c : arg)
        if (!isws(c)) s += lc(c);
    size_t of = s.find("of");
    if (of != std::string::npos) s = s.substr(0, of);
    if (s == "odd") {
        a = 2;
        b = 1;
        return true;
    }
    if (s == "even") {
        a = 2;
        b = 0;
        return true;
    }
    size_t n = s.find('n');
    if (n == std::string::npos) {
        a = 0;
        b = atoi(s.c_str());
        return !s.empty();
    }
    std::string as = s.substr(0, n), bs = s.substr(n + 1);
    a = as.empty() || as == "+" ? 1 : as == "-" ? -1 : atoi(as.c_str());
    b = bs.empty() ? 0 : atoi(bs.c_str());
    return true;
}

bool parse_selector(std::string_view s, Selector& out);
bool parse_compound(std::string_view s, size_t& i, Compound& c, uint32_t& spec) {
    bool any = false;
    while (i < s.size()) {
        char ch = s[i];
        if (ch == '*') {
            ++i;
            any = true;
            continue;
        }
        if (ch == '|') {  // namespace prefix: ignore
            ++i;
            continue;
        }
        if (ident_char(ch) && !isdigit_(ch) && !any) {
            c.tag = lower(read_ident(s, i));
            spec += 1;
            any = true;
            continue;
        }
        if (ch == '#') {
            ++i;
            c.id = read_ident(s, i);
            spec += 1 << 20;
            any = true;
            continue;
        }
        if (ch == '.') {
            ++i;
            c.classes.push_back(read_ident(s, i));
            spec += 1 << 10;
            any = true;
            continue;
        }
        if (ch == '[') {
            size_t e = s.find(']', i);
            if (e == npos) return false;
            std::string_view body = trim(s.substr(i + 1, e - i - 1));
            i = e + 1;
            AttrSel a;
            size_t k = 0;
            while (k < body.size() && !in(std::string_view(&body[k], 1), {"=", "~", "|", "^", "$", "*"}) && !isws(body[k])) ++k;
            a.name = lower(body.substr(0, k));
            while (k < body.size() && isws(body[k])) ++k;
            if (k < body.size()) {
                char op = body[k];
                if (op == '=') {
                    a.op = '=';
                    ++k;
                } else if (k + 1 < body.size() && body[k + 1] == '=') {
                    a.op = op;
                    k += 2;
                } else
                    return false;
                std::string_view v = trim(body.substr(k));
                if (v.size() >= 2 && (v.back() == 'i' || v.back() == 's') && isws(v[v.size() - 2])) v = trim(v.substr(0, v.size() - 1));
                if (!v.empty() && (v[0] == '"' || v[0] == '\'')) v = v.substr(1, v.size() >= 2 ? v.size() - 2 : 0);
                a.val = std::string(v);
            }
            c.attrs.push_back(std::move(a));
            spec += 1 << 10;
            any = true;
            continue;
        }
        if (ch == ':') {
            ++i;
            bool elem = false;
            if (i < s.size() && s[i] == ':') {
                elem = true;
                ++i;
            }
            std::string name = lower(read_ident(s, i));
            std::string arg;
            if (i < s.size() && s[i] == '(') {
                int depth = 0;
                size_t st = i + 1;
                for (; i < s.size(); ++i) {
                    if (s[i] == '(') ++depth;
                    if (s[i] == ')' && --depth == 0) break;
                }
                arg = std::string(s.substr(st, i - st));
                if (i < s.size()) ++i;
            }
            any = true;
            if (name == "host" || name == "slotted") {  // CSS Scoping 3.2 and 3.3: the host from inside, and what its slots hold
                Pseudo hp;
                hp.k = name == "host" ? Pseudo::Host : Pseudo::Slotted;
                std::string_view a = trim(arg);
                if (!a.empty()) {
                    auto inner = std::make_shared<Compound>();
                    size_t k = 0;
                    uint32_t sp2 = 0;
                    if (!parse_compound(a, k, *inner, sp2) || k != a.size()) c.never = true;
                    hp.inner = inner;
                } else if (hp.k == Pseudo::Slotted)
                    c.never = true;
                spec += 1 << 10;
                c.pseudos.push_back(std::move(hp));
                continue;
            }
            if ((name == "before" || name == "after") && arg.empty()) {  // CSS 2 12.1, CSS Pseudo-Elements 4: with one colon or two
                if (c.pseudoElement) c.never = true;
                c.pseudoElement = name == "before" ? 1 : 2;
                spec += 1;
                continue;
            }
            if (elem || in(name, {"first-line", "first-letter", "selection", "placeholder", "marker", "backdrop", "file-selector-button"})) {
                c.never = true;
                continue;
            }
            if (c.pseudoElement) c.never = true;  // a pseudo-class after a pseudo-element is not supported
            spec += 1 << 10;
            Pseudo p;
            if (name == "first-child")
                p.k = Pseudo::FirstChild;
            else if (name == "last-child")
                p.k = Pseudo::LastChild;
            else if (name == "only-child")
                p.k = Pseudo::OnlyChild;
            else if (name == "root")
                p.k = Pseudo::Root;
            else if (name == "link" || name == "any-link")
                p.k = Pseudo::Link;
            else if (name == "empty")
                p.k = Pseudo::Empty;
            else if (name == "first-of-type")
                p.k = Pseudo::FirstOfType;
            else if (name == "last-of-type")
                p.k = Pseudo::LastOfType;
            else if (name == "nth-child" || name == "nth-last-child") {
                p.k = name == "nth-last-child" ? Pseudo::NthLastChild : Pseudo::NthChild;
                if (!parse_nth(arg, p.a, p.b)) c.never = true;
            } else if (name == "not" || name == "is" || name == "where" || name == "matches" || name == "-webkit-any" || name == "has") {
                // a selector list (Selectors 4, 4.2-4.5); :is() and :not() count as their most specific argument,
                // :where() as nothing. An argument that does not parse drops out of :is() and :where().
                auto list = std::make_shared<std::vector<Selector>>();
                uint32_t most = 0;
                bool bad = false;
                for (auto part : split_top(arg, ',')) {
                    std::string_view a = trim(part);
                    char comb = ' ';
                    if (name == "has" && !a.empty() && (a[0] == '>' || a[0] == '+' || a[0] == '~')) {
                        comb = a[0];
                        a = trim(a.substr(1));
                    }
                    Selector one;
                    bool unknown = false;  // a compound with a part that is not understood never matches
                    bool parsed = parse_selector(a, one);
                    for (auto& part : one.parts) unknown = unknown || part.never;
                    if (!parsed || unknown || one.parts.back().pseudoElement) {
                        bad = true;
                        continue;
                    }
                    most = std::max(most, one.spec);
                    if (name == "has") {  // anchor the relative selector at the element :has() is tried on
                        Selector rel;
                        rel.parts.emplace_back().pseudos.push_back(Pseudo{Pseudo::HasAnchor});
                        rel.combs.push_back(comb);
                        for (auto& part : one.parts) rel.parts.push_back(std::move(part));
                        for (char k : one.combs) rel.combs.push_back(k);
                        one = std::move(rel);
                    }
                    list->push_back(std::move(one));
                }
                // :not() and :has() take only arguments that parse (their lists are not forgiving); :is() and :where()
                // drop the ones that do not, and match nothing when none is left
                if (list->empty() || (bad && (name == "not" || name == "has"))) {
                    c.never = true;
                    continue;
                }
                p.k = name == "not" ? Pseudo::Not : name == "has" ? Pseudo::Has : Pseudo::Is;
                p.list = list;
                spec -= 1 << 10;  // the pseudo-class itself counts as its argument does
                if (name != "where") spec += most;
            } else if (name == "defined") {
                p.k = Pseudo::Defined;
            } else if (name == "nth-of-type" || name == "nth-last-of-type") {
                p.k = name == "nth-of-type" ? Pseudo::NthOfType : Pseudo::NthLastOfType;
                if (!parse_nth(arg, p.a, p.b)) c.never = true;
            } else if (name == "only-of-type") {
                p.k = Pseudo::OnlyOfType;
            } else if (name == "hover") {
                p.k = Pseudo::Hover;
            } else if (name == "active") {
                p.k = Pseudo::Active;
            } else if (name == "focus") {
                p.k = Pseudo::Focus;
            } else if (name == "focus-within") {
                p.k = Pseudo::FocusWithin;
            } else if (name == "focus-visible") {
                p.k = Pseudo::FocusVisible;
            } else if (name == "checked") {
                p.k = Pseudo::Checked;
            } else if (name == "disabled") {
                p.k = Pseudo::Disabled;
            } else if (name == "enabled") {
                p.k = Pseudo::Enabled;
            } else if (name == "required") {
                p.k = Pseudo::Required;
            } else if (name == "optional") {
                p.k = Pseudo::Optional;
            } else if (name == "read-only") {
                p.k = Pseudo::ReadOnly;
            } else if (name == "read-write") {
                p.k = Pseudo::ReadWrite;
            } else if (name == "placeholder-shown") {
                p.k = Pseudo::PlaceholderShown;
            } else if (name == "target") {
                p.k = Pseudo::Target;
            } else if (name == "lang") {
                p.k = Pseudo::Lang;
                p.arg = lower(trim(arg));
                if (p.arg.size() >= 2 && (p.arg[0] == '"' || p.arg[0] == '\'')) p.arg = p.arg.substr(1, p.arg.size() - 2);
            } else if (in(name, {"scope", "dir", "valid", "in-range"})) {
                // treated as always true: there is no scoping root but the document, text runs left to right, and
                // form controls are not validated
                continue;
            } else if (in(name, {"visited", "indeterminate", "invalid", "out-of-range", "fullscreen", "modal", "popover-open", "playing",
                                 "paused", "autofill", "user-invalid", "picture-in-picture", "open", "closed"})) {
                p.k = Pseudo::Never;  // states that never hold here (a link is never reported as visited: Selectors 4, 8.1)
            } else {
                c.never = true;  // vendor and unknown pseudo-classes make the whole rule invalid
                continue;
            }
            c.pseudos.push_back(std::move(p));
            continue;
        }
        break;
    }
    return any;
}

bool parse_selector(std::string_view s, Selector& out) {
    size_t i = 0;
    while (i < s.size() && isws(s[i])) ++i;
    while (true) {
        Compound c;
        if (!parse_compound(s, i, c, out.spec)) return false;
        if (!out.parts.empty() && out.parts.back().pseudoElement) return false;  // a pseudo-element ends a selector
        out.parts.push_back(std::move(c));
        size_t before = i;
        while (i < s.size() && isws(s[i])) ++i;
        if (i >= s.size()) break;
        char comb = ' ';
        if (s[i] == '>' || s[i] == '+' || s[i] == '~') {
            comb = s[i++];
            while (i < s.size() && isws(s[i])) ++i;
        } else if (i == before) {
            return false;
        }
        out.combs.push_back(comb);
    }
    return !out.parts.empty();
}

int parse_media(std::string_view prelude, Sheet& sh) {
    sh.media.push_back(MediaQuery{std::string(trim(prelude))});
    return (int)sh.media.size() - 1;
}

bool parse_display(const std::string& v, Display& d);

// ---- @supports (CSS Conditional 3, section 2; selector() from Conditional 4)
// Whether this engine takes a declaration: a property it knows, with a value of the kind the property takes
bool supports_declaration(std::string_view decl) {
    size_t colon = decl.find(':');
    if (colon == npos) return false;
    std::string prop = lower(trim(decl.substr(0, colon)));
    std::string value = lower(trim(decl.substr(colon + 1)));
    if (value.empty() || contains(value, "!important")) return false;
    if (prop.size() > 2 && prop[0] == '-' && prop[1] == '-') return true;  // a custom property takes anything
    static const char* const known[] = {
        "align-content", "align-items", "align-self", "animation", "animation-name", "background", "background-attachment",
        "background-clip", "background-color", "background-image", "background-origin", "background-position",
        "background-repeat", "background-size", "block-size", "border", "border-bottom", "border-bottom-color",
        "border-bottom-style", "border-bottom-width", "border-color", "border-left", "border-left-color", "border-left-style",
        "border-left-width", "border-radius", "border-right", "border-right-color", "border-right-style", "border-right-width",
        "border-style", "border-top", "border-top-color", "border-top-style", "border-top-width", "border-width", "bottom",
        "box-sizing", "clear", "clip-path", "color", "column-gap", "content", "counter-increment", "counter-reset",
        "counter-set", "display", "fill", "fill-opacity", "fill-rule", "flex", "flex-basis", "flex-direction", "flex-flow",
        "flex-grow", "flex-shrink", "flex-wrap", "float", "font", "font-family", "font-size", "font-style", "font-weight", "gap",
        "grid", "grid-area", "grid-auto-columns", "grid-auto-flow", "grid-auto-rows", "grid-column", "grid-column-end",
        "grid-column-start", "grid-row", "grid-row-end", "grid-row-start", "grid-template", "grid-template-areas",
        "grid-template-columns", "grid-template-rows", "height", "inline-size", "inset", "justify-content", "justify-items",
        "justify-self", "left", "line-height", "list-style", "list-style-type", "margin", "margin-bottom", "margin-left",
        "margin-right", "margin-top", "mask", "mask-image", "max-height", "max-width", "min-height", "min-width", "opacity",
        "overflow", "overflow-x", "overflow-y", "padding", "padding-bottom", "padding-left", "padding-right", "padding-top",
        "place-content", "place-items", "place-self", "position", "right", "row-gap", "stroke", "stroke-opacity",
        "stroke-width", "text-align", "text-decoration", "text-decoration-line", "text-transform", "text-wrap", "top",
        "transform", "transform-origin", "vertical-align", "visibility", "white-space", "width", "z-index"};
    bool isKnown = false;
    for (const char* k : known) isKnown = isKnown || prop == k;
    if (!isKnown) return false;
    if (contains(value, "var(")) return true;
    if (in(value, {"inherit", "initial", "unset", "revert", "revert-layer"})) return true;
    if (prop == "display") {
        Display d;
        return parse_display(value, d);
    }
    if (prop == "position") return in(value, {"static", "relative", "absolute", "fixed", "sticky"});
    if (prop == "color" || prop == "background-color" || (istarts(prop, "border-") && prop.size() > 6 && prop.compare(prop.size() - 6, 6, "-color") == 0)) {
        Color c;
        return parse_color(value, c);
    }
    if (in(prop, {"width", "height", "min-width", "min-height", "max-width", "max-height", "top", "right", "bottom", "left",
                  "margin-top", "margin-right", "margin-bottom", "margin-left", "padding-top", "padding-right", "padding-bottom",
                  "padding-left", "row-gap", "column-gap", "inline-size", "block-size", "flex-basis"})) {
        Len l;
        return parse_len(value, 16, l, true);
    }
    return true;
}
bool supports_condition(std::string_view v);
bool supports_in_parens(std::string_view v) {
    v = trim(v);
    std::string l = lower(v);
    if (istarts(l, "selector(") && l.back() == ')') {
        Selector sel;
        if (!parse_selector(trim(v.substr(9, v.size() - 10)), sel)) return false;
        for (auto& part : sel.parts)
            if (part.never) return false;
        return true;
    }
    if (v.size() < 2 || v[0] != '(' || v.back() != ')') return false;  // font-tech(), font-format() and the unknown
    std::string_view inner = trim(v.substr(1, v.size() - 2));
    if (!inner.empty() && (inner[0] == '(' || istarts(lower(inner), "not ") || istarts(lower(inner), "selector(")))
        return supports_condition(inner);
    return supports_declaration(inner);
}
// not <in-parens> | <in-parens> [and <in-parens>]* | <in-parens> [or <in-parens>]*
bool supports_condition(std::string_view v) {
    v = trim(v);
    if (istarts(lower(v), "not ") || istarts(lower(v), "not(")) return !supports_in_parens(v.substr(3));
    std::vector<std::string_view> terms;
    std::vector<std::string> ops;
    size_t i = 0, start = 0;
    int depth = 0;
    while (i < v.size()) {
        char c = v[i];
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (depth == 0 && isws(c)) {
            size_t j = i;
            while (j < v.size() && isws(v[j])) ++j;
            size_t k = j;
            while (k < v.size() && isalpha((unsigned char)v[k])) ++k;
            std::string word = lower(v.substr(j, k - j));
            if ((word == "and" || word == "or") && k < v.size() && (isws(v[k]) || v[k] == '(')) {
                terms.push_back(v.substr(start, i - start));
                ops.push_back(word);
                start = i = k;
                continue;
            }
        }
        ++i;
    }
    terms.push_back(v.substr(start));
    for (size_t k = 1; k < ops.size(); ++k)
        if (ops[k] != ops[0]) return false;  // and and or may not be mixed without parentheses
    bool any = ops.empty() || ops[0] == "and";
    for (auto& t : terms) {
        bool r = supports_in_parens(t);
        if (ops.empty() || ops[0] == "and") any = any && r;
        else any = any || r;
    }
    return any;
}

// The index of a layer in the sheet's list, which it joins the first time it is named
int sheet_layer(Sheet& sh, const std::string& name) {
    if (name.empty()) return -1;
    for (size_t i = 0; i < sh.layers.size(); ++i)
        if (sh.layers[i] == name) return (int)i;
    sh.layers.push_back(name);
    return (int)sh.layers.size() - 1;
}
// @layer's name inside the layer `outer`: dotted names are relative to it, and a block without a name is a layer
// of its own that no other rule can name
std::string layer_name(const std::string& outer, std::string_view name) {
    static int anonymous = 0;
    std::string n(trim(name));
    if (n.empty()) n = "\x01" + std::to_string(++anonymous);
    return outer.empty() ? n : outer + "." + n;
}
bool parse_url_token(std::string_view t, std::string& out);
int parse_media(std::string_view prelude, Sheet& sh);
// @import url [layer | layer(name)]? [supports(...)]? <media-query-list>? (CSS Cascade 5, 2)
void parse_import(std::string_view v, Sheet& sh) {
    auto T = tokens(v);
    if (T.empty()) return;
    SheetImport im;
    if (T[0].size() >= 2 && (T[0][0] == '"' || T[0][0] == '\'')) im.url = T[0].substr(1, T[0].size() - 2);
    else if (!parse_url_token(T[0], im.url)) return;
    size_t i = 1;
    if (i < T.size() && (lower(T[i]) == "layer" || istarts(lower(T[i]), "layer("))) {
        std::string l = T[i];
        im.layer = sheet_layer(sh, layer_name({}, l.size() > 6 ? std::string_view(l).substr(6, l.size() - 7) : std::string_view()));
        ++i;
    }
    if (i < T.size() && istarts(lower(T[i]), "supports(")) {
        std::string c = T[i].substr(9, T[i].size() - 10);
        if (!supports_condition(c.find(':') != std::string::npos && c[0] != '(' ? "(" + c + ")" : c)) return;
        ++i;
    }
    std::string mediaText;
    for (; i < T.size(); ++i) mediaText += T[i] + " ";
    if (!trim(mediaText).empty()) im.media = parse_media(mediaText, sh);
    sh.imports.push_back(std::move(im));
}

// @font-face (CSS Fonts 4, 4.1-4.5): font-family and src; the rest is left to the font files themselves
void parse_font_face(std::string_view body, Sheet& sh) {
    FontFace face;
    for (auto& d : parse_decls(body)) {
        if (d.prop == "font-family") {
            std::string f(trim(d.val));
            if (f.size() >= 2 && (f[0] == '"' || f[0] == '\'')) f = f.substr(1, f.size() - 2);
            face.family = f;
        } else if (d.prop == "unicode-range") {
            face.unicodeRange = lower(trim(d.val));
        } else if (d.prop == "src") {
            for (auto part : split_top(d.val, ',')) {
                std::string spaced;  // url(a)format(b) is two tokens
                for (size_t k = 0; k < part.size(); ++k) {
                    spaced += part[k];
                    if (part[k] == ')' && k + 1 < part.size() && !isws(part[k + 1])) spaced += ' ';
                }
                auto T = tokens(trim(spaced));
                if (T.empty()) continue;
                std::string url, format;
                if (istarts(lower(T[0]), "local(")) {
                    std::string n = T[0].substr(6, T[0].size() - 7);
                    if (n.size() >= 2 && (n[0] == '"' || n[0] == '\'')) n = n.substr(1, n.size() - 2);
                    face.src.push_back({"local:" + n, ""});
                    continue;
                }
                if (!parse_url_token(T[0], url)) continue;
                for (size_t k = 1; k < T.size(); ++k)
                    if (istarts(lower(T[k]), "format(")) {
                        format = lower(T[k].substr(7, T[k].size() - 8));
                        if (format.size() >= 2 && (format[0] == '"' || format[0] == '\'')) format = format.substr(1, format.size() - 2);
                        format = format.substr(0, format.find(' '));  // "woff2 supports variations": the format is the first word
                    }
                face.src.push_back({url, format});
            }
        }
    }
    if (!face.family.empty() && !face.src.empty()) sh.fontFaces.push_back(std::move(face));
}

// @keyframes (CSS Animations 1, 3.1): the opacity, transform and timing function of each keyframe selector
void parse_keyframes(std::string_view prelude, std::string_view body, Sheet& sh) {
    std::string name(trim(prelude));
    if (name.size() >= 2 && (name[0] == '"' || name[0] == '\'')) name = name.substr(1, name.size() - 2);
    if (name.empty()) return;
    auto kf = std::make_shared<Keyframes>();
    size_t i = 0;
    while (i < body.size()) {
        size_t open = body.find('{', i);
        if (open == npos) break;
        size_t close = block_end(body, open);
        if (close == npos) close = body.size();
        std::string_view selectors = trim(body.substr(i, open - i));
        auto decls = parse_decls(body.substr(open + 1, close - open - 1));
        i = close + 1;
        for (auto sel : split_top(selectors, ',')) {
            std::string l = lower(trim(sel));
            float offset;
            std::string_view u;
            if (l == "from") offset = 0;
            else if (l == "to") offset = 1;
            else if (parse_num(l, offset, u) && u == "%") offset /= 100;
            else continue;
            if (offset < 0 || offset > 1) continue;
            Keyframe* f = nullptr;  // keyframes of the same offset merge, the later winning
            for (auto& k : kf->frames)
                if (k.offset == offset) f = &k;
            if (!f) {
                kf->frames.push_back(Keyframe{});
                f = &kf->frames.back();
                f->offset = offset;
            }
            for (auto& d : decls) {
                std::string v = lower(trim(d.val));
                if (d.prop == "opacity") {
                    float n;
                    std::string_view unit;
                    if (parse_num(v, n, unit)) f->opacity = std::clamp(unit == "%" ? n / 100 : n, 0.f, 1.f), f->hasOpacity = true;
                } else if (d.prop == "transform" || d.prop == "-webkit-transform") {
                    TransformList list;
                    if (v == "none") f->transform = nullptr, f->hasTransform = true;
                    else if (parse_transform(v, 16, list)) f->transform = std::make_shared<const TransformList>(std::move(list)), f->hasTransform = true;
                } else if (d.prop == "animation-timing-function") {
                    f->hasTiming = parse_timing(v, f->timing);
                }
            }
        }
    }
    std::stable_sort(kf->frames.begin(), kf->frames.end(), [](const Keyframe& a, const Keyframe& b) { return a.offset < b.offset; });
    for (auto& f : kf->frames) {
        kf->opacity = kf->opacity || f.hasOpacity;
        kf->transform = kf->transform || f.hasTransform;
    }
    sh.keyframes.push_back({name, std::move(kf)});
}

// ---- CSS Nesting 1: a rule inside a rule. The nested selector is taken relative to the parent: & stands for the
// parent's elements, and a selector without & is a descendant of them. & becomes :is(parent) unless the parent's
// text can stand in for it unchanged: a single compound selector, or a single selector at the start.
std::string nest_selector(std::string_view parent, std::string_view child) {
    auto parents = split_top(parent, ',');
    std::string list = std::string(trim(parent));
    bool single = parents.size() == 1;
    bool compound = single;
    int depth = 0;
    for (char c : list) {
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') --depth;
        else if (depth == 0 && (isws(c) || c == '>' || c == '+' || c == '~')) compound = false;
    }
    std::string out;
    for (auto part : split_top(child, ',')) {
        std::string_view p = trim(part);
        std::string one;
        if (p.find('&') == npos) {
            one = (single ? list : ":is(" + list + ")") + " " + std::string(p);
        } else {
            for (size_t i = 0; i < p.size(); ++i) {
                if (p[i] != '&') {
                    one += p[i];
                    continue;
                }
                bool leading = trim(std::string_view(one)).empty();
                one += single && (compound || leading) ? list : ":is(" + list + ")";
            }
        }
        if (!out.empty()) out += ", ";
        out += one;
    }
    return out;
}

void parse_block(std::string_view s, int media, Sheet& sh, int depth, const std::string& layer = {});
// A style rule, with the rules nested in it (CSS Nesting 1, section 2)
void parse_style_rule(std::string_view selText, std::string_view body, int media, Sheet& sh, int depth, const std::string& layer) {
    std::string decls;
    std::vector<std::pair<std::string_view, std::string_view>> nested;
    size_t i = 0, start = 0;
    int paren = 0;
    char q = 0;
    while (i < body.size()) {
        char c = body[i];
        if (q) {
            if (c == '\\') ++i;
            else if (c == q) q = 0;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') q = c;
        else if (c == '(') ++paren;
        else if (c == ')') --paren;
        else if (paren <= 0 && c == ';') {
            decls.append(body.substr(start, i - start + 1));
            start = i + 1;
        } else if (paren <= 0 && c == '{') {
            size_t end = block_end(body, i);
            if (end == npos) end = body.size();
            nested.push_back({trim(body.substr(start, i - start)), body.substr(i + 1, std::min(end, body.size()) - i - 1)});
            i = end + 1;
            start = std::min(i, body.size());
            continue;
        }
        ++i;
    }
    if (start < body.size()) decls.append(body.substr(start));
    auto d = std::make_shared<std::vector<Decl>>(parse_decls(decls));
    if (!d->empty())
        for (auto part : split_top(selText, ',')) {
            Selector x;
            if (parse_selector(trim(part), x)) sh.rules.push_back({std::move(x), d, media, sheet_layer(sh, layer)});
        }
    if (depth >= 8) return;
    for (auto& [prelude, inner] : nested) {
        if (prelude.empty()) continue;
        if (prelude[0] == '@') {  // conditional group rules inside a style rule hold declarations for it (2.3)
            size_t j = 1;
            std::string kw = lower(read_ident(prelude, j));
            std::string_view rest = trim(prelude.substr(j));
            if (kw == "media") parse_style_rule(selText, inner, parse_media(rest, sh), sh, depth + 1, layer);
            else if (kw == "supports") {
                if (supports_condition(rest)) parse_style_rule(selText, inner, media, sh, depth + 1, layer);
            } else if (kw == "layer")
                parse_style_rule(selText, inner, media, sh, depth + 1, layer_name(layer, rest));
            else if (kw == "container" || kw == "scope")
                parse_style_rule(selText, inner, media, sh, depth + 1, layer);
            continue;
        }
        parse_style_rule(nest_selector(selText, prelude), inner, media, sh, depth + 1, layer);
    }
}

void parse_block(std::string_view s, int media, Sheet& sh, int depth, const std::string& layer) {
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (isws(s[i]) || s[i] == ';')) ++i;
        if (i >= s.size()) break;
        if (s[i] == '@') {
            size_t j = i + 1;
            std::string kw = lower(read_ident(s, j));
            size_t brace = npos;
            {
                int pd = 0;
                char q = 0;
                for (size_t k = j; k < s.size(); ++k) {
                    char c = s[k];
                    if (q) {
                        if (c == q) q = 0;
                        continue;
                    }
                    if (c == '"' || c == '\'') q = c;
                    else if (c == '(') ++pd;
                    else if (c == ')') --pd;
                    else if (pd <= 0 && (c == '{' || c == ';')) {
                        brace = k;
                        break;
                    }
                }
            }
            if (brace == npos) brace = s.size();  // a statement at the very end may leave out its semicolon
            if (brace == s.size() || s[brace] == ';') {
                std::string_view statement = trim(s.substr(j, brace - j));
                if (kw == "layer")  // @layer a, b; puts the layers in order before their rules come
                    for (auto name : split_top(statement, ',')) sheet_layer(sh, layer_name(layer, name));
                else if (kw == "import" && sh.rules.empty() && depth == 0)
                    parse_import(statement, sh);
                i = brace + 1;
                continue;
            }
            size_t end = block_end(s, brace);
            if (end == npos) end = s.size();
            std::string_view prelude = trim(s.substr(j, brace - j));
            std::string_view inner = s.substr(brace + 1, end - brace - 1);
            if (depth < 8) {
                if (kw == "media")
                    parse_block(inner, parse_media(prelude, sh), sh, depth + 1, layer);
                else if (kw == "supports") {
                    if (supports_condition(prelude)) parse_block(inner, media, sh, depth + 1, layer);
                } else if (kw == "layer")
                    parse_block(inner, media, sh, depth + 1, layer_name(layer, prelude));
                else if (kw == "container" || kw == "scope" || kw == "document" || kw == "-moz-document")
                    parse_block(inner, media, sh, depth + 1, layer);
                else if (kw == "font-face")
                    parse_font_face(inner, sh);
                else if (kw == "keyframes" || kw == "-webkit-keyframes")
                    parse_keyframes(prelude, inner, sh);
            }
            i = end + 1;
            continue;
        }
        size_t brace = s.find('{', i);
        if (brace == npos) break;
        size_t end = block_end(s, brace);
        if (end == npos) end = s.size();
        parse_style_rule(trim(s.substr(i, brace - i)), s.substr(brace + 1, std::min(end, s.size()) - brace - 1), media, sh, depth, layer);
        i = end + 1;
    }
}

}  // namespace

std::vector<Decl> parse_decls(std::string_view body) {
    std::vector<Decl> out;
    for (auto part : split_top(body, ';')) {
        size_t c = part.find(':');
        if (c == npos) continue;
        std::string_view name = trim(part.substr(0, c));
        if (name.empty() || name.find('{') != npos || name.find('}') != npos) continue;
        Decl d;
        d.prop = (name.size() > 2 && name[0] == '-' && name[1] == '-') ? std::string(name) : lower(name);
        std::string_view v = trim(part.substr(c + 1));
        size_t bang = v.rfind('!');
        if (bang != npos && ieq(trim(v.substr(bang + 1)), "important")) {
            d.important = true;
            v = trim(v.substr(0, bang));
        }
        d.val = std::string(v);
        out.push_back(std::move(d));
    }
    return out;
}

// ============================================================================
// Backgrounds (CSS Backgrounds 3, section 2) and gradients (CSS Images 3, section 3)
// ============================================================================
namespace {

std::string_view strip_fn(std::string_view t, std::string_view name) {  // the arguments of name(...), or empty
    if (t.size() < name.size() + 2 || t.substr(0, name.size()) != name || t[name.size()] != '(' || t.back() != ')') return {};
    return t.substr(name.size() + 1, t.size() - name.size() - 2);
}
// url(x), url("x") or url('x'): the URL as written
bool parse_url_token(std::string_view raw, std::string& out) {
    std::string_view t = trim(raw);
    if (t.size() < 5 || lower(t.substr(0, 4)) != "url(" || t.back() != ')') return false;
    std::string_view in = trim(t.substr(4, t.size() - 5));
    if (in.size() >= 2 && (in[0] == '"' || in[0] == '\'') && in.back() == in[0]) in = in.substr(1, in.size() - 2);
    out = std::string(in);
    return true;
}
// angular: the positions are angles or percentages of a turn (a conic gradient's), kept as percentages
bool parse_stops(const std::vector<std::string_view>& args, size_t first, float fs, std::vector<GradientStop>& out, bool angular = false) {
    auto position = [&](const std::string& t, Len& l) {
        if (!angular) return parse_len(t, fs, l, false);
        double rad;
        float v;
        std::string_view u;
        if (parse_num(t, v, u) && u == "%") {
            l = {Len::Pct, v};
            return true;
        }
        if (!parse_angle(t, rad)) return false;
        l = {Len::Pct, (float)(rad * 50 / 3.14159265358979323846)};
        return true;
    };
    for (size_t i = first; i < args.size(); ++i) {
        auto T = tokens(trim(args[i]));
        if (T.empty()) return false;
        Color c;
        if (!parse_color(T[0], c)) {
            Len hint;  // a colour hint (CSS Images 4, 3.5.3) between two stops: the midpoint is not moved
            if (T.size() == 1 && position(T[0], hint)) continue;
            return false;
        }
        GradientStop st;
        st.c = c;
        if (T.size() >= 2 && !position(T[1], st.pos)) return false;
        out.push_back(st);
        if (T.size() == 3) {  // a stop with two positions is two stops of the same colour
            GradientStop second = st;
            if (!position(T[2], second.pos)) return false;
            out.push_back(second);
        }
    }
    return out.size() >= 1;
}
// <position> for background-position and gradient centres (CSS Values 4, 9.2): keywords, lengths, percentages, and
// edge offsets such as "right 10px bottom 20%"
bool parse_position(const std::vector<std::string>& T, float fs, Len& x, Len& y) {
    auto kw = [](const std::string& t, bool& horizontal, float& pct) {
        if (t == "left") { horizontal = true; pct = 0; return true; }
        if (t == "right") { horizontal = true; pct = 100; return true; }
        if (t == "top") { horizontal = false; pct = 0; return true; }
        if (t == "bottom") { horizontal = false; pct = 100; return true; }
        if (t == "center") { horizontal = true; pct = 50; return true; }
        return false;
    };
    Len a, b;
    x = Len{Len::Pct, 50};
    y = Len{Len::Pct, 50};
    if (T.empty() || T.size() > 4) return false;
    if (T.size() <= 2) {
        bool h1 = true, h2 = false;
        float p1 = 0, p2 = 50;
        bool k1 = kw(T[0], h1, p1), k2 = T.size() == 2 && kw(T[1], h2, p2);
        if (!k1 && !parse_len(T[0], fs, a, false)) return false;
        if (T.size() == 2 && !k2 && !parse_len(T[1], fs, b, false)) return false;
        if (T.size() == 1) {
            if (!k1) { x = a; return true; }
            if (T[0] == "top" || T[0] == "bottom") y = Len{Len::Pct, p1};
            else x = Len{Len::Pct, p1};
            return true;
        }
        if (k1 && !h1 && T[0] != "center") {  // "top left": the vertical keyword first
            y = Len{Len::Pct, p1};
            if (k2) x = Len{Len::Pct, p2};
            else return false;
            return true;
        }
        x = k1 ? Len{Len::Pct, p1} : a;
        if (k2) {
            if (h2 && T[1] != "center") {  // "center left"
                y = x;
                x = Len{Len::Pct, p2};
            } else y = Len{Len::Pct, p2};
        } else y = b;
        return true;
    }
    // three or four values: keyword [offset] keyword [offset]
    size_t i = 0;
    for (int axis = 0; axis < 2 && i < T.size(); ++axis) {
        bool h;
        float p;
        if (!kw(T[i], h, p)) return false;
        std::string word = T[i++];
        Len off{Len::Px, 0};
        bool hasOff = i < T.size() && parse_len(T[i], fs, off, false);
        if (hasOff) ++i;
        Len v{Len::Pct, p};
        if (hasOff) {  // an offset from the right or bottom edge counts back from 100%
            if (off.u == Len::Pct) v.v = word == "right" || word == "bottom" ? 100 - off.v : off.v;
            else v.off = word == "right" || word == "bottom" ? -off.v : off.v;
        }
        bool vertical = word == "top" || word == "bottom";
        (vertical ? y : x) = v;
    }
    return i == T.size();
}
bool parse_gradient(std::string_view raw, float fs, BgImage& g) {
    std::string lv = lower(trim(raw));
    std::string_view args;
    std::string_view t = lv;
    g.repeating = false;
    if (t.substr(0, 10) == "repeating-") {
        g.repeating = true;
        t = t.substr(10);
    }
    if (!(args = strip_fn(t, "linear-gradient")).empty() || t == "linear-gradient()") {
        g.k = BgImage::Linear;
        auto parts = split_top(args, ',');
        size_t first = 0;
        auto T = tokens(trim(parts[0]));
        if (!T.empty() && T[0] == "to") {
            g.toCorner = 0;
            for (size_t i = 1; i < T.size(); ++i) {
                if (T[i] == "top") g.toCorner |= 1;
                else if (T[i] == "right") g.toCorner |= 2;
                else if (T[i] == "bottom") g.toCorner |= 4;
                else if (T[i] == "left") g.toCorner |= 8;
                else return false;
            }
            if (g.toCorner == 1) g.angle = 0, g.toCorner = 0;
            else if (g.toCorner == 2) g.angle = 90, g.toCorner = 0;
            else if (g.toCorner == 4) g.angle = 180, g.toCorner = 0;
            else if (g.toCorner == 8) g.angle = 270, g.toCorner = 0;
            first = 1;
        } else if (T.size() == 1) {
            double rad;
            if (parse_angle(T[0], rad)) {
                g.angle = (float)(rad * 180.0 / 3.14159265358979323846);
                first = 1;
            }
        }
        return parse_stops(parts, first, fs, g.stops);
    }
    if (!(args = strip_fn(t, "radial-gradient")).empty()) {
        g.k = BgImage::Radial;
        auto parts = split_top(args, ',');
        auto T = tokens(trim(parts[0]));
        size_t first = 0;
        Color probe;
        if (!T.empty() && !parse_color(T[0], probe)) {
            first = 1;
            size_t at = T.size();
            for (size_t i = 0; i < T.size(); ++i)
                if (T[i] == "at") at = i;
            std::vector<Len> sizes;
            for (size_t i = 0; i < at; ++i) {
                const std::string& w = T[i];
                if (w == "circle") g.circle = true;
                else if (w == "ellipse") g.circle = false;
                else if (w == "closest-side") g.extent = 1;
                else if (w == "closest-corner") g.extent = 2;
                else if (w == "farthest-side") g.extent = 3;
                else if (w == "farthest-corner") g.extent = 0;
                else {
                    Len l;
                    if (!parse_len(w, fs, l, false)) return false;
                    sizes.push_back(l);
                }
            }
            if (sizes.size() == 1) { g.extent = 4; g.rx = g.ry = sizes[0]; g.circle = true; }
            else if (sizes.size() == 2) { g.extent = 4; g.rx = sizes[0]; g.ry = sizes[1]; }
            else if (sizes.size() > 2) return false;
            if (at < T.size()) {
                std::vector<std::string> pos(T.begin() + at + 1, T.end());
                if (!parse_position(pos, fs, g.cx, g.cy)) return false;
            }
        }
        return parse_stops(parts, first, fs, g.stops);
    }
    if (!(args = strip_fn(t, "conic-gradient")).empty()) {
        // CSS Images 4, 3.3: conic-gradient([from <angle>]? [at <position>]?, <angular-color-stop-list>)
        g.k = BgImage::Conic;
        g.angle = 0;
        auto parts = split_top(args, ',');
        auto T = tokens(lower(trim(parts[0])));
        size_t first = 0;
        if (!T.empty() && (T[0] == "from" || T[0] == "at")) {
            first = 1;
            size_t i = 0;
            if (T[0] == "from") {
                double rad;
                if (T.size() < 2 || !parse_angle(T[1], rad)) return false;
                g.angle = (float)(rad * 180 / 3.14159265358979323846);
                i = 2;
            }
            if (i < T.size()) {
                if (T[i] != "at") return false;
                if (!parse_position(std::vector<std::string>(T.begin() + i + 1, T.end()), fs, g.cx, g.cy)) return false;
            }
        }
        return parse_stops(parts, first, fs, g.stops, true);
    }
    return false;
}
// One background image: none, url(), or a gradient
bool parse_bg_image(std::string_view raw, float fs, BgImage& out) {
    std::string_view t = trim(raw);
    std::string lv = lower(t);
    if (lv == "none") { out.k = BgImage::None; return true; }
    if (parse_url_token(t, out.url)) { out.k = BgImage::Url; return true; }
    if (lv.find("gradient(") != std::string::npos) return parse_gradient(t, fs, out);
    if (lv.rfind("image-set(", 0) == 0 || lv.rfind("-webkit-image-set(", 0) == 0) {  // CSS Images 4, 2.4: the first image
        size_t a = t.find('('), b = t.rfind(')');
        auto opts = split_top(t.substr(a + 1, b - a - 1), ',');
        auto T = tokens(trim(opts[0]));
        if (T.empty()) return false;
        std::string first = T[0];
        if (!first.empty() && (first[0] == '"' || first[0] == '\'')) { out.k = BgImage::Url; out.url = first.substr(1, first.size() - 2); return true; }
        return parse_bg_image(first, fs, out);
    }
    return false;
}
bool parse_bg_repeat(const std::vector<std::string>& T, uint16_t& out) {
    auto one = [](const std::string& t) -> int { return t == "repeat" ? 0 : t == "no-repeat" ? 1 : t == "space" ? 2 : t == "round" ? 3 : -1; };
    if (T.size() == 1) {
        if (T[0] == "repeat-x") { out = 0 | (1 << 8); return true; }
        if (T[0] == "repeat-y") { out = 1 | (0 << 8); return true; }
        int r = one(T[0]);
        if (r < 0) return false;
        out = (uint16_t)(r | (r << 8));
        return true;
    }
    if (T.size() == 2) {
        int a = one(T[0]), b = one(T[1]);
        if (a < 0 || b < 0) return false;
        out = (uint16_t)(a | (b << 8));
        return true;
    }
    return false;
}
bool parse_bg_size(const std::vector<std::string>& T, float fs, BgSize& out) {
    if (T.size() == 1 && (T[0] == "cover" || T[0] == "contain")) { out.mode = T[0] == "cover" ? 1 : 2; return true; }
    if (T.empty() || T.size() > 2) return false;
    out.mode = 0;
    if (!parse_len(T[0], fs, out.w, true)) return false;
    out.h = Len{};
    if (T.size() == 2 && !parse_len(T[1], fs, out.h, true)) return false;
    return true;
}
int box_keyword(const std::string& t) { return t == "border-box" ? 0 : t == "padding-box" ? 1 : t == "content-box" ? 2 : t == "text" ? 3 : -1; }
// The `background` shorthand (CSS Backgrounds 3, 3.10): comma-separated layers, the colour in the last one only
bool parse_background(std::string_view raw, float fs, Background& bg, Color& color, bool& hasColor) {
    auto layers = split_top(raw, ',');
    for (size_t li = 0; li < layers.size(); ++li) {
        BgImage img;
        BgPos pos;
        BgSize size;
        uint16_t repeat = 0;
        int origin = -1, clip = -1;
        auto T = tokens(trim(layers[li]));
        std::vector<std::string> lowerT;
        for (auto& t : T) lowerT.push_back(lower(t));
        size_t i = 0;
        bool haveImage = false, havePos = false, haveRepeat = false;
        while (i < T.size()) {
            const std::string& w = lowerT[i];
            Color c;
            if (!haveImage && parse_bg_image(T[i], fs, img)) { haveImage = true; ++i; continue; }
            if (li + 1 == layers.size() && parse_color(w, c)) { color = c; hasColor = true; ++i; continue; }
            if (!haveRepeat) {
                uint16_t r;
                if (i + 1 < T.size() && parse_bg_repeat({lowerT[i], lowerT[i + 1]}, r)) { repeat = r; haveRepeat = true; i += 2; continue; }
                if (parse_bg_repeat({w}, r)) { repeat = r; haveRepeat = true; ++i; continue; }
            }
            int bk = box_keyword(w);
            if (bk >= 0) {
                if (origin < 0) origin = clip = bk;
                else clip = bk;
                ++i;
                continue;
            }
            if (w == "scroll" || w == "fixed" || w == "local") { ++i; continue; }  // background-attachment: not kept
            if (!havePos) {  // <position> [ / <size> ]
                size_t j = i;
                std::vector<std::string> posT;
                while (j < T.size() && lowerT[j] != "/" && posT.size() < 4) {
                    Len probe;
                    bool h;
                    const std::string& x = lowerT[j];
                    bool isKw = x == "left" || x == "right" || x == "top" || x == "bottom" || x == "center";
                    if (!isKw && !parse_len(x, fs, probe, false)) break;
                    (void)h;
                    posT.push_back(x);
                    ++j;
                }
                if (!posT.empty()) {
                    // take the longest prefix that is a position
                    size_t n = posT.size();
                    while (n > 0 && !parse_position(std::vector<std::string>(posT.begin(), posT.begin() + n), fs, pos.x, pos.y)) --n;
                    if (n == 0) return false;
                    i += n;
                    havePos = true;
                    if (i < T.size() && lowerT[i] == "/") {
                        ++i;
                        std::vector<std::string> sizeT;
                        while (i < T.size() && sizeT.size() < 2) {
                            Len probe;
                            const std::string& x = lowerT[i];
                            if (x != "auto" && x != "cover" && x != "contain" && !parse_len(x, fs, probe, false)) break;
                            sizeT.push_back(x);
                            ++i;
                            if (x == "cover" || x == "contain") break;
                        }
                        if (!parse_bg_size(sizeT, fs, size)) return false;
                    }
                    continue;
                }
            }
            return false;
        }
        bg.images.push_back(img);
        bg.pos.push_back(pos);
        bg.size.push_back(size);
        bg.repeat.push_back(repeat);
        bg.origin.push_back((uint8_t)(origin < 0 ? 1 : origin));
        bg.clip.push_back((uint8_t)(clip < 0 ? 0 : clip));
    }
    return true;
}
// A background longhand: its comma-separated values into the list of the Background it changes
bool apply_bg_longhand(std::string_view prop, std::string_view raw, float fs, Background& bg) {
    auto items = split_top(raw, ',');
    if (prop == "background-image") {
        bg.images.clear();
        for (auto v : items) {
            BgImage img;
            if (!parse_bg_image(v, fs, img)) return false;
            bg.images.push_back(img);
        }
        return true;
    }
    if (prop == "background-position" || prop == "background-position-x" || prop == "background-position-y") {
        std::vector<BgPos> out;
        for (size_t k = 0; k < items.size(); ++k) {
            auto v = items[k];
            BgPos p = k < bg.pos.size() ? bg.pos[k] : BgPos{};
            std::vector<std::string> T;
            for (auto& t : tokens(trim(v))) T.push_back(lower(t));
            Len x, y;
            if (prop == "background-position") {
                if (!parse_position(T, fs, x, y)) return false;
                p.x = x;
                p.y = y;
            } else {
                if (T.empty() || !parse_position(T, fs, x, y)) return false;
                bool vertical = prop == "background-position-y";
                if (vertical) p.y = T[0] == "top" || T[0] == "bottom" || T[0] == "center" ? y : x;
                else p.x = x;
            }
            out.push_back(p);
        }
        bg.pos = std::move(out);
        return true;
    }
    if (prop == "background-size") {
        std::vector<BgSize> out;
        for (auto v : items) {
            std::vector<std::string> T;
            for (auto& t : tokens(trim(v))) T.push_back(lower(t));
            BgSize s;
            if (!parse_bg_size(T, fs, s)) return false;
            out.push_back(s);
        }
        bg.size = std::move(out);
        return true;
    }
    if (prop == "background-repeat") {
        std::vector<uint16_t> out;
        for (auto v : items) {
            std::vector<std::string> T;
            for (auto& t : tokens(trim(v))) T.push_back(lower(t));
            uint16_t r;
            if (!parse_bg_repeat(T, r)) return false;
            out.push_back(r);
        }
        bg.repeat = std::move(out);
        return true;
    }
    if (prop == "background-origin" || prop == "background-clip") {
        std::vector<uint8_t> out;
        for (auto v : items) {
            int k = box_keyword(lower(trim(v)));
            if (k < 0 || (k == 3 && prop == "background-origin")) return false;
            out.push_back((uint8_t)k);
        }
        (prop == "background-origin" ? bg.origin : bg.clip) = std::move(out);
        return true;
    }
    return true;  // background-attachment, -blend-mode: not kept
}

// Makes the URLs in a style sheet's declarations absolute: they are relative to the sheet (CSS Values 4, 4.5.1)
void absolutize_urls(std::string& val, const std::string& base) {
    std::string out;
    size_t i = 0;
    while (true) {
        size_t at = val.find("url(", i);
        if (at == std::string::npos) break;
        size_t close = val.find(')', at);
        if (close == std::string::npos) break;
        std::string inner(trim(std::string_view(val).substr(at + 4, close - at - 4)));
        char q = 0;
        if (inner.size() >= 2 && (inner[0] == '"' || inner[0] == '\'') && inner.back() == inner[0]) {
            q = inner[0];
            inner = inner.substr(1, inner.size() - 2);
        }
        out += val.substr(i, at - i);
        if (inner.rfind("data:", 0) == 0 || inner.empty() || inner[0] == '#') out += val.substr(at, close + 1 - at);
        else out += "url(\"" + resolve_url(base, inner) + "\")";
        (void)q;
        i = close + 1;
    }
    if (i == 0) return;
    out += val.substr(i);
    val = std::move(out);
}

}  // namespace

Sheet parse_sheet(std::string_view css, const std::string& base) {
    Sheet sh;
    std::string clean = strip_comments(css);
    parse_block(clean, -1, sh, 0);
    if (!base.empty()) {
        for (auto& r : sh.rules)
            for (auto& d : *r.decls)
                if (d.val.find("url(") != std::string::npos) absolutize_urls(d.val, base);
        for (auto& im : sh.imports) im.url = resolve_url(base, im.url);
        for (auto& face : sh.fontFaces)
            for (auto& [url, format] : face.src)
                if (!istarts(url, "local:")) url = resolve_url(base, url);
    }
    return sh;
}

// ============================================================================
// Cascade
// ============================================================================
namespace {

bool match_nth(int a, int b, int n) {
    if (a == 0) return n == b;
    int d = n - b;
    return d / a >= 0 && d % a == 0;
}

// What the element is to the scope of the rule being matched: 0 an element of that tree (or any element, for a
// rule of the document), 1 the tree's host, 2 a child of the host that a slot may hold.
int g_scopeRole = 0;
bool match_at(const Selector& s, int idx, const Node* e, int budget = 0);
// The form-associated elements that can be disabled (HTML 4.10.18.5)
bool form_control(const Node* e) { return in(e->name, {"button", "input", "select", "textarea", "optgroup", "option", "fieldset"}); }
bool actually_disabled(const Node* e) {
    if (e->attr("disabled")) return true;
    for (const Node* n = e->parent; n; n = n->parent) {  // inside a disabled fieldset, outside its first legend
        if (n->type != Node::T::Element || n->name != "fieldset" || !n->attr("disabled")) continue;
        const Node* legend = nullptr;
        for (auto& k : n->kids)
            if (k->type == Node::T::Element && k->name == "legend") { legend = k.get(); break; }
        bool inLegend = false;
        for (const Node* m = e; m && m != n; m = m->parent) inLegend = inLegend || m == legend;
        if (!inLegend) return true;
    }
    return false;
}
bool text_input(const Node* e) {
    const std::string* t = e->attr("type");
    std::string type = t ? lower(*t) : "text";
    return in(type, {"text", "search", "url", "tel", "email", "password", "number", "date", "month", "week", "time", "datetime-local", ""});
}
// :has() (Selectors 4, 4.5): an element matches one of the relative selectors, anchored at e. Only elements after e
// in tree order can: its descendants, or for '+' and '~' its later siblings and what they hold.
const Node* g_hasAnchor = nullptr;
bool has_relative(const Pseudo& p, const Node* e) {
    const Node* saved = g_hasAnchor;
    g_hasAnchor = e;
    bool found = false;
    for (const Selector& one : *p.list) {
        std::vector<const Node*> todo;
        auto push_kids = [&](const Node* n) {
            for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) todo.push_back(it->get());
        };
        if (one.combs[0] == '+' || one.combs[0] == '~') {
            if (!e->parent) continue;
            auto& sibs = e->parent->kids;
            for (auto it = sibs.rbegin(); it != sibs.rend() && it->get() != e; ++it) todo.push_back(it->get());
        } else
            push_kids(e);
        while (!todo.empty() && !found) {
            const Node* n = todo.back();
            todo.pop_back();
            if (n->type != Node::T::Element) continue;
            found = match_at(one, (int)one.parts.size() - 1, n);
            push_kids(n);
        }
        if (found) break;
    }
    g_hasAnchor = saved;
    return found;
}

bool match_compound(const Compound& c, const Node* e) {
    bool isHost = g_selectorScope && e == g_selectorScope->parent;
    bool isSlotted = g_scopeRole == 2 && g_selectorScope && e->parent == g_selectorScope->parent;
    if (isHost || isSlotted) {  // only :host and ::slotted() reach outside the tree
        bool reaches = false;
        for (auto& p : c.pseudos)
            if (p.k == (isHost ? Pseudo::Host : Pseudo::Slotted)) reaches = true;
        if (!reaches) return false;
    }
    if (c.never) return false;
    if (!c.tag.empty() && c.tag != e->name) return false;
    if (!c.id.empty() && c.id != e->id) return false;
    for (auto& cl : c.classes) {
        bool f = false;
        for (auto& x : e->classes)
            if (x == cl) {
                f = true;
                break;
            }
        if (!f) return false;
    }
    for (auto& a : c.attrs) {
        const std::string* v = e->attr(a.name);
        if (!v) return false;
        std::string_view sv = *v, val = a.val;
        switch (a.op) {
            case 0: break;
            case '=':
                if (a.name == "type" ? !ieq(sv, val) : sv != val) return false;
                break;
            case '~': {
                bool f = false;
                size_t i = 0;
                while (i < sv.size()) {
                    while (i < sv.size() && isws(sv[i])) ++i;
                    size_t j = i;
                    while (j < sv.size() && !isws(sv[j])) ++j;
                    if (sv.substr(i, j - i) == val) f = true;
                    i = j;
                }
                if (!f) return false;
                break;
            }
            case '|':
                if (!(sv == val || (sv.size() > val.size() && sv.substr(0, val.size()) == val && sv[val.size()] == '-'))) return false;
                break;
            case '^':
                if (val.empty() || sv.substr(0, val.size()) != val) return false;
                break;
            case '$':
                if (val.empty() || sv.size() < val.size() || sv.substr(sv.size() - val.size()) != val) return false;
                break;
            case '*':
                if (val.empty() || sv.find(val) == npos) return false;
                break;
        }
    }
    for (auto& p : c.pseudos) {
        switch (p.k) {
            case Pseudo::FirstChild:
                if (e->elemIndex != 0) return false;
                break;
            case Pseudo::LastChild:
                if (e->elemIndex + 1 != e->elemCount) return false;
                break;
            case Pseudo::OnlyChild:
                if (e->elemCount != 1) return false;
                break;
            case Pseudo::Root:
                if (!e->parent || e->parent->type != Node::T::Document) return false;
                break;
            case Pseudo::Link:
                if (!((e->name == "a" || e->name == "area") && e->attr("href"))) return false;
                break;
            case Pseudo::Empty:
                for (auto& k : e->kids)
                    if (k->type == Node::T::Element || (k->type == Node::T::Text && !k->text.empty())) return false;
                break;
            case Pseudo::NthChild:
                if (!match_nth(p.a, p.b, (int)e->elemIndex + 1)) return false;
                break;
            case Pseudo::NthLastChild:
                if (!match_nth(p.a, p.b, (int)(e->elemCount - e->elemIndex))) return false;
                break;
            case Pseudo::FirstOfType:
            case Pseudo::LastOfType: {
                if (!e->parent) return false;
                const Node* first = nullptr;
                const Node* last = nullptr;
                for (auto& k : e->parent->kids)
                    if (k->type == Node::T::Element && k->name == e->name) {
                        if (!first) first = k.get();
                        last = k.get();
                    }
                if ((p.k == Pseudo::FirstOfType ? first : last) != e) return false;
                break;
            }
            case Pseudo::Not:
                for (auto& one : *p.list)
                    if (match_at(one, (int)one.parts.size() - 1, e)) return false;
                break;
            case Pseudo::Is: {
                bool any = false;
                for (auto& one : *p.list)
                    if (match_at(one, (int)one.parts.size() - 1, e)) {
                        any = true;
                        break;
                    }
                if (!any) return false;
                break;
            }
            case Pseudo::Has:
                if (!has_relative(p, e)) return false;
                break;
            case Pseudo::HasAnchor:
                if (e != g_hasAnchor) return false;
                break;
            case Pseudo::NthOfType:
            case Pseudo::NthLastOfType:
            case Pseudo::OnlyOfType: {
                if (!e->parent) return false;
                int before = 0, after = 0;
                bool seen = false;
                for (auto& k : e->parent->kids) {
                    if (k.get() == e) { seen = true; continue; }
                    if (k->type == Node::T::Element && k->name == e->name) (seen ? after : before)++;
                }
                if (p.k == Pseudo::OnlyOfType ? before + after != 0
                                              : !match_nth(p.a, p.b, p.k == Pseudo::NthOfType ? before + 1 : after + 1)) return false;
                break;
            }
            case Pseudo::Hover:  // the element, or one inside it, is under the pointer or pressed
            case Pseudo::Active:
            case Pseudo::FocusWithin: {
                auto& chain = p.k == Pseudo::Hover ? g_userState.hover : p.k == Pseudo::Active ? g_userState.active : g_userState.focus;
                if (std::find(chain.begin(), chain.end(), e) == chain.end()) return false;
                break;
            }
            case Pseudo::Focus:
            case Pseudo::FocusVisible:
                if (g_userState.focus.empty() || g_userState.focus[0] != e || (p.k == Pseudo::FocusVisible && !g_userState.focusVisible)) return false;
                break;
            case Pseudo::Checked: {
                bool on = false;
                if (e->name == "input" || e->name == "option") on = g_userState.checked ? g_userState.checked(e) : (e->attr("checked") || e->attr("selected"));
                if (!on) return false;
                break;
            }
            case Pseudo::Disabled:
            case Pseudo::Enabled: {
                if (!form_control(e)) return false;
                if (actually_disabled(e) != (p.k == Pseudo::Disabled)) return false;
                break;
            }
            case Pseudo::Required:
            case Pseudo::Optional: {
                if (!(e->name == "input" || e->name == "select" || e->name == "textarea")) return false;
                if ((e->attr("required") != nullptr) != (p.k == Pseudo::Required)) return false;
                break;
            }
            case Pseudo::ReadOnly:
            case Pseudo::ReadWrite: {
                bool editable = (e->name == "textarea" || (e->name == "input" && text_input(e))) && !e->attr("readonly") && !actually_disabled(e);
                if (!editable) {
                    const std::string* ce = e->attr("contenteditable");
                    editable = ce && (ce->empty() || lower(*ce) == "true" || lower(*ce) == "plaintext-only");
                }
                if (editable != (p.k == Pseudo::ReadWrite)) return false;
                break;
            }
            case Pseudo::PlaceholderShown: {
                if (!((e->name == "input" && text_input(e)) || e->name == "textarea") || !e->attr("placeholder")) return false;
                bool empty = g_userState.valueEmpty ? g_userState.valueEmpty(e) : !(e->attr("value") && !e->attr("value")->empty());
                if (!empty) return false;
                break;
            }
            case Pseudo::Target:
                if (!g_userState.target || g_userState.target != e) return false;
                break;
            case Pseudo::Lang: {  // the nearest lang attribute, matched as a language range prefix (Selectors 4, 7.2)
                std::string lang;
                for (const Node* n = e; n; n = n->parent)
                    if (const std::string* a = n->type == Node::T::Element ? n->attr("lang") : nullptr) {
                        lang = lower(*a);
                        break;
                    }
                if (!(lang == p.arg || (lang.size() > p.arg.size() && lang.compare(0, p.arg.size(), p.arg) == 0 && lang[p.arg.size()] == '-')))
                    return false;
                break;
            }
            case Pseudo::Never:
                return false;
            case Pseudo::Host:
            case Pseudo::Slotted: {
                if (!g_selectorScope || (p.k == Pseudo::Host ? e != g_selectorScope->parent : e->parent != g_selectorScope->parent)) return false;
                if (p.inner) {  // the argument is about the element as its own tree sees it
                    const Node* scope = g_selectorScope;
                    g_selectorScope = nullptr;
                    bool ok = match_compound(*p.inner, e);
                    g_selectorScope = scope;
                    if (!ok) return false;
                }
                break;
            }
            case Pseudo::Defined:  // every built-in element, and a custom one once it has been upgraded (HTML 4.16.4)
                if (e->ceState != 1 && e->name.find('-') != std::string::npos) return false;
                break;
        }
    }
    return true;
}

// The parent a selector sees: a tree ends at its shadow root, above which only the scope's own host shows.
const Node* selector_parent(const Node* e) {
    if (g_selectorScope && e == g_selectorScope->parent) return nullptr;
    const Node* par = e->parent;
    if (par && par->isShadowRoot()) return par == g_selectorScope ? par->parent : nullptr;
    return par;
}
bool match_at(const Selector& s, int idx, const Node* e, int budget) {
    if (!match_compound(s.parts[idx], e)) return false;
    if (idx == 0) return true;
    char comb = s.combs[idx - 1];
    const Node* par = selector_parent(e);
    if (comb == '>') return par && par->type == Node::T::Element && match_at(s, idx - 1, par);
    if (comb == ' ') {
        for (const Node* p = par; p && p->type == Node::T::Element; p = selector_parent(p))
            if (match_at(s, idx - 1, p)) return true;
        return false;
    }
    par = e->parent;  // siblings are those of the real tree
    if (!par) return false;
    if (comb == '+') {
        const Node* prev = nullptr;
        for (auto& k : par->kids) {
            if (k.get() == e) break;
            if (k->type == Node::T::Element) prev = k.get();
        }
        return prev && match_at(s, idx - 1, prev);
    }
    if (comb == '~') {
        for (auto& k : par->kids) {
            if (k.get() == e) break;
            if (k->type == Node::T::Element && match_at(s, idx - 1, k.get())) return true;
        }
    }
    return false;
}


std::string subst_vars(std::string_view v, const Style& s, int depth) {
    std::string out;
    size_t i = 0;
    while (i < v.size()) {
        size_t p = v.find("var(", i);
        if (p == npos) {
            out.append(v.substr(i));
            break;
        }
        out.append(v.substr(i, p - i));
        size_t j = p + 4, k = j;
        int d = 1;
        for (; k < v.size(); ++k) {
            if (v[k] == '(') ++d;
            if (v[k] == ')' && --d == 0) break;
        }
        std::string_view inner = v.substr(j, k - j);
        size_t comma = inner.find(',');
        std::string name(trim(inner.substr(0, comma)));
        std::string fallback = comma != npos ? std::string(trim(inner.substr(comma + 1))) : std::string();
        std::string rep = fallback;
        if (s.vars)
            if (const std::string* found = s.vars->find(name)) rep = *found;
        if (depth < 8 && rep.find("var(") != std::string::npos) rep = subst_vars(rep, s, depth + 1);
        out += rep;
        i = k < v.size() ? k + 1 : k;
    }
    return out;
}

bool inherited_prop(std::string_view p) {
    return p == "color" || p == "fill" || p == "stroke" || istarts(p, "fill-") || istarts(p, "stroke-") || istarts(p, "font") || p == "line-height" || p == "text-align" || p == "white-space" ||
           istarts(p, "list-style") || p == "text-transform" || p == "visibility" || istarts(p, "text-decoration") ||
           p == "letter-spacing" || p == "word-spacing" || p == "text-indent" || p == "overflow-wrap" || p == "word-wrap" ||
           p == "word-break";
}

void copy_prop(Style& d, const Style& s, std::string_view p) {
    if (p == "color") d.color = s.color;
    else if (istarts(p, "background")) {
        if (p == "background" || p == "background-color") d.bg = s.bg;
        if (p != "background-color") d.bgImages = s.bgImages;
    }
    else if (p == "font-size") d.fontSize = s.fontSize;
    else if (p == "font-weight") d.weight = s.weight;
    else if (p == "font-style") d.italic = s.italic;
    else if (p == "font-family") d.family = s.family;
    else if (p == "font") {
        d.fontSize = s.fontSize;
        d.weight = s.weight;
        d.italic = s.italic;
        d.family = s.family;
        d.lhPx = s.lhPx;
        d.lhFactor = s.lhFactor;
    } else if (p == "line-height") {
        d.lhPx = s.lhPx;
        d.lhFactor = s.lhFactor;
    } else if (p == "text-align") d.textAlign = s.textAlign;
    else if (p == "display") d.display = s.display;
    else if (p == "visibility") d.hidden = s.hidden;
    else if (p == "white-space") d.whiteSpace = s.whiteSpace;
    else if (istarts(p, "text-decoration")) {
        d.underline = s.underline;
        d.strike = s.strike;
    } else if (istarts(p, "list-style")) d.listStyle = s.listStyle;
    else if (p == "text-transform") d.textTransform = s.textTransform;
    else if (p == "letter-spacing") d.letterSpacing = s.letterSpacing;
    else if (p == "word-spacing") d.wordSpacing = s.wordSpacing;
    else if (p == "text-indent") d.textIndent = s.textIndent;
    else if (p == "overflow-wrap" || p == "word-wrap") d.overflowWrap = s.overflowWrap;
    else if (p == "word-break") d.wordBreak = s.wordBreak;
    else if (p == "text-overflow") d.textOverflow = s.textOverflow;
    else if (p == "aspect-ratio") d.aspectRatio = s.aspectRatio;
    else if (p == "object-fit") d.objectFit = s.objectFit;
    else if (p == "object-position") std::copy(s.objectPos, s.objectPos + 2, d.objectPos);
    else if (p == "vertical-align") d.vAlign = s.vAlign;
    else if (p == "box-sizing") d.borderBox = s.borderBox;
    else if (istarts(p, "margin")) std::copy(s.margin, s.margin + 4, d.margin);
    else if (istarts(p, "padding")) std::copy(s.padding, s.padding + 4, d.padding);
    else if (istarts(p, "border")) {
        std::copy(s.border, s.border + 4, d.border);
        std::copy(s.borderStyle, s.borderStyle + 4, d.borderStyle);
        std::copy(s.borderColor, s.borderColor + 4, d.borderColor);
    } else if (p == "width") d.width = s.width;
    else if (p == "height") d.height = s.height;
    else if (p == "min-width") d.minWidth = s.minWidth;
    else if (p == "max-width") d.maxWidth = s.maxWidth;
    else if (p == "min-height") d.minHeight = s.minHeight;
    else if (p == "max-height") d.maxHeight = s.maxHeight;
    else if (p == "position") d.position = s.position;
    else if (p == "transform") d.transform = s.transform;
    else if (p == "fill") d.fill = s.fill;
    else if (p == "stroke") d.stroke = s.stroke;
    else if (p == "stroke-width") d.strokeWidth = s.strokeWidth;
    else if (p == "fill-rule") d.fillEvenOdd = s.fillEvenOdd;
    else if (p == "fill-opacity") d.fillOpacity = s.fillOpacity;
    else if (p == "stroke-opacity") d.strokeOpacity = s.strokeOpacity;
    else if (p == "transform-origin") std::copy(s.transformOrigin, s.transformOrigin + 2, d.transformOrigin);
    else if (p == "top" || p == "right" || p == "bottom" || p == "left" || p == "inset") std::copy(s.inset, s.inset + 4, d.inset);
    else if (p == "z-index") {
        d.zIndex = s.zIndex;
        d.zAuto = s.zAuto;
    } else if (istarts(p, "overflow")) d.overflow = s.overflow;
    else if (p == "float") d.floatSide = s.floatSide;
    else if (p == "clear") d.clear = s.clear;
    else if (p == "border-radius") d.radius = s.radius;
    else if (p == "flex") {
        d.flexGrow = s.flexGrow;
        d.flexShrink = s.flexShrink;
        d.flexBasis = s.flexBasis;
    } else if (p == "flex-direction") d.flexDir = s.flexDir;
    else if (p == "flex-wrap") d.flexWrap = s.flexWrap;
    else if (p == "justify-content") {
        d.justify = s.justify;
        d.justifyNormal = s.justifyNormal;
    } else if (p == "align-content") {
        d.alignContent = s.alignContent;
        d.alignContentNormal = s.alignContentNormal;
    } else if (p == "align-items") d.alignItems = s.alignItems;
    else if (p == "align-self") d.alignSelf = s.alignSelf;
    else if (p == "justify-items") d.justifyItems = s.justifyItems;
    else if (p == "justify-self") d.justifySelf = s.justifySelf;
    else if (p == "clip-path") d.clipPath = s.clipPath;
    else if (istarts(p, "animation")) {
        d.animLists = s.animLists;
        d.animated = s.animated;
    } else if (istarts(p, "transition")) d.transitionLists = s.transitionLists;
    else if (p == "box-shadow") d.boxShadow = s.boxShadow;
    else if (p == "text-shadow") d.textShadow = s.textShadow;
    else if (istarts(p, "outline")) {
        d.outlineStyle = s.outlineStyle;
        d.outlineWidth = s.outlineWidth;
        d.outlineOffset = s.outlineOffset;
        d.outlineColor = s.outlineColor;
    }
    else if (istarts(p, "grid-row") || istarts(p, "grid-column") || p == "grid-area") {
        std::copy(s.gridRow, s.gridRow + 2, d.gridRow);
        std::copy(s.gridCol, s.gridCol + 2, d.gridCol);
    } else if (istarts(p, "grid")) d.grid = s.grid;
    else if (p == "gap") {
        d.gapRow = s.gapRow;
        d.gapCol = s.gapCol;
    }
}

void inherit(Style& s, const Style& p) {
    s.alpha = p.alpha;
    s.fill = p.fill;
    s.stroke = p.stroke;
    s.strokeWidth = p.strokeWidth;
    s.fillEvenOdd = p.fillEvenOdd;
    s.fillOpacity = p.fillOpacity;
    s.strokeOpacity = p.strokeOpacity;
    s.color = p.color;
    s.textShadow = p.textShadow;
    s.fontSize = p.fontSize;
    s.weight = p.weight;
    s.italic = p.italic;
    s.family = p.family;
    s.lhPx = p.lhPx;
    s.lhFactor = p.lhFactor;
    s.textAlign = p.textAlign;
    s.centerBlocks = p.centerBlocks;
    s.underline = p.underline;
    s.strike = p.strike;
    s.whiteSpace = p.whiteSpace;
    s.listStyle = p.listStyle;
    s.textTransform = p.textTransform;
    s.letterSpacing = p.letterSpacing;
    s.wordSpacing = p.wordSpacing;
    s.textIndent = p.textIndent;
    s.overflowWrap = p.overflowWrap;
    s.wordBreak = p.wordBreak;
    s.hidden = p.hidden;
    s.vars = p.vars;
}

bool parse_display(const std::string& v, Display& d) {
    if (v == "none") d = Display::None;
    else if (in(v, {"flex", "block flex", "-webkit-box", "-ms-flexbox", "-webkit-flex"})) d = Display::Flex;
    else if (in(v, {"inline-flex", "inline flex", "inline-flexbox", "-webkit-inline-box", "-webkit-inline-flex"}))
        d = Display::InlineFlex;
    else if (v == "grid" || v == "block grid") d = Display::Grid;
    else if (v == "inline-grid" || v == "inline grid") d = Display::InlineGrid;
    else if (in(v, {"block", "flow-root", "block flow", "table-caption", "block flow-root"}))
        d = Display::Block;
    else if (in(v, {"inline", "contents", "inline flow", "ruby", "ruby-text"})) d = Display::Inline;
    else if (in(v, {"inline-block", "inline flow-root"})) d = Display::InlineBlock;
    else if (v == "list-item" || v == "block list-item") d = Display::ListItem;
    else if (v == "table" || v == "inline-table") d = Display::Table;
    else if (in(v, {"table-row-group", "table-header-group", "table-footer-group"})) d = Display::RowGroup;
    else if (v == "table-row") d = Display::Row;
    else if (v == "table-cell") d = Display::Cell;
    else if (v == "table-column" || v == "table-column-group") d = Display::None;
    else return false;
    return true;
}

bool font_size(const std::string& v, float pfs, float& out) {
    static const std::pair<const char*, float> kw[] = {{"xx-small", 9.f},  {"x-small", 10.f},  {"small", 13.f},     {"medium", 16.f},
                                                       {"large", 18.f},    {"x-large", 24.f},  {"xx-large", 32.f},  {"xxx-large", 48.f}};
    for (auto& k : kw)
        if (v == k.first) {
            out = k.second;
            return true;
        }
    if (v == "smaller") {
        out = pfs / 1.2f;
        return true;
    }
    if (v == "larger") {
        out = pfs * 1.2f;
        return true;
    }
    Len l;
    if (!parse_len(v, pfs, l, false)) return false;
    if (l.u == Len::Px) out = l.v;
    else if (l.u == Len::Pct) out = pfs * l.v / 100.f + l.off;
    else return false;
    if (out < 0) return false;
    out = std::min(out, 1000.f);
    return true;
}

void apply_line_height(Style& s, const std::string& lv) {
    if (lv == "normal") {
        s.lhPx = -1;
        s.lhFactor = 1.2f;
        return;
    }
    float v;
    std::string_view u;
    if (parse_num(lv, v, u) && u.empty()) {
        s.lhPx = -1;
        s.lhFactor = std::max(0.f, v);
        return;
    }
    Len l;
    if (parse_len(lv, s.fontSize, l, false)) {
        if (l.u == Len::Px) s.lhPx = std::max(0.f, l.v);
        else if (l.u == Len::Pct) s.lhPx = s.fontSize * l.v / 100.f;
    }
}

std::wstring resolve_family(std::string_view list) {
    if (StyleEngine::fontResolver) return StyleEngine::fontResolver(list);
    return L"Times New Roman";
}

void font_shorthand(Style& s, const Style& parent, const std::string& val) {
    std::string lv = lower(trim(val));
    if (in(lv, {"caption", "icon", "menu", "message-box", "small-caption", "status-bar"})) {
        s.family = resolve_family("system-ui");
        s.fontSize = 13.33f;
        return;
    }
    auto T = tokens(val);
    bool italic = false;
    uint16_t w = 400;
    size_t i = 0;
    bool ok = false;
    for (; i < T.size(); ++i) {
        std::string t = lower(T[i]);
        if (t == "italic" || t == "oblique") {
            italic = true;
            continue;
        }
        if (t == "normal" || t == "small-caps" || contains(t, "condensed") || contains(t, "expanded")) continue;
        if (t == "bold" || t == "bolder") {
            w = 700;
            continue;
        }
        if (t == "lighter") {
            w = 300;
            continue;
        }
        if (t.size() == 3 && isdigit_(t[0]) && t[1] == '0' && t[2] == '0') {
            w = (uint16_t)atoi(t.c_str());
            continue;
        }
        std::string sz = t, lh;
        size_t sl = t.find('/');
        if (sl != std::string::npos) {
            sz = t.substr(0, sl);
            lh = t.substr(sl + 1);
            if (lh.empty() && i + 1 < T.size()) lh = lower(T[++i]);
        } else if (i + 1 < T.size() && T[i + 1][0] == '/') {
            lh = lower(T[++i].substr(1));
            if (lh.empty() && i + 1 < T.size()) lh = lower(T[++i]);
        }
        float v;
        if (!font_size(sz, parent.fontSize, v)) return;
        s.fontSize = v;
        s.italic = italic;
        s.weight = w;
        s.lhPx = -1;
        s.lhFactor = 1.2f;
        if (!lh.empty()) apply_line_height(s, lh);
        ++i;
        ok = true;
        break;
    }
    if (!ok) return;
    std::string fam;
    for (; i < T.size(); ++i) {
        if (!fam.empty()) fam += ' ';
        fam += T[i];
    }
    if (!fam.empty()) s.family = resolve_family(fam);
}

void set4(Len* arr, const std::vector<std::string>& t, float fs, bool allowAuto) {
    size_t n = t.size();
    if (n < 1 || n > 4) return;
    Len v[4];
    for (size_t i = 0; i < n; ++i)
        if (!parse_len(t[i], fs, v[i], allowAuto)) return;
    arr[0] = v[0];
    arr[1] = n > 1 ? v[1] : v[0];
    arr[2] = n > 2 ? v[2] : v[0];
    arr[3] = n > 3 ? v[3] : arr[1];
}

bool parse_bw(std::string_view s, float fs, float& w) {
    if (s == "thin") w = 1;
    else if (s == "medium") w = 3;
    else if (s == "thick") w = 5;
    else {
        Len x;
        if (!parse_len(s, fs, x, false) || x.u != Len::Px) return false;
        w = std::max(0.f, x.v);
    }
    return true;
}

int parse_bs(std::string_view s) {
    if (s == "none" || s == "hidden") return 0;
    if (s == "solid") return 1;
    if (s == "dashed") return 2;
    if (s == "dotted") return 3;
    if (s == "double") return 4;
    if (s == "groove" || s == "ridge" || s == "inset" || s == "outset") return 5;
    return -1;
}

bool parse_color_cc(std::string_view t, Color& c) {
    if (t == "currentcolor") {
        c = {0, 0, 0, -1};
        return true;
    }
    return parse_color(t, c);
}

void border_side(Style& s, int side, const std::vector<std::string>& T) {
    float w = 3;
    int st = 0;
    Color c{0, 0, 0, -1};
    for (auto& t : T) {
        int b = parse_bs(t);
        if (b >= 0) {
            st = b;
            continue;
        }
        float bw;
        if (parse_bw(t, s.fontSize, bw)) {
            w = bw;
            continue;
        }
        Color cc;
        if (parse_color_cc(t, cc)) c = cc;
    }
    s.border[side] = w;
    s.borderStyle[side] = (uint8_t)st;
    s.borderColor[side] = c;
}

int side_of(std::string_view s) {
    if (s == "top" || s == "block-start") return 0;
    if (s == "right" || s == "inline-end") return 1;
    if (s == "bottom" || s == "block-end") return 2;
    if (s == "left" || s == "inline-start") return 3;
    return -1;
}

int list_kw(std::string_view t) {
    if (t == "disc") return 0;
    if (t == "circle") return 1;
    if (t == "square") return 2;
    if (t == "decimal") return 3;
    if (t == "decimal-leading-zero") return 9;
    if (t == "lower-greek") return 10;
    if (t == "none") return 4;
    if (t == "lower-alpha" || t == "lower-latin") return 5;
    if (t == "upper-alpha" || t == "upper-latin") return 6;
    if (t == "lower-roman") return 7;
    if (t == "upper-roman") return 8;
    return -1;
}

// ---- CSS Grid 1, section 7: track lists, areas and placement
// The component values of a track list, with each [line names] kept as one token
std::vector<std::string> grid_tokens(std::string_view v) {
    std::vector<std::string> out;
    for (auto& t : tokens(v)) {
        if (!out.empty() && out.back()[0] == '[' && out.back().back() != ']') out.back() += " " + t;
        else out.push_back(t);
    }
    return out;
}
bool parse_breadth(std::string_view t, float fs, GridBreadth& b) {
    std::string l = lower(t);
    float n;
    std::string_view u;
    if (l == "auto") b.k = GridBreadth::Auto;
    else if (l == "min-content") b.k = GridBreadth::MinContent;
    else if (l == "max-content") b.k = GridBreadth::MaxContent;
    else if (parse_num(l, n, u) && u == "fr" && n >= 0) {
        b.k = GridBreadth::Fr;
        b.fr = n;
    } else if (parse_len(l, fs, b.len, false) && b.len.definite() && b.len.v >= 0) b.k = GridBreadth::Length;
    else return false;
    return true;
}
bool parse_track(std::string_view t, float fs, GridTrack& tr) {
    std::string l = lower(t);
    if (istarts(l, "minmax(") && l.back() == ')') {
        auto args = split_top(std::string_view(l).substr(7, l.size() - 8), ',');
        return args.size() == 2 && parse_breadth(trim(args[0]), fs, tr.min) && tr.min.k != GridBreadth::Fr &&
               parse_breadth(trim(args[1]), fs, tr.max);
    }
    if (istarts(l, "fit-content(") && l.back() == ')') {
        tr.min.k = GridBreadth::Auto;
        tr.max.k = GridBreadth::MaxContent;
        return parse_len(std::string_view(l).substr(12, l.size() - 13), fs, tr.fitContent, false) && tr.fitContent.definite();
    }
    if (!parse_breadth(l, fs, tr.max)) return false;
    tr.min = tr.max;
    if (tr.max.k == GridBreadth::Fr) tr.min = GridBreadth{};  // a flexible track has an automatic minimum
    return true;
}
std::vector<std::string> line_names(std::string_view t) {  // "[a b]"
    std::vector<std::string> out;
    for (auto& n : tokens(t.substr(1, t.size() - 2))) out.push_back(n);
    return out;
}
// grid-template-columns / -rows (7.2): "none" or a track list with names, repeat(n, ...) and one repeat(auto-fill
// or auto-fit, ...) of fixed tracks
bool parse_track_list(std::string_view v, float fs, GridTrackList& out, bool allowRepeat = true) {
    out = GridTrackList{};
    if (lower(trim(v)) == "none") return true;
    for (auto& t : grid_tokens(v)) {
        std::string l = lower(t);
        if (t[0] == '[') {
            if (t.back() != ']') return false;
            for (auto& n : line_names(t)) out.names.back().push_back(n);
        } else if (istarts(l, "repeat(") && l.back() == ')') {
            if (!allowRepeat) return false;
            std::string inner = t.substr(7, t.size() - 8);
            size_t comma = inner.find(',');
            if (comma == std::string::npos) return false;
            std::string count = lower(trim(std::string_view(inner).substr(0, comma)));
            GridTrackList sub;
            if (!parse_track_list(std::string_view(inner).substr(comma + 1), fs, sub, false) || sub.tracks.empty()) return false;
            if (count == "auto-fill" || count == "auto-fit") {
                if (out.autoAt >= 0) return false;
                out.autoAt = (int)out.tracks.size();
                out.autoFit = count == "auto-fit";
                out.autoTracks = sub.tracks;
                out.autoNames = sub.names;
                out.names.push_back({});  // the line after the group
                continue;
            }
            int n = atoi(count.c_str());
            if (n < 1 || n > 10000) return false;
            for (int k = 0; k < n; ++k) {
                for (auto& nm : sub.names[0]) out.names.back().push_back(nm);
                for (size_t i = 0; i < sub.tracks.size(); ++i) {
                    out.tracks.push_back(sub.tracks[i]);
                    out.names.push_back(sub.names[i + 1]);
                }
            }
        } else {
            GridTrack tr;
            if (!parse_track(t, fs, tr)) return false;
            out.tracks.push_back(tr);
            out.names.push_back({});
        }
    }
    return true;
}
bool parse_auto_tracks(std::string_view v, float fs, std::vector<GridTrack>& out) {  // grid-auto-columns / -rows
    std::vector<GridTrack> r;
    for (auto& t : tokens(v)) {
        GridTrack tr;
        if (!parse_track(t, fs, tr)) return false;
        r.push_back(tr);
    }
    if (r.empty()) return false;
    out = std::move(r);
    return true;
}
// grid-template-areas (7.3): each string a row, each name a cell, "." an empty cell; every name a rectangle
bool parse_areas(std::string_view v, GridTemplate& g) {
    g.areas.clear();
    g.areaRows = g.areaCols = 0;
    if (lower(trim(v)) == "none") return true;
    std::vector<std::vector<std::string>> rows;
    for (auto& t : tokens(v)) {
        if (t.size() < 2 || (t[0] != '"' && t[0] != '\'')) return false;
        std::vector<std::string> cells;
        std::string inner = t.substr(1, t.size() - 2);
        size_t i = 0;
        while (i < inner.size()) {
            if (isws(inner[i])) { ++i; continue; }
            size_t st = i;
            if (inner[i] == '.') {
                while (i < inner.size() && inner[i] == '.') ++i;
                cells.push_back("");
            } else {
                while (i < inner.size() && !isws(inner[i]) && inner[i] != '.') ++i;
                cells.push_back(inner.substr(st, i - st));
            }
        }
        if (!rows.empty() && cells.size() != rows[0].size()) return false;
        if (cells.empty()) return false;
        rows.push_back(std::move(cells));
    }
    std::vector<GridArea> areas;
    for (int r = 0; r < (int)rows.size(); ++r)
        for (int c = 0; c < (int)rows[r].size(); ++c) {
            const std::string& n = rows[r][c];
            if (n.empty()) continue;
            auto it = std::find_if(areas.begin(), areas.end(), [&](const GridArea& a) { return a.name == n; });
            if (it == areas.end()) areas.push_back({n, r, c, r + 1, c + 1});
            else {
                it->row1 = std::max(it->row1, r + 1);
                it->col0 = std::min(it->col0, c);
                it->col1 = std::max(it->col1, c + 1);
            }
        }
    for (auto& a : areas)  // a name that does not fill its rectangle makes the declaration invalid
        for (int r = a.row0; r < a.row1; ++r)
            for (int c = a.col0; c < a.col1; ++c)
                if (rows[r][c] != a.name) return false;
    g.areas = std::move(areas);
    g.areaRows = (int)rows.size();
    g.areaCols = rows.empty() ? 0 : (int)rows[0].size();
    return true;
}
// grid-template (7.4): none, <rows> / <columns>, or rows of area strings, each with its height
bool parse_grid_template(std::string_view v, float fs, GridTemplate& g) {
    g.cols = g.rows = GridTrackList{};
    g.areas.clear();
    g.areaRows = g.areaCols = 0;
    if (lower(trim(v)) == "none") return true;
    auto parts = split_top(v, '/');
    if (parts.size() > 2) return false;
    if (v.find('"') == std::string_view::npos && v.find('\'') == std::string_view::npos)
        return parts.size() == 2 && parse_track_list(parts[0], fs, g.rows) && parse_track_list(parts[1], fs, g.cols);
    std::string strings;
    GridTrackList rows;
    bool pendingRow = false;
    for (auto& t : grid_tokens(parts[0])) {
        if (t[0] == '"' || t[0] == '\'') {
            if (pendingRow) {
                rows.tracks.push_back(GridTrack{});
                rows.names.push_back({});
            }
            strings += t + " ";
            pendingRow = true;
        } else if (t[0] == '[') {
            for (auto& n : line_names(t)) rows.names.back().push_back(n);
        } else {
            GridTrack tr;
            if (!pendingRow || !parse_track(t, fs, tr)) return false;
            rows.tracks.push_back(tr);
            rows.names.push_back({});
            pendingRow = false;
        }
    }
    if (pendingRow) {
        rows.tracks.push_back(GridTrack{});
        rows.names.push_back({});
    }
    if (!parse_areas(strings, g)) return false;
    g.rows = std::move(rows);
    return parts.size() == 1 || parse_track_list(parts[1], fs, g.cols, false);
}
// grid (7.8): a grid-template, or auto-flow on one side with the auto tracks of the other axis
bool parse_grid(std::string_view v, float fs, GridTemplate& g) {
    g = GridTemplate{};
    std::string l = lower(v);
    if (l.find("auto-flow") == std::string::npos) return parse_grid_template(v, fs, g);
    auto parts = split_top(v, '/');
    if (parts.size() != 2) return false;
    for (int side = 0; side < 2; ++side) {
        std::string p = lower(trim(parts[side]));
        if (p.find("auto-flow") == std::string::npos) continue;
        g.flowColumn = side == 1;
        std::string rest;
        for (auto& t : tokens(parts[side])) {
            std::string tl = lower(t);
            if (tl == "auto-flow") continue;
            if (tl == "dense") g.dense = true;
            else rest += t + " ";
        }
        if (!trim(rest).empty() && !parse_auto_tracks(rest, fs, side == 0 ? g.autoRows : g.autoCols)) return false;
        return parse_track_list(parts[1 - side], fs, side == 0 ? g.cols : g.rows);
    }
    return false;
}
// <grid-line> (8.3): auto | <integer> && <ident>? | span && [<integer> || <ident>] | <ident>
bool parse_grid_line(std::string_view v, GridLine& out) {
    out = GridLine{};
    auto T = tokens(v);
    if (T.size() == 1 && lower(T[0]) == "auto") return true;
    bool span = false;
    for (auto& t : T) {
        std::string l = lower(t);
        float n;
        std::string_view u;
        if (l == "span") span = true;
        else if (parse_num(l, n, u) && u.empty() && n == (int)n && n != 0) out.line = (int)n;
        else if (l == "auto" || l == "span") return false;
        else out.name = t;
    }
    if (T.empty()) return false;
    if (span) {
        out.span = out.line ? out.line : 1;
        if (out.span < 0) return false;
        out.line = 0;
    }
    return true;
}
// what an omitted end of grid-row, grid-column or grid-area becomes (8.4): the same name, or auto
GridLine grid_line_default(const GridLine& from) {
    GridLine g;
    if (from.line == 0 && from.span == 0 && !from.name.empty()) g.name = from.name;
    return g;
}
uint8_t self_alignment(const std::string& v) {  // 0 stretch (normal), 1 start, 2 center, 3 end
    std::string t = istarts(v, "safe ") || istarts(v, "unsafe ") ? v.substr(v.find(' ') + 1) : v;
    if (in(t, {"start", "flex-start", "self-start", "left", "baseline", "first baseline", "last baseline"})) return 1;
    if (t == "center") return 2;
    if (in(t, {"end", "flex-end", "self-end", "right"})) return 3;
    return 0;
}
uint8_t content_distribution(const std::string& v) {  // as Style::justify
    std::string t = istarts(v, "safe ") || istarts(v, "unsafe ") ? v.substr(v.find(' ') + 1) : v;
    return t == "center" ? 1 : (t == "flex-end" || t == "end" || t == "right") ? 2 : t == "space-between" ? 3 : t == "space-around" ? 4
                                                                                 : t == "space-evenly" ? 5 : 0;
}

// box-shadow and text-shadow: none, or shadows of two to four lengths, a colour, and for a box inset
bool parse_shadows(std::string_view v, float fs, bool box, std::shared_ptr<const Shadows>& out) {
    std::string l = lower(trim(v));
    if (l == "none") {
        out = nullptr;
        return true;
    }
    auto list = std::make_shared<Shadows>();
    for (auto part : split_top(l, ',')) {
        Shadow sh;
        std::vector<float> lens;
        for (auto& t : tokens(part)) {
            Len len;
            Color c;
            if (box && t == "inset") sh.inset = true;
            else if (parse_len(t, fs, len, false) && len.u == Len::Px) lens.push_back(len.v);
            else if (t == "currentcolor") sh.currentColor = true;
            else if (parse_color(t, c)) {
                sh.c = c;
                sh.currentColor = false;
            } else
                return false;
        }
        if (lens.size() < 2 || lens.size() > (box ? 4u : 3u)) return false;
        sh.dx = lens[0];
        sh.dy = lens[1];
        sh.blur = lens.size() > 2 ? std::max(0.f, lens[2]) : 0;
        sh.spread = lens.size() > 3 ? lens[3] : 0;
        list->push_back(sh);
    }
    out = std::move(list);
    return true;
}

// clip-path (CSS Masking 1, 3.1): none, or a basic shape and a reference box in either order. A url() to an SVG
// clipPath is not kept: it clips nothing.
bool parse_clip_path(std::string_view v, float fs, std::shared_ptr<const ClipShape>& out) {
    std::string l = lower(trim(v));
    out = nullptr;
    if (l == "none" || istarts(l, "url(")) return true;
    auto shape = std::make_shared<ClipShape>();
    bool any = false;
    for (auto& t : tokens(l)) {
        if (t == "border-box" || t == "padding-box" || t == "content-box" || t == "margin-box" || t == "fill-box" ||
            t == "stroke-box" || t == "view-box") {
            shape->box = t == "padding-box" ? 1 : t == "content-box" ? 2 : t == "margin-box" ? 3 : 0;
            continue;
        }
        size_t open = t.find('(');
        if (open == std::string::npos || t.back() != ')') return false;
        std::string fn = t.substr(0, open), args = t.substr(open + 1, t.size() - open - 2);
        any = true;
        if (fn == "inset") {  // inset(<length-percentage>{1,4} [round <border-radius>]?)
            shape->k = ClipShape::Inset;
            std::vector<std::string> T = tokens(args), lens;
            size_t i = 0;
            for (; i < T.size() && T[i] != "round"; ++i) lens.push_back(T[i]);
            if (lens.empty() || lens.size() > 4) return false;
            Len L[4];
            for (size_t k = 0; k < lens.size(); ++k)
                if (!parse_len(lens[k], fs, L[k], false)) return false;
            shape->len[0] = L[0];
            shape->len[1] = lens.size() > 1 ? L[1] : L[0];
            shape->len[2] = lens.size() > 2 ? L[2] : L[0];
            shape->len[3] = lens.size() > 3 ? L[3] : shape->len[1];
            if (i + 1 < T.size() && !parse_len(T[i + 1], fs, shape->round, false)) return false;
        } else if (fn == "circle" || fn == "ellipse") {  // circle(<radius>? [at <position>]?)
            shape->k = fn == "circle" ? ClipShape::Circle : ClipShape::Ellipse;
            std::vector<std::string> T = tokens(args);
            size_t i = 0;
            int nRadii = fn == "circle" ? 1 : 2;
            for (int r = 0; r < nRadii; ++r) {
                if (i >= T.size() || T[i] == "at") {
                    shape->radiusKw[r] = 1;
                    continue;
                }
                if (T[i] == "closest-side") shape->radiusKw[r] = 1;
                else if (T[i] == "farthest-side") shape->radiusKw[r] = 2;
                else if (!parse_len(T[i], fs, shape->len[r], false)) return false;
                ++i;
            }
            if (i < T.size()) {
                if (T[i] != "at" || !parse_position(std::vector<std::string>(T.begin() + i + 1, T.end()), fs, shape->cx, shape->cy)) return false;
            }
        } else if (fn == "polygon") {  // polygon(<fill-rule>?, [<x> <y>]#)
            shape->k = ClipShape::Polygon;
            auto parts = split_top(args, ',');
            for (size_t k = 0; k < parts.size(); ++k) {
                std::string part = lower(trim(parts[k]));
                if (k == 0 && (part == "evenodd" || part == "nonzero")) {
                    shape->evenOdd = part == "evenodd";
                    continue;
                }
                auto T = tokens(part);
                std::pair<Len, Len> pt;
                if (T.size() != 2 || !parse_len(T[0], fs, pt.first, false) || !parse_len(T[1], fs, pt.second, false)) return false;
                shape->points.push_back(pt);
            }
            if (shape->points.size() < 3) return false;
        } else
            return false;
    }
    if (!any) {  // a reference box alone clips to that box
        shape->k = ClipShape::Inset;
    }
    out = std::move(shape);
    return true;
}

void apply(Style& s, const Style& parent, std::string_view prop, std::string_view raw) {
    std::string val = raw.find("var(") != npos ? subst_vars(raw, s, 0) : std::string(raw);
    std::string lv = lower(trim(val));
    if (lv.empty()) return;
    if (lv == "inherit") {
        copy_prop(s, parent, prop);
        return;
    }
    if (lv == "initial" || lv == "unset" || lv == "revert" || lv == "revert-layer") {
        static const Style init;
        if (lv != "initial" && inherited_prop(prop))
            copy_prop(s, parent, prop);
        else
            copy_prop(s, init, prop);
        return;
    }
    const float fs = s.fontSize;
    auto T = tokens(lv);

    if (prop == "content") {
        s.content = std::string(trim(val));
    } else if (prop == "box-shadow" || prop == "-webkit-box-shadow" || prop == "text-shadow") {
        std::shared_ptr<const Shadows> list;
        if (parse_shadows(val, fs, prop != "text-shadow", list)) (prop == "text-shadow" ? s.textShadow : s.boxShadow) = std::move(list);
    } else if (prop == "outline" || prop == "outline-style" || prop == "outline-width" || prop == "outline-color") {
        // CSS UI 4, 4.1-4.4: the shorthand takes the three in any order
        for (auto& t : tokens(lv)) {
            Len len;
            Color c;
            if (prop != "outline-width" && prop != "outline-color" &&
                in(t, {"none", "hidden", "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset", "auto"}))
                s.outlineStyle = t == "none" || t == "hidden" ? 0 : t == "auto" ? 2 : 1;
            else if (prop != "outline-style" && prop != "outline-color" && (t == "thin" || t == "medium" || t == "thick"))
                s.outlineWidth = t == "thin" ? 1.f : t == "medium" ? 3.f : 5.f;
            else if (prop != "outline-style" && prop != "outline-color" && parse_len(t, fs, len, false) && len.u == Len::Px)
                s.outlineWidth = std::max(0.f, len.v);
            else if (prop != "outline-style" && prop != "outline-width" && (t == "currentcolor" || t == "invert" || t == "auto"))
                s.outlineColor = Color{0, 0, 0, -1};
            else if (prop != "outline-style" && prop != "outline-width" && parse_color(t, c))
                s.outlineColor = c;
        }
    } else if (prop == "outline-offset") {
        Len len;
        if (parse_len(lv, fs, len, false) && len.u == Len::Px) s.outlineOffset = len.v;
    } else if (prop == "clip-path" || prop == "-webkit-clip-path") {
        std::shared_ptr<const ClipShape> shape;
        if (parse_clip_path(val, fs, shape)) s.clipPath = std::move(shape);
    } else if (prop == "counter-reset") {
        s.counterReset = std::string(trim(val));
    } else if (prop == "counter-increment") {
        s.counterIncrement = std::string(trim(val));
    } else if (prop == "counter-set") {
        s.counterSet = std::string(trim(val));
    } else if (prop == "display") {
        Display d;
        if (parse_display(lv, d)) {
            s.display = d;
            s.flowRoot = lv == "flow-root" || lv == "block flow-root";
        }
    } else if (prop == "color") {
        Color c;
        if (lv == "currentcolor") s.color = parent.color;
        else if (parse_color(lv, c)) s.color = c;
    } else if (prop == "background-color") {
        Color c;
        if (lv == "currentcolor") s.bg = s.color;
        else if (parse_color(lv, c)) s.bg = c;
    } else if (prop == "background") {
        auto bg = std::make_shared<Background>();
        Color c{};
        bool hasColor = false;
        if (parse_background(trim(val), fs, *bg, c, hasColor)) {
            s.bg = hasColor ? c : Color{};
            bool any = false;
            for (auto& im : bg->images) any = any || im.k != BgImage::None;
            s.bgImages = any ? std::shared_ptr<const Background>(std::move(bg)) : nullptr;
        }
    } else if (istarts(prop, "background-")) {
        auto bg = s.bgImages ? std::make_shared<Background>(*s.bgImages) : std::make_shared<Background>();
        if (apply_bg_longhand(prop, trim(val), fs, *bg)) s.bgImages = std::move(bg);  // the longhands may come before the image
    } else if (prop == "font-size") {
        float v;
        if (font_size(lv, parent.fontSize, v)) s.fontSize = v;
    } else if (prop == "font-weight") {
        if (lv == "normal") s.weight = 400;
        else if (lv == "bold") s.weight = 700;
        else if (lv == "bolder") s.weight = parent.weight < 350 ? 400 : parent.weight < 550 ? 700 : 900;
        else if (lv == "lighter") s.weight = parent.weight < 550 ? 100 : parent.weight < 750 ? 400 : 700;
        else {
            int w = atoi(lv.c_str());
            if (w > 0) s.weight = (uint16_t)std::clamp(w, 1, 999);
        }
    } else if (prop == "font-style") {
        s.italic = lv == "italic" || istarts(lv, "oblique");
    } else if (prop == "font-family") {
        s.family = resolve_family(val);
    } else if (prop == "font") {
        font_shorthand(s, parent, val);
    } else if (prop == "line-height") {
        apply_line_height(s, lv);
    } else if (prop == "text-align") {
        if (lv == "left" || lv == "start" || lv == "justify" || lv == "-webkit-left") s.textAlign = 0;
        else if (lv == "right" || lv == "end" || lv == "-webkit-right") s.textAlign = 1;
        else if (lv == "center") s.textAlign = 2;
        else if (lv == "-webkit-center" || lv == "-moz-center") {
            s.textAlign = 2;
            s.centerBlocks = true;
        }
        if (lv != "-webkit-center" && lv != "-moz-center") s.centerBlocks = false;
    } else if (prop == "text-decoration" || prop == "text-decoration-line") {
        if (contains(lv, "none")) s.underline = s.strike = false;
        if (contains(lv, "underline")) s.underline = true;
        if (contains(lv, "line-through")) s.strike = true;
    } else if (prop == "white-space") {
        if (lv == "normal") s.whiteSpace = 0;
        else if (lv == "pre") s.whiteSpace = 1;
        else if (lv == "nowrap") s.whiteSpace = 2;
        else if (lv == "pre-wrap" || lv == "break-spaces") s.whiteSpace = 3;
        else if (lv == "pre-line") s.whiteSpace = 4;
    } else if (prop == "text-wrap" || prop == "text-wrap-mode") {
        if (lv == "nowrap" && s.whiteSpace == 0) s.whiteSpace = 2;
    } else if (prop == "list-style-type" || prop == "list-style") {
        for (auto& t : T) {
            int ls = list_kw(t);
            if (ls >= 0) s.listStyle = (uint8_t)ls;
        }
    } else if (prop == "letter-spacing" || prop == "word-spacing") {
        Len len;
        float px = 0;
        if (lv != "normal") {
            if (!parse_len(lv, fs, len, false) || len.u != Len::Px) return;
            px = len.v;
        }
        (prop == "letter-spacing" ? s.letterSpacing : s.wordSpacing) = px;
    } else if (prop == "text-indent") {
        Len len;
        if (!T.empty() && parse_len(T[0], fs, len, false)) s.textIndent = len;
    } else if (prop == "overflow-wrap" || prop == "word-wrap") {
        s.overflowWrap = lv == "break-word" || lv == "anywhere" ? 1 : 0;
    } else if (prop == "word-break") {
        s.wordBreak = lv == "break-all" ? 1 : lv == "keep-all" ? 2 : 0;
        if (lv == "break-word") s.overflowWrap = 1;  // a legacy value: normal with overflow-wrap: anywhere
    } else if (prop == "text-overflow") {
        s.textOverflow = T.size() == 1 && T[0] == "ellipsis" ? 1 : T.size() == 2 && T[1] == "ellipsis" ? 1 : 0;
    } else if (prop == "aspect-ratio") {  // auto | <ratio> | auto && <ratio>; the ratio applies to boxes that have none
        s.aspectRatio = 0;
        std::string r;
        for (auto& t : T)
            if (t != "auto") r += t;
        size_t slash = r.find('/');
        double a = atof(r.c_str()), b = slash == std::string::npos ? 1 : atof(r.c_str() + slash + 1);
        if (!r.empty() && a > 0 && b > 0) s.aspectRatio = (float)(a / b);
    } else if (prop == "object-fit") {
        s.objectFit = lv == "contain" ? 1 : lv == "cover" ? 2 : lv == "none" ? 3 : lv == "scale-down" ? 4 : 0;
    } else if (prop == "object-position") {
        Len x, y;
        if (parse_position(T, fs, x, y)) {
            s.objectPos[0] = x;
            s.objectPos[1] = y;
        }
    } else if (prop == "text-transform") {
        s.textTransform = lv == "uppercase" ? 1 : lv == "lowercase" ? 2 : lv == "capitalize" ? 3 : 0;
    } else if (prop == "vertical-align") {
        if (lv == "baseline") s.vAlign = 0;
        else if (lv == "top" || lv == "text-top") s.vAlign = 1;
        else if (lv == "middle") s.vAlign = 2;
        else if (lv == "bottom" || lv == "text-bottom") s.vAlign = 3;
        else if (lv == "sub") s.vAlign = 4;
        else if (lv == "super") s.vAlign = 5;
    } else if (prop == "visibility") {
        s.hidden = lv == "hidden" || lv == "collapse";
    } else if (prop == "box-sizing") {
        s.borderBox = lv == "border-box";
    } else if (prop == "margin") {
        set4(s.margin, T, fs, true);
    } else if (prop == "padding") {
        set4(s.padding, T, fs, false);
    } else if (istarts(prop, "margin-") || istarts(prop, "padding-")) {
        bool m = prop[0] == 'm';
        Len* arr = m ? s.margin : s.padding;
        std::string_view side = prop.substr(m ? 7 : 8);
        Len l;
        if (side == "inline" || side == "block") {
            Len v[2];
            if (T.empty() || T.size() > 2) return;
            for (size_t i = 0; i < T.size(); ++i)
                if (!parse_len(T[i], fs, v[i], m)) return;
            if (T.size() == 1) v[1] = v[0];
            if (side == "inline") {
                arr[3] = v[0];
                arr[1] = v[1];
            } else {
                arr[0] = v[0];
                arr[2] = v[1];
            }
            return;
        }
        int i = side_of(side);
        if (i >= 0 && parse_len(lv, fs, l, m)) arr[i] = l;
    } else if (prop == "width" || prop == "inline-size") {
        parse_len(lv, fs, s.width, true);
    } else if (prop == "height" || prop == "block-size") {
        parse_len(lv, fs, s.height, true);
    } else if (prop == "min-width" || prop == "min-height") {
        Len l;
        if (parse_len(lv, fs, l, true)) (prop == "min-width" ? s.minWidth : s.minHeight) = l;
    } else if (prop == "max-width" || prop == "max-height") {
        Len l;
        if (parse_len(lv, fs, l, false)) (prop == "max-width" ? s.maxWidth : s.maxHeight) = l;
    } else if (prop == "position") {
        s.position = lv == "relative" ? 1 : lv == "absolute" ? 2 : lv == "fixed" ? 3 : (lv == "sticky" || lv == "-webkit-sticky") ? 4 : 0;
    } else if (prop == "top" || prop == "right" || prop == "bottom" || prop == "left") {
        Len l;
        if (parse_len(lv, fs, l, true)) s.inset[side_of(prop)] = l;
    } else if (prop == "inset") {
        set4(s.inset, T, fs, true);
    } else if (prop == "z-index") {
        if (lv == "auto") {
            s.zAuto = true;
            s.zIndex = 0;
        } else {
            s.zAuto = false;
            s.zIndex = std::clamp(atoi(lv.c_str()), -30000, 30000);
        }
    } else if (prop == "overflow" || prop == "overflow-x" || prop == "overflow-y") {
        const std::string& t = T.empty() ? lv : T[0];
        uint8_t o = (t == "hidden" || t == "clip") ? 1 : (t == "auto" || t == "scroll" || t == "overlay") ? 2 : 0;
        if (prop == "overflow" || o > s.overflow || o == 0) s.overflow = o;
    } else if (prop == "float") {
        s.floatSide = (lv == "left" || lv == "inline-start") ? 1 : (lv == "right" || lv == "inline-end") ? 2 : 0;
    } else if (prop == "clear") {
        s.clear = (lv == "left" || lv == "inline-start") ? 1 : (lv == "right" || lv == "inline-end") ? 2 : lv == "both" ? 3 : 0;
    } else if (prop == "fill" || prop == "stroke") {
        // <paint> (SVG 2, section 13.2). A paint server (url()) is not drawn yet: its fallback colour is used, or none.
        Paint& paint = prop == "fill" ? s.fill : s.stroke;
        std::string v = lv;
        if (istarts(v, "url(")) {
            size_t close = v.find(')');
            v = close == npos ? std::string() : std::string(trim(std::string_view(v).substr(close + 1)));
            if (v.empty()) v = "none";
        }
        Color c;
        if (v == "none") paint = {Paint::None, {}};
        else if (v == "currentcolor") paint = {Paint::Current, {}};
        else if (v == "context-fill" || v == "context-stroke") paint = {Paint::None, {}};
        else if (parse_color(v, c)) paint = {Paint::Rgb, c};
    } else if (prop == "stroke-width") {
        Len l;
        float v;
        std::string_view u;
        if (parse_num(lv, v, u) && u.empty() && v >= 0) s.strokeWidth = {Len::Px, v};
        else if (parse_len(lv, fs, l, false) && (l.u == Len::Px || l.u == Len::Pct) && l.v >= 0) s.strokeWidth = l;
    } else if (prop == "fill-rule") {
        if (lv == "evenodd" || lv == "nonzero") s.fillEvenOdd = lv == "evenodd";
    } else if (prop == "fill-opacity" || prop == "stroke-opacity") {
        float v;
        std::string_view u;
        if (parse_num(lv, v, u) && (u.empty() || u == "%")) (prop == "fill-opacity" ? s.fillOpacity : s.strokeOpacity) = std::clamp(u == "%" ? v / 100 : v, 0.f, 1.f);
    } else if (prop == "transform") {
        TransformList list;
        if (lv == "none") s.transform.reset();
        else if (parse_transform(lv, fs, list)) s.transform = std::make_shared<const TransformList>(std::move(list));
    } else if (prop == "transform-origin") {
        // <length-percentage> or keywords, x first unless the keywords say otherwise (CSS Transforms 1, section 6)
        Len x{Len::Pct, 50}, y{Len::Pct, 50};
        bool ok = !T.empty() && T.size() <= 3, xSet = false, ySet = false;
        for (size_t k = 0; ok && k < T.size() && k < 2; ++k) {
            const std::string& t = T[k];
            Len l;
            if (t == "left" || t == "right") (xSet ? ok = false : (x = {Len::Pct, t == "left" ? 0.f : 100.f}, xSet = true));
            else if (t == "top" || t == "bottom") (ySet ? ok = false : (y = {Len::Pct, t == "top" ? 0.f : 100.f}, ySet = true));
            else if (t == "center") (k == 0 && !xSet ? xSet = true : ySet = true);
            else if (parse_len(t, fs, l, false) && (l.u == Len::Px || l.u == Len::Pct)) (k == 0 ? (x = l, xSet = true) : (y = l, ySet = true));
            else ok = false;
        }
        if (ok) s.transformOrigin[0] = x, s.transformOrigin[1] = y;
    } else if (prop == "opacity") {
        float v;
        std::string_view u;
        if (parse_num(lv, v, u)) {
            s.opacity = std::clamp(u == "%" ? v / 100.f : v, 0.f, 1.f);
            s.opacity0 = s.opacity <= 0.01f;
        }
    } else if (istarts(prop, "animation")) {
        auto lists = s.animLists ? std::make_shared<AnimLists>(*s.animLists) : std::make_shared<AnimLists>();
        std::vector<std::string> items;
        for (auto part : split_top(val, ',')) items.emplace_back(trim(part));
        if (prop == "animation") {  // each item: a name, times (duration, then delay), a timing function and keywords
            *lists = AnimLists{};
            for (auto& item : items) {
                std::string name = "none", dur = "0s", delay = "0s", count = "1", dir = "normal", fill = "none", state = "running", timing = "ease";
                int times = 0;
                for (auto& t : tokens(item)) {
                    std::string l = lower(t);
                    float n;
                    std::string_view u;
                    Timing tf;
                    if (parse_num(l, n, u) && (u == "s" || u == "ms")) (times++ == 0 ? dur : delay) = l;
                    else if (parse_timing(l, tf)) timing = l;
                    else if (l == "infinite" || (parse_num(l, n, u) && u.empty())) count = l;
                    else if (in(l, {"normal", "reverse", "alternate", "alternate-reverse"})) dir = l;
                    else if (in(l, {"forwards", "backwards", "both"})) fill = l;
                    else if (l == "running" || l == "paused") state = l;
                    else name = t;
                }
                lists->names.push_back(name);
                lists->durations.push_back(dur);
                lists->delays.push_back(delay);
                lists->iterations.push_back(count);
                lists->directions.push_back(dir);
                lists->fills.push_back(fill);
                lists->states.push_back(state);
                lists->timings.push_back(timing);
            }
        } else if (prop == "animation-name") lists->names = items;
        else if (prop == "animation-duration") lists->durations = items;
        else if (prop == "animation-delay") lists->delays = items;
        else if (prop == "animation-iteration-count") lists->iterations = items;
        else if (prop == "animation-direction") lists->directions = items;
        else if (prop == "animation-fill-mode") lists->fills = items;
        else if (prop == "animation-play-state") lists->states = items;
        else if (prop == "animation-timing-function") lists->timings = items;
        s.animLists = std::move(lists);
        s.animated = false;
        for (auto& n : s.animLists->names) s.animated = s.animated || lower(n) != "none";
    } else if (istarts(prop, "transition")) {
        auto lists = s.transitionLists ? std::make_shared<TransitionLists>(*s.transitionLists) : std::make_shared<TransitionLists>();
        std::vector<std::string> items;
        for (auto part : split_top(lv, ',')) items.emplace_back(trim(part));
        if (prop == "transition") {
            *lists = TransitionLists{};
            for (auto& item : items) {
                std::string property = "all", dur = "0s", delay = "0s", timing = "ease";
                int times = 0;
                for (auto& t : tokens(item)) {
                    float n;
                    std::string_view u;
                    Timing tf;
                    if (parse_num(t, n, u) && (u == "s" || u == "ms")) (times++ == 0 ? dur : delay) = t;
                    else if (parse_timing(t, tf)) timing = t;
                    else if (t != "allow-discrete" && t != "normal") property = t;
                }
                lists->properties.push_back(property);
                lists->durations.push_back(dur);
                lists->delays.push_back(delay);
                lists->timings.push_back(timing);
            }
        } else if (prop == "transition-property") lists->properties = items;
        else if (prop == "transition-duration") lists->durations = items;
        else if (prop == "transition-delay") lists->delays = items;
        else if (prop == "transition-timing-function") lists->timings = items;
        s.transitionLists = std::move(lists);
    } else if (prop == "mask-image" || prop == "-webkit-mask-image" || prop == "mask" || prop == "-webkit-mask") {
        s.masked = lv != "none";
    } else if (prop == "clip" || prop == "clip-path") {
        // The "visually hidden" idiom: clip: rect(0 0 0 0) / clip-path: inset(50%).
        std::string z;
        for (char ch : lv)
            if (!isws(ch) && ch != ',') z += ch;
        if (z == "rect(0000)" || z == "rect(0px0px0px0px)" || z == "rect(1px1px1px1px)" || z == "rect(0,0,0,0)" || z == "inset(50%)" ||
            z == "inset(100%)")
            s.hidden = true;
    } else if (prop == "border-radius" || prop == "border-top-left-radius") {
        if (!T.empty()) {
            Len l;
            if (parse_len(T[0], fs, l, false)) s.radius = l.u == Len::Px ? std::max(0.f, l.v) : l.u == Len::Pct ? (l.v >= 50 ? 9999.f : l.v) : 0;
        }
    } else if (prop == "flex-direction") {
        s.flexDir = lv == "row-reverse" ? 1 : lv == "column" ? 2 : lv == "column-reverse" ? 3 : 0;
    } else if (prop == "flex-wrap") {
        s.flexWrap = istarts(lv, "wrap");
    } else if (prop == "flex-flow") {
        for (auto& t : T) {
            if (t == "row") s.flexDir = 0;
            else if (t == "row-reverse") s.flexDir = 1;
            else if (t == "column") s.flexDir = 2;
            else if (t == "column-reverse") s.flexDir = 3;
            else if (t == "wrap" || t == "wrap-reverse") s.flexWrap = true;
            else if (t == "nowrap") s.flexWrap = false;
        }
    } else if (prop == "justify-content" || prop == "align-content" || prop == "place-content") {
        // place-content is <align-content> <justify-content>?, the second the same as the first when left out
        std::string align = lv, justify = lv;
        if (prop == "place-content") {
            auto halves = tokens(lv);
            if (halves.empty()) return;
            align = halves[0];
            justify = halves.size() > 1 ? halves[1] : halves[0];
        }
        if (prop != "justify-content") {
            s.alignContent = content_distribution(align);
            s.alignContentNormal = align == "normal" || align == "stretch";
        }
        if (prop != "align-content") {
            s.justify = content_distribution(justify);
            s.justifyNormal = justify == "normal" || justify == "stretch";
        }
    } else if (prop == "justify-items" || prop == "justify-self") {
        uint8_t a = self_alignment(lv);
        if (prop == "justify-items") s.justifyItems = a;
        else s.justifySelf = lv == "auto" ? 255 : a;
    } else if (prop == "place-items" || prop == "place-self") {
        auto halves = tokens(lv);
        if (halves.empty()) return;
        std::string j = halves.size() > 1 ? halves[1] : halves[0];
        if (prop == "place-items") {
            s.alignItems = self_alignment(halves[0]);
            s.justifyItems = self_alignment(j);
        } else {
            s.alignSelf = halves[0] == "auto" ? 255 : self_alignment(halves[0]);
            s.justifySelf = j == "auto" ? 255 : self_alignment(j);
        }
    } else if (istarts(prop, "grid-template") || prop == "grid" || istarts(prop, "grid-auto")) {
        auto g = s.grid ? std::make_shared<GridTemplate>(*s.grid) : std::make_shared<GridTemplate>();
        bool ok = true;
        if (prop == "grid-template-columns") ok = parse_track_list(val, fs, g->cols);
        else if (prop == "grid-template-rows") ok = parse_track_list(val, fs, g->rows);
        else if (prop == "grid-template-areas") ok = parse_areas(val, *g);
        else if (prop == "grid-template") ok = parse_grid_template(val, fs, *g);
        else if (prop == "grid") ok = parse_grid(val, fs, *g);
        else if (prop == "grid-auto-columns") ok = parse_auto_tracks(val, fs, g->autoCols);
        else if (prop == "grid-auto-rows") ok = parse_auto_tracks(val, fs, g->autoRows);
        else if (prop == "grid-auto-flow") {
            g->flowColumn = contains(lv, "column");
            g->dense = contains(lv, "dense");
        }
        if (ok) s.grid = std::move(g);
    } else if (prop == "grid-row-start" || prop == "grid-row-end" || prop == "grid-column-start" || prop == "grid-column-end") {
        GridLine gl;
        if (parse_grid_line(val, gl)) (contains(prop, "row") ? s.gridRow : s.gridCol)[contains(prop, "end") ? 1 : 0] = gl;
    } else if (prop == "grid-row" || prop == "grid-column" || prop == "grid-area") {
        auto parts = split_top(val, '/');
        GridLine L[4];
        size_t n = std::min<size_t>(parts.size(), prop == "grid-area" ? 4 : 2);
        for (size_t i = 0; i < n; ++i)
            if (!parse_grid_line(parts[i], L[i])) return;
        if (prop == "grid-area") {
            if (n < 2) L[1] = grid_line_default(L[0]);
            if (n < 3) L[2] = grid_line_default(L[0]);
            if (n < 4) L[3] = grid_line_default(L[1]);
            s.gridRow[0] = L[0];
            s.gridCol[0] = L[1];
            s.gridRow[1] = L[2];
            s.gridCol[1] = L[3];
        } else {
            if (n < 2) L[1] = grid_line_default(L[0]);
            GridLine* to = prop == "grid-row" ? s.gridRow : s.gridCol;
            to[0] = L[0];
            to[1] = L[1];
        }
    } else if (prop == "align-items" || prop == "align-self") {
        std::string t = lv;
        if (istarts(t, "safe ")) t = t.substr(5);
        uint8_t a = (t == "flex-start" || t == "start" || t == "baseline" || t == "first baseline" || t == "self-start") ? 1
                    : t == "center" ? 2 : (t == "flex-end" || t == "end" || t == "self-end") ? 3 : 0;
        if (prop == "align-items") s.alignItems = a;
        else s.alignSelf = t == "auto" ? 255 : a;
    } else if (prop == "gap" || prop == "grid-gap") {
        Len a, b;
        if (!T.empty() && parse_len(T[0], fs, a, false)) {
            s.gapRow = s.gapCol = a.u == Len::Px ? a.v : 0;
            if (T.size() > 1 && parse_len(T[1], fs, b, false)) s.gapCol = b.u == Len::Px ? b.v : 0;
        }
    } else if (prop == "row-gap" || prop == "column-gap" || prop == "grid-row-gap" || prop == "grid-column-gap") {
        Len a;
        if (parse_len(lv, fs, a, false)) (contains(prop, "row") ? s.gapRow : s.gapCol) = a.u == Len::Px ? a.v : 0;
    } else if (prop == "flex-grow" || prop == "flex-shrink" || prop == "-webkit-box-flex") {
        float v;
        std::string_view u;
        if (parse_num(lv, v, u) && u.empty() && v >= 0) (prop == "flex-shrink" ? s.flexShrink : s.flexGrow) = v;
    } else if (prop == "flex-basis") {
        Len l;
        if (lv == "content") s.flexBasis = Len{};
        else if (parse_len(lv, fs, l, true)) s.flexBasis = l;
    } else if (prop == "flex") {
        if (lv == "none") {
            s.flexGrow = 0;
            s.flexShrink = 0;
            s.flexBasis = Len{};
        } else if (lv == "auto") {
            s.flexGrow = 1;
            s.flexShrink = 1;
            s.flexBasis = Len{};
        } else {
            float g = 0, sh = 1;
            Len basis{Len::Px, 0};
            int nums = 0;
            bool ok = !T.empty();
            for (auto& t : T) {
                float v;
                std::string_view u;
                Len l;
                if (parse_num(t, v, u) && u.empty() && nums < 2) {
                    (nums == 0 ? g : sh) = v;
                    ++nums;
                } else if (t == "auto" || t == "content") {
                    basis = Len{};
                } else if (parse_len(t, fs, l, false)) {
                    basis = l;
                } else {
                    ok = false;
                }
            }
            if (ok) {
                s.flexGrow = std::max(0.f, g);
                s.flexShrink = std::max(0.f, sh);
                s.flexBasis = basis;
            }
        }
    } else if (prop == "border") {
        for (int i = 0; i < 4; ++i) border_side(s, i, T);
    } else if (istarts(prop, "border-")) {
        std::string_view rest = prop.substr(7);
        int side = side_of(rest);
        if (side >= 0) {
            border_side(s, side, T);
            return;
        }
        if (rest == "width" || rest == "style" || rest == "color") {
            size_t n = T.size();
            if (n < 1 || n > 4) return;
            int idx[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
            for (int i = 0; i < 4; ++i) {
                const std::string& t = T[idx[n - 1][i]];
                if (rest == "width") {
                    float w;
                    if (parse_bw(t, fs, w)) s.border[i] = w;
                } else if (rest == "style") {
                    int b = parse_bs(t);
                    if (b >= 0) s.borderStyle[i] = (uint8_t)b;
                } else {
                    Color c;
                    if (parse_color_cc(t, c)) s.borderColor[i] = c;
                }
            }
            return;
        }
        size_t dash = rest.rfind('-');
        if (dash == npos) return;
        side = side_of(rest.substr(0, dash));
        std::string_view what = rest.substr(dash + 1);
        if (side < 0) return;
        if (what == "width") {
            float w;
            if (parse_bw(lv, fs, w)) s.border[side] = w;
        } else if (what == "style") {
            int b = parse_bs(lv);
            if (b >= 0) s.borderStyle[side] = (uint8_t)b;
        } else if (what == "color") {
            Color c;
            if (parse_color_cc(lv, c)) s.borderColor[side] = c;
        }
    }
}

std::string dim_attr(const std::string& v) {
    std::string t(trim(v));
    if (t.empty()) return t;
    if (t.back() == '%') return t;
    bool num = true;
    for (char c : t)
        if (!isdigit_(c) && c != '.') num = false;
    return num ? t + "px" : t;
}

// Presentational attributes (bgcolor, width, align, ...) mapped to CSS declarations.
void hints(const Node* e, std::vector<Decl>& out) {
    auto add = [&](const char* p, std::string v) { out.push_back({p, std::move(v), false}); };
    const std::string& n = e->name;
    // SVG presentation attributes (SVG 2, section 6.6) are author rules that come before all others. Only elements
    // inside an svg element have them; width and height are ones on the svg element itself.
    static const char* presentation[] = {"fill", "stroke", "stroke-width", "fill-rule", "fill-opacity", "stroke-opacity", "opacity", "color", "display", "visibility"};
    bool anyPresentation = n == "svg";
    for (const char* a : presentation) anyPresentation = anyPresentation || e->attr(a);
    if (anyPresentation) {
        bool inSvg = false;
        for (const Node* p = e; p && !inSvg; p = p->parent) inSvg = p->name == "svg";
        if (inSvg) {
            for (const char* a : presentation)
                if (const std::string* v = e->attr(a)) add(a, *v);
            if (n == "svg") {
                for (const char* a : {"width", "height"})
                    if (const std::string* v = e->attr(a)) {
                        std::string t(trim(*v));
                        bool plain = !t.empty() && t.find_first_not_of("0123456789.+-eE") == std::string::npos;
                        add(a, plain ? t + "px" : t);  // a unitless length is in px
                    }
            }
        }
    }
    if (auto a = e->attr("bgcolor")) add("background-color", *a);
    if (auto a = e->attr("hidden"); a) add("display", "none");
    if (n == "font") {
        if (auto a = e->attr("color")) add("color", *a);
        if (auto a = e->attr("face")) add("font-family", *a);
        if (auto a = e->attr("size")) {
            static const char* sz[] = {"x-small", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large"};
            int v = atoi(a->c_str());
            if (!a->empty() && ((*a)[0] == '+' || (*a)[0] == '-')) v += 3;
            add("font-size", sz[std::clamp(v, 1, 7)]);
        }
    }
    if (n == "body") {
        if (auto a = e->attr("text")) add("color", *a);
        if (auto a = e->attr("background"); a && !e->attr("bgcolor")) {
        }
    }
    if (in(n, {"img", "table", "td", "th", "hr", "col", "iframe", "canvas", "video", "embed", "object", "input"})) {
        if (auto a = e->attr("width")) add("width", dim_attr(*a));
    }
    if (in(n, {"img", "table", "td", "th", "tr", "iframe", "canvas", "video", "embed", "object"})) {
        if (auto a = e->attr("height")) add("height", dim_attr(*a));
    }
    if (auto a = e->attr("align")) {
        std::string v = lower(*a);
        if (n == "table" && v == "center") {
            add("margin-left", "auto");
            add("margin-right", "auto");
        } else if (in(n, {"td", "th", "tr", "div", "p", "h1", "h2", "h3", "h4", "h5", "h6", "caption", "thead", "tbody"})) {
            add("text-align", (v == "center" || v == "middle") ? "-webkit-center" : v);
        }
    }
    if (auto a = e->attr("valign")) add("vertical-align", lower(*a));
    if (n == "td" || n == "th") {
        if (e->attr("nowrap")) add("white-space", "nowrap");
        const Node* p = e->parent;
        while (p && p->name != "table") p = p->parent;
        if (p) {
            if (auto cp = p->attr("cellpadding")) add("padding", dim_attr(*cp));
            if (auto b = p->attr("border"); b && atoi(b->c_str()) > 0) add("border", "1px inset gray");
        }
        if (e->parent && e->parent->name == "tr")
            if (auto va = e->parent->attr("valign"); va && !e->attr("valign")) add("vertical-align", lower(*va));
    }
    if (n == "table")
        if (auto b = e->attr("border"); b && !b->empty()) {
            int w = atoi(b->c_str());
            if (w > 0) add("border", std::to_string(w) + "px outset gray");
        }
    if (n == "img")
        if (auto b = e->attr("border")) add("border", dim_attr(*b) + " solid");
    if (n == "hr") {
        if (e->attr("noshade")) add("border-style", "solid");
        if (auto c = e->attr("color")) {
            add("border-color", *c);
            add("background-color", *c);
        }
    }
    if (n == "ol")
        if (auto st = e->attr("start")) add("counter-reset", "list-item " + std::to_string(atoi(st->c_str()) - 1));
    if (n == "li")
        if (auto v = e->attr("value")) add("counter-set", "list-item " + std::to_string(atoi(v->c_str())));
    if (n == "ul" || n == "ol" || n == "li")
        if (auto t = e->attr("type")) {
            std::string v = *t;
            add("list-style-type", v == "1" ? "decimal" : v == "a" ? "lower-alpha" : v == "A" ? "upper-alpha" : v == "i" ? "lower-roman" : v == "I" ? "upper-roman" : lower(v));
        }
}

}  // namespace
const Node* g_selectorScope = nullptr;
UserState g_userState;
namespace {
// The cascade's order of precedence (CSS Cascade 5, 6): origin and importance, then the layer, then specificity,
// then order of appearance. Important declarations of a layer beat those of later layers and unlayered ones.
uint64_t key(int lvl, uint32_t spec, uint32_t order, uint8_t layerRank = 0) {
    if (lvl == 3) layerRank = (uint8_t)(255 - layerRank);
    return (uint64_t(lvl) << 60) | (uint64_t(layerRank) << 52) | (uint64_t(spec & 0x3FFFFFFF) << 22) | uint64_t(order & 0x3FFFFF);
}

// The buckets a compound can be found by (0 id, 1 class, 2 tag, 3 attribute name, 4 the root): an element that the
// compound matches is in at least one of them. False when there is none and the rule has to be tried everywhere.
// :is() and :where() are found by the buckets of all their arguments.
using RuleKey = std::pair<int, std::string>;
bool rule_keys(const Compound& c, std::vector<RuleKey>& out) {
    if (!c.id.empty()) out.push_back({0, c.id});
    else if (!c.classes.empty()) out.push_back({1, c.classes[0]});
    else if (!c.tag.empty()) out.push_back({2, c.tag});
    else if (!c.attrs.empty()) out.push_back({3, c.attrs[0].name});
    else {
        for (auto& p : c.pseudos) {
            if (p.k == Pseudo::Root) {
                out.push_back({4, {}});
                return true;
            }
            if (p.k == Pseudo::Is && p.list) {
                std::vector<RuleKey> all;
                bool keyed = true;
                for (auto& one : *p.list) keyed = keyed && rule_keys(one.parts.back(), all);
                if (keyed) {
                    out.insert(out.end(), all.begin(), all.end());
                    return true;
                }
            }
        }
        return false;
    }
    return true;
}

uint8_t user_actions(const Selector& s) {
    uint8_t r = 0;
    for (auto& c : s.parts)
        for (auto& p : c.pseudos) {
            switch (p.k) {
                case Pseudo::Hover: r |= StyleEngine::UsesHover; break;
                case Pseudo::Active: r |= StyleEngine::UsesActive; break;
                case Pseudo::Focus:
                case Pseudo::FocusWithin:
                case Pseudo::FocusVisible: r |= StyleEngine::UsesFocus; break;
                case Pseudo::Checked:
                case Pseudo::PlaceholderShown: r |= StyleEngine::UsesFormState; break;
                case Pseudo::Target: r |= StyleEngine::UsesTarget; break;
                default: break;
            }
            if (p.list)
                for (auto& one : *p.list) r |= user_actions(one);
        }
    return r;
}

}  // namespace

// ---- Media Queries 4
namespace {
// A value in a media feature: a length in px, a number, a ratio or a resolution in dppx. False when it is none of
// these (an identifier is matched by the caller).
bool media_value(std::string_view v, const MediaEnv& env, double& out) {
    std::string s = lower(trim(v));
    size_t slash = s.find('/');
    if (slash != std::string::npos) {  // <ratio>: a / b
        double a = atof(s.substr(0, slash).c_str()), b = atof(s.substr(slash + 1).c_str());
        if (b <= 0) return false;
        out = a / b;
        return true;
    }
    float n;
    std::string_view u;
    if (!parse_num(s, n, u)) return false;
    static const std::pair<const char*, double> units[] = {
        {"", 1}, {"px", 1}, {"em", 16}, {"rem", 16}, {"ex", 8}, {"ch", 8}, {"cm", 96 / 2.54}, {"mm", 96 / 25.4}, {"q", 96 / 101.6},
        {"in", 96}, {"pt", 96.0 / 72}, {"pc", 16}, {"dppx", 1}, {"x", 1}, {"dpi", 1 / 96.0}, {"dpcm", 2.54 / 96}};
    for (auto& [name, k] : units)
        if (u == name) {
            out = n * k;
            return true;
        }
    if (u == "vw" || u == "svw" || u == "lvw" || u == "dvw") out = n * env.w / 100;
    else if (u == "vh" || u == "svh" || u == "lvh" || u == "dvh") out = n * env.h / 100;
    else if (u == "vmin") out = n * std::min(env.w, env.h) / 100;
    else if (u == "vmax") out = n * std::max(env.w, env.h) / 100;
    else return false;
    return true;
}
// The value of a feature this engine knows, as a number (range features) or a word (discrete ones); -1 when the
// feature is unknown
int media_feature(const std::string& name, const MediaEnv& env, double& num, std::string& word) {
    if (name == "width" || name == "device-width") num = env.w;
    else if (name == "height" || name == "device-height") num = env.h;
    else if (name == "aspect-ratio" || name == "device-aspect-ratio") num = env.h > 0 ? env.w / env.h : 0;
    else if (name == "resolution" || name == "-webkit-device-pixel-ratio") num = env.dpr;
    else if (name == "color") num = 8;
    else if (name == "color-index" || name == "monochrome" || name == "grid") num = 0;
    else {
        static const std::pair<const char*, const char*> discrete[] = {
            {"hover", "hover"}, {"any-hover", "hover"}, {"pointer", "fine"}, {"any-pointer", "fine"},
            {"prefers-color-scheme", "light"}, {"prefers-reduced-motion", "no-preference"},
            {"prefers-contrast", "no-preference"}, {"prefers-reduced-transparency", "no-preference"},
            {"prefers-reduced-data", "no-preference"}, {"forced-colors", "none"}, {"inverted-colors", "none"},
            {"color-gamut", "srgb"}, {"dynamic-range", "standard"}, {"video-dynamic-range", "standard"},
            {"display-mode", "browser"}, {"scripting", "enabled"}, {"update", "fast"}, {"overflow-block", "scroll"},
            {"overflow-inline", "scroll"}, {"orientation", ""}};
        for (auto& [n, v] : discrete)
            if (name == n) {
                word = name == "orientation" ? (env.h >= env.w ? "portrait" : "landscape") : v;
                return 1;
            }
        return -1;
    }
    return 0;
}
// 1 true, 0 false, -1 unknown (Media Queries 4, 3.2: what this engine cannot read is unknown, and false at the end)
int media_condition(std::string_view v, const MediaEnv& env);
int media_in_parens(std::string_view v, const MediaEnv& env) {
    v = trim(v);
    if (v.size() < 2 || v[0] != '(' || v.back() != ')') return -1;  // <general-enclosed>
    std::string_view inner = trim(v.substr(1, v.size() - 2));
    std::string li = lower(inner);
    if (!inner.empty() && (inner[0] == '(' || istarts(li, "not ") || istarts(li, "not("))) return media_condition(inner, env);
    // a feature: (name), (name: value), or a range with <, <=, >, >= or =
    size_t op = inner.find_first_of("<>=");
    size_t colon = inner.find(':');
    auto compare = [](double a, const std::string& o, double b) {
        const double eps = 1e-6;
        if (o == "<") return a < b - eps;
        if (o == "<=") return a <= b + eps;
        if (o == ">") return a > b + eps;
        if (o == ">=") return a >= b - eps;
        return std::fabs(a - b) <= eps;
    };
    if (op == std::string::npos) {
        std::string name = lower(trim(colon == std::string::npos ? inner : inner.substr(0, colon)));
        std::string prefix;
        if (istarts(name, "min-") || istarts(name, "max-")) {
            prefix = name.substr(0, 3);
            name = name.substr(4);
        } else if (istarts(name, "-webkit-min-") || istarts(name, "-webkit-max-")) {
            prefix = name.substr(8, 3);
            name = "-webkit-" + name.substr(12);
        }
        double num = 0;
        std::string word;
        int kind = media_feature(name, env, num, word);
        if (kind < 0) return -1;
        if (colon == std::string::npos) {  // boolean context: anything but 0 and none
            if (!prefix.empty()) return -1;
            return kind == 0 ? num != 0 : (word != "none" && word != "no-preference");
        }
        std::string value = lower(trim(inner.substr(colon + 1)));
        if (kind == 1) return prefix.empty() ? value == word : -1;
        double want;
        if (!media_value(value, env, want)) return -1;
        if (prefix == "min") return compare(num, ">=", want);
        if (prefix == "max") return compare(num, "<=", want);
        return compare(num, "=", want);
    }
    // range form: value op name, name op value, or value op name op value
    std::vector<std::string> parts, ops;
    size_t i = 0, start = 0;
    while (i < inner.size()) {
        if (inner[i] == '<' || inner[i] == '>' || inner[i] == '=') {
            parts.push_back(std::string(trim(inner.substr(start, i - start))));
            std::string o(1, inner[i]);
            if (i + 1 < inner.size() && inner[i + 1] == '=' && inner[i] != '=') o += '=', ++i;
            ops.push_back(o);
            start = ++i;
            continue;
        }
        ++i;
    }
    parts.push_back(std::string(trim(inner.substr(start))));
    if (parts.size() < 2 || parts.size() > 3) return -1;
    size_t at = parts.size();  // which part is the feature's name
    double num = 0;
    std::string word;
    for (size_t k = 0; k < parts.size(); ++k) {
        double probe;
        std::string name = lower(parts[k]);
        if (!media_value(name, env, probe) && media_feature(name, env, num, word) == 0) at = k;
    }
    if (at == parts.size()) return -1;
    bool ok = true;
    for (size_t k = 0; k + 1 < parts.size(); ++k) {
        double a = num, b = num;
        if (k != at && !media_value(parts[k], env, a)) return -1;
        if (k + 1 != at && !media_value(parts[k + 1], env, b)) return -1;
        ok = ok && compare(a, ops[k], b);
    }
    return ok;
}
// <media-condition>: not <in-parens>, or <in-parens> joined by and, or by or
int media_condition(std::string_view v, const MediaEnv& env) {
    v = trim(v);
    std::string l = lower(v);
    if (istarts(l, "not ") || istarts(l, "not(")) {
        int r = media_in_parens(v.substr(3), env);
        return r < 0 ? -1 : !r;
    }
    std::vector<std::string_view> terms;
    std::string op;
    size_t i = 0, start = 0;
    int depth = 0;
    while (i < v.size()) {
        char c = v[i];
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (depth == 0 && isws(c)) {
            size_t j = i;
            while (j < v.size() && isws(v[j])) ++j;
            size_t k = j;
            while (k < v.size() && isalpha((unsigned char)v[k])) ++k;
            std::string w = lower(v.substr(j, k - j));
            if ((w == "and" || w == "or") && k < v.size() && (isws(v[k]) || v[k] == '(')) {
                if (!op.empty() && op != w) return -1;  // and and or are not mixed without parentheses
                op = w;
                terms.push_back(v.substr(start, i - start));
                start = i = k;
                continue;
            }
        }
        ++i;
    }
    terms.push_back(v.substr(start));
    int result = op == "or" ? 0 : 1;
    for (auto t : terms) {
        int r = media_in_parens(t, env);
        if (op == "or") result = (r == 1 || result == 1) ? 1 : (r < 0 || result < 0) ? -1 : 0;
        else result = (r == 0 || result == 0) ? 0 : (r < 0 || result < 0) ? -1 : 1;
    }
    return result;
}
// <media-query>: a condition, or [not | only]? <media-type> [and <condition-without-or>]?
bool media_query(std::string_view q, const MediaEnv& env) {
    q = trim(q);
    if (q.empty()) return false;
    if (q[0] == '(' || istarts(lower(q), "not (") || istarts(lower(q), "not(")) return media_condition(q, env) == 1;
    auto T = tokens(lower(q));
    size_t i = 0;
    bool negate = false;
    if (T[i] == "not") negate = true, ++i;
    else if (T[i] == "only") ++i;
    if (i >= T.size()) return false;
    std::string type = T[i++];
    if (in(type, {"and", "or", "not", "only", "layer"})) return false;
    bool typeOk = type == "all" || type == "screen";
    int cond = 1;
    if (i < T.size()) {
        if (T[i] != "and") return false;
        size_t at = lower(q).find(" and ");
        cond = media_condition(q.substr(at + 5), env);
        if (cond < 0) return false;  // an unknown condition makes the whole query false, not its negation true
    }
    bool r = typeOk && cond == 1;
    return negate ? !r : r;
}
}  // namespace

bool media_matches(std::string_view list, const MediaEnv& env) {
    if (trim(list).empty()) return true;
    for (auto q : split_top(list, ','))
        if (media_query(q, env)) return true;
    return false;
}

void StyleEngine::build(float vw, float vh) {
    MediaEnv env{vw, vh, 1};
    keyframes_.clear();
    for (auto& a : sheets_)
        for (auto& [name, kf] : a.sheet->keyframes) keyframes_[name] = kf;
    all_.clear();
    byAttr_.clear();
    root_.clear();
    userActions_ = 0;
    byId_.clear();
    byClass_.clear();
    byTag_.clear();
    uni_.clear();
    pseudoById_.clear();
    pseudoByClass_.clear();
    pseudoByTag_.clear();
    pseudoUni_.clear();
    hasMedia_ = false;
    // The layer order (CSS Cascade 5, 6.4.3): layers in the order their names first appear, each after the layers
    // nested in it; unlayered rules after every layer. Rank 0 is left for presentational hints.
    struct LayerNode {
        std::vector<int> kids;
        uint8_t rank = 0;
    };
    std::vector<LayerNode> tree(1);
    std::unordered_map<std::string, int> layerId{{"", 0}};
    std::function<int(const std::string&)> layerOf = [&](const std::string& full) -> int {
        auto found = layerId.find(full);
        if (found != layerId.end()) return found->second;
        size_t dot = full.rfind('.');
        int parent = dot == std::string::npos ? 0 : layerOf(full.substr(0, dot));  // a.b comes into being inside a
        tree.push_back({});
        int id = (int)tree.size() - 1;
        layerId[full] = id;
        tree[parent].kids.push_back(id);
        return id;
    };
    auto fullName = [](const std::string& prefix, const std::string& name) {
        return prefix.empty() ? name : name.empty() ? prefix : prefix + "." + name;
    };
    for (auto& a : sheets_) {
        if (a.ua) continue;
        if (!a.layer.empty()) layerOf(a.layer);
        for (auto& name : a.sheet->layers) layerOf(fullName(a.layer, name));
    }
    int next = 1;
    std::function<void(int)> rankFrom = [&](int id) {
        for (int k : tree[id].kids) rankFrom(k);
        tree[id].rank = (uint8_t)std::min(next++, 255);
    };
    rankFrom(0);
    uint32_t order = 1;
    for (auto& [sh, ua, scope, layer, gate] : sheets_) {
        bool gated = gate != nullptr;  // an import for media that never match (print) has no alternatives
        if (gated) hasMedia_ = true;
        if (gated && !media_matches(gate->text, env)) continue;
        for (auto& r : sh->rules) {
            ++order;
            if (r.media >= 0) {
                hasMedia_ = true;
                if (!media_matches(sh->media[r.media].text, env)) continue;
            }
            uint8_t rank = 0;
            if (!ua) rank = tree[layerOf(fullName(layer, r.layer >= 0 ? sh->layers[r.layer] : std::string()))].rank;
            all_.push_back({&r, order, ua, scope, rank});
            uint32_t idx = (uint32_t)all_.size() - 1;
            const Compound& last = r.sel.parts.back();
            if (last.never) continue;
            userActions_ |= user_actions(r.sel);
            bool gen = last.pseudoElement != 0;
            std::vector<RuleKey> keys;
            if (gen || !rule_keys(last, keys)) {
                if (!last.id.empty()) (gen ? pseudoById_ : byId_)[last.id].push_back(idx);
                else if (!last.classes.empty()) (gen ? pseudoByClass_ : byClass_)[last.classes[0]].push_back(idx);
                else if (!last.tag.empty()) (gen ? pseudoByTag_ : byTag_)[last.tag].push_back(idx);
                else (gen ? pseudoUni_ : uni_).push_back(idx);
                continue;
            }
            for (auto& [kind, k] : keys) {
                switch (kind) {
                    case 0: byId_[k].push_back(idx); break;
                    case 1: byClass_[k].push_back(idx); break;
                    case 2: byTag_[k].push_back(idx); break;
                    case 3: byAttr_[k].push_back(idx); break;
                    default: root_.push_back(idx); break;
                }
            }
        }
    }
}

void StyleEngine::compute(Document& d, float vw, float vh) {
    g_vw = vw;
    g_vh = vh;
    g_rootFont = 16;
    build(vw, vh);
    Style root;
    root.family = resolve_family("serif");
    counters_.clear();
    depth_ = 0;
    if (d.html) computeTree(d.html, root);
}

void StyleEngine::computeTree(Node* e, const Style& parent) {
    computeElem(e, parent);
    if (e->name == "html") g_rootFont = e->style->fontSize;
    if (e->style->display == Display::None) {
        for (auto& k : e->kids) k->style.reset();
        return;
    }
    if (e->shadow)  // children no slot takes are not rendered: FlatKids leaves them out, and they keep no style
        for (auto& k : e->kids) k->style.reset();
    ++depth_;
    for (Node* k : FlatKids(e))
        if (k->type == Node::T::Element && k != e->before.get() && k != e->after.get()) computeTree(k, *e->style);
    if (e->after) {  // ::after comes after the children in tree order, so its counters are known only now
        applyCounters(*e->after->style, false, depth_);
        std::string text;
        if (!generatedText(e, *e->after->style, text)) e->after.reset();
        else if (!text.empty()) {
            auto t = std::make_unique<Node>();
            t->type = Node::T::Text;
            t->text = std::move(text);
            e->after->append(std::move(t));
        }
    }
    --depth_;
    while (!counters_.empty() && counters_.back().depth > depth_) counters_.pop_back();
}

namespace {
// The counters a counter-reset, counter-increment or counter-set value names, each with its number (CSS Lists 3, 4)
std::vector<std::pair<std::string, int>> counter_list(std::string_view v, int dflt) {
    std::vector<std::pair<std::string, int>> out;
    size_t i = 0;
    while (i < v.size()) {
        while (i < v.size() && isws(v[i])) ++i;
        size_t st = i;
        while (i < v.size() && !isws(v[i])) ++i;
        std::string_view w = v.substr(st, i - st);
        if (w.empty()) break;
        if (w.size() > 9 && w.substr(0, 9) == "reversed(" && w.back() == ')') w = w.substr(9, w.size() - 10);
        bool number = isdigit_(w[0]) || ((w[0] == '-' || w[0] == '+') && w.size() > 1 && isdigit_(w[1]));
        if (number) {
            if (!out.empty()) out.back().second = atoi(std::string(w).c_str());
        } else if (w != "none")
            out.push_back({std::string(w), dflt});
    }
    return out;
}
}  // namespace

// Resets, then increments, then sets (CSS Lists 3, 4.5). A list item increments list-item by one unless its
// counter-increment names that counter.
void StyleEngine::applyCounters(Style& s, bool listItem, int depth) {
    // An element takes over its previous sibling's counters only where its parent has none of that name (4.5.1):
    // a nested reset ends with the element that made it
    for (size_t i = counters_.size(); i-- > 0 && counters_[i].depth == depth;) {
        bool outer = false;
        for (size_t j = 0; j < i && !outer; ++j) outer = counters_[j].depth < depth && counters_[j].name == counters_[i].name;
        if (outer) counters_.erase(counters_.begin() + (std::ptrdiff_t)i);
    }
    for (auto& [name, v] : counter_list(s.counterReset, 0)) {
        for (size_t i = counters_.size(); i-- > 0;)
            if (counters_[i].name == name) {
                if (counters_[i].depth == depth) counters_.erase(counters_.begin() + (std::ptrdiff_t)i);  // a sibling's
                break;
            }
        counters_.push_back({name, v, depth});
    }
    auto innermost = [&](const std::string& name) -> Counter& {
        for (size_t i = counters_.size(); i-- > 0;)
            if (counters_[i].name == name) return counters_[i];
        counters_.push_back({name, 0, depth});  // none in scope: the element makes one
        return counters_.back();
    };
    bool namesListItem = false;
    for (auto& [name, v] : counter_list(s.counterIncrement, 1)) {
        namesListItem = namesListItem || name == "list-item";
        innermost(name).value += v;
    }
    if (listItem && !namesListItem) innermost("list-item").value += 1;
    for (auto& [name, v] : counter_list(s.counterSet, 0)) innermost(name).value = v;
    if (listItem) s.listItem = innermost("list-item").value;
}

std::string counter_text(int value, uint8_t style) {
    auto alphabetic = [](int v, const char* const* digits, int n) {  // bijective base n: a, b, ... z, aa, ab ...
        std::vector<const char*> r;
        for (; v > 0; v = (v - 1) / n) r.push_back(digits[(v - 1) % n]);
        std::string s;
        for (auto it = r.rbegin(); it != r.rend(); ++it) s += *it;
        return s;
    };
    static const char* const lower[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                                        "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
    static const char* const upper[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static const char* const greek[] = {"\xCE\xB1", "\xCE\xB2", "\xCE\xB3", "\xCE\xB4", "\xCE\xB5", "\xCE\xB6", "\xCE\xB7", "\xCE\xB8",
                                        "\xCE\xB9", "\xCE\xBA", "\xCE\xBB", "\xCE\xBC", "\xCE\xBD", "\xCE\xBE", "\xCE\xBF", "\xCF\x80",
                                        "\xCF\x81", "\xCF\x83", "\xCF\x84", "\xCF\x85", "\xCF\x86", "\xCF\x87", "\xCF\x88", "\xCF\x89"};
    switch (style) {
        case 0: return "\xE2\x80\xA2";
        case 1: return "\xE2\x97\xA6";
        case 2: return "\xE2\x96\xAA";
        case 4: return "";
        case 5:
        case 6:
        case 10:
            if (value >= 1) return alphabetic(value, style == 5 ? lower : style == 6 ? upper : greek, style == 10 ? 24 : 26);
            break;
        case 7:
        case 8:
            if (value >= 1 && value <= 3999) {
                static const std::pair<int, const char*> t[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
                                                                {50, "l"},   {40, "xl"},  {10, "x"},  {9, "ix"},   {5, "v"},   {4, "iv"}, {1, "i"}};
                std::string r;
                for (auto& [v, s] : t)
                    for (; value >= v; value -= v) r += s;
                if (style == 8)
                    for (auto& ch : r) ch = (char)toupper((unsigned char)ch);
                return r;
            }
            break;
        case 9:
            if (value >= 0 && value < 10) return "0" + std::to_string(value);  // the minus sign counts towards the padding

            break;
    }
    return std::to_string(value);
}

void StyleEngine::computeElem(Node* e, const Style& parent) {
    auto s = std::make_unique<Style>();
    inherit(*s, parent);

    cand_.clear();
    auto addAll = [&](const std::unordered_map<std::string, std::vector<uint32_t>>& m, const std::string& k) {
        auto it = m.find(k);
        if (it != m.end()) cand_.insert(cand_.end(), it->second.begin(), it->second.end());
    };
    if (!e->id.empty()) addAll(byId_, e->id);
    for (auto& c : e->classes) addAll(byClass_, c);
    addAll(byTag_, e->name);
    for (auto& [name, value] : e->attrs) addAll(byAttr_, name);
    if (e->parent && e->parent->type == Node::T::Document) cand_.insert(cand_.end(), root_.begin(), root_.end());
    cand_.insert(cand_.end(), uni_.begin(), uni_.end());
    std::sort(cand_.begin(), cand_.end());
    cand_.erase(std::unique(cand_.begin(), cand_.end()), cand_.end());

    std::vector<std::pair<uint64_t, const Decl*>> L;
    std::vector<Decl> H;
    hints(e, H);
    for (auto& d : H) L.push_back({key(1, 0, 0), &d});
    // A sheet of a shadow tree styles the elements of that tree, its host (:host) and the host's children
    // (::slotted()); the document's sheets style everything else. User-agent rules apply everywhere.
    const Node* own = g_shadowRoots ? shadow_root_of(e) : nullptr;
    const Node* asHost = e->shadow.get();
    const Node* asSlotted = e->parent && e->parent->shadow ? e->parent->shadow.get() : nullptr;
    for (uint32_t idx : cand_) {
        const IR& ir = all_[idx];
        int role = ir.ua || ir.scope == own ? 0 : asHost && ir.scope == asHost ? 1 : asSlotted && ir.scope == asSlotted ? 2 : -1;
        if (role < 0) continue;
        g_selectorScope = ir.ua ? own : ir.scope;
        g_scopeRole = role;
        bool hit = match_at(ir.r->sel, (int)ir.r->sel.parts.size() - 1, e);
        g_selectorScope = nullptr;
        g_scopeRole = 0;
        if (!hit) continue;
        for (const Decl& d : *ir.r->decls) {
            int lvl = d.important ? (ir.ua ? 5 : 3) : (ir.ua ? 0 : 1);
            // rules that reach out of a shadow tree give way to the outer tree's own rules for the element (CSS Scoping 3.4)
            L.push_back({key(lvl, role ? 0 : ir.r->sel.spec, role ? 0 : ir.order, role ? 0 : ir.layerRank), &d});
        }
    }
    std::vector<Decl> IL;
    if (auto st = e->attr("style")) {
        IL = parse_decls(*st);
        for (auto& d : IL) L.push_back({key(d.important ? 4 : 2, 0, 0), &d});
    }
    std::stable_sort(L.begin(), L.end(), [](auto& a, auto& b) { return a.first < b.first; });

    // Pass 0: custom properties.
    bool anyVar = false;
    for (auto& p : L)
        if (p.second->prop.size() > 2 && p.second->prop[0] == '-' && p.second->prop[1] == '-') anyVar = true;
    if (anyVar) {
        std::unordered_map<std::string, std::string> set;
        for (auto& p : L)
            if (p.second->prop.size() > 2 && p.second->prop[0] == '-' && p.second->prop[1] == '-') set[p.second->prop] = p.second->val;
        s->vars = VarMap::over(s->vars, std::move(set));
    }
    // Pass 1: font properties (em units elsewhere depend on font-size). Pass 2: the rest.
    for (auto& p : L)
        if (istarts(p.second->prop, "font")) apply(*s, parent, p.second->prop, p.second->val);
    for (auto& p : L) {
        const std::string& pr = p.second->prop;
        if (!istarts(pr, "font") && !(pr.size() > 1 && pr[0] == '-' && pr[1] == '-')) apply(*s, parent, pr, p.second->val);
    }
    for (int i = 0; i < 4; ++i) {
        if (s->borderStyle[i] == 0) s->border[i] = 0;
        if (s->borderColor[i].a < 0) s->borderColor[i] = s->color;
    }
    // No compositor yet: fully transparent elements are simply not painted (unless an animation would reveal them).
    resolveAnimations(e, *s);
    if (s->opacity0 && !s->animated && !s->animOpacity) s->hidden = true;
    // an element whose opacity runs is painted with it (anim.h); an animated one that starts at 0 is shown as it will end
    s->alpha = parent.alpha * (s->opacity0 || s->animOpacity ? 1.f : s->opacity);
    if (s->masked) s->bg = Color{};
    // Out-of-flow boxes are blockified (CSS 2 9.7); an absolutely positioned box does not float.
    if (s->position == 2 || s->position == 3) s->floatSide = 0;
    if (s->position == 2 || s->position == 3 || s->floatSide) {
        if (s->display == Display::Inline || s->display == Display::InlineBlock) s->display = Display::Block;
        else if (s->display == Display::InlineFlex) s->display = Display::Flex;
        else if (s->display == Display::InlineGrid) s->display = Display::Grid;
    }
    e->style = std::move(s);
    if (e->style->display != Display::None) applyCounters(*e->style, e->style->display == Display::ListItem, depth_);

    // ::before and ::after (CSS 2 12.1): their rules cascade like the element's own, over its style
    e->before.reset();
    e->after.reset();
    if (pseudoById_.empty() && pseudoByClass_.empty() && pseudoByTag_.empty() && pseudoUni_.empty()) return;
    if (e->style->display == Display::None || replaced_like(e)) return;
    cand_.clear();
    if (!e->id.empty()) addAll(pseudoById_, e->id);
    for (auto& c : e->classes) addAll(pseudoByClass_, c);
    addAll(pseudoByTag_, e->name);
    cand_.insert(cand_.end(), pseudoUni_.begin(), pseudoUni_.end());
    if (cand_.empty()) return;
    std::sort(cand_.begin(), cand_.end());
    cand_.erase(std::unique(cand_.begin(), cand_.end()), cand_.end());
    Cascade P[3];
    for (uint32_t idx : cand_) {
        const IR& ir = all_[idx];
        int role = ir.ua || ir.scope == own ? 0 : asHost && ir.scope == asHost ? 1 : asSlotted && ir.scope == asSlotted ? 2 : -1;
        if (role < 0) continue;
        g_selectorScope = ir.ua ? own : ir.scope;
        g_scopeRole = role;
        bool hit = match_at(ir.r->sel, (int)ir.r->sel.parts.size() - 1, e);
        g_selectorScope = nullptr;
        g_scopeRole = 0;
        if (!hit) continue;
        for (const Decl& d : *ir.r->decls) {
            int lvl = d.important ? (ir.ua ? 5 : 3) : (ir.ua ? 0 : 1);
            P[ir.r->sel.parts.back().pseudoElement].push_back(
                {key(lvl, role ? 0 : ir.r->sel.spec, role ? 0 : ir.order, role ? 0 : ir.layerRank), &d});
        }
    }
    for (uint8_t kind = 1; kind <= 2; ++kind)
        if (!P[kind].empty()) computePseudo(e, kind, P[kind]);
}

namespace {
// A CSS string token's value: the quotes taken off and the escapes resolved (CSS Syntax 3, 4.3.5 and 4.3.7)
std::string css_string_value(std::string_view s, size_t& i) {
    char q = s[i++];
    std::string out;
    while (i < s.size() && s[i] != q) {
        char ch = s[i++];
        if (ch != '\\') { out += ch; continue; }
        if (i >= s.size()) break;
        if (s[i] == '\n') { ++i; continue; }
        int n = 0;
        uint32_t cp = 0;
        while (n < 6 && i < s.size() && isxdigit((unsigned char)s[i])) {
            cp = cp * 16 + (uint32_t)(isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower(s[i]) - 'a' + 10));
            ++i;
            ++n;
        }
        if (n == 0) { out += s[i++]; continue; }
        if (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
        else {
            out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 63));
            out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63));
        }
    }
    if (i < s.size()) ++i;
    return out;
}
// The text `content` generates for an element's ::before or ::after (CSS Generated Content 3, 1.1), or false when it
// generates no box: none, normal, or a value this engine cannot read. `counter` gives the values of a counter in
// scope, outermost first.
bool generated_text(const Node* e, std::string_view v, const std::function<std::vector<int>(const std::string&)>& counter,
                    std::string& out) {
    std::string_view t = trim(v);
    std::string lv = lower(t);
    if (lv.empty() || lv == "none" || lv == "normal") return false;
    size_t i = 0;
    while (i < t.size()) {
        if (isws(t[i])) { ++i; continue; }
        if (t[i] == '"' || t[i] == '\'') { out += css_string_value(t, i); continue; }
        if (t[i] == '/') break;  // alternative text for speech follows
        size_t st = i;
        while (i < t.size() && (isalnum((unsigned char)t[i]) || t[i] == '-' || t[i] == '_')) ++i;
        std::string word = lower(t.substr(st, i - st));
        if (i < t.size() && t[i] == '(') {
            size_t close = t.find(')', i);
            if (close == std::string_view::npos) return false;
            std::string arg(trim(t.substr(i + 1, close - i - 1)));
            i = close + 1;
            if (word == "attr") {
                if (auto a = e->attr(lower(arg))) out += *a;
            } else if (word == "counter" || word == "counters") {  // CSS Lists 3, 4.8
                auto args = split_top(arg, ',');
                size_t want = word == "counter" ? 1 : 2;
                if (args.size() < want || args.size() > want + 1) return false;
                std::string sep;
                if (word == "counters") {
                    std::string_view a = trim(args[1]);
                    size_t k = 0;
                    if (a.empty() || (a[0] != '"' && a[0] != '\'')) return false;
                    sep = css_string_value(a, k);
                }
                int style = 3;
                if (args.size() > want) style = list_kw(lower(trim(args[want])));
                if (style < 0) style = 3;  // a style defined by @counter-style, which is not kept: decimal
                std::vector<int> values = counter(std::string(trim(args[0])));
                if (values.empty()) values.push_back(0);  // none in scope: a new one at 0
                if (word == "counter") values.erase(values.begin(), values.end() - 1);
                for (size_t k = 0; k < values.size(); ++k) out += (k ? sep : "") + counter_text(values[k], (uint8_t)style);
            } else if (word != "url" && word != "image-set" && word != "linear-gradient")
                return false;
            continue;
        }
        if (word == "open-quote") out += "\xE2\x80\x9C";
        else if (word == "close-quote") out += "\xE2\x80\x9D";
        else if (word == "no-open-quote" || word == "no-close-quote") {
        } else
            return false;
        if (i == st) ++i;
    }
    return true;
}
}  // namespace

void StyleEngine::computePseudo(Node* e, uint8_t kind, Cascade& L) {
    std::stable_sort(L.begin(), L.end(), [](auto& a, auto& b) { return a.first < b.first; });
    const Style& parent = *e->style;
    auto s = std::make_unique<Style>();
    inherit(*s, parent);
    std::unordered_map<std::string, std::string> set;
    for (auto& p : L)
        if (p.second->prop.size() > 2 && p.second->prop[0] == '-' && p.second->prop[1] == '-') set[p.second->prop] = p.second->val;
    if (!set.empty()) s->vars = VarMap::over(s->vars, std::move(set));
    for (auto& p : L)
        if (istarts(p.second->prop, "font")) apply(*s, parent, p.second->prop, p.second->val);
    for (auto& p : L) {
        const std::string& pr = p.second->prop;
        if (!istarts(pr, "font") && !(pr.size() > 1 && pr[0] == '-' && pr[1] == '-')) apply(*s, parent, pr, p.second->val);
    }
    for (int i = 0; i < 4; ++i) {
        if (s->borderStyle[i] == 0) s->border[i] = 0;
        if (s->borderColor[i].a < 0) s->borderColor[i] = s->color;
    }
    if (s->opacity0 && !s->animated) s->hidden = true;
    s->alpha = parent.alpha * (s->opacity0 ? 1.f : s->opacity);
    if (s->position == 2 || s->position == 3) {
        if (s->display == Display::Inline || s->display == Display::InlineBlock) s->display = Display::Block;
        else if (s->display == Display::InlineFlex) s->display = Display::Flex;
        else if (s->display == Display::InlineGrid) s->display = Display::Grid;
    }
    std::string content = lower(trim(s->content));
    if (s->display == Display::None || content.empty() || content == "none" || content == "normal") return;
    std::string text;
    if (kind == 1) {
        applyCounters(*s, false, depth_ + 1);
        if (!generatedText(e, *s, text)) return;
    }
    auto box = std::make_unique<Node>();
    box->type = Node::T::Element;
    box->name = kind == 1 ? "::before" : "::after";
    box->parent = e;
    box->style = std::move(s);
    if (!text.empty()) {
        auto t = std::make_unique<Node>();
        t->type = Node::T::Text;
        t->text = std::move(text);
        box->append(std::move(t));
    }
    (kind == 1 ? e->before : e->after) = std::move(box);
}

bool StyleEngine::generatedText(Node* e, const Style& s, std::string& out) {
    auto values = [&](const std::string& name) {
        std::vector<int> r;
        for (auto& c : counters_)
            if (c.name == name) r.push_back(c.value);
        return r;
    };
    return generated_text(e, s.content, values, out);
}

namespace {
float time_ms(const std::string& t) {  // <time>: s or ms
    float n;
    std::string_view u;
    if (!parse_num(lower(t), n, u)) return 0;
    return u == "ms" ? n : u == "s" ? n * 1000 : 0;
}
bool same_transform(const std::shared_ptr<const TransformList>& a, const std::shared_ptr<const TransformList>& b) {
    if (a == b) return true;
    float ma[6] = {1, 0, 0, 1, 0, 0}, mb[6] = {1, 0, 0, 1, 0, 0};
    if (a) transform_matrix(*a, 100, 100, ma);
    if (b) transform_matrix(*b, 100, 100, mb);
    for (int i = 0; i < 6; ++i)
        if (std::fabs(ma[i] - mb[i]) > 1e-4f) return false;
    return true;
}
}  // namespace

// Animations and transitions (CSS Animations 1, CSS Transitions 1): each animation gets its @keyframes and keeps
// the time it started while its name stays; a change of opacity or transform that a transition covers starts one
// from where the old value is now
void StyleEngine::resolveAnimations(Node* e, Style& s) {
    double now = now_ms();
    std::shared_ptr<NodeAnim>& na = e->anim;
    if (s.animLists && s.animated) {
        const AnimLists& L = *s.animLists;
        auto at = [](const std::vector<std::string>& v, size_t i, const char* dflt) { return v.empty() ? std::string(dflt) : v[i % v.size()]; };
        auto list = std::make_shared<std::vector<AnimationDecl>>();
        for (size_t i = 0; i < L.names.size(); ++i) {
            AnimationDecl d;
            d.name = L.names[i];
            if (lower(d.name) == "none") continue;
            if (d.name.size() >= 2 && (d.name[0] == '"' || d.name[0] == '\'')) d.name = d.name.substr(1, d.name.size() - 2);
            d.duration = time_ms(at(L.durations, i, "0s"));
            d.delay = time_ms(at(L.delays, i, "0s"));
            std::string count = lower(at(L.iterations, i, "1"));
            d.iterations = count == "infinite" ? -1.f : std::max(0.f, (float)atof(count.c_str()));
            std::string dir = lower(at(L.directions, i, "normal"));
            d.direction = dir == "reverse" ? 1 : dir == "alternate" ? 2 : dir == "alternate-reverse" ? 3 : 0;
            std::string fill = lower(at(L.fills, i, "none"));
            d.fill = fill == "forwards" ? 1 : fill == "backwards" ? 2 : fill == "both" ? 3 : 0;
            d.paused = lower(at(L.states, i, "running")) == "paused";
            parse_timing(at(L.timings, i, "ease"), d.timing);
            if (auto kf = keyframes_.find(d.name); kf != keyframes_.end()) d.frames = kf->second;
            if (d.frames) {
                s.animOpacity = s.animOpacity || d.frames->opacity;
                s.animTransform = s.animTransform || d.frames->transform;
            }
            list->push_back(std::move(d));
        }
        if (!list->empty()) {
            if (!na) na = std::make_shared<NodeAnim>();
            std::vector<std::pair<std::string, double>> starts;
            for (auto& d : *list) {
                double start = now;
                for (auto& [name, t] : na->starts)
                    if (name == d.name) start = t;
                starts.push_back({d.name, start});
            }
            if (starts.size() != na->starts.size()) na->seen.clear();
            na->starts = std::move(starts);
            s.animations = std::move(list);
        } else if (na) {
            na->starts.clear();
            na->seen.clear();
        }
    } else if (na) {
        na->starts.clear();
        na->seen.clear();
    }
    if (s.transitionLists && !na) na = std::make_shared<NodeAnim>();
    if (!na) return;
    // transitions (CSS Transitions 1, 3): only a change from a style the element had before starts one
    if (s.transitionLists && na->styled) {
        const TransitionLists& T = *s.transitionLists;
        auto find = [&](const char* property, TransitionDecl& out) {
            bool found = false;
            for (size_t i = 0; i < T.properties.size(); ++i) {
                std::string p = lower(T.properties[i]);
                if (p != property && p != "all") continue;
                auto at = [&](const std::vector<std::string>& v, const char* dflt) { return v.empty() ? std::string(dflt) : v[i % v.size()]; };
                out.property = p;
                out.duration = time_ms(at(T.durations, "0s"));
                out.delay = time_ms(at(T.delays, "0s"));
                parse_timing(at(T.timings, "ease"), out.timing);
                found = out.duration > 0 || out.delay > 0;  // the last that names the property counts
            }
            return found;
        };
        AnimPaint current;  // where a running transition has got to
        current.state = *na;
        current.baseOpacity = na->lastOpacity;
        current.baseTransform = na->lastTransform;
        current.opacity = current.transform = true;
        float nowOpacity;
        std::shared_ptr<const TransformList> nowTransform;
        animate(current, now, nowOpacity, nowTransform);
        TransitionDecl t;
        if (std::fabs(s.opacity - na->lastOpacity) > 1e-4f && find("opacity", t)) {
            std::erase_if(na->transitions, [](const NodeAnim::Transition& x) { return !x.transform; });
            NodeAnim::Transition tr;
            tr.fromOpacity = nowOpacity, tr.toOpacity = s.opacity, tr.start = now, tr.duration = t.duration, tr.delay = t.delay, tr.timing = t.timing;
            na->transitions.push_back(tr);
        }
        if (!same_transform(s.transform, na->lastTransform) && find("transform", t)) {
            std::erase_if(na->transitions, [](const NodeAnim::Transition& x) { return x.transform; });
            NodeAnim::Transition tr;
            tr.transform = true, tr.from = nowTransform, tr.to = s.transform, tr.start = now, tr.duration = t.duration, tr.delay = t.delay, tr.timing = t.timing;
            na->transitions.push_back(tr);
        }
    }
    // finished transitions are done with; a value that changed with none running ends them too
    std::erase_if(na->transitions, [&](const NodeAnim::Transition& x) {
        if (now >= x.start + x.delay + x.duration) return true;
        return x.transform ? !same_transform(x.to, s.transform) : std::fabs(x.toOpacity - s.opacity) > 1e-4f;
    });
    for (auto& x : na->transitions) (x.transform ? s.animTransform : s.animOpacity) = true;
    na->styled = true;
    na->lastOpacity = s.opacity;
    na->lastTransform = s.transform;
}

// Selector access for the scripting layer (querySelector, matches, closest).
bool selector_parse(std::string_view text, Selector& out) { return parse_selector(text, out) && !out.parts.empty(); }
bool selector_matches(const Selector& s, const Node* e) {  // a pseudo-element is never an element the DOM can give
    return !s.parts.empty() && !s.parts.back().pseudoElement && match_at(s, (int)s.parts.size() - 1, e);
}

}  // namespace tb
