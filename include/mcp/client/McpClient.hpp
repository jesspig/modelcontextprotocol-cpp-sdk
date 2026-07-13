#pragma once

#include <memory>

namespace mcp {

class McpClient {
public:
    template <typename CompletionToken>
    static auto Create(
        std::unique_ptr<class ClientTransport> transport,
        const void* options = nullptr,
        CompletionToken&& token = {});

    virtual ~McpClient() = default;

    virtual void Connect() = 0;
    virtual void Close() = 0;
};

} // namespace mcp
