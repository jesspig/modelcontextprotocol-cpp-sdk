// JsonValue.cpp — JsonValue parsing and serialization implementation

#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <detail/JsonSerializer.hpp>

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace mcp::detail {

namespace json {
JsonValue ParseDocument(std::string_view json);
}

JsonValue ParseJsonString(std::string_view json) {
    return json::ParseDocument(json);
}

// ── Hand-written JSON serializer ──

static void DumpString(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
        case '"':  os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        case '\b': os << "\\b";  break;
        case '\f': os << "\\f";  break;
        case '\n': os << "\\n";  break;
        case '\r': os << "\\r";  break;
        case '\t': os << "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                   << static_cast<int>(static_cast<unsigned char>(c)) << std::dec;
            } else {
                os << c;
            }
        }
    }
    os << '"';
}

// Recursively serialize a JsonValue to JSON text with optional indentation.
static void DumpValue(std::ostream& os, const JsonValue& jv, int indent, int depth) {
    auto indent_line = [&]() {
        for (int i = 0; i < depth * indent; ++i) os << ' ';
    };

    if (jv.IsNull()) {
        os << "null";
    } else if (jv.IsBool()) {
        os << (jv.GetBool() ? "true" : "false");
    } else if (jv.IsInt()) {
        os << jv.GetInt();
    } else if (jv.IsDouble()) {
        double d = jv.GetDouble();
        if (std::isfinite(d)) {
            std::ostringstream tmp;
            tmp << std::setprecision(std::numeric_limits<double>::max_digits10) << d;
            std::string repr = tmp.str();
            if (repr.find_first_of(".eE") == std::string::npos) repr += ".0";
            os << repr;
        } else {
            os << "null";
        }
    } else if (jv.IsString()) {
        DumpString(os, jv.GetString());
    } else if (jv.IsArray()) {
        const auto& arr = jv.GetArray();
        if (arr.empty()) {
            os << "[]";
        } else if (indent < 0) {
            os << '[';
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0) os << ',';
                DumpValue(os, arr[i], indent, depth);
            }
            os << ']';
        } else {
            os << "[\n";
            for (size_t i = 0; i < arr.size(); ++i) {
                indent_line();
                DumpValue(os, arr[i], indent, depth + 1);
                if (i + 1 < arr.size()) os << ',';
                os << '\n';
            }
            indent_line();
            os << ']';
        }
    } else if (jv.IsObject()) {
        const auto& obj = jv.GetObject();
        if (obj.empty()) {
            os << "{}";
        } else if (indent < 0) {
            os << '{';
            bool first = true;
            for (const auto& [key, val] : obj) {
                if (!first) os << ',';
                first = false;
                DumpString(os, key);
                os << ':';
                DumpValue(os, val, indent, depth);
            }
            os << '}';
        } else {
            os << "{\n";
            bool first = true;
            for (const auto& [key, val] : obj) {
                if (!first) os << ",\n";
                first = false;
                indent_line();
                DumpString(os, key);
                os << ':';
                if (indent >= 0) os << ' ';
                DumpValue(os, val, indent, depth + 1);
            }
            os << '\n';
            indent_line();
            os << '}';
        }
    }
}

std::string DumpJsonString(const JsonValue& jv, int indent) {
    std::ostringstream os;
    DumpValue(os, jv, indent, 0);
    return os.str();
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
