// FileTokenCache.cpp
// File-based OAuth token cache with DPAPI encryption (Windows) or plaintext storage (POSIX)
#include <mcp/JsonValue.hpp>
#include <mcp/storage/FileTokenCache.hpp>
#include <mcp/Log.hpp>
#include <mcp/detail/AtomicJsonFile.hpp>

#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <sys/stat.h>
#endif

namespace {

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

} // namespace

namespace mcp {

FileTokenCache::FileTokenCache(std::filesystem::path cache_path)
    : cache_path_(std::move(cache_path))
{
    Load();
}

FileTokenCache::~FileTokenCache() {
    Save();
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

void FileTokenCache::Load() {
    std::error_code ec;
    if (!std::filesystem::exists(cache_path_, ec)) return;

#ifdef _WIN32
    std::ifstream file(cache_path_, std::ios::binary);
    if (!file.is_open()) return;
    std::vector<BYTE> buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    auto json_str = UnprotectData(buf);

    if (json_str.empty()) {
        MCP_LOG(Error, "token cache: DPAPI decryption failed for " + cache_path_.string() + "; token cache ignored");
        return;
    }

    try {
        auto json = JsonValue::Parse(json_str);
        if (json.IsNull()) return;
        TokenContainer tc;
        if (auto* v = json.Find("access_token")) tc.access_token = v->GetString();
        if (auto* v = json.Find("refresh_token")) tc.refresh_token = v->GetString();
        if (auto* v = json.Find("token_type")) tc.token_type = v->GetString();
        if (auto* v = json.Find("expires_at")) tc.expires_at = v->GetInt();
        if (auto* v = json.Find("scopes"); v && v->IsArray()) {
            for (const auto& s : v->GetArray())
                tc.scopes.push_back(s.GetString());
        }
        tokens_ = std::move(tc);
    } catch (...) {
        MCP_LOG(Warning, "token cache parse failed (Win32)");
    }
#else
    std::ifstream file(cache_path_);
    if (!file.is_open()) return;
    try {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto json = JsonValue::Parse(content);
        if (json.IsNull()) return;
        TokenContainer tc;
        if (auto* v = json.Find("access_token")) tc.access_token = v->GetString();
        if (auto* v = json.Find("refresh_token")) tc.refresh_token = v->GetString();
        if (auto* v = json.Find("token_type")) tc.token_type = v->GetString();
        if (auto* v = json.Find("expires_at")) tc.expires_at = v->GetInt();
        if (auto* v = json.Find("scopes"); v && v->IsArray()) {
            for (const auto& s : v->GetArray())
                tc.scopes.push_back(s.GetString());
        }
        tokens_ = std::move(tc);
    } catch (...) {
        MCP_LOG(Warning, "token cache parse failed");
    }
    file.close();
    if (chmod(cache_path_.string().c_str(), 0600) != 0) {
        MCP_LOG(Error, "token cache: chmod failed for " + cache_path_.string());
    }
#endif
}

void FileTokenCache::Save() {
    if (!tokens_) {
        std::error_code ec;
        if (!std::filesystem::remove(cache_path_, ec) && ec) {
            MCP_LOG(Error, "token cache: failed to remove " + cache_path_.string());
        }
        return;
    }
    JsonValue::Object obj;
    obj["access_token"] = JsonValue(tokens_->access_token);
    obj["refresh_token"] = JsonValue(tokens_->refresh_token);
    obj["token_type"] = JsonValue(tokens_->token_type);
    obj["expires_at"] = JsonValue(static_cast<int64_t>(tokens_->expires_at));
    {
        JsonValue::Array scopes_arr;
        for (const auto& s : tokens_->scopes)
            scopes_arr.push_back(JsonValue(s));
        obj["scopes"] = JsonValue(std::move(scopes_arr));
    }
    auto dump_str = JsonValue(std::move(obj)).Dump(2);
#ifdef _WIN32
    auto plaintext = dump_str;
    auto encrypted = ProtectData(plaintext);
    if (encrypted.empty()) {
        MCP_LOG(Error, "token cache: CryptProtectData failed for " + cache_path_.string());
        return;
    }
    if (!detail::WriteAtomic(cache_path_,
            std::string(reinterpret_cast<const char*>(encrypted.data()), encrypted.size()))) {
        MCP_LOG(Error, "token cache: failed to write " + cache_path_.string());
    }
#else
    if (detail::WriteAtomic(cache_path_, dump_str)) {
        if (chmod(cache_path_.string().c_str(), 0600) != 0) {
            MCP_LOG(Error, "token cache: chmod failed for " + cache_path_.string());
        }
    }
#endif
}

} // namespace mcp
