// McpTypesParams.cpp — Request params serialization and elicitation helpers

#include <mcp/McpTypes.hpp>
#include <mcp/McpError.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

// Forward declarations from other modules
JsonValue SerializeRequestMeta(const RequestMeta& v);
RequestMeta DeserializeRequestMeta(const JsonValue& j);
JsonValue SerializeClientCapabilities(const ClientCapabilities& v);
ClientCapabilities DeserializeClientCapabilities(const JsonValue& j);
JsonValue SerializeImplementation(const Implementation& v);
Implementation DeserializeImplementation(const JsonValue& j);
JsonValue SerializeContentVariant(const ContentVariant& v);
ContentVariant DeserializeContentVariant(const JsonValue& j);
JsonValue SerializeSubscriptionFilter(const SubscriptionFilter& v);
SubscriptionFilter DeserializeSubscriptionFilter(const JsonValue& j);
JsonValue SerializeElicitResult(const ElicitResult& v);
ElicitResult DeserializeElicitResult(const JsonValue& j);
JsonValue SerializeInputRequests(const InputRequests& v);
InputRequests DeserializeInputRequests(const JsonValue& j);
JsonValue SerializeLoggingLevel(LoggingLevel l);
LoggingLevel DeserializeLoggingLevel(const JsonValue& j);

// ── PaginatedRequestParams ──

JsonValue SerializePaginatedRequestParams(const PaginatedRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    detail::SerializeOptional(obj, "cursor", v.cursor);
    if (v.meta) obj[detail::kMeta] = SerializeRequestMeta(*v.meta);
    return obj;
}

PaginatedRequestParams DeserializePaginatedRequestParams(const JsonValue& j) {
    PaginatedRequestParams v;
    detail::DeserializeOptional(j, "cursor", v.cursor);
    auto* m = j.Find(detail::kMeta);
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

// ── ResourceRequestParams ──

JsonValue SerializeResourceRequestParams(const ResourceRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    if (v.meta) obj[detail::kMeta] = SerializeRequestMeta(*v.meta);
    return obj;
}

ResourceRequestParams DeserializeResourceRequestParams(const JsonValue& j) {
    ResourceRequestParams v;
    v.uri = j[detail::kUri].GetString();
    auto* m = j.Find(detail::kMeta);
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

// ── CallToolRequestParams ──

JsonValue SerializeCallToolRequestParams(const CallToolRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kName] = JsonValue(v.name);
    detail::SerializeOptional(obj, detail::kArguments, v.arguments);
    if (v.meta) obj[detail::kMeta] = SerializeRequestMeta(*v.meta);
    detail::SerializeOptional(obj, detail::kInputResponses, v.input_responses);
    detail::SerializeOptional(obj, detail::kRequestState, v.request_state);
    return obj;
}

CallToolRequestParams DeserializeCallToolRequestParams(const JsonValue& j) {
    CallToolRequestParams v;
    v.name = j[detail::kName].GetString();
    detail::DeserializeOptional(j, detail::kArguments, v.arguments);
    auto* m = j.Find(detail::kMeta);
    if (m) v.meta = DeserializeRequestMeta(*m);
    detail::DeserializeOptional(j, detail::kInputResponses, v.input_responses);
    detail::DeserializeOptional(j, detail::kRequestState, v.request_state);
    return v;
}

// ── GetPromptRequestParams ──

JsonValue SerializeGetPromptRequestParams(const GetPromptRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kName] = JsonValue(v.name);
    detail::SerializeOptional(obj, detail::kArguments, v.arguments);
    if (v.meta) obj[detail::kMeta] = SerializeRequestMeta(*v.meta);
    return obj;
}

GetPromptRequestParams DeserializeGetPromptRequestParams(const JsonValue& j) {
    GetPromptRequestParams v;
    v.name = j[detail::kName].GetString();
    detail::DeserializeOptional(j, detail::kArguments, v.arguments);
    auto* m = j.Find(detail::kMeta);
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

// ── CompleteRequestParams ──

JsonValue SerializeCompleteRequestParams(const CompleteRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["ref"] = v.ref;
    obj["argumentName"] = JsonValue(v.argument_name);
    obj["argumentValue"] = JsonValue(v.argument_value);
    if (v.meta) obj[detail::kMeta] = SerializeRequestMeta(*v.meta);
    return obj;
}

CompleteRequestParams DeserializeCompleteRequestParams(const JsonValue& j) {
    CompleteRequestParams v;
    v.ref = j["ref"];
    v.argument_name = j["argumentName"].GetString();
    v.argument_value = j["argumentValue"].GetString();
    auto* m = j.Find(detail::kMeta);
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

// ── DiscoverRequestParams ──

JsonValue SerializeDiscoverRequestParams(const DiscoverRequestParams&) {
    return JsonValue(JsonValue::object_tag);
}

DiscoverRequestParams DeserializeDiscoverRequestParams(const JsonValue&) {
    return DiscoverRequestParams{};
}

// ── InitializeRequestParams ──

JsonValue SerializeInitializeRequestParams(const InitializeRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kProtocolVersion] = JsonValue(v.protocol_version);
    obj[detail::kCapabilities] = SerializeClientCapabilities(v.capabilities);
    obj["clientInfo"] = SerializeImplementation(v.client_info);
    return obj;
}

InitializeRequestParams DeserializeInitializeRequestParams(const JsonValue& j) {
    InitializeRequestParams v;
    v.protocol_version = j[detail::kProtocolVersion].GetString();
    v.capabilities = DeserializeClientCapabilities(j[detail::kCapabilities]);
    v.client_info = DeserializeImplementation(j["clientInfo"]);
    return v;
}

// ── ElicitRequestParams ──

JsonValue SerializeElicitRequestParams(const ElicitRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kMessage] = JsonValue(v.message);
    detail::SerializeOptional(obj, detail::kRequestedSchema, v.requested_schema);
    return obj;
}

ElicitRequestParams DeserializeElicitRequestParams(const JsonValue& j) {
    ElicitRequestParams v;
    v.message = j[detail::kMessage].GetString();
    detail::DeserializeOptional(j, detail::kRequestedSchema, v.requested_schema);
    return v;
}

// ── SamplingMessage ──

JsonValue SerializeSamplingMessage(const SamplingMessage& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kRole] = JsonValue(v.role);
    obj[detail::kContent] = SerializeContentVariant(v.content);
    return obj;
}

SamplingMessage DeserializeSamplingMessage(const JsonValue& j) {
    SamplingMessage v;
    v.role = j[detail::kRole].GetString();
    v.content = DeserializeContentVariant(j[detail::kContent]);
    return v;
}

// ── CreateMessageRequestParams ──

JsonValue SerializeCreateMessageRequestParams(const CreateMessageRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    {
        JsonValue::Array arr;
        for (const auto& msg : v.messages) arr.push_back(SerializeSamplingMessage(msg));
        obj[detail::kMessages] = JsonValue(std::move(arr));
    }
    obj["maxTokens"] = JsonValue(v.max_tokens);
    detail::SerializeOptional(obj, detail::kStopReason, v.stop_reason);
    detail::SerializeOptional(obj, "modelPreference", v.model_preference);
    return obj;
}

CreateMessageRequestParams DeserializeCreateMessageRequestParams(const JsonValue& j) {
    CreateMessageRequestParams v;
    auto* msgs = j.Find(detail::kMessages);
    if (msgs && msgs->IsArray()) {
        std::vector<SamplingMessage> vec;
        for (const auto& mv : msgs->GetArray()) vec.push_back(DeserializeSamplingMessage(mv));
        v.messages = std::move(vec);
    }
    v.max_tokens = j["maxTokens"].GetInt();
    detail::DeserializeOptional(j, detail::kStopReason, v.stop_reason);
    detail::DeserializeOptional(j, "modelPreference", v.model_preference);
    return v;
}

// ── Root ──

JsonValue SerializeRoot(const Root& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    detail::SerializeOptional(obj, detail::kName, v.name);
    return obj;
}

Root DeserializeRoot(const JsonValue& j) {
    Root v;
    v.uri = j[detail::kUri].GetString();
    detail::DeserializeOptional(j, detail::kName, v.name);
    return v;
}

// ── ListRootsRequestParams ──

JsonValue SerializeListRootsRequestParams(const ListRootsRequestParams&) {
    return JsonValue(JsonValue::object_tag);
}

ListRootsRequestParams DeserializeListRootsRequestParams(const JsonValue&) {
    return ListRootsRequestParams{};
}

// ── SetLevelRequestParams ──

JsonValue SerializeSetLevelRequestParams(const SetLevelRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kLevel] = SerializeLoggingLevel(v.level);
    return obj;
}

SetLevelRequestParams DeserializeSetLevelRequestParams(const JsonValue& j) {
    SetLevelRequestParams v;
    v.level = DeserializeLoggingLevel(j[detail::kLevel]);
    return v;
}

// ====================================================================
// Free functions for elicitation helpers
// ====================================================================

JsonValue MakeInputRequestForElicitation(const ElicitRequestParams& params) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kMethod] = JsonValue(std::string(methods::kElicit));
    obj[detail::kParams] = SerializeElicitRequestParams(params);
    return obj;
}

JsonValue MakeInputResponseFromElicitResult(const ElicitResult& result) {
    return SerializeElicitResult(result);
}

bool IsInputRequiredResult(const JsonValue& j) {
    if (!j.IsObject()) return false;
    auto* rt = j.Find(detail::kResultType);
    return rt && rt->IsString() && rt->GetString() == detail::kInputRequiredValue;
}

std::optional<InputRequests> ExtractInputRequests(const JsonValue& result) {
    if (!result.IsObject()) return std::nullopt;
    auto* ir = result.Find(detail::kInputRequests);
    if (!ir) return std::nullopt;
    return DeserializeInputRequests(*ir);
}

} // namespace mcp
