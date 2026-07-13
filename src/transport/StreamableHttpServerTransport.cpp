#include <mcp/transport/StreamableHttpServerTransport.hpp>

namespace mcp {

StreamableHttpServerTransport::StreamableHttpServerTransport() = default;
StreamableHttpServerTransport::~StreamableHttpServerTransport() = default;

void StreamableHttpServerTransport::Start() {}
void StreamableHttpServerTransport::Close() {}
void StreamableHttpServerTransport::SendMessageAsync(JsonRpcMessage) {}
Transport::MessageChannel& StreamableHttpServerTransport::GetMessageChannel() {
    static MessageChannel ch(*new asio::io_context, 16);
    return ch;
}
asio::io_context& StreamableHttpServerTransport::IoContext() {
    static asio::io_context ctx;
    return ctx;
}

} // namespace mcp
