#pragma once
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>

#include <cmath>
#include <cstddef>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace mcp { namespace detail {

bool ValidateJsonSchema(const JsonValue& instance, const JsonValue& schema,
                        std::string& error_out);

inline constexpr size_t kMaxJsonSchemaDepth = 32;

inline bool JsonSchemaTypeMatches(const JsonValue& instance, std::string_view type_name) {
    if (type_name == "integer") {
        if (instance.IsInt()) return true;
        if (instance.IsDouble()) {
            double v = instance.GetDouble();
            return std::floor(v) == v;
        }
        return false;
    }
    return (type_name == "object" && instance.IsObject())
        || (type_name == "array" && instance.IsArray())
        || (type_name == "string" && instance.IsString())
        || (type_name == "number" && instance.IsNumber())
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

inline bool SchemaNumberValue(const JsonValue& v, double& out) {
    if (v.IsInt()) {
        out = static_cast<double>(v.GetInt());
        return true;
    }
    if (v.IsDouble()) {
        out = v.GetDouble();
        return true;
    }
    return false;
}

inline bool SchemaCountValue(const JsonValue& v, size_t& out) {
    if (v.IsInt() && v.GetInt() >= 0) {
        out = static_cast<size_t>(v.GetInt());
        return true;
    }
    if (v.IsDouble() && v.GetDouble() >= 0.0 && std::floor(v.GetDouble()) == v.GetDouble()) {
        out = static_cast<size_t>(v.GetDouble());
        return true;
    }
    return false;
}

inline size_t Utf8CodePointLength(std::string_view text) {
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t width = 1;
        if ((c & 0x80u) == 0u) width = 1;
        else if ((c & 0xE0u) == 0xC0u) width = 2;
        else if ((c & 0xF0u) == 0xE0u) width = 3;
        else if ((c & 0xF8u) == 0xF0u) width = 4;
        i += width;
        ++count;
    }
    return count;
}

inline bool RegexSearchValid(const std::string& pattern, const std::string& subject,
                             bool& matched) {
    try {
        std::regex re(pattern, std::regex::ECMAScript);
        matched = std::regex_search(subject, re);
        return true;
    } catch (const std::regex_error&) {
        return false;
    }
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
                                       std::vector<const JsonValue*>& ref_chain,
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
    const char* kRefKeys[] = {"$ref", "$dynamicRef", "$recursiveRef"};
    for (const char* ref_key : kRefKeys) {
        auto* ref = schema.Find(ref_key);
        if (!ref) continue;
        if (!ref->IsString()) {
            error_out = std::string("invalid ") + ref_key;
            return false;
        }
        const std::string& ref_value = ref->GetString();
        if (!ref_value.empty() && ref_value[0] != '#') {
            error_out = std::string("remote ") + ref_key + " not dereferenced '" + ref_value + "'";
            return false;
        }
        const JsonValue* target = ResolveSchemaRef(root, ref_value, error_out);
        if (!target) return false;
        for (const JsonValue* visited : ref_chain) {
            if (visited == target) {
                throw McpError(McpErrorCode::InvalidParams,
                    std::string("circular ") + ref_key + " '" + ref_value + "'");
            }
        }
        ref_chain.push_back(target);
        bool ref_ok = ValidateJsonSchemaWithRoot(instance, *target, root, depth + 1,
                                                 ref_chain, error_out);
        ref_chain.pop_back();
        if (!ref_ok) return false;
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
            if (!ValidateJsonSchemaWithRoot(instance, sub, root, depth + 1, ref_chain, error_out)) return false;
        }
    }
    if (auto* any = schema.Find("anyOf"); any && any->IsArray()) {
        bool matched = false;
        std::string ignored;
        for (const auto& sub : any->GetArray()) {
            if (ValidateJsonSchemaWithRoot(instance, sub, root, depth + 1, ref_chain, ignored)) {
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
            if (ValidateJsonSchemaWithRoot(instance, sub, root, depth + 1, ref_chain, ignored)) ++matched;
        }
        if (matched != 1) { error_out = "value does not match oneOf"; return false; }
    }
    if (auto* n = schema.Find("not")) {
        std::string ignored;
        if (ValidateJsonSchemaWithRoot(instance, *n, root, depth + 1, ref_chain, ignored)) {
            error_out = "value matches forbidden not schema";
            return false;
        }
    }
    if (auto* cond = schema.Find("if")) {
        std::string ignored;
        if (ValidateJsonSchemaWithRoot(instance, *cond, root, depth + 1, ref_chain, ignored)) {
            if (auto* then = schema.Find("then")) {
                if (!ValidateJsonSchemaWithRoot(instance, *then, root, depth + 1, ref_chain, error_out))
                    return false;
            }
        } else if (auto* els = schema.Find("else")) {
            if (!ValidateJsonSchemaWithRoot(instance, *els, root, depth + 1, ref_chain, error_out)) return false;
        }
    }
    if (instance.IsObject()) {
        const auto& obj = instance.GetObject();
        if (auto* req = schema.Find("required"); req && req->IsArray()) {
            for (const auto& r : req->GetArray()) {
                if (!r.IsString()) continue;
                if (!instance.Contains(r.GetString())) {
                    error_out = "missing required property '" + r.GetString() + "'";
                    return false;
                }
            }
        }
        if (auto* mn = schema.Find("minProperties")) {
            size_t bound = 0;
            if (SchemaCountValue(*mn, bound) && obj.size() < bound) {
                error_out = "too few properties";
                return false;
            }
        }
        if (auto* mx = schema.Find("maxProperties")) {
            size_t bound = 0;
            if (SchemaCountValue(*mx, bound) && obj.size() > bound) {
                error_out = "too many properties";
                return false;
            }
        }
        auto* props = schema.Find("properties");
        bool has_props = props && props->IsObject();
        if (has_props) {
            for (const auto& [key, sub] : props->GetObject()) {
                auto* val = instance.Find(key);
                if (val && !ValidateJsonSchemaWithRoot(*val, sub, root, depth + 1, ref_chain, error_out)) {
                    error_out = "property '" + key + "': " + error_out;
                    return false;
                }
            }
        }
        auto* pattern_props = schema.Find("patternProperties");
        bool has_pattern = pattern_props && pattern_props->IsObject();
        if (has_pattern) {
            for (const auto& [pattern, sub] : pattern_props->GetObject()) {
                for (const auto& [key, val] : obj) {
                    bool matched = false;
                    if (!RegexSearchValid(pattern, key, matched) || !matched) continue;
                    if (!ValidateJsonSchemaWithRoot(val, sub, root, depth + 1, ref_chain, error_out)) {
                        error_out = "property '" + key + "': " + error_out;
                        return false;
                    }
                }
            }
        }
        auto PatternCovered = [&](const std::string& key) {
            if (!has_pattern) return false;
            for (const auto& [pattern, sub] : pattern_props->GetObject()) {
                (void)sub;
                bool matched = false;
                if (RegexSearchValid(pattern, key, matched) && matched) return true;
            }
            return false;
        };
        if (auto* add = schema.Find("additionalProperties")) {
            for (const auto& [key, val] : obj) {
                if (has_props && props->Contains(key)) continue;
                if (PatternCovered(key)) continue;
                if (!ValidateJsonSchemaWithRoot(val, *add, root, depth + 1, ref_chain, error_out)) {
                    error_out = "property '" + key + "': " + error_out;
                    return false;
                }
            }
        }
        if (auto* uneval = schema.Find("unevaluatedProperties")) {
            for (const auto& [key, val] : obj) {
                if (has_props && props->Contains(key)) continue;
                if (PatternCovered(key)) continue;
                if (!ValidateJsonSchemaWithRoot(val, *uneval, root, depth + 1, ref_chain, error_out)) {
                    error_out = "property '" + key + "': " + error_out;
                    return false;
                }
            }
        }
        if (auto* names = schema.Find("propertyNames")) {
            for (const auto& [key, value] : obj) {
                (void)value;
                JsonValue key_value(key);
                if (!ValidateJsonSchemaWithRoot(key_value, *names, root, depth + 1, ref_chain, error_out)) {
                    error_out = "property name '" + key + "': " + error_out;
                    return false;
                }
            }
        }
        if (auto* dep = schema.Find("dependentRequired"); dep && dep->IsObject()) {
            for (const auto& [prop, reqs] : dep->GetObject()) {
                if (!instance.Contains(prop) || !reqs.IsArray()) continue;
                for (const auto& r : reqs.GetArray()) {
                    if (!r.IsString()) continue;
                    if (!instance.Contains(r.GetString())) {
                        error_out = "missing dependent property '" + r.GetString() + "'";
                        return false;
                    }
                }
            }
        }
        if (auto* ds = schema.Find("dependentSchemas"); ds && ds->IsObject()) {
            for (const auto& [prop, sub] : ds->GetObject()) {
                if (!instance.Contains(prop)) continue;
                if (!ValidateJsonSchemaWithRoot(instance, sub, root, depth + 1, ref_chain, error_out)) return false;
            }
        }
        if (auto* deps = schema.Find("dependencies"); deps && deps->IsObject()) {
            for (const auto& [prop, sub] : deps->GetObject()) {
                if (!instance.Contains(prop)) continue;
                if (sub.IsArray()) {
                    for (const auto& r : sub.GetArray()) {
                        if (!r.IsString()) continue;
                        if (!instance.Contains(r.GetString())) {
                            error_out = "missing dependent property '" + r.GetString() + "'";
                            return false;
                        }
                    }
                } else {
                    if (!ValidateJsonSchemaWithRoot(instance, sub, root, depth + 1, ref_chain, error_out)) return false;
                }
            }
        }
    }
    if (instance.IsArray()) {
        const auto& arr = instance.GetArray();
        auto* items = schema.Find("items");
        auto* prefix = schema.Find("prefixItems");
        auto* additional = schema.Find("additionalItems");
        bool has_prefix = prefix && prefix->IsArray();
        bool items_is_array = items && items->IsArray();
        for (size_t i = 0; i < arr.size(); ++i) {
            const JsonValue* sub = nullptr;
            if (has_prefix && i < prefix->Size()) {
                sub = &prefix->GetArray()[i];
            } else if (items_is_array) {
                if (i < items->Size()) sub = &items->GetArray()[i];
                else if (additional) sub = additional;
            } else if (items) {
                sub = items;
            }
            if (sub && !ValidateJsonSchemaWithRoot(arr[i], *sub, root,
                                                   depth + 1, ref_chain, error_out)) {
                error_out = "item " + std::to_string(i) + ": " + error_out;
                return false;
            }
        }
        if (auto* uneval = schema.Find("unevaluatedItems")) {
            for (size_t i = 0; i < arr.size(); ++i) {
                bool covered = false;
                if (has_prefix && i < prefix->Size()) covered = true;
                if (items && !items_is_array) covered = true;
                if (items_is_array && i < items->Size()) covered = true;
                else if (items_is_array && additional) covered = true;
                if (covered) continue;
                if (!ValidateJsonSchemaWithRoot(arr[i], *uneval, root, depth + 1, ref_chain, error_out)) {
                    error_out = "item " + std::to_string(i) + ": " + error_out;
                    return false;
                }
            }
        }
        if (auto* contains = schema.Find("contains")) {
            size_t matched = 0;
            std::string ignored;
            for (const auto& item : arr) {
                if (ValidateJsonSchemaWithRoot(item, *contains, root, depth + 1, ref_chain, ignored)) ++matched;
            }
            size_t min_c = 1;
            size_t max_c = arr.size() + 1;
            if (auto* mn = schema.Find("minContains")) {
                size_t bound = 0;
                if (SchemaCountValue(*mn, bound)) min_c = bound;
            }
            if (auto* mx = schema.Find("maxContains")) {
                size_t bound = 0;
                if (SchemaCountValue(*mx, bound)) max_c = bound;
            }
            if (matched < min_c) { error_out = "array contains too few matches"; return false; }
            if (matched > max_c) { error_out = "array contains too many matches"; return false; }
        }
        if (auto* u = schema.Find("uniqueItems"); u && u->IsBool() && u->GetBool()) {
            for (size_t i = 0; i < arr.size(); ++i) {
                for (size_t j = i + 1; j < arr.size(); ++j) {
                    if (arr[i] == arr[j]) { error_out = "array items not unique"; return false; }
                }
            }
        }
        if (auto* mn = schema.Find("minItems")) {
            size_t bound = 0;
            if (SchemaCountValue(*mn, bound) && arr.size() < bound) {
                error_out = "too few items";
                return false;
            }
        }
        if (auto* mx = schema.Find("maxItems")) {
            size_t bound = 0;
            if (SchemaCountValue(*mx, bound) && arr.size() > bound) {
                error_out = "too many items";
                return false;
            }
        }
    }
    if (instance.IsString()) {
        const std::string& text = instance.GetString();
        size_t length = Utf8CodePointLength(text);
        if (auto* mn = schema.Find("minLength")) {
            size_t bound = 0;
            if (SchemaCountValue(*mn, bound) && length < bound) {
                error_out = "string too short";
                return false;
            }
        }
        if (auto* mx = schema.Find("maxLength")) {
            size_t bound = 0;
            if (SchemaCountValue(*mx, bound) && length > bound) {
                error_out = "string too long";
                return false;
            }
        }
        if (auto* pat = schema.Find("pattern"); pat && pat->IsString()) {
            bool matched = false;
            if (RegexSearchValid(pat->GetString(), text, matched) && !matched) {
                error_out = "string does not match pattern";
                return false;
            }
        }
    }
    if (instance.IsNumber()) {
        double v = instance.IsDouble() ? instance.GetDouble()
                                       : static_cast<double>(instance.GetInt());
        if (auto* mn = schema.Find("minimum")) {
            double m = 0.0;
            if (SchemaNumberValue(*mn, m) && v < m) { error_out = "below minimum"; return false; }
        }
        if (auto* mx = schema.Find("maximum")) {
            double m = 0.0;
            if (SchemaNumberValue(*mx, m) && v > m) { error_out = "above maximum"; return false; }
        }
        if (auto* emn = schema.Find("exclusiveMinimum")) {
            double m = 0.0;
            if (SchemaNumberValue(*emn, m)) {
                if (v <= m) { error_out = "below exclusiveMinimum"; return false; }
            } else if (emn->IsBool() && emn->GetBool()) {
                if (auto* mn = schema.Find("minimum")) {
                    double m2 = 0.0;
                    if (SchemaNumberValue(*mn, m2) && v <= m2) {
                        error_out = "below exclusiveMinimum";
                        return false;
                    }
                }
            }
        }
        if (auto* emx = schema.Find("exclusiveMaximum")) {
            double m = 0.0;
            if (SchemaNumberValue(*emx, m)) {
                if (v >= m) { error_out = "above exclusiveMaximum"; return false; }
            } else if (emx->IsBool() && emx->GetBool()) {
                if (auto* mx = schema.Find("maximum")) {
                    double m2 = 0.0;
                    if (SchemaNumberValue(*mx, m2) && v >= m2) {
                        error_out = "above exclusiveMaximum";
                        return false;
                    }
                }
            }
        }
        if (auto* mult = schema.Find("multipleOf")) {
            if (instance.IsInt() && mult->IsInt()) {
                int64_t divisor = mult->GetInt();
                if (divisor > 0 && instance.GetInt() % divisor != 0) {
                    error_out = "not multipleOf";
                    return false;
                }
            } else {
                double m = 0.0;
                if (SchemaNumberValue(*mult, m) && m > 0.0) {
                    double q = v / m;
                    double nearest = std::round(q);
                    double scale = std::fabs(q) > 1.0 ? std::fabs(q) : 1.0;
                    if (std::fabs(q - nearest) > 1e-9 * scale) {
                        error_out = "not multipleOf";
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

inline bool ValidateJsonSchema(const JsonValue& instance, const JsonValue& schema,
                               std::string& error_out) {
    std::vector<const JsonValue*> ref_chain{&schema};
    return ValidateJsonSchemaWithRoot(instance, schema, schema, 0, ref_chain, error_out);
}

}} // namespace mcp::detail
