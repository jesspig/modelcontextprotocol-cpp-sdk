#pragma once

#include <mcp/Export.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>

#include <functional>
#include <future>
#include <string_view>
#include <utility>

namespace mcp {

struct MCP_API RequestHandle {
    RequestId id;
    std::future<RequestOutcome> future;

    RequestHandle() = default;

    RequestHandle(RequestId request_id,
                  std::future<RequestOutcome> request_future,
                  std::function<void(std::string_view)> cancel)
        : id(std::move(request_id))
        , future(std::move(request_future))
        , cancel_(std::move(cancel))
    {
    }

    RequestHandle(RequestHandle&&) noexcept = default;
    RequestHandle& operator=(RequestHandle&&) noexcept = default;
    RequestHandle(const RequestHandle&) = delete;
    RequestHandle& operator=(const RequestHandle&) = delete;

    void Cancel(std::string_view reason = {}) {
        if (cancel_) cancel_(reason);
    }

private:
    std::function<void(std::string_view)> cancel_;
};

} // namespace mcp
