// JsonValue.cpp — JsonValue parsing and serialization implementation

#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <detail/JsonSerializer.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace mcp::detail {

namespace json {
JsonValue ParseDocument(std::string_view json);
}

JsonValue ParseJsonString(std::string_view json) {
    return json::ParseDocument(json);
}

// ── Hand-written JSON serializer ──

static void AppendEscapedString(std::string& out, const std::string& s) {
    out.push_back('"');
    static constexpr char kHexDigits[] = "0123456789abcdef";
    size_t run_start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        const char* esc = nullptr;
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b";  break;
        case '\f': esc = "\\f";  break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c < 0x20) {
                out.append(s, run_start, i - run_start);
                out += "\\u00";
                out.push_back(static_cast<char>('0' + (c >> 4)));
                out.push_back(kHexDigits[c & 0xF]);
                run_start = i + 1;
            }
            continue;
        }
        out.append(s, run_start, i - run_start);
        out += esc;
        run_start = i + 1;
    }
    out.append(s, run_start, s.size() - run_start);
    out.push_back('"');
}

static size_t EstimateSize(const JsonValue& jv, int indent) {
    if (jv.IsString()) return jv.GetString().size() + 8;
    if (jv.IsArray()) {
        const auto& arr = jv.GetArray();
        if (arr.empty()) return 2;
        size_t n = arr.size() * 4 + 2;
        if (indent >= 0) n += arr.size() * static_cast<size_t>(indent);
        for (const auto& el : arr) n += EstimateSize(el, indent);
        return n;
    }
    if (jv.IsObject()) {
        const auto& obj = jv.GetObject();
        if (obj.empty()) return 2;
        size_t n = obj.size() * 8 + 2;
        if (indent >= 0) n += obj.size() * static_cast<size_t>(indent);
        for (const auto& [key, val] : obj) n += key.size() + 3 + EstimateSize(val, indent);
        return n;
    }
    if (jv.IsInt()) return 24;
    if (jv.IsDouble()) return 32;
    return 8;
}

// Recursively serialize a JsonValue to JSON text with optional indentation.
static void DumpValue(std::string& out, const JsonValue& jv, int indent, int depth) {
    auto indent_line = [&]() {
        int n = depth * indent;
        if (n > 0) out.append(static_cast<size_t>(n), ' ');
    };

    if (jv.IsNull()) {
        out += "null";
    } else if (jv.IsBool()) {
        out += jv.GetBool() ? "true" : "false";
    } else if (jv.IsInt()) {
        out += std::to_string(jv.GetInt());
    } else if (jv.IsDouble()) {
        double d = jv.GetDouble();
        if (std::isfinite(d)) {
            char buf[64];
            int len = std::snprintf(buf, sizeof(buf), "%.*g",
                std::numeric_limits<double>::max_digits10, d);
            std::string_view repr(buf, static_cast<size_t>(len));
            out += repr;
            if (repr.find_first_of(".eE") == std::string_view::npos) out += ".0";
        } else {
            out += "null";
        }
    } else if (jv.IsString()) {
        AppendEscapedString(out, jv.GetString());
    } else if (jv.IsArray()) {
        const auto& arr = jv.GetArray();
        if (arr.empty()) {
            out += "[]";
        } else if (indent < 0) {
            out.push_back('[');
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0) out.push_back(',');
                DumpValue(out, arr[i], indent, depth);
            }
            out.push_back(']');
        } else {
            out += "[\n";
            for (size_t i = 0; i < arr.size(); ++i) {
                indent_line();
                DumpValue(out, arr[i], indent, depth + 1);
                if (i + 1 < arr.size()) out.push_back(',');
                out.push_back('\n');
            }
            indent_line();
            out.push_back(']');
        }
    } else {
        const auto& obj = jv.GetObject();
        if (obj.empty()) {
            out += "{}";
        } else if (indent < 0) {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, val] : obj) {
                if (!first) out.push_back(',');
                first = false;
                AppendEscapedString(out, key);
                out.push_back(':');
                DumpValue(out, val, indent, depth);
            }
            out.push_back('}');
        } else {
            out += "{\n";
            bool first = true;
            for (const auto& [key, val] : obj) {
                if (!first) out += ",\n";
                first = false;
                indent_line();
                AppendEscapedString(out, key);
                out.push_back(':');
                out.push_back(' ');
                DumpValue(out, val, indent, depth + 1);
            }
            out.push_back('\n');
            indent_line();
            out.push_back('}');
        }
    }
}

std::string DumpJsonString(const JsonValue& jv, int indent) {
    std::string out;
    out.reserve(EstimateSize(jv, indent));
    DumpValue(out, jv, indent, 0);
    return out;
}

} // namespace mcp::detail

// ── JsonValue public methods ──

namespace mcp {

JsonValue JsonValue::Parse(std::string_view json) {
    return detail::ParseJsonString(json);
}

std::string JsonValue::Dump(int indent) const {
    return detail::DumpJsonString(*this, indent);
}

// ── Accessors ──

bool JsonValue::GetBool() const {
    if (!IsBool())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue::GetBool: expected bool, got ") + detail::JsonValueTypeName(*this));
    return std::get<bool>(data_);
}

int64_t JsonValue::GetInt() const {
    if (!IsInt())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue::GetInt: expected int, got ") + detail::JsonValueTypeName(*this));
    return std::get<int64_t>(data_);
}

double JsonValue::GetDouble() const {
    if (!IsDouble())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue::GetDouble: expected double, got ") + detail::JsonValueTypeName(*this));
    return std::get<double>(data_);
}

const std::string& JsonValue::GetString() const {
    if (!IsString())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue::GetString: expected string, got ") + detail::JsonValueTypeName(*this));
    return std::get<std::string>(data_);
}

const JsonValue::Array& JsonValue::GetArray() const {
    if (!IsArray())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue::GetArray: expected array, got ") + detail::JsonValueTypeName(*this));
    return std::get<Array>(data_);
}

JsonValue::Array& JsonValue::GetArray() {
    if (!IsArray())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue::GetArray: expected array, got ") + detail::JsonValueTypeName(*this));
    return std::get<Array>(data_);
}

const JsonValue::Object& JsonValue::GetObject() const {
    if (!IsObject())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue::GetObject: expected object, got ") + detail::JsonValueTypeName(*this));
    return std::get<Object>(data_);
}

JsonValue::Object& JsonValue::GetObject() {
    if (!IsObject())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue::GetObject: expected object, got ") + detail::JsonValueTypeName(*this));
    return std::get<Object>(data_);
}

size_t JsonValue::Size() const {
    if (IsArray())  return GetArray().size();
    if (IsObject()) return GetObject().size();
    if (IsString()) return GetString().size();
    return 0;
}

bool JsonValue::Empty() const {
    if (IsArray())  return GetArray().empty();
    if (IsObject()) return GetObject().empty();
    return IsNull();
}

bool JsonValue::Contains(std::string_view key) const {
    return IsObject() && GetObject().find(key) != GetObject().end();
}

JsonValue& JsonValue::operator[](size_t i) {
    return GetArray()[i];
}

const JsonValue& JsonValue::operator[](size_t i) const {
    return GetArray()[i];
}

JsonValue& JsonValue::operator[](std::string_view key) {
    auto& obj = GetObject();
    auto [pos, _] = obj.emplace(std::string(key), nullptr);
    return pos->second;
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
    auto& obj = GetObject();
    auto it = obj.find(key);
    if (it == obj.end())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue: key not found: '") + std::string(key) + "'");
    return it->second;
}

const JsonValue* JsonValue::Find(std::string_view key) const {
    if (!IsObject()) return nullptr;
    auto it = GetObject().find(key);
    return it != GetObject().end() ? &it->second : nullptr;
}

JsonValue* JsonValue::Find(std::string_view key) {
    if (!IsObject()) return nullptr;
    auto it = GetObject().find(key);
    return it != GetObject().end() ? &it->second : nullptr;
}

const JsonValue& JsonValue::At(std::string_view key) const {
    auto& obj = GetObject();
    auto it = obj.find(key);
    if (it == obj.end())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue::At: key not found: '") + std::string(key) + "'");
    return it->second;
}

JsonValue& JsonValue::At(std::string_view key) {
    auto& obj = GetObject();
    auto it = obj.find(key);
    if (it == obj.end())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string("JsonValue::At: key not found: '") + std::string(key) + "'");
    return it->second;
}

void JsonValue::PushBack(JsonValue val) {
    GetArray().push_back(std::move(val));
}

bool JsonValue::operator==(const JsonValue& other) const {
    return data_ == other.data_;
}

} // namespace mcp
