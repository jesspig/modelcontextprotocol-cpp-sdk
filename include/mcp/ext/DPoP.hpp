#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

} // namespace mcp::ext::dpop
