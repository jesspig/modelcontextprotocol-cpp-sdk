#pragma once

#include <mcp/Export.hpp>

#include <mcp/JsonValue.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Meta.hpp>
#include <mcp/Transport.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

namespace mcp {

class McpServer;

template <typename TParams>
class MCP_API RequestContext {
public:
    using LogFn = std::function<void(LoggingLevel, std::string_view)>;

    RequestContext(
        McpServer& server,
        const JsonRpcRequest& jsonrpc_request,
        TParams params,
        LogFn log_fn = nullptr,
        std::shared_ptr<const std::atomic<bool>> cancellation_flag = nullptr)
        : server_(&server)
        , jsonrpc_request_(jsonrpc_request)
        , params_(std::move(params))
        , log_fn_(std::move(log_fn))
        , cancellation_flag_(std::move(cancellation_flag))
    {
        if (jsonrpc_request_.meta) {
            auto* lv = jsonrpc_request_.meta->Find("io.modelcontextprotocol/logLevel");
            if (lv && lv->IsInt()) {
                log_level_ = static_cast<LoggingLevel>(lv->GetInt());
            }
        }
    }

    const TParams& Params() const { return params_; }
    McpServer& Server() const { return *server_; }
    const mcp::JsonRpcRequest& GetRequest() const { return jsonrpc_request_; }
    std::optional<LoggingLevel> LogLevel() const { return log_level_; }

    void Log(LoggingLevel level, std::string_view data) const {
        if (log_level_ && static_cast<int>(level) < static_cast<int>(*log_level_))
            return;
        if (log_fn_) log_fn_(level, data);
    }

    bool IsCancellationRequested() const {
        return cancellation_flag_ && cancellation_flag_->load();
    }

private:
    McpServer* server_;
    JsonRpcRequest jsonrpc_request_;
    TParams params_;
    std::optional<LoggingLevel> log_level_;
    LogFn log_fn_;
    std::shared_ptr<const std::atomic<bool>> cancellation_flag_;
};

} // namespace mcp
