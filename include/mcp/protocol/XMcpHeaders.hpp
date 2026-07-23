#pragma once

#include <mcp/JsonValue.hpp>

#include <map>
#include <string>
#include <string_view>

namespace mcp {

// Scan a JSON Schema (inputSchema) for x-mcp-header declarations.
// Returns a map: paramName -> headerName (or empty string as flag).
// The x-mcp-header value may be:
//   - a string: the exact header name to use (e.g., "Mcp-Param-MyParam")
//   - true/absent: header name defaults to "Mcp-Param-{ParamName}"
inline std::map<std::string, std::string> ScanXMcpHeaders(const JsonValue& schema) {
    std::map<std::string, std::string> result;
    auto* props = schema.Find("properties");
    if (!props || !props->IsObject()) return result;

    for (auto it = props->begin(); it != props->end(); ++it) {
        const auto& prop = it->second;
        if (!prop.IsObject()) continue;

        auto* header_it = prop.Find("x-mcp-header");
        if (!header_it) continue;

        std::string param_name = it->first;
        if (header_it->IsString()) {
            result[param_name] = header_it->GetString();
        } else {
            result[param_name] = "Mcp-Param-" + param_name;
        }
    }
    return result;
}

inline std::map<std::string, std::string> ExtractXMcpHeaderValues(
    const JsonValue& params,
    const std::map<std::string, std::string>& xmcp_decls)
{
    std::map<std::string, std::string> result;
    if (!params.IsObject()) return result;

    for (const auto& [param_name, header_name] : xmcp_decls) {
        auto* val = params.Find(param_name);
        if (!val) continue;

        std::string str_val;
        if (val->IsString())         str_val = val->GetString();
        else if (val->IsInt())       str_val = std::to_string(val->GetInt());
        else if (val->IsBool())      str_val = val->GetBool() ? "true" : "false";
        else continue;

        result[header_name] = str_val;
    }
    return result;
}

} // namespace mcp
