#include <mcp/storage/FileTokenCache.hpp>
#include <mcp/Log.hpp>

#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
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
    if (!std::filesystem::exists(cache_path_)) return;

#ifdef _WIN32
    std::ifstream file(cache_path_, std::ios::binary);
    if (!file.is_open()) return;
    std::vector<BYTE> buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    auto json_str = UnprotectData(buf);

    if (json_str.empty()) {
        // Fallback: try plaintext JSON (migration from unencrypted cache)
        std::ifstream plain_file(cache_path_);
        if (!plain_file.is_open()) return;
        try {
            auto json = nlohmann::json::parse(plain_file, nullptr, false);
            if (json.is_discarded()) return;
            TokenContainer tc;
            tc.access_token = json.value("access_token", "");
            tc.refresh_token = json.value("refresh_token", "");
            tc.token_type = json.value("token_type", "Bearer");
            tc.expires_at = json.value("expires_at", 0LL);
            if (json.contains("scopes")) {
                tc.scopes = json["scopes"].get<std::vector<std::string>>();
            }
            tokens_ = std::move(tc);
        } catch (...) { MCP_LOG(Warning, "token cache fallback parse failed"); }
        return;
    }

    try {
        auto json = nlohmann::json::parse(json_str, nullptr, false);
        if (json.is_discarded()) return;
        TokenContainer tc;
        tc.access_token = json.value("access_token", "");
        tc.refresh_token = json.value("refresh_token", "");
        tc.token_type = json.value("token_type", "Bearer");
        tc.expires_at = json.value("expires_at", 0LL);
        if (json.contains("scopes")) {
            tc.scopes = json["scopes"].get<std::vector<std::string>>();
        }
        tokens_ = std::move(tc);
    } catch (...) {
        MCP_LOG(Warning, "token cache parse failed (Win32)");
    }
#else
    std::ifstream file(cache_path_);
    if (!file.is_open()) return;
    try {
        auto json = nlohmann::json::parse(file, nullptr, false);
        if (json.is_discarded()) return;
        TokenContainer tc;
        tc.access_token = json.value("access_token", "");
        tc.refresh_token = json.value("refresh_token", "");
        tc.token_type = json.value("token_type", "Bearer");
        tc.expires_at = json.value("expires_at", 0LL);
        if (json.contains("scopes")) {
            tc.scopes = json["scopes"].get<std::vector<std::string>>();
        }
        tokens_ = std::move(tc);
    } catch (...) {
        MCP_LOG(Warning, "token cache parse failed");
    }
    file.close();
    chmod(cache_path_.string().c_str(), 0600);
#endif
}

void FileTokenCache::Save() {
    if (!tokens_) {
        std::filesystem::remove(cache_path_);
        return;
    }
    nlohmann::json j;
    j["access_token"] = tokens_->access_token;
    j["refresh_token"] = tokens_->refresh_token;
    j["token_type"] = tokens_->token_type;
    j["expires_at"] = tokens_->expires_at;
    j["scopes"] = tokens_->scopes;
    auto tmp_path = cache_path_;
    tmp_path += ".tmp";
#ifdef _WIN32
    auto plaintext = j.dump();
    auto encrypted = ProtectData(plaintext);
    if (encrypted.empty()) {
        std::filesystem::remove(tmp_path);
        return;
    }
    {
        std::ofstream file(tmp_path, std::ios::binary);
        if (!file.is_open()) {
            std::filesystem::remove(tmp_path);
            return;
        }
        file.write(reinterpret_cast<const char*>(encrypted.data()), encrypted.size());
    }
    std::filesystem::rename(tmp_path, cache_path_);
#else
    {
        std::ofstream file(tmp_path);
        if (!file.is_open()) {
            std::filesystem::remove(tmp_path);
            return;
        }
        file << j.dump(2);
        file.close();
        chmod(tmp_path.string().c_str(), 0600);
    }
    std::filesystem::rename(tmp_path, cache_path_);
#endif
}

} // namespace mcp
