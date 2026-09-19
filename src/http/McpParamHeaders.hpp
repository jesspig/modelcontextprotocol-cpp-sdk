#pragma once
// McpParamHeaders.hpp — SEP-2243 custom header annotations (x-mcp-header) and value encoding

#include <mcp/JsonRpc.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/detail/McpParamAnnotations.hpp>

#include <cctype>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mcp {
namespace http_detail {

inline constexpr std::string_view kMcpParamPrefix = "mcp-param-";
inline constexpr std::string_view kBase64SentinelOpen = "=?base64?";
inline constexpr std::string_view kBase64SentinelClose = "?=";

// RFC 4648 section 4 (padded). The Mcp-Param-* sentinel defined by the
// Streamable HTTP spec uses this alphabet; it is NOT interchangeable with the
// unpadded base64url used by PKCE and request-state.
inline std::string StandardBase64Encode(std::string_view data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        const uint32_t n = (static_cast<uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8) |
                           static_cast<uint32_t>(static_cast<unsigned char>(data[i + 2]));
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
        i += 3;
    }
    const size_t remaining = data.size() - i;
    if (remaining == 1) {
        const uint32_t n = static_cast<uint32_t>(static_cast<unsigned char>(data[i])) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const uint32_t n = (static_cast<uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

inline int Base64SextetValue(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline std::optional<std::string> StandardBase64Decode(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        int sextets[4];
        int padding = 0;
        for (size_t k = 0; k < 4; ++k) {
            const unsigned char c = static_cast<unsigned char>(text[i + k]);
            if (c == '=') {
                if (k < 2) return std::nullopt;
                sextets[k] = 0;
                ++padding;
                continue;
            }
            if (padding > 0) return std::nullopt;
            const int value = Base64SextetValue(c);
            if (value < 0) return std::nullopt;
            sextets[k] = value;
        }
        const uint32_t n = (static_cast<uint32_t>(sextets[0]) << 18) |
                           (static_cast<uint32_t>(sextets[1]) << 12) |
                           (static_cast<uint32_t>(sextets[2]) << 6) |
                           static_cast<uint32_t>(sextets[3]);
        out.push_back(static_cast<char>((n >> 16) & 0xFF));
        if (padding < 2) out.push_back(static_cast<char>((n >> 8) & 0xFF));
        if (padding < 1) out.push_back(static_cast<char>(n & 0xFF));
    }
    return out;
}

inline bool MatchesBase64Sentinel(std::string_view value) {
    const size_t open = kBase64SentinelOpen.size();
    const size_t close = kBase64SentinelClose.size();
    if (value.size() < open + close) return false;
    return value.substr(0, open) == kBase64SentinelOpen &&
           value.substr(value.size() - close) == kBase64SentinelClose;
}

inline bool IsPlainHeaderSafe(std::string_view value) {
    if (value.empty()) return true;
    if (value.front() == ' ' || value.front() == '\t') return false;
    if (value.back() == ' ' || value.back() == '\t') return false;
    for (char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == 0x20 || c == 0x09) continue;
        if (c < 0x21 || c > 0x7E) return false;
    }
    return true;
}

using detail::McpParamAnnotation;
using detail::ToolParamAnnotations;
using detail::ParseToolParamAnnotations;

inline std::optional<JsonValue> ValueAtPath(const JsonValue& root,
                                            const std::vector<std::string>& path) {
    const JsonValue* current = &root;
    for (const auto& key : path) {
        if (!current->IsObject()) return std::nullopt;
        const JsonValue* next = current->Find(key);
        if (!next) return std::nullopt;
        current = next;
    }
    return *current;
}

inline std::optional<std::string> EncodeHeaderValue(const JsonValue& value) {
    std::string text;
    if (value.IsString()) {
        text = value.GetString();
    } else if (value.IsBool()) {
        text = value.GetBool() ? "true" : "false";
    } else if (value.IsInt()) {
        text = std::to_string(value.GetInt());
    } else {
        return std::nullopt;
    }
    if (IsPlainHeaderSafe(text) && !MatchesBase64Sentinel(text)) return text;
    return std::string(kBase64SentinelOpen) + StandardBase64Encode(text) +
           std::string(kBase64SentinelClose);
}

inline std::optional<JsonValue> DecodeHeaderValue(std::string_view header_value) {
    if (MatchesBase64Sentinel(header_value)) {
        const size_t open = kBase64SentinelOpen.size();
        const size_t close = kBase64SentinelClose.size();
        const std::string_view inner = header_value.substr(open, header_value.size() - open - close);
        auto decoded = StandardBase64Decode(inner);
        if (!decoded.has_value()) return std::nullopt;
        return JsonValue(std::move(*decoded));
    }
    return JsonValue(std::string(header_value));
}

inline bool NumericTextEqual(std::string_view left, std::string_view right) {
    try {
        size_t left_used = 0;
        size_t right_used = 0;
        const double left_value = std::stod(std::string(left), &left_used);
        const double right_value = std::stod(std::string(right), &right_used);
        if (left_used != left.size() || right_used != right.size()) return false;
        return left_value == right_value;
    } catch (...) {
        return false;
    }
}

inline bool HeaderValueMatchesBody(const JsonValue& header_value, const JsonValue& body_value) {
    if (body_value.IsInt() || body_value.IsDouble()) {
        const std::string body_text = body_value.IsInt()
                                          ? std::to_string(body_value.GetInt())
                                          : std::to_string(body_value.GetDouble());
        if (header_value.IsInt()) return header_value.GetInt() == body_value.GetInt();
        if (header_value.IsDouble()) return header_value.GetDouble() == body_value.GetDouble();
        if (header_value.IsString()) return NumericTextEqual(header_value.GetString(), body_text);
        return false;
    }
    if (body_value.IsBool()) {
        if (header_value.IsBool()) return header_value.GetBool() == body_value.GetBool();
        if (header_value.IsString()) {
            return header_value.GetString() == (body_value.GetBool() ? "true" : "false");
        }
        return false;
    }
    if (body_value.IsString()) {
        if (!header_value.IsString()) return false;
        return header_value.GetString() == body_value.GetString();
    }
    return false;
}

// Client-side annotation cache, populated from tools/list responses so that
// tools/call mirrors annotated arguments into Mcp-Param-* request headers.
class ToolAnnotationCache {
public:
    void NoteToolsListRequest(std::string key) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_tools_list_.insert(std::move(key));
    }

    bool ConsumeToolsListRequest(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_tools_list_.erase(key) > 0;
    }

    // Drops tools whose x-mcp-header annotations violate the spec constraints
    // and returns their names so callers can log the rejection.
    std::vector<std::string> ObserveToolsListResult(JsonValue& result) {
        std::vector<std::string> rejected;
        if (!result.IsObject()) return rejected;
        JsonValue* tools = result.Find("tools");
        if (!tools || !tools->IsArray()) return rejected;

        std::map<std::string, std::vector<McpParamAnnotation>, std::less<>> refreshed;
        JsonValue::Array kept;
        kept.reserve(tools->GetArray().size());
        for (auto& tool : tools->GetArray()) {
            if (!tool.IsObject()) {
                kept.push_back(std::move(tool));
                continue;
            }
            const JsonValue* name = tool.Find("name");
            const std::string tool_name =
                (name && name->IsString()) ? name->GetString() : std::string();
            const JsonValue* schema = tool.Find("inputSchema");
            if (schema) {
                auto parsed = ParseToolParamAnnotations(*schema);
                if (!parsed.IsValid()) {
                    rejected.push_back(tool_name);
                    continue;
                }
                if (!tool_name.empty()) refreshed[tool_name] = std::move(parsed.annotations);
            }
            kept.push_back(std::move(tool));
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            tools_ = std::move(refreshed);
        }
        tools->GetArray() = std::move(kept);
        return rejected;
    }

    std::vector<std::pair<std::string, std::string>> BuildParamHeaders(
        const std::string& method, const JsonValue& params) const {
        std::vector<std::pair<std::string, std::string>> headers;
        if (method != "tools/call" || !params.IsObject()) return headers;
        const JsonValue* name = params.Find("name");
        if (!name || !name->IsString()) return headers;
        const JsonValue* arguments = params.Find("arguments");
        if (!arguments || !arguments->IsObject()) return headers;

        std::vector<McpParamAnnotation> annotations;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tools_.find(name->GetString());
            if (it == tools_.end()) return headers;
            annotations = it->second;
        }
        for (const auto& annotation : annotations) {
            auto value = ValueAtPath(*arguments, annotation.property_path);
            if (!value.has_value() || value->IsNull()) continue;
            auto encoded = EncodeHeaderValue(*value);
            if (!encoded.has_value()) continue;
            headers.emplace_back("Mcp-Param-" + annotation.header_name, std::move(*encoded));
        }
        return headers;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        tools_.clear();
        pending_tools_list_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<McpParamAnnotation>, std::less<>> tools_;
    std::set<std::string> pending_tools_list_;
};

inline std::string KeyFromId(const RequestId& id) {
    if (auto* numeric = std::get_if<int64_t>(&id)) return std::to_string(*numeric);
    if (auto* text = std::get_if<std::string>(&id)) return *text;
    return {};
}

} // namespace http_detail
} // namespace mcp
