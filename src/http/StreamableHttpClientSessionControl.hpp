#pragma once

namespace mcp {

class IStreamableHttpSessionControl {
public:
    virtual ~IStreamableHttpSessionControl() = default;
    virtual void SuspendSends() = 0;
};

} // namespace mcp
