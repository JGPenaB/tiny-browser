#include "text.h"

#include "css.h"

using Microsoft::WRL::ComPtr;

namespace tb {

TextSystem* TextSystem::inst = nullptr;

bool TextSystem::init() {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(f_.GetAddressOf()))))
        return false;
    f_->GetSystemFontCollection(&sys_);
    inst = this;
    StyleEngine::fontResolver = [](std::string_view l) { return inst->resolveFamily(l); };
    return true;
}

static std::wstring key_of(const Style& s) {
    std::wstring k = s.family;
    k += L'|';
    k += std::to_wstring((int)std::lround(s.fontSize * 100));
    k += L'|';
    k += std::to_wstring(s.weight);
    k += s.italic ? L"i" : L"n";
    return k;
}

bool TextSystem::hasFamily(const std::wstring& name) {
    auto it = famExists_.find(name);
    if (it != famExists_.end()) return it->second;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (sys_) sys_->FindFamilyName(name.c_str(), &idx, &exists);
    famExists_[name] = exists != FALSE;
    return exists != FALSE;
}

TextSystem::Entry& TextSystem::entry(const Style& s) {
    std::wstring k = key_of(s);
    auto it = fmts_.find(k);
    if (it != fmts_.end()) return it->second;
    Entry e;
    float size = std::max(1.f, s.fontSize);
    DWRITE_FONT_WEIGHT w = (DWRITE_FONT_WEIGHT)std::clamp<int>(s.weight, 1, 999);
    DWRITE_FONT_STYLE st = s.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    std::wstring fam = hasFamily(s.family) ? s.family : L"Segoe UI";
    f_->CreateTextFormat(fam.c_str(), sys_.Get(), w, st, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &e.fmt);
    float asc = 0.9f, desc = 0.22f;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    sys_->FindFamilyName(fam.c_str(), &idx, &exists);
    if (exists) {
        ComPtr<IDWriteFontFamily> ff;
        ComPtr<IDWriteFont> font;
        if (SUCCEEDED(sys_->GetFontFamily(idx, &ff)) &&
            SUCCEEDED(ff->GetFirstMatchingFont(w, DWRITE_FONT_STRETCH_NORMAL, st, &font))) {
            DWRITE_FONT_METRICS m;
            font->GetMetrics(&m);
            asc = m.ascent / (float)m.designUnitsPerEm;
            desc = m.descent / (float)m.designUnitsPerEm;
        }
    }
    e.asc = asc * size;
    e.desc = desc * size;
    if (e.fmt) {
        e.fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        e.fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, e.asc + e.desc, e.asc);
    }
    e.key = k;
    return fmts_.emplace(k, std::move(e)).first->second;
}

float TextSystem::width(const Style& s, std::wstring_view t) {
    if (t.empty()) return 0;
    Entry& e = entry(s);
    std::wstring k = e.key;
    k += L'\x1f';
    k.append(t);
    auto it = widths_.find(k);
    if (it != widths_.end()) return it->second;
    float w = 0;
    ComPtr<IDWriteTextLayout> l;
    if (e.fmt && SUCCEEDED(f_->CreateTextLayout(t.data(), (UINT32)t.size(), e.fmt.Get(), 100000.f, 1000.f, &l))) {
        DWRITE_TEXT_METRICS m;
        l->GetMetrics(&m);
        w = m.widthIncludingTrailingWhitespace;
    }
    if (widths_.size() > 400000) widths_.clear();
    widths_.emplace(std::move(k), w);
    return w;
}

void TextSystem::metrics(const Style& s, float& asc, float& desc) {
    Entry& e = entry(s);
    asc = e.asc;
    desc = e.desc;
}

ComPtr<IDWriteTextLayout> TextSystem::layout(const Style& s, std::wstring_view t, bool underline, bool strike) {
    Entry& e = entry(s);
    ComPtr<IDWriteTextLayout> l;
    if (!e.fmt) return l;
    f_->CreateTextLayout(t.data(), (UINT32)t.size(), e.fmt.Get(), 100000.f, 1000.f, &l);
    if (l) {
        DWRITE_TEXT_RANGE r{0, (UINT32)t.size()};
        if (underline) l->SetUnderline(TRUE, r);
        if (strike) l->SetStrikethrough(TRUE, r);
    }
    return l;
}

std::wstring TextSystem::resolveFamily(std::string_view list) {
    std::string key(list);
    auto it = famCache_.find(key);
    if (it != famCache_.end()) return it->second;
    std::wstring result = L"Times New Roman";
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string_view::npos) j = list.size();
        std::string_view name = trim(list.substr(i, j - i));
        if (name.size() >= 2 && (name[0] == '"' || name[0] == '\'')) name = name.substr(1, name.size() - 2);
        std::string l = lower(name);
        std::wstring cand;
        if (l == "serif" || l == "ui-serif") cand = L"Times New Roman";
        else if (l == "sans-serif") cand = L"Arial";
        else if (l == "monospace" || l == "ui-monospace" || l == "menlo" || l == "monaco" || l == "sf mono" ||
                 l == "sfmono-regular" || l == "courier" || l == "liberation mono" || l == "dejavu sans mono")
            cand = L"Consolas";
        else if (l == "system-ui" || l == "-apple-system" || l == "blinkmacsystemfont" || l == "ui-sans-serif" ||
                 l == "san francisco" || l == "sf pro text" || l == "sf pro display" || l == "roboto" || l == "ubuntu" ||
                 l == "cantarell" || l == "noto sans" || l == "oxygen" || l == "fira sans" || l == "droid sans")
            cand = L"Segoe UI";
        else if (l == "helvetica" || l == "helvetica neue" || l == "liberation sans" || l == "arimo")
            cand = L"Arial";
        else if (l == "cursive") cand = L"Comic Sans MS";
        else if (l == "fantasy") cand = L"Impact";
        else if (l == "emoji" || l == "apple color emoji" || l == "noto color emoji") cand = L"Segoe UI Emoji";
        else if (!name.empty()) {
            std::wstring w = widen(name);
            if (hasFamily(w)) cand = w;
        }
        if (!cand.empty() && hasFamily(cand)) {
            result = cand;
            break;
        }
        i = j + 1;
    }
    famCache_[key] = result;
    return result;
}

}  // namespace tb
