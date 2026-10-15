#pragma once

#include <mcp/JsonValue.hpp>

#ifdef GetObject
#pragma push_macro("GetObject")
#undef GetObject
#define MCP_POP_GETOBJECT_MACRO_ANNOT 1
#endif

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace mcp { namespace detail {

inline bool IsParamHeaderTokenChar(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

inline bool IsValidParamHeaderName(std::string_view name) {
    if (name.empty()) return false;
    for (char ch : name) {
        if (!IsParamHeaderTokenChar(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

struct McpParamAnnotation {
    std::vector<std::string> property_path;
    std::string header_name;
};

struct ToolParamAnnotations {
    std::vector<McpParamAnnotation> annotations;
    std::string invalid_reason;

    bool IsValid() const { return invalid_reason.empty(); }
};

inline ToolParamAnnotations ParseToolParamAnnotations(const JsonValue& input_schema) {
    ToolParamAnnotations result;
    if (!input_schema.IsObject()) return result;

    std::vector<std::string> seen_names;

    struct Pending {
        const JsonValue* schema;
        std::vector<std::string> path;
        bool legal;
    };
    std::vector<Pending> stack;
    stack.push_back({&input_schema, {}, true});

    while (!stack.empty()) {
        Pending current = std::move(stack.back());
        stack.pop_back();
        if (!current.schema->IsObject()) continue;
        const auto& schema_obj = current.schema->GetObject();

        if (auto annotation = schema_obj.find("x-mcp-header"); annotation != schema_obj.end()) {
            if (!current.legal) {
                result.invalid_reason =
                    "x-mcp-header is not statically reachable through properties";
                result.annotations.clear();
                return result;
            }
            if (!annotation->second.IsString()) {
                result.invalid_reason = "x-mcp-header must be a string";
                result.annotations.clear();
                return result;
            }
            const std::string& header_name = annotation->second.GetString();
            if (header_name.empty()) {
                result.invalid_reason = "x-mcp-header must not be empty";
                result.annotations.clear();
                return result;
            }
            if (!IsValidParamHeaderName(header_name)) {
                result.invalid_reason =
                    "x-mcp-header '" + header_name + "' is not a valid HTTP field name";
                result.annotations.clear();
                return result;
            }
            for (const auto& seen : seen_names) {
                if (seen.size() == header_name.size()) {
                    bool equal = true;
                    for (size_t i = 0; i < seen.size(); ++i) {
                        if (std::tolower(static_cast<unsigned char>(seen[i])) !=
                            std::tolower(static_cast<unsigned char>(header_name[i]))) {
                            equal = false;
                            break;
                        }
                    }
                    if (equal) {
                        result.invalid_reason =
                            "x-mcp-header '" + header_name + "' is not unique";
                        result.annotations.clear();
                        return result;
                    }
                }
            }

            auto type_it = schema_obj.find("type");
            const std::string type_name =
                (type_it != schema_obj.end() && type_it->second.IsString())
                    ? type_it->second.GetString()
                    : std::string();
            if (type_name != "string" && type_name != "integer" && type_name != "boolean") {
                result.invalid_reason =
                    "x-mcp-header is only allowed on string, integer or boolean properties";
                result.annotations.clear();
                return result;
            }

            seen_names.push_back(header_name);
            result.annotations.push_back({current.path, header_name});
        }

        for (const auto& [key, child] : schema_obj) {
            if (key == "properties") {
                if (!child.IsObject()) continue;
                for (const auto& [property_name, property_schema] : child.GetObject()) {
                    std::vector<std::string> child_path = current.path;
                    child_path.push_back(property_name);
                    stack.push_back({&property_schema, std::move(child_path), current.legal});
                }
            } else if (key == "$ref" || key == "items" || key == "prefixItems" ||
                       key == "contains" || key == "additionalProperties" ||
                       key == "patternProperties" || key == "propertyNames" ||
                       key == "unevaluatedProperties" || key == "oneOf" || key == "anyOf" ||
                       key == "allOf" || key == "not" || key == "if" || key == "then" ||
                       key == "else") {
                if (child.IsArray()) {
                    for (const auto& element : child.GetArray()) {
                        stack.push_back({&element, current.path, false});
                    }
                } else {
                    stack.push_back({&child, current.path, false});
                }
            }
        }
    }

    return result;
}

}} // namespace mcp::detail

#ifdef MCP_POP_GETOBJECT_MACRO_ANNOT
#pragma pop_macro("GetObject")
#undef MCP_POP_GETOBJECT_MACRO_ANNOT
#endif
