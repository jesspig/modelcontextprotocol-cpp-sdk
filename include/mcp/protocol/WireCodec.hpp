#pragma once
#include <mcp/Export.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/JsonValue.hpp>

#include <memory>
#include <string_view>

namespace mcp {

enum class WireValidation {
    Ok,
    NotInEra,
    Invalid,
};

class MCP_API WireCodec {
public:
    virtual ~WireCodec() = default;

    virtual bool HasRequestMethod(std::string_view method) const = 0;
    virtual bool HasNotificationMethod(std::string_view method) const = 0;

    virtual WireValidation ValidateRequest(
        std::string_view method, const JsonValue& raw) const = 0;
    virtual WireValidation ValidateResponse(
        std::string_view method, const JsonValue& raw) const = 0;
    virtual WireValidation ValidateNotification(
        std::string_view method, const JsonValue& raw) const = 0;

    virtual void StampOutgoingRequest(
        JsonValue& request_body,
        const RequestMeta& meta) const = 0;

    virtual std::optional<RequestMeta> ExtractIncomingMeta(
        const JsonValue&) const { return std::nullopt; }

    virtual JsonValue EncodeResult(
        std::string_view method, const JsonValue& result) const = 0;

    virtual int32_t EncodeErrorCode(int32_t code) const = 0;

    virtual std::string_view Era() const = 0;
};

MCP_API std::unique_ptr<WireCodec> MakeWireCodec(std::string_view protocol_version);

} // namespace mcp
