// RequestState.hpp - HMAC-protected requestState mint/verify helpers (MRTR)

#pragma once

#include <mcp/JsonValue.hpp>
#include <mcp/detail/sha256.hpp>

#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace mcp {
namespace detail {

// ── Hex encoding of raw bytes ──
inline std::string HexEncode(std::string_view raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
    }
    return out;
}

// ── HMAC-SHA256 (RFC 2104), built on the standalone Sha256 ──
inline std::string HmacSha256(std::string_view key, std::string_view data) {
    std::string block = key.size() > 64 ? Sha256::Hash(key) : std::string(key);
    block.resize(64, '\0');
    std::string ipad(64, '\x36');
    std::string opad(64, '\x5c');
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] = static_cast<char>(ipad[i] ^ block[i]);
        opad[i] = static_cast<char>(opad[i] ^ block[i]);
    }
    std::string inner_input = ipad;
    inner_input.append(data);
    std::string outer_input = opad;
    outer_input.append(Sha256::Hash(inner_input));
    return Sha256::Hash(outer_input);
}

// ── Base64url (RFC 4648 §5, unpadded) ──
inline std::string Base64UrlEncode(std::string_view raw) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < raw.size()) {
        uint32_t n = (static_cast<unsigned char>(raw[i]) << 16)
            | (static_cast<unsigned char>(raw[i + 1]) << 8)
            | static_cast<unsigned char>(raw[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 0x3f]);
        out.push_back(kAlphabet[(n >> 12) & 0x3f]);
        out.push_back(kAlphabet[(n >> 6) & 0x3f]);
        out.push_back(kAlphabet[n & 0x3f]);
        i += 3;
    }
    if (i + 1 == raw.size()) {
        uint32_t n = static_cast<unsigned char>(raw[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3f]);
        out.push_back(kAlphabet[(n >> 12) & 0x3f]);
    } else if (i + 2 == raw.size()) {
        uint32_t n = (static_cast<unsigned char>(raw[i]) << 16)
            | (static_cast<unsigned char>(raw[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3f]);
        out.push_back(kAlphabet[(n >> 12) & 0x3f]);
        out.push_back(kAlphabet[(n >> 6) & 0x3f]);
    }
    return out;
}

inline std::optional<std::string> Base64UrlDecode(std::string_view text) {
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    std::string out;
    out.reserve(text.size() / 4 * 3);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : text) {
        int v = value_of(c);
        if (v < 0) return std::nullopt;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xff));
        }
    }
    return out;
}

// Wire format: <base64url(payload_json)>.<hex(hmac_sha256(key, payload_json))>
inline std::string MintRequestState(std::string_view key, const JsonValue& payload) {
    std::string payload_str = payload.Dump();
    std::string sig = HexEncode(HmacSha256(key, payload_str));
    std::string state = Base64UrlEncode(payload_str);
    state.push_back('.');
    state.append(sig);
    return state;
}

// Constant-time signature check; when ttl > 0 the payload must carry an
// integer "iat" (unix seconds) no older than ttl.
inline bool VerifyRequestState(
    std::string_view key, std::string_view state, JsonValue& out_payload,
    std::chrono::seconds ttl = std::chrono::seconds(0))
{
    auto dot = state.find('.');
    if (dot == std::string_view::npos) return false;
    auto payload_str = Base64UrlDecode(state.substr(0, dot));
    if (!payload_str) return false;
    auto sig_hex = state.substr(dot + 1);
    std::string expected = HexEncode(HmacSha256(key, *payload_str));
    if (expected.size() != sig_hex.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(expected[i])
            ^ static_cast<unsigned char>(sig_hex[i]);
    }
    if (diff != 0) return false;
    try {
        out_payload = JsonValue::Parse(*payload_str);
    } catch (...) {
        return false;
    }
    if (ttl.count() > 0) {
        auto* iat = out_payload.Find("iat");
        if (!iat || !iat->IsInt()) return false;
        std::time_t now = std::time(nullptr);
        if (now - iat->GetInt() > ttl.count()) return false;
    }
    return true;
}

// Untrusted payload peek (signature is enforced by the verifier seam before
// the tool handler runs; this is for handlers reading their own round data).
// Accepts both minted "<base64url>.<sig>" states and bare JSON payloads
// (servers without a minting key pass handler states through verbatim).
inline std::optional<JsonValue> DecodeRequestStatePayload(std::string_view state) {
    if (state.find('.') == std::string_view::npos) {
        try {
            return JsonValue::Parse(state);
        } catch (...) {
            return std::nullopt;
        }
    }
    auto payload_str = Base64UrlDecode(state.substr(0, state.find('.')));
    if (!payload_str) return std::nullopt;
    try {
        return JsonValue::Parse(*payload_str);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace detail
} // namespace mcp
