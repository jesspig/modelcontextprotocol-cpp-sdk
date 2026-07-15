#pragma once

#include <string_view>

namespace mcp::headers {

// ── MCP Protocol Headers (SEP-2243) ──
inline constexpr std::string_view kMcpProtocolVersion = "MCP-Protocol-Version";
inline constexpr std::string_view kMcpMethod = "Mcp-Method";
inline constexpr std::string_view kMcpName = "Mcp-Name";
inline constexpr std::string_view kMcpSessionId = "Mcp-Session-Id";
inline constexpr std::string_view kMcpParamPrefix = "Mcp-Param-";
inline constexpr std::string_view kLastEventId = "Last-Event-ID";
inline constexpr std::string_view kContentType = "Content-Type";
inline constexpr std::string_view kAccept = "Accept";

// ── Content Types ──
inline constexpr std::string_view kApplicationJson = "application/json";
inline constexpr std::string_view kTextEventStream = "text/event-stream";

// ── Base64 sentinel ──
inline constexpr std::string_view kBase64Prefix = "=?base64?";
inline constexpr std::string_view kBase64Suffix = "?=";

} // namespace mcp::headers
