#include <detail/JsonSchemaValidator.hpp>
#include <mcp/JsonValue.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    try {
        const mcp::JsonValue instance = mcp::JsonValue::Parse(text);
        static const mcp::JsonValue fixed_schema = mcp::JsonValue::Parse(
            R"({"type":"object","properties":{"a":{"type":"integer","minimum":0},"b":{"type":"array","items":{"type":"string"}}}})");
        std::string error;
        (void)mcp::detail::ValidateJsonSchema(instance, fixed_schema, error);
        (void)mcp::detail::ValidateJsonSchema(fixed_schema, instance, error);
    } catch (...) {
    }
    return 0;
}
