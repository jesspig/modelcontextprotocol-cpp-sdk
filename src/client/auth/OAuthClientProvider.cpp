// OAuthClientProvider.cpp
// OAuth PKCE flow, token refresh, and metadata discovery implementation
#include <mcp/client/auth/OAuthClientProvider.hpp>
#include <detail/JsonFields.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>
#include <mcp/detail/sha256.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include <chrono>
#ifdef MCP_HAVE_OPENSSL
#include <openssl/rand.h>
#endif

#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {

std::string UrlEncode(std::string_view input) {
    std::string result;
    result.reserve(input.size() * 3);
    for (unsigned char c : input) {
        if (('0' <= c && c <= '9') ||
            ('A' <= c && c <= 'Z') ||
            ('a' <= c && c <= 'z') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            result += static_cast<char>(c);
        } else if (c == ' ') {
            result += '+';
        } else {
            result += '%';
            result += "0123456789ABCDEF"[c >> 4];
            result += "0123456789ABCDEF"[c & 0xF];
        }
    }
    return result;
}

using NetResponse = mcp::detail::net::HttpResponseInfo;

std::optional<NetResponse> HttpGetUrl(const std::string& url) {
    try {
        mcp::detail::net::HttpClient client;
        mcp::detail::net::HttpRequestSpec req;
        req.method = "GET";
        req.url = url;
        return client.Request(req);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<NetResponse> HttpPostUrl(
    const std::string& url, const std::string& body,
    const std::unordered_map<std::string, std::string>& headers = {})
{
    try {
        mcp::detail::net::HttpClient client;
        mcp::detail::net::HttpRequestSpec req;
        req.method = "POST";
        req.url = url;
        req.body = body;
        req.headers = headers;
        return client.Request(req);
    } catch (...) {
        return std::nullopt;
    }
}

} // anonymous namespace

namespace mcp {

namespace {

// Parse an OAuth authorization-server metadata document.
OAuthMetadata ParseMetadataJson(const JsonValue& json) {
    OAuthMetadata meta;
    if (auto* v = json.Find("issuer"))
        meta.issuer = v->GetString();
    if (auto* v = json.Find("authorization_endpoint"))
        meta.authorization_endpoint = v->GetString();
    if (auto* v = json.Find("token_endpoint"))
        meta.token_endpoint = v->GetString();
    if (auto* v = json.Find("revocation_endpoint"))
        meta.revocation_endpoint = v->GetString();
    if (auto* v = json.Find("registration_endpoint"))
        meta.registration_endpoint = v->GetString();
    if (auto* v = json.Find("jwks_uri"))
        meta.jwks_uri = v->GetString();
    if (auto* v = json.Find(detail::kResource))
        meta.resource = v->GetString();

    auto extract_vec = [&](const char* key, std::vector<std::string>& vec) {
        if (auto* v = json.Find(key)) {
            if (v->IsArray()) {
                for (const auto& item : v->GetArray())
                    if (item.IsString()) vec.push_back(item.GetString());
            }
        }
    };
    extract_vec("scopes_supported", meta.scopes_supported);
    extract_vec("response_types_supported", meta.response_types_supported);
    extract_vec("grant_types_supported", meta.grant_types_supported);
    extract_vec("code_challenge_methods_supported", meta.code_challenge_methods_supported);

    return meta;
}

// RFC 9728 §3.2: when a resource_metadata document advertises a `resource`,
// it must match the URL the client actually requested (exactly, or at least
// its scheme+host authority), otherwise the document does not belong to this
// resource and must not be trusted.
void VerifyResourceMatch(const std::string& requested_url,
                         const std::optional<std::string>& resource)
{
    if (!resource || resource->empty()) return;
    if (*resource == requested_url) return;
    auto scheme_end = requested_url.find("://");
    if (scheme_end != std::string::npos) {
        auto host_end = requested_url.find_first_of("/?#", scheme_end + 3);
        if (*resource == requested_url.substr(0, host_end)) return;
    }
    throw McpError(McpErrorCode::InternalError,
        "OAuth resource_metadata resource does not match the request URL");
}

} // anonymous namespace

// ====================================================================
// PKCE implementation
// ====================================================================
namespace pkce {

std::string Base64UrlEncode(std::string_view input) {
    static const char* b64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    result.reserve(((input.size() + 2) / 3) * 4);

    for (size_t i = 0; i < input.size(); i += 3) {
        size_t remaining = input.size() - i;
        uint32_t octet_a = (unsigned char)input[i];
        uint32_t octet_b = remaining > 1 ? (unsigned char)input[i + 1] : 0;
        uint32_t octet_c = remaining > 2 ? (unsigned char)input[i + 2] : 0;
        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;
        result.push_back(b64_chars[(triple >> 18) & 0x3F]);
        result.push_back(b64_chars[(triple >> 12) & 0x3F]);
        if (remaining > 1) result.push_back(b64_chars[(triple >> 6) & 0x3F]);
        if (remaining > 2) result.push_back(b64_chars[triple & 0x3F]);
    }
    return result;
}

std::string GenerateCodeVerifier() {
    std::vector<unsigned char> random_bytes(32);
#ifdef MCP_HAVE_OPENSSL
    if (RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1) {
        throw std::runtime_error("pkce: RAND_bytes failed");
    }
#else
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& b : random_bytes) b = static_cast<unsigned char>(dist(gen));
#endif
    return Base64UrlEncode(
        std::string_view(reinterpret_cast<const char*>(random_bytes.data()),
                         random_bytes.size()));
}

std::string ComputeCodeChallenge(std::string_view code_verifier) {
    auto hash = detail::Sha256::Hash(code_verifier);
    return Base64UrlEncode(hash);
}

} // namespace pkce

// ====================================================================
// TokenContainer
// ====================================================================
bool TokenContainer::IsExpired() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() >= expires_at;
}

bool TokenContainer::WillExpireSoon(int64_t margin_ms) const {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (now_ms + margin_ms) >= expires_at;
}

// ====================================================================
// InMemoryTokenCache
// ====================================================================
void InMemoryTokenCache::StoreTokens(const TokenContainer& tokens) {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_ = tokens;
}

std::optional<TokenContainer> InMemoryTokenCache::GetTokens() {
    std::lock_guard<std::mutex> lock(mutex_);
    return tokens_;
}

void InMemoryTokenCache::ClearTokens() {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_.reset();
}

// ====================================================================
// HTTP helper
// ====================================================================
JsonValue OAuthClientProvider::HttpPost(
    std::string_view url_str,
    const std::map<std::string, std::string>& form_data)
{
    std::string body;
    for (auto it = form_data.begin(); it != form_data.end(); ++it) {
        if (it != form_data.begin()) body += '&';
        body += UrlEncode(it->first) + '=' + UrlEncode(it->second);
    }

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    auto resp = HttpPostUrl(std::string(url_str), body, headers);
    if (!resp || resp->status_code < 200 || resp->status_code >= 300)
        return JsonValue();

    return JsonValue::Parse(resp->body);
}

// ====================================================================
// OAuthClientProvider
// ====================================================================
OAuthClientProvider::OAuthClientProvider(OAuthClientOptions options)
    : options_(std::move(options))
    , token_cache_(options_.token_cache
        ? options_.token_cache
        : std::make_shared<InMemoryTokenCache>())
{}

OAuthClientProvider::~OAuthClientProvider() = default;

bool OAuthClientProvider::Authenticate() {
    if (!DiscoverMetadata()) return false;

    if (!options_.client_id && metadata_->registration_endpoint) {
        if (!RegisterClient()) return false;
    }

    auto cached = token_cache_->GetTokens();
    if (cached && !cached->IsExpired()) return true;
    if (cached && cached->refresh_token.size() > 0) return RefreshTokens();

    return StartAuthorizationFlow();
}

bool OAuthClientProvider::AuthenticateClientCredentials() {
    if (!metadata_) {
        if (!DiscoverMetadata()) return false;
    }
    if (!options_.client_id || !options_.client_secret ||
        options_.client_secret->empty()) {
        MCP_LOG(Error, "client_credentials requires client_id and client_secret");
        return false;
    }
    std::map<std::string, std::string> form;
    form["grant_type"] = "client_credentials";
    form["client_id"] = *options_.client_id;
    form["client_secret"] = *options_.client_secret;
    for (auto& s : options_.scopes) {
        if (!form["scope"].empty()) form["scope"] += " ";
        form["scope"] += s;
    }

    auto json = HttpPost(metadata_->token_endpoint, form);
    if (json.IsNull()) return false;
    if (!ValidateTokenIssuer(json)) return false;
    return ParseAndStoreTokenResponse(json);
}

bool OAuthClientProvider::DiscoverMetadata() {
    // Try RFC 8414 well-known discovery first, fall back to hardcoded URLs
    if (auto discovered = OAuthMetadata::Discover(options_.server_url)) {
        metadata_ = std::move(discovered);
        if (metadata_->issuer.empty()) metadata_->issuer = options_.server_url;
        return true;
    }

    if (options_.server_url.empty()) {
        MCP_LOG(Error, "OAuth metadata discovery failed and server_url is empty");
        return false;
    }
    MCP_LOG(Warning, "OAuth metadata discovery failed; falling back to server_url defaults");
    OAuthMetadata meta;
    meta.issuer = options_.server_url;
    meta.authorization_endpoint = options_.server_url + "/authorize";
    meta.token_endpoint = options_.server_url + "/token";
    metadata_ = std::move(meta);
    return true;
}

bool OAuthClientProvider::ValidateTokenIssuer(const JsonValue& response) const {
    if (!metadata_) return false;
    auto* it = response.Find("iss");
    if (!it || !it->IsString()) {
        MCP_LOG(Error, "OAuth token response missing required 'iss' claim (RFC 9207)");
        return false;
    }
    if (it->GetString() != metadata_->issuer) {
        MCP_LOG(Error, "OAuth token response issuer mismatch");
        return false;
    }
    return true;
}

bool OAuthClientProvider::RegisterClient() {
    if (!metadata_->registration_endpoint) return false;
    auto info = ClientRegistrationInfo::Register(
        *metadata_->registration_endpoint,
        options_.redirect_uri, "mcp-cpp-client");
    if (!info) return false;
    registration_ = std::move(info);
    return true;
}

bool OAuthClientProvider::StartAuthorizationFlow() {
    code_verifier_ = pkce::GenerateCodeVerifier();
    code_challenge_ = pkce::ComputeCodeChallenge(code_verifier_);
    state_ = pkce::GenerateCodeVerifier();

    std::string auth_url = metadata_->authorization_endpoint;
    auth_url += "?response_type=code";
    auth_url += "&client_id=" + UrlEncode(options_.client_id.value_or(
        registration_ ? registration_->client_id : ""));
    auth_url += "&redirect_uri=" + UrlEncode(options_.redirect_uri);
    auth_url += "&code_challenge=" + code_challenge_;
    auth_url += "&code_challenge_method=S256";
    auth_url += "&state=" + UrlEncode(state_);
    if (!options_.scopes.empty()) {
        std::string scope_str;
        for (auto& s : options_.scopes) {
            if (!scope_str.empty()) scope_str += " ";
            scope_str += s;
        }
        auth_url += "&scope=" + UrlEncode(scope_str);
    }

    if (options_.authorization_redirect_handler)
        options_.authorization_redirect_handler(auth_url);
    if (!options_.authorization_code_callback) return false;
    auto callback_result = options_.authorization_code_callback();
    if (!callback_result) return false;
    // CSRF check: the state echoed back must match what we sent (RFC 6749 §10.12)
    if (callback_result->state != state_) return false;

    return ExchangeCodeForToken(callback_result->code, code_verifier_);
}

bool OAuthClientProvider::ExchangeCodeForToken(
    std::string_view code, std::string_view code_verifier)
{
    std::map<std::string, std::string> form;
    form["grant_type"] = "authorization_code";
    form["code"] = std::string(code);
    form["redirect_uri"] = options_.redirect_uri;
    form["client_id"] = options_.client_id.value_or(
        registration_ ? registration_->client_id : "");
    form["code_verifier"] = std::string(code_verifier);

    auto json = HttpPost(metadata_->token_endpoint, form);
    if (json.IsNull()) return false;
    if (!ValidateTokenIssuer(json)) return false;
    return ParseAndStoreTokenResponse(json);
}

bool OAuthClientProvider::ParseAndStoreTokenResponse(const JsonValue& json) {
    TokenContainer tokens;
    if (auto* v = json.Find("access_token")) tokens.access_token = v->GetString();
    if (auto* v = json.Find("refresh_token")) tokens.refresh_token = v->GetString();
    if (auto* v = json.Find("token_type")) tokens.token_type = v->GetString();
    auto expires_in = json.Find("expires_in") ? json.Find("expires_in")->GetInt() : 3600;
    tokens.expires_at =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() +
        expires_in * 1000;

    if (tokens.access_token.empty()) return false;
    token_cache_->StoreTokens(tokens);
    return true;
}

bool OAuthClientProvider::RefreshTokens() {
    auto cached = token_cache_->GetTokens();
    if (!cached || cached->refresh_token.empty()) return false;

    std::map<std::string, std::string> form;
    form["grant_type"] = "refresh_token";
    form["refresh_token"] = cached->refresh_token;
    form["client_id"] = options_.client_id.value_or(
        registration_ ? registration_->client_id : "");

    auto json = HttpPost(metadata_->token_endpoint, form);
    if (json.IsNull()) return false;
    if (!ValidateTokenIssuer(json)) return false;

    TokenContainer tokens;
    if (auto* v = json.Find("access_token")) tokens.access_token = v->GetString();
    // refresh_token is retained when absent (RFC 6749 non-rotating refresh tokens)
    tokens.refresh_token = json.Find("refresh_token") ? json.Find("refresh_token")->GetString() : cached->refresh_token;
    if (auto* v = json.Find("token_type")) tokens.token_type = v->GetString();
    auto expires_in = json.Find("expires_in") ? json.Find("expires_in")->GetInt() : 3600;
    tokens.expires_at =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() +
        expires_in * 1000;

    if (tokens.access_token.empty()) {
        MCP_LOG(Error, "OAuth refresh response missing access_token");
        return false;
    }
    token_cache_->StoreTokens(tokens);
    return true;
}

std::string OAuthClientProvider::GetAccessToken() {
    std::lock_guard<std::mutex> lock(refresh_mutex_);
    auto tokens = token_cache_->GetTokens();
    if (!tokens) return {};
    if (tokens->WillExpireSoon()) {
        if (!tokens->refresh_token.empty()) {
            if (!RefreshTokens()) {
                throw McpError(McpErrorCode::InternalError,
                    "OAuth token refresh failed");
            }
        }
        tokens = token_cache_->GetTokens();
    }
    return tokens ? tokens->access_token : std::string{};
}

void OAuthClientProvider::Revoke() {
    auto tokens = token_cache_->GetTokens();
    if (tokens && !tokens->access_token.empty() &&
        metadata_ && metadata_->revocation_endpoint) {
        std::map<std::string, std::string> form;
        form["token"] = tokens->access_token;
        // Best-effort server-side revocation (RFC 7009); the local cache is
        // cleared regardless of the HTTP result.
        HttpPost(*metadata_->revocation_endpoint, form);
    }
    token_cache_->ClearTokens();
}
bool OAuthClientProvider::IsAuthenticated() const {
    auto tokens = token_cache_->GetTokens();
    return tokens.has_value() && !tokens->IsExpired();
}
bool OAuthClientProvider::HasToken() const {
    auto tokens = token_cache_->GetTokens();
    return tokens.has_value() && !tokens->access_token.empty();
}
std::string OAuthClientProvider::GetAuthorizationHeader() const {
    auto tokens = token_cache_->GetTokens();
    if (!tokens || tokens->access_token.empty()) return {};
    return "Bearer " + tokens->access_token;
}

bool OAuthClientProvider::StepUpAuthorization(
    const std::vector<std::string>& additional_scopes)
{
    for (auto& s : additional_scopes) {
        if (std::find(options_.scopes.begin(), options_.scopes.end(), s)
            == options_.scopes.end()) {
            options_.scopes.push_back(s);
        }
    }
    Revoke();
    return StartAuthorizationFlow();
}

bool OAuthClientProvider::HandleAuthChallenge(std::string_view www_authenticate) {
    // RFC 9728: the challenge advertises "resource_metadata=\"<uri>\"".
    auto header = std::string(www_authenticate);
    auto pos = header.find("resource_metadata=");
    if (pos == std::string::npos) return false;
    pos = header.find('"', pos);
    if (pos == std::string::npos) return false;
    auto end = header.find('"', pos + 1);
    if (end == std::string::npos) return false;
    auto metadata_url = header.substr(pos + 1, end - pos - 1);

    auto resp = HttpGetUrl(metadata_url);
    if (!resp || resp->status_code < 200 || resp->status_code >= 300) return false;
    auto json = JsonValue::Parse(resp->body);
    if (!json.IsObject()) return false;

    auto meta = ParseMetadataJson(json);
    if (meta.issuer.empty() && meta.authorization_endpoint.empty() &&
        meta.token_endpoint.empty()) {
        return false;
    }
    VerifyResourceMatch(options_.server_url, meta.resource);
    if (!metadata_) metadata_ = OAuthMetadata{};
    if (!meta.issuer.empty()) metadata_->issuer = meta.issuer;
    if (!meta.authorization_endpoint.empty()) metadata_->authorization_endpoint = meta.authorization_endpoint;
    if (!meta.token_endpoint.empty()) metadata_->token_endpoint = meta.token_endpoint;
    if (meta.revocation_endpoint) metadata_->revocation_endpoint = meta.revocation_endpoint;
    if (meta.registration_endpoint) metadata_->registration_endpoint = meta.registration_endpoint;
    if (meta.jwks_uri) metadata_->jwks_uri = meta.jwks_uri;
    if (!meta.scopes_supported.empty()) metadata_->scopes_supported = meta.scopes_supported;
    if (!meta.response_types_supported.empty()) metadata_->response_types_supported = meta.response_types_supported;
    if (!meta.grant_types_supported.empty()) metadata_->grant_types_supported = meta.grant_types_supported;
    if (!meta.code_challenge_methods_supported.empty()) metadata_->code_challenge_methods_supported = meta.code_challenge_methods_supported;
    return true;
}

std::optional<OAuthMetadata> OAuthMetadata::Discover(
    std::string_view server_url)
{
    std::string url(server_url);
    while (!url.empty() && url.back() == '/') url.pop_back();
    std::string metadata_url = url + "/.well-known/oauth-authorization-server";

    auto resp = HttpGetUrl(metadata_url);
    if (resp && resp->status_code >= 200 && resp->status_code < 300) {
        auto json = JsonValue::Parse(resp->body);
        if (json.IsObject()) {
            return ParseMetadataJson(json);
        }
    }

    return std::nullopt;
}

std::optional<ClientRegistrationInfo> ClientRegistrationInfo::Register(
    std::string_view registration_endpoint,
    std::string_view redirect_uri,
    std::string_view client_name)
{
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array uris;
        uris.push_back(JsonValue(std::string(redirect_uri)));
        obj["redirect_uris"] = JsonValue(std::move(uris));
    }
    obj["client_name"] = JsonValue(std::string(client_name));

    auto body = JsonValue(std::move(obj)).Dump();

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";

    auto resp = HttpPostUrl(std::string(registration_endpoint), body, headers);
    if (!resp || resp->status_code < 200 || resp->status_code >= 300)
        return std::nullopt;

    auto json = JsonValue::Parse(resp->body);
    if (json.IsNull()) return std::nullopt;

    ClientRegistrationInfo info;
    if (auto* v = json.Find("client_id"))
        info.client_id = v->GetString();
    if (auto* v = json.Find("client_secret"))
        info.client_secret = v->GetString();
    if (auto* v = json.Find("grant_types")) {
        if (v->IsArray()) {
            for (const auto& item : v->GetArray())
                if (item.IsString()) info.grant_types.push_back(item.GetString());
        }
    }
    if (auto* v = json.Find("redirect_uris")) {
        if (v->IsArray()) {
            for (const auto& item : v->GetArray())
                if (item.IsString()) info.redirect_uris.push_back(item.GetString());
        }
    }

    if (info.client_id.empty()) return std::nullopt;
    return info;
}

} // namespace mcp
