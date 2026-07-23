#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

    // Tag types for empty containers
    enum ObjectTag { object_tag };
    enum ArrayTag { array_tag };
    JsonValue(ObjectTag) : data_(Object{}) {}
    JsonValue(ArrayTag)  : data_(Array{}) {}

    // Static factories
    static JsonValue Parse(std::string_view json);
    static JsonValue FromObject(Object v) { return JsonValue(std::move(v)); }
    static JsonValue FromArray(Array v)   { return JsonValue(std::move(v)); }

    // Serialize to JSON string
    std::string Dump(int indent = -1) const;

    // Type checks
    bool IsNull()   const { return std::holds_alternative<std::nullptr_t>(data_); }
    bool IsBool()   const { return std::holds_alternative<bool>(data_); }
    bool IsInt()    const { return std::holds_alternative<int64_t>(data_); }
    bool IsDouble() const { return std::holds_alternative<double>(data_); }
    bool IsNumber() const { return IsInt() || IsDouble(); }
    bool IsString() const { return std::holds_alternative<std::string>(data_); }
    bool IsArray()  const { return std::holds_alternative<Array>(data_); }
    bool IsObject() const { return std::holds_alternative<Object>(data_); }

    // Accessors
    bool            GetBool()   const;
    int64_t         GetInt()    const;
    double          GetDouble() const;
    const std::string& GetString() const;
    const Array&    GetArray()  const;
    Array&          GetArray();
    const Object&   GetObject() const;
    Object&         GetObject();

    // Container queries
    size_t Size()   const;
    bool   Empty()  const;
    bool   Contains(std::string_view key) const;

    // Element access for arrays
    JsonValue&       operator[](size_t i);
    const JsonValue& operator[](size_t i) const;

    // Element access for objects
    JsonValue&       operator[](std::string_view key);
    const JsonValue& operator[](std::string_view key) const;

    // Optional key lookup (returns nullptr if not found)
    const JsonValue* Find(std::string_view key) const;
    JsonValue*       Find(std::string_view key);

    // Required key access (throws if missing)
    const JsonValue& At(std::string_view key) const;
    JsonValue&       At(std::string_view key);

    // Push back for arrays
    void PushBack(JsonValue val);

    // Equality
    bool operator==(const JsonValue& other) const;
    bool operator!=(const JsonValue& other) const { return !(*this == other); }

    // Iteration over object
    Object::const_iterator begin() const { return GetObject().begin(); }
    Object::const_iterator end()   const { return GetObject().end(); }
    Object::iterator       begin()       { return GetObject().begin(); }
    Object::iterator       end()         { return GetObject().end(); }
};

} // namespace mcp
