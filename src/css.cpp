#include "css.h"

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

bool parse_calc(std::string_view inner, float fs, Len& out) {
    auto T = tokens(inner);
    float px = 0, pct = 0;
    bool hasPct = false;
    float sign = 1;
    char pend = 0;
    float lastPx = 0, lastPct = 0;
    for (auto& t : T) {
        if (t == "+") {
            sign = 1;
            continue;
        }
        if (t == "-") {
            sign = -1;
            continue;
        }
        if (t == "*" || t == "/") {
            pend = t[0];
            continue;
        }
        float n;
        std::string_view u;
        if (pend && parse_num(t, n, u) && u.empty()) {
            // apply to the last term
            px -= lastPx;
            pct -= lastPct;
            if (pend == '*') {
                lastPx *= n;
                lastPct *= n;
            } else if (n != 0) {
                lastPx /= n;
                lastPct /= n;
            }
            px += lastPx;
            pct += lastPct;
            pend = 0;
            continue;
        }
        Len l;
        std::string_view tv = t;
        if (tv.size() > 1 && tv.front() == '(' && tv.back() == ')') tv = tv.substr(1, tv.size() - 2);
        if (istarts(tv, "calc(")) tv = tv.substr(5, tv.size() - 6);
        if (tv != std::string_view(t) && tv.find(' ') != npos) {
            if (!parse_calc(tv, fs, l)) return false;
        } else if (!parse_len(tv, fs, l, false)) {
            if (parse_num(tv, n, u) && u.empty()) {
                l = {Len::Px, n};
            } else
                return false;
        }
        lastPx = lastPct = 0;
        if (l.u == Len::Px)
            lastPx = sign * l.v;
        else if (l.u == Len::Pct) {
            lastPct = sign * l.v;
            lastPx = sign * l.off;
            hasPct = true;
        } else
            return false;
        px += lastPx;
        pct += lastPct;
        sign = 1;
    }
    if (hasPct)
        out = {Len::Pct, pct, px};
    else
        out = {Len::Px, px};
    return true;
}

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
    if (istarts(s, "calc(") || istarts(s, "-webkit-calc(")) {
        size_t a = s.find('('), b = s.rfind(')');
        if (b == std::string::npos || b < a) return false;
        return parse_calc(std::string_view(s).substr(a + 1, b - a - 1), fs, out);
    }
    if (istarts(s, "min(") || istarts(s, "max(") || istarts(s, "clamp(")) {
        // Approximate: use the last argument for min()/clamp(), first for max().
        size_t a = s.find('('), b = s.rfind(')');
        if (b == std::string::npos) return false;
        auto args = split_top(std::string_view(s).substr(a + 1, b - a - 1), ',');
        std::string_view pick = istarts(s, "max(") ? args.front() : (istarts(s, "clamp(") && args.size() == 3 ? args[1] : args.back());
        return parse_len(pick, fs, out, allowAuto);
    }
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
    return false;
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
            if (elem || in(name, {"before", "after", "first-line", "first-letter", "selection", "placeholder", "marker",
                                  "backdrop", "file-selector-button"})) {
                c.never = true;
                continue;
            }
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
            else if (name == "nth-child" || name == "nth-last-child" || name == "nth-of-type") {
                p.k = name == "nth-last-child" ? Pseudo::NthLastChild : Pseudo::NthChild;
                if (!parse_nth(arg, p.a, p.b)) c.never = true;
            } else if (name == "not") {
                auto inner = std::make_shared<Compound>();
                size_t k = 0;
                uint32_t sp2 = 0;
                std::string_view a = trim(arg);
                if (!parse_compound(a, k, *inner, sp2) || k != a.size()) {
                    c.never = true;
                    continue;
                }
                p.k = Pseudo::Not;
                p.inner = inner;
            } else if (name == "defined") {
                p.k = Pseudo::Defined;
            } else if (in(name, {"enabled", "visited-not", "scope", "lang", "dir", "ltr"})) {
                continue;  // treated as always-true
            } else {
                // hover, focus, active, visited, checked, is(), where(), has()... never match in M1.
                c.never = true;
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

float media_len(std::string_view v) {
    float n;
    std::string_view u;
    v = trim(v);
    if (!parse_num(v, n, u)) return -1;
    if (u == "em" || u == "rem") return n * 16;
    return n;
}

int parse_media(std::string_view prelude, Sheet& sh) {
    MediaQuery mq;
    for (auto alt : split_top(prelude, ',')) {
        std::string a = lower(trim(alt));
        if (a.empty() || a == "all" || a == "screen") {
            mq.alts.push_back({-1.f, -1.f});
            continue;
        }
        if (istarts(a, "not ") || istarts(a, "print") || contains(a, "speech")) continue;
        if (contains(a, "prefers-color-scheme") && contains(a, "dark")) continue;
        if (contains(a, "prefers-reduced-motion") && contains(a, "reduce")) continue;
        if (contains(a, "prefers-contrast") || contains(a, "forced-colors: active") || contains(a, "hover: none") ||
            contains(a, "pointer: coarse") || contains(a, "portrait") || contains(a, "-webkit-min-device-pixel-ratio") ||
            contains(a, "min-resolution"))
            continue;
        float mn = -1, mx = -1;
        auto grab = [&](const char* key, float& dst) {
            size_t p = a.find(key);
            if (p == std::string::npos) return;
            size_t c = a.find(':', p);
            if (c == std::string::npos) return;
            size_t e = a.find(')', c);
            dst = media_len(std::string_view(a).substr(c + 1, (e == std::string::npos ? a.size() : e) - c - 1));
        };
        grab("min-width", mn);
        grab("max-width", mx);
        // Range syntax: (width >= 600px), (width < 900px)
        for (size_t p = a.find("width"); p != std::string::npos; p = a.find("width", p + 5)) {
            if (p > 0 && a[p - 1] == '-') continue;
            size_t k = p + 5;
            while (k < a.size() && isws(a[k])) ++k;
            if (k >= a.size()) break;
            bool ge = a.compare(k, 2, ">=") == 0, le = a.compare(k, 2, "<=") == 0;
            bool gt = !ge && a[k] == '>', lt = !le && a[k] == '<';
            if (!(ge || le || gt || lt)) continue;
            k += (ge || le) ? 2 : 1;
            size_t e = a.find(')', k);
            float v = media_len(std::string_view(a).substr(k, (e == std::string::npos ? a.size() : e) - k));
            if (v < 0) continue;
            if (ge || gt) mn = v + (gt ? 0.02f : 0);
            if (le || lt) mx = v - (lt ? 0.02f : 0);
        }
        mq.alts.push_back({mn, mx});
    }
    sh.media.push_back(std::move(mq));
    return (int)sh.media.size() - 1;
}

void parse_block(std::string_view s, int media, Sheet& sh, int depth) {
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
            if (brace == npos) break;
            if (s[brace] == ';') {
                i = brace + 1;
                continue;
            }
            size_t end = block_end(s, brace);
            if (end == npos) end = s.size();
            std::string_view prelude = trim(s.substr(j, brace - j));
            std::string_view inner = s.substr(brace + 1, end - brace - 1);
            if (depth < 8) {
                if (kw == "media")
                    parse_block(inner, parse_media(prelude, sh), sh, depth + 1);
                else if (kw == "supports" || kw == "layer" || kw == "container" || kw == "scope" || kw == "document" ||
                         kw == "-moz-document")
                    parse_block(inner, media, sh, depth + 1);
            }
            i = end + 1;
            continue;
        }
        size_t brace = s.find('{', i);
        if (brace == npos) break;
        size_t end = block_end(s, brace);
        if (end == npos) end = s.size();
        std::string_view sel = s.substr(i, brace - i);
        std::string_view body = s.substr(brace + 1, end - brace - 1);
        // CSS nesting: treat nested rule blocks inside a declaration block conservatively (ignore them).
        auto decls = std::make_shared<std::vector<Decl>>(parse_decls(body));
        if (!decls->empty()) {
            for (auto part : split_top(sel, ',')) {
                Selector x;
                if (parse_selector(trim(part), x)) sh.rules.push_back({std::move(x), decls, media});
            }
        }
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

Sheet parse_sheet(std::string_view css) {
    Sheet sh;
    std::string clean = strip_comments(css);
    parse_block(clean, -1, sh, 0);
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
                if (p.inner && match_compound(*p.inner, e)) return false;
                break;
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
bool match_at(const Selector& s, int idx, const Node* e, int budget = 0) {
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

bool media_match(const MediaQuery& m, float vw) {
    for (auto& a : m.alts)
        if ((a.first < 0 || vw >= a.first) && (a.second < 0 || vw <= a.second)) return true;
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
        if (s.vars) {
            auto it = s.vars->find(name);
            if (it != s.vars->end()) rep = it->second;
        }
        if (depth < 8 && rep.find("var(") != std::string::npos) rep = subst_vars(rep, s, depth + 1);
        out += rep;
        i = k < v.size() ? k + 1 : k;
    }
    return out;
}

bool inherited_prop(std::string_view p) {
    return p == "color" || istarts(p, "font") || p == "line-height" || p == "text-align" || p == "white-space" ||
           istarts(p, "list-style") || p == "text-transform" || p == "visibility" || istarts(p, "text-decoration");
}

void copy_prop(Style& d, const Style& s, std::string_view p) {
    if (p == "color") d.color = s.color;
    else if (istarts(p, "background")) d.bg = s.bg;
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
    else if (p == "top" || p == "right" || p == "bottom" || p == "left" || p == "inset") std::copy(s.inset, s.inset + 4, d.inset);
    else if (p == "z-index") {
        d.zIndex = s.zIndex;
        d.zAuto = s.zAuto;
    } else if (istarts(p, "overflow")) d.overflow = s.overflow;
    else if (p == "float") d.floatSide = s.floatSide;
    else if (p == "border-radius") d.radius = s.radius;
    else if (p == "flex") {
        d.flexGrow = s.flexGrow;
        d.flexShrink = s.flexShrink;
        d.flexBasis = s.flexBasis;
    } else if (p == "flex-direction") d.flexDir = s.flexDir;
    else if (p == "flex-wrap") d.flexWrap = s.flexWrap;
    else if (p == "justify-content") d.justify = s.justify;
    else if (p == "align-items") d.alignItems = s.alignItems;
    else if (p == "align-self") d.alignSelf = s.alignSelf;
    else if (p == "gap") {
        d.gapRow = s.gapRow;
        d.gapCol = s.gapCol;
    }
}

void inherit(Style& s, const Style& p) {
    s.color = p.color;
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
    s.hidden = p.hidden;
    s.vars = p.vars;
}

bool parse_display(const std::string& v, Display& d) {
    if (v == "none") d = Display::None;
    else if (in(v, {"flex", "block flex", "-webkit-box", "-ms-flexbox", "-webkit-flex"})) d = Display::Flex;
    else if (in(v, {"inline-flex", "inline flex", "inline-flexbox", "-webkit-inline-box", "-webkit-inline-flex"}))
        d = Display::InlineFlex;
    else if (in(v, {"block", "flow-root", "grid", "block flow", "block grid", "table-caption", "block flow-root"}))
        d = Display::Block;
    else if (in(v, {"inline", "contents", "inline flow", "ruby", "ruby-text"})) d = Display::Inline;
    else if (in(v, {"inline-block", "inline-grid", "inline flow-root", "inline grid"})) d = Display::InlineBlock;
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
    if (t == "decimal" || t == "decimal-leading-zero") return 3;
    if (t == "none") return 4;
    if (t == "lower-alpha" || t == "lower-latin") return 5;
    if (t == "upper-alpha" || t == "upper-latin") return 6;
    if (t == "lower-roman") return 7;
    if (t == "upper-roman") return 8;
    return -1;
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

    if (prop == "display") {
        Display d;
        if (parse_display(lv, d)) s.display = d;
    } else if (prop == "color") {
        Color c;
        if (lv == "currentcolor") s.color = parent.color;
        else if (parse_color(lv, c)) s.color = c;
    } else if (prop == "background-color") {
        Color c;
        if (lv == "currentcolor") s.bg = s.color;
        else if (parse_color(lv, c)) s.bg = c;
    } else if (prop == "background") {
        s.bg = Color{};
        for (auto& t : T) {
            Color c;
            if (parse_color(t, c)) {
                s.bg = c;
            } else if (contains(t, "gradient(")) {
                size_t a = t.find('('), b = t.rfind(')');
                if (a == std::string::npos || b == std::string::npos) continue;
                for (auto arg : split_top(std::string_view(t).substr(a + 1, b - a - 1), ',')) {
                    auto at = tokens(arg);
                    if (!at.empty() && parse_color(at[0], c)) {
                        s.bg = c;
                        break;
                    }
                }
            }
        }
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
    } else if (prop == "opacity") {
        float v;
        std::string_view u;
        if (parse_num(lv, v, u)) s.opacity0 = (u == "%" ? v / 100.f : v) <= 0.01f;
    } else if (prop == "animation" || prop == "animation-name") {
        s.animated = lv != "none";
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
    } else if (prop == "justify-content") {
        std::string t = lv;
        if (istarts(t, "safe ")) t = t.substr(5);
        s.justify = (t == "center") ? 1 : (t == "flex-end" || t == "end" || t == "right") ? 2 : t == "space-between" ? 3 : t == "space-around" ? 4 : t == "space-evenly" ? 5 : 0;
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
    if (n == "ul" || n == "ol" || n == "li")
        if (auto t = e->attr("type")) {
            std::string v = *t;
            add("list-style-type", v == "1" ? "decimal" : v == "a" ? "lower-alpha" : v == "A" ? "upper-alpha" : v == "i" ? "lower-roman" : v == "I" ? "upper-roman" : lower(v));
        }
}

}  // namespace
const Node* g_selectorScope = nullptr;
namespace {
uint64_t key(int lvl, uint32_t spec, uint32_t order) {
    return (uint64_t(lvl) << 56) | (uint64_t(spec & 0x3FFFFFFF) << 24) | uint64_t(order & 0xFFFFFF);
}

}  // namespace

void StyleEngine::build(float vw) {
    all_.clear();
    byId_.clear();
    byClass_.clear();
    byTag_.clear();
    uni_.clear();
    hasMedia_ = false;
    uint32_t order = 1;
    for (auto& [sh, ua, scope] : sheets_) {
        for (auto& r : sh->rules) {
            ++order;
            if (r.media >= 0) {
                hasMedia_ = true;
                if (!media_match(sh->media[r.media], vw)) continue;
            }
            all_.push_back({&r, order, ua, scope});
            uint32_t idx = (uint32_t)all_.size() - 1;
            const Compound& last = r.sel.parts.back();
            if (last.never) continue;
            if (!last.id.empty())
                byId_[last.id].push_back(idx);
            else if (!last.classes.empty())
                byClass_[last.classes[0]].push_back(idx);
            else if (!last.tag.empty())
                byTag_[last.tag].push_back(idx);
            else
                uni_.push_back(idx);
        }
    }
}

void StyleEngine::compute(Document& d, float vw, float vh) {
    g_vw = vw;
    g_vh = vh;
    g_rootFont = 16;
    build(vw);
    Style root;
    root.family = resolve_family("serif");
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
    for (Node* k : FlatKids(e))
        if (k->type == Node::T::Element) computeTree(k, *e->style);
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
            L.push_back({key(lvl, role ? 0 : ir.r->sel.spec, role ? 0 : ir.order), &d});
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
        auto m = s->vars ? std::make_shared<VarMap>(*s->vars) : std::make_shared<VarMap>();
        for (auto& p : L)
            if (p.second->prop.size() > 2 && p.second->prop[0] == '-' && p.second->prop[1] == '-') (*m)[p.second->prop] = p.second->val;
        s->vars = std::move(m);
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
    if (s->opacity0 && !s->animated) s->hidden = true;
    if (s->masked) s->bg = Color{};
    // Out-of-flow boxes are blockified.
    if (s->position == 2 || s->position == 3) {
        if (s->display == Display::Inline || s->display == Display::InlineBlock) s->display = Display::Block;
        else if (s->display == Display::InlineFlex) s->display = Display::Flex;
    }
    e->style = std::move(s);
}

// Selector access for the scripting layer (querySelector, matches, closest).
bool selector_parse(std::string_view text, Selector& out) { return parse_selector(text, out) && !out.parts.empty(); }
bool selector_matches(const Selector& s, const Node* e) { return !s.parts.empty() && match_at(s, (int)s.parts.size() - 1, e); }

}  // namespace tb
