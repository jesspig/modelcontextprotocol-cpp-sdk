#pragma once

#include <mcp/ErrorCodes.hpp>
#include <mcp/JsonRpc.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace mcp {

class McpError : public std::runtime_error {
public:
    explicit McpError(McpErrorCode code, std::string message)
        : std::runtime_error(message)
        , code_(code)
        , message_(std::move(message)) {}

    McpError(McpErrorCode code, std::string message, std::optional<JsonValue> data)
        : std::runtime_error(message)
        , code_(code)
        , message_(std::move(message))
        , data_(std::move(data)) {}

    McpErrorCode Code() const noexcept { return code_; }
    std::string_view ErrorMessage() const noexcept { return message_; }
    const std::optional<JsonValue>& Data() const noexcept { return data_; }


private:
    McpErrorCode code_;
    std::string message_;
    std::optional<JsonValue> data_;
};

struct UrlElicitationRequiredData {
    std::string mode;
    std::string elicitation_id;
    std::string url;
    std::string message;
};

inline std::optional<UrlElicitationRequiredData> ParseUrlElicitationRequired(
    const McpError& error)
{
    if (error.Code() != McpErrorCode::UrlElicitationRequired) {
        return std::nullopt;
    }
    const auto& data = error.Data();
    if (!data.has_value() || !data->IsObject()) {
        return std::nullopt;
    }
    const JsonValue* elicitations = data->Find("elicitations");
    if (elicitations == nullptr || !elicitations->IsArray()) {
        return std::nullopt;
    }
    for (const JsonValue& item : elicitations->GetArray()) {
        if (!item.IsObject()) {
            continue;
        }
        const JsonValue* mode = item.Find("mode");
        const JsonValue* id = item.Find("elicitationId");
        const JsonValue* url = item.Find("url");
        const JsonValue* message = item.Find("message");
        if (mode == nullptr || !mode->IsString() ||
            id == nullptr || !id->IsString() ||
            url == nullptr || !url->IsString() ||
            message == nullptr || !message->IsString()) {
            continue;
        }
        return UrlElicitationRequiredData{
            mode->GetString(), id->GetString(), url->GetString(),
            message->GetString()};
    }
    return std::nullopt;
}

} // namespace mcp
