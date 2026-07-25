#pragma once
// HttpTransportMode.hpp — HTTP transport mode selection

#include <string_view>

namespace mcp {

// HTTP transport mode.
// Matches C# HttpTransportMode enum.
enum class HttpTransportMode {
    AutoDetect,    // Try StreamableHttp first, fall back to SSE
    StreamableHttp,
    Sse
};

} // namespace mcp
