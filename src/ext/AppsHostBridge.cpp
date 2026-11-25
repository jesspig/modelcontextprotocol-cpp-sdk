#include <mcp/ext/AppsHostBridge.hpp>
#include <mcp/McpError.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mcp::ext {
namespace {

using Clock = std::chrono::system_clock;

constexpr std::size_t kMaxOriginBytes = 2048;

[[noreturn]] void Reject(const char* reason) {
    throw McpError(McpErrorCode::InvalidParams, reason);
}

int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
}

void FillRandom(uint8_t* buffer, std::size_t size) {
    std::random_device rd;
    for (std::size_t i = 0; i < size; ++i) {
        buffer[i] = static_cast<uint8_t>(rd());
    }
}

bool IsAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string HexEncode(const std::array<uint8_t, kAppsCapabilityTokenBytes>& token) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(token.size() * 2);
    for (uint8_t byte : token) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

bool DecodeToken(std::string_view hex, std::array<uint8_t, kAppsCapabilityTokenBytes>& out) {
    if (hex.size() != kAppsCapabilityTokenBytes * 2) return false;
    for (std::size_t i = 0; i < kAppsCapabilityTokenBytes; ++i) {
        const int hi = HexDigit(hex[i * 2]);
        const int lo = HexDigit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

uint8_t TokenDiff(const std::array<uint8_t, kAppsCapabilityTokenBytes>& lhs,
                  const std::array<uint8_t, kAppsCapabilityTokenBytes>& rhs) {
    uint8_t diff = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        diff = static_cast<uint8_t>(diff | (lhs[i] ^ rhs[i]));
    }
    return diff;
}

bool IsValidMethod(std::string_view method) {
    if (method.empty()) return false;
    for (char c : method) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool IsKnownField(std::string_view key) {
    return key == "schema_version" || key == "id" || key == "method" || key == "params";
}

bool SamePermissions(const std::vector<std::string>& stored,
                     const std::set<std::string>& effective) {
    if (stored.size() != effective.size()) return false;
    auto it = effective.begin();
    for (const std::string& permission : stored) {
        if (permission != *it) return false;
        ++it;
    }
    return true;
}

class BoundedJsonParser {
public:
    BoundedJsonParser(std::string_view input, const AppsHostBridgeOptions& options)
        : input_(input), options_(options) {}

    JsonValue Parse() {
        SkipWhitespace();
        JsonValue value = ParseValue(0);
        SkipWhitespace();
        if (pos_ != input_.size()) Reject("apps bridge: invalid message JSON");
        return value;
    }

private:
    void SkipWhitespace() {
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos_;
        }
    }

    void CountNode() {
        if (++nodes_ > options_.max_nodes) Reject("apps bridge: JSON node budget exceeded");
    }

    void CheckStringBytes(std::size_t size) const {
        if (size > options_.max_string_bytes) Reject("apps bridge: JSON string budget exceeded");
    }

    static bool IsDigit(char c) {
        return c >= '0' && c <= '9';
    }

    JsonValue ParseValue(std::size_t depth) {
        if (depth > options_.max_depth) Reject("apps bridge: JSON depth budget exceeded");
        CountNode();
        if (pos_ >= input_.size()) Reject("apps bridge: invalid message JSON");
        const char c = input_[pos_];
        switch (c) {
            case '{': return ParseObject(depth);
            case '[': return ParseArray(depth);
            case '"': return JsonValue(ParseString());
            case 't': ParseLiteral("true"); return JsonValue(true);
            case 'f': ParseLiteral("false"); return JsonValue(false);
            case 'n': ParseLiteral("null"); return JsonValue(nullptr);
            case '-':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                return ParseNumber();
            default: Reject("apps bridge: invalid message JSON");
        }
    }

    void ParseLiteral(std::string_view expected) {
        if (input_.substr(pos_, expected.size()) != expected) Reject("apps bridge: invalid message JSON");
        pos_ += expected.size();
    }

    JsonValue ParseObject(std::size_t depth) {
        ++pos_;
        JsonValue::Object object;
        std::set<std::string> keys;
        SkipWhitespace();
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            return JsonValue(std::move(object));
        }
        while (true) {
            SkipWhitespace();
            if (pos_ >= input_.size() || input_[pos_] != '"') Reject("apps bridge: invalid message JSON");
            std::string key = ParseString();
            if (!keys.insert(key).second) Reject("apps bridge: duplicate object key");
            SkipWhitespace();
            if (pos_ >= input_.size() || input_[pos_] != ':') Reject("apps bridge: invalid message JSON");
            ++pos_;
            SkipWhitespace();
            JsonValue value = ParseValue(depth + 1);
            object.emplace(std::move(key), std::move(value));
            SkipWhitespace();
            if (pos_ >= input_.size()) Reject("apps bridge: invalid message JSON");
            const char c = input_[pos_++];
            if (c == '}') break;
            if (c != ',') Reject("apps bridge: invalid message JSON");
        }
        return JsonValue(std::move(object));
    }

    JsonValue ParseArray(std::size_t depth) {
        ++pos_;
        JsonValue::Array items;
        SkipWhitespace();
        if (pos_ < input_.size() && input_[pos_] == ']') {
            ++pos_;
            return JsonValue(std::move(items));
        }
        while (true) {
            SkipWhitespace();
            items.push_back(ParseValue(depth + 1));
            SkipWhitespace();
            if (pos_ >= input_.size()) Reject("apps bridge: invalid message JSON");
            const char c = input_[pos_++];
            if (c == ']') break;
            if (c != ',') Reject("apps bridge: invalid message JSON");
        }
        return JsonValue(std::move(items));
    }

    std::string ParseString() {
        ++pos_;
        std::string result;
        while (true) {
            const std::size_t start = pos_;
            while (pos_ < input_.size()) {
                const unsigned char c = static_cast<unsigned char>(input_[pos_]);
                if (c == '"' || c == '\\') break;
                if (c < 0x20) Reject("apps bridge: invalid message JSON");
                if (c < 0x80) {
                    ++pos_;
                    continue;
                }
                pos_ += ValidateUtf8Sequence();
            }
            CheckStringBytes(result.size() + (pos_ - start));
            result.append(input_.data() + start, pos_ - start);
            if (pos_ >= input_.size()) Reject("apps bridge: invalid message JSON");
            if (input_[pos_] == '"') {
                ++pos_;
                return result;
            }
            ++pos_;
            if (pos_ >= input_.size()) Reject("apps bridge: invalid message JSON");
            const char e = input_[pos_++];
            switch (e) {
                case '"': CheckStringBytes(result.size() + 1); result.push_back('"'); break;
                case '\\': CheckStringBytes(result.size() + 1); result.push_back('\\'); break;
                case '/': CheckStringBytes(result.size() + 1); result.push_back('/'); break;
                case 'b': CheckStringBytes(result.size() + 1); result.push_back('\b'); break;
                case 'f': CheckStringBytes(result.size() + 1); result.push_back('\f'); break;
                case 'n': CheckStringBytes(result.size() + 1); result.push_back('\n'); break;
                case 'r': CheckStringBytes(result.size() + 1); result.push_back('\r'); break;
                case 't': CheckStringBytes(result.size() + 1); result.push_back('\t'); break;
                case 'u': ParseUnicodeEscape(result); break;
                default: Reject("apps bridge: invalid message JSON");
            }
        }
    }

    void ParseUnicodeEscape(std::string& result) {
        uint32_t cp = ParseHex4();
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos_ + 1 >= input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
                Reject("apps bridge: invalid message JSON");
            }
            pos_ += 2;
            const uint32_t low = ParseHex4();
            if (low < 0xDC00 || low > 0xDFFF) Reject("apps bridge: invalid message JSON");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            Reject("apps bridge: invalid message JSON");
        }
        const std::size_t bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        CheckStringBytes(result.size() + bytes);
        AppendUtf8(result, cp);
    }

    uint32_t ParseHex4() {
        if (pos_ + 4 > input_.size()) Reject("apps bridge: invalid message JSON");
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = HexDigit(input_[pos_ + i]);
            if (digit < 0) Reject("apps bridge: invalid message JSON");
            value = (value << 4) | static_cast<uint32_t>(digit);
        }
        pos_ += 4;
        return value;
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::size_t ValidateUtf8Sequence() const {
        const std::size_t p = pos_;
        const unsigned char first = static_cast<unsigned char>(input_[p]);
        std::size_t continuation = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (first >= 0xC2 && first <= 0xDF) {
            continuation = 1;
        } else if (first >= 0xE0 && first <= 0xEF) {
            continuation = 2;
            if (first == 0xE0) low = 0xA0;
            if (first == 0xED) high = 0x9F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            continuation = 3;
            if (first == 0xF0) low = 0x90;
            if (first == 0xF4) high = 0x8F;
        } else {
            Reject("apps bridge: invalid message JSON");
        }
        if (p + continuation >= input_.size()) Reject("apps bridge: invalid message JSON");
        const unsigned char second = static_cast<unsigned char>(input_[p + 1]);
        if (second < low || second > high) Reject("apps bridge: invalid message JSON");
        for (std::size_t i = 2; i <= continuation; ++i) {
            const unsigned char c = static_cast<unsigned char>(input_[p + i]);
            if ((c & 0xC0) != 0x80) Reject("apps bridge: invalid message JSON");
        }
        return continuation + 1;
    }

    double ParseFloatingNumber(std::string_view token) const {
#if defined(_LIBCPP_VERSION)
        std::string adjusted(token);
        const lconv* lc = std::localeconv();
        const char* decimal = lc == nullptr ? "." : lc->decimal_point;
        if (decimal == nullptr || decimal[0] == '\0') decimal = ".";
        const std::size_t decimal_length = std::strlen(decimal);
        if (decimal_length != 1 || decimal[0] != '.') {
            std::string converted;
            converted.reserve(adjusted.size());
            for (char c : adjusted) {
                if (c == '.') {
                    converted.append(decimal, decimal_length);
                } else {
                    converted.push_back(c);
                }
            }
            adjusted = std::move(converted);
        }
        char* end = nullptr;
        errno = 0;
        const double value = std::strtod(adjusted.c_str(), &end);
        if (end != adjusted.c_str() + adjusted.size() || errno == ERANGE || !std::isfinite(value)) {
            Reject("apps bridge: non-finite number");
        }
        return value;
#else
        double value = 0.0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size() || !std::isfinite(value)) {
            Reject("apps bridge: non-finite number");
        }
        return value;
#endif
    }

    JsonValue ParseNumber() {
        const std::size_t start = pos_;
        std::size_t i = pos_;
        bool floating = false;
        if (input_[i] == '-') ++i;
        if (i >= input_.size() || !IsDigit(input_[i])) Reject("apps bridge: invalid message JSON");
        if (input_[i] == '0') {
            ++i;
            if (i < input_.size() && IsDigit(input_[i])) Reject("apps bridge: invalid message JSON");
        } else {
            while (i < input_.size() && IsDigit(input_[i])) ++i;
        }
        if (i < input_.size() && input_[i] == '.') {
            floating = true;
            ++i;
            if (i >= input_.size() || !IsDigit(input_[i])) Reject("apps bridge: invalid message JSON");
            while (i < input_.size() && IsDigit(input_[i])) ++i;
        }
        if (i < input_.size() && (input_[i] == 'e' || input_[i] == 'E')) {
            floating = true;
            ++i;
            if (i < input_.size() && (input_[i] == '+' || input_[i] == '-')) ++i;
            if (i >= input_.size() || !IsDigit(input_[i])) Reject("apps bridge: invalid message JSON");
            while (i < input_.size() && IsDigit(input_[i])) ++i;
        }
        const std::string_view token = input_.substr(start, i - start);
        pos_ = i;
        const char* first = token.data();
        const char* last = first + token.size();
        if (!floating) {
            int64_t value = 0;
            const auto result = std::from_chars(first, last, value);
            if (result.ec != std::errc{} || result.ptr != last) {
                Reject("apps bridge: non-finite number");
            }
            return JsonValue(value);
        }
        return JsonValue(ParseFloatingNumber(token));
    }

    std::string_view input_;
    const AppsHostBridgeOptions& options_;
    std::size_t pos_ = 0;
    std::size_t nodes_ = 0;
};

} // namespace

bool NormalizeOrigin(std::string_view origin, std::string& normalized) {
    if (origin.empty() || origin.size() > kMaxOriginBytes || origin == "*" || origin == "null") {
        return false;
    }
    for (char c : origin) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) return false;
    }
    const std::size_t separator = origin.find("://");
    if (separator == std::string_view::npos) return false;
    std::string scheme(origin.substr(0, separator));
    for (char& c : scheme) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    if (scheme != "http" && scheme != "https") return false;
    const std::string_view authority = origin.substr(separator + 3);
    if (authority.empty() || authority.find('/') != std::string_view::npos ||
        authority.find('?') != std::string_view::npos ||
        authority.find('#') != std::string_view::npos ||
        authority.find('@') != std::string_view::npos ||
        authority.find('\\') != std::string_view::npos ||
        authority.find('*') != std::string_view::npos) {
        return false;
    }
    std::string_view host = authority;
    std::string_view port_text;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return false;
        host = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            port_text = authority.substr(close + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
        }
    }
    if (host.empty()) return false;
    for (char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':' ||
            c == '[' || c == ']';
        if (!ok) return false;
    }
    uint32_t port = scheme == "https" ? 443 : 80;
    if (!port_text.empty()) {
        if (port_text.size() > 5) return false;
        uint32_t parsed = 0;
        for (char c : port_text) {
            if (!IsAsciiDigit(c)) return false;
            parsed = parsed * 10 + static_cast<uint32_t>(c - '0');
            if (parsed > 65535) return false;
        }
        if (parsed == 0) return false;
        port = parsed;
    }
    std::string host_copy(host);
    for (char& c : host_copy) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    normalized = std::string(scheme) + "://" + host_copy + ":" + std::to_string(port);
    return true;
}

bool IsAppsSensitiveMethod(std::string_view method) {
    return method == "tools/call" || method == "resources/read";
}

struct AppsHostBridge::Impl {
    struct CapabilityRecord {
        std::array<uint8_t, kAppsCapabilityTokenBytes> token{};
        std::string resource_uri;
        std::vector<std::string> allowed_origins;
        std::vector<std::string> permissions;
        int64_t expires_at_ms = 0;
        std::size_t max_message_bytes = 0;
        bool single_use = false;
        bool consumed = false;
    };

    AppsHostBridgeOptions options;
    std::vector<std::string> normalized_origins;
    std::set<std::string> effective_permissions;
    mutable std::mutex mutex;
    mutable std::vector<CapabilityRecord> capabilities;
};

void AppsHostBridge::ValidateTrustedContext(const AppsMessageContext& context) const {
    if (!context.sandbox_verified) Reject("apps bridge: sandbox not verified");
    if (context.resource_uri != impl_->options.ui_meta.resource_uri) {
        Reject("apps bridge: resource uri mismatch");
    }
    std::string source;
    std::string target;
    if (!NormalizeOrigin(context.source_origin, source) ||
        !NormalizeOrigin(context.target_origin, target)) {
        Reject("apps bridge: origin rejected");
    }
    const auto allowed = [&](const std::string& origin) {
        return std::find(impl_->normalized_origins.begin(), impl_->normalized_origins.end(), origin) !=
            impl_->normalized_origins.end();
    };
    if (!allowed(source) || !allowed(target)) Reject("apps bridge: origin rejected");
}

bool AppsHostBridge::CheckCapability(const AppsMessageContext& context, std::string_view method,
                                     std::size_t payload_bytes, bool consume) const {
    std::array<uint8_t, kAppsCapabilityTokenBytes> token{};
    if (!DecodeToken(context.capability, token)) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const Impl::CapabilityRecord* matched = nullptr;
    for (const Impl::CapabilityRecord& record : impl_->capabilities) {
        if (TokenDiff(record.token, token) == 0) matched = &record;
    }
    if (matched == nullptr) return false;
    bool valid = true;
    valid = valid && matched->resource_uri == context.resource_uri;
    valid = valid && !matched->consumed;
    valid = valid && matched->expires_at_ms > NowMillis();
    valid = valid && matched->max_message_bytes >= payload_bytes;
    valid = valid && matched->allowed_origins == impl_->normalized_origins;
    valid = valid && SamePermissions(matched->permissions, impl_->effective_permissions);
    if (IsAppsSensitiveMethod(method) && !matched->single_use) valid = false;
    if (consume && valid && matched->single_use) {
        const_cast<Impl::CapabilityRecord*>(matched)->consumed = true;
    }
    return valid;
}

AppsHostBridge::AppsHostBridge(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AppsHostBridge::~AppsHostBridge() = default;

std::unique_ptr<AppsHostBridge> AppsHostBridge::Create(AppsHostBridgeOptions options) {
    ValidateUiMeta(options.ui_meta);
    if (options.max_message_bytes == 0 || options.max_depth == 0 || options.max_nodes == 0 ||
        options.max_string_bytes == 0 || options.max_id_bytes == 0 ||
        options.max_method_bytes == 0 || options.capability_ttl.count() <= 0) {
        Reject("apps bridge: invalid options");
    }
    auto impl = std::make_unique<Impl>();
    for (const std::string& origin : options.allowed_origins) {
        std::string canonical;
        if (!NormalizeOrigin(origin, canonical)) Reject("apps bridge: invalid allowed origin");
        impl->normalized_origins.push_back(std::move(canonical));
    }
    for (const std::string& method : options.ui_meta.permissions) {
        if (method.size() > options.max_method_bytes || !IsValidMethod(method)) {
            Reject("apps bridge: invalid ui permission");
        }
    }
    std::set<std::string> host_methods;
    for (const std::string& method : options.host_allowed_methods) {
        if (method.size() > options.max_method_bytes || !IsValidMethod(method)) {
            Reject("apps bridge: invalid host method");
        }
        host_methods.insert(method);
    }
    for (const std::string& method : options.ui_meta.permissions) {
        if (host_methods.count(method) != 0) impl->effective_permissions.insert(method);
    }
    impl->options = std::move(options);
    return std::unique_ptr<AppsHostBridge>(new AppsHostBridge(std::move(impl)));
}

std::string AppsHostBridge::SerializeHostMessage(const AppsPostMessage& message,
                                                 const AppsMessageContext& context) const {
    ValidateTrustedContext(context);
    if (message.schema_version != kAppsMessageSchemaVersion) {
        Reject("apps bridge: schema version rejected");
    }
    if (message.method.empty() || message.method.size() > impl_->options.max_method_bytes ||
        !IsValidMethod(message.method) ||
        impl_->effective_permissions.count(message.method) == 0) {
        Reject("apps bridge: method rejected");
    }
    if (message.request_id.has_value() &&
        (message.request_id->empty() || message.request_id->size() > impl_->options.max_id_bytes)) {
        Reject("apps bridge: request id rejected");
    }
    const std::string params_dump = message.params.Dump();
    if (params_dump.size() > impl_->options.max_message_bytes) {
        Reject("apps bridge: message byte budget exceeded");
    }
    BoundedJsonParser(params_dump, impl_->options).Parse();
    JsonValue::Object object;
    object["schema_version"] = JsonValue(message.schema_version);
    if (message.request_id.has_value()) object["id"] = JsonValue(*message.request_id);
    object["method"] = JsonValue(message.method);
    object["params"] = message.params;
    const std::string payload = JsonValue(std::move(object)).Dump();
    if (payload.size() > impl_->options.max_message_bytes) {
        Reject("apps bridge: message byte budget exceeded");
    }
    return payload;
}

AppsPostMessage AppsHostBridge::ParseAppMessage(std::string_view payload,
                                                const AppsMessageContext& context) const {
    ValidateTrustedContext(context);
    if (payload.size() > impl_->options.max_message_bytes) {
        Reject("apps bridge: message byte budget exceeded");
    }
    const JsonValue root = BoundedJsonParser(payload, impl_->options).Parse();
    if (!root.IsObject()) Reject("apps bridge: invalid message JSON");
    for (const auto& entry : root.GetObject()) {
        if (!IsKnownField(entry.first)) Reject("apps bridge: unknown message field");
    }
    const JsonValue* version = root.Find("schema_version");
    const JsonValue* method = root.Find("method");
    const JsonValue* params = root.Find("params");
    if (version == nullptr || !version->IsString() ||
        version->GetString() != kAppsMessageSchemaVersion) {
        Reject("apps bridge: schema version rejected");
    }
    if (method == nullptr || !method->IsString() || method->GetString().empty() ||
        method->GetString().size() > impl_->options.max_method_bytes ||
        !IsValidMethod(method->GetString()) ||
        impl_->effective_permissions.count(method->GetString()) == 0) {
        Reject("apps bridge: method rejected");
    }
    if (params == nullptr) Reject("apps bridge: invalid message JSON");
    std::optional<std::string> id;
    const JsonValue* id_value = root.Find("id");
    if (id_value != nullptr) {
        if (id_value->IsString()) {
            if (id_value->GetString().empty() ||
                id_value->GetString().size() > impl_->options.max_id_bytes) {
                Reject("apps bridge: request id rejected");
            }
            id = id_value->GetString();
        } else if (id_value->IsInt()) {
            id = std::to_string(id_value->GetInt());
        } else {
            Reject("apps bridge: request id rejected");
        }
    }
    if (!CheckCapability(context, method->GetString(), payload.size(), true)) {
        Reject("apps bridge: capability rejected");
    }
    return AppsPostMessage{version->GetString(), id, method->GetString(), *params};
}

bool AppsHostBridge::IsMethodAllowed(std::string_view method,
                                     const AppsMessageContext& context) const {
    if (method.empty() || method.size() > impl_->options.max_method_bytes ||
        !IsValidMethod(method) || impl_->effective_permissions.count(std::string(method)) == 0) {
        return false;
    }
    return CheckCapability(context, method, 0, false);
}

std::string AppsHostBridge::CreateCapability(const AppsMessageContext& context) const {
    ValidateTrustedContext(context);
    std::array<uint8_t, kAppsCapabilityTokenBytes> token{};
    FillRandom(token.data(), token.size());
    Impl::CapabilityRecord record;
    record.token = token;
    record.resource_uri = context.resource_uri;
    record.allowed_origins = impl_->normalized_origins;
    record.permissions.assign(impl_->effective_permissions.begin(), impl_->effective_permissions.end());
    record.expires_at_ms = NowMillis() + impl_->options.capability_ttl.count();
    record.max_message_bytes = impl_->options.max_message_bytes;
    record.single_use = impl_->options.single_use_sensitive_methods;
    record.consumed = false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->capabilities.push_back(std::move(record));
    return HexEncode(token);
}

AppsSandboxPolicy AppsHostBridge::GetSandboxPolicy() const {
    AppsSandboxPolicy policy;
    policy.csp = impl_->options.ui_meta.csp;
    return policy;
}

} // namespace mcp::ext
