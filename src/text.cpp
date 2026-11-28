#include "text.h"

#include <dwrite_3.h>

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

IDWriteFontCollection* TextSystem::collectionFor(const std::wstring& family) {
    auto it = web_.find(family);
    return it != web_.end() && it->second.collection ? it->second.collection.Get() : sys_.Get();
}

bool TextSystem::addWebFont(const std::wstring& name, const std::string& bytes) {
    ComPtr<IDWriteFactory5> f5;
    if (bytes.empty() || FAILED(f_.As(&f5))) return false;
    if (!memLoader_) {
        ComPtr<IDWriteInMemoryFontFileLoader> loader;
        if (FAILED(f5->CreateInMemoryFontFileLoader(&loader)) || FAILED(f5->RegisterFontFileLoader(loader.Get()))) return false;
        memLoader_ = loader;
    }
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    memLoader_.As(&loader);
    // WOFF and WOFF2 are unpacked first (CSS Fonts 4, 4.3)
    ComPtr<IDWriteFontFile> file;
    DWRITE_CONTAINER_TYPE container = f5->AnalyzeContainerType(bytes.data(), (UINT32)bytes.size());
    if (container != DWRITE_CONTAINER_TYPE_UNKNOWN) {
        ComPtr<IDWriteFontFileStream> stream;
        if (FAILED(f5->UnpackFontFile(container, bytes.data(), (UINT32)bytes.size(), &stream))) return false;
        UINT64 size = 0;
        stream->GetFileSize(&size);
        const void* data = nullptr;
        void* ctx = nullptr;
        if (size == 0 || FAILED(stream->ReadFileFragment(&data, 0, size, &ctx))) return false;
        HRESULT hr = loader->CreateInMemoryFontFileReference(f5.Get(), data, (UINT32)size, nullptr, &file);
        stream->ReleaseFileFragment(ctx);
        if (FAILED(hr)) return false;
    } else if (FAILED(loader->CreateInMemoryFontFileReference(f5.Get(), bytes.data(), (UINT32)bytes.size(), nullptr, &file)))
        return false;
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType;
    DWRITE_FONT_FACE_TYPE faceType;
    UINT32 faces = 0;
    if (FAILED(file->Analyze(&supported, &fileType, &faceType, &faces)) || !supported || faces == 0) return false;
    WebFamily& fam = web_[name];
    ComPtr<IDWriteFactory6> f6;
    f_.As(&f6);
    for (UINT32 i = 0; i < faces; ++i) {
        // a variable font with a weight axis stands in for each weight from 100 to 900 that the axis reaches
        ComPtr<IDWriteFontResource> res;
        if (f6 && SUCCEEDED(f6->CreateFontResource(file.Get(), i, &res)) && res->GetFontAxisCount() > 0) {
            UINT32 n = res->GetFontAxisCount();
            std::vector<DWRITE_FONT_AXIS_VALUE> values(n);
            std::vector<DWRITE_FONT_AXIS_RANGE> ranges(n);
            res->GetDefaultFontAxisValues(values.data(), n);
            res->GetFontAxisRanges(ranges.data(), n);
            int wght = -1;
            for (UINT32 k = 0; k < n; ++k)
                if (ranges[k].axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT && ranges[k].maxValue > ranges[k].minValue) wght = (int)k;
            if (wght >= 0) {
                for (int w = 100; w <= 900; w += 100) {
                    if (w < ranges[wght].minValue - 50 || w > ranges[wght].maxValue + 50) continue;
                    values[wght].value = std::clamp((float)w, ranges[wght].minValue, ranges[wght].maxValue);
                    ComPtr<IDWriteFontFaceReference1> ref;
                    if (SUCCEEDED(res->CreateFontFaceReference(DWRITE_FONT_SIMULATIONS_NONE, values.data(), n, &ref)))
                        fam.faces.push_back({ref, w});
                }
                continue;
            }
        }
        ComPtr<IDWriteFontFaceReference> ref;
        if (SUCCEEDED(f5->CreateFontFaceReference(file.Get(), i, DWRITE_FONT_SIMULATIONS_NONE, &ref))) fam.faces.push_back({ref, 0});
    }
    // the family again, with every face it has so far, under the page's name for it
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(f5->CreateFontSetBuilder(&builder))) return false;
    DWRITE_FONT_PROPERTY props[] = {{DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME, name.c_str(), L""},
                                    {DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME, name.c_str(), L""},
                                    {DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, name.c_str(), L""}};
    for (auto& [face, weight] : fam.faces) {
        ComPtr<IDWriteFontFaceReference> ref;
        face.As(&ref);
        std::wstring w = std::to_wstring(weight);
        DWRITE_FONT_PROPERTY withWeight[] = {props[0], props[1], props[2], {DWRITE_FONT_PROPERTY_ID_WEIGHT, w.c_str(), L""}};
        builder->AddFontFaceReference(ref.Get(), withWeight, weight ? 4 : 3);
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> coll;
    if (FAILED(builder->CreateFontSet(&set)) || FAILED(f5->CreateFontCollectionFromFontSet(set.Get(), &coll))) return false;
    fam.collection = coll;
    famExists_.erase(name);
    for (auto it = fmts_.begin(); it != fmts_.end();)  // formats made before the face came
        it = it->first.rfind(name + L"|", 0) == 0 ? fmts_.erase(it) : std::next(it);
    lastStyle_ = nullptr;
    lastEntry_ = nullptr;
    widths_.clear();
    return true;
}

bool TextSystem::hasFamily(const std::wstring& name) {
    if (web_.count(name)) return web_[name].collection != nullptr;
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
    IDWriteFontCollection* coll = collectionFor(fam);
    f_->CreateTextFormat(fam.c_str(), coll, w, st, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &e.fmt);
    float asc = 0.9f, desc = 0.22f;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    coll->FindFamilyName(fam.c_str(), &idx, &exists);
    if (exists) {
        ComPtr<IDWriteFontFamily> ff;
        ComPtr<IDWriteFont> font;
        if (SUCCEEDED(coll->GetFontFamily(idx, &ff)) &&
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
    // letter-spacing comes after every character (CSS Text 3, 8.2); word-spacing after every space (8.1)
    auto spacing = [&]() {
        float extra = 0;
        if (s.letterSpacing != 0)
            for (wchar_t ch : t)
                if ((ch & 0xFC00) != 0xDC00) extra += s.letterSpacing;
        if (s.wordSpacing != 0)
            for (wchar_t ch : t)
                if (ch == L' ' || ch == 0xA0) extra += s.wordSpacing;
        return extra;
    };
    auto it = widths_.find(k);
    if (it != widths_.end()) return it->second + spacing();
    float w = 0;
    ComPtr<IDWriteTextLayout> l;
    if (e.fmt && SUCCEEDED(f_->CreateTextLayout(t.data(), (UINT32)t.size(), e.fmt.Get(), 100000.f, 1000.f, &l))) {
        DWRITE_TEXT_METRICS m;
        l->GetMetrics(&m);
        w = m.widthIncludingTrailingWhitespace;
    }
    if (widths_.size() > 400000) widths_.clear();
    widths_.emplace(std::move(k), w);
    return w + spacing();
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
        ComPtr<IDWriteTextLayout1> l1;
        if ((s.letterSpacing != 0 || s.wordSpacing != 0) && SUCCEEDED(l.As(&l1))) {
            if (s.letterSpacing != 0) l1->SetCharacterSpacing(0, s.letterSpacing, 0, r);
            if (s.wordSpacing != 0)
                for (UINT32 i = 0; i < (UINT32)t.size(); ++i)
                    if (t[i] == L' ' || t[i] == 0xA0) l1->SetCharacterSpacing(0, s.letterSpacing + s.wordSpacing, 0, DWRITE_TEXT_RANGE{i, 1});
        }
    }
    return l;
}

std::wstring TextSystem::resolveFamily(std::string_view list) {
    std::string key(list);
    if (webFamilies_ && !webFamilies_->empty()) key += "\x01" + std::to_string(webVersion_);  // the page's own fonts count
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
        if (webFamilies_)  // the page's @font-face families come before the system's (CSS Fonts 4, 4.1)
            if (auto wf = webFamilies_->find(l); wf != webFamilies_->end() && hasFamily(wf->second)) {
                result = wf->second;
                break;
            }
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
