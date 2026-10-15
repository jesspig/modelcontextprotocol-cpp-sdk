#pragma once

#include <string_view>

namespace mcp {

enum class HttpTransportMode {
    AutoDetect,
    StreamableHttp,
    Sse
};

} // namespace mcp
