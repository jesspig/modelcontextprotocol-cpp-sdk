#pragma once

#include <mcp/transport/detail/Limits.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp { namespace detail { namespace net {

struct UrlSecurityAddress {
    int family{0};
    std::array<unsigned char, 16> bytes{};
    std::size_t length{0};

    [[nodiscard]] bool operator==(const UrlSecurityAddress& other) const {
        return family == other.family && length == other.length && bytes == other.bytes;
    }
};

enum class UrlAddressClass {
    Invalid,
    Public,
    Loopback,
    PrivateAddress,
    LinkLocal,
    Multicast,
    Unspecified,
    Reserved,
    Mapped,
};

inline std::optional<unsigned int> ParseHexGroup(std::string_view text) {
    if (text.empty() || text.size() > 4) return std::nullopt;
    unsigned int value = 0;
    for (char c : text) {
        value <<= 4;
        if (c >= '0' && c <= '9') value += static_cast<unsigned int>(c - '0');
        else if (c >= 'a' && c <= 'f') value += static_cast<unsigned int>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value += static_cast<unsigned int>(c - 'A' + 10);
        else return std::nullopt;
    }
    return value;
}

inline std::optional<UrlSecurityAddress> ParseIpv4Literal(std::string_view text) {
    std::array<unsigned char, 4> out{};
    std::size_t part = 0;
    std::size_t pos = 0;
    bool complete = false;
    while (part < 4) {
        auto dot = text.find('.', pos);
        std::string_view octet = dot == std::string_view::npos
            ? text.substr(pos) : text.substr(pos, dot - pos);
        if (octet.empty() || octet.size() > 3) return std::nullopt;
        if (octet.size() > 1 && octet.front() == '0') return std::nullopt;
        unsigned int value = 0;
        for (char c : octet) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + static_cast<unsigned int>(c - '0');
        }
        if (value > 255) return std::nullopt;
        out[part++] = static_cast<unsigned char>(value);
        if (dot == std::string_view::npos) {
            complete = true;
            break;
        }
        pos = dot + 1;
    }
    if (part != 4 || !complete) return std::nullopt;
    UrlSecurityAddress address;
    address.family = 4;
    address.length = 4;
    std::copy(out.begin(), out.end(), address.bytes.begin());
    return address;
}

inline std::optional<UrlSecurityAddress> ParseIpv6Literal(std::string_view text) {
    std::vector<unsigned int> groups;
    auto compress = text.find("::");
    bool compressed = compress != std::string_view::npos;
    std::string_view left = compressed ? text.substr(0, compress) : text;
    std::string_view right = compressed ? text.substr(compress + 2) : std::string_view{};
    if (compressed && right.find("::") != std::string_view::npos) return std::nullopt;

    auto append_groups = [&](std::string_view part) {
        std::size_t pos = 0;
        while (pos <= part.size()) {
            auto colon = part.find(':', pos);
            std::string_view token = colon == std::string_view::npos
                ? part.substr(pos) : part.substr(pos, colon - pos);
            if (token.find('.') != std::string_view::npos) {
                if (colon != std::string_view::npos) return false;
                auto ipv4 = ParseIpv4Literal(token);
                if (!ipv4) return false;
                groups.push_back((static_cast<unsigned int>(ipv4->bytes[0]) << 8) |
                                 static_cast<unsigned int>(ipv4->bytes[1]));
                groups.push_back((static_cast<unsigned int>(ipv4->bytes[2]) << 8) |
                                 static_cast<unsigned int>(ipv4->bytes[3]));
            } else {
                auto group = ParseHexGroup(token);
                if (!group) return false;
                groups.push_back(*group);
            }
            if (colon == std::string_view::npos) break;
            pos = colon + 1;
        }
        return true;
    };

    if (!left.empty() && !append_groups(left)) return std::nullopt;
    if (!right.empty() && !append_groups(right)) return std::nullopt;
    if (groups.size() > 8) return std::nullopt;
    if (!compressed && groups.size() != 8) return std::nullopt;
    if (compressed && groups.size() >= 8) return std::nullopt;

    std::array<unsigned char, 16> bytes{};
    std::size_t head = compressed ? groups.size() : 0;
    std::size_t tail_index = compressed ? 8 - groups.size() : 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        std::size_t index = i < head ? i : tail_index + (i - head);
        bytes[index * 2] = static_cast<unsigned char>(groups[i] >> 8);
        bytes[index * 2 + 1] = static_cast<unsigned char>(groups[i] & 0xFF);
    }
    UrlSecurityAddress address;
    address.family = 6;
    address.length = 16;
    address.bytes = bytes;
    return address;
}

inline std::optional<UrlSecurityAddress> ParseIpLiteral(std::string_view text) {
    if (text.find(':') != std::string_view::npos) return ParseIpv6Literal(text);
    return ParseIpv4Literal(text);
}

inline bool AllZeroExceptLast(const std::array<unsigned char, 16>& bytes, std::size_t last) {
    for (std::size_t i = 0; i < last; ++i)
        if (bytes[i] != 0) return false;
    return true;
}

inline UrlAddressClass ClassifyAddress(const UrlSecurityAddress& address) {
    if (address.family == 4 && address.length == 4) {
        const unsigned char a = address.bytes[0];
        const unsigned char b = address.bytes[1];
        const unsigned char c = address.bytes[2];
        if (a == 0)
            return b == 0 && c == 0 && address.bytes[3] == 0
                ? UrlAddressClass::Unspecified : UrlAddressClass::Reserved;
        if (a == 10) return UrlAddressClass::PrivateAddress;
        if (a == 127) return UrlAddressClass::Loopback;
        if (a == 169 && b == 254) return UrlAddressClass::LinkLocal;
        if (a == 172 && b >= 16 && b <= 31) return UrlAddressClass::PrivateAddress;
        if (a == 192 && b == 168) return UrlAddressClass::PrivateAddress;
        if (a == 100 && b >= 64 && b <= 127) return UrlAddressClass::Reserved;
        if (a == 192 && b == 0 && (c == 0 || c == 2)) return UrlAddressClass::Reserved;
        if (a == 192 && b == 88 && c == 99) return UrlAddressClass::Reserved;
        if (a == 198 && (b == 18 || b == 19)) return UrlAddressClass::Reserved;
        if (a == 198 && b == 51 && c == 100) return UrlAddressClass::Reserved;
        if (a == 203 && b == 0 && c == 113) return UrlAddressClass::Reserved;
        if (a >= 224 && a <= 239) return UrlAddressClass::Multicast;
        if (a >= 240) return UrlAddressClass::Reserved;
        return UrlAddressClass::Public;
    }
    if (address.family == 6 && address.length == 16) {
        const auto& x = address.bytes;
        bool all_zero = std::all_of(x.begin(), x.end(), [](unsigned char v) { return v == 0; });
        if (all_zero) return UrlAddressClass::Unspecified;
        if (AllZeroExceptLast(x, 15) && x[15] == 1) return UrlAddressClass::Loopback;
        if (AllZeroExceptLast(x, 10) && x[10] == 0xFF && x[11] == 0xFF)
            return UrlAddressClass::Mapped;
        if ((x[0] & 0xFE) == 0xFC) return UrlAddressClass::PrivateAddress;
        if (x[0] == 0xFE && (x[1] & 0xC0) == 0x80) return UrlAddressClass::LinkLocal;
        if (x[0] == 0xFF) return UrlAddressClass::Multicast;
        if (x[0] == 0x20 && x[1] == 0x01 && x[2] == 0x0D && x[3] == 0xB8)
            return UrlAddressClass::Reserved;
        if (x[0] == 0x20 && x[1] == 0x01 && x[2] == 0x00 && x[3] == 0x00)
            return UrlAddressClass::Reserved;
        if (x[0] == 0x20 && x[1] == 0x02) return UrlAddressClass::Reserved;
        if (x[0] == 0x00 && x[1] == 0x64 && x[2] == 0xFF && x[3] == 0x9B)
            return UrlAddressClass::Reserved;
        return UrlAddressClass::Public;
    }
    return UrlAddressClass::Invalid;
}

[[nodiscard]] inline bool IsPublicAddress(const UrlSecurityAddress& address) {
    return ClassifyAddress(address) == UrlAddressClass::Public;
}

struct UrlSecurityOrigin {
    std::string scheme;
    std::string host;
    std::uint16_t port{0};

    [[nodiscard]] std::string HostForKey() const {
        if (host.find(':') != std::string::npos) return "[" + host + "]";
        return host;
    }

    [[nodiscard]] std::string Key() const {
        return scheme + "://" + HostForKey() + ":" + std::to_string(port);
    }

    [[nodiscard]] bool operator==(const UrlSecurityOrigin& other) const {
        return Key() == other.Key();
    }
};

inline std::string ToLowerAsciiCopy(std::string_view input) {
    std::string out(input);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

inline bool IsValidDnsHost(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    if (host.front() == '.' || host.back() == '.') return false;
    std::size_t label_length = 0;
    for (char c : host) {
        unsigned char u = static_cast<unsigned char>(c);
        bool alpha = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
        bool digit = (u >= '0' && u <= '9');
        if (alpha || digit || c == '-') {
            if (c == '-' && label_length == 0) return false;
            ++label_length;
            if (label_length > 63) return false;
        } else if (c == '.') {
            if (label_length == 0) return false;
            label_length = 0;
        } else {
            return false;
        }
    }
    return label_length > 0;
}

inline std::optional<UrlSecurityOrigin> ParseUrlSecurityOrigin(std::string_view url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return std::nullopt;
    std::string scheme = ToLowerAsciiCopy(url.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") return std::nullopt;
    std::string_view rest = url.substr(scheme_end + 3);
    auto authority_end = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, authority_end);
    if (authority.empty() || authority.find('@') != std::string_view::npos ||
        authority.find('%') != std::string_view::npos ||
        authority.find_first_of("\r\n\t ") != std::string_view::npos)
        return std::nullopt;

    UrlSecurityOrigin origin;
    origin.scheme = std::move(scheme);
    std::uint16_t default_port = origin.scheme == "https" ? 443 : 80;
    if (authority.front() == '[') {
        auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        std::string_view literal = authority.substr(1, close - 1);
        if (!ParseIpLiteral(literal)) return std::nullopt;
        origin.host = ToLowerAsciiCopy(literal);
        std::string_view suffix = authority.substr(close + 1);
        if (suffix.empty()) {
            origin.port = default_port;
        } else {
            if (suffix.front() != ':') return std::nullopt;
            suffix.remove_prefix(1);
            if (suffix.empty() || suffix.find_first_not_of("0123456789") != std::string_view::npos)
                return std::nullopt;
            unsigned long value = 0;
            try {
                value = std::stoul(std::string(suffix));
            } catch (...) {
                return std::nullopt;
            }
            if (value == 0 || value > 65535) return std::nullopt;
            origin.port = static_cast<std::uint16_t>(value);
        }
        return origin;
    }

    auto colon = authority.find(':');
    std::string_view host_text = colon == std::string_view::npos
        ? authority : authority.substr(0, colon);
    if (host_text.empty()) return std::nullopt;
    origin.host = ToLowerAsciiCopy(host_text);
    if (auto literal = ParseIpLiteral(host_text)) {
        (void)literal;
    } else {
        bool all_numeric = std::all_of(host_text.begin(), host_text.end(),
            [](char c) { return (c >= '0' && c <= '9') || c == '.' || c == 'x' || c == 'X'; });
        bool hex_numeric = std::all_of(host_text.begin(), host_text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F') || c == 'x' || c == 'X' || c == '.';
        });
        bool starts_with_digit = host_text.front() >= '0' && host_text.front() <= '9';
        if (all_numeric || (hex_numeric && starts_with_digit) ||
            !IsValidDnsHost(origin.host)) return std::nullopt;
    }
    if (colon == std::string_view::npos) {
        origin.port = default_port;
        return origin;
    }
    std::string_view port_text = authority.substr(colon + 1);
    if (port_text.empty() || port_text.find_first_not_of("0123456789") != std::string_view::npos)
        return std::nullopt;
    unsigned long value = 0;
    try {
        value = std::stoul(std::string(port_text));
    } catch (...) {
        return std::nullopt;
    }
    if (value == 0 || value > 65535) return std::nullopt;
    origin.port = static_cast<std::uint16_t>(value);
    return origin;
}

class IUrlSecurityResolver {
public:
    virtual ~IUrlSecurityResolver() = default;
    virtual std::vector<UrlSecurityAddress> Resolve(std::string_view host) = 0;
};

std::shared_ptr<IUrlSecurityResolver> CreateSystemUrlSecurityResolver();

struct UrlSecurityPolicyConfig {
    std::vector<std::string> allowed_origins;
    std::vector<std::pair<std::string, std::uint16_t>> loopback_exceptions;
    std::size_t max_dns_answers{16};
    std::size_t max_redirects{static_cast<std::size_t>(kMaxHttpRedirects)};
    bool allow_cross_origin_redirect{false};
    std::shared_ptr<IUrlSecurityResolver> resolver;
};

class UrlSecurityPolicy {
public:
    explicit UrlSecurityPolicy(UrlSecurityPolicyConfig config)
        : config_(std::move(config))
        , resolver_(config_.resolver ? config_.resolver : CreateSystemUrlSecurityResolver())
    {
        for (const auto& text : config_.allowed_origins) {
            auto parsed = ParseUrlSecurityOrigin(text);
            if (parsed) allowed_.push_back(std::move(*parsed));
        }
        for (const auto& entry : config_.loopback_exceptions) {
            loopback_exceptions_.emplace_back(ToLowerAsciiCopy(entry.first), entry.second);
        }
    }

    [[nodiscard]] bool Allows(const UrlSecurityOrigin& origin) const {
        return std::find(allowed_.begin(), allowed_.end(), origin) != allowed_.end();
    }

    [[nodiscard]] std::size_t MaxRedirects() const { return config_.max_redirects; }

    [[nodiscard]] bool AllowsCrossOriginRedirect() const {
        return config_.allow_cross_origin_redirect;
    }

    [[nodiscard]] bool Validate(std::string_view url, std::string& error,
                                std::vector<UrlSecurityAddress>& addresses) const {
        addresses.clear();
        if (url.empty() || url.size() > 8192) {
            error = "url_length";
            return false;
        }
        auto origin = ParseUrlSecurityOrigin(url);
        if (!origin) {
            error = "invalid_url";
            return false;
        }
        if (!Allows(*origin)) {
            error = "origin_not_allowed";
            return false;
        }
        auto literal = ParseIpLiteral(origin->host);
        if (!literal) {
            if (origin->host == "localhost") {
                auto resolved = Resolve(origin->host);
                if (!ValidateAnswers(*origin, resolved, error, addresses)) return false;
                return true;
            }
            auto resolved = Resolve(origin->host);
            if (!ValidateAnswers(*origin, resolved, error, addresses)) return false;
            return true;
        }
        if (!IsPublicAddress(*literal) && !LoopbackException(*origin, *literal)) {
            error = "address_forbidden";
            return false;
        }
        addresses.push_back(*literal);
        return true;
    }

    [[nodiscard]] bool ValidateRedirect(std::string_view from_url, std::string_view to_url,
                                        std::string& error,
                                        std::vector<UrlSecurityAddress>& addresses) const {
        if (!Validate(to_url, error, addresses)) return false;
        if (config_.allow_cross_origin_redirect) return true;
        auto from = ParseUrlSecurityOrigin(from_url);
        auto to = ParseUrlSecurityOrigin(to_url);
        if (!from || !to || !(*from == *to)) {
            error = "cross_origin_redirect";
            return false;
        }
        return true;
    }

private:
    [[nodiscard]] bool LoopbackException(const UrlSecurityOrigin& origin,
                                         const UrlSecurityAddress& address) const {
        if (ClassifyAddress(address) != UrlAddressClass::Loopback) return false;
        auto it = std::find(loopback_exceptions_.begin(), loopback_exceptions_.end(),
                            std::make_pair(origin.host, origin.port));
        return it != loopback_exceptions_.end();
    }

    [[nodiscard]] bool ValidateAnswers(const UrlSecurityOrigin& origin,
                                       const std::vector<UrlSecurityAddress>& answers,
                                       std::string& error,
                                       std::vector<UrlSecurityAddress>& addresses) const {
        if (answers.empty()) {
            error = "dns_no_answer";
            return false;
        }
        if (answers.size() > config_.max_dns_answers) {
            error = "dns_answer_count";
            return false;
        }
        for (const auto& address : answers) {
            if (!IsPublicAddress(address) && !LoopbackException(origin, address)) {
                error = "address_forbidden";
                return false;
            }
        }
        addresses = answers;
        return true;
    }

    [[nodiscard]] std::vector<UrlSecurityAddress> Resolve(std::string_view host) const {
        if (!resolver_) return {};
        try {
            return resolver_->Resolve(host);
        } catch (...) {
            return {};
        }
    }

    UrlSecurityPolicyConfig config_;
    std::shared_ptr<IUrlSecurityResolver> resolver_;
    std::vector<UrlSecurityOrigin> allowed_;
    std::vector<std::pair<std::string, std::uint16_t>> loopback_exceptions_;
};

}}} // namespace mcp::detail::net
