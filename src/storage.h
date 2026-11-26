#pragma once
#include "common.h"

namespace tb {

std::wstring app_data_dir();  // %APPDATA%\TinyBrowser (created on demand)
bool read_file(const std::wstring& p, std::string& out);
bool write_file_atomic(const std::wstring& p, const void* data, size_t n);
// Windows DPAPI: encrypts for the current Windows user. `tag` is mixed in as extra entropy.
bool dpapi_protect(const std::string& plain, std::string& out, const char* tag);
bool dpapi_unprotect(const std::string& blob, std::string& out, const char* tag);
void secure_wipe(std::string& s);

struct Bookmark {
    std::string url, title;
};

class Bookmarks {
public:
    explicit Bookmarks(std::wstring path = {}) : path_(std::move(path)) {}
    std::vector<Bookmark> items;
    void load();
    void save() const;
    int find(const std::string& url) const;
    void toggle(const std::string& url, const std::string& title);
    void removeAt(size_t i);

private:
    std::wstring path() const;
    std::wstring path_;
};

struct Credential {
    std::string origin, user, pass;
};

// Saved logins, encrypted at rest with Windows DPAPI (bound to the current Windows user).
// Plaintext buffers are wiped after use.
class PasswordVault {
public:
    explicit PasswordVault(std::wstring path) : path_(std::move(path)) {}
    ~PasswordVault();
    bool load();
    bool save() const;
    void lock();  // wipes decrypted entries from memory
    void put(const std::string& origin, const std::string& user, const std::string& pass);
    const Credential* find(const std::string& origin, const std::string& user) const;
    std::vector<const Credential*> forOrigin(const std::string& origin) const;
    bool removeAt(size_t i);
    const std::vector<Credential>& all() const { return items_; }
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
    std::vector<Credential> items_;
};

}  // namespace tb
