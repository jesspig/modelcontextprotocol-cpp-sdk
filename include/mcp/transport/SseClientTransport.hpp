#pragma once

#include <mcp/Export.hpp>

#include <mcp/Transport.hpp>
#include <string>

namespace mcp {

class MCP_API SseClientTransport : public IClientTransport {
public:
    explicit SseClientTransport(std::string_view server_url, std::string_view name = {});
    ~SseClientTransport() override;

    std::string_view Name() const override;
    std::shared_ptr<ITransport> Connect() override;

private:
    std::string server_url_;
    std::string name_;
};

} // namespace mcp
