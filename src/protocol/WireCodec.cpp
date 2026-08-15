// WireCodec.cpp
// Per-era WireCodec implementations (2025-11-25 and 2026-07-28)
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/Content.hpp>
#include <mcp/Capabilities.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/Log.hpp>
#include <mcp/ProtocolVersion.hpp>
#include <detail/JsonFields.hpp>

#include <cstdint>
#include <unordered_set>

namespace mcp {
namespace {

// Shared methods — supported by both 2025 and 2026 era
inline const std::unordered_set<std::string_view> kCommonRequestMethods = {
    "tools/list", "tools/call",
    "resources/list", "resources/read", "resources/templates/list",
    "prompts/list", "prompts/get",
    "completion/complete",
};

inline const std::unordered_set<std::string_view> k2025OnlyRequestMethods = {
    "initialize",
    "ping",
    "resources/subscribe", "resources/unsubscribe",
    "logging/setLevel", "roots/list", "sampling/createMessage",
    "elicitation/create",
    "tasks/get", "tasks/update", "tasks/cancel",
    "tasks/result", "tasks/list",
};

inline const std::unordered_set<std::string_view> k2026OnlyRequestMethods = {
    "server/discover",
    "subscriptions/listen",
};

inline const std::unordered_set<std::string_view> kCommonNotifMethods = {
    "notifications/cancelled",
    "notifications/progress",
    "notifications/message",
    "notifications/resources/updated",
    "notifications/resources/list_changed",
    "notifications/tools/list_changed",
    "notifications/prompts/list_changed",
};

inline const std::unordered_set<std::string_view> k2025OnlyNotifMethods = {
    "notifications/initialized",
    "notifications/roots/list_changed",
    "notifications/elicitation/complete",
    "notifications/tasks/status",
    "notifications/tasks/working",
    "notifications/tasks/completed",
    "notifications/tasks/failed",
    "notifications/tasks/cancelled",
    "notifications/tasks/input_required",
};

inline const std::unordered_set<std::string_view> k2026OnlyNotifMethods = {
    "notifications/subscriptions/acknowledged",
};

inline std::unordered_set<std::string_view> MakeEraMethods(
    const std::unordered_set<std::string_view>& common,
    const std::unordered_set<std::string_view>& only) {
    auto set = common;
    set.insert(only.begin(), only.end());
    return set;
}

// ====================================================================
// rev2025-11-25 — initialization-handshake era
// ====================================================================
class Rev2025Codec : public WireCodec {
public:
    static constexpr std::string_view kEra = "2025-11-25";

    bool HasRequestMethod(std::string_view method) const override {
        static const auto methods = MakeEraMethods(
            kCommonRequestMethods, k2025OnlyRequestMethods);
        return methods.count(method) > 0;
    }

    bool HasNotificationMethod(std::string_view method) const override {
        static const auto notifs = MakeEraMethods(
            kCommonNotifMethods, k2025OnlyNotifMethods);
        return notifs.count(method) > 0;
    }

    WireValidation ValidateRequest(
        std::string_view method, const JsonValue& raw) const override {
        if (method == "initialize") {
            const JsonValue* params = raw.Find(detail::kParams);
            if (!params || !params->IsObject() ||
                !params->Contains(detail::kProtocolVersion) ||
                !params->Contains(detail::kCapabilities) ||
                !params->Contains(detail::kClientInfo)) {
                return WireValidation::Invalid;
            }
        }
        return WireValidation::Ok;
    }

    WireValidation ValidateResponse(
        std::string_view /*method*/, const JsonValue& /*raw*/) const override {
        // 2025-era responses have no additional structural requirements
        // beyond standard JSON-RPC validation (handled by the message parser).
        return WireValidation::Ok;
    }

    WireValidation ValidateNotification(
        std::string_view /*method*/, const JsonValue& /*raw*/) const override {
        // 2025-era notifications have no additional structural requirements
        // beyond standard JSON-RPC validation (handled by the message parser).
        return WireValidation::Ok;
    }

    void StampOutgoingRequest(
        JsonValue& /*request_body*/,
        const RequestMeta& /*meta*/) const override {
    }

    JsonValue EncodeResult(
        std::string_view /*method*/, const JsonValue& result) const override {
        return result;
    }

    int32_t EncodeErrorCode(int32_t code) const override {
        return code;
    }

    std::string_view Era() const override { return kEra; }
};

// ====================================================================
// rev2026-07-28 — stateless, per-request _meta era
// ====================================================================
class Rev2026Codec : public WireCodec {
public:
    static constexpr std::string_view kEra = "2026-07-28";

    bool HasRequestMethod(std::string_view method) const override {
        static const auto methods = MakeEraMethods(
            kCommonRequestMethods, k2026OnlyRequestMethods);
        return methods.count(method) > 0;
    }

    bool HasNotificationMethod(std::string_view method) const override {
        static const auto notifs = MakeEraMethods(
            kCommonNotifMethods, k2026OnlyNotifMethods);
        return notifs.count(method) > 0;
    }

    WireValidation ValidateRequest(
        std::string_view method, const JsonValue& raw) const override {
        if (!HasRequestMethod(method)) {
            return WireValidation::NotInEra;
        }
        if (method != "server/discover" &&
            !raw.Contains(detail::kMeta)) {
            return WireValidation::Invalid;
        }
        return WireValidation::Ok;
    }

    WireValidation ValidateResponse(
        std::string_view method, const JsonValue& raw) const override {
        if (!raw.Contains(detail::kResultType)) {
            return WireValidation::Invalid;
        }
        static const std::unordered_set<std::string_view> kListMethods = {
            "tools/list", "resources/list", "resources/templates/list",
            "prompts/list",
        };
        if (kListMethods.count(method) > 0) {
            auto* rt = raw.Find(detail::kResultType);
            if (rt && (!rt->IsString() || rt->GetString() != detail::kCompleteValue)) {
                return WireValidation::Invalid;
            }
        }
        return WireValidation::Ok;
    }

    WireValidation ValidateNotification(
        std::string_view /*method*/, const JsonValue& raw) const override {
        if (raw.Contains("id") || raw.Contains("result") || raw.Contains("error")) {
            return WireValidation::Invalid;
        }
        return WireValidation::Ok;
    }

    void StampOutgoingRequest(
        JsonValue& body,
        const RequestMeta& meta) const override {
        JsonValue meta_obj(JsonValue::object_tag);
        if (meta.client_info) {
            meta_obj[detail::kMetaClientInfoKey] =
                SerializeImplementation(*meta.client_info);
        }
        if (meta.client_capabilities) {
            meta_obj[detail::kMetaClientCapabilitiesKey] =
                SerializeClientCapabilities(*meta.client_capabilities);
        }
        meta_obj[detail::kMetaProtocolVersionKey] = JsonValue(meta.protocol_version);
        body[detail::kMeta] = std::move(meta_obj);
    }

    JsonValue EncodeResult(
        std::string_view /*method*/, const JsonValue& result) const override {
        JsonValue j = result;
        // The 2026 era flattens cache fields onto the result top level:
        // ttlMs/cacheScope replace the nested cacheHint object.
        if (auto* ch = j.Find(detail::kCacheHint); ch && ch->IsObject()) {
            if (auto* ttl = ch->Find(detail::kTTLMs); ttl && ttl->IsInt())
                j[detail::kTTLMs] = *ttl;
            if (auto* scope = ch->Find(detail::kCacheScope); scope && scope->IsString())
                j[detail::kCacheScope] = *scope;
            j.GetObject().erase(std::string(detail::kCacheHint));
        }
        if (!j.Contains(detail::kResultType)) {
            j[detail::kResultType] = JsonValue(detail::kCompleteValue);
        }
        return j;
    }

    int32_t EncodeErrorCode(int32_t code) const override {
        switch (code) {
            case static_cast<int32_t>(McpErrorCode::RequestTimeout):
            case static_cast<int32_t>(McpErrorCode::ConnectionRefused):
            case static_cast<int32_t>(McpErrorCode::TlsHandshakeFailed):
                MCP_LOG(Warning,
                    "internal error code remapped to InternalError on the 2026 era: " +
                    std::to_string(code));
                return static_cast<int32_t>(McpErrorCode::InternalError);
            default:
                return code;
        }
    }

    std::string_view Era() const override { return kEra; }
};

} // anonymous namespace

// ── Factory ──
std::unique_ptr<WireCodec> MakeWireCodec(std::string_view protocol_version) {
    if (protocol_version >= kLatestProtocolVersion) {
        return std::make_unique<Rev2026Codec>();
    }
    return std::make_unique<Rev2025Codec>();
}

} // namespace mcp
