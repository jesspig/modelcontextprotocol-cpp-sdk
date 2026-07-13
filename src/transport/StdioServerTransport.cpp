#include <mcp/transport/StdioServerTransport.hpp>

namespace mcp {

StdioServerTransport::StdioServerTransport() = default;
StdioServerTransport::~StdioServerTransport() = default;

std::string_view StdioServerTransport::SessionId() const { return {}; }
void StdioServerTransport::Start() {}
void StdioServerTransport::SendMessage(const void*) {}
void StdioServerTransport::Close() {}

} // namespace mcp
