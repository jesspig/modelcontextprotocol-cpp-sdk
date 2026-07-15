#pragma once

#include <mcp/Transport.hpp>
#include <memory>

namespace mcp {

class InMemoryTransport {
public:
struct Pair {
        std::shared_ptr<ITransport> client;
        std::shared_ptr<ITransport> server;
    };

    static Pair CreatePair();
};

} // namespace mcp
