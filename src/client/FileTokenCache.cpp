#include <mcp/JsonValue.hpp>
#include <mcp/storage/FileTokenCache.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>
#include <mcp/detail/AtomicJsonFile.hpp>

#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr size_t kMaxIssuerAnchorBytes = 4096;
constexpr size_t kMaxCoordinateBytes = 128;

#ifdef _WIN32
std::vector<BYTE> ProtectData(const std::string& data) {
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(data.data()));
    input.cbData = static_cast<DWORD>(data.size());
    DATA_BLOB output;
    if (CryptProtectData(&input, L"MCP Token Cache", nullptr, nullptr, nullptr, 0, &output)) {
        std::vector<BYTE> result(output.pbData, output.pbData + output.cbData);
        LocalFree(output.pbData);
        return result;
    }
    return {};
}

std::string UnprotectData(const std::vector<BYTE>& data) {
    DATA_BLOB input;
    input.pbData = const_cast<BYTE*>(data.data());
    input.cbData = static_cast<DWORD>(data.size());
    DATA_BLOB output;
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);
        return result;
    }
    return {};
}
#endif

std::filesystem::path IssuerAnchorPath(const std::filesystem::path& cache_path) {
    auto anchor = cache_path;
    anchor += ".issuer";
    return anchor;
}

#ifndef _WIN32
bool ValidateTokenCachePermissions(const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        MCP_LOG(Error, "token cache: cannot stat " + path.string());
        return false;
    }
    if (info.st_uid != ::getuid()) {
        MCP_LOG(Error, "token cache: refusing to load " + path.string() +
            " because it is owned by another user");
        return false;
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        MCP_LOG(Error, "token cache: refusing to load " + path.string() +
            " because it is readable by group or others");
        return false;
    }
    return true;
}
#endif

bool IsSafeCacheFile(const std::filesystem::path& path) {
    std::error_code ec;
    auto status = std::filesystem::symlink_status(path, ec);
    if (ec || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
        return false;
    }
#ifdef _WIN32
    DWORD attributes = ::GetFileAttributesW(path.wstring().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
#else
    if (!ValidateTokenCachePermissions(path)) return false;
#endif
    return true;
}

std::optional<std::string> ReadIssuerAnchor(const std::filesystem::path& anchor) {
    if (!IsSafeCacheFile(anchor)) {
        MCP_LOG(Error, "issuer anchor: rejected unsafe file type or permissions");
        return std::nullopt;
    }
    std::error_code ec;
    auto size = std::filesystem::file_size(anchor, ec);
    if (ec || size == 0 || size > kMaxIssuerAnchorBytes) {
        MCP_LOG(Error, "issuer anchor: rejected unreadable or oversized file");
        return std::nullopt;
    }
    std::ifstream file(anchor, std::ios::binary);
    if (!file.is_open()) {
        MCP_LOG(Error, "issuer anchor: failed to open file");
        return std::nullopt;
    }
    std::string content(static_cast<size_t>(size), '\0');
    file.read(content.data(), static_cast<std::streamsize>(size));
    if (static_cast<size_t>(file.gcount()) != content.size()) {
        MCP_LOG(Error, "issuer anchor: short read");
        return std::nullopt;
    }
    for (char c : content) {
        auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            MCP_LOG(Error, "issuer anchor: rejected control character");
            return std::nullopt;
        }
    }
    return content;
}

std::optional<mcp::ext::dpop::EcP256Jwk> ParseDpopKey(const mcp::JsonValue& json) {
    auto* field = json.Find("dpop_key");
    if (!field || !field->IsObject()) return std::nullopt;
    auto* x = field->Find("x");
    auto* y = field->Find("y");
    auto* d = field->Find("d");
    if (!x || !y || !d || !x->IsString() || !y->IsString() || !d->IsString()) {
        return std::nullopt;
    }
    if (x->GetString().size() > kMaxCoordinateBytes ||
        y->GetString().size() > kMaxCoordinateBytes ||
        d->GetString().size() > kMaxCoordinateBytes) {
        MCP_LOG(Error, "token cache: rejected oversized DPoP key coordinates");
        return std::nullopt;
    }
    mcp::ext::dpop::EcP256Jwk key{x->GetString(), y->GetString(), d->GetString()};
    if (!mcp::ext::dpop::ValidateKey(key)) {
        MCP_LOG(Error, "token cache: rejected DPoP key with invalid curve or key pair");
        return std::nullopt;
    }
    return key;
}

} // namespace

namespace mcp {

FileTokenCache::FileTokenCache(std::filesystem::path cache_path)
    : cache_path_(std::move(cache_path))
{
    Load();
}

FileTokenCache::~FileTokenCache() {
    try {
        Save();
    } catch (const std::exception& e) {
        MCP_LOG(Error, std::string("token cache: failed to persist tokens: ") + e.what());
    }
}

void FileTokenCache::StoreTokens(const TokenContainer& tokens) {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_ = tokens;
    Save();
}

std::optional<TokenContainer> FileTokenCache::GetTokens() {
    std::lock_guard<std::mutex> lock(mutex_);
    return tokens_;
}

void FileTokenCache::ClearTokens() {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_.reset();
    Save();
}

void FileTokenCache::StoreBoundIssuer(const std::string& issuer) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto anchor = IssuerAnchorPath(cache_path_);
    if (issuer.empty()) {
        std::error_code ec;
        std::filesystem::remove(anchor, ec);
        return;
    }
    try {
        (void)detail::WriteAtomicSecure(anchor, issuer);
    } catch (const std::exception& e) {
        MCP_LOG(Error, "token cache: failed to persist the issuer anchor: " +
            std::string(e.what()));
    }
}

std::optional<std::string> FileTokenCache::GetBoundIssuer() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadIssuerAnchor(IssuerAnchorPath(cache_path_));
}

void FileTokenCache::StoreDpopKey(const ext::dpop::EcP256Jwk& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rejected_dpop_key_json_) return;
    if (!ext::dpop::ValidateKey(key)) return;
    dpop_key_ = key;
    Save();
}

std::optional<ext::dpop::EcP256Jwk> FileTokenCache::GetDpopKey() {
    std::lock_guard<std::mutex> lock(mutex_);
    return dpop_key_;
}

void FileTokenCache::ClearDpopKey() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rejected_dpop_key_json_) return;
    dpop_key_.reset();
    Save();
}

void FileTokenCache::Load() {
    std::error_code ec;
    if (!std::filesystem::exists(cache_path_, ec)) return;

    std::string json_str;
#ifdef _WIN32
    std::ifstream file(cache_path_, std::ios::binary);
    if (!file.is_open()) return;
    std::vector<BYTE> buf((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    json_str = UnprotectData(buf);
    if (json_str.empty()) {
        MCP_LOG(Error, "token cache: DPAPI decryption failed; cache ignored");
        return;
    }
#else
    if (!ValidateTokenCachePermissions(cache_path_)) return;
    std::ifstream file(cache_path_, std::ios::binary);
    if (!file.is_open()) return;
    json_str.assign(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
#endif

    try {
        auto json = JsonValue::Parse(json_str);
        if (json.IsNull()) return;
        TokenContainer tc;
        if (auto* v = json.Find("access_token")) tc.access_token = v->GetString();
        if (auto* v = json.Find("refresh_token")) tc.refresh_token = v->GetString();
        if (auto* v = json.Find("token_type")) tc.token_type = v->GetString();
        if (auto* v = json.Find("expires_at")) tc.expires_at = v->GetInt();
        if (auto* v = json.Find("resource"); v && v->IsString() && !v->GetString().empty())
            tc.resource = v->GetString();
        if (auto* v = json.Find("scopes"); v && v->IsArray()) {
            for (const auto& s : v->GetArray())
                tc.scopes.push_back(s.GetString());
        }
        tokens_ = std::move(tc);
        if (auto key = ParseDpopKey(json)) {
            dpop_key_ = std::move(key);
        } else if (auto* field = json.Find("dpop_key"); field && field->IsObject()) {
            rejected_dpop_key_json_ = *field;
        }
    } catch (...) {
        MCP_LOG(Warning, "token cache parse failed; cache ignored");
    }
}

void FileTokenCache::Save() {
    if (!tokens_ && !dpop_key_ && !rejected_dpop_key_json_) {
        std::error_code ec;
        if (!std::filesystem::remove(cache_path_, ec) && ec) {
            MCP_LOG(Error, "token cache: failed to remove " + cache_path_.string());
        }
        return;
    }
    JsonValue::Object obj;
    if (tokens_) {
        obj["access_token"] = JsonValue(tokens_->access_token);
        obj["refresh_token"] = JsonValue(tokens_->refresh_token);
        obj["token_type"] = JsonValue(tokens_->token_type);
        obj["expires_at"] = JsonValue(static_cast<int64_t>(tokens_->expires_at));
        if (tokens_->resource && !tokens_->resource->empty())
            obj["resource"] = JsonValue(*tokens_->resource);
        JsonValue::Array scopes_arr;
        for (const auto& s : tokens_->scopes)
            scopes_arr.push_back(JsonValue(s));
        obj["scopes"] = JsonValue(std::move(scopes_arr));
    }
    if (rejected_dpop_key_json_) {
        obj["dpop_key"] = *rejected_dpop_key_json_;
    } else if (dpop_key_) {
        JsonValue::Object key_obj;
        key_obj["x"] = JsonValue(dpop_key_->x);
        key_obj["y"] = JsonValue(dpop_key_->y);
        key_obj["d"] = JsonValue(dpop_key_->d);
        obj["dpop_key"] = JsonValue(std::move(key_obj));
    }
    auto dump_str = JsonValue(std::move(obj)).Dump(2);
#ifdef _WIN32
    auto plaintext = dump_str;
    auto encrypted = ProtectData(plaintext);
    if (encrypted.empty()) {
        MCP_LOG(Error, "token cache: CryptProtectData failed for " + cache_path_.string());
        return;
    }
    (void)detail::WriteAtomicSecure(cache_path_,
        std::string(reinterpret_cast<const char*>(encrypted.data()), encrypted.size()));
#else
    (void)detail::WriteAtomicSecure(cache_path_, dump_str);
#endif
}

} // namespace mcp
