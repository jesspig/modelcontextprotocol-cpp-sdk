#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <detail/JsonSerializer.hpp>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace mcp::detail {

namespace json {
JsonValue ParseDocument(std::string_view json);
}

namespace {

std::string JsonValueFormatDoubleFallback(double d) {
    char buf[64];
    int len = std::snprintf(buf, sizeof(buf), "%.*g",
        std::numeric_limits<double>::max_digits10, d);
    return std::string(buf, static_cast<size_t>(len));
}

std::string JsonValueFormatDouble(double d) {
#if defined(_MSC_VER) || (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE >= 11) || defined(__cpp_lib_to_chars)
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::general);
    if (res.ec == std::errc()) return std::string(buf, res.ptr);
#endif
    return JsonValueFormatDoubleFallback(d);
}

} // namespace

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

static void DumpValue(std::string& out, const JsonValue& jv, int indent, int depth) {
    auto indent_at = [&](int level) {
        int n = level * indent;
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
            std::string repr = JsonValueFormatDouble(d);
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
                indent_at(depth + 1);
                DumpValue(out, arr[i], indent, depth + 1);
                if (i + 1 < arr.size()) out.push_back(',');
                out.push_back('\n');
            }
            indent_at(depth);
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
                indent_at(depth + 1);
                AppendEscapedString(out, key);
                out.push_back(':');
                out.push_back(' ');
                DumpValue(out, val, indent, depth + 1);
            }
            out.push_back('\n');
            indent_at(depth);
            out.push_back('}');
        }
    }
}

} // namespace mcp::detail

namespace mcp {

namespace {

const JsonValue& JsonValueLookupRequired(const JsonValue::Object& obj,
                                          std::string_view key, const char* message) {
    auto it = obj.find(key);
    if (it == obj.end())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string(message) + std::string(key) + "'");
    return it->second;
}

JsonValue& JsonValueLookupRequired(JsonValue::Object& obj,
                                    std::string_view key, const char* message) {
    auto it = obj.find(key);
    if (it == obj.end())
        throw McpError(McpErrorCode::DeserializeFailed,
            std::string(message) + std::string(key) + "'");
    return it->second;
}

} // namespace

JsonValue JsonValue::Parse(std::string_view json) {
    return detail::json::ParseDocument(json);
}

std::string JsonValue::SanitizeUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        unsigned char b0 = static_cast<unsigned char>(text[i]);
        if (b0 < 0x80) {
            out.push_back(static_cast<char>(b0));
            ++i;
            continue;
        }
        size_t cont = 0;
        unsigned char lo = 0x80;
        unsigned char hi = 0xBF;
        bool lead_ok = true;
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            cont = 1;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            cont = 2;
            if (b0 == 0xE0) lo = 0xA0;
            else if (b0 == 0xED) hi = 0x9F;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            cont = 3;
            if (b0 == 0xF0) lo = 0x90;
            else if (b0 == 0xF4) hi = 0x8F;
        } else {
            lead_ok = false;
        }
        bool seq_ok = lead_ok && i + cont < text.size();
        if (seq_ok) {
            unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
            seq_ok = b1 >= lo && b1 <= hi;
            for (size_t k = 2; seq_ok && k <= cont; ++k) {
                unsigned char c = static_cast<unsigned char>(text[i + k]);
                seq_ok = (c & 0xC0) == 0x80;
            }
        }
        if (seq_ok) {
            out.append(text.data() + i, cont + 1);
            i += cont + 1;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

std::string JsonValue::Dump(int indent) const {
    std::string out;
    out.reserve(detail::EstimateSize(*this, indent));
    detail::DumpValue(out, *this, indent, 0);
    return out;
}

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
    return JsonValueLookupRequired(GetObject(), key, "JsonValue: key not found: '");
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
    return JsonValueLookupRequired(GetObject(), key, "JsonValue::At: key not found: '");
}

JsonValue& JsonValue::At(std::string_view key) {
    return JsonValueLookupRequired(GetObject(), key, "JsonValue::At: key not found: '");
}

void JsonValue::PushBack(JsonValue val) {
    GetArray().push_back(std::move(val));
}

JsonValue::~JsonValue() {
    if (IsArray()) {
        if (std::get<Array>(data_).empty()) {
            return;
        }
    } else if (IsObject()) {
        if (std::get<Object>(data_).empty()) {
            return;
        }
    } else {
        return;
    }
    std::vector<JsonValue> stack;
    if (IsArray()) {
        Array moved = std::move(std::get<Array>(data_));
        data_.emplace<std::nullptr_t>(nullptr);
        stack.reserve(moved.size() + 1);
        for (auto& child : moved) {
            stack.push_back(std::move(child));
        }
    } else {
        Object moved = std::move(std::get<Object>(data_));
        data_.emplace<std::nullptr_t>(nullptr);
        stack.reserve(moved.size() + 1);
        for (auto& entry : moved) {
            stack.push_back(std::move(entry.second));
        }
    }
    while (!stack.empty()) {
        JsonValue node = std::move(stack.back());
        stack.pop_back();
        if (node.IsArray()) {
            Array& arr = std::get<Array>(node.data_);
            for (auto& child : arr) {
                stack.push_back(std::move(child));
            }
        } else if (node.IsObject()) {
            Object& obj = std::get<Object>(node.data_);
            for (auto& entry : obj) {
                stack.push_back(std::move(entry.second));
            }
        }
    }
}

JsonValue::JsonValue(const JsonValue& other) : data_(nullptr) {
    if (!other.IsArray() && !other.IsObject()) {
        data_ = other.data_;
        return;
    }
    std::vector<std::pair<const JsonValue*, JsonValue*>> stack;
    if (other.IsArray()) {
        const Array& src = std::get<Array>(other.data_);
        data_.emplace<Array>();
        Array& dst = std::get<Array>(data_);
        dst.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i) {
            dst.emplace_back(nullptr);
        }
        stack.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i) {
            stack.emplace_back(&src[i], &dst[i]);
        }
    } else {
        const Object& src = std::get<Object>(other.data_);
        data_.emplace<Object>();
        Object& dst = std::get<Object>(data_);
        stack.reserve(src.size());
        for (const auto& entry : src) {
            auto pos = dst.emplace(entry.first, nullptr).first;
            stack.emplace_back(&entry.second, &pos->second);
        }
    }
    while (!stack.empty()) {
        auto work = stack.back();
        stack.pop_back();
        const JsonValue* src = work.first;
        JsonValue* dst = work.second;
        if (!src->IsArray() && !src->IsObject()) {
            dst->data_ = src->data_;
        } else if (src->IsArray()) {
            const Array& sarr = std::get<Array>(src->data_);
            dst->data_.emplace<Array>();
            Array& darr = std::get<Array>(dst->data_);
            darr.reserve(sarr.size());
            for (size_t i = 0; i < sarr.size(); ++i) {
                darr.emplace_back(nullptr);
            }
            for (size_t i = 0; i < sarr.size(); ++i) {
                stack.emplace_back(&sarr[i], &darr[i]);
            }
        } else {
            const Object& sobj = std::get<Object>(src->data_);
            dst->data_.emplace<Object>();
            Object& dobj = std::get<Object>(dst->data_);
            for (const auto& entry : sobj) {
                auto pos = dobj.emplace(entry.first, nullptr).first;
                stack.emplace_back(&entry.second, &pos->second);
            }
        }
    }
}

JsonValue& JsonValue::operator=(const JsonValue& other) {
    if (this == &other) {
        return *this;
    }
    JsonValue tmp(other);
    data_.swap(tmp.data_);
    return *this;
}

bool JsonValue::operator==(const JsonValue& other) const {
    return data_ == other.data_;
}

} // namespace mcp
