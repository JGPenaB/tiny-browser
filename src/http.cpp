// Networking: WinHTTP transport, browser-owned redirects and cookie jar, image decoding, page loading.
#include <windows.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include "html.h"
#include "net.h"
#include "storage.h"
#include "url.h"

using Microsoft::WRL::ComPtr;

namespace tb {

static constexpr auto npos = std::string_view::npos;

// ============================================================================ sites and cookies

std::string site_of(const std::string& host) {
    if (host.empty() || host[0] == '[') return host;
    bool ip = true;
    for (char c : host)
        if (!isdigit_(c) && c != '.') ip = false;
    if (ip) return host;
    std::vector<std::string_view> labels;
    size_t i = 0;
    std::string_view h = host;
    while (i <= h.size()) {
        size_t j = h.find('.', i);
        if (j == npos) j = h.size();
        labels.push_back(h.substr(i, j - i));
        i = j + 1;
    }
    size_t n = labels.size();
    if (n <= 2) return host;
    static const char* second[] = {"co", "com", "org", "net", "gov", "ac", "edu", "ne", "or", "go", "gob", "nom", "mil"};
    bool two = false;
    if (labels[n - 1].size() == 2)
        for (auto s : second)
            if (labels[n - 2] == s) two = true;
    size_t take = two ? 3 : 2;
    if (n < take) return host;
    std::string r;
    for (size_t k = n - take; k < n; ++k) {
        if (!r.empty()) r += '.';
        r += labels[k];
    }
    return r;
}

namespace {

struct Cookie {
    std::string name, value, domain, path;
    int64_t expires = 0;  // unix seconds; 0 = session cookie
    bool secure = false, httpOnly = false, hostOnly = true;
    uint8_t sameSite = 1;  // 0 None, 1 Lax, 2 Strict
    int64_t created = 0;
};

std::mutex g_cmu;
std::vector<Cookie> g_cookies;
std::wstring g_cpath;
bool g_cdirty = false;
const char* kCookieTag = "TinyBrowser.cookies.v1";

bool is_public_suffix(const std::string& d) {
    if (d.find('.') == std::string::npos) return d != "localhost";
    return site_of("x." + d) != d;
}

int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + (int64_t)doe - 719468;
}

int64_t parse_cookie_date(std::string_view s) {
    int day = -1, mon = -1, year = -1, hh = -1, mm = 0, ss = 0;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && !isalnum_(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && (isalnum_(s[j]) || s[j] == ':')) ++j;
        std::string t(s.substr(i, j - i));
        i = j;
        if (t.empty()) continue;
        if (t.find(':') != std::string::npos) {
            sscanf(t.c_str(), "%d:%d:%d", &hh, &mm, &ss);
        } else if (isdigit_(t[0])) {
            int v = atoi(t.c_str());
            if (t.size() <= 2 && day < 0) day = v;
            else if (year < 0) {
                year = v;
                if (t.size() <= 2) year += v < 70 ? 2000 : 1900;
            }
        } else if (t.size() >= 3 && mon < 0) {
            static const char* months = "janfebmaraprmayjunjulaugsepoctnovdec";
            std::string m3 = lower(t.substr(0, 3));
            const char* p = strstr(months, m3.c_str());
            if (p && (p - months) % 3 == 0) mon = (int)(p - months) / 3 + 1;
        }
    }
    if (day < 1 || mon < 1 || year < 1601 || hh < 0) return -1;
    return days_from_civil(year, (unsigned)mon, (unsigned)day) * 86400 + hh * 3600 + mm * 60 + ss;
}

bool has_ctl(std::string_view s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

bool domain_match(const Cookie& c, const std::string& host) {
    if (c.hostOnly) return host == c.domain;
    return host == c.domain ||
           (host.size() > c.domain.size() && host.compare(host.size() - c.domain.size(), c.domain.size(), c.domain) == 0 &&
            host[host.size() - c.domain.size() - 1] == '.');
}

bool path_match(const Cookie& c, const std::string& p) {
    if (c.path == "/" || p == c.path) return true;
    if (p.size() > c.path.size() && p.compare(0, c.path.size(), c.path) == 0) return c.path.back() == '/' || p[c.path.size()] == '/';
    return false;
}

}  // namespace

CookieJar& CookieJar::get() {
    static CookieJar j;
    return j;
}

void CookieJar::store(const Url& u, std::string_view h, bool fromScript) {
    if (h.size() > 4096 || has_ctl(h)) return;
    Cookie c;
    int64_t now = (int64_t)time(nullptr);
    c.created = now;
    bool hasMaxAge = false, first = true;
    size_t i = 0;
    while (i <= h.size()) {
        size_t j = h.find(';', i);
        if (j == npos) j = h.size();
        std::string_view part = trim(h.substr(i, j - i));
        i = j + 1;
        size_t eq = part.find('=');
        std::string_view k = trim(part.substr(0, eq)), v = eq == npos ? std::string_view{} : trim(part.substr(eq + 1));
        if (first) {
            first = false;
            if (eq == npos || k.empty()) return;
            c.name = std::string(k);
            c.value = std::string(v);
            continue;
        }
        std::string key = lower(k);
        if (key == "domain") {
            std::string d = lower(v);
            if (!d.empty() && d[0] == '.') d.erase(0, 1);
            if (!d.empty()) {
                c.domain = d;
                c.hostOnly = false;
            }
        } else if (key == "path") {
            if (!v.empty() && v[0] == '/') c.path = std::string(v);
        } else if (key == "expires") {
            if (!hasMaxAge) {
                int64_t t = parse_cookie_date(v);
                if (t >= 0) c.expires = std::max<int64_t>(t, 1);
            }
        } else if (key == "max-age") {
            hasMaxAge = true;
            long long s = atoll(std::string(v).c_str());
            c.expires = s <= 0 ? 1 : now + s;
        } else if (key == "secure") {
            c.secure = true;
        } else if (key == "httponly") {
            c.httpOnly = true;
        } else if (key == "samesite") {
            std::string sv = lower(v);
            c.sameSite = sv == "none" ? 0 : sv == "strict" ? 2 : 1;
        }
    }
    if (c.domain.empty()) {
        c.domain = u.host;
    } else {
        Cookie probe = c;
        if (!domain_match(probe, u.host)) return;
        if (is_public_suffix(c.domain)) {
            if (u.host != c.domain) return;
            c.hostOnly = true;
        }
    }
    if (c.path.empty()) {
        size_t sl = u.path.rfind('/');
        c.path = (sl == std::string::npos || sl == 0) ? "/" : u.path.substr(0, sl);
    }
    if (c.secure && u.scheme != "https") return;
    if (c.name.rfind("__Secure-", 0) == 0 && !c.secure) return;
    if (c.name.rfind("__Host-", 0) == 0 && (!c.secure || !c.hostOnly || c.path != "/")) return;
    if (c.sameSite == 0 && !c.secure) c.sameSite = 1;
    if (fromScript && c.httpOnly) return;  // scripts can neither create nor replace HttpOnly cookies

    std::lock_guard<std::mutex> lk(g_cmu);
    if (fromScript)
        for (auto& o : g_cookies)
            if (o.httpOnly && o.name == c.name && o.domain == c.domain && o.path == c.path) return;
    for (size_t k = 0; k < g_cookies.size();) {
        Cookie& o = g_cookies[k];
        if (o.name == c.name && o.domain == c.domain && o.path == c.path) {
            if (o.expires) g_cdirty = true;
            c.created = o.created;
            g_cookies.erase(g_cookies.begin() + k);
        } else {
            ++k;
        }
    }
    if (c.expires != 0 && c.expires <= now) return;  // deletion
    if (c.expires) g_cdirty = true;
    g_cookies.push_back(std::move(c));
    if (g_cookies.size() > 3000) g_cookies.erase(g_cookies.begin(), g_cookies.begin() + 300);
}

std::string CookieJar::header(const Url& u, bool crossSiteNav, bool crossSitePost, bool forScript) {
    std::lock_guard<std::mutex> lk(g_cmu);
    int64_t now = (int64_t)time(nullptr);
    std::vector<const Cookie*> m;
    for (auto& c : g_cookies) {
        if (c.expires && c.expires <= now) continue;
        if (!domain_match(c, u.host) || !path_match(c, u.path)) continue;
        if (c.secure && u.scheme != "https") continue;
        if (forScript && c.httpOnly) continue;
        if (crossSiteNav && c.sameSite == 2) continue;
        if (crossSitePost && c.sameSite != 0) continue;
        m.push_back(&c);
    }
    std::stable_sort(m.begin(), m.end(), [](const Cookie* a, const Cookie* b) {
        return a->path.size() != b->path.size() ? a->path.size() > b->path.size() : a->created < b->created;
    });
    std::string r;
    for (auto* c : m) {
        if (!r.empty()) r += "; ";
        r += c->name + "=" + c->value;
    }
    return r;
}

void CookieJar::load(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_cmu);
    g_cpath = path;
    g_cookies.clear();
    std::string blob, plain;
    if (!read_file(path, blob) || blob.empty() || !dpapi_unprotect(blob, plain, kCookieTag)) return;
    int64_t now = (int64_t)time(nullptr);
    size_t i = 0;
    while (i < plain.size()) {
        size_t e = plain.find('\n', i);
        if (e == std::string::npos) e = plain.size();
        std::string_view line(plain.data() + i, e - i);
        i = e + 1;
        std::vector<std::string> f;
        size_t p = 0;
        while (p <= line.size()) {
            size_t t = line.find('\t', p);
            if (t == npos) t = line.size();
            f.emplace_back(line.substr(p, t - p));
            p = t + 1;
        }
        if (f.size() < 8) continue;
        Cookie c;
        c.name = f[0];
        c.value = f[1];
        c.domain = f[2];
        c.path = f[3];
        c.expires = atoll(f[4].c_str());
        int flags = atoi(f[5].c_str());
        c.secure = flags & 1;
        c.httpOnly = (flags & 2) != 0;
        c.hostOnly = (flags & 4) != 0;
        c.sameSite = (uint8_t)atoi(f[6].c_str());
        c.created = atoll(f[7].c_str());
        if (c.expires > now) g_cookies.push_back(std::move(c));
    }
    secure_wipe(plain);
}

void CookieJar::save() {
    std::lock_guard<std::mutex> lk(g_cmu);
    if (g_cpath.empty() || !g_cdirty) return;
    int64_t now = (int64_t)time(nullptr);
    std::string plain;
    for (auto& c : g_cookies) {
        if (!c.expires || c.expires <= now) continue;
        int flags = (c.secure ? 1 : 0) | (c.httpOnly ? 2 : 0) | (c.hostOnly ? 4 : 0);
        plain += c.name + "\t" + c.value + "\t" + c.domain + "\t" + c.path + "\t" + std::to_string(c.expires) + "\t" +
                 std::to_string(flags) + "\t" + std::to_string(c.sameSite) + "\t" + std::to_string(c.created) + "\n";
    }
    std::string blob;
    if (dpapi_protect(plain, blob, kCookieTag) && write_file_atomic(g_cpath, blob.data(), blob.size())) g_cdirty = false;
    secure_wipe(plain);
}

void CookieJar::clear() {
    std::lock_guard<std::mutex> lk(g_cmu);
    g_cookies.clear();
    g_cdirty = true;
}

size_t CookieJar::count() {
    std::lock_guard<std::mutex> lk(g_cmu);
    return g_cookies.size();
}

// ============================================================================ transport

namespace {

const wchar_t* kUserAgent = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) TinyBrowser/0.2";

HINTERNET session() {
    static HINTERNET s = [] {
        HINTERNET h = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h)
            h = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) {
            DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(h, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof dec);
            DWORD proto = WINHTTP_PROTOCOL_FLAG_HTTP2;
            WinHttpSetOption(h, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &proto, sizeof proto);
            WinHttpSetTimeouts(h, 10000, 10000, 15000, 30000);
        }
        return h;
    }();
    return s;
}

std::string net_error(DWORD e) {
    switch (e) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "Server not found (DNS lookup failed)";
        case ERROR_WINHTTP_CANNOT_CONNECT: return "Could not connect to the server";
        case ERROR_WINHTTP_TIMEOUT: return "The connection timed out";
        case ERROR_WINHTTP_SECURE_FAILURE: return "Secure connection failed (TLS/certificate error)";
        case ERROR_WINHTTP_CONNECTION_ERROR: return "The connection was reset";
        case ERROR_WINHTTP_INVALID_URL: return "Invalid URL";
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "Unsupported URL scheme";
    }
    return "Network error " + std::to_string(e);
}

std::string content_type_for(const std::string& path) {
    std::string l = lower(path);
    auto ends = [&](const char* e) {
        size_t n = strlen(e);
        return l.size() >= n && l.compare(l.size() - n, n, e) == 0;
    };
    if (ends(".html") || ends(".htm") || ends(".xhtml")) return "text/html";
    if (ends(".css")) return "text/css";
    if (ends(".js") || ends(".mjs")) return "text/javascript";
    if (ends(".json")) return "application/json";
    if (ends(".png")) return "image/png";
    if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
    if (ends(".gif")) return "image/gif";
    if (ends(".bmp")) return "image/bmp";
    if (ends(".webp")) return "image/webp";
    if (ends(".ico")) return "image/x-icon";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".txt") || ends(".md") || ends(".log") || ends(".csv") || ends(".ini") || ends(".cpp") || ends(".h") ||
        ends(".c") || ends(".py") || ends(".xml") || ends(".ps1") || ends(".bat") || ends(".cmake"))
        return "text/plain";
    return "application/octet-stream";
}

FetchResult fetch_file(const Url& u, const std::string& raw) {
    FetchResult r;
    r.finalUrl = raw;
    std::string p = percent_decode(u.path);
    if (p.size() >= 3 && p[0] == '/' && p[2] == ':') p.erase(0, 1);
    for (auto& ch : p)
        if (ch == '/') ch = '\\';
    if (!u.host.empty()) p = "\\\\" + u.host + p;
    std::wstring wp = widen(p);
    DWORD attrs = GetFileAttributesW(wp.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        std::string html = "<html><head><title>Index of " + escape_html(p) +
                           "</title><style>body{font-family:Segoe UI,sans-serif;margin:24px}a{text-decoration:none}"
                           "li{margin:3px 0}</style></head><body><h1 style=\"font-size:20px\">Index of " +
                           escape_html(p) + "</h1><ul><li><a href=\"../\">..</a></li>";
        WIN32_FIND_DATAW fd;
        std::wstring pat = wp;
        if (!pat.empty() && pat.back() != L'\\') pat += L'\\';
        HANDLE h = FindFirstFileW((pat + L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::string name = narrow(fd.cFileName);
                if (name == "." || name == "..") continue;
                bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                html += "<li><a href=\"" + escape_html(percent_encode_path(name)) + (dir ? "/" : "") + "\">" +
                        escape_html(name) + (dir ? "/" : "") + "</a></li>";
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        html += "</ul></body></html>";
        r.status = 200;
        r.body = html;
        r.contentType = "text/html";
        if (!r.finalUrl.empty() && r.finalUrl.back() != '/') r.finalUrl += '/';
        return r;
    }
    if (!read_file(wp, r.body)) {
        r.error = "File not found: " + p;
        return r;
    }
    r.status = 200;
    r.contentType = content_type_for(p);
    return r;
}

std::string base64_decode(std::string_view s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v = val(c);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((acc >> bits) & 0xFF);
        }
    }
    return out;
}

FetchResult fetch_data(const std::string& url) {
    FetchResult r;
    r.finalUrl = url;
    std::string_view s = std::string_view(url).substr(5);
    size_t comma = s.find(',');
    if (comma == npos) {
        r.error = "Malformed data: URL";
        return r;
    }
    std::string meta(s.substr(0, comma));
    std::string_view payload = s.substr(comma + 1);
    bool b64 = false;
    std::string lm = lower(meta);
    if (lm.size() >= 7 && lm.compare(lm.size() - 7, 7, ";base64") == 0) {
        b64 = true;
        meta = meta.substr(0, meta.size() - 7);
    }
    r.contentType = meta.empty() ? "text/plain" : meta;
    r.body = b64 ? base64_decode(percent_decode(payload)) : percent_decode(payload);
    r.status = 200;
    return r;
}

std::wstring query_header(HINTERNET rq, DWORD which) {
    DWORD len = 0;
    WinHttpQueryHeaders(rq, which, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return {};
    std::wstring buf(len / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(rq, which, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &len, WINHTTP_NO_HEADER_INDEX)) return {};
    buf.resize(len / sizeof(wchar_t));
    return buf;
}

std::string clean_header(std::string_view v) {
    std::string r;
    for (char c : v)
        if (c != '\r' && c != '\n') r += c;
    return r;
}

// Referrer policy: strict-origin-when-cross-origin.
std::string referer_for(const std::string& ref, const Url& target) {
    if (ref.empty()) return {};
    Url r = Url::parse(ref);
    if (!r.valid || (r.scheme != "http" && r.scheme != "https")) return {};
    if (r.scheme == "https" && target.scheme == "http") return {};
    bool same = r.scheme == target.scheme && r.hostport() == target.hostport();
    return same ? r.str(false) : r.origin() + "/";
}

struct Hop {
    int status = 0;
    std::string body, contentType, location, error, rawHeaders;
};

Hop http_once(const Url& u, const std::string& method, const std::string& body, const FetchOpts& o) {
    Hop r;
    bool https = u.scheme == "https";
    INTERNET_PORT port = u.port.empty() ? (https ? 443 : 80) : (INTERNET_PORT)atoi(u.port.c_str());
    HINTERNET s = session();
    if (!s) {
        r.error = "WinHTTP is unavailable";
        return r;
    }
    HINTERNET c = WinHttpConnect(s, widen(u.host).c_str(), port, 0);
    if (!c) {
        r.error = net_error(GetLastError());
        return r;
    }
    std::string pq = percent_encode_path(u.path + (u.hasQuery ? "?" + u.query : ""));
    HINTERNET rq = WinHttpOpenRequest(c, widen(method).c_str(), widen(pq).c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!rq) {
        r.error = net_error(GetLastError());
        WinHttpCloseHandle(c);
        return r;
    }
    DWORD dis = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(rq, WINHTTP_OPTION_DISABLE_FEATURE, &dis, sizeof dis);

    bool thirdParty = !o.firstPartySite.empty() && site_of(u.host) != o.firstPartySite;
    bool crossNav = o.topLevel && !o.initiatorSite.empty() && site_of(u.host) != o.initiatorSite;
    std::string hdrs = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\n"
                       "Accept-Language: en-US,en;q=0.9\r\n";
    if (!thirdParty) {
        std::string ck = CookieJar::get().header(u, crossNav, crossNav && method != "GET");
        if (!ck.empty()) hdrs += "Cookie: " + clean_header(ck) + "\r\n";
    }
    std::string ref = referer_for(o.referrer, u);
    if (!ref.empty()) hdrs += "Referer: " + clean_header(ref) + "\r\n";
    if (method != "GET" || o.sendOrigin) {
        Url ro = Url::parse(o.referrer);
        hdrs += "Origin: " + (ro.valid && ro.hierarchical ? clean_header(ro.origin()) : std::string("null")) + "\r\n";
        if (!o.contentType.empty()) hdrs += "Content-Type: " + clean_header(o.contentType) + "\r\n";
    }
    for (auto& extra : o.headers) hdrs += clean_header(extra.first) + ": " + clean_header(extra.second) + "\r\n";
    std::wstring wh = widen(hdrs);
    BOOL ok = WinHttpSendRequest(rq, wh.c_str(), (DWORD)wh.size(), body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                 (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(rq, nullptr);
    if (!ok) {
        r.error = net_error(GetLastError());
    } else {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &len, WINHTTP_NO_HEADER_INDEX);
        r.status = (int)status;
        r.contentType = narrow(query_header(rq, WINHTTP_QUERY_CONTENT_TYPE));
        r.location = narrow(query_header(rq, WINHTTP_QUERY_LOCATION));
        if (!thirdParty) {
            for (DWORD idx = 0; idx < 64;) {
                DWORD l2 = 0, i2 = idx;
                WinHttpQueryHeaders(rq, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &l2, &i2);
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || l2 == 0) break;
                std::wstring buf(l2 / sizeof(wchar_t), L'\0');
                i2 = idx;
                if (!WinHttpQueryHeaders(rq, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &l2, &i2)) break;
                buf.resize(l2 / sizeof(wchar_t));
                CookieJar::get().store(u, narrow(buf));
                idx = i2 > idx ? i2 : idx + 1;
            }
        }
        if (o.wantHeaders) r.rawHeaders = narrow(query_header(rq, WINHTTP_QUERY_RAW_HEADERS_CRLF));
        bool redirect = (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && !r.location.empty();
        if (!redirect) {
            std::vector<char> buf(1 << 16);
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(rq, buf.data(), (DWORD)buf.size(), &got)) {
                    if (r.body.empty()) r.error = net_error(GetLastError());
                    break;
                }
                if (got == 0) break;
                r.body.append(buf.data(), got);
                if (r.body.size() > 64u * 1024 * 1024) break;
            }
        }
    }
    WinHttpCloseHandle(rq);
    WinHttpCloseHandle(c);
    return r;
}

}  // namespace

FetchResult fetch(const std::string& url, const FetchOpts& o) {
    double t0 = now_ms();
    FetchResult r;
    r.finalUrl = url;
    if (istarts(url, "data:")) {
        r = fetch_data(url);
        r.ms = now_ms() - t0;
        return r;
    }
    std::string cur = url, method = o.method.empty() ? "GET" : o.method, body = o.body;
    Url first = Url::parse(url);
    bool done = false;
    for (int hop = 0; hop < 20 && !done; ++hop) {
        Url u = Url::parse(cur);
        if (!u.valid) {
            r.error = "Invalid URL";
            break;
        }
        if (u.scheme == "file") {
            if (hop > 0) {
                r.error = "Blocked redirect to a local file";
                break;
            }
            r = fetch_file(u, cur);
            r.ms = now_ms() - t0;
            return r;
        }
        if (u.scheme != "http" && u.scheme != "https") {
            r.error = "Unsupported URL scheme: " + u.scheme;
            break;
        }
        Hop h = http_once(u, method, body, o);
        if (!h.error.empty() && method == "GET" &&
            (h.error == net_error(ERROR_WINHTTP_CONNECTION_ERROR) || h.error == net_error(ERROR_WINHTTP_SECURE_FAILURE)))
            h = http_once(u, method, body, o);  // one retry for transient resets
        r.finalUrl = cur;
        if (!h.error.empty()) {
            r.error = h.error;
            break;
        }
        bool redirect = (h.status == 301 || h.status == 302 || h.status == 303 || h.status == 307 || h.status == 308) && !h.location.empty();
        if (redirect) {
            if (h.status == 303 || ((h.status == 301 || h.status == 302) && method != "GET")) {
                method = "GET";
                body.clear();
            }
            cur = resolve_url(cur, h.location);
            if (hop == 19) r.error = "Too many redirects";
            continue;
        }
        r.status = h.status;
        r.body = std::move(h.body);
        r.contentType = std::move(h.contentType);
        r.rawHeaders = std::move(h.rawHeaders);
        done = true;
    }
    if (first.valid && first.hasFragment && r.finalUrl.find('#') == std::string::npos) r.finalUrl += "#" + first.fragment;
    r.ms = now_ms() - t0;
    return r;
}

bool decode_image(const std::string& bytes, ImageRes& out) {
    if (bytes.empty()) return false;
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> st;
    if (FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size()))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return false;
    ComPtr<IWICBitmapSource> src = frame;
    const UINT kMax = 4096;  // downscale very large images to keep GPU memory in check
    if (w > kMax || h > kMax) {
        float sc = std::min(kMax / (float)w, kMax / (float)h);
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), UINT(w * sc), UINT(h * sc), WICBitmapInterpolationModeFant)))
            src = scaler;
    }
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(f->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeMedianCut)))
        return false;
    UINT cw = 0, ch = 0;
    cv->GetSize(&cw, &ch);
    out.px.resize((size_t)cw * ch * 4);
    if (FAILED(cv->CopyPixels(nullptr, cw * 4, (UINT)out.px.size(), out.px.data()))) {
        out.px.clear();
        return false;
    }
    out.w = (int)cw;
    out.h = (int)ch;
    return true;
}

static UINT codepage_for(const std::string& label) {
    std::string l = lower(label);
    struct M {
        const char* n;
        UINT cp;
    };
    static const M map[] = {{"utf-8", CP_UTF8},     {"utf8", CP_UTF8},        {"iso-8859-1", 1252},     {"latin1", 1252},
                            {"windows-1252", 1252}, {"us-ascii", 1252},       {"ascii", 1252},          {"shift_jis", 932},
                            {"shift-jis", 932},     {"sjis", 932},            {"euc-jp", 20932},        {"gbk", 936},
                            {"gb2312", 936},        {"gb18030", 54936},       {"big5", 950},            {"euc-kr", 949},
                            {"koi8-r", 20866},      {"koi8-u", 21866},        {"windows-1251", 1251},   {"windows-1250", 1250},
                            {"windows-1253", 1253}, {"windows-1254", 1254},   {"windows-1255", 1255},   {"windows-1256", 1256},
                            {"iso-8859-2", 28592},  {"iso-8859-5", 28595},    {"iso-8859-7", 28597},    {"iso-8859-9", 28599},
                            {"iso-8859-15", 28605}};
    for (auto& m : map)
        if (l == m.n) return m.cp;
    return 0;
}

static std::string find_charset(std::string_view s) {
    size_t p = s.find("charset");
    if (p == npos) return {};
    p += 7;
    while (p < s.size() && (isws(s[p]) || s[p] == '=' || s[p] == '"' || s[p] == '\'')) ++p;
    size_t e = p;
    while (e < s.size() && (isalnum_(s[e]) || s[e] == '-' || s[e] == '_')) ++e;
    return std::string(s.substr(p, e - p));
}

std::string bytes_to_utf8(const std::string& body, const std::string& contentType) {
    const std::string& s = body;
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) return s.substr(3);
    if (s.size() >= 2 && (uint8_t)s[0] == 0xFF && (uint8_t)s[1] == 0xFE) {
        std::wstring w((const wchar_t*)(s.data() + 2), (s.size() - 2) / 2);
        return narrow(w);
    }
    UINT cp = codepage_for(find_charset(lower(contentType)));
    if (!cp) cp = codepage_for(find_charset(lower(std::string_view(s).substr(0, std::min<size_t>(s.size(), 2048)))));
    if (!cp || cp == CP_UTF8) {
        if (s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0) return s;
        cp = 1252;  // invalid UTF-8: fall back to Windows-1252 like browsers do
    }
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return s;
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), w.data(), n);
    return narrow(w);
}

static std::string message_page(const std::string& title, const std::string& msg, const std::string& url) {
    return "<!doctype html><html><head><title>" + escape_html(title) +
           "</title><style>body{font-family:Segoe UI,sans-serif;background:#f6f7f9;color:#1f2328;margin:0}"
           "main{max-width:640px;margin:80px auto;padding:0 24px}h1{font-size:26px;font-weight:600}"
           "p{color:#4b535c;font-size:15px}code{background:#eceff2;padding:2px 6px}</style></head><body><main><h1>" +
           escape_html(title) + "</h1><p>" + escape_html(msg) + "</p><p><code>" + escape_html(url) +
           "</code></p></main></body></html>";
}

template <class F>
static void parallel_for(size_t n, size_t threads, F&& fn) {
    if (n == 0) return;
    std::atomic<size_t> next{0};
    std::vector<std::thread> ts;
    size_t tc = std::min(threads, n);
    for (size_t t = 0; t < tc; ++t)
        ts.emplace_back([&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;) {
                size_t i = next++;
                if (i >= n) break;
                fn(i);
            }
            CoUninitialize();
        });
    for (auto& t : ts) t.join();
}

bool (*script_policy)(const std::string& pageUrl) = nullptr;

std::unique_ptr<LoadResult> load_page(const std::string& url, const std::string* preset, const FetchOpts* nav) {
    auto R = std::make_unique<LoadResult>();
    R->url = url;
    std::string html;
    double t0 = now_ms();
    if (preset) {
        html = *preset;
    } else {
        FetchOpts o = nav ? *nav : FetchOpts{};
        o.topLevel = true;
        o.firstPartySite.clear();
        FetchResult f = fetch(url, o);
        R->tFetch = now_ms() - t0;
        R->net.push_back({url, f.contentType, f.error, f.status, f.body.size(), f.ms});
        if (!f.error.empty()) {
            html = message_page("This page can't be reached", f.error, url);
        } else {
            if (!f.finalUrl.empty()) R->url = f.finalUrl;
            std::string ct = lower(f.contentType);
            if (istarts(ct, "image/") && !contains(ct, "svg")) {
                html = "<!doctype html><html><head><title>" + escape_html(R->url) +
                       "</title></head><body style=\"margin:0;background:#202124;text-align:center\"><img src=\"" +
                       escape_html(R->url) + "\"></body></html>";
            } else if (contains(ct, "html") || contains(ct, "xml") || ct.empty()) {
                html = bytes_to_utf8(f.body, f.contentType);
            } else if (istarts(ct, "text/") || contains(ct, "json") || contains(ct, "javascript")) {
                html = "<!doctype html><html><body style=\"margin:8px\"><pre style=\"white-space:pre-wrap\">" +
                       escape_html(bytes_to_utf8(f.body, f.contentType)) + "</pre></body></html>";
            } else {
                html = message_page("Can't display this file",
                                    "The server sent " + f.contentType + " (" + std::to_string(f.body.size()) +
                                        " bytes). Downloads are planned for a later milestone.",
                                    R->url);
            }
        }
    }
    double t1 = now_ms();
    R->doc = parse_html(html, R->url);
    R->tParse = now_ms() - t1;

    // Collect subresources in document order.
    struct Job {
        bool isInline;
        std::string text;  // inline css or URL
    };
    std::vector<Job> cssJobs;
    struct ScriptJob {
        Node* node;
        bool isInline;
        std::string text;  // code or URL
        ScriptSource::Kind kind = ScriptSource::Classic;
        bool noFetch = false;  // a module with a src: the module loader fetches it, with its own checks
    };
    std::vector<ScriptJob> scriptJobs;
    bool wantScripts = !script_policy || script_policy(R->url);
    std::vector<std::pair<const Node*, std::string>> imgRefs;
    const std::string& base = R->doc->baseUrl;
    std::vector<Node*> stack{R->doc->root.get()};
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        if (n->type == Node::T::Element) {
            if (n->name == "style") {
                auto media = n->attr("media");
                if (!media || !contains(lower(*media), "print") || contains(lower(*media), "screen"))
                    cssJobs.push_back({true, text_content(n)});
            } else if (n->name == "link") {
                auto rel = n->attr("rel");
                auto href = n->attr("href");
                auto media = n->attr("media");
                if (rel && href && contains(lower(*rel), "stylesheet") && !contains(lower(*rel), "alternate") &&
                    (!media || !contains(lower(*media), "print") || contains(lower(*media), "screen")))
                    cssJobs.push_back({false, resolve_url(base, *href)});
            } else if (n->name == "img" && imgRefs.size() < 300) {
                const std::string* src = n->attr("src");
                std::string s = src ? *src : "";
                bool placeholder = s.empty() || (istarts(s, "data:image/gif") && s.size() < 200) ||
                                   (istarts(s, "data:image/svg") && s.size() < 300);
                if (placeholder)
                    if (auto ds = n->attr("data-src")) s = *ds;
                if (s.empty())
                    if (auto ss = n->attr("srcset")) {
                        std::string_view v = trim(*ss);
                        size_t e = 0;
                        while (e < v.size() && !isws(v[e]) && v[e] != ',') ++e;
                        s = std::string(v.substr(0, e));
                    }
                if (!s.empty()) imgRefs.push_back({n, resolve_url(base, s)});
            } else if (n->name == "script" && wantScripts && scriptJobs.size() < 150) {
                const std::string* type = n->attr("type");
                std::string ty = type ? lower(std::string(trim(*type))) : "";
                bool classic = ty.empty() || ty == "text/javascript" || ty == "application/javascript" || ty == "application/x-javascript" ||
                               ty == "text/ecmascript" || ty == "application/ecmascript" || ty == "text/jscript" || ty == "javascript";
                if (classic && !n->attr("nomodule")) {  // nomodule marks a fallback for browsers without modules
                    const std::string* src = n->attr("src");
                    if (src && !src->empty()) {
                        scriptJobs.push_back({n, false, resolve_url(base, *src)});
                    } else if (!src) {
                        std::string code = text_content(n);
                        if (!trim(code).empty()) scriptJobs.push_back({n, true, std::move(code)});
                    }
                } else if (ty == "module") {
                    const std::string* src = n->attr("src");
                    if (src && !src->empty()) {
                        scriptJobs.push_back({n, false, resolve_url(base, *src), ScriptSource::Module, true});
                    } else if (!src) {
                        std::string code = text_content(n);
                        if (!trim(code).empty()) scriptJobs.push_back({n, true, std::move(code), ScriptSource::Module});
                    }
                } else if (ty == "importmap" && !n->attr("src")) {
                    scriptJobs.push_back({n, true, text_content(n), ScriptSource::ImportMap});
                }
            }
        }
        for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) stack.push_back(it->get());
    }

    std::vector<std::string> imgUrls;
    std::unordered_map<std::string, int> imgIndex;
    for (auto& [node, u] : imgRefs)
        if (!imgIndex.count(u) && imgUrls.size() < 160) {
            imgIndex[u] = (int)imgUrls.size();
            imgUrls.push_back(u);
        }

    double t2 = now_ms();
    FetchOpts sub;
    sub.referrer = R->url;
    {
        Url du = Url::parse(R->url);
        sub.firstPartySite = du.valid && !du.host.empty() ? site_of(du.host) : std::string("local");
    }
    size_t nCss = cssJobs.size(), nImg = imgUrls.size(), nScript = scriptJobs.size();
    std::vector<std::string> scriptCode(nScript);
    std::vector<NetEntry> scriptNet(nScript);
    std::vector<char> scriptOk(nScript, 0);
    std::vector<Sheet> sheets(nCss);
    std::vector<NetEntry> cssNet(nCss);
    std::vector<ImageRes> images(nImg);
    std::vector<NetEntry> imgNet(nImg);
    parallel_for(nCss + nImg + nScript, 8, [&](size_t i) {
        if (i >= nCss + nImg) {
            size_t k = i - nCss - nImg;
            ScriptJob& j = scriptJobs[k];
            if (j.noFetch) {
                scriptOk[k] = 1;
                return;
            }
            if (j.isInline) {
                scriptCode[k] = std::move(j.text);
                scriptOk[k] = 1;
                return;
            }
            FetchResult f = fetch(j.text, sub);
            scriptNet[k] = {j.text, f.contentType.empty() ? "text/javascript" : f.contentType, f.error, f.status, f.body.size(), f.ms};
            if (f.error.empty() && f.status < 400 && f.body.size() < 12u * 1024 * 1024) {
                scriptCode[k] = bytes_to_utf8(f.body, f.contentType);
                scriptOk[k] = 1;
            }
        } else if (i < nCss) {
            Job& j = cssJobs[i];
            if (j.isInline) {
                sheets[i] = parse_sheet(j.text);
                return;
            }
            FetchResult f = fetch(j.text, sub);
            cssNet[i] = {j.text, f.contentType.empty() ? "text/css" : f.contentType, f.error, f.status, f.body.size(), f.ms};
            if (f.error.empty() && f.status < 400) sheets[i] = parse_sheet(bytes_to_utf8(f.body, f.contentType));
        } else {
            size_t k = i - nCss;
            FetchResult f = fetch(imgUrls[k], sub);
            imgNet[k] = {imgUrls[k], f.contentType, f.error, f.status, f.body.size(), f.ms};
            images[k].url = imgUrls[k];
            if (f.error.empty() && f.status < 400) {
                if (!decode_image(f.body, images[k]) && imgNet[k].error.empty()) imgNet[k].error = "decode failed";
            }
        }
    });
    for (size_t i = 0; i < nCss; ++i) {
        if (!cssJobs[i].isInline) R->net.push_back(cssNet[i]);
        R->sheets.push_back(std::move(sheets[i]));
    }
    for (auto& e : imgNet) R->net.push_back(e);
    for (size_t k = 0; k < nScript; ++k) {
        ScriptJob& j = scriptJobs[k];
        if (!j.isInline && !j.noFetch) R->net.push_back(scriptNet[k]);
        if (!scriptOk[k]) continue;
        ScriptSource src{j.node, std::move(scriptCode[k]), j.isInline ? R->url : j.text, j.kind};
        src.external = !j.isInline;
        src.async = j.node->attr("async") != nullptr;
        src.deferred = j.kind == ScriptSource::Module || (j.kind == ScriptSource::Classic && src.external && !src.async && j.node->attr("defer"));
        R->scripts.push_back(std::move(src));
    }
    R->images = std::move(images);
    for (auto& [node, u] : imgRefs) {
        auto it = imgIndex.find(u);
        if (it != imgIndex.end()) R->imgNode[node] = it->second;
    }
    R->tSub = now_ms() - t2;
    return R;
}

}  // namespace tb
