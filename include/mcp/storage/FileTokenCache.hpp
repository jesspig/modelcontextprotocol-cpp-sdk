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

    void StoreTokens(const TokenContainer& tokens) override;
    std::optional<TokenContainer> GetTokens() override;
    void ClearTokens() override;
    void StoreBoundIssuer(const std::string& issuer) override;
    std::optional<std::string> GetBoundIssuer() override;
    void StoreDpopKey(const ext::dpop::EcP256Jwk& key) override;
    std::optional<ext::dpop::EcP256Jwk> GetDpopKey() override;
    void ClearDpopKey() override;

private:
    void Load();
    void Save();

    std::filesystem::path cache_path_;
    std::optional<TokenContainer> tokens_;
    std::optional<ext::dpop::EcP256Jwk> dpop_key_;
    std::optional<JsonValue> rejected_dpop_key_json_;
    std::mutex mutex_;
};

} // namespace mcp
