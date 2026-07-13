#include <mcp/transport/StreamableHttpServerTransport.hpp>

namespace mcp {

StreamableHttpServerTransport::StreamableHttpServerTransport() = default;
StreamableHttpServerTransport::~StreamableHttpServerTransport() = default;

std::string_view StreamableHttpServerTransport::SessionId() const { return {}; }
void StreamableHttpServerTransport::Start() {}
void StreamableHttpServerTransport::SendMessage(const void*) {}
void StreamableHttpServerTransport::Close() {}

} // namespace mcp
