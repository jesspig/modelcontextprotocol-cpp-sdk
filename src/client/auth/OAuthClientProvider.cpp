#include <mcp/client/auth/OAuthClientProvider.hpp>

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#ifdef MCP_HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/rand.h>
#else
#include <random>
#endif

#include <sstream>

namespace mcp {

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
        uint32_t octet_a = i < input.size() ? (unsigned char)input[i] : 0;
        uint32_t octet_b = i + 1 < input.size() ? (unsigned char)input[i + 1] : 0;
        uint32_t octet_c = i + 2 < input.size() ? (unsigned char)input[i + 2] : 0;
        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;
        result.push_back(b64_chars[(triple >> 18) & 0x3F]);
        result.push_back(b64_chars[(triple >> 12) & 0x3F]);
        result.push_back(b64_chars[(triple >> 6) & 0x3F]);
        result.push_back(b64_chars[triple & 0x3F]);
    }
    auto pad = result.find_last_not_of('=');
    if (pad != std::string::npos) result.resize(pad + 1);
    return result;
}

std::string GenerateCodeVerifier() {
#ifdef MCP_HAVE_OPENSSL
    std::vector<unsigned char> random_bytes(32);
    RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size()));
#else
    std::vector<unsigned char> random_bytes(32);
    std::random_device rd;
    for (auto& b : random_bytes) b = static_cast<unsigned char>(rd());
#endif
    return Base64UrlEncode(
        std::string_view(reinterpret_cast<const char*>(random_bytes.data()),
                         random_bytes.size()));
}

std::string ComputeCodeChallenge(std::string_view code_verifier) {
#ifdef MCP_HAVE_OPENSSL
    std::vector<unsigned char> hash(EVP_MAX_MD_SIZE);
    unsigned int hash_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, code_verifier.data(), code_verifier.size());
    EVP_DigestFinal_ex(ctx, hash.data(), &hash_len);
    EVP_MD_CTX_free(ctx);
    return Base64UrlEncode(
        std::string_view(reinterpret_cast<const char*>(hash.data()), hash_len));
#else
    // Without OpenSSL, return the raw verifier as challenge (S256 not available)
    // This is NOT cryptographically secure — for production, install OpenSSL
    return Base64UrlEncode(code_verifier);
#endif
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
// HTTP helper — minimal asio-based POST for OAuth endpoints
// ====================================================================
nlohmann::json OAuthClientProvider::HttpPost(
    std::string_view url_str,
    const std::map<std::string, std::string>& form_data)
{
    auto protocol_end = url_str.find("://");
    if (protocol_end == std::string::npos) return {};
    auto host_start = protocol_end + 3;
    auto path_start = url_str.find('/', host_start);
    auto host = std::string(
        url_str.substr(host_start, path_start - host_start));
    auto path = std::string(path_start != std::string::npos
        ? url_str.substr(path_start) : std::string_view("/"));

    asio::io_context io_ctx;
    asio::ip::tcp::resolver resolver(io_ctx);
    asio::ip::tcp::socket socket(io_ctx);

    try {
        auto endpoints = resolver.resolve(host, "80");
        asio::connect(socket, endpoints);

        std::string body;
        for (auto& [key, val] : form_data) {
            if (!body.empty()) body += "&";
            body += key + "=" + val;
        }

        std::string request =
            "POST " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + body;

        asio::write(socket, asio::buffer(request));

        std::string response;
        asio::error_code ec;
        while (true) {
            char buf[4096];
            size_t len = socket.read_some(asio::buffer(buf), ec);
            if (ec) break;
            response.append(buf, len);
        }

        auto body_start = response.find("\r\n\r\n");
        if (body_start == std::string::npos) return {};
        auto json_str = response.substr(body_start + 4);
        return nlohmann::json::parse(json_str, nullptr, false);
    } catch (...) {
        return {};
    }
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

bool OAuthClientProvider::DiscoverMetadata() {
    OAuthMetadata meta;
    meta.authorization_endpoint = options_.server_url + "/authorize";
    meta.token_endpoint = options_.server_url + "/token";
    metadata_ = std::move(meta);
    return true;
}

bool OAuthClientProvider::ValidateTokenIssuer(const nlohmann::json& response) const {
    if (!metadata_) return false;
    auto it = response.find("iss");
    if (it == response.end()) {
        return true;
    }
    return it->get<std::string>() == metadata_->issuer;
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

    std::string auth_url = metadata_->authorization_endpoint;
    auth_url += "?response_type=code";
    auth_url += "&client_id=" + (options_.client_id.value_or(
        registration_ ? registration_->client_id : ""));
    auth_url += "&redirect_uri=" + options_.redirect_uri;
    auth_url += "&code_challenge=" + code_challenge_;
    auth_url += "&code_challenge_method=S256";
    if (!options_.scopes.empty()) {
        std::string scope_str;
        for (auto& s : options_.scopes) {
            if (!scope_str.empty()) scope_str += " ";
            scope_str += s;
        }
        auth_url += "&scope=" + scope_str;
    }

    if (options_.authorization_redirect_handler)
        options_.authorization_redirect_handler(auth_url);
    if (!options_.authorization_code_callback) return false;
    auto code = options_.authorization_code_callback();
    if (!code) return false;

    return ExchangeCodeForToken(*code, code_verifier_);
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
    if (json.is_discarded()) return false;
    if (!ValidateTokenIssuer(json)) return false;

    TokenContainer tokens;
    tokens.access_token = json.value("access_token", "");
    tokens.refresh_token = json.value("refresh_token", "");
    tokens.token_type = json.value("token_type", "Bearer");
    tokens.expires_at =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() +
        json.value("expires_in", 3600) * 1000;

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
    if (json.is_discarded()) return false;
    if (!ValidateTokenIssuer(json)) return false;

    TokenContainer tokens;
    tokens.access_token = json.value("access_token", cached->access_token);
    tokens.refresh_token = json.value("refresh_token", cached->refresh_token);
    tokens.token_type = json.value("token_type", "Bearer");
    tokens.expires_at =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() +
        json.value("expires_in", 3600) * 1000;

    token_cache_->StoreTokens(tokens);
    return true;
}

std::string OAuthClientProvider::GetAccessToken() {
    auto tokens = token_cache_->GetTokens();
    if (!tokens) return {};
    if (tokens->WillExpireSoon()) {
        if (!tokens->refresh_token.empty()) RefreshTokens();
        tokens = token_cache_->GetTokens();
    }
    return tokens ? tokens->access_token : std::string{};
}

void OAuthClientProvider::Revoke() { token_cache_->ClearTokens(); }
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

std::optional<OAuthMetadata> OAuthMetadata::Discover(
    std::string_view server_url)
{
    OAuthMetadata meta;
    meta.issuer = std::string(server_url);
    meta.authorization_endpoint = std::string(server_url) + "/authorize";
    meta.token_endpoint = std::string(server_url) + "/token";
    return meta;
}

std::optional<ClientRegistrationInfo> ClientRegistrationInfo::Register(
    std::string_view, std::string_view redirect_uri, std::string_view client_name)
{
    ClientRegistrationInfo info;
    info.client_id = std::string(client_name) + "-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count());
    info.redirect_uris = {std::string(redirect_uri)};
    return info;
}

} // namespace mcp
