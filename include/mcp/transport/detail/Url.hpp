#pragma once

#include <stdexcept>
#include <string>
#include <cstdint>

namespace mcp { namespace detail {

struct UrlParts {
    std::string scheme;
    std::string host;
    uint16_t port{0};
    std::string path;
};

inline uint16_t ParsePort(const std::string& port_str, const std::string& url) {
    if (port_str.empty() || port_str.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("invalid port in URL: " + url);
    unsigned long port = 0;
    try {
        port = std::stoul(port_str);
    } catch (const std::out_of_range&) {
        throw std::invalid_argument("port out of range in URL: " + url);
    }
    if (port > 65535)
        throw std::invalid_argument("port out of range in URL: " + url);
    return static_cast<uint16_t>(port);
}

inline UrlParts ParseUrl(const std::string& url) {
    UrlParts c;

    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) return c;
    c.scheme = url.substr(0, scheme_pos);

    auto start = scheme_pos + 3;
    auto path_pos = url.find('/', start);
    auto host_port = (path_pos == std::string::npos) ? url.substr(start) : url.substr(start, path_pos - start);
    c.path = (path_pos == std::string::npos) ? "/" : url.substr(path_pos);

    uint16_t default_port = (c.scheme == "https" || c.scheme == "wss") ? 443 : 80;

    if (!host_port.empty() && host_port[0] == '[') {
        auto close_pos = host_port.find(']');
        if (close_pos == std::string::npos)
            throw std::invalid_argument("unterminated IPv6 host in URL: " + url);
        c.host = host_port.substr(1, close_pos - 1);
        if (close_pos + 1 < host_port.size()) {
            if (host_port[close_pos + 1] != ':')
                throw std::invalid_argument("invalid port in URL: " + url);
            c.port = ParsePort(host_port.substr(close_pos + 2), url);
        } else {
            c.port = default_port;
        }
        return c;
    }

    auto colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        c.host = host_port.substr(0, colon_pos);
        c.port = ParsePort(host_port.substr(colon_pos + 1), url);
    } else {
        c.host = host_port;
        c.port = default_port;
    }

    return c;
}

inline bool EqualUrlHost(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

inline bool IsSameOriginRedirect(const std::string& from_url,
                                 const std::string& to_url) {
    UrlParts from = ParseUrl(from_url);
    UrlParts to = ParseUrl(to_url);
    if (from.host.empty() || to.host.empty()) return false;
    if (!EqualUrlHost(from.host, to.host)) return false;
    if (from.scheme == to.scheme) return from.port == to.port;
    if (from.scheme == "http" && to.scheme == "https") return true;
    return false;
}

inline std::string StripUrlFragment(const std::string& url) {
    auto hash = url.find('#');
    return hash == std::string::npos ? url : url.substr(0, hash);
}

inline std::string ResolveRedirectUrl(const std::string& base_url,
                                      const std::string& location) {
    size_t first = location.find_first_not_of(" \t");
    size_t last = location.find_last_not_of(" \t");
    if (first == std::string::npos)
        throw std::invalid_argument("empty redirect location");
    std::string loc = location.substr(first, last - first + 1);
    if (loc.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("invalid redirect location");
    if (loc.find("://") != std::string::npos) return StripUrlFragment(loc);
    UrlParts base = ParseUrl(base_url);
    if (base.host.empty() || (base.scheme != "http" && base.scheme != "https"))
        throw std::invalid_argument("invalid base URL for redirect: " + base_url);
    std::string host = base.host.find(':') != std::string::npos ? "[" + base.host + "]" : base.host;
    std::string origin = base.scheme + "://" + host;
    bool default_port = (base.scheme == "https" && base.port == 443) ||
                        (base.scheme == "http" && base.port == 80);
    if (!default_port) origin += ":" + std::to_string(base.port);
    if (!loc.empty() && loc[0] == '/') return origin + StripUrlFragment(loc);
    std::string path = base.path;
    auto slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? "/" : path.substr(0, slash + 1);
    return origin + dir + StripUrlFragment(loc);
}

inline bool IsRedirectStatus(int status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

}} // namespace mcp::detail
