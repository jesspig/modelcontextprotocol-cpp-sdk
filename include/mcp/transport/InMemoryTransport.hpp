#pragma once

#include <mcp/Transport.hpp>
#include <memory>

namespace mcp {

class InMemoryTransport {
public:
    struct Pair {
        std::unique_ptr<Transport> client;
        std::unique_ptr<Transport> server;
    };

    static Pair CreatePair();
};

} // namespace mcp
