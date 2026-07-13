#pragma once

#include <mcp/Transport.hpp>

namespace mcp {

class StreamableHttpServerTransport : public Transport {
public:
    StreamableHttpServerTransport();
    ~StreamableHttpServerTransport() override;

    std::string_view SessionId() const override;
    void Start() override;
    void SendMessage(const void* message) override;
    void Close() override;
};

} // namespace mcp
