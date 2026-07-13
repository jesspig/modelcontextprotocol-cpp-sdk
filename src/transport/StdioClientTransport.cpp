#include <mcp/transport/StdioClientTransport.hpp>

namespace mcp {

StdioClientTransport::StdioClientTransport(const StdioClientTransportOptions& options)
    : options_(options) {}

StdioClientTransport::~StdioClientTransport() = default;

std::string_view StdioClientTransport::Name() const { return options_.name; }

std::unique_ptr<Transport> StdioClientTransport::Connect() {
    return nullptr;
}

} // namespace mcp
