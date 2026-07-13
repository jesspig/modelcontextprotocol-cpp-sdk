#pragma once

#include <mcp/McpTypes.hpp>
#include <mcp/server/RequestContext.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

// ── Extension — base class for pluggable MCP extensions ──
// Extensions can contribute tools, resources, methods, and intercept calls.
// Mimics Python's Extension base class and SEP-2133.
class Extension {
public:
    virtual ~Extension() = default;

    // Unique identifier for this extension (e.g. "com.example/my-ext")
    virtual std::string Identifier() const = 0;

    // Tools contributed by this extension
    virtual std::vector<std::shared_ptr<McpServerTool>> Tools() { return {}; }

    // Methods contributed: (method_name, handler)
    virtual std::vector<std::pair<std::string,
        std::function<void(const JsonRpcRequest&, std::promise<nlohmann::json>)>>>
    Methods() { return {}; }

    // Tool call interceptor — can modify params, result, or reject
    // Return true to continue with the next interceptor/handler
    // Return false if the interceptor handled the call completely
    virtual bool InterceptToolCall(
        const RequestContext<CallToolRequestParams>& ctx,
        CallToolResult& result) { return true; }
};

// ── ExtensionListResult — response to server/extensions/list ──
struct ExtensionListResult : Result {
    std::vector<std::string> extensions;
};
inline void to_json(nlohmann::json& j, const ExtensionListResult& v) {
    j = nlohmann::json{{"extensions", v.extensions}};
    if (v.meta) j["_meta"] = *v.meta;
}
inline void from_json(const nlohmann::json& j, ExtensionListResult& v) {
    v.extensions = j.at("extensions").get<std::vector<std::string>>();
    if (auto it = j.find("_meta"); it != j.end()) v.meta = *it;
}

} // namespace mcp
