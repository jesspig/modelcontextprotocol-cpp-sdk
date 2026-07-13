#pragma once

#include <mcp/Transport.hpp>

namespace mcp {

class SseClientTransport : public ClientTransport {
public:
    SseClientTransport();
    ~SseClientTransport() override;

    std::string_view Name() const override;
    std::unique_ptr<Transport> Connect() override;
};

} // namespace mcp
