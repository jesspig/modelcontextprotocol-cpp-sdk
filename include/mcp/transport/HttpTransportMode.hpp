#pragma once

#include <string_view>

namespace mcp {

/// HTTP transport mode.
/// Matches C# HttpTransportMode enum.
enum class HttpTransportMode {
    AutoDetect,    // Try StreamableHttp first, fall back to SSE
    StreamableHttp,
    Sse
};

/// Convert HttpTransportMode to string.
inline std::string_view HttpTransportModeToString(HttpTransportMode mode) {
    switch (mode) {
        case HttpTransportMode::AutoDetect: return "AutoDetect";
        case HttpTransportMode::StreamableHttp: return "StreamableHttp";
        case HttpTransportMode::Sse: return "Sse";
    }
    return "Unknown";
}

} // namespace mcp
