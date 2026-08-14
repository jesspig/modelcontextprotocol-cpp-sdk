#pragma once

// Sha1.hpp — WebSocket 握手用的 SHA-1、base64 与 CSPRNG 工具

#include <algorithm>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef MCP_HAVE_OPENSSL
#include <openssl/rand.h>
#endif

namespace mcp { namespace detail { namespace net {

namespace {

inline uint32_t RotateLeft(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

} // anonymous namespace

inline std::string Sha1Raw(std::string_view data) {
    std::vector<uint8_t> buffer(data.begin(), data.end());
    buffer.push_back(0x80);
    while (buffer.size() % 64 != 56)
        buffer.push_back(0x00);
    uint64_t bit_length = static_cast<uint64_t>(data.size()) * 8;
    for (int i = 7; i >= 0; --i)
        buffer.push_back(static_cast<uint8_t>((bit_length >> (i * 8)) & 0xFF));

    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    for (std::size_t offset = 0; offset < buffer.size(); offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            std::size_t p = offset + static_cast<std::size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(buffer[p]) << 24) |
                   (static_cast<uint32_t>(buffer[p + 1]) << 16) |
                   (static_cast<uint32_t>(buffer[p + 2]) << 8) |
                   static_cast<uint32_t>(buffer[p + 3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0];
        uint32_t b = h[1];
        uint32_t c = h[2];
        uint32_t d = h[3];
        uint32_t e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = RotateLeft(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = RotateLeft(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::string out(20, '\0');
    for (int i = 0; i < 5; ++i) {
        std::size_t p = static_cast<std::size_t>(i) * 4;
        out[p] = static_cast<char>((h[i] >> 24) & 0xFF);
        out[p + 1] = static_cast<char>((h[i] >> 16) & 0xFF);
        out[p + 2] = static_cast<char>((h[i] >> 8) & 0xFF);
        out[p + 3] = static_cast<char>(h[i] & 0xFF);
    }
    return out;
}

inline std::string Sha1Hex(std::string_view data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string raw = Sha1Raw(data);
    std::string out;
    out.reserve(40);
    for (char c : raw) {
        unsigned char u = static_cast<unsigned char>(c);
        out.push_back(kHex[u >> 4]);
        out.push_back(kHex[u & 0x0F]);
    }
    return out;
}

inline std::string Base64Encode(std::string_view data) {
    static constexpr char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
                     (static_cast<uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8) |
                     static_cast<uint32_t>(static_cast<unsigned char>(data[i + 2]));
        out.push_back(kBase64[(v >> 18) & 0x3F]);
        out.push_back(kBase64[(v >> 12) & 0x3F]);
        out.push_back(kBase64[(v >> 6) & 0x3F]);
        out.push_back(kBase64[v & 0x3F]);
    }
    std::size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t v = static_cast<uint32_t>(static_cast<unsigned char>(data[i])) << 16;
        out.push_back(kBase64[(v >> 18) & 0x3F]);
        out.push_back(kBase64[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
                     (static_cast<uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8);
        out.push_back(kBase64[(v >> 18) & 0x3F]);
        out.push_back(kBase64[(v >> 12) & 0x3F]);
        out.push_back(kBase64[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

inline void RandomBytes(unsigned char* out, int n) {
#ifdef MCP_HAVE_OPENSSL
    if (RAND_bytes(out, n) != 1)
        throw std::runtime_error("RAND_bytes failed");
#else
    std::random_device rd;
    for (int i = 0; i < n; i += 4) {
        uint32_t v = rd();
        int copy = std::min(4, n - i);
        for (int j = 0; j < copy; ++j)
            out[i + j] = static_cast<unsigned char>((v >> (j * 8)) & 0xFF);
    }
#endif
}

inline std::string RandomBytesBase64(int n) {
    std::vector<unsigned char> bytes(static_cast<std::size_t>(n));
    RandomBytes(bytes.data(), n);
    return Base64Encode(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

}}}
