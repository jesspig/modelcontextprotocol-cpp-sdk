#pragma once
// Url.hpp — URL parsing utilities

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
        // IPv6 literal: [::1]:port
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

}} // namespace mcp::detail
