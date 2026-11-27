#pragma once
#include <unordered_map>

#include "css.h"
#include "dom.h"
#include "layout.h"

#include "script.h"

namespace tb {

struct FetchResult {
    int status = 0;
    std::string body, finalUrl, contentType, error;
    std::string rawHeaders;  // status line and response headers, when FetchOpts::wantHeaders is set
    double ms = 0;
};

struct FetchOpts {
    std::string method = "GET";
    std::string body, contentType;
    std::string referrer;        // URL of the document that initiated the request
    std::string firstPartySite;  // set for subresources: registrable domain of the top-level page
    std::string initiatorSite;   // set for top-level navigations started from page content
    bool topLevel = false;
    std::vector<std::pair<std::string, std::string>> headers;  // extra request headers (script requests)
    bool wantHeaders = false;                                  // fill FetchResult::rawHeaders
    bool sendOrigin = false;                                   // send Origin even for GET (cross-origin script requests)
};

// Synchronous fetch for http(s) (WinHTTP: TLS via SChannel, HTTP/2, gzip/deflate/brotli), file: and data: URLs.
// Redirects and cookies are handled by the browser itself (see CookieJar), not by WinHTTP.
FetchResult fetch(const std::string& url, const FetchOpts& opts = {});
bool decode_image(const std::string& bytes, ImageRes& out);  // WIC: PNG, JPEG, GIF, BMP, ICO, TIFF, WebP
std::string bytes_to_utf8(const std::string& body, const std::string& contentType);
std::string site_of(const std::string& host);  // approximate registrable domain (eTLD+1)

// Browser-owned cookie store. Third-party cookies are blocked, SameSite is enforced on cross-site
// navigations, and persistent cookies are saved DPAPI-encrypted.
class CookieJar {
public:
    static CookieJar& get();
    void load(const std::wstring& path);  // enables persistence
    void save();
    void clear();
    size_t count();
    // Used by the HTTP layer:
    void store(const struct Url& u, std::string_view setCookie, bool fromScript = false);
    std::string header(const struct Url& u, bool crossSiteNav, bool crossSitePost, bool forScript = false);
};

struct NetEntry {
    std::string url, type, error;
    int status = 0;
    size_t bytes = 0;
    double ms = 0;
};

// What the page's own elements ask to be loaded. Both give "" when there is nothing to load.
std::string image_source(const Node* img);        // an <img>'s picture: src, or what stands in for a placeholder; not resolved
std::string stylesheet_href(const Node* link);    // a <link rel=stylesheet> for the screen: its href; not resolved
bool style_for_screen(const Node* styleOrLink);   // the media attribute does not keep it from the screen

// Everything a navigation produces off the UI thread: parsed DOM, parsed stylesheets, decoded images.
struct LoadResult {
    int tabId = 0;
    uint64_t gen = 0;
    std::string url;
    std::unique_ptr<Document> doc;
    std::vector<Sheet> sheets;
    std::vector<std::string> sheetKeys;  // for each sheet: "t:" + the text of its <style>, or "u:" + the URL it was linked from
    std::vector<ImageRes> images;
    std::unordered_map<const Node*, int> imgNode;
    std::vector<NetEntry> net;
    std::vector<ScriptSource> scripts;  // classic scripts in document order, external ones already fetched
    double tFetch = 0, tParse = 0, tSub = 0;
};

// Decides whether a page may run scripts (per-site setting). Null: scripts are allowed everywhere.
extern bool (*script_policy)(const std::string& pageUrl);

std::unique_ptr<LoadResult> load_page(const std::string& url, const std::string* presetHtml, const FetchOpts* nav = nullptr);

}  // namespace tb
