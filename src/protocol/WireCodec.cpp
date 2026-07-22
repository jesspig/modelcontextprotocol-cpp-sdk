#include <mcp/protocol/WireCodec.hpp>

#include <cstdint>
#include <unordered_set>

namespace mcp {
namespace {

// ── Conversion helpers ──
JsonValue ImplementationToJson(const Implementation& v) {
    JsonValue j(JsonValue::object_tag);
    j["name"] = JsonValue(v.name);
    j["version"] = JsonValue(v.version);
    if (v.title)       j["title"] = JsonValue(*v.title);
    if (v.description) j["description"] = JsonValue(*v.description);
    if (v.website_url) j["websiteUrl"] = JsonValue(*v.website_url);
    return j;
}

Implementation ImplementationFromJson(const JsonValue& j) {
    Implementation v;
    v.name = j.At("name").GetString();
    v.version = j.At("version").GetString();
    if (auto* t = j.Find("title"))       v.title = t->GetString();
    if (auto* d = j.Find("description")) v.description = d->GetString();
    if (auto* w = j.Find("websiteUrl"))  v.website_url = w->GetString();
    return v;
}

JsonValue ClientCapabilitiesToJson(const ClientCapabilities& v) {
    JsonValue j(JsonValue::object_tag);
    if (v.roots)     { JsonValue sub(JsonValue::object_tag); j["roots"] = std::move(sub); }
    if (v.sampling)  { JsonValue sub(JsonValue::object_tag); j["sampling"] = std::move(sub); }
    return j;
}

ClientCapabilities ClientCapabilitiesFromJson(const JsonValue& j) {
    ClientCapabilities v;
    if (j.Find("roots"))    v.roots = RootsCapability{};
    if (j.Find("sampling")) v.sampling = SamplingCapability{};
    return v;
}

// Shared methods — supported by both 2025 and 2026 era
inline const std::unordered_set<std::string> kCommonRequestMethods = {
    "tools/list", "tools/call",
    "resources/list", "resources/read", "resources/templates/list",
    "prompts/list", "prompts/get",
    "completion/complete",
    "elicitation/create",
};

inline const std::unordered_set<std::string> k2025OnlyRequestMethods = {
    "ping", "initialize",
    "resources/subscribe", "resources/unsubscribe",
    "logging/setLevel", "roots/list", "sampling/createMessage",
};

inline const std::unordered_set<std::string> k2026OnlyRequestMethods = {
    "server/discover",
    "server/extensions/list",
    "subscriptions/listen",
    "tasks/get", "tasks/update", "tasks/cancel",
};

inline const std::unordered_set<std::string> kCommonNotifMethods = {
    "notifications/cancelled",
    "notifications/progress",
    "notifications/resources/updated",
    "notifications/resources/list_changed",
    "notifications/tools/list_changed",
    "notifications/prompts/list_changed",
    "notifications/subscriptions/acknowledged",
};

inline const std::unordered_set<std::string> k2025OnlyNotifMethods = {
    "notifications/initialized",
    "notifications/message",
    "notifications/roots/list_changed",
    "notifications/elicitation/complete",
};

inline const std::unordered_set<std::string> k2026OnlyNotifMethods = {
    "notifications/tasks/status",
    "notifications/tasks/working",
    "notifications/tasks/completed",
    "notifications/tasks/failed",
    "notifications/tasks/cancelled",
    "notifications/tasks/input_required",
};

inline std::unordered_set<std::string> MakeEraMethods(
    const std::unordered_set<std::string>& common,
    const std::unordered_set<std::string>& only) {
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
        return methods.count(std::string(method)) > 0;
    }

    bool HasNotificationMethod(std::string_view method) const override {
        static const auto notifs = MakeEraMethods(
            kCommonNotifMethods, k2025OnlyNotifMethods);
        return notifs.count(std::string(method)) > 0;
    }

    WireValidation ValidateRequest(
        std::string_view /*method*/, const JsonValue& /*raw*/) const override {
        return WireValidation::Ok;
    }

    WireValidation ValidateResponse(
        std::string_view /*method*/, const JsonValue& /*raw*/) const override {
        return WireValidation::Ok;
    }

    WireValidation ValidateNotification(
        std::string_view /*method*/, const JsonValue& /*raw*/) const override {
        return WireValidation::Ok;
    }

    void StampOutgoingRequest(
        JsonValue& /*request_body*/,
        const RequestMeta& /*meta*/) const override {
    }

    std::optional<RequestMeta> ExtractIncomingMeta(
        const JsonValue& /*request_body*/) const override {
        return std::nullopt;
    }

    JsonValue DecodeResult(
        std::string_view /*method*/, const JsonValue& raw) const override {
        return raw;
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
    static constexpr std::string_view kProtocolVersionKey =
        "io.modelcontextprotocol/protocolVersion";
    static constexpr std::string_view kClientInfoKey =
        "io.modelcontextprotocol/clientInfo";
    static constexpr std::string_view kClientCapabilitiesKey =
        "io.modelcontextprotocol/clientCapabilities";

    bool HasRequestMethod(std::string_view method) const override {
        static const auto methods = MakeEraMethods(
            kCommonRequestMethods, k2026OnlyRequestMethods);
        return methods.count(std::string(method)) > 0;
    }

    bool HasNotificationMethod(std::string_view method) const override {
        static const auto notifs = MakeEraMethods(
            kCommonNotifMethods, k2026OnlyNotifMethods);
        return notifs.count(std::string(method)) > 0;
    }

    WireValidation ValidateRequest(
        std::string_view method, const JsonValue& raw) const override {
        if (!HasRequestMethod(method)) {
            return WireValidation::NotInEra;
        }
        if (method != "server/discover" &&
            !raw.Contains("_meta")) {
            return WireValidation::Invalid;
        }
        return WireValidation::Ok;
    }

    WireValidation ValidateResponse(
        std::string_view /*method*/, const JsonValue& /*raw*/) const override {
        return WireValidation::Ok;
    }

    WireValidation ValidateNotification(
        std::string_view /*method*/, const JsonValue& /*raw*/) const override {
        return WireValidation::Ok;
    }

    void StampOutgoingRequest(
        JsonValue& body,
        const RequestMeta& meta) const override {
        JsonValue meta_obj(JsonValue::object_tag);
        meta_obj[kProtocolVersionKey] = JsonValue(meta.protocol_version);
        if (meta.client_info) {
            meta_obj[kClientInfoKey] = ImplementationToJson(*meta.client_info);
        }
        if (meta.client_capabilities) {
            meta_obj[kClientCapabilitiesKey] = ClientCapabilitiesToJson(*meta.client_capabilities);
        }
        body["_meta"] = std::move(meta_obj);
    }

    std::optional<RequestMeta> ExtractIncomingMeta(
        const JsonValue& body) const override {
        auto* m = body.Find("_meta");
        if (!m) return std::nullopt;

        RequestMeta meta;
        if (auto* v = m->Find(kProtocolVersionKey))
            meta.protocol_version = v->GetString();
        if (auto* v = m->Find(kClientInfoKey))
            meta.client_info = ImplementationFromJson(*v);
        if (auto* v = m->Find(kClientCapabilitiesKey))
            meta.client_capabilities = ClientCapabilitiesFromJson(*v);
        return meta;
    }

    JsonValue DecodeResult(
        std::string_view /*method*/, const JsonValue& raw) const override {
        return raw;
    }

    JsonValue EncodeResult(
        std::string_view /*method*/, const JsonValue& result) const override {
        JsonValue j = result;
        if (!j.Contains("resultType")) {
            j["resultType"] = JsonValue("complete");
        }
        return j;
    }

    int32_t EncodeErrorCode(int32_t code) const override {
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
    return std::make_unique<Rev2025Codec>();
}

} // namespace mcp
