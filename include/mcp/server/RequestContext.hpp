#pragma once

#include <mcp/JsonRpc.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Meta.hpp>
#include <mcp/Transport.hpp>

#include <functional>
#include <memory>
#include <string_view>

namespace mcp {

class McpServer;

// ── RequestContext (对应 C# RequestContext<TParams>) ──
template <typename TParams>
class RequestContext {
public:
    using LogFn = std::function<void(LoggingLevel, std::string_view)>;

    RequestContext(
        McpServer& server,
        const JsonRpcRequest& jsonrpc_request,
        TParams params,
        asio::io_context& io_ctx,
        LogFn log_fn = nullptr)
        : server_(&server)
        , jsonrpc_request_(&jsonrpc_request)
        , params_(std::move(params))
        , io_ctx_(&io_ctx)
        , log_fn_(std::move(log_fn))
    {
        // Extract per-request log level from _meta (2026-era)
        if (jsonrpc_request_->meta) {
            auto it = jsonrpc_request_->meta->find("io.modelcontextprotocol/logLevel");
            if (it != jsonrpc_request_->meta->end()) {
                log_level_ = it->get<LoggingLevel>();
            }
        }
    }

    // Accessors
    const TParams& Params() const { return params_; }
    McpServer& Server() const { return *server_; }
    const mcp::JsonRpcRequest& GetRequest() const { return *jsonrpc_request_; }
    asio::io_context& IoContext() const { return *io_ctx_; }
    std::optional<LoggingLevel> LogLevel() const { return log_level_; }

    // Send a logging notification filtered by per-request logLevel (2026-era)
    void Log(LoggingLevel level, std::string_view data) const {
        if (log_level_ && static_cast<int>(level) < static_cast<int>(*log_level_))
            return;
        if (log_fn_) log_fn_(level, data);
    }

private:
    McpServer* server_;
    const JsonRpcRequest* jsonrpc_request_;
    TParams params_;
    asio::io_context* io_ctx_;
    std::optional<LoggingLevel> log_level_;
    LogFn log_fn_;
};

} // namespace mcp
