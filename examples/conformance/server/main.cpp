#include "ConformanceServer.hpp"

#include <mcp/Methods.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/ProtocolVersion.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>

#include <mcp/http/EventStore.hpp>

#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <string_view>

namespace {

constexpr uint16_t kDefaultConformancePort = 3010;

std::string GenerateRequestStateKey()
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::mt19937_64 engine(std::random_device{}());
    std::string key;
    key.reserve(64);
    for (int i = 0; i < 64; ++i) {
        key.push_back(kHex[engine() % 16]);
    }
    return key;
}

void SendConformanceLog(mcp::McpSessionHandler& session, std::string data)
{
    mcp::JsonValue params(mcp::JsonValue::object_tag);
    params["level"] = mcp::JsonValue("info");
    params["logger"] = mcp::JsonValue("conformance-test-server");
    params["data"] = mcp::JsonValue(std::move(data));
    session.SendNotification(mcp::notifications::kMessage, std::move(params));
}

uint16_t ParsePort(int argc, char** argv)
{
    uint16_t port = kDefaultConformancePort;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
    }
    return port;
}

bool ParseStatelessFlag(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--stateless") return true;
    }
    return false;
}

void WireCompletionHandler(mcp::McpServer& server)
{
    server.SetCompletionHandler([](const mcp::CompleteRequestParams&) {
        mcp::CompleteResult result;
        mcp::JsonValue completion(mcp::JsonValue::object_tag);
        completion["values"] = mcp::JsonValue(mcp::JsonValue::array_tag);
        completion["total"] = mcp::JsonValue(static_cast<int64_t>(0));
        completion["hasMore"] = mcp::JsonValue(false);
        result.completion = std::move(completion);
        return result;
    });
}

void WireLoggingSetLevelNotice(mcp::McpServer& server)
{
    server.GetSessionHandler().SetOnRequestCallback(
        [&session = server.GetSessionHandler()](
            std::string_view method, const mcp::JsonRpcRequest& req) {
            if (method != mcp::methods::kSetLoggingLevel) return;
            std::string level = "unknown";
            if (req.params) {
                if (auto* value = req.params->Find("level"); value && value->IsString()) {
                    level = value->GetString();
                }
            }
            SendConformanceLog(session, "Log level set to: " + level);
        });
}

void WireResourceSubscriptionHandlers(mcp::McpServer& server)
{
    auto& session = server.GetSessionHandler();

    session.SetRequestHandler(mcp::methods::kSubscribeResource,
        [&session](const mcp::JsonRpcRequest& req, std::promise<mcp::JsonValue> promise) {
            std::string uri;
            if (req.params) {
                if (auto* value = req.params->Find("uri"); value && value->IsString()) {
                    uri = value->GetString();
                }
            }
            session.AddSubscription(mcp::Subscription{uri, {}});
            SendConformanceLog(session, "Subscribed to resource: " + uri);
            promise.set_value(mcp::SerializeEmptyResult(mcp::EmptyResult{}));
        });

    session.SetRequestHandler(mcp::methods::kUnsubscribeResource,
        [&session](const mcp::JsonRpcRequest& req, std::promise<mcp::JsonValue> promise) {
            std::string uri;
            if (req.params) {
                if (auto* value = req.params->Find("uri"); value && value->IsString()) {
                    uri = value->GetString();
                }
            }
            session.RemoveSubscription(uri);
            SendConformanceLog(session, "Unsubscribed from resource: " + uri);
            promise.set_value(mcp::SerializeEmptyResult(mcp::EmptyResult{}));
        });
}

} // namespace

int main(int argc, char** argv)
{
    using namespace mcp;
    using namespace mcp::conformance;

    const uint16_t port = ParsePort(argc, argv);
    const bool stateless = ParseStatelessFlag(argc, argv);

    StreamableHttpServerOptions transport_options;
    transport_options.port = port;
    transport_options.endpoint = "/mcp";
    transport_options.stateless = stateless;
    transport_options.enable_legacy_sse = true;
    transport_options.event_store = std::make_shared<EventStore>();
    transport_options.session_store = nullptr;
    transport_options.server_name = "mcp-conformance-test-server";
    transport_options.server_version = "1.0.0";

    McpServer* server_ptr = nullptr;
    transport_options.resolve_param_annotations =
        [&server_ptr](const std::string& method, const std::string& name) {
            if (!server_ptr) return std::vector<McpParamAnnotationInfo>{};
            return server_ptr->ResolveToolParamAnnotations(method, name);
        };

    auto transport = std::make_shared<StreamableHttpServerTransport>(transport_options);

    ServerOptions server_options;
    server_options.server_info = Implementation{"mcp-conformance-test-server", "1.0.0"};
    if (!stateless) {
        server_options.protocol_version = std::string(kLegacyProtocolVersion);
    }
    server_options.declare_logging = true;
    server_options.declare_completions = true;
    server_options.request_state_key = GenerateRequestStateKey();
    if (stateless) {
        server_options.task_store = MakeConformanceTaskStore();
        std::map<std::string, CacheHint, std::less<>> cache_hints;
        for (const char* method : {"tools/list", "prompts/list",
                "resources/list", "resources/templates/list", "resources/read",
                "skills/list", "skills/get", "resources/directory/read",
                "server/discover"}) {
            CacheHint hint;
            hint.ttl_ms = 60000;
            hint.cache_scope = "public";
            cache_hints[method] = hint;
        }
        server_options.cache_hints = std::move(cache_hints);
    }

    auto server = McpServer::Create(transport, server_options);
    server_ptr = server.get();

    RegisterContentTools(*server);
    RegisterInteractionTools(*server);
    RegisterMrtrTools(*server);
    RegisterConformanceResources(*server);
    RegisterConformancePrompts(*server);
    if (stateless) {
        RegisterConformanceSkills(*server);
        RegisterConformanceTaskTools(*server);
    }

    WireCompletionHandler(*server);
    WireLoggingSetLevelNotice(*server);
    WireResourceSubscriptionHandlers(*server);

    std::cerr << "MCP Conformance Test Server running on http://localhost:" << port << std::endl;
    std::cerr << "  - MCP endpoint: http://localhost:" << port << "/mcp" << std::endl;
    std::cerr << "  - mode: " << (stateless ? "stateless (2026-07-28)" : "stateful (2025-11-25)") << std::endl;

    server->Run();
    return 0;
}
