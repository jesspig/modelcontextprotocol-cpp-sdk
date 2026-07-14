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
    switch (l) {
        case LoggingLevel::Debug:     j = "debug";     break;
        case LoggingLevel::Info:      j = "info";      break;
        case LoggingLevel::Notice:    j = "notice";    break;
        case LoggingLevel::Warning:   j = "warning";   break;
        case LoggingLevel::Error:     j = "error";     break;
        case LoggingLevel::Critical:  j = "critical";  break;
        case LoggingLevel::Alert:     j = "alert";     break;
        case LoggingLevel::Emergency: j = "emergency"; break;
    }
}
inline void from_json(const nlohmann::json& j, LoggingLevel& l) {
    auto s = j.get<std::string>();
    if (s == "debug")         l = LoggingLevel::Debug;
    else if (s == "info")     l = LoggingLevel::Info;
    else if (s == "notice")   l = LoggingLevel::Notice;
    else if (s == "warning")  l = LoggingLevel::Warning;
    else if (s == "error")    l = LoggingLevel::Error;
    else if (s == "critical") l = LoggingLevel::Critical;
    else if (s == "alert")    l = LoggingLevel::Alert;
    else if (s == "emergency")l = LoggingLevel::Emergency;
    else throw std::runtime_error("unknown LoggingLevel: " + s);
}

// ═══════════════════════════════════════════════════════════════════════
// Structured _meta type hierarchy (2026-07-28, SEP-2575)
// ═══════════════════════════════════════════════════════════════════════

// MetaObject — 所有 _meta 值的基类型
// 序列化保持自由格式（additionalProperties）
struct MetaObject {
    std::optional<ProgressToken> progress_token;
    std::optional<nlohmann::json> extensions;
};
inline void to_json(nlohmann::json& j, const MetaObject& v) {
    j = nlohmann::json::object();
    if (v.progress_token) {
        std::visit([&j](const auto& val) { j["progressToken"] = val; }, *v.progress_token);
    }
    if (v.extensions) {
        for (auto& [k, val] : v.extensions->items()) j[k] = val;
    }
}
inline void from_json(const nlohmann::json& j, MetaObject& v) {
    if (auto it = j.find("progressToken"); it != j.end()) {
        const auto& val = *it;
        if (val.is_string()) v.progress_token = val.get<std::string>();
        else v.progress_token = val.get<int64_t>();
    }
    nlohmann::json ext = nlohmann::json::object();
    for (auto& [key, val] : j.items()) {
        if (key != "progressToken") ext[key] = val;
    }
    if (!ext.empty()) v.extensions = std::move(ext);
}

// RequestMetaObject — 请求的 _meta，2026-era 结构化字段
struct RequestMetaObject : MetaObject {
    std::string protocol_version{kLatestProtocolVersion.data()};
    std::optional<Implementation> client_info;
    std::optional<ClientCapabilities> client_capabilities;
    std::optional<LoggingLevel> log_level;
};
inline void to_json(nlohmann::json& j, const RequestMetaObject& v) {
    j = static_cast<const MetaObject&>(v);
    j["io.modelcontextprotocol/protocolVersion"] = v.protocol_version;
    if (v.client_info)         j["io.modelcontextprotocol/clientInfo"] = *v.client_info;
    if (v.client_capabilities) j["io.modelcontextprotocol/clientCapabilities"] = *v.client_capabilities;
    if (v.log_level)           j["io.modelcontextprotocol/logLevel"] = *v.log_level;
}
inline void from_json(const nlohmann::json& j, RequestMetaObject& v) {
    from_json(j, static_cast<MetaObject&>(v));
    if (auto it = j.find("io.modelcontextprotocol/protocolVersion"); it != j.end())
        v.protocol_version = it->get<std::string>();
    if (auto it = j.find("io.modelcontextprotocol/clientInfo"); it != j.end())
        v.client_info = it->get<Implementation>();
    if (auto it = j.find("io.modelcontextprotocol/clientCapabilities"); it != j.end())
        v.client_capabilities = it->get<ClientCapabilities>();
    if (auto it = j.find("io.modelcontextprotocol/logLevel"); it != j.end())
        v.log_level = it->get<LoggingLevel>();
    if (v.extensions) {
        v.extensions->erase("io.modelcontextprotocol/protocolVersion");
        v.extensions->erase("io.modelcontextprotocol/clientInfo");
        v.extensions->erase("io.modelcontextprotocol/clientCapabilities");
        v.extensions->erase("io.modelcontextprotocol/logLevel");
        if (v.extensions->empty()) v.extensions.reset();
    }
}

// NotificationMetaObject — 通知的 _meta，2026-era
struct NotificationMetaObject : MetaObject {
    std::optional<RequestId> subscription_id;
};
inline void to_json(nlohmann::json& j, const NotificationMetaObject& v) {
    j = static_cast<const MetaObject&>(v);
    if (v.subscription_id) {
        std::visit([&j](const auto& val) {
            j["io.modelcontextprotocol/subscriptionId"] = val;
        }, *v.subscription_id);
    }
}
inline void from_json(const nlohmann::json& j, NotificationMetaObject& v) {
    from_json(j, static_cast<MetaObject&>(v));
    if (auto it = j.find("io.modelcontextprotocol/subscriptionId"); it != j.end()) {
        const auto& val = *it;
        if (val.is_number_integer()) v.subscription_id = val.get<int64_t>();
        else v.subscription_id = val.get<std::string>();
    }
    if (v.extensions) {
        v.extensions->erase("io.modelcontextprotocol/subscriptionId");
        if (v.extensions->empty()) v.extensions.reset();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// RequestMeta — 保留向后兼容 (fields mirror RequestMetaObject)
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
// NotificationMeta — 保留向后兼容
// ═══════════════════════════════════════════════════════════════════════
struct NotificationMeta {
    std::optional<RequestId> subscription_id;
};
inline void to_json(nlohmann::json& j, const NotificationMeta& v) {
    j = nlohmann::json::object();
    if (v.subscription_id) {
        std::visit([&j](const auto& val) {
            j["io.modelcontextprotocol/subscriptionId"] = val;
        }, *v.subscription_id);
    }
}
inline void from_json(const nlohmann::json& j, NotificationMeta& v) {
    if (auto it = j.find("io.modelcontextprotocol/subscriptionId"); it != j.end()) {
        const auto& val = *it;
        if (val.is_number_integer()) v.subscription_id = val.get<int64_t>();
        else v.subscription_id = val.get<std::string>();
    }
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
