#pragma once

#include <mcp/JsonRpc.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Transport.hpp>

#include <memory>
#include <string_view>

namespace mcp {

class McpServer;

// ── RequestContext (对应 C# RequestContext<TParams>) ──
template <typename TParams>
class RequestContext {
public:
    RequestContext(
        McpServer& server,
        const JsonRpcRequest& jsonrpc_request,
        TParams params,
        asio::io_context& io_ctx)
        : server_(&server)
        , jsonrpc_request_(&jsonrpc_request)
        , params_(std::move(params))
        , io_ctx_(&io_ctx)
    {}

    // Accessors
    const TParams& Params() const { return params_; }
    McpServer& Server() const { return *server_; }
    const mcp::JsonRpcRequest& GetRequest() const { return *jsonrpc_request_; }
    asio::io_context& IoContext() const { return *io_ctx_; }

private:
    McpServer* server_;
    const JsonRpcRequest* jsonrpc_request_;
    TParams params_;
    asio::io_context* io_ctx_;
};

} // namespace mcp
