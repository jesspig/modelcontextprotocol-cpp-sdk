#include <mcp/transport/StdioServerTransport.hpp>

namespace mcp {

StdioServerTransport::StdioServerTransport() = default;
StdioServerTransport::~StdioServerTransport() = default;

void StdioServerTransport::Start() {}
void StdioServerTransport::Close() {}
void StdioServerTransport::SendMessageAsync(JsonRpcMessage) {}
Transport::MessageChannel& StdioServerTransport::GetMessageChannel() {
    static MessageChannel ch(*new asio::io_context, 16);
    return ch;
}
asio::io_context& StdioServerTransport::IoContext() {
    static asio::io_context ctx;
    return ctx;
}

} // namespace mcp
