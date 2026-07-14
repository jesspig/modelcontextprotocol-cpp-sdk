#include <mcp/protocol/WireCodec.hpp>

#include <cstdint>
#include <unordered_set>

namespace mcp {
namespace {

// ====================================================================
// rev2025-11-25 — initialization-handshake era
// ====================================================================
class Rev2025Codec : public WireCodec {
public:
    static constexpr std::string_view kEra = "2025-11-25";

    bool HasRequestMethod(std::string_view method) const override {
        static const std::unordered_set<std::string> methods = {
            "ping",
            "initialize",
            "tools/list", "tools/call",
            "resources/list", "resources/read", "resources/templates/list",
            "resources/subscribe", "resources/unsubscribe",
            "prompts/list", "prompts/get",
            "completion/complete",
            "logging/setLevel",
            "roots/list",
            "sampling/createMessage",
            "elicitation/create",
        };
        return methods.count(std::string(method)) > 0;
    }

    bool HasNotificationMethod(std::string_view method) const override {
        static const std::unordered_set<std::string> notifs = {
            "notifications/initialized",
            "notifications/cancelled",
            "notifications/progress",
            "notifications/message",
            "notifications/resources/updated",
            "notifications/resources/list_changed",
            "notifications/tools/list_changed",
            "notifications/prompts/list_changed",
            "notifications/roots/list_changed",
            "notifications/elicitation/complete",
            "notifications/subscriptions/acknowledged",
        };
        return notifs.count(std::string(method)) > 0;
    }

    WireValidation ValidateRequest(
        std::string_view /*method*/, const nlohmann::json& /*raw*/) const override {
        // 2025-era: no structural validation beyond JSON-RPC
        return WireValidation::Ok;
    }

    WireValidation ValidateResponse(
        std::string_view /*method*/, const nlohmann::json& /*raw*/) const override {
        return WireValidation::Ok;
    }

    WireValidation ValidateNotification(
        std::string_view /*method*/, const nlohmann::json& /*raw*/) const override {
        return WireValidation::Ok;
    }

    void StampOutgoingRequest(
        nlohmann::json& /*request_body*/,
        const RequestMeta& /*meta*/) const override {
        // 2025-era: no per-request _meta envelope;
        // client info and capabilities are negotiated during initialize
    }

    std::optional<RequestMeta> ExtractIncomingMeta(
        const nlohmann::json& /*request_body*/) const override {
        // 2025-era: _meta may exist but is not protocol-mandated
        return std::nullopt;
    }

    nlohmann::json DecodeResult(
        std::string_view /*method*/, const nlohmann::json& raw) const override {
        // 2025-era: result is identity — no resultType field expected
        return raw;
    }

    nlohmann::json EncodeResult(
        std::string_view /*method*/, const nlohmann::json& result) const override {
        // 2025-era: identity encoding
        return result;
    }

    int32_t EncodeErrorCode(int32_t code) const override {
        // 2025-era: identity mapping
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
    static constexpr std::string_view kProtocolVersionKey =
        "io.modelcontextprotocol/protocolVersion";
    static constexpr std::string_view kClientInfoKey =
        "io.modelcontextprotocol/clientInfo";
    static constexpr std::string_view kClientCapabilitiesKey =
        "io.modelcontextprotocol/clientCapabilities";

    bool HasRequestMethod(std::string_view method) const override {
        static const std::unordered_set<std::string> methods = {
            "server/discover",
            "tools/list", "tools/call",
            "resources/list", "resources/read", "resources/templates/list",
            "resources/subscribe", "resources/unsubscribe",
            "prompts/list", "prompts/get",
            "completion/complete",
            "subscriptions/listen",
            "elicitation/create",
            "tasks/get", "tasks/update", "tasks/cancel",
        };
        return methods.count(std::string(method)) > 0;
    }

    bool HasNotificationMethod(std::string_view method) const override {
        static const std::unordered_set<std::string> notifs = {
            "notifications/cancelled",
            "notifications/progress",
            "notifications/resources/updated",
            "notifications/resources/list_changed",
            "notifications/tools/list_changed",
            "notifications/prompts/list_changed",
            "notifications/subscriptions/acknowledged",
            "notifications/tasks/status",
            "notifications/tasks/working",
            "notifications/tasks/completed",
            "notifications/tasks/failed",
            "notifications/tasks/cancelled",
            "notifications/tasks/input_required",
        };
        return notifs.count(std::string(method)) > 0;
    }

    WireValidation ValidateRequest(
        std::string_view method, const nlohmann::json& raw) const override {
        if (!HasRequestMethod(method)) {
            return WireValidation::NotInEra;
        }
        // Verify _meta is present for requests that require it
        // (all client-initiated requests in 2026 era)
        if (method != "server/discover" &&
            !raw.contains("_meta")) {
            return WireValidation::Invalid;
        }
        return WireValidation::Ok;
    }

    WireValidation ValidateResponse(
        std::string_view /*method*/, const nlohmann::json& /*raw*/) const override {
        return WireValidation::Ok;
    }

    WireValidation ValidateNotification(
        std::string_view /*method*/, const nlohmann::json& /*raw*/) const override {
        return WireValidation::Ok;
    }

    void StampOutgoingRequest(
        nlohmann::json& body,
        const RequestMeta& meta) const override {
        body["_meta"] = nlohmann::json::object();
        body["_meta"][kProtocolVersionKey] = meta.protocol_version;
        if (meta.client_info) {
            body["_meta"][kClientInfoKey] = *meta.client_info;
        }
        if (meta.client_capabilities) {
            body["_meta"][kClientCapabilitiesKey] = *meta.client_capabilities;
        }
    }

    std::optional<RequestMeta> ExtractIncomingMeta(
        const nlohmann::json& body) const override {
        auto it = body.find("_meta");
        if (it == body.end()) return std::nullopt;

        const auto& m = *it;
        RequestMeta meta;
        if (auto v = m.find(kProtocolVersionKey); v != m.end())
            meta.protocol_version = v->get<std::string>();
        if (auto v = m.find(kClientInfoKey); v != m.end())
            meta.client_info = v->get<Implementation>();
        if (auto v = m.find(kClientCapabilitiesKey); v != m.end())
            meta.client_capabilities = v->get<ClientCapabilities>();
        return meta;
    }

    nlohmann::json DecodeResult(
        std::string_view /*method*/, const nlohmann::json& raw) const override {
        return raw;
    }

    nlohmann::json EncodeResult(
        std::string_view /*method*/, const nlohmann::json& result) const override {
        auto j = result;
        j["resultType"] = "complete";
        return j;
    }

    int32_t EncodeErrorCode(int32_t code) const override {
        // 2026-era: SEP-2164 renumbers error codes
        // HeaderMismatch: -32001 → -32020
        // MissingRequiredClientCapability: -32003 → -32021
        // UnsupportedProtocolVersion: -32004 → -32022
        switch (code) {
            case -32001: return -32020;
            case -32003: return -32021;
            case -32004: return -32022;
            default:     return code;
        }
    }

    std::string_view Era() const override { return kEra; }
};

} // anonymous namespace

// ── Factory ──
std::unique_ptr<WireCodec> MakeWireCodec(std::string_view protocol_version) {
    if (protocol_version >= "2026-07-28") {
        return std::make_unique<Rev2026Codec>();
    }
    // Legacy versions (2024-11-05, 2025-03-26, 2025-06-18, 2025-11-25)
    // and unknown versions fall back to 2025 codec
    return std::make_unique<Rev2025Codec>();
}

} // namespace mcp
