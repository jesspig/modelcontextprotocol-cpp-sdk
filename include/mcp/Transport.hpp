#pragma once

#include <memory>
#include <string_view>

namespace mcp {

class Transport {
public:
    virtual ~Transport() = default;

    virtual std::string_view SessionId() const = 0;
    virtual void Start() = 0;
    virtual void SendMessage(const void* message) = 0;
    virtual void Close() = 0;
};

class ClientTransport {
public:
    virtual ~ClientTransport() = default;
    virtual std::string_view Name() const = 0;
    virtual std::unique_ptr<Transport> Connect() = 0;
};

} // namespace mcp
