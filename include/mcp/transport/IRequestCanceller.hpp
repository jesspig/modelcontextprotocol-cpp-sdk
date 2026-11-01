#pragma once

#include <mcp/Export.hpp>
#include <mcp/JsonRpc.hpp>

#include <string_view>

namespace mcp {

class MCP_API IRequestCanceller {
public:
    virtual ~IRequestCanceller() = default;
    virtual void CancelRequest(const RequestId& id, std::string_view reason) = 0;
};

} // namespace mcp
