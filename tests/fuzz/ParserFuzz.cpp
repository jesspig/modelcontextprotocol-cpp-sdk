#include <mcp/JsonRpc.hpp>
#include <mcp/JsonValue.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    try {
        const mcp::JsonValue value = mcp::JsonValue::Parse(text);
        (void)value;
    } catch (...) {
    }
    try {
        const mcp::JsonRpcMessage message = mcp::DeserializeMessage(text);
        (void)message;
    } catch (...) {
    }
    return 0;
}
