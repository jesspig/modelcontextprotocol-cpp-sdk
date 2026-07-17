#pragma once

#include <string>
#include <cstdint>

namespace mcp { namespace detail {

struct UrlParts {
    std::string scheme;
    std::string host;
    uint16_t port{0};
    std::string path;
};

inline UrlParts ParseUrl(const std::string& url) {
    UrlParts c;

    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) return c;
    c.scheme = url.substr(0, scheme_pos);

    auto start = scheme_pos + 3;
    auto path_pos = url.find('/', start);
    auto host_port = (path_pos == std::string::npos) ? url.substr(start) : url.substr(start, path_pos - start);
    c.path = (path_pos == std::string::npos) ? "/" : url.substr(path_pos);

    auto colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        c.host = host_port.substr(0, colon_pos);
        c.port = static_cast<uint16_t>(std::stoul(host_port.substr(colon_pos + 1)));
    } else {
        c.host = host_port;
        c.port = (c.scheme == "https" || c.scheme == "wss") ? 443 : 80;
    }

    return c;
}

}} // namespace mcp::detail
