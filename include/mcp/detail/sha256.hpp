#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace mcp { namespace detail {

// Minimal SHA-256 implementation — standalone, no OpenSSL required.
// Based on the FIPS 180-4 specification.
class Sha256 {
public:
    Sha256() { Reset(); }

    void Update(std::string_view data) {
        for (auto c : data) {
            buf_[buf_len_++] = static_cast<uint8_t>(c);
            if (buf_len_ == 64) {
                ProcessBlock();
                total_bits_ += 512;
                buf_len_ = 0;
            }
        }
    }

    std::array<uint8_t, 32> Final() {
        uint64_t bits = total_bits_ + buf_len_ * 8;
        buf_[buf_len_++] = 0x80;
        if (buf_len_ > 56) {
            while (buf_len_ < 64) buf_[buf_len_++] = 0;
            ProcessBlock();
            buf_len_ = 0;
        }
        while (buf_len_ < 56) buf_[buf_len_++] = 0;
        for (int i = 7; i >= 0; --i) {
            buf_[buf_len_++] = static_cast<uint8_t>(bits >> (i * 8));
        }
        ProcessBlock();

        std::array<uint8_t, 32> result;
        for (int i = 0; i < 8; ++i) {
            result[i * 4]     = static_cast<uint8_t>(state_[i] >> 24);
            result[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            result[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            result[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        return result;
    }

    static std::string Hash(std::string_view data) {
        Sha256 sha;
        sha.Update(data);
        auto hash = sha.Final();
        return std::string(reinterpret_cast<const char*>(hash.data()), hash.size());
    }

private:
    void Reset() {
        state_ = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };
        buf_len_ = 0;
        total_bits_ = 0;
    }

    static uint32_t Rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    static uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    static uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    static uint32_t Sig0(uint32_t x) { return Rotr(x, 2) ^ Rotr(x, 13) ^ Rotr(x, 22); }
    static uint32_t Sig1(uint32_t x) { return Rotr(x, 6) ^ Rotr(x, 11) ^ Rotr(x, 25); }
    static uint32_t Sig2(uint32_t x) { return Rotr(x, 7) ^ Rotr(x, 18) ^ (x >> 3); }
    static uint32_t Sig3(uint32_t x) { return Rotr(x, 17) ^ Rotr(x, 19) ^ (x >> 10); }

    void ProcessBlock() {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
            0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(buf_[i * 4]) << 24)
                 | (static_cast<uint32_t>(buf_[i * 4 + 1]) << 16)
                 | (static_cast<uint32_t>(buf_[i * 4 + 2]) << 8)
                 | static_cast<uint32_t>(buf_[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = Sig3(w[i - 2]) + w[i - 7] + Sig2(w[i - 15]) + w[i - 16];
        }

        auto s = state_;
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = s[7] + Sig1(s[4]) + Ch(s[4], s[5], s[6]) + k[i] + w[i];
            uint32_t t2 = Sig0(s[0]) + Maj(s[0], s[1], s[2]);
            s[7] = s[6]; s[6] = s[5]; s[5] = s[4];
            s[4] = s[3] + t1;
            s[3] = s[2]; s[2] = s[1]; s[1] = s[0];
            s[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i) state_[i] += s[i];
    }

    std::array<uint32_t, 8> state_;
    uint8_t buf_[64]{};
    size_t buf_len_{};
    uint64_t total_bits_{};
};

}} // namespace mcp::detail
