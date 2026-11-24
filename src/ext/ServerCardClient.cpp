#include <mcp/ext/ServerCardClient.hpp>

#include <mcp/Content.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/McpError.hpp>
#include <mcp/detail/StringUtils.hpp>
#include <mcp/detail/sha256.hpp>
#include <mcp/transport/detail/Url.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_map>
#include <vector>

namespace mcp::ext {

namespace {

std::string_view ServerCardClientTrim(std::string_view text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool ServerCardClientHasControl(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char c) {
        auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7f;
    });
}

std::string_view ServerCardClientStripNormalizer(std::string_view tag) {
    tag = ServerCardClientTrim(tag);
    if (tag.size() >= 2 && (tag[0] == 'W' || tag[0] == 'w') && tag[1] == '/')
        tag.remove_prefix(2);
    return ServerCardClientTrim(tag);
}

std::string ServerCardClientRequireString(const JsonValue& root,
                                          std::string_view key,
                                          std::string_view where) {
    auto* value = root.Find(key);
    if (!value || !value->IsString())
        throw McpError(McpErrorCode::InvalidParams,
                       std::string("server card ") + std::string(where) +
                           " must be a string");
    return value->GetString();
}

std::optional<std::string> ServerCardClientOptionalString(const JsonValue& root,
                                                          std::string_view key,
                                                          std::string_view where) {
    auto* value = root.Find(key);
    if (!value) return std::nullopt;
    if (!value->IsString())
        throw McpError(McpErrorCode::InvalidParams,
                       std::string("server card ") + std::string(where) +
                           " must be a string");
    return value->GetString();
}

const JsonValue& ServerCardClientRequireArray(const JsonValue& root,
                                              std::string_view key) {
    auto* value = root.Find(key);
    if (!value || !value->IsArray())
        throw McpError(McpErrorCode::InvalidParams,
                       std::string("server card ") + std::string(key) +
                           " must be an array");
    return *value;
}

std::optional<std::string> ServerCardClientFindHeader(
    const std::unordered_map<std::string, std::string>& headers,
    std::string_view name) {
    if (auto it = headers.find(std::string(name)); it != headers.end())
        return it->second;
    auto wanted = detail::ToLower(name);
    for (const auto& [key, value] : headers) {
        if (detail::ToLower(key) == wanted) return value;
    }
    return std::nullopt;
}

} // namespace

std::string BuildServerCardDiscoveryUrl(std::string_view endpoint_url) {
    std::string url(endpoint_url);
    auto cut = url.find_first_of("?#");
    if (cut != std::string::npos) url.resize(cut);
    if (url.empty())
        throw McpError(McpErrorCode::InvalidParams, "server card endpoint url is empty");
    if (ServerCardClientHasControl(url))
        throw McpError(McpErrorCode::InvalidParams, "server card endpoint url has control characters");

    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        throw McpError(McpErrorCode::InvalidParams, "server card endpoint url is malformed");
    auto scheme = detail::ToLower(std::string_view(url).substr(0, scheme_end));
    if (scheme != "http" && scheme != "https")
        throw McpError(McpErrorCode::InvalidParams,
                       "server card endpoint url must use http/https");

    const auto authority_start = scheme_end + 3;
    auto path_start = url.find('/', authority_start);
    std::string authority = path_start == std::string::npos
        ? url.substr(authority_start)
        : url.substr(authority_start, path_start - authority_start);
    if (authority.empty() || authority.find('@') != std::string::npos)
        throw McpError(McpErrorCode::InvalidParams,
                       "server card endpoint url has empty authority or userinfo");
    if (authority.find_first_of(" \t\\") != std::string::npos)
        throw McpError(McpErrorCode::InvalidParams, "server card endpoint url authority is malformed");

    std::string path = path_start == std::string::npos ? "/" : url.substr(path_start);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path == "/") path.clear();

    try {
        auto parts = detail::ParseUrl(url);
        if (parts.host.empty())
            throw McpError(McpErrorCode::InvalidParams, "server card endpoint url has no host");
    } catch (const std::invalid_argument& e) {
        throw McpError(McpErrorCode::InvalidParams,
                       std::string("server card endpoint url is malformed: ") + e.what());
    }

    return scheme + "://" + authority + path + "/server-card";
}

bool IsServerCardJsonMediaType(std::string_view content_type) {
    auto semicolon = content_type.find(';');
    auto base = ServerCardClientTrim(semicolon == std::string_view::npos
        ? content_type : content_type.substr(0, semicolon));
    auto lower = detail::ToLower(base);
    if (lower == "application/json") return true;
    constexpr std::string_view kApplicationPrefix = "application/";
    return lower.size() > kApplicationPrefix.size() + 4 &&
           lower.compare(0, kApplicationPrefix.size(), kApplicationPrefix) == 0 &&
           lower.compare(lower.size() - 5, 5, "+json") == 0;
}

bool ServerCardEtagMatches(std::string_view if_none_match, std::string_view etag) {
    if (if_none_match.empty() || etag.empty()) return false;
    auto expected = ServerCardClientStripNormalizer(etag);
    if (expected.empty()) return false;
    std::string_view list(if_none_match);
    std::size_t pos = 0;
    while (pos <= list.size()) {
        auto end = list.find(',', pos);
        auto candidate = ServerCardClientStripNormalizer(
            list.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
        if (candidate == "*") return true;
        if (candidate == expected) return true;
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    return false;
}

std::string CanonicalizeServerCardBody(std::string_view body) {
    try {
        return JsonValue::Parse(body).Dump();
    } catch (const std::exception&) {
        throw McpError(McpErrorCode::InvalidParams, "invalid server card JSON");
    }
}

std::string ComputeServerCardEtag(std::string_view body) {
    constexpr char kHex[] = "0123456789abcdef";
    const auto digest = detail::Sha256::Hash(CanonicalizeServerCardBody(body));
    std::string hex;
    hex.reserve(digest.size() * 2 + 2);
    hex.push_back('"');
    for (unsigned char byte : digest) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 0x0f]);
    }
    hex.push_back('"');
    return hex;
}

ServerCard ParseServerCard(std::string_view json) {
    JsonValue root;
    try {
        root = JsonValue::Parse(json);
    } catch (const std::exception&) {
        throw McpError(McpErrorCode::InvalidParams, "invalid server card JSON");
    }
    if (!root.IsObject())
        throw McpError(McpErrorCode::InvalidParams, "server card must be a JSON object");

    ServerCard card;
    card.name = ServerCardClientRequireString(root, "name", "name");
    card.version = ServerCardClientRequireString(root, "version", "version");
    card.description = ServerCardClientOptionalString(root, "description", "description");
    card.title = ServerCardClientOptionalString(root, "title", "title");
    card.repository = ServerCardClientOptionalString(root, "repository", "repository");
    card.website_url = ServerCardClientOptionalString(root, "website_url", "website_url");

    const auto& icons = ServerCardClientRequireArray(root, "icons").GetArray();
    for (const auto& icon : icons) {
        if (!icon.IsObject())
            throw McpError(McpErrorCode::InvalidParams,
                           "server card icons must contain objects");
        ServerCardClientRequireString(icon, "src", "icon src");
        card.icons.push_back(DeserializeIcon(icon));
    }

    const auto& endpoints = ServerCardClientRequireArray(root, "endpoints").GetArray();
    for (const auto& entry : endpoints) {
        if (!entry.IsObject())
            throw McpError(McpErrorCode::InvalidParams,
                           "server card endpoints must contain objects");
        ServerCardEndpoint endpoint;
        endpoint.url = ServerCardClientRequireString(entry, "url", "endpoint url");
        for (std::string_view field : {std::string_view("headers"), std::string_view("variables")}) {
            auto* table = entry.Find(field);
            if (!table) continue;
            if (!table->IsObject())
                throw McpError(McpErrorCode::InvalidParams,
                               std::string("server card endpoint ") + std::string(field) +
                                   " must be an object");
            for (const auto& [key, value] : table->GetObject()) {
                if (!value.IsString())
                    throw McpError(McpErrorCode::InvalidParams,
                                   "server card endpoint " + std::string(field) +
                                       " values must be strings");
                if (field == "headers") endpoint.headers[key] = value.GetString();
                else endpoint.variables[key] = value.GetString();
            }
        }
        card.endpoints.push_back(std::move(endpoint));
    }

    const auto& versions =
        ServerCardClientRequireArray(root, "supportedProtocolVersions").GetArray();
    for (const auto& version : versions) {
        if (!version.IsString())
            throw McpError(McpErrorCode::InvalidParams,
                           "server card supportedProtocolVersions must contain strings");
        card.supported_protocol_versions.push_back(version.GetString());
    }

    if (auto* meta = root.Find("meta")) card.meta = *meta;

    ValidateCard(card);
    return card;
}

ServerCardFetchResult FetchServerCard(std::string_view endpoint_url,
                                      const ServerCardClientOptions& options) {
    const std::string url = BuildServerCardDiscoveryUrl(endpoint_url);
    detail::net::HttpRequestSpec request;
    request.method = "GET";
    request.url = url;
    request.headers["accept"] = "application/json";
    request.timeout = options.timeout;
    request.max_response_bytes =
        options.max_response_bytes.value_or(kDefaultServerCardResponseBytes);
    request.redirect_policy = detail::HttpRedirectPolicy::SameOriginOnly;
    request.max_redirects = options.max_redirects;
    if (options.if_none_match)
        request.headers["if-none-match"] = *options.if_none_match;

    detail::net::HttpClient client;
    auto response = client.Request(request);

    if (response.status_code == 304) {
        if (!options.cached_card)
            throw McpError(McpErrorCode::InvalidParams,
                           "server card 304 without a cached card");
        auto etag = ServerCardClientFindHeader(response.headers, "etag");
        ServerCardFetchResult result;
        result.card = *options.cached_card;
        result.etag = etag.value_or(options.if_none_match.value_or(""));
        result.from_cache = true;
        return result;
    }
    if (response.status_code != 200)
        throw McpError(McpErrorCode::ProtocolViolation,
                       "server card fetch failed with HTTP status " +
                           std::to_string(response.status_code));

    const auto content_type = ServerCardClientFindHeader(response.headers, "content-type");
    if (!content_type || !IsServerCardJsonMediaType(*content_type))
        throw McpError(McpErrorCode::ProtocolViolation,
                       "server card response content-type is not application/json");
    if (response.body.empty())
        throw McpError(McpErrorCode::InvalidParams, "server card response body is empty");

    ServerCardFetchResult result;
    result.card = ParseServerCard(response.body);
    auto etag = ServerCardClientFindHeader(response.headers, "etag");
    if (etag) result.etag = *etag;
    return result;
}

} // namespace mcp::ext
