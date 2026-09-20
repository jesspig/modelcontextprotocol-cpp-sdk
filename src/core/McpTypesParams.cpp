#include <mcp/McpTypes.hpp>
#include <mcp/McpError.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSerializer.hpp>

namespace mcp {

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

GetPromptRequestParams DeserializeGetPromptRequestParams(const JsonValue& j) {
    GetPromptRequestParams v;
    v.name = j[detail::kName].GetString();
    detail::DeserializeOptional(j, detail::kArguments, v.arguments);
    auto* m = j.Find(detail::kMeta);
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

JsonValue SerializeCompleteRequestParams(const CompleteRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj["ref"] = v.ref;
    JsonValue argument(JsonValue::object_tag);
    argument["name"] = JsonValue(v.argument_name);
    argument["value"] = JsonValue(v.argument_value);
    obj["argument"] = std::move(argument);
    if (v.meta) obj[detail::kMeta] = SerializeRequestMeta(*v.meta);
    return obj;
}

CompleteRequestParams DeserializeCompleteRequestParams(const JsonValue& j) {
    CompleteRequestParams v;
    if (auto* r = j.Find("ref")) v.ref = *r;
    if (auto* a = j.Find("argument"); a && a->IsObject()) {
        if (auto* n = a->Find("name"); n && n->IsString()) v.argument_name = n->GetString();
        if (auto* val = a->Find("value"); val && val->IsString()) v.argument_value = val->GetString();
    } else {
        if (auto* n = j.Find("argumentName"); n && n->IsString()) v.argument_name = n->GetString();
        if (auto* val = j.Find("argumentValue"); val && val->IsString()) v.argument_value = val->GetString();
    }
    auto* m = j.Find(detail::kMeta);
    if (m) v.meta = DeserializeRequestMeta(*m);
    return v;
}

JsonValue SerializeDiscoverRequestParams(const DiscoverRequestParams&) {
    return JsonValue(JsonValue::object_tag);
}

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

JsonValue SerializeElicitRequestParams(const ElicitRequestParams& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kMessage] = JsonValue(v.message);
    detail::SerializeOptional(obj, detail::kRequestedSchema, v.requested_schema);
    if (v.mode != detail::kForm) {
        obj[detail::kMode] = JsonValue(v.mode);
        detail::SerializeOptional(obj, detail::kUrl, v.url);
        detail::SerializeOptional(obj, "elicitationId", v.elicitation_id);
    }
    return obj;
}

ElicitRequestParams DeserializeElicitRequestParams(const JsonValue& j) {
    ElicitRequestParams v;
    v.message = j[detail::kMessage].GetString();
    detail::DeserializeOptional(j, detail::kRequestedSchema, v.requested_schema);
    auto* mode = j.Find(detail::kMode);
    if (mode && mode->IsString()) v.mode = mode->GetString();
    detail::DeserializeOptional(j, detail::kUrl, v.url);
    detail::DeserializeOptional(j, "elicitationId", v.elicitation_id);
    return v;
}

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
    const JsonValue& max_tokens_val = j["maxTokens"];
    if (!max_tokens_val.IsInt())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("CreateMessageRequestParams: field 'maxTokens' expected int, got ") +
            detail::JsonValueTypeName(max_tokens_val));
    v.max_tokens = max_tokens_val.GetInt();
    detail::DeserializeOptional(j, detail::kStopReason, v.stop_reason);
    detail::DeserializeOptional(j, "modelPreference", v.model_preference);
    return v;
}

JsonValue SerializeRoot(const Root& v) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kUri] = JsonValue(v.uri);
    detail::SerializeOptional(obj, detail::kName, v.name);
    return obj;
}

JsonValue SerializeListRootsRequestParams(const ListRootsRequestParams&) {
    return JsonValue(JsonValue::object_tag);
}

ListRootsRequestParams DeserializeListRootsRequestParams(const JsonValue&) {
    return ListRootsRequestParams{};
}

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

InputRequest MakeInputRequestForElicitation(const ElicitRequestParams& params) {
    InputRequest request;
    request.method = std::string(methods::kElicit);
    request.params = SerializeElicitRequestParams(params);
    return request;
}

InputRequest MakeInputRequestForSampling(const CreateMessageRequestParams& params) {
    InputRequest request;
    request.method = std::string(methods::kCreateMessage);
    request.params = SerializeCreateMessageRequestParams(params);
    return request;
}

InputRequest MakeInputRequestForRoots(const ListRootsRequestParams& params) {
    InputRequest request;
    request.method = std::string(methods::kListRoots);
    request.params = SerializeListRootsRequestParams(params);
    return request;
}

JsonValue MakeInputResponseFromElicitResult(const ElicitResult& result) {
    JsonValue obj(JsonValue::object_tag);
    obj["action"] = JsonValue(result.action.empty() ? std::string("cancel") : result.action);
    detail::SerializeOptional(obj, "content", result.content);
    return obj;
}

JsonValue MakeInputResponseFromCreateMessageResult(const CreateMessageResult& result) {
    JsonValue obj(JsonValue::object_tag);
    obj[detail::kRole] = JsonValue(result.role);
    obj[detail::kContent] = SerializeContentVariant(result.content);
    obj["model"] = JsonValue(result.model);
    detail::SerializeOptional(obj, detail::kStopReason, result.stop_reason);
    return obj;
}

JsonValue MakeInputResponseFromListRootsResult(const ListRootsResult& result) {
    JsonValue obj(JsonValue::object_tag);
    JsonValue::Array arr;
    for (const auto& root : result.roots) arr.push_back(SerializeRoot(root));
    obj[detail::kRoots] = JsonValue(std::move(arr));
    return obj;
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
