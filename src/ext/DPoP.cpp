#include <mcp/ext/DPoP.hpp>
#include <mcp/detail/Base64Url.hpp>
#include <mcp/detail/sha256.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>

#if defined(MCP_HAVE_OPENSSL)
#define MCP_DPOP_HAVE_OPENSSL 1
#endif

#if defined(MCP_DPOP_HAVE_OPENSSL)
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#endif

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mcp::ext::dpop {

namespace detail {

std::string DpopEscapeJson(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    buf[0] = '\\';
                    buf[1] = 'u';
                    buf[2] = '0';
                    buf[3] = '0';
                    buf[4] = "0123456789ABCDEF"[(c >> 4) & 0xF];
                    buf[5] = "0123456789ABCDEF"[c & 0xF];
                    buf[6] = '\0';
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string DpopBase64Url(std::string_view raw) {
    return mcp::detail::Base64UrlEncode(raw);
}

std::string DpopSha256Base64Url(std::string_view input) {
    return DpopBase64Url(mcp::detail::Sha256::Hash(input));
}

std::string DpopThumbprintJson(std::string_view x, std::string_view y) {
    std::string out("{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"");
    out += DpopEscapeJson(x);
    out += "\",\"y\":\"";
    out += DpopEscapeJson(y);
    out += "\"}";
    return out;
}

} // namespace detail

std::string ComputeThumbprint(std::string_view x, std::string_view y) {
    if (x.empty() || y.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "DPoP thumbprint requires x and y");
    }
    return detail::DpopSha256Base64Url(detail::DpopThumbprintJson(x, y));
}

std::string PublicJwkJson(const EcP256Jwk& key) {
    std::string out("{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"");
    out += detail::DpopEscapeJson(key.x);
    out += "\",\"y\":\"";
    out += detail::DpopEscapeJson(key.y);
    out += "\"}";
    return out;
}

bool IsNonceFresh(int64_t iat_seconds, int64_t now_seconds, int64_t window_seconds) {
    if (window_seconds < 0) return false;
    if (iat_seconds > now_seconds) return false;
    return (now_seconds - iat_seconds) <= window_seconds;
}

int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

JtiReplayCache::JtiReplayCache(int64_t window_seconds)
    : window_seconds_(window_seconds < 0 ? 0 : window_seconds) {}

bool JtiReplayCache::CheckAndStore(std::string_view jti,
                                   int64_t iat_seconds,
                                   int64_t now_seconds) {
    if (jti.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = seen_.begin(); it != seen_.end();) {
        if (now_seconds - it->second > window_seconds_) it = seen_.erase(it);
        else ++it;
    }
    std::string key(jti);
    if (seen_.find(key) != seen_.end()) return false;
    seen_.emplace(std::move(key), iat_seconds);
    return true;
}

void JtiReplayCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    seen_.clear();
}

std::string GenerateNonce(std::size_t random_bytes) {
    if (random_bytes == 0) random_bytes = 16;
    if (random_bytes > 64) random_bytes = 64;
    std::string raw(random_bytes, '\0');
#if defined(MCP_DPOP_HAVE_OPENSSL)
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&raw[0]),
                   static_cast<int>(random_bytes)) != 1) {
        throw McpError(McpErrorCode::InternalError, "DPoP nonce generation failed");
    }
#else
    std::random_device rd;
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<char>(rd() & 0xFF);
#endif
    return mcp::detail::Base64UrlEncode(raw);
}

namespace verify_detail {

std::string LowerCopy(std::string_view input) {
    std::string out(input);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string StripHtuSuffix(std::string_view htu) {
    size_t end = htu.size();
    for (size_t i = 0; i < htu.size(); ++i) {
        if (htu[i] == '?' || htu[i] == '#') {
            end = i;
            break;
        }
    }
    return std::string(htu.substr(0, end));
}

const JsonValue* RequireStringField(const JsonValue& obj, std::string_view key) {
    auto* field = obj.Find(key);
    if (!field || !field->IsString()) return nullptr;
    return field;
}

} // namespace verify_detail

#if defined(MCP_DPOP_HAVE_OPENSSL)

namespace detail {

struct DpopEcKeyDeleter {
    void operator()(EC_KEY* key) const noexcept { EC_KEY_free(key); }
};

struct DpopBnDeleter {
    void operator()(BIGNUM* bn) const noexcept { BN_free(bn); }
};

struct DpopPointDeleter {
    void operator()(EC_POINT* point) const noexcept { EC_POINT_free(point); }
};

struct DpopSigDeleter {
    void operator()(ECDSA_SIG* sig) const noexcept { ECDSA_SIG_free(sig); }
};

struct DpopBioDeleter {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};

struct DpopEvpPkeyDeleter {
    void operator()(EVP_PKEY* pkey) const noexcept { EVP_PKEY_free(pkey); }
};

using DpopEcKeyPtr = std::unique_ptr<EC_KEY, DpopEcKeyDeleter>;
using DpopBnPtr = std::unique_ptr<BIGNUM, DpopBnDeleter>;
using DpopPointPtr = std::unique_ptr<EC_POINT, DpopPointDeleter>;
using DpopSigPtr = std::unique_ptr<ECDSA_SIG, DpopSigDeleter>;
using DpopBioPtr = std::unique_ptr<BIO, DpopBioDeleter>;
using DpopEvpPkeyPtr = std::unique_ptr<EVP_PKEY, DpopEvpPkeyDeleter>;

std::string DpopBnToFixed32(const BIGNUM* bn) {
    unsigned char buf[32];
    if (BN_bn2binpad(bn, buf, sizeof(buf)) != static_cast<int>(sizeof(buf))) {
        throw McpError(McpErrorCode::InternalError, "DPoP key coordinate encoding failed");
    }
    return std::string(reinterpret_cast<const char*>(buf), sizeof(buf));
}

DpopBnPtr DpopBinToBn(std::string_view raw) {
    DpopBnPtr bn(BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()),
                           static_cast<int>(raw.size()), nullptr));
    if (!bn) {
        throw McpError(McpErrorCode::InternalError, "DPoP key material invalid");
    }
    return bn;
}

std::string DpopRandomBase64Url(std::size_t bytes) {
    std::string raw(bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&raw[0]),
                   static_cast<int>(bytes)) != 1) {
        throw McpError(McpErrorCode::InternalError, "DPoP random generation failed");
    }
    return DpopBase64Url(raw);
}

std::string DpopSignEs256Raw(std::string_view private_scalar,
                             std::string_view signing_input) {
    if (private_scalar.empty() || signing_input.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "ES256 signing requires key and input");
    }
    std::string digest = mcp::detail::Sha256::Hash(signing_input);
    DpopEcKeyPtr ec(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
    if (!ec) {
        throw McpError(McpErrorCode::InternalError, "ES256 signing failed");
    }
    DpopBnPtr priv = DpopBinToBn(private_scalar);
    const EC_GROUP* group = EC_KEY_get0_group(ec.get());
    if (!group) {
        throw McpError(McpErrorCode::InternalError, "ES256 signing failed");
    }
    DpopPointPtr pub(EC_POINT_new(group));
    if (!pub) {
        throw McpError(McpErrorCode::InternalError, "ES256 signing failed");
    }
    if (EC_POINT_mul(group, pub.get(), priv.get(), nullptr, nullptr, nullptr) != 1) {
        throw McpError(McpErrorCode::InternalError, "ES256 signing failed");
    }
    if (EC_KEY_set_private_key(ec.get(), priv.get()) != 1 ||
        EC_KEY_set_public_key(ec.get(), pub.get()) != 1) {
        throw McpError(McpErrorCode::InternalError, "ES256 signing failed");
    }
    DpopSigPtr sig(ECDSA_do_sign(
        reinterpret_cast<const unsigned char*>(digest.data()),
        static_cast<int>(digest.size()), ec.get()));
    if (!sig) {
        throw McpError(McpErrorCode::InternalError, "ES256 signing failed");
    }
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(sig.get(), &r, &s);
    if (!r || !s) {
        throw McpError(McpErrorCode::InternalError, "ES256 signing failed");
    }
    std::string raw_sig = DpopBnToFixed32(r);
    raw_sig += DpopBnToFixed32(s);
    return raw_sig;
}

std::string DpopPrivateScalarToFixed32(const EC_KEY* key) {
    const BIGNUM* priv = EC_KEY_get0_private_key(key);
    if (!priv) {
        throw McpError(McpErrorCode::InvalidParams, "ES256 private key has no scalar");
    }
    unsigned char priv_buf[32];
    if (BN_bn2binpad(priv, priv_buf, sizeof(priv_buf)) != static_cast<int>(sizeof(priv_buf))) {
        throw McpError(McpErrorCode::InvalidParams, "ES256 private key scalar must be 32 bytes");
    }
    return std::string(reinterpret_cast<const char*>(priv_buf), sizeof(priv_buf));
}

std::string SignEs256JwtWithPem(std::string_view header_json,
                                std::string_view payload_json,
                                std::string_view private_key_pem)
{
    if (header_json.empty() || payload_json.empty() || private_key_pem.empty()) {
        throw McpError(McpErrorCode::InvalidParams,
                       "ES256 JWT signing requires header, payload and private key PEM");
    }
    std::string signing_input = DpopBase64Url(header_json);
    signing_input += ".";
    signing_input += DpopBase64Url(payload_json);

    DpopBioPtr bio(BIO_new_mem_buf(private_key_pem.data(),
                                   static_cast<int>(private_key_pem.size())));
    if (!bio) {
        throw McpError(McpErrorCode::InternalError, "ES256 signing failed");
    }
    DpopEvpPkeyPtr pkey(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
    if (!pkey) {
        throw McpError(McpErrorCode::InvalidParams, "ES256 private key PEM is not parsable");
    }
    DpopEcKeyPtr key(EVP_PKEY_get1_EC_KEY(pkey.get()));
    if (!key) {
        throw McpError(McpErrorCode::InvalidParams, "ES256 private key PEM is not an EC key");
    }
    if (EC_GROUP_get_curve_name(EC_KEY_get0_group(key.get())) != NID_X9_62_prime256v1) {
        throw McpError(McpErrorCode::InvalidParams, "ES256 private key must be on the P-256 curve");
    }
    if (EC_KEY_check_key(key.get()) != 1) {
        throw McpError(McpErrorCode::InvalidParams, "ES256 private key PEM is not a valid EC key");
    }
    std::string raw_sig =
        DpopSignEs256Raw(DpopPrivateScalarToFixed32(key.get()), signing_input);
    std::string jwt = signing_input;
    jwt += ".";
    jwt += DpopBase64Url(raw_sig);
    return jwt;
}

} // namespace detail

EcP256Jwk GenerateKey() {
    detail::DpopEcKeyPtr key(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
    if (!key || EC_KEY_generate_key(key.get()) != 1) {
        throw McpError(McpErrorCode::InternalError, "DPoP key generation failed");
    }
    const BIGNUM* priv = EC_KEY_get0_private_key(key.get());
    const EC_POINT* pub = EC_KEY_get0_public_key(key.get());
    const EC_GROUP* group = EC_KEY_get0_group(key.get());
    if (!priv || !pub || !group) {
        throw McpError(McpErrorCode::InternalError, "DPoP key generation failed");
    }
    detail::DpopBnPtr x(BN_new());
    detail::DpopBnPtr y(BN_new());
    if (!x || !y) {
        throw McpError(McpErrorCode::InternalError, "DPoP key generation failed");
    }
    if (EC_POINT_get_affine_coordinates(group, pub, x.get(), y.get(), nullptr) != 1) {
        throw McpError(McpErrorCode::InternalError, "DPoP key generation failed");
    }
    EcP256Jwk out;
    out.x = detail::DpopBase64Url(detail::DpopBnToFixed32(x.get()));
    out.y = detail::DpopBase64Url(detail::DpopBnToFixed32(y.get()));
    unsigned char priv_buf[32];
    if (BN_bn2binpad(priv, priv_buf, sizeof(priv_buf)) != static_cast<int>(sizeof(priv_buf))) {
        throw McpError(McpErrorCode::InternalError, "DPoP key generation failed");
    }
    out.d = detail::DpopBase64Url(
        std::string(reinterpret_cast<const char*>(priv_buf), sizeof(priv_buf)));
    return out;
}

std::string CreateProof(const EcP256Jwk& key,
                        std::string_view htm,
                        std::string_view htu,
                        std::optional<std::string_view> access_token,
                        std::optional<std::string_view> nonce,
                        std::optional<int64_t> iat_override_seconds) {
    if (key.x.empty() || key.y.empty() || key.d.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "DPoP proof requires a complete EC key");
    }
    if (htm.empty() || htu.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "DPoP proof requires htm and htu");
    }
    auto decoded = mcp::detail::Base64UrlDecode(key.d);
    if (!decoded || decoded->size() != 32) {
        throw McpError(McpErrorCode::InvalidParams, "DPoP key material invalid");
    }
    std::string header("{\"alg\":\"ES256\",\"jwk\":");
    header += PublicJwkJson(key);
    header += ",\"typ\":\"dpop+jwt\"}";
    int64_t iat = iat_override_seconds ? *iat_override_seconds : NowSeconds();
    std::string payload("{\"htm\":\"");
    payload += detail::DpopEscapeJson(htm);
    payload += "\",\"htu\":\"";
    payload += detail::DpopEscapeJson(htu);
    payload += "\",\"iat\":";
    payload += std::to_string(iat);
    payload += ",\"jti\":\"";
    payload += detail::DpopRandomBase64Url(16);
    payload += "\"";
    if (access_token && !access_token->empty()) {
        payload += ",\"ath\":\"";
        payload += detail::DpopSha256Base64Url(*access_token);
        payload += "\"";
    }
    if (nonce && !nonce->empty()) {
        payload += ",\"nonce\":\"";
        payload += detail::DpopEscapeJson(*nonce);
        payload += "\"";
    }
    payload += "}";
    std::string signing_input = detail::DpopBase64Url(header);
    signing_input += ".";
    signing_input += detail::DpopBase64Url(payload);
    std::string raw_sig = detail::DpopSignEs256Raw(*decoded, signing_input);
    std::string proof = signing_input;
    proof += ".";
    proof += detail::DpopBase64Url(raw_sig);
    return proof;
}

bool VerifyProof(std::string_view proof,
                 const DPoPVerifyOptions& options,
                 JtiReplayCache& replay) {
    if (options.htm.empty() || options.htu.empty() || options.access_token.empty())
        return false;
    if (options.window_seconds < 0) return false;
    int64_t now = options.now_seconds != 0 ? options.now_seconds : NowSeconds();
    size_t dot1 = proof.find('.');
    if (dot1 == std::string_view::npos || dot1 == 0) return false;
    size_t dot2 = proof.find('.', dot1 + 1);
    if (dot2 == std::string_view::npos || dot2 == dot1 + 1) return false;
    if (proof.find('.', dot2 + 1) != std::string_view::npos) return false;
    std::string header_seg(proof.substr(0, dot1));
    std::string payload_seg(proof.substr(dot1 + 1, dot2 - dot1 - 1));
    std::string sig_seg(proof.substr(dot2 + 1));
    if (sig_seg.empty()) return false;
    auto header_raw = mcp::detail::Base64UrlDecode(header_seg);
    auto payload_raw = mcp::detail::Base64UrlDecode(payload_seg);
    auto sig_raw = mcp::detail::Base64UrlDecode(sig_seg);
    if (!header_raw || !payload_raw || !sig_raw || sig_raw->size() != 64)
        return false;
    JsonValue header;
    JsonValue payload;
    try {
        header = JsonValue::Parse(*header_raw);
        payload = JsonValue::Parse(*payload_raw);
    } catch (...) {
        return false;
    }
    if (!header.IsObject() || !payload.IsObject()) return false;
    auto* alg = verify_detail::RequireStringField(header, "alg");
    if (!alg || alg->GetString() != "ES256") return false;
    auto* typ = verify_detail::RequireStringField(header, "typ");
    if (!typ || verify_detail::LowerCopy(typ->GetString()) != "dpop+jwt") return false;
    auto* jwk = header.Find("jwk");
    if (!jwk || !jwk->IsObject()) return false;
    auto* kty = verify_detail::RequireStringField(*jwk, "kty");
    auto* crv = verify_detail::RequireStringField(*jwk, "crv");
    auto* x = verify_detail::RequireStringField(*jwk, "x");
    auto* y = verify_detail::RequireStringField(*jwk, "y");
    if (!kty || !crv || !x || !y) return false;
    if (kty->GetString() != "EC" || crv->GetString() != "P-256") return false;
    if (x->GetString().empty() || y->GetString().empty()) return false;
    if (jwk->Find("d") != nullptr) return false;
    auto* htm = verify_detail::RequireStringField(payload, "htm");
    if (!htm || htm->GetString() != options.htm) return false;
    auto* htu = verify_detail::RequireStringField(payload, "htu");
    if (!htu || htu->GetString().empty()) return false;
    std::string bearing = verify_detail::StripHtuSuffix(htu->GetString());
    std::string expected = verify_detail::StripHtuSuffix(options.htu);
    bool htu_ok = !expected.empty() && bearing == expected;
    if (!htu_ok && options.alternate_htu && !options.alternate_htu->empty())
        htu_ok = bearing == verify_detail::StripHtuSuffix(*options.alternate_htu);
    if (!htu_ok) return false;
    auto* iat = payload.Find("iat");
    if (!iat || !iat->IsInt()) return false;
    if (!IsNonceFresh(iat->GetInt(), now, options.window_seconds)) return false;
    auto* jti = verify_detail::RequireStringField(payload, "jti");
    if (!jti || jti->GetString().empty()) return false;
    auto* ath = verify_detail::RequireStringField(payload, "ath");
    if (!ath || ath->GetString() != detail::DpopSha256Base64Url(options.access_token))
        return false;
    auto* nonce = payload.Find("nonce");
    if (nonce) {
        if (!nonce->IsString()) return false;
        if (options.expected_nonce && !options.expected_nonce->empty() &&
            nonce->GetString() != *options.expected_nonce)
            return false;
    }
    auto x_raw = mcp::detail::Base64UrlDecode(x->GetString());
    auto y_raw = mcp::detail::Base64UrlDecode(y->GetString());
    if (!x_raw || !y_raw || x_raw->size() != 32 || y_raw->size() != 32)
        return false;
    std::string signing_input = header_seg;
    signing_input += ".";
    signing_input += payload_seg;
    std::string digest = mcp::detail::Sha256::Hash(signing_input);
    detail::DpopEcKeyPtr ec(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
    if (!ec) return false;
    const EC_GROUP* group = EC_KEY_get0_group(ec.get());
    if (!group) return false;
    detail::DpopPointPtr pub(EC_POINT_new(group));
    detail::DpopBnPtr x_bn(BN_bin2bn(
        reinterpret_cast<const unsigned char*>(x_raw->data()), 32, nullptr));
    detail::DpopBnPtr y_bn(BN_bin2bn(
        reinterpret_cast<const unsigned char*>(y_raw->data()), 32, nullptr));
    if (!pub || !x_bn || !y_bn) return false;
    if (EC_POINT_set_affine_coordinates(group, pub.get(), x_bn.get(), y_bn.get(),
                                        nullptr) != 1)
        return false;
    if (EC_KEY_set_public_key(ec.get(), pub.get()) != 1) return false;
    if (EC_KEY_check_key(ec.get()) != 1) return false;
    detail::DpopSigPtr sig(ECDSA_SIG_new());
    detail::DpopBnPtr r(BN_bin2bn(
        reinterpret_cast<const unsigned char*>(sig_raw->data()), 32, nullptr));
    detail::DpopBnPtr s(BN_bin2bn(
        reinterpret_cast<const unsigned char*>(sig_raw->data() + 32), 32, nullptr));
    if (!sig || !r || !s) return false;
    if (ECDSA_SIG_set0(sig.get(), r.get(), s.get()) != 1) return false;
    r.release();
    s.release();
    int verified = ECDSA_do_verify(
        reinterpret_cast<const unsigned char*>(digest.data()),
        static_cast<int>(digest.size()), sig.get(), ec.get());
    if (verified != 1) return false;
    return replay.CheckAndStore(jti->GetString(), iat->GetInt(), now);
}

#else

EcP256Jwk GenerateKey() {
    throw McpError(McpErrorCode::InternalError,
                   "DPoP requires OpenSSL EC support, which is not available in this build");
}

std::string CreateProof(const EcP256Jwk&,
                        std::string_view,
                        std::string_view,
                        std::optional<std::string_view>,
                        std::optional<std::string_view>,
                        std::optional<int64_t>) {
    throw McpError(McpErrorCode::InternalError,
                   "DPoP requires OpenSSL EC support, which is not available in this build");
}

bool VerifyProof(std::string_view,
                 const DPoPVerifyOptions&,
                 JtiReplayCache&) {
    throw McpError(McpErrorCode::InternalError,
                   "DPoP requires OpenSSL EC support, which is not available in this build");
}

namespace detail {

std::string SignEs256JwtWithPem(std::string_view,
                                std::string_view,
                                std::string_view) {
    throw McpError(McpErrorCode::InternalError,
                   "ES256 JWT signing requires OpenSSL EC support, which is not available in this build");
}

} // namespace detail

#endif

} // namespace mcp::ext::dpop

#if defined(MCP_DPOP_HAVE_OPENSSL)
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif
