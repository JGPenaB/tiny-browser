#pragma once
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <unordered_map>

#include "common.h"

namespace tb {

struct Style;

// DirectWrite wrapper with format, metric and word-width caches. UI thread only.
class TextSystem {
public:
    bool init();
    IDWriteFactory* factory() const { return f_.Get(); }
    float width(const Style& s, std::wstring_view t);
    void metrics(const Style& s, float& asc, float& desc);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout(const Style& s, std::wstring_view t, bool underline, bool strike);
    std::wstring resolveFamily(std::string_view list);
    bool hasFamily(const std::wstring& name);
    // Web fonts (CSS Fonts 4, 4): a font file's faces join the family `name` (a name of the page's own, which no
    // system font has). False when the file is not a font DirectWrite can read (TrueType, OpenType, WOFF, WOFF2).
    bool addWebFont(const std::wstring& name, const std::string& bytes);
    // The page being styled: its @font-face families that have a face, from the CSS name (in lower case) to the
    // family's name here. `version` changes whenever the map does.
    void setWebFamilies(const std::unordered_map<std::string, std::wstring>* families, uint64_t version) {
        webFamilies_ = families;
        webVersion_ = version;
    }
    size_t cacheSize() const { return widths_.size(); }
    static TextSystem* inst;

private:
    struct Entry {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        float asc = 0, desc = 0;
        std::wstring key;
    };
    Entry& entry(const Style& s);
    Microsoft::WRL::ComPtr<IDWriteFactory> f_;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> sys_;
    struct WebFamily {
        // IDWriteFontFaceReference, and the weight it stands for (0: what the font file says)
        std::vector<std::pair<Microsoft::WRL::ComPtr<IUnknown>, int>> faces;
        Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
    };
    std::unordered_map<std::wstring, WebFamily> web_;
    Microsoft::WRL::ComPtr<IUnknown> memLoader_;  // IDWriteInMemoryFontFileLoader
    const std::unordered_map<std::string, std::wstring>* webFamilies_ = nullptr;
    uint64_t webVersion_ = 0;
    IDWriteFontCollection* collectionFor(const std::wstring& family);
    std::unordered_map<std::wstring, Entry> fmts_;
    std::unordered_map<std::wstring, float> widths_;
    std::unordered_map<std::string, std::wstring> famCache_;
    std::unordered_map<std::wstring, bool> famExists_;
    const Style* lastStyle_ = nullptr;
    Entry* lastEntry_ = nullptr;
};

}  // namespace tb
