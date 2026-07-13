#pragma once

#include <string_view>

namespace mcp::headers {

// ── MCP Protocol Headers (SEP-2243) ──
inline constexpr std::string_view McpProtocolVersion = "MCP-Protocol-Version";
inline constexpr std::string_view McpMethod = "Mcp-Method";
inline constexpr std::string_view McpName = "Mcp-Name";
inline constexpr std::string_view McpSessionId = "Mcp-Session-Id";
inline constexpr std::string_view McpParamPrefix = "Mcp-Param-";
inline constexpr std::string_view LastEventId = "Last-Event-ID";
inline constexpr std::string_view ContentType = "Content-Type";
inline constexpr std::string_view Accept = "Accept";

// ── Content Types ──
inline constexpr std::string_view ApplicationJson = "application/json";
inline constexpr std::string_view TextEventStream = "text/event-stream";

// ── Base64 sentinel ──
inline constexpr std::string_view Base64Prefix = "=?base64?";
inline constexpr std::string_view Base64Suffix = "?=";

} // namespace mcp::headers
