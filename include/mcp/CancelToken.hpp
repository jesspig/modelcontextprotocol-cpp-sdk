#pragma once

#include <mcp/Export.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mcp {

class MCP_API CancelToken {
public:
    using CancelHandler = std::function<void(std::string_view)>;

    CancelToken();

    void Cancel(std::string_view reason = {});
    bool IsCancelled() const;
    std::string Reason() const;

    std::size_t AddCancelHandler(CancelHandler handler);
    void RemoveCancelHandler(std::size_t handler_id);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace mcp
