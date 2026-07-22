#include <mcp/JsonValue.hpp>
#include <detail/JsonSerializer.hpp>

#include <simdjson.h>

#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace mcp::detail {

using namespace simdjson;

JsonValue FromSimdJsonValue(ondemand::value val) {
    // Try each type in order
    {
        int64_t i;
        if (val.get_int64().get(i) == SUCCESS)
            return JsonValue(i);
    }
    {
        uint64_t u;
        if (val.get_uint64().get(u) == SUCCESS)
            return JsonValue(static_cast<int64_t>(u));
    }
    {
        double d;
        if (val.get_double().get(d) == SUCCESS)
            return JsonValue(d);
    }
    {
        std::string_view s;
        if (val.get_string().get(s) == SUCCESS)
            return JsonValue(std::string(s));
    }
    {
        bool b;
        if (val.get_bool().get(b) == SUCCESS)
            return JsonValue(b);
    }
    if (val.is_null()) {
        return JsonValue(nullptr);
    }
    // Array
    {
        ondemand::array arr;
        if (val.get_array().get(arr) == SUCCESS) {
            JsonValue::Array result;
            for (auto elem_result : arr) {
                ondemand::value elem;
                if (elem_result.get(elem) == SUCCESS)
                    result.push_back(FromSimdJsonValue(elem));
            }
            return JsonValue(std::move(result));
        }
    }
    // Object
    {
        ondemand::object obj;
        if (val.get_object().get(obj) == SUCCESS) {
            JsonValue::Object result;
            for (auto field_result : obj) {
                ondemand::field field;
                if (std::move(field_result).get(field) == SUCCESS) {
                    std::string_view key;
                    if (field.unescaped_key().get(key) == SUCCESS) {
                        ondemand::value fval = field.value();
                        result.emplace(std::string(key),
                                       FromSimdJsonValue(fval));
                    }
                }
            }
            return JsonValue(std::move(result));
        }
    }
    return JsonValue(nullptr);
}

JsonValue ParseJsonString(std::string_view json) {
    ondemand::parser parser;
    auto padded = padded_string(json.data(), json.size());
    ondemand::document doc;
    auto error = parser.iterate(padded).get(doc);
    if (error) {
        throw std::runtime_error("JSON parse error");
    }
    ondemand::value val = doc.get_value();
    return FromSimdJsonValue(val);
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
            os << d;
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
    return std::get<bool>(data_);
}

int64_t JsonValue::GetInt() const {
    return std::get<int64_t>(data_);
}

double JsonValue::GetDouble() const {
    return std::get<double>(data_);
}

const std::string& JsonValue::GetString() const {
    return std::get<std::string>(data_);
}

const JsonValue::Array& JsonValue::GetArray() const {
    return std::get<Array>(data_);
}

JsonValue::Array& JsonValue::GetArray() {
    return std::get<Array>(data_);
}

const JsonValue::Object& JsonValue::GetObject() const {
    return std::get<Object>(data_);
}

JsonValue::Object& JsonValue::GetObject() {
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
    auto it = obj.find(key);
    if (it != obj.end()) return it->second;
    auto [pos, _] = obj.emplace(std::string(key), nullptr);
    return pos->second;
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
    return GetObject().at(std::string(key));
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
    return GetObject().at(std::string(key));
}

JsonValue& JsonValue::At(std::string_view key) {
    return GetObject().at(std::string(key));
}

void JsonValue::PushBack(JsonValue val) {
    GetArray().push_back(std::move(val));
}

bool JsonValue::operator==(const JsonValue& other) const {
    return data_ == other.data_;
}

} // namespace mcp
