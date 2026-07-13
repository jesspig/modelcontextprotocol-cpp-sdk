#include <mcp/server/McpServer.hpp>
#include <mcp/Transport.hpp>

namespace mcp {

class McpServerImpl : public McpServer {
public:
    explicit McpServerImpl(std::unique_ptr<Transport> transport) {}

    void Run() override {}
    void Close() override {}

    void RegisterTool(
        std::string_view name,
        const void* options,
        void* handler) override {}
};

std::unique_ptr<McpServer> McpServer::Create(
    std::unique_ptr<Transport> transport) {
    return std::make_unique<McpServerImpl>(std::move(transport));
}

} // namespace mcp
