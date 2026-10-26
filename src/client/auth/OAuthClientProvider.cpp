#include <mcp/client/auth/OAuthClientProvider.hpp>
#include <detail/JsonFields.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/detail/Base64Url.hpp>
#include <mcp/detail/sha256.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include <chrono>
#ifdef MCP_HAVE_OPENSSL
#include <openssl/rand.h>
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

#include <algorithm>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace {

constexpr std::string_view kClientAssertionTypeJwtBearer =
    "urn:ietf:params:oauth:client-assertion-type:jwt-bearer";
constexpr std::string_view kPrivateKeyJwtAuthMethod = "private_key_jwt";
constexpr std::string_view kEs256SigningAlgorithm = "ES256";
constexpr std::string_view kEs256JwtHeader = R"({"alg":"ES256","typ":"JWT"})";
constexpr int64_t kClientAssertionLifetimeSeconds = 60;
constexpr std::size_t kClientAssertionJtiBytes = 16;

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

std::string NormalizeResourceUrl(std::string_view url) {
    std::string out(url);
    if (auto hash = out.find('#'); hash != std::string::npos) out.resize(hash);
    if (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

std::string DpopHtu(std::string_view url) {
    std::string out(url);
    if (auto hash = out.find('#'); hash != std::string::npos) out.resize(hash);
    if (auto query = out.find('?'); query != std::string::npos) out.resize(query);
    return out;
}

bool IsSafeHttpUrl(std::string_view url) {
    if (url.empty() || url.size() > 2048) return false;
    bool http = url.size() >= 7 && url.substr(0, 7) == "http://";
    bool https = url.size() >= 8 && url.substr(0, 8) == "https://";
    if (!http && !https) return false;
    for (char c : url) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) return false;
    }
    return true;
}

std::string ToLowerAscii(std::string_view input) {
    std::string out(input);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

struct UrlParts {
    std::string scheme;
    std::string host;
    std::string port;
};

std::optional<UrlParts> ParseUrl(std::string_view url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return std::nullopt;
    UrlParts parts;
    parts.scheme = ToLowerAscii(url.substr(0, scheme_end));
    if (parts.scheme != "http" && parts.scheme != "https") return std::nullopt;
    auto rest = url.substr(scheme_end + 3);
    auto authority_end = rest.find_first_of("/?#");
    std::string authority(rest.substr(0, authority_end));
    auto at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (authority.empty()) return std::nullopt;
    if (authority.front() == '[') {
        auto close = authority.find(']');
        if (close == std::string::npos) return std::nullopt;
        parts.host = ToLowerAscii(authority.substr(1, close - 1));
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return std::nullopt;
            parts.port = authority.substr(close + 2);
        }
    } else {
        auto colon = authority.find(':');
        if (colon == std::string::npos) {
            parts.host = ToLowerAscii(authority);
        } else {
            parts.host = ToLowerAscii(authority.substr(0, colon));
            parts.port = authority.substr(colon + 1);
        }
    }
    if (parts.host.empty()) return std::nullopt;
    for (char c : parts.port) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    return parts;
}

std::string EffectivePort(const UrlParts& parts) {
    if (!parts.port.empty()) return parts.port;
    return parts.scheme == "https" ? "443" : "80";
}

std::string UrlOrigin(std::string_view url) {
    auto parts = ParseUrl(url);
    if (!parts) return {};
    return parts->scheme + "://" + parts->host + ":" + EffectivePort(*parts);
}

bool IsLoopbackHost(std::string_view host) {
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

bool IsSecureEndpointUrl(std::string_view url) {
    if (!IsSafeHttpUrl(url)) return false;
    auto parts = ParseUrl(url);
    if (!parts) return false;
    if (parts->scheme == "https") return true;
    return IsLoopbackHost(parts->host);
}

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

class RefreshInFlightGuard {
public:
    RefreshInFlightGuard(std::mutex& mutex,
                        bool& in_flight,
                        std::thread::id& owner,
                        std::condition_variable& finished)
        : mutex_(mutex)
        , in_flight_(in_flight)
        , owner_(owner)
        , finished_(finished) {}

    ~RefreshInFlightGuard() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            in_flight_ = false;
            owner_ = std::thread::id{};
        }
        finished_.notify_all();
    }

    RefreshInFlightGuard(const RefreshInFlightGuard&) = delete;
    RefreshInFlightGuard& operator=(const RefreshInFlightGuard&) = delete;

private:
    std::mutex& mutex_;
    bool& in_flight_;
    std::thread::id& owner_;
    std::condition_variable& finished_;
};

} // anonymous namespace

namespace mcp {

namespace {

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
    extract_vec("token_endpoint_auth_methods_supported",
                meta.token_endpoint_auth_methods_supported);

    return meta;
}

void VerifyResourceMatch(const std::string& requested_url,
                         const std::optional<std::string>& resource)
{
    if (!resource || resource->empty()) return;
    if (NormalizeResourceUrl(*resource) == NormalizeResourceUrl(requested_url)) return;
    auto scheme_end = requested_url.find("://");
    if (scheme_end != std::string::npos) {
        auto host_end = requested_url.find_first_of("/?#", scheme_end + 3);
        std::string host_root = host_end == std::string::npos
            ? requested_url : requested_url.substr(0, host_end);
        if (NormalizeResourceUrl(*resource) == NormalizeResourceUrl(host_root)) return;
    }
    throw McpError(McpErrorCode::InternalError,
        "OAuth resource_metadata resource does not match the request URL");
}

bool IsClientIdMetadataDocumentUrl(std::string_view client_id) {
    bool http = client_id.size() >= 7 && client_id.substr(0, 7) == "http://";
    bool https = client_id.size() >= 8 && client_id.substr(0, 8) == "https://";
    return http || https;
}

} // anonymous namespace

namespace pkce {

std::string Base64UrlEncode(std::string_view input) {
    return detail::Base64UrlEncode(input);
}

std::string GenerateCodeVerifier() {
    std::vector<unsigned char> random_bytes(32);
#ifdef MCP_HAVE_OPENSSL
    if (RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1) {
        throw std::runtime_error("pkce: RAND_bytes failed");
    }
#elif defined(_WIN32)
    if (BCryptGenRandom(nullptr, random_bytes.data(),
            static_cast<ULONG>(random_bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("pkce: BCryptGenRandom failed");
    }
#else
    std::random_device rd;
    for (auto& b : random_bytes) {
        b = static_cast<unsigned char>(rd() & 0xFF);
    }
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

bool TokenContainer::IsExpired() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() >= expires_at;
}

bool TokenContainer::WillExpireSoon(int64_t margin_ms) const {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (now_ms + margin_ms) >= expires_at;
}

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

void InMemoryTokenCache::StoreBoundIssuer(const std::string& issuer) {
    std::lock_guard<std::mutex> lock(mutex_);
    bound_issuer_ = issuer;
}

std::optional<std::string> InMemoryTokenCache::GetBoundIssuer() {
    std::lock_guard<std::mutex> lock(mutex_);
    return bound_issuer_;
}

OAuthClientProvider::TokenEndpointResponse OAuthClientProvider::HttpPost(
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
    if (options_.enable_dpop) {
        headers["DPoP"] = BuildDPoPProof("POST", std::string(url_str));
    }

    TokenEndpointResponse response;
    auto resp = HttpPostUrl(std::string(url_str), body, headers);
    if (!resp) return response;
    response.transport_ok = true;
    response.status_code = resp->status_code;
    try {
        response.json = JsonValue::Parse(resp->body);
    } catch (...) {
        response.json = JsonValue();
    }
    if (response.json.IsObject()) {
        if (auto* error = response.json.Find("error");
            error && error->IsString() && !error->GetString().empty()) {
            response.error_code = error->GetString();
        }
    }
    return response;
}

OAuthClientProvider::OAuthClientProvider(OAuthClientOptions options)
    : options_(std::move(options))
    , token_cache_(options_.token_cache
        ? options_.token_cache
        : std::make_shared<InMemoryTokenCache>())
{}

OAuthClientProvider::~OAuthClientProvider() = default;

bool OAuthClientProvider::Authenticate() {
    if (!DiscoverMetadata()) return false;

    if (options_.client_id && IsClientIdMetadataDocumentUrl(*options_.client_id)) {
        FetchClientMetadataDocument();
    } else if (!options_.client_id) {
        auto meta = MetadataSnapshot();
        if (meta && meta->registration_endpoint) {
            if (!RegisterClient()) return false;
        }
    }

    auto cached = token_cache_->GetTokens();
    if (cached && !TokenResourceMatches(*cached)) {
        token_cache_->ClearTokens();
        cached.reset();
    }
    if (cached && !cached->IsExpired()) return true;
    if (cached && cached->refresh_token.size() > 0) {
        auto outcome = RefreshTokensInternal();
        if (outcome == RefreshOutcome::Refreshed) return true;
        if (outcome == RefreshOutcome::InvalidGrant) return StartAuthorizationFlow();
        return false;
    }

    return StartAuthorizationFlow();
}

bool OAuthClientProvider::AuthenticateClientCredentials() {
    if (!EnsureMetadata()) return false;
    auto meta = MetadataSnapshot();
    if (!meta) return false;
    if (!options_.client_id || options_.client_id->empty()) {
        MCP_LOG(Error, "client_credentials requires client_id");
        return false;
    }
    std::map<std::string, std::string> form;
    form["grant_type"] = "client_credentials";
    form["client_id"] = *options_.client_id;
    for (auto& s : ResolveScopes()) {
        if (!form["scope"].empty()) form["scope"] += " ";
        form["scope"] += s;
    }
    if (auto resource = ResolveResourceIndicator(); !resource.empty())
        form[detail::kResource] = std::move(resource);
    if (UsesClientAssertion()) {
        ApplyClientAuthentication(form);
    } else {
        if (!options_.client_secret || options_.client_secret->empty()) {
            MCP_LOG(Error, "client_credentials requires client_secret or an advertised private_key_jwt assertion");
            return false;
        }
        form["client_secret"] = *options_.client_secret;
    }

    auto response = HttpPost(meta->token_endpoint, form);
    if (!response.Succeeded() || response.json.IsNull()) return false;
    if (!ValidateTokenIssuer(response.json)) return false;
    return ParseAndStoreTokenResponse(response.json);
}

bool OAuthClientProvider::DiscoverMetadata() {
    std::optional<OAuthMetadata> discovered = OAuthMetadata::Discover(options_.server_url);
    bool from_discovery = discovered && !discovered->issuer.empty();
    OAuthMetadata meta;
    if (from_discovery) {
        meta = std::move(*discovered);
    } else {
        if (discovered) {
            MCP_LOG(Warning, "OAuth metadata discovery response missing required 'issuer'; falling back to server_url defaults");
        }
        if (options_.server_url.empty()) {
            MCP_LOG(Error, "OAuth metadata discovery failed and server_url is empty");
            return false;
        }
        MCP_LOG(Warning, "OAuth metadata discovery failed; falling back to server_url defaults");
        meta.issuer = options_.server_url;
        meta.authorization_endpoint = options_.server_url + "/authorize";
        meta.token_endpoint = options_.server_url + "/token";
    }
    ValidateMetadataEndpoints(meta);

    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (from_discovery && !IssuerBindingAllows(meta.issuer)) {
        MCP_LOG(Error, "OAuth issuer mismatch: refusing to bind to an authorization server "
            "other than the pinned one");
        return false;
    }
    metadata_ = std::move(meta);
    if (from_discovery) EnforceIssuerBinding();
    return true;
}

bool OAuthClientProvider::EnsureMetadata() {
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex_);
        if (metadata_) return true;
    }
    return DiscoverMetadata();
}

std::optional<OAuthMetadata> OAuthClientProvider::MetadataSnapshot() const {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    return metadata_;
}

std::optional<ClientRegistrationInfo> OAuthClientProvider::RegistrationSnapshot() const {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    return registration_;
}

bool OAuthClientProvider::TokenResourceMatches(const TokenContainer& tokens) const {
    if (!tokens.resource || tokens.resource->empty()) return true;
    return NormalizeResourceUrl(*tokens.resource) ==
        NormalizeResourceUrl(ResolveResourceIndicator());
}

bool OAuthClientProvider::IssuerBindingAllows(const std::string& issuer) const {
    if (issuer.empty()) return true;
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (bound_issuer_ && !bound_issuer_->empty() && *bound_issuer_ != issuer) return false;
    auto persisted = token_cache_->GetBoundIssuer();
    if (!persisted || persisted->empty()) return true;
    return *persisted == issuer;
}

void OAuthClientProvider::EnforceIssuerBinding() {
    if (!metadata_ || metadata_->issuer.empty()) return;
    auto persisted = token_cache_->GetBoundIssuer();
    if (!persisted || persisted->empty()) {
        token_cache_->StoreBoundIssuer(metadata_->issuer);
    }
    bound_issuer_ = metadata_->issuer;
}

void OAuthClientProvider::RequireSecureEndpoint(
    std::string_view url, std::string_view kind) const
{
    if (IsSecureEndpointUrl(url)) return;
    MCP_LOG(Error, "OAuth " + std::string(kind) +
        " endpoint rejected: https is required (loopback hosts excepted)");
    throw McpError(McpErrorCode::InvalidParams,
        std::string("OAuth ") + std::string(kind) +
        " endpoint must use https (loopback hosts excepted)");
}

void OAuthClientProvider::ValidateMetadataEndpoints(const OAuthMetadata& meta) const {
    if (!meta.authorization_endpoint.empty())
        RequireSecureEndpoint(meta.authorization_endpoint, "authorization");
    if (!meta.token_endpoint.empty())
        RequireSecureEndpoint(meta.token_endpoint, "token");
    if (meta.revocation_endpoint && !meta.revocation_endpoint->empty())
        RequireSecureEndpoint(*meta.revocation_endpoint, "revocation");
    if (meta.registration_endpoint && !meta.registration_endpoint->empty())
        RequireSecureEndpoint(*meta.registration_endpoint, "registration");
}

bool OAuthClientProvider::IsAllowedAuthorizationServer(const OAuthMetadata& meta) const {
    std::string anchor = meta.issuer.empty() ? meta.token_endpoint : meta.issuer;
    if (anchor.empty()) return false;
    std::string anchor_origin = UrlOrigin(anchor);
    if (anchor_origin.empty()) return false;

    std::string server_origin = UrlOrigin(options_.server_url);
    bool allowed = !server_origin.empty() && anchor_origin == server_origin;
    if (!allowed) {
        for (const auto& candidate : options_.allowed_authorization_server_origins) {
            if (!candidate.empty() && UrlOrigin(candidate) == anchor_origin) {
                allowed = true;
                break;
            }
        }
    }
    if (!allowed) return false;

    std::string issuer_origin = meta.issuer.empty() ? anchor_origin : UrlOrigin(meta.issuer);
    if (issuer_origin.empty()) return false;
    if (!meta.authorization_endpoint.empty() &&
        UrlOrigin(meta.authorization_endpoint) != issuer_origin) {
        return false;
    }
    if (!meta.token_endpoint.empty() &&
        UrlOrigin(meta.token_endpoint) != issuer_origin) {
        return false;
    }
    return true;
}

bool OAuthClientProvider::IsSecureEndpoint(std::string_view url) {
    return IsSecureEndpointUrl(url);
}

bool OAuthClientProvider::IsSameOrigin(std::string_view lhs, std::string_view rhs) {
    std::string left = UrlOrigin(lhs);
    std::string right = UrlOrigin(rhs);
    return !left.empty() && left == right;
}

bool OAuthClientProvider::ValidateTokenIssuer(const JsonValue& response) const {
    auto meta = MetadataSnapshot();
    if (!meta) return false;
    auto* it = response.Find("iss");
    if (!it) {
        MCP_LOG(Warning,
            "OAuth token response carries no 'iss' claim; accepted per local policy (RFC 9207 leaves it optional)");
        return true;
    }
    if (!it->IsString()) {
        MCP_LOG(Error, "OAuth token response 'iss' claim is present but not a string");
        return false;
    }
    if (it->GetString() != meta->issuer) {
        MCP_LOG(Error, "OAuth token response issuer mismatch");
        return false;
    }
    return true;
}

bool OAuthClientProvider::CodeChallengeS256Supported() const {
    auto meta = MetadataSnapshot();
    if (!meta) return true;
    if (meta->code_challenge_methods_supported.empty()) return true;
    return std::find(meta->code_challenge_methods_supported.begin(),
        meta->code_challenge_methods_supported.end(), "S256")
        != meta->code_challenge_methods_supported.end();
}

void OAuthClientProvider::NotifyTokenUpdate(const TokenContainer& tokens) const {
    auto handler = options_.token_update_handler;
    if (!handler) return;
    try {
        handler(tokens);
    } catch (...) {
    }
}

std::string OAuthClientProvider::ResolveResourceIndicator() const {
    if (options_.resource && !options_.resource->empty()) return *options_.resource;
    auto meta = MetadataSnapshot();
    if (meta && meta->resource && !meta->resource->empty())
        return *meta->resource;
    return options_.server_url;
}

std::vector<std::string> OAuthClientProvider::ResolveScopes() const {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (!options_.scope_filter) return options_.scopes;
    return options_.scope_filter(options_.scopes);
}

bool OAuthClientProvider::FetchClientMetadataDocument() {
    if (!options_.client_id) return false;
    if (!IsSafeHttpUrl(*options_.client_id)) {
        MCP_LOG(Warning, "CIMD: client_id is not a safe http URL; using configured client_id");
        return false;
    }

    auto resp = HttpGetUrl(*options_.client_id);
    if (!resp || resp->status_code < 200 || resp->status_code >= 300) {
        MCP_LOG(Warning, "CIMD: client metadata document unavailable; using configured client_id");
        return false;
    }
    if (resp->body.size() > mcp::detail::kMaxDcrBytes) {
        MCP_LOG(Warning, "CIMD: client metadata document exceeds size limit; using configured client_id");
        return false;
    }
    auto json = JsonValue::Parse(resp->body);
    if (!json.IsObject()) return false;

    if (auto* v = json.Find("client_id")) {
        if (v->IsString() && v->GetString() != *options_.client_id) {
            MCP_LOG(Error, "CIMD: document client_id does not match the requested URL");
            return false;
        }
    }

    ClientRegistrationInfo info;
    info.client_id = *options_.client_id;
    if (auto* v = json.Find("token_endpoint_auth_method")) {
        if (v->IsString() && !v->GetString().empty())
            info.token_endpoint_auth_method = v->GetString();
    }
    if (auto* v = json.Find("grant_types")) {
        if (v->IsArray()) {
            for (const auto& item : v->GetArray())
                if (item.IsString()) info.grant_types.push_back(item.GetString());
            if (info.grant_types.size() > 128) {
                MCP_LOG(Warning, "CIMD: client metadata document lists too many grant_types; using configured client_id");
                return false;
            }
        }
    }
    if (auto* v = json.Find("redirect_uris")) {
        if (v->IsArray()) {
            for (const auto& item : v->GetArray())
                if (item.IsString()) info.redirect_uris.push_back(item.GetString());
            if (info.redirect_uris.size() > 128) {
                MCP_LOG(Warning, "CIMD: client metadata document lists too many redirect_uris; using configured client_id");
                return false;
            }
            if (!options_.redirect_uri.empty() && !info.redirect_uris.empty() &&
                std::find(info.redirect_uris.begin(), info.redirect_uris.end(),
                    options_.redirect_uri) == info.redirect_uris.end()) {
                MCP_LOG(Error, "CIMD: configured redirect_uri is not listed by the client metadata document");
                return false;
            }
        }
    }
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    registration_ = std::move(info);
    return true;
}

bool OAuthClientProvider::PrivateKeyJwtAdvertised() const {
    auto meta = MetadataSnapshot();
    if (!meta) return false;
    return std::find(meta->token_endpoint_auth_methods_supported.begin(),
        meta->token_endpoint_auth_methods_supported.end(),
        std::string(kPrivateKeyJwtAuthMethod))
        != meta->token_endpoint_auth_methods_supported.end();
}

bool OAuthClientProvider::UsesClientAssertion() const {
    if (!options_.private_key_pem || options_.private_key_pem->empty()) return false;
    return PrivateKeyJwtAdvertised();
}

std::string OAuthClientProvider::CreateClientAssertion() const {
    if (options_.signing_algorithm != std::string(kEs256SigningAlgorithm)) {
        throw McpError(McpErrorCode::InvalidParams,
            "OAuth client assertion supports only the ES256 signing algorithm");
    }
    auto registration = RegistrationSnapshot();
    std::string client_id = options_.client_id.value_or(
        registration ? registration->client_id : std::string());
    if (client_id.empty()) {
        throw McpError(McpErrorCode::InvalidParams,
            "OAuth private_key_jwt client assertion requires a client_id");
    }
    auto meta = MetadataSnapshot();
    std::string audience;
    if (meta) audience = meta->issuer;
    if (audience.empty() && meta) audience = meta->token_endpoint;
    if (audience.empty()) {
        throw McpError(McpErrorCode::InvalidParams,
            "OAuth private_key_jwt client assertion requires an issuer audience");
    }
    if (!options_.private_key_pem || options_.private_key_pem->empty()) {
        throw McpError(McpErrorCode::InvalidParams,
            "OAuth private_key_jwt client assertion requires private_key_pem");
    }
    int64_t now_seconds = options_.client_assertion_clock_seconds
        ? options_.client_assertion_clock_seconds()
        : ext::dpop::NowSeconds();
    JsonValue payload(JsonValue::object_tag);
    payload["iss"] = JsonValue(client_id);
    payload["sub"] = JsonValue(client_id);
    payload["aud"] = JsonValue(audience);
    payload["jti"] = JsonValue(ext::dpop::GenerateNonce(kClientAssertionJtiBytes));
    payload["iat"] = JsonValue(now_seconds);
    payload["exp"] = JsonValue(now_seconds + kClientAssertionLifetimeSeconds);
    return ext::dpop::detail::SignEs256JwtWithPem(
        kEs256JwtHeader, JsonValue(std::move(payload)).Dump(), *options_.private_key_pem);
}

void OAuthClientProvider::ApplyClientAuthentication(
    std::map<std::string, std::string>& form) const
{
    if (!UsesClientAssertion()) return;
    form["client_assertion_type"] = std::string(kClientAssertionTypeJwtBearer);
    form["client_assertion"] = CreateClientAssertion();
}

bool OAuthClientProvider::RegisterClient() {
    auto meta = MetadataSnapshot();
    if (!meta || !meta->registration_endpoint) return false;
    RequireSecureEndpoint(*meta->registration_endpoint, "registration");
    std::string_view application_type = options_.application_type.empty()
        ? std::string_view{"native"}
        : std::string_view{options_.application_type};
    auto info = ClientRegistrationInfo::Register(
        *meta->registration_endpoint,
        options_.redirect_uri, "mcp-cpp-client", application_type);
    if (!info) return false;
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    registration_ = std::move(info);
    return true;
}

bool OAuthClientProvider::StartAuthorizationFlow() {
    std::lock_guard<std::mutex> flow_lock(auth_flow_mutex_);
    if (!EnsureMetadata()) return false;
    auto meta = MetadataSnapshot();
    if (!meta) return false;
    if (!CodeChallengeS256Supported()) return false;
    std::string code_verifier = pkce::GenerateCodeVerifier();
    std::string code_challenge = pkce::ComputeCodeChallenge(code_verifier);
    std::string state = pkce::GenerateCodeVerifier();

    std::string auth_url = meta->authorization_endpoint;
    auth_url += "?response_type=code";
    auto registration = RegistrationSnapshot();
    auth_url += "&client_id=" + UrlEncode(options_.client_id.value_or(
        registration ? registration->client_id : ""));
    auth_url += "&redirect_uri=" + UrlEncode(options_.redirect_uri);
    auth_url += "&code_challenge=" + code_challenge;
    auth_url += "&code_challenge_method=S256";
    auth_url += "&state=" + UrlEncode(state);
    const auto scopes = ResolveScopes();
    if (!scopes.empty()) {
        std::string scope_str;
        for (auto& s : scopes) {
            if (!scope_str.empty()) scope_str += " ";
            scope_str += s;
        }
        auth_url += "&scope=" + UrlEncode(scope_str);
    }
    if (auto resource = ResolveResourceIndicator(); !resource.empty())
        auth_url += "&resource=" + UrlEncode(resource);

    if (options_.authorization_redirect_handler)
        options_.authorization_redirect_handler(auth_url);
    if (!options_.authorization_code_callback) return false;
    auto callback_result = options_.authorization_code_callback();
    if (!callback_result) return false;
    if (callback_result->state != state) return false;
    if (callback_result->iss && *callback_result->iss != meta->issuer) {
        MCP_LOG(Error, "OAuth authorization response issuer mismatch");
        return false;
    }

    return ExchangeCodeForToken(callback_result->code, code_verifier);
}

bool OAuthClientProvider::ExchangeCodeForToken(
    std::string_view code, std::string_view code_verifier)
{
    if (code.empty() || code_verifier.empty()) return false;
    auto meta = MetadataSnapshot();
    if (!meta) return false;
    std::map<std::string, std::string> form;
    form["grant_type"] = "authorization_code";
    form["code"] = std::string(code);
    form["redirect_uri"] = options_.redirect_uri;
    auto registration = RegistrationSnapshot();
    form["client_id"] = options_.client_id.value_or(
        registration ? registration->client_id : "");
    form["code_verifier"] = std::string(code_verifier);
    if (auto resource = ResolveResourceIndicator(); !resource.empty())
        form[detail::kResource] = std::move(resource);
    ApplyClientAuthentication(form);

    auto response = HttpPost(meta->token_endpoint, form);
    if (!response.Succeeded() || response.json.IsNull()) return false;
    if (!ValidateTokenIssuer(response.json)) return false;
    return ParseAndStoreTokenResponse(response.json);
}

bool OAuthClientProvider::ParseAndStoreTokenResponse(const JsonValue& json) {
    TokenContainer tokens;
    if (auto* v = json.Find("access_token")) tokens.access_token = v->GetString();
    if (auto* v = json.Find("refresh_token")) tokens.refresh_token = v->GetString();
    if (auto* v = json.Find("token_type")) tokens.token_type = v->GetString();
    if (auto* v = json.Find("scope"); v && v->IsString()) {
        const auto& scope_str = v->GetString();
        size_t pos = 0;
        while (pos < scope_str.size()) {
            while (pos < scope_str.size() && scope_str[pos] == ' ') ++pos;
            size_t end = pos;
            while (end < scope_str.size() && scope_str[end] != ' ') ++end;
            if (end > pos) tokens.scopes.emplace_back(scope_str.substr(pos, end - pos));
            pos = end;
        }
    }
    if (auto indicator = ResolveResourceIndicator(); !indicator.empty())
        tokens.resource = std::move(indicator);
    auto expires_in = json.Find("expires_in") ? json.Find("expires_in")->GetInt() : 3600;
    tokens.expires_at =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() +
        expires_in * 1000;

    if (tokens.access_token.empty()) return false;
    token_cache_->StoreTokens(tokens);
    NotifyTokenUpdate(tokens);
    return true;
}

bool OAuthClientProvider::RefreshTokens() {
    return RefreshTokensInternal() == RefreshOutcome::Refreshed;
}

OAuthClientProvider::RefreshOutcome OAuthClientProvider::RefreshTokensInternal() {
    auto cached = token_cache_->GetTokens();
    if (!cached || cached->refresh_token.empty()) return RefreshOutcome::TransientFailure;
    if (!EnsureMetadata()) return RefreshOutcome::TransientFailure;
    auto meta = MetadataSnapshot();
    if (!meta) return RefreshOutcome::TransientFailure;

    std::map<std::string, std::string> form;
    form["grant_type"] = "refresh_token";
    form["refresh_token"] = cached->refresh_token;
    auto registration = RegistrationSnapshot();
    form["client_id"] = options_.client_id.value_or(
        registration ? registration->client_id : "");
    if (auto resource = ResolveResourceIndicator(); !resource.empty())
        form[detail::kResource] = std::move(resource);
    ApplyClientAuthentication(form);

    auto response = HttpPost(meta->token_endpoint, form);
    if (!response.Succeeded()) {
        if (response.transport_ok && response.IsCredentialRejection()) {
            MCP_LOG(Error, "OAuth refresh rejected by the authorization server: " +
                response.error_code);
            token_cache_->ClearTokens();
            return RefreshOutcome::InvalidGrant;
        }
        MCP_LOG(Error, "OAuth token refresh request failed (status " +
            std::to_string(response.status_code) + ")");
        return RefreshOutcome::TransientFailure;
    }
    if (response.json.IsNull()) return RefreshOutcome::TransientFailure;
    if (!ValidateTokenIssuer(response.json)) return RefreshOutcome::TransientFailure;

    TokenContainer tokens;
    if (auto* v = response.json.Find("access_token")) tokens.access_token = v->GetString();
    tokens.refresh_token = response.json.Find("refresh_token")
        ? response.json.Find("refresh_token")->GetString() : cached->refresh_token;
    if (auto* v = response.json.Find("token_type")) tokens.token_type = v->GetString();
    if (auto* v = response.json.Find("scope"); v && v->IsString()) {
        const auto& scope_str = v->GetString();
        size_t pos = 0;
        while (pos < scope_str.size()) {
            while (pos < scope_str.size() && scope_str[pos] == ' ') ++pos;
            size_t end = pos;
            while (end < scope_str.size() && scope_str[end] != ' ') ++end;
            if (end > pos) tokens.scopes.emplace_back(scope_str.substr(pos, end - pos));
            pos = end;
        }
    } else {
        tokens.scopes = cached->scopes;
    }
    if (auto indicator = ResolveResourceIndicator(); !indicator.empty())
        tokens.resource = std::move(indicator);
    else
        tokens.resource = cached->resource;
    auto expires_in = response.json.Find("expires_in")
        ? response.json.Find("expires_in")->GetInt() : 3600;
    tokens.expires_at =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() +
        expires_in * 1000;

    if (tokens.access_token.empty()) {
        MCP_LOG(Error, "OAuth refresh response missing access_token");
        return RefreshOutcome::TransientFailure;
    }
    token_cache_->StoreTokens(tokens);
    NotifyTokenUpdate(tokens);
    return RefreshOutcome::Refreshed;
}

void OAuthClientProvider::RefreshTokensCoalesced() {
    for (;;) {
        std::unique_lock<std::mutex> lock(refresh_mutex_);
        if (!refresh_in_flight_) {
            refresh_in_flight_ = true;
            refresh_owner_ = std::this_thread::get_id();
            RefreshInFlightGuard guard(refresh_mutex_, refresh_in_flight_,
                                       refresh_owner_, refresh_cv_);
            lock.unlock();
            if (!RefreshTokens())
                throw McpError(McpErrorCode::InternalError,
                    "OAuth token refresh failed");
            return;
        }
        if (refresh_owner_ == std::this_thread::get_id()) {
            MCP_LOG(Warning,
                "OAuth token refresh re-entered on the refreshing thread; serving the pending token without waiting");
            return;
        }
        refresh_cv_.wait(lock);
        lock.unlock();
        auto tokens = token_cache_->GetTokens();
        if (tokens && TokenResourceMatches(*tokens) && !tokens->WillExpireSoon())
            return;
    }
}

std::string OAuthClientProvider::GetAccessToken() {
    auto tokens = token_cache_->GetTokens();
    if (!tokens) return {};
    if (!TokenResourceMatches(*tokens)) return {};
    if (!tokens->WillExpireSoon() || tokens->refresh_token.empty())
        return tokens->access_token;
    RefreshTokensCoalesced();
    tokens = token_cache_->GetTokens();
    return tokens ? tokens->access_token : std::string{};
}

void OAuthClientProvider::Revoke() {
    auto tokens = token_cache_->GetTokens();
    auto meta = MetadataSnapshot();
    if (tokens && !tokens->access_token.empty() &&
        meta && meta->revocation_endpoint && !meta->revocation_endpoint->empty()) {
        RequireSecureEndpoint(*meta->revocation_endpoint, "revocation");
        std::map<std::string, std::string> form;
        form["token"] = tokens->access_token;
        HttpPost(*meta->revocation_endpoint, form);
    }
    token_cache_->ClearTokens();
}
bool OAuthClientProvider::IsAuthenticated() const {
    auto tokens = token_cache_->GetTokens();
    return tokens.has_value() && !tokens->IsExpired() && TokenResourceMatches(*tokens);
}
bool OAuthClientProvider::HasToken() const {
    auto tokens = token_cache_->GetTokens();
    return tokens.has_value() && !tokens->access_token.empty() && TokenResourceMatches(*tokens);
}
std::string OAuthClientProvider::GetAuthorizationHeader() const {
    auto tokens = token_cache_->GetTokens();
    if (!tokens || tokens->access_token.empty()) return {};
    if (!TokenResourceMatches(*tokens)) return {};
    if (options_.enable_dpop) return "DPoP " + tokens->access_token;
    return "Bearer " + tokens->access_token;
}

std::string OAuthClientProvider::BuildDPoPProof(
    std::string_view htm,
    std::string_view htu,
    std::optional<std::string_view> access_token) {
    if (!options_.enable_dpop) return {};
    std::lock_guard<std::mutex> lock(dpop_mutex_);
    if (!dpop_key_) dpop_key_ = ext::dpop::GenerateKey();
    std::optional<std::string_view> nonce;
    if (dpop_nonce_ && !dpop_nonce_->empty()) nonce = *dpop_nonce_;
    return ext::dpop::CreateProof(*dpop_key_, htm, htu, access_token, nonce);
}

void OAuthClientProvider::SetDPoPNonce(std::string nonce) {
    std::lock_guard<std::mutex> lock(dpop_mutex_);
    dpop_nonce_ = std::move(nonce);
}

void OAuthClientProvider::ApplyDPoPProofHeader(
    const HttpRequestHeaderContext& context,
    std::map<std::string, std::string>& headers)
{
    if (!options_.enable_dpop) return;
    if (context.http_method.empty()) return;
    auto htu = DpopHtu(context.url);
    if (htu.empty()) return;
    std::optional<std::string_view> access_token;
    auto tokens = token_cache_->GetTokens();
    if (tokens && !tokens->access_token.empty() && TokenResourceMatches(*tokens)) {
        access_token = tokens->access_token;
    }
    std::string proof = BuildDPoPProof(context.http_method, htu, access_token);
    if (proof.empty()) return;
    headers["DPoP"] = std::move(proof);
}

void OAuthClientProvider::ObserveDPoPNonce(std::string_view dpop_nonce) {
    if (!options_.enable_dpop) return;
    if (dpop_nonce.empty() || dpop_nonce.size() > 1024) return;
    SetDPoPNonce(std::string(dpop_nonce));
}

bool OAuthClientProvider::StepUpAuthorization(
    const std::vector<std::string>& additional_scopes)
{
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex_);
        for (auto& s : additional_scopes) {
            if (std::find(options_.scopes.begin(), options_.scopes.end(), s)
                == options_.scopes.end()) {
                options_.scopes.push_back(s);
            }
        }
    }
    Revoke();
    return StartAuthorizationFlow();
}

bool OAuthClientProvider::HandleAuthChallenge(std::string_view www_authenticate) {
    try {
        if (www_authenticate.size() > 4096) return false;
        auto header = std::string(www_authenticate);
        auto pos = header.find("resource_metadata=");
        if (pos == std::string::npos) return false;
        pos = header.find('"', pos);
        if (pos == std::string::npos) return false;
        auto end = header.find('"', pos + 1);
        if (end == std::string::npos || end == pos + 1 || end - pos - 1 > 2048)
            return false;
        auto metadata_url = header.substr(pos + 1, end - pos - 1);
        if (!IsSecureEndpointUrl(metadata_url)) return false;

        auto resp = HttpGetUrl(metadata_url);
        if (!resp || resp->status_code < 200 || resp->status_code >= 300) return false;
        if (resp->body.size() > 65536) return false;
        JsonValue json;
        try {
            json = JsonValue::Parse(resp->body);
        } catch (...) {
            return false;
        }
        if (!json.IsObject()) return false;

        auto meta = ParseMetadataJson(json);
        if (meta.issuer.empty() && meta.authorization_endpoint.empty() &&
            meta.token_endpoint.empty()) {
            return false;
        }
        ValidateMetadataEndpoints(meta);
        try {
            VerifyResourceMatch(options_.server_url, meta.resource);
        } catch (...) {
            return false;
        }
        if (!IsAllowedAuthorizationServer(meta)) {
            MCP_LOG(Error, "OAuth authorization challenge rejected: the authorization "
                "server is not same-origin with the MCP server and is not allow-listed");
            return false;
        }
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            if (!meta.issuer.empty() && !IssuerBindingAllows(meta.issuer)) {
                MCP_LOG(Error, "OAuth authorization challenge issuer mismatch; "
                    "challenge rejected");
                return false;
            }
            if (!metadata_) metadata_ = OAuthMetadata{};
            if (!meta.issuer.empty()) metadata_->issuer = meta.issuer;
            if (!meta.authorization_endpoint.empty()) metadata_->authorization_endpoint = meta.authorization_endpoint;
            if (!meta.token_endpoint.empty()) metadata_->token_endpoint = meta.token_endpoint;
            if (meta.revocation_endpoint && IsSafeHttpUrl(*meta.revocation_endpoint))
                metadata_->revocation_endpoint = meta.revocation_endpoint;
            if (meta.registration_endpoint && IsSafeHttpUrl(*meta.registration_endpoint))
                metadata_->registration_endpoint = meta.registration_endpoint;
            if (meta.jwks_uri && IsSafeHttpUrl(*meta.jwks_uri))
                metadata_->jwks_uri = meta.jwks_uri;
            if (!meta.scopes_supported.empty() && meta.scopes_supported.size() <= 128)
                metadata_->scopes_supported = meta.scopes_supported;
            if (!meta.response_types_supported.empty() && meta.response_types_supported.size() <= 128)
                metadata_->response_types_supported = meta.response_types_supported;
            if (!meta.grant_types_supported.empty() && meta.grant_types_supported.size() <= 128)
                metadata_->grant_types_supported = meta.grant_types_supported;
            if (!meta.code_challenge_methods_supported.empty() &&
                meta.code_challenge_methods_supported.size() <= 8)
                metadata_->code_challenge_methods_supported = meta.code_challenge_methods_supported;
            if (!meta.token_endpoint_auth_methods_supported.empty() &&
                meta.token_endpoint_auth_methods_supported.size() <= 128)
                metadata_->token_endpoint_auth_methods_supported =
                    meta.token_endpoint_auth_methods_supported;
            EnforceIssuerBinding();
        }
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

std::optional<JsonValue> FetchMetadataDocument(const std::string& url) {
    if (!IsSafeHttpUrl(url)) return std::nullopt;
    auto resp = HttpGetUrl(url);
    if (!resp || resp->status_code < 200 || resp->status_code >= 300) return std::nullopt;
    if (resp->body.size() > 65536) return std::nullopt;
    try {
        JsonValue json = JsonValue::Parse(resp->body);
        if (!json.IsObject()) return std::nullopt;
        return json;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> DiscoverAuthorizationServerUrl(const JsonValue& prm) {
    auto* servers = prm.Find("authorization_servers");
    if (!servers || !servers->IsArray()) return std::nullopt;
    for (const auto& item : servers->GetArray()) {
        if (item.IsString() && IsSafeHttpUrl(item.GetString()))
            return item.GetString();
    }
    return std::nullopt;
}

} // anonymous namespace

std::optional<OAuthMetadata> OAuthMetadata::Discover(
    std::string_view server_url)
{
    std::string url(server_url);
    while (!url.empty() && url.back() == '/') url.pop_back();

    if (auto prm = FetchMetadataDocument(url + "/.well-known/oauth-protected-resource")) {
        if (auto as_url = DiscoverAuthorizationServerUrl(*prm)) {
            std::string as_base(*as_url);
            while (!as_base.empty() && as_base.back() == '/') as_base.pop_back();
            const std::string suffix("/.well-known/oauth-authorization-server");
            std::string as_doc_url = as_base.size() >= suffix.size() &&
                as_base.compare(as_base.size() - suffix.size(), suffix.size(), suffix) == 0
                ? as_base : as_base + suffix;
            if (auto as_doc = FetchMetadataDocument(as_doc_url)) {
                auto meta = ParseMetadataJson(*as_doc);
                if (meta.issuer.empty() && meta.authorization_endpoint.empty() &&
                    meta.token_endpoint.empty()) {
                    MCP_LOG(Warning, "OAuth authorization server metadata resolved via protected-resource metadata is empty; falling back to RFC8414 discovery");
                } else {
                    if (!meta.resource || meta.resource->empty()) {
                        if (auto* r = prm->Find("resource")) {
                            if (r->IsString() && !r->GetString().empty())
                                meta.resource = r->GetString();
                        }
                    }
                    return meta;
                }
            } else {
                MCP_LOG(Warning, "OAuth authorization server metadata unreachable via protected-resource metadata; falling back to RFC8414 discovery");
            }
        } else {
            MCP_LOG(Warning, "OAuth protected-resource metadata lists no usable authorization_servers entry; falling back to RFC8414 discovery");
        }
    }

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
    std::string_view client_name,
    std::string_view application_type)
{
    if (!IsSecureEndpointUrl(registration_endpoint)) {
        MCP_LOG(Error, "OAuth registration endpoint rejected: https is required "
            "(loopback hosts excepted)");
        throw McpError(McpErrorCode::InvalidParams,
            "OAuth registration endpoint must use https (loopback hosts excepted)");
    }

    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array uris;
        uris.push_back(JsonValue(std::string(redirect_uri)));
        obj["redirect_uris"] = JsonValue(std::move(uris));
    }
    obj["client_name"] = JsonValue(std::string(client_name));
    obj["application_type"] = JsonValue(
        std::string(application_type.empty() ? "native" : application_type));

    auto body = JsonValue(std::move(obj)).Dump();

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";

    auto resp = HttpPostUrl(std::string(registration_endpoint), body, headers);
    if (!resp || resp->status_code < 200 || resp->status_code >= 300)
        return std::nullopt;
    if (resp->body.size() > mcp::detail::kMaxDcrBytes)
        return std::nullopt;

    auto json = JsonValue::Parse(resp->body);
    if (json.IsNull()) return std::nullopt;

    ClientRegistrationInfo info;
    if (auto* v = json.Find("client_id"))
        info.client_id = v->GetString();
    if (auto* v = json.Find("client_secret"))
        info.client_secret = v->GetString();
    if (auto* v = json.Find("token_endpoint_auth_method")) {
        if (v->IsString() && !v->GetString().empty())
            info.token_endpoint_auth_method = v->GetString();
    }
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
