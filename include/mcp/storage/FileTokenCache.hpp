// FileTokenCache.hpp - OAuth token cache persisted to file

#pragma once

#include <mcp/client/auth/TokenCache.hpp>

#include <filesystem>
#include <mutex>
#include <string>

namespace mcp {

class FileTokenCache : public ITokenCache {
public:
    explicit FileTokenCache(std::filesystem::path cache_path);
    ~FileTokenCache() override;

    // ITokenCache interface
    void StoreTokens(const TokenContainer& tokens) override;
    std::optional<TokenContainer> GetTokens() override;
    void ClearTokens() override;

private:
    void Load();
    void Save();

    std::filesystem::path cache_path_;
    std::optional<TokenContainer> tokens_;
    std::mutex mutex_;
};

} // namespace mcp
