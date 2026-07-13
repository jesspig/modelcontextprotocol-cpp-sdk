#include <mcp/transport/SseClientTransport.hpp>

namespace mcp {

SseClientTransport::SseClientTransport() = default;
SseClientTransport::~SseClientTransport() = default;

std::string_view SseClientTransport::Name() const { return {}; }
std::unique_ptr<Transport> SseClientTransport::Connect() { return nullptr; }

} // namespace mcp
