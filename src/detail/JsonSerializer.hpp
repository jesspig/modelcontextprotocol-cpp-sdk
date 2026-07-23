#pragma once

#include <mcp/JsonValue.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::detail {

// ── Core JSON conversion (implemented in JsonValue.cpp) ──
// Only used for converting JsonValue ↔ simdjson internally.
// JsonValue::Parse and JsonValue::Dump are the public interface.

JsonValue ParseJsonString(std::string_view json);
std::string DumpJsonString(const JsonValue& jv, int indent = -1);

// ── Optional field helpers ──
// All use JsonValue which is pure C++17 (no external lib needed)

template <typename T>
inline void SerializeOptional(JsonValue& obj, const char* key,
                               const std::optional<T>& opt) {
    if (opt.has_value()) {
        obj[key] = JsonValue(*opt);
    }
}

// string specialization
template <>
inline void SerializeOptional(JsonValue& obj, const char* key,
                               const std::optional<std::string>& opt) {
    if (opt.has_value()) {
        obj[key] = JsonValue(*opt);
    }
}

// bool specialization
template <>
inline void SerializeOptional(JsonValue& obj, const char* key,
                               const std::optional<bool>& opt) {
    if (opt.has_value()) {
        obj[key] = JsonValue(opt.value());
    }
}

// int64 specialization
template <>
inline void SerializeOptional(JsonValue& obj, const char* key,
                               const std::optional<int64_t>& opt) {
    if (opt.has_value()) {
        obj[key] = JsonValue(opt.value());
    }
}

// double specialization
template <>
inline void SerializeOptional(JsonValue& obj, const char* key,
                               const std::optional<double>& opt) {
    if (opt.has_value()) {
        obj[key] = JsonValue(opt.value());
    }
}

// JsonValue specialization (passthrough)
template <>
inline void SerializeOptional(JsonValue& obj, const char* key,
                               const std::optional<JsonValue>& opt) {
    if (opt.has_value()) {
        obj[key] = *opt;
    }
}

// ── Optional deserialization helpers ──

template <typename T>
inline void DeserializeOptional(const JsonValue& j, const char* key,
                                 std::optional<T>& opt) {
    auto* v = j.Find(key);
    if (v) {
        opt = T{};
    }
}

template <>
inline void DeserializeOptional(const JsonValue& j, const char* key,
                                 std::optional<std::string>& opt) {
    auto* v = j.Find(key);
    if (v && v->IsString()) {
        opt = v->GetString();
    }
}

template <>
inline void DeserializeOptional(const JsonValue& j, const char* key,
                                 std::optional<bool>& opt) {
    auto* v = j.Find(key);
    if (v && v->IsBool()) {
        opt = v->GetBool();
    }
}

template <>
inline void DeserializeOptional(const JsonValue& j, const char* key,
                                 std::optional<int64_t>& opt) {
    auto* v = j.Find(key);
    if (v && v->IsInt()) {
        opt = v->GetInt();
    }
}

template <>
inline void DeserializeOptional(const JsonValue& j, const char* key,
                                 std::optional<double>& opt) {
    auto* v = j.Find(key);
    if (v && v->IsDouble()) {
        opt = v->GetDouble();
    }
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
    if (v && v->IsArray()) {
        result.reserve(v->Size());
        for (const auto& elem : v->GetArray()) {
            result.push_back(deser(elem));
        }
    }
    return result;
}

} // namespace mcp::detail
