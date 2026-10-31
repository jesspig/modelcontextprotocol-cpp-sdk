#include <mcp/Content.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Transport.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/client/auth/OAuthClientProvider.hpp>
#include <mcp/client/auth/TokenCache.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/protocol/MessageFilter.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/McpTaskStore.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(MCP_COMPAT_RUNTIME)

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "compat runtime: FAIL: " << message << "\n";
        ++failures;
    }
}

void ExpectDeserializeFailed(const mcp::JsonValue& value, const char* label) {
    try {
        (void)mcp::DeserializeFileRef(value);
        std::cerr << "compat runtime: FAIL: expected DeserializeFailed for "
                  << label << "\n";
        ++failures;
    } catch (const mcp::McpError& error) {
        Check(error.Code() == mcp::McpErrorCode::DeserializeFailed,
              "FileRef error code is DeserializeFailed");
    }
}

void CheckFileRefStrictness() {
    mcp::JsonValue base(mcp::JsonValue::object_tag);
    base["uri"] = "file:///data.bin";

    mcp::JsonValue fractional = base;
    fractional["offset"] = 1.5;
    ExpectDeserializeFailed(fractional, "fractional offset");

    mcp::JsonValue out_of_range = base;
    out_of_range["length"] = 1e30;
    ExpectDeserializeFailed(out_of_range, "out-of-range length");

    mcp::JsonValue non_numeric = base;
    non_numeric["offset"] = "1024";
    ExpectDeserializeFailed(non_numeric, "non-numeric offset");

    mcp::JsonValue nan_value = base;
    nan_value["length"] = std::numeric_limits<double>::quiet_NaN();
    ExpectDeserializeFailed(nan_value, "NaN length");

    mcp::JsonValue integral_double = base;
    integral_double["offset"] = 2.0;
    integral_double["length"] = 4.0;
    const mcp::FileRef accepted = mcp::DeserializeFileRef(integral_double);
    Check(accepted.offset == 2, "integral double offset accepted");
    Check(accepted.length == 4, "integral double length accepted");
}

void CheckResourceLinkName() {
    mcp::ResourceLink missing_name;
    missing_name.uri = "file:///data.bin";
    bool missing_name_threw = false;
    try {
        (void)mcp::SerializeResourceLink(missing_name);
    } catch (const mcp::McpError& error) {
        missing_name_threw = error.Code() == mcp::McpErrorCode::InvalidParams;
    }
    Check(missing_name_threw, "ResourceLink without name throws InvalidParams");

    mcp::ResourceLink link;
    link.uri = "file:///data.bin";
    link.name = "data.bin";
    link.description = "binary data";
    link.mime_type = "application/octet-stream";
    link.size = 128;
    link.icons.push_back(mcp::Icon{"https://example.com/data.png"});
    const mcp::JsonValue serialized = mcp::SerializeResourceLink(link);
    Check(serialized.Find("name") != nullptr, "ResourceLink name serialized");
    Check(serialized.Find("description") != nullptr,
          "ResourceLink description serialized");
    Check(serialized.Find("mimeType") != nullptr, "ResourceLink mimeType serialized");
    Check(serialized.Find("size") != nullptr, "ResourceLink size serialized");
    Check(serialized.Find("icons") != nullptr, "ResourceLink icons serialized");
}

void CheckCompleteContext() {
    mcp::CompleteRequestParams params;
    params.ref = mcp::JsonValue::Object{{"type", "ref/prompt"}, {"name", "weather"}};
    params.argument_name = "city";
    params.argument_value = "Shanghai";
    params.context_arguments = {{"locale", "zh-CN"}};
    const mcp::JsonValue serialized = mcp::SerializeCompleteRequestParams(params);
    const mcp::JsonValue* context = serialized.Find("context");
    Check(context != nullptr, "CompleteRequestParams context serialized");
    if (context != nullptr) {
        const mcp::JsonValue* arguments = context->Find("arguments");
        Check(arguments != nullptr, "CompleteRequestParams context.arguments serialized");
        if (arguments != nullptr) {
            const mcp::JsonValue* locale = arguments->Find("locale");
            Check(locale != nullptr && locale->IsString() &&
                      locale->GetString() == "zh-CN",
                  "CompleteRequestParams context argument value");
        }
    }
}

} // namespace

int main() {
    CheckFileRefStrictness();
    CheckResourceLinkName();
    CheckCompleteContext();
    if (failures != 0) {
        std::cerr << "compat runtime: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "compat runtime: all checks passed\n";
    return 0;
}

#elif defined(MCP_COMPAT_PROBE_MODE)

#if defined(MCP_COMPAT_PROBE_RESOURCE_LINK_AGGREGATE)
void Probe() {
    std::optional<std::string> title = "report";
    mcp::ResourceLink link{"resource_link", "file:///report", title,
                           std::nullopt, std::nullopt};
    (void)link;
}
#endif

#if defined(MCP_COMPAT_PROBE_CLIENT_OPTIONS_APPS_EXTENSION)
void Probe() {
    mcp::ClientOptions options;
    options.enable_apps_extension = true;
}
#endif

#if defined(MCP_COMPAT_PROBE_CLIENT_OPTIONS_AGGREGATE)
void Probe() {
    mcp::ClientOptions options{
        mcp::Implementation{"client", "1.0"},
        std::nullopt,
        mcp::ConnectMode::Auto};
    (void)options;
}
#endif

#if defined(MCP_COMPAT_PROBE_SERVER_OPTIONS_LEGACY_SHIM)
void Probe() {
    mcp::ServerOptions options;
    options.input_required_config.emplace();
    options.input_required_config->legacy_shim = false;
}
#endif

#if defined(MCP_COMPAT_PROBE_SERVER_OPTIONS_ENABLE_DPOP)
void Probe() {
    mcp::ServerOptions options;
    options.enable_dpop = true;
}
#endif

#if defined(MCP_COMPAT_PROBE_SERVER_OPTIONS_INTERCEPTOR_ALIAS)
void Probe() {
    mcp::ServerOptions options;
    options.enable_interceptor_alias = true;
}
#endif

#if defined(MCP_COMPAT_PROBE_CLIENT_OPTIONS_URL_HANDLER_VOID)
void Probe() {
    mcp::ClientOptions options;
    options.url_elicitation_handler =
        [](const mcp::ElicitRequestParams&) {};
}
#endif

#if defined(MCP_COMPAT_PROBE_SET_URL_HANDLER_VOID)
void Probe(mcp::McpClient& client) {
    client.SetUrlElicitationHandler(
        [](const mcp::ElicitRequestParams&) {});
}
#endif

#if defined(MCP_COMPAT_PROBE_GET_CAPABILITIES_SIGNATURE)
void Probe() {
    const mcp::ServerCapabilities& (mcp::McpServer::*getter)() const =
        &mcp::McpServer::GetCapabilities;
    (void)getter;
}
#endif

#if defined(MCP_COMPAT_PROBE_COMPLETE_REQUEST_PARAMS_AGGREGATE)
void Probe() {
    mcp::CompleteRequestParams params{
        mcp::JsonValue::Object{}, "name", "value", std::nullopt};
    (void)params;
}
#endif

#if defined(MCP_COMPAT_PROBE_TASK_STATE_AGGREGATE)
void Probe() {
    mcp::TaskState state{"task", mcp::TaskStatus::Pending};
    (void)state;
}
#endif

#if defined(MCP_COMPAT_PROBE_PENDING_REQUEST_AGGREGATE)
void Probe() {
    mcp::PendingRequest pending{
        [](mcp::JsonValue) {},
        std::chrono::steady_clock::now(),
        std::nullopt};
    (void)pending;
}
#endif

#else

namespace {

class CompatTransport final : public mcp::ITransport {
public:
    std::string_view SessionId() const override { return {}; }
    mcp::MessageChannel& GetMessageChannel() override { return channel_; }
    void SendMessageAsync(mcp::JsonRpcMessage) override {}
    void Close() override {}

private:
    mcp::MessageChannel channel_{8};
};

class CompatTokenCache final : public mcp::ITokenCache {
public:
    void StoreTokens(const mcp::TokenContainer& tokens) override {
        tokens_ = tokens;
    }
    std::optional<mcp::TokenContainer> GetTokens() override {
        return tokens_;
    }
    void ClearTokens() override { tokens_.reset(); }

private:
    std::optional<mcp::TokenContainer> tokens_;
};

class CompatTaskStore final : public mcp::IMcpTaskStore {
public:
    mcp::TaskState CreateTask(const std::string& task_id) override {
        mcp::TaskState state;
        state.task_id = task_id;
        return state;
    }
    std::optional<mcp::TaskState> GetTask(const std::string&) override {
        return std::nullopt;
    }
    bool UpdateTask(const std::string&,
                    const std::optional<mcp::JsonValue>&) override {
        return false;
    }
    bool CancelTask(const std::string&,
                    const std::optional<std::string>&) override {
        return false;
    }
    bool SetTaskStatus(const std::string&, mcp::TaskStatus) override {
        return false;
    }
    std::vector<mcp::TaskState> GetAllTasks() override { return {}; }
};

class CompatWireCodec final : public mcp::WireCodec {
public:
    bool HasRequestMethod(std::string_view) const override { return true; }
    bool HasNotificationMethod(std::string_view) const override { return true; }
    mcp::WireValidation ValidateRequest(
        std::string_view, const mcp::JsonValue&) const override {
        return mcp::WireValidation::Ok;
    }
    mcp::WireValidation ValidateResponse(
        std::string_view, const mcp::JsonValue&) const override {
        return mcp::WireValidation::Ok;
    }
    mcp::WireValidation ValidateNotification(
        std::string_view, const mcp::JsonValue&) const override {
        return mcp::WireValidation::Ok;
    }
    void StampOutgoingRequest(
        mcp::JsonValue&, const mcp::RequestMeta&) const override {}
    mcp::JsonValue EncodeResult(
        std::string_view, const mcp::JsonValue& result) const override {
        return result;
    }
    int32_t EncodeErrorCode(int32_t code) const override { return code; }
    std::string_view Era() const override { return "compat"; }
};

void PreviousPublicApiBaseline() {
    mcp::ResourceLink link;
    link.uri = "file:///report";
    link.name = "report";
    link.description = "report data";
    link.mime_type = "text/plain";
    link.size = 1;
    link.icons.push_back(mcp::Icon{"https://example.com/report.png"});
    (void)mcp::SerializeResourceLink(link);

    mcp::CompleteRequestParams complete;
    complete.ref = mcp::JsonValue::Object{{"type", "ref/prompt"}, {"name", "p"}};
    complete.argument_name = "arg";
    complete.argument_value = "value";
    complete.context_arguments.emplace("locale", "en");
    (void)mcp::SerializeCompleteRequestParams(complete);

    mcp::FileRef file_ref;
    file_ref.uri = "file:///data";
    file_ref.offset = 1;
    file_ref.length = 2;
    static_assert(std::is_same_v<decltype(mcp::FileRef::offset), int64_t>);
    static_assert(std::is_same_v<decltype(mcp::FileRef::length), int64_t>);

    mcp::TaskState task;
    task.task_id = "task";
    task.owner = "session";
    task.status = mcp::TaskStatus::Pending;

    mcp::ClientOptions client_options;
    client_options.elicitation_handler =
        [](const mcp::ElicitRequestParams&) { return mcp::ElicitResult{}; };
    client_options.url_elicitation_handler =
        client_options.elicitation_handler;
    client_options.enable_etag_cache = true;
    client_options.cache_auth_context = [] { return std::string("ctx"); };
    client_options.on_transport_close = [] {};
    client_options.on_transport_error = [](std::string_view) {};
    static_assert(std::is_same_v<
                  decltype(&mcp::McpServer::GetCapabilities),
                  mcp::ServerCapabilities (mcp::McpServer::*)() const>);

    mcp::ServerOptions server_options;
    server_options.enable_apps_extension = true;
    server_options.input_required_config.emplace();
    server_options.input_required_config->max_rounds = 3;
    server_options.incoming_filters = std::make_shared<mcp::FilterPipeline>();

    mcp::StreamableHttpServerOptions http_options;
    http_options.listen_hold_open = true;
    http_options.bearer_auth.emplace();
    http_options.bearer_auth->enable_dpop = true;

    mcp::OAuthClientOptions oauth_options;
    oauth_options.allowed_authorization_server_origins.push_back(
        "https://auth.example.com");
    static_assert(std::is_same_v<
                  decltype(&mcp::OAuthClientProvider::IsSecureEndpoint),
                  bool (*)(std::string_view)>);
    static_assert(std::is_same_v<
                  decltype(&mcp::OAuthClientProvider::IsSameOrigin),
                  bool (*)(std::string_view, std::string_view)>);

    mcp::PendingRequest pending;
    pending.method = "tools/call";
    pending.request_id = "1";
    (void)pending;

    using SetUrlHandler = void (mcp::McpClient::*)(mcp::UrlElicitationHandler);
    SetUrlHandler set_url_handler = &mcp::McpClient::SetUrlElicitationHandler;
    (void)set_url_handler;
    using SetLegacyUrlHandler = void (mcp::McpClient::*)(
        std::function<void(const mcp::ElicitRequestParams&)>);
    SetLegacyUrlHandler set_legacy_url_handler =
        &mcp::McpClient::SetLegacyUrlElicitationHandler;
    (void)set_legacy_url_handler;

    CompatTransport transport;
    CompatTokenCache token_cache;
    CompatTaskStore task_store;
    CompatWireCodec codec;
    (void)transport;
    (void)token_cache;
    (void)task_store;

    const mcp::WireMessageView request_view =
        mcp::MakeWireRequestView(mcp::JsonRpcRequest{});
    const mcp::WireMessageView notification_view =
        mcp::MakeWireNotificationView(mcp::JsonRpcNotification{});
    (void)request_view;
    (void)notification_view;
    mcp::WireCodec& codec_base = codec;
    (void)codec_base.ValidateRequest("tools/list", request_view);
    (void)codec_base.ValidateNotification(
        "notifications/progress", notification_view);
}

} // namespace

#endif
