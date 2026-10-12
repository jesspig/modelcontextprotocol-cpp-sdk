#pragma once
#include <mcp/JsonValue.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace mcp { namespace detail {

bool ValidateJsonSchema(const JsonValue& instance, const JsonValue& schema,
                        std::string& error_out);

inline constexpr size_t kMaxJsonSchemaDepth = 32;

inline bool JsonSchemaTypeMatches(const JsonValue& instance, std::string_view type_name) {
    return (type_name == "object" && instance.IsObject())
        || (type_name == "array" && instance.IsArray())
        || (type_name == "string" && instance.IsString())
        || (type_name == "number" && instance.IsNumber())
        || (type_name == "integer" && instance.IsInt())
        || (type_name == "boolean" && instance.IsBool())
        || (type_name == "null" && instance.IsNull());
}

inline bool JsonSchemaTypeMatchesAny(const JsonValue& instance, const JsonValue& type_value) {
    if (type_value.IsString()) return JsonSchemaTypeMatches(instance, type_value.GetString());
    if (type_value.IsArray()) {
        for (const auto& opt : type_value.GetArray()) {
            if (opt.IsString() && JsonSchemaTypeMatches(instance, opt.GetString())) return true;
        }
    }
    return false;
}

inline const JsonValue* ResolveJsonPointer(const JsonValue& root, std::string_view pointer,
                                           std::string& error_out) {
    const JsonValue* current = &root;
    size_t pos = 0;
    while (true) {
        size_t slash = pointer.find('/', pos);
        std::string_view token = (slash == std::string_view::npos)
            ? pointer.substr(pos) : pointer.substr(pos, slash - pos);
        std::string key;
        key.reserve(token.size());
        for (size_t i = 0; i < token.size(); ++i) {
            if (token[i] == '~' && i + 1 < token.size()) {
                if (token[i + 1] == '1') key.push_back('/');
                else if (token[i + 1] == '0') key.push_back('~');
                else key.push_back(token[i + 1]);
                ++i;
            } else {
                key.push_back(token[i]);
            }
        }
        if (current->IsObject()) {
            auto* next = current->Find(key);
            if (!next) {
                error_out = "unresolvable $ref";
                return nullptr;
            }
            current = next;
        } else if (current->IsArray()) {
            size_t index = 0;
            bool digits = !key.empty();
            for (char c : key) digits = digits && c >= '0' && c <= '9';
            if (digits) {
                for (char c : key) index = index * 10 + static_cast<size_t>(c - '0');
            }
            if (!digits || index >= current->Size()) {
                error_out = "unresolvable $ref";
                return nullptr;
            }
            current = &current->GetArray()[index];
        } else {
            error_out = "unresolvable $ref";
            return nullptr;
        }
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }
    return current;
}

inline const JsonValue* FindSchemaAnchor(const JsonValue& node, std::string_view anchor) {
    if (node.IsObject()) {
        if (auto* a = node.Find("$anchor"); a && a->IsString() && a->GetString() == anchor)
            return &node;
        for (const auto& [key, value] : node.GetObject()) {
            if (auto* found = FindSchemaAnchor(value, anchor)) return found;
        }
        return nullptr;
    }
    if (node.IsArray()) {
        for (const auto& item : node.GetArray()) {
            if (auto* found = FindSchemaAnchor(item, anchor)) return found;
        }
    }
    return nullptr;
}

inline const JsonValue* ResolveSchemaRef(const JsonValue& root, std::string_view ref,
                                         std::string& error_out) {
    if (ref.empty() || ref[0] != '#') {
        error_out = "unresolvable $ref '" + std::string(ref) + "'";
        return nullptr;
    }
    if (ref.size() == 1) return &root;
    if (ref[1] == '/') return ResolveJsonPointer(root, ref.substr(2), error_out);
    if (auto* found = FindSchemaAnchor(root, ref.substr(1))) return found;
    error_out = "unresolvable $ref '" + std::string(ref) + "'";
    return nullptr;
}

inline bool ValidateJsonSchemaWithRoot(const JsonValue& instance, const JsonValue& schema,
                                       const JsonValue& root, size_t depth,
                                       std::string& error_out) {
    if (schema.IsBool()) {
        if (schema.GetBool()) return true;
        error_out = "boolean schema false";
        return false;
    }
    if (schema.IsNull()) return true;
    if (!schema.IsObject()) return true;
    if (depth > kMaxJsonSchemaDepth) {
        error_out = "schema depth exceeded";
        return false;
    }
    if (auto* ref = schema.Find("$ref")) {
        if (!ref->IsString()) {
            error_out = "invalid $ref";
            return false;
        }
        const JsonValue* target = ResolveSchemaRef(root, ref->GetString(), error_out);
        if (!target) return false;
        if (!ValidateJsonSchemaWithRoot(instance, *target, root, depth + 1, error_out)) return false;
    }
    if (auto* c = schema.Find("const")) {
        if (*c != instance) {
            error_out = "value does not match const";
            return false;
        }
    }
    if (auto* e = schema.Find("enum"); e && e->IsArray()) {
        bool found = false;
        for (const auto& opt : e->GetArray()) {
            if (opt == instance) { found = true; break; }
        }
        if (!found) { error_out = "value not in enum"; return false; }
    }
    if (auto* t = schema.Find("type")) {
        if (!JsonSchemaTypeMatchesAny(instance, *t)) { error_out = "type mismatch"; return false; }
    }
    if (auto* all = schema.Find("allOf"); all && all->IsArray()) {
        for (const auto& sub : all->GetArray()) {
            if (!ValidateJsonSchemaWithRoot(instance, sub, root, depth + 1, error_out)) return false;
        }
    }
    if (auto* any = schema.Find("anyOf"); any && any->IsArray()) {
        bool matched = false;
        std::string ignored;
        for (const auto& sub : any->GetArray()) {
            if (ValidateJsonSchemaWithRoot(instance, sub, root, depth + 1, ignored)) {
                matched = true;
                break;
            }
        }
        if (!matched) { error_out = "value does not match anyOf"; return false; }
    }
    if (auto* one = schema.Find("oneOf"); one && one->IsArray()) {
        size_t matched = 0;
        std::string ignored;
        for (const auto& sub : one->GetArray()) {
            if (ValidateJsonSchemaWithRoot(instance, sub, root, depth + 1, ignored)) ++matched;
        }
        if (matched != 1) { error_out = "value does not match oneOf"; return false; }
    }
    if (auto* n = schema.Find("not")) {
        std::string ignored;
        if (ValidateJsonSchemaWithRoot(instance, *n, root, depth + 1, ignored)) {
            error_out = "value matches forbidden not schema";
            return false;
        }
    }
    if (auto* cond = schema.Find("if")) {
        std::string ignored;
        if (ValidateJsonSchemaWithRoot(instance, *cond, root, depth + 1, ignored)) {
            if (auto* then = schema.Find("then")) {
                if (!ValidateJsonSchemaWithRoot(instance, *then, root, depth + 1, error_out))
                    return false;
            }
        } else if (auto* els = schema.Find("else")) {
            if (!ValidateJsonSchemaWithRoot(instance, *els, root, depth + 1, error_out)) return false;
        }
    }
    if (instance.IsObject()) {
        if (auto* req = schema.Find("required"); req && req->IsArray()) {
            for (const auto& r : req->GetArray()) {
                if (!r.IsString()) continue;
                if (!instance.Contains(r.GetString())) {
                    error_out = "missing required property '" + r.GetString() + "'";
                    return false;
                }
            }
        }
        auto* props = schema.Find("properties");
        bool has_props = props && props->IsObject();
        if (has_props) {
            for (const auto& [key, sub] : props->GetObject()) {
                auto* val = instance.Find(key);
                if (val && !ValidateJsonSchemaWithRoot(*val, sub, root, depth + 1, error_out)) {
                    error_out = "property '" + key + "': " + error_out;
                    return false;
                }
            }
        }
        if (auto* add = schema.Find("additionalProperties")) {
            for (const auto& [key, val] : instance.GetObject()) {
                if (has_props && props->Contains(key)) continue;
                if (!ValidateJsonSchemaWithRoot(val, *add, root, depth + 1, error_out)) {
                    error_out = "property '" + key + "': " + error_out;
                    return false;
                }
            }
        }
    }
    if (instance.IsArray()) {
        auto* items = schema.Find("items");
        auto* prefix = schema.Find("prefixItems");
        bool has_prefix = prefix && prefix->IsArray();
        for (size_t i = 0; i < instance.Size(); ++i) {
            const JsonValue* sub = nullptr;
            if (has_prefix && i < prefix->Size()) sub = &prefix->GetArray()[i];
            else if (items) sub = items;
            if (sub && !ValidateJsonSchemaWithRoot(instance.GetArray()[i], *sub, root,
                                                  depth + 1, error_out)) {
                error_out = "item " + std::to_string(i) + ": " + error_out;
                return false;
            }
        }
        if (auto* mn = schema.Find("minItems"); mn && mn->IsInt() &&
            instance.Size() < static_cast<size_t>(mn->GetInt())) {
            error_out = "too few items"; return false;
        }
        if (auto* mx = schema.Find("maxItems"); mx && mx->IsInt() &&
            instance.Size() > static_cast<size_t>(mx->GetInt())) {
            error_out = "too many items"; return false;
        }
    }
    if (instance.IsString()) {
        if (auto* mn = schema.Find("minLength"); mn && mn->IsInt() &&
            static_cast<int64_t>(instance.GetString().size()) < mn->GetInt()) {
            error_out = "string too short"; return false;
        }
        if (auto* mx = schema.Find("maxLength"); mx && mx->IsInt() &&
            static_cast<int64_t>(instance.GetString().size()) > mx->GetInt()) {
            error_out = "string too long"; return false;
        }
    }
    if (instance.IsNumber()) {
        double v = instance.IsDouble() ? instance.GetDouble()
                                       : static_cast<double>(instance.GetInt());
        if (auto* mn = schema.Find("minimum"); mn && mn->IsNumber()) {
            double m = mn->IsDouble() ? mn->GetDouble() : static_cast<double>(mn->GetInt());
            if (v < m) { error_out = "below minimum"; return false; }
        }
        if (auto* mx = schema.Find("maximum"); mx && mx->IsNumber()) {
            double m = mx->IsDouble() ? mx->GetDouble() : static_cast<double>(mx->GetInt());
            if (v > m) { error_out = "above maximum"; return false; }
        }
    }
    return true;
}

inline bool ValidateJsonSchema(const JsonValue& instance, const JsonValue& schema,
                               std::string& error_out) {
    return ValidateJsonSchemaWithRoot(instance, schema, schema, 0, error_out);
}

}} // namespace mcp::detail
