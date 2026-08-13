#pragma once
// JsonSchemaValidator.hpp — minimal JSON Schema subset validator (SEP-2106)
#include <mcp/JsonValue.hpp>

#include <string>

namespace mcp { namespace detail {

// Validate `instance` against a JSON Schema subset (draft-07 style):
// type, properties, required, items, enum, minimum/maximum,
// minLength/maxLength, minItems/maxItems. Returns false and sets error_out
// with a message that includes the failing path on validation failure.
bool ValidateJsonSchema(const JsonValue& instance, const JsonValue& schema,
                        std::string& error_out);

inline bool ValidateJsonSchema(const JsonValue& instance, const JsonValue& schema,
                               std::string& error_out) {
    if (schema.IsNull()) return true;

    // enum: value must match one of the listed constants
    if (auto* e = schema.Find("enum"); e && e->IsArray()) {
        bool found = false;
        for (const auto& opt : e->GetArray()) {
            if (opt == instance) { found = true; break; }
        }
        if (!found) { error_out = "value not in enum"; return false; }
    }

    // type: string or array of strings
    if (auto* t = schema.Find("type"); t) {
        bool ok = false;
        if (t->IsString()) {
            ok = (t->GetString() == "object" && instance.IsObject())
              || (t->GetString() == "array" && instance.IsArray())
              || (t->GetString() == "string" && instance.IsString())
              || (t->GetString() == "number" && instance.IsNumber())
              || (t->GetString() == "integer" && instance.IsInt())
              || (t->GetString() == "boolean" && instance.IsBool())
              || (t->GetString() == "null" && instance.IsNull());
        } else if (t->IsArray()) {
            for (const auto& opt : t->GetArray()) {
                if (!opt.IsString()) continue;
                bool m = (opt.GetString() == "object" && instance.IsObject())
                      || (opt.GetString() == "array" && instance.IsArray())
                      || (opt.GetString() == "string" && instance.IsString())
                      || (opt.GetString() == "number" && instance.IsNumber())
                      || (opt.GetString() == "integer" && instance.IsInt())
                      || (opt.GetString() == "boolean" && instance.IsBool())
                      || (opt.GetString() == "null" && instance.IsNull());
                if (m) { ok = true; break; }
            }
        }
        if (!ok) { error_out = "type mismatch"; return false; }
    }

    // object constraints
    if (schema.IsObject() && instance.IsObject()) {
        if (auto* req = schema.Find("required"); req && req->IsArray()) {
            for (const auto& r : req->GetArray()) {
                if (!r.IsString()) continue;
                if (!instance.Contains(r.GetString())) {
                    error_out = "missing required property '" + r.GetString() + "'";
                    return false;
                }
            }
        }
        if (auto* props = schema.Find("properties"); props && props->IsObject()) {
            for (const auto& [key, sub] : props->GetObject()) {
                auto* val = instance.Find(key);
                if (val && !ValidateJsonSchema(*val, sub, error_out)) {
                    error_out = "property '" + key + "': " + error_out;
                    return false;
                }
            }
        }
    }

    // array constraints
    if (instance.IsArray() && schema.IsObject()) {
        if (auto* items = schema.Find("items"); items) {
            for (size_t i = 0; i < instance.Size(); ++i) {
                if (!ValidateJsonSchema(instance.GetArray()[i], *items, error_out)) {
                    error_out = "item " + std::to_string(i) + ": " + error_out;
                    return false;
                }
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

    // string constraints
    if (instance.IsString() && schema.IsObject()) {
        if (auto* mn = schema.Find("minLength"); mn && mn->IsInt() &&
            static_cast<int64_t>(instance.GetString().size()) < mn->GetInt()) {
            error_out = "string too short"; return false;
        }
        if (auto* mx = schema.Find("maxLength"); mx && mx->IsInt() &&
            static_cast<int64_t>(instance.GetString().size()) > mx->GetInt()) {
            error_out = "string too long"; return false;
        }
    }

    // number constraints
    if (instance.IsNumber() && schema.IsObject()) {
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

}} // namespace mcp::detail
