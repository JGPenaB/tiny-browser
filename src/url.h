#pragma once
#include "common.h"

namespace tb {

struct Url {
    std::string scheme, host, port, path, query, fragment;
    bool valid = false, hierarchical = false, hasQuery = false, hasFragment = false;
    // Schemes the browser has no special rules for (x:/path, app://host/path): `generic` when the path is a list
    // of segments that relative references resolve against, `hasAuthority` when a host follows the scheme.
    bool generic = false, hasAuthority = false;

    static Url parse(std::string_view s);
    std::string hostport() const { return port.empty() ? host : host + ":" + port; }
    std::string origin() const { return scheme + "://" + hostport(); }
    std::string str(bool withFragment = true) const;
};

std::string resolve_url(const std::string& base, std::string_view rel);
std::string strip_fragment(const std::string& url);
std::string percent_decode(std::string_view s);
std::string percent_encode_path(std::string_view s);
std::string form_encode(std::string_view s);

}  // namespace tb
