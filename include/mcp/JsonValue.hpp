#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#ifdef GetObject
#pragma push_macro("GetObject")
#undef GetObject
#define MCP_POP_GETOBJECT_MACRO 1
#endif

namespace mcp {

class JsonValue {
public:
    using Object = std::map<std::string, JsonValue, std::less<>>;
    using Array  = std::vector<JsonValue>;

private:
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> data_;

public:
    JsonValue() : data_(nullptr) {}
    JsonValue(std::nullptr_t) : data_(nullptr) {}
    JsonValue(bool v) : data_(v) {}
    JsonValue(int v) : data_(static_cast<int64_t>(v)) {}
    JsonValue(int64_t v) : data_(v) {}
    JsonValue(double v) : data_(v) {}
    JsonValue(const std::string& v) : data_(v) {}
    JsonValue(std::string&& v) : data_(std::move(v)) {}
    JsonValue(const char* v) : data_(std::string(v)) {}
    JsonValue(const char* v, size_t len) : data_(std::string(v, len)) {}
    JsonValue(Array v) : data_(std::move(v)) {}
    JsonValue(Object v) : data_(std::move(v)) {}

    enum ObjectTag { object_tag };
    enum ArrayTag { array_tag };
    JsonValue(ObjectTag) : data_(Object{}) {}
    JsonValue(ArrayTag)  : data_(Array{}) {}

    static JsonValue Parse(std::string_view json);

    std::string Dump(int indent = -1) const;

    bool IsNull()   const { return std::holds_alternative<std::nullptr_t>(data_); }
    bool IsBool()   const { return std::holds_alternative<bool>(data_); }
    bool IsInt()    const { return std::holds_alternative<int64_t>(data_); }
    bool IsDouble() const { return std::holds_alternative<double>(data_); }
    bool IsNumber() const { return IsInt() || IsDouble(); }
    bool IsString() const { return std::holds_alternative<std::string>(data_); }
    bool IsArray()  const { return std::holds_alternative<Array>(data_); }
    bool IsObject() const { return std::holds_alternative<Object>(data_); }

    bool            GetBool()   const;
    int64_t         GetInt()    const;
    double          GetDouble() const;
    const std::string& GetString() const;
    const Array&    GetArray()  const;
    Array&          GetArray();
    const Object&   GetObject() const;
    Object&         GetObject();

    size_t Size()   const;
    bool   Empty()  const;
    bool   Contains(std::string_view key) const;

    JsonValue&       operator[](size_t i);
    const JsonValue& operator[](size_t i) const;

    JsonValue&       operator[](std::string_view key);
    const JsonValue& operator[](std::string_view key) const;

    const JsonValue* Find(std::string_view key) const;
    JsonValue*       Find(std::string_view key);

    const JsonValue& At(std::string_view key) const;
    JsonValue&       At(std::string_view key);

    void PushBack(JsonValue val);

    bool operator==(const JsonValue& other) const;
    bool operator!=(const JsonValue& other) const { return !(*this == other); }

    Object::const_iterator begin() const { return GetObject().begin(); }
    Object::const_iterator end()   const { return GetObject().end(); }
    Object::iterator       begin()       { return GetObject().begin(); }
    Object::iterator       end()         { return GetObject().end(); }
};

} // namespace mcp

#ifdef MCP_POP_GETOBJECT_MACRO
#pragma pop_macro("GetObject")
#undef MCP_POP_GETOBJECT_MACRO
#endif
