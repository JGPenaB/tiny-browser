#include "storage.h"

#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>

#include <cstring>

namespace tb {

std::wstring app_data_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".");
    dir += L"\\TinyBrowser";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool dpapi_protect(const std::string& plain, std::string& out, const char* tag) {
    DATA_BLOB in{(DWORD)plain.size(), (BYTE*)plain.data()}, res{}, ent{(DWORD)strlen(tag), (BYTE*)tag};
    if (!CryptProtectData(&in, L"Tiny Browser", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &res)) return false;
    out.assign((const char*)res.pbData, res.cbData);
    LocalFree(res.pbData);
    return true;
}

bool dpapi_unprotect(const std::string& blob, std::string& out, const char* tag) {
    DATA_BLOB in{(DWORD)blob.size(), (BYTE*)blob.data()}, res{}, ent{(DWORD)strlen(tag), (BYTE*)tag};
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &res)) return false;
    out.assign((const char*)res.pbData, res.cbData);
    SecureZeroMemory(res.pbData, res.cbData);
    LocalFree(res.pbData);
    return true;
}

void secure_wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

bool read_file(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)std::min<LONGLONG>(sz.QuadPart, 64ll << 20));
    DWORD got = 0;
    BOOL ok = out.empty() || ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
    out.resize(got);
    CloseHandle(h);
    return ok != FALSE;
}

// Writes to a temp file and atomically replaces the target.
bool write_file_atomic(const std::wstring& p, const void* data, size_t n) {
    std::wstring tmp = p + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = n == 0 || WriteFile(h, data, (DWORD)n, &put, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || put != n) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

// ------------------------------------------------------------------ bookmarks

std::wstring Bookmarks::path() const { return path_.empty() ? app_data_dir() + L"\\bookmarks.tsv" : path_; }

void Bookmarks::load() {
    items.clear();
    std::string data;
    if (!read_file(path(), data)) return;
    size_t i = 0;
    while (i < data.size()) {
        size_t e = data.find('\n', i);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (!line.empty()) items.push_back({line.substr(0, tab), tab == std::string::npos ? "" : line.substr(tab + 1)});
        i = e + 1;
    }
}

void Bookmarks::save() const {
    std::string out;
    for (auto& b : items) {
        std::string t = b.title;
        for (auto& c : t)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        out += b.url + "\t" + t + "\n";
    }
    write_file_atomic(path(), out.data(), out.size());
}

int Bookmarks::find(const std::string& url) const {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].url == url) return (int)i;
    return -1;
}

void Bookmarks::toggle(const std::string& url, const std::string& title) {
    int i = find(url);
    if (i >= 0)
        items.erase(items.begin() + i);
    else
        items.push_back({url, title.empty() ? url : title});
    save();
}

void Bookmarks::removeAt(size_t i) {
    if (i < items.size()) {
        items.erase(items.begin() + i);
        save();
    }
}

// ------------------------------------------------------------------ password vault

static const char kEntropy[] = "TinyBrowser.vault.v1";
static const char kMagic[4] = {'T', 'B', 'V', '1'};

static void wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

PasswordVault::~PasswordVault() { lock(); }

void PasswordVault::lock() {
    for (auto& c : items_) wipe(c.pass);
    items_.clear();
}

bool PasswordVault::load() {
    for (auto& c : items_) wipe(c.pass);
    items_.clear();
    std::string blob;
    if (!read_file(path_, blob)) return true;  // no vault yet
    if (blob.empty()) return true;
    DATA_BLOB in{(DWORD)blob.size(), (BYTE*)blob.data()}, out{}, ent{(DWORD)sizeof(kEntropy) - 1, (BYTE*)kEntropy};
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    std::string plain((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    bool ok = false;
    size_t p = 0;
    auto rd32 = [&](uint32_t& v) {
        if (p + 4 > plain.size()) return false;
        memcpy(&v, plain.data() + p, 4);
        p += 4;
        return true;
    };
    auto rds = [&](std::string& s) {
        uint32_t n;
        if (!rd32(n) || p + n > plain.size()) return false;
        s.assign(plain.data() + p, n);
        p += n;
        return true;
    };
    if (plain.size() >= 8 && memcmp(plain.data(), kMagic, 4) == 0) {
        p = 4;
        uint32_t count = 0;
        if (rd32(count)) {
            ok = true;
            for (uint32_t i = 0; i < count && ok; ++i) {
                Credential c;
                ok = rds(c.origin) && rds(c.user) && rds(c.pass);
                if (ok) items_.push_back(std::move(c));
            }
        }
    }
    wipe(plain);
    return ok;
}

bool PasswordVault::save() const {
    std::string plain(kMagic, 4);
    auto put32 = [&](uint32_t v) { plain.append((const char*)&v, 4); };
    auto puts = [&](const std::string& s) {
        put32((uint32_t)s.size());
        plain += s;
    };
    put32((uint32_t)items_.size());
    for (auto& c : items_) {
        puts(c.origin);
        puts(c.user);
        puts(c.pass);
    }
    DATA_BLOB in{(DWORD)plain.size(), (BYTE*)plain.data()}, out{}, ent{(DWORD)sizeof(kEntropy) - 1, (BYTE*)kEntropy};
    BOOL ok = CryptProtectData(&in, L"Tiny Browser saved passwords", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    wipe(plain);
    if (!ok) return false;
    bool w = write_file_atomic(path_, out.pbData, out.cbData);
    LocalFree(out.pbData);
    return w;
}

void PasswordVault::put(const std::string& origin, const std::string& user, const std::string& pass) {
    for (auto& c : items_)
        if (c.origin == origin && c.user == user) {
            wipe(c.pass);
            c.pass = pass;
            return;
        }
    items_.push_back({origin, user, pass});
}

const Credential* PasswordVault::find(const std::string& origin, const std::string& user) const {
    for (auto& c : items_)
        if (c.origin == origin && c.user == user) return &c;
    return nullptr;
}

std::vector<const Credential*> PasswordVault::forOrigin(const std::string& origin) const {
    std::vector<const Credential*> r;
    for (auto& c : items_)
        if (c.origin == origin) r.push_back(&c);
    return r;
}

bool PasswordVault::removeAt(size_t i) {
    if (i >= items_.size()) return false;
    wipe(items_[i].pass);
    items_.erase(items_.begin() + i);
    return true;
}

}  // namespace tb
