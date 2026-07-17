#pragma once

#include <mcp/Export.hpp>

#include <mcp/Transport.hpp>
#include <memory>

namespace mcp {

class MCP_API InMemoryTransport {
public:
struct Pair {
        std::shared_ptr<ITransport> client;
        std::shared_ptr<ITransport> server;
    };

    static Pair CreatePair();
};

} // namespace mcp
