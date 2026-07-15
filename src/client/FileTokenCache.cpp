#include <mcp/storage/FileTokenCache.hpp>

#include <filesystem>
#include <fstream>

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
        // Corrupted file; start fresh
    }
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
    std::ofstream file(cache_path_);
    if (file.is_open()) {
        file << j.dump(2);
    }
}

} // namespace mcp
