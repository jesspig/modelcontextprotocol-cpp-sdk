#pragma once

#include <mcp/Export.hpp>

#include <mcp/JsonValue.hpp>
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
class MCP_API RequestContext {
public:
    using LogFn = std::function<void(LoggingLevel, std::string_view)>;

    RequestContext(
        McpServer& server,
        const JsonRpcRequest& jsonrpc_request,
        TParams params,
        LogFn log_fn = nullptr)
        : server_(&server)
        , jsonrpc_request_(&jsonrpc_request)
        , params_(std::move(params))
        , log_fn_(std::move(log_fn))
    {
        if (jsonrpc_request_->meta) {
            auto* lv = jsonrpc_request_->meta->Find("io.modelcontextprotocol/logLevel");
            if (lv && lv->IsInt()) {
                log_level_ = static_cast<LoggingLevel>(lv->GetInt());
            }
        }
    }

    const TParams& Params() const { return params_; }
    McpServer& Server() const { return *server_; }
    const mcp::JsonRpcRequest& GetRequest() const { return *jsonrpc_request_; }
    std::optional<LoggingLevel> LogLevel() const { return log_level_; }

    void Log(LoggingLevel level, std::string_view data) const {
        if (log_level_ && static_cast<int>(level) < static_cast<int>(*log_level_))
            return;
        if (log_fn_) log_fn_(level, data);
    }

private:
    McpServer* server_;
    const JsonRpcRequest* jsonrpc_request_;
    TParams params_;
    std::optional<LoggingLevel> log_level_;
    LogFn log_fn_;
};

} // namespace mcp
