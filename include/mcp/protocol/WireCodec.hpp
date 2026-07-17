#pragma once

#include <mcp/Export.hpp>
#include <mcp/McpTypes.hpp>

#include <memory>
#include <string_view>

namespace mcp {

// ── Wire validation outcome ──
enum class WireValidation {
    Ok,
    NotInEra,
    Invalid,
};

// ── WireCodec interface — per-era wire vocabulary ──
// Corresponds to C# era gating in McpProtocolVersions
// and TypeScript's WireCodec interface.
class MCP_API WireCodec {
public:
    virtual ~WireCodec() = default;

    // ── Method membership (deletion story per era) ──
    virtual bool HasRequestMethod(std::string_view method) const = 0;
    virtual bool HasNotificationMethod(std::string_view method) const = 0;

    // ── Request validation ──
    virtual WireValidation ValidateRequest(
        std::string_view method, const nlohmann::json& raw) const = 0;
    virtual WireValidation ValidateResponse(
        std::string_view method, const nlohmann::json& raw) const = 0;
    virtual WireValidation ValidateNotification(
        std::string_view method, const nlohmann::json& raw) const = 0;

    // ── Envelope / _meta handling ──
    // Stamp outgoing request with era-appropriate _meta
    virtual void StampOutgoingRequest(
        nlohmann::json& request_body,
        const RequestMeta& meta) const = 0;

    // Extract _meta from incoming request (2026: _meta envelope fields)
    virtual std::optional<RequestMeta> ExtractIncomingMeta(
        const nlohmann::json& request_body) const = 0;

    // ── Result encoding/decoding ──
    // Decode wire result, handling resultType discrimination (2026)
    virtual nlohmann::json DecodeResult(
        std::string_view method, const nlohmann::json& raw) const = 0;

    // Encode result, stamping resultType (2026)
    virtual nlohmann::json EncodeResult(
        std::string_view method, const nlohmann::json& result) const = 0;

    // ── Error code remapping ──
    virtual int32_t EncodeErrorCode(int32_t code) const = 0;

    // ── Protocol version string ──
    virtual std::string_view Era() const = 0;
};

// ── Factory ──
// Returns the correct codec for the negotiated protocol version.
// Falls back to 2025 codec for unknown/legacy versions.
MCP_API std::unique_ptr<WireCodec> MakeWireCodec(std::string_view protocol_version);

} // namespace mcp
