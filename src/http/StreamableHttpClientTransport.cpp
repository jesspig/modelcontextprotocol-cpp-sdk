#include <mcp/transport/StreamableHttpClientTransport.hpp>

namespace mcp {

StreamableHttpClientTransport::StreamableHttpClientTransport(
    const HttpClientTransportOptions& options)
    : options_(options) {}

StreamableHttpClientTransport::~StreamableHttpClientTransport() = default;

std::string_view StreamableHttpClientTransport::Name() const {
    return options_.name;
}

std::unique_ptr<Transport> StreamableHttpClientTransport::Connect() {
    return nullptr;
}

} // namespace mcp
