#include <mcp/ext/ServerCard.hpp>
#include <mcp/Content.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/McpError.hpp>
#include <mcp/detail/StringUtils.hpp>
#include <mcp/transport/detail/Url.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace mcp::ext {

namespace {

constexpr std::size_t kMaxCardTextBytes = 8192;
constexpr std::size_t kMaxCardUrlBytes = 2048;
constexpr std::size_t kMaxCardHeaderValueBytes = 4096;
constexpr std::size_t kMaxCardVariableNameBytes = 128;

bool ServerCardHasControl(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char c) {
        auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7f;
    });
}

bool ServerCardIsHttpUrl(const std::string& url) {
    if (url.empty() || url.size() > kMaxCardUrlBytes || ServerCardHasControl(url))
        return false;
    if (url.find_first_of(" \t\\") != std::string::npos) return false;
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    auto scheme = detail::ToLower(std::string_view(url).substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") return false;
    auto start = scheme_end + 3;
    auto path_start = url.find('/', start);
    auto authority = path_start == std::string::npos
        ? url.substr(start) : url.substr(start, path_start - start);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    try {
        return !detail::ParseUrl(url).host.empty();
    } catch (const std::exception&) {
        return false;
    }
}

bool ServerCardIsDataIconSrc(std::string_view src) {
    if (src.size() < 11 || src.size() > kMaxCardUrlBytes || ServerCardHasControl(src))
        return false;
    auto lower = detail::ToLower(src.substr(0, 5));
    if (lower != "data:") return false;
    auto comma = src.find(",");
    if (comma == std::string_view::npos) return false;
    auto meta = detail::ToLower(src.substr(0, comma));
    if (meta.find("image/") == std::string_view::npos ||
        meta.find(";base64") == std::string_view::npos)
        return false;
    auto payload = src.substr(comma + 1);
    if (payload.empty()) return false;
    return std::all_of(payload.begin(), payload.end(), [](char c) {
        auto u = static_cast<unsigned char>(c);
        return std::isalnum(u) || c == '+' || c == '/' || c == '=';
    });
}

bool ServerCardIsVersionString(std::string_view version) {
    if (version.size() != 10 || version[4] != '-' || version[7] != '-') return false;
    for (std::size_t i = 0; i < version.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (!std::isdigit(static_cast<unsigned char>(version[i]))) return false;
    }
    int month = (version[5] - '0') * 10 + (version[6] - '0');
    int day = (version[8] - '0') * 10 + (version[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

bool ServerCardIsHeaderName(std::string_view name) {
    if (name.empty() || name.size() > kMaxCardVariableNameBytes) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        auto u = static_cast<unsigned char>(c);
        return std::isalnum(u) || std::string_view("!#$%&'*+-.^_`|~").find(c) !=
            std::string_view::npos;
    });
}

bool ServerCardIsVariableName(std::string_view name) {
    if (name.empty() || name.size() > kMaxCardVariableNameBytes) return false;
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
        return false;
    return std::all_of(name.begin() + 1, name.end(), [](char c) {
        auto u = static_cast<unsigned char>(c);
        return std::isalnum(u) || c == '_' || c == '-';
    });
}

bool ServerCardIsIconSize(std::string_view size) {
    if (size == "any") return true;
    auto x = size.find('x');
    if (x == std::string_view::npos || x == 0 || x + 1 == size.size()) return false;
    for (std::size_t i = 0; i < size.size(); ++i) {
        if (i == x) continue;
        if (!std::isdigit(static_cast<unsigned char>(size[i]))) return false;
    }
    return size.find('x', x + 1) == std::string_view::npos;
}

void ServerCardCheckText(const std::string& value, std::string_view field) {
    if (value.size() > kMaxCardTextBytes || ServerCardHasControl(value))
        throw McpError(McpErrorCode::InvalidParams,
                       std::string("server card ") + std::string(field) + " is invalid");
}

void ServerCardCheckUrl(const std::string& value, std::string_view field) {
    if (!ServerCardIsHttpUrl(value))
        throw McpError(McpErrorCode::InvalidParams,
                       std::string("server card ") + std::string(field) +
                           " must use a valid http/https url");
}

} // namespace

void ValidateCard(const ServerCard& card) {
    if (card.name.empty() || card.name.size() > kMaxCardTextBytes ||
        ServerCardHasControl(card.name))
        throw McpError(McpErrorCode::InvalidParams, "server card name must not be empty");
    if (card.version.empty() || card.version.size() > kMaxCardTextBytes ||
        ServerCardHasControl(card.version))
        throw McpError(McpErrorCode::InvalidParams, "server card version must not be empty");
    if (card.description) ServerCardCheckText(*card.description, "description");
    if (card.title) ServerCardCheckText(*card.title, "title");
    if (card.repository) ServerCardCheckUrl(*card.repository, "repository");
    if (card.website_url) ServerCardCheckUrl(*card.website_url, "website_url");

    for (const auto& icon : card.icons) {
        if (icon.src.empty() ||
            (!ServerCardIsHttpUrl(icon.src) && !ServerCardIsDataIconSrc(icon.src)))
            throw McpError(McpErrorCode::InvalidParams,
                           "server card icon src must be an http(s) or image data url");
        if (icon.mime_type) {
            auto mime = detail::ToLower(*icon.mime_type);
            if (mime.compare(0, 6, "image/") != 0 || mime.size() == 6 ||
                ServerCardHasControl(*icon.mime_type) ||
                mime.find_first_of(" \t") != std::string::npos)
                throw McpError(McpErrorCode::InvalidParams,
                               "server card icon mime_type must be an image type");
        }
        if (icon.sizes) {
            for (const auto& size : *icon.sizes) {
                if (size.empty() || size.size() > 32 || !ServerCardIsIconSize(size))
                    throw McpError(McpErrorCode::InvalidParams,
                                   "server card icon size is invalid");
            }
        }
        if (icon.theme && *icon.theme != "light" && *icon.theme != "dark")
            throw McpError(McpErrorCode::InvalidParams,
                           "server card icon theme must be light or dark");
    }

    if (card.endpoints.empty())
        throw McpError(McpErrorCode::InvalidParams, "server card endpoints must not be empty");
    for (const auto& endpoint : card.endpoints) {
        ServerCardCheckUrl(endpoint.url, "endpoint url");
        for (const auto& [key, value] : endpoint.headers) {
            if (!ServerCardIsHeaderName(key) || value.size() > kMaxCardHeaderValueBytes ||
                ServerCardHasControl(value))
                throw McpError(McpErrorCode::InvalidParams,
                               "server card endpoint header is invalid");
        }
        for (const auto& [key, value] : endpoint.variables) {
            if (!ServerCardIsVariableName(key) || value.size() > kMaxCardTextBytes ||
                ServerCardHasControl(value))
                throw McpError(McpErrorCode::InvalidParams,
                               "server card endpoint variable is invalid");
        }
    }

    for (const auto& version : card.supported_protocol_versions) {
        if (!ServerCardIsVersionString(version))
            throw McpError(McpErrorCode::InvalidParams,
                           "server card supportedProtocolVersions entry is invalid");
    }
}

JsonValue SerializeCard(const ServerCard& card) {
    ValidateCard(card);
    JsonValue::Object root;
    root["name"] = JsonValue(card.name);
    root["version"] = JsonValue(card.version);
    root["description"] = JsonValue(card.description.value_or(""));
    if (card.title) root["title"] = JsonValue(*card.title);
    if (card.repository) root["repository"] = JsonValue(*card.repository);
    if (card.website_url) root["website_url"] = JsonValue(*card.website_url);

    JsonValue::Array icons;
    for (const auto& icon : card.icons) icons.push_back(SerializeIcon(icon));
    root["icons"] = JsonValue(std::move(icons));

    JsonValue::Array endpoints;
    for (const auto& endpoint : card.endpoints) {
        JsonValue::Object entry;
        entry["url"] = JsonValue(endpoint.url);
        if (!endpoint.headers.empty()) {
            JsonValue::Object headers;
            for (const auto& [key, value] : endpoint.headers) headers[key] = JsonValue(value);
            entry["headers"] = JsonValue(std::move(headers));
        }
        if (!endpoint.variables.empty()) {
            JsonValue::Object variables;
            for (const auto& [key, value] : endpoint.variables) variables[key] = JsonValue(value);
            entry["variables"] = JsonValue(std::move(variables));
        }
        endpoints.push_back(JsonValue(std::move(entry)));
    }
    root["endpoints"] = JsonValue(std::move(endpoints));

    JsonValue::Array versions;
    for (const auto& version : card.supported_protocol_versions) versions.push_back(JsonValue(version));
    root["supportedProtocolVersions"] = JsonValue(std::move(versions));
    if (card.meta) root["meta"] = *card.meta;
    return JsonValue(std::move(root));
}

} // namespace mcp::ext
