#pragma once

#include <memory>
#include <string_view>

namespace mcp {

class McpServer {
public:
    static std::unique_ptr<McpServer> Create(
        std::unique_ptr<class Transport> transport);

    virtual ~McpServer() = default;

    virtual void Run() = 0;
    virtual void Close() = 0;

    virtual void RegisterTool(
        std::string_view name,
        const void* options,
        void* handler) = 0;
};

} // namespace mcp
