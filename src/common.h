#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tb {

std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view s);
void append_utf8(std::string& out, uint32_t cp);
double now_ms();
std::string escape_html(std::string_view s);

inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }
inline std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = lc(c);
    return r;
}
inline bool isws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
inline bool isalpha_(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isdigit_(char c) { return c >= '0' && c <= '9'; }
inline bool isalnum_(char c) { return isalpha_(c) || isdigit_(c); }
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && isws(s.front())) s.remove_prefix(1);
    while (!s.empty() && isws(s.back())) s.remove_suffix(1);
    return s;
}
inline bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lc(a[i]) != lc(b[i])) return false;
    return true;
}
inline bool istarts(std::string_view s, std::string_view p) { return s.size() >= p.size() && ieq(s.substr(0, p.size()), p); }
inline bool contains(std::string_view s, std::string_view p) { return s.find(p) != std::string_view::npos; }

struct Color {
    float r = 0, g = 0, b = 0, a = 0;
    bool transparent() const { return a <= 0.001f; }
};
inline Color rgb(uint32_t hex, float a = 1.f) {
    return {((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a};
}

}  // namespace tb
