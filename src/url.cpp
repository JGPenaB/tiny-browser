#include "url.h"

namespace tb {

static constexpr auto npos = std::string_view::npos;

static bool scheme_char(char c) { return isalnum_(c) || c == '+' || c == '-' || c == '.'; }

static std::string remove_dots(std::string_view p) {
    if (p.empty()) return "/";
    std::vector<std::string_view> out;
    size_t i = (p[0] == '/') ? 1 : 0;
    bool trailing = false;
    while (true) {
        size_t j = p.find('/', i);
        bool last = j == npos;
        std::string_view seg = p.substr(i, last ? npos : j - i);
        if (seg == "." || seg == "%2e") {
            if (last) trailing = true;
        } else if (seg == ".." || ieq(seg, "%2e%2e")) {
            if (!out.empty()) out.pop_back();
            if (last) trailing = true;
        } else {
            out.push_back(seg);
        }
        if (last) break;
        i = j + 1;
    }
    std::string r;
    for (auto s : out) {
        r += '/';
        r += s;
    }
    if (trailing || r.empty()) r += '/';
    return r;
}

Url Url::parse(std::string_view s) {
    Url u;
    s = trim(s);
    size_t c = s.find(':');
    if (c == npos || c == 0 || !isalpha_(s[0])) return u;
    for (size_t i = 0; i < c; ++i)
        if (!scheme_char(s[i])) return u;
    u.scheme = lower(s.substr(0, c));
    std::string_view rest = s.substr(c + 1);
    size_t h = rest.find('#');
    if (h != npos) {
        u.fragment = std::string(rest.substr(h + 1));
        u.hasFragment = true;
        rest = rest.substr(0, h);
    }
    bool special = u.scheme == "http" || u.scheme == "https" || u.scheme == "file" || u.scheme == "ftp" ||
                   u.scheme == "ws" || u.scheme == "wss";
    if (special) {
        bool hasAuth = rest.substr(0, 2) == "//";
        if (hasAuth) rest.remove_prefix(2);
        if (hasAuth || u.scheme != "file") {
            size_t e = rest.find_first_of("/?");
            std::string_view auth = rest.substr(0, e);
            rest = e == npos ? std::string_view{} : rest.substr(e);
            size_t at = auth.rfind('@');
            if (at != npos) auth = auth.substr(at + 1);
            size_t colon = npos;
            if (!auth.empty() && auth[0] == '[') {
                size_t rb = auth.find(']');
                if (rb != npos && rb + 1 < auth.size() && auth[rb + 1] == ':') colon = rb + 1;
            } else {
                colon = auth.rfind(':');
            }
            if (colon != npos) {
                u.port = std::string(auth.substr(colon + 1));
                auth = auth.substr(0, colon);
            }
            u.host = lower(auth);
        }
        u.hierarchical = true;
        if ((u.scheme == "http" && u.port == "80") || (u.scheme == "https" && u.port == "443")) u.port.clear();
        for (char ch : u.port)
            if (!isdigit_(ch)) return Url{};
    }
    size_t q = rest.find('?');
    if (q != npos) {
        u.query = std::string(rest.substr(q + 1));
        u.hasQuery = true;
        rest = rest.substr(0, q);
    }
    u.path = std::string(rest);
    if (u.hierarchical) u.path = remove_dots(u.path.empty() ? "/" : u.path);
    if ((u.scheme == "http" || u.scheme == "https") && u.host.empty()) return Url{};
    u.valid = true;
    return u;
}

std::string Url::str(bool withFragment) const {
    std::string r = scheme + ":";
    if (hierarchical) {
        r += "//";
        r += hostport();
    }
    r += path;
    if (hasQuery) {
        r += '?';
        r += query;
    }
    if (withFragment && hasFragment) {
        r += '#';
        r += fragment;
    }
    return r;
}

std::string resolve_url(const std::string& base, std::string_view rel) {
    // Strip ASCII whitespace that commonly sneaks into attributes.
    std::string clean;
    for (char ch : trim(rel))
        if (ch != '\n' && ch != '\r' && ch != '\t') clean += ch;
    std::string_view r = clean;
    size_t c = r.find(':');
    if (c != npos && c > 0 && isalpha_(r[0])) {
        bool ok = true;
        for (size_t i = 0; i < c; ++i)
            if (!scheme_char(r[i])) ok = false;
        if (ok) {
            if (istarts(r, "data:") || istarts(r, "javascript:") || istarts(r, "mailto:")) return std::string(r);
            Url u = Url::parse(r);
            if (u.valid) return u.str();
        }
    }
    Url b = Url::parse(base);
    if (!b.valid || !b.hierarchical) return std::string(r);
    if (r.empty()) {
        b.hasFragment = false;
        return b.str();
    }
    if (r.substr(0, 2) == "//") return Url::parse(b.scheme + ":" + std::string(r)).str();
    std::string prefix = b.scheme + "://" + b.hostport();
    std::string out;
    if (r[0] == '#') {
        b.fragment = std::string(r.substr(1));
        b.hasFragment = true;
        return b.str();
    }
    if (r[0] == '?')
        out = prefix + b.path + std::string(r);
    else if (r[0] == '/')
        out = prefix + std::string(r);
    else
        out = prefix + b.path.substr(0, b.path.rfind('/') + 1) + std::string(r);
    Url u = Url::parse(out);
    return u.valid ? u.str() : out;
}

std::string strip_fragment(const std::string& url) {
    size_t h = url.find('#');
    return h == std::string::npos ? url : url.substr(0, h);
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int a = hexv(s[i + 1]), b = hexv(s[i + 2]);
            if (a >= 0 && b >= 0) {
                r += char(a * 16 + b);
                i += 2;
                continue;
            }
        }
        r += s[i];
    }
    return r;
}

std::string percent_encode_path(std::string_view s) {
    static const char* hx = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '`' || c == '{' || c == '}') {
            r += '%';
            r += hx[c >> 4];
            r += hx[c & 15];
        } else {
            r += char(c);
        }
    }
    return r;
}

std::string form_encode(std::string_view s) {
    static const char* hx = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (isalnum_(char(c)) || c == '-' || c == '_' || c == '.' || c == '~')
            r += char(c);
        else if (c == ' ')
            r += '+';
        else {
            r += '%';
            r += hx[c >> 4];
            r += hx[c & 15];
        }
    }
    return r;
}

}  // namespace tb
