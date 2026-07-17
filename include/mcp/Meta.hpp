#pragma once

#include <mcp/Capabilities.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/ProtocolVersion.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <variant>

namespace mcp {

using ProgressToken = std::variant<std::string, int64_t>;

inline void to_json(nlohmann::json& j, const ProgressToken& pt) {
    std::visit([&j](const auto& v) { j = v; }, pt);
}
inline void from_json(const nlohmann::json& j, ProgressToken& pt) {
    if (j.is_string()) pt = j.get<std::string>();
    else pt = j.get<int64_t>();
}

enum class LoggingLevel {
    Debug, Info, Notice, Warning, Error, Critical, Alert, Emergency
};

inline void to_json(nlohmann::json& j, LoggingLevel l) {
    static const char* names[] = {"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};
    auto i = static_cast<int>(l);
    j = (i >= 0 && i < 8) ? names[i] : "debug";
}
inline void from_json(const nlohmann::json& j, LoggingLevel& l) {
    auto s = j.get<std::string>();
    static const std::pair<const char*, LoggingLevel> map[] = {
        {"debug", LoggingLevel::Debug}, {"info", LoggingLevel::Info},
        {"notice", LoggingLevel::Notice}, {"warning", LoggingLevel::Warning},
        {"error", LoggingLevel::Error}, {"critical", LoggingLevel::Critical},
        {"alert", LoggingLevel::Alert}, {"emergency", LoggingLevel::Emergency},
    };
    for (auto& [name, level] : map) {
        if (s == name) { l = level; return; }
    }
    throw std::runtime_error("unknown LoggingLevel: " + s);
}

// ═══════════════════════════════════════════════════════════════════════
// RequestMeta — 保留向后兼容
// ═══════════════════════════════════════════════════════════════════════
struct RequestMeta {
    std::optional<ProgressToken> progress_token;
    std::string protocol_version{kLatestProtocolVersion.data()};
    std::optional<Implementation> client_info;
    std::optional<ClientCapabilities> client_capabilities;
    std::optional<LoggingLevel> log_level;
    std::optional<nlohmann::json> extensions;
};
inline void to_json(nlohmann::json& j, const RequestMeta& v) {
    j = nlohmann::json::object();
    if (v.progress_token) {
        std::visit([&j](const auto& val) { j["progressToken"] = val; }, *v.progress_token);
    }
    j["io.modelcontextprotocol/protocolVersion"] = v.protocol_version;
    if (v.client_info)         j["io.modelcontextprotocol/clientInfo"] = *v.client_info;
    if (v.client_capabilities) j["io.modelcontextprotocol/clientCapabilities"] = *v.client_capabilities;
    if (v.log_level)           j["io.modelcontextprotocol/logLevel"] = *v.log_level;
    if (v.extensions) {
        for (auto& [k, val] : v.extensions->items()) j[k] = val;
    }
}
inline void from_json(const nlohmann::json& j, RequestMeta& v) {
    if (auto it = j.find("progressToken"); it != j.end()) {
        const auto& val = *it;
        if (val.is_string()) v.progress_token = val.get<std::string>();
        else v.progress_token = val.get<int64_t>();
    }
    if (auto it = j.find("io.modelcontextprotocol/protocolVersion"); it != j.end())
        v.protocol_version = it->get<std::string>();
    if (auto it = j.find("io.modelcontextprotocol/clientInfo"); it != j.end())
        v.client_info = it->get<Implementation>();
    if (auto it = j.find("io.modelcontextprotocol/clientCapabilities"); it != j.end())
        v.client_capabilities = it->get<ClientCapabilities>();
    if (auto it = j.find("io.modelcontextprotocol/logLevel"); it != j.end())
        v.log_level = it->get<LoggingLevel>();
}

// ═══════════════════════════════════════════════════════════════════════
// CacheHint
// ═══════════════════════════════════════════════════════════════════════
struct CacheHint {
    std::optional<int64_t> ttl_ms;
    std::optional<std::string> cache_scope;
};
inline void to_json(nlohmann::json& j, const CacheHint& v) {
    j = nlohmann::json::object();
    if (v.ttl_ms)     j["ttlMs"] = *v.ttl_ms;
    if (v.cache_scope) j["cacheScope"] = *v.cache_scope;
}
inline void from_json(const nlohmann::json& j, CacheHint& v) {
    if (auto it = j.find("ttlMs"); it != j.end())     v.ttl_ms = it->get<int64_t>();
    if (auto it = j.find("cacheScope"); it != j.end()) v.cache_scope = it->get<std::string>();
}

} // namespace mcp
