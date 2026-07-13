#pragma once

#include <memory>
#include <type_traits>

namespace mcp {

class McpServerTool {
public:
    virtual ~McpServerTool() = default;

    template <typename Callable,
              typename = std::enable_if_t<!std::is_same_v<Callable, McpServerTool>>>
    static std::shared_ptr<McpServerTool> Create(
        Callable&& /*callable*/,
        const void* /*options*/ = nullptr) {
        return nullptr;
    }
};

} // namespace mcp
