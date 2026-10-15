#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mcp::ext::dpop {

struct EcP256Jwk {
    std::string x;
    std::string y;
    std::string d;
};

EcP256Jwk GenerateKey();

std::string ComputeThumbprint(std::string_view x, std::string_view y);

std::string PublicJwkJson(const EcP256Jwk& key);

std::string CreateProof(const EcP256Jwk& key,
                        std::string_view htm,
                        std::string_view htu,
                        std::optional<std::string_view> access_token = std::nullopt,
                        std::optional<std::string_view> nonce = std::nullopt,
                        std::optional<int64_t> iat_override_seconds = std::nullopt);

bool IsNonceFresh(int64_t iat_seconds, int64_t now_seconds, int64_t window_seconds = 300);

int64_t NowSeconds();

struct DPoPVerifyOptions {
    std::string htm;
    std::string htu;
    std::string access_token;
    std::optional<std::string> alternate_htu;
    std::optional<std::string> expected_nonce;
    int64_t window_seconds = 300;
    int64_t now_seconds = 0;
};

class JtiReplayCache {
public:
    explicit JtiReplayCache(int64_t window_seconds = 300);
    bool CheckAndStore(std::string_view jti, int64_t iat_seconds, int64_t now_seconds);
    void Clear();
private:
    int64_t window_seconds_;
    std::mutex mutex_;
    std::unordered_map<std::string, int64_t> seen_;
};

bool VerifyProof(std::string_view proof,
                 const DPoPVerifyOptions& options,
                 JtiReplayCache& replay);

std::string GenerateNonce(std::size_t random_bytes = 16);

namespace detail {

std::string SignEs256JwtWithPem(std::string_view header_json,
                                std::string_view payload_json,
                                std::string_view private_key_pem);

} // namespace detail

} // namespace mcp::ext::dpop
