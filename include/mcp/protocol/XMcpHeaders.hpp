#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <string_view>

namespace mcp {

// Scan a JSON Schema (inputSchema) for x-mcp-header declarations.
// Returns a map: paramName -> headerName (or empty string as flag).
// The x-mcp-header value may be:
//   - a string: the exact header name to use (e.g., "Mcp-Param-MyParam")
//   - true/absent: header name defaults to "Mcp-Param-{ParamName}"
inline std::map<std::string, std::string> ScanXMcpHeaders(const nlohmann::json& schema) {
    std::map<std::string, std::string> result;
    auto props = schema.find("properties");
    if (props == schema.end() || !props->is_object()) return result;

    for (auto it = props->begin(); it != props->end(); ++it) {
        const auto& prop = it.value();
        if (!prop.is_object()) continue;

        auto header_it = prop.find("x-mcp-header");
        if (header_it == prop.end()) continue;

        std::string param_name = it.key();
        if (header_it->is_string()) {
            result[param_name] = header_it->get<std::string>();
        } else {
            // Boolean true or other truthy value: use default header name
            result[param_name] = "Mcp-Param-" + param_name;
        }
    }
    return result;
}

// Extract parameter values from a JSON-RPC params object for specific x-mcp-header declared params.
// Returns map: headerName -> headerValue (stringified).
inline std::map<std::string, std::string> ExtractXMcpHeaderValues(
    const nlohmann::json& params,
    const std::map<std::string, std::string>& xmcp_decls)
{
    std::map<std::string, std::string> result;
    if (!params.is_object()) return result;

    for (const auto& [param_name, header_name] : xmcp_decls) {
        auto val_it = params.find(param_name);
        if (val_it == params.end()) continue;
        const auto& val = *val_it;

        std::string str_val;
        if (val.is_string())       str_val = val.get<std::string>();
        else if (val.is_number_integer()) str_val = std::to_string(val.get<int64_t>());
        else if (val.is_boolean())  str_val = val.get<bool>() ? "true" : "false";
        else continue; // skip complex types per spec (primitives only)

        result[header_name] = str_val;
    }
    return result;
}

} // namespace mcp
