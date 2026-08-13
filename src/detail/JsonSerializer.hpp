// JsonSerializer.hpp - Internal JSON serialization helpers (optional/vector field helpers)

#pragma once

#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mcp::detail {

// ── Core JSON conversion (implemented in JsonValue.cpp) ──
// JsonValue::Parse and JsonValue::Dump are the public interface.

JsonValue ParseJsonString(std::string_view json);
std::string DumpJsonString(const JsonValue& jv, int indent = -1);

// ── Type name helper for error messages ──

inline const char* JsonValueTypeName(const JsonValue& j) noexcept {
    if (j.IsNull()) return "null";
    if (j.IsBool()) return "bool";
    if (j.IsInt()) return "int";
    if (j.IsDouble()) return "double";
    if (j.IsString()) return "string";
    if (j.IsArray()) return "array";
    return "object";
}

// ── Optional field helpers ──
// All use JsonValue which is pure C++17 (no external lib needed)

template <typename T>
inline void SerializeOptional(JsonValue& obj, const char* key,
                               const std::optional<T>& opt) {
    if (opt.has_value()) {
        obj[key] = JsonValue(*opt);
    }
}

// ── Optional deserialization helpers ──
// Generic template is intentionally not implemented: missing explicit
// specializations fail at compile time instead of silently defaulting.

template <typename T>
struct DependentFalse : std::false_type {};

template <typename T>
inline void DeserializeOptional(const JsonValue&, const char*, std::optional<T>&) {
    static_assert(DependentFalse<T>::value,
        "DeserializeOptional: explicit specialization required for this type");
}

template <>
inline void DeserializeOptional(const JsonValue& j, const char* key,
                                 std::optional<std::string>& opt) {
    auto* v = j.Find(key);
    if (!v) return;
    if (!v->IsString())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("DeserializeOptional: field '") + key +
            "' expected string, got " + JsonValueTypeName(*v));
    opt = v->GetString();
}

template <>
inline void DeserializeOptional(const JsonValue& j, const char* key,
                                 std::optional<bool>& opt) {
    auto* v = j.Find(key);
    if (!v) return;
    if (!v->IsBool())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("DeserializeOptional: field '") + key +
            "' expected bool, got " + JsonValueTypeName(*v));
    opt = v->GetBool();
}

template <>
inline void DeserializeOptional(const JsonValue& j, const char* key,
                                 std::optional<int64_t>& opt) {
    auto* v = j.Find(key);
    if (!v) return;
    if (!v->IsInt())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("DeserializeOptional: field '") + key +
            "' expected int, got " + JsonValueTypeName(*v));
    opt = v->GetInt();
}

template <>
inline void DeserializeOptional(const JsonValue& j, const char* key,
                                 std::optional<double>& opt) {
    auto* v = j.Find(key);
    if (!v) return;
    if (!v->IsNumber())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("DeserializeOptional: field '") + key +
            "' expected number, got " + JsonValueTypeName(*v));
    opt = v->IsDouble() ? v->GetDouble() : static_cast<double>(v->GetInt());
}

template <>
inline void DeserializeOptional(const JsonValue& j, const char* key,
                                 std::optional<JsonValue>& opt) {
    auto* v = j.Find(key);
    if (v) {
        opt = *v;
    }
}

// ── Vector field serialization ──

template <typename T, typename SerializeFn>
inline void SerializeVector(JsonValue& obj, const char* key,
                             const std::vector<T>& vec,
                             SerializeFn&& ser) {
    if (vec.empty()) return;
    JsonValue::Array arr;
    arr.reserve(vec.size());
    for (const auto& item : vec) {
        arr.push_back(ser(item));
    }
    obj[key] = JsonValue(std::move(arr));
}

// ── Vector deserialization helpers ──

template <typename T, typename DeserializeFn>
inline std::vector<T> DeserializeVector(const JsonValue& j, const char* key,
                                         DeserializeFn&& deser) {
    std::vector<T> result;
    auto* v = j.Find(key);
    if (!v) return result;
    if (!v->IsArray())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("DeserializeVector: field '") + key +
            "' expected array, got " + JsonValueTypeName(*v));
    result.reserve(v->Size());
    for (const auto& elem : v->GetArray()) {
        result.push_back(deser(elem));
    }
    return result;
}

template <typename T, typename DeserializeFn>
inline std::vector<T> DeserializeVector(const JsonValue& array,
                                         DeserializeFn&& deser) {
    std::vector<T> result;
    if (array.IsArray()) {
        result.reserve(array.Size());
        for (const auto& elem : array.GetArray()) {
            result.push_back(deser(elem));
        }
    }
    return result;
}

} // namespace mcp::detail
