#pragma once

#include <mcp/Meta.hpp>
#include <mcp/Content.hpp>
#include <mcp/Tool.hpp>
#include <mcp/Resource.hpp>
#include <mcp/Prompt.hpp>
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace mcp {

/// Result type discriminator.
enum class ResultType {
    Complete,
    InputRequired,
};

ResultType ResultTypeFromString(std::string_view s);
std::string_view ResultTypeToString(ResultType type);

/// Common paginated request params.
struct PaginatedRequestParams {
    RequestMeta meta;
    std::optional<std::string> cursor;

    nlohmann::json ToJson() const;
    static PaginatedRequestParams FromJson(const nlohmann::json& j);
};

/// Common paginated result.
struct PaginatedResult {
    std::optional<std::string> next_cursor;
    std::optional<nlohmann::json> meta;
};

/// Common result base.
struct Result {
    std::optional<nlohmann::json> meta;
    ResultType result_type{ResultType::Complete};
};

/// Empty result (e.g., ping, subscribe).
struct EmptyResult : Result {
    nlohmann::json ToJson() const;
    static EmptyResult FromJson(const nlohmann::json& j);
};

/// Ping result.
struct PingResult : Result {};

// ── Tool results ──

struct CallToolResult : Result {
    std::vector<Content> content;
    std::optional<nlohmann::json> structured_content;
    bool is_error{false};

    nlohmann::json ToJson() const;
    static CallToolResult FromJson(const nlohmann::json& j);

    static CallToolResult Success(std::string_view text);
    static CallToolResult Error(std::string_view message);
};

struct ListToolsResult : PaginatedResult {
    std::vector<Tool> tools;

    nlohmann::json ToJson() const;
    static ListToolsResult FromJson(const nlohmann::json& j);
};

// ── Resource results ──

struct ListResourcesResult : PaginatedResult {
    std::vector<Resource> resources;

    nlohmann::json ToJson() const;
    static ListResourcesResult FromJson(const nlohmann::json& j);
};

struct ListResourceTemplatesResult : PaginatedResult {
    std::vector<ResourceTemplate> resource_templates;

    nlohmann::json ToJson() const;
    static ListResourceTemplatesResult FromJson(const nlohmann::json& j);
};

struct ReadResourceResult : Result {
    std::vector<ResourceContents> contents;

    nlohmann::json ToJson() const;
    static ReadResourceResult FromJson(const nlohmann::json& j);
};

// ── Prompt results ──

struct ListPromptsResult : PaginatedResult {
    std::vector<Prompt> prompts;

    nlohmann::json ToJson() const;
    static ListPromptsResult FromJson(const nlohmann::json& j);
};

struct GetPromptResult : Result {
    std::vector<PromptMessage> messages;
    std::optional<std::string> description;

    nlohmann::json ToJson() const;
    static GetPromptResult FromJson(const nlohmann::json& j);
};

// ── Completion ──

struct CompleteResult : Result {
    nlohmann::json completion;

    nlohmann::json ToJson() const;
    static CompleteResult FromJson(const nlohmann::json& j);
};

// ── Subscription ──

struct Subscription {
    std::string uri;

    nlohmann::json ToJson() const;
    static Subscription FromJson(const nlohmann::json& j);
};

struct SubscriptionFilter {
    bool tools_list_changed{false};
    bool resources_list_changed{false};
    bool prompts_list_changed{false};

    nlohmann::json ToJson() const;
};

// ── Discovery ──

struct DiscoverResult : Result {
    std::vector<std::string> supported_versions;
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;

    nlohmann::json ToJson() const;
    static DiscoverResult FromJson(const nlohmann::json& j);
};

// ── Initialize (legacy) ──

struct InitializeResult : Result {
    std::string protocol_version;
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;

    nlohmann::json ToJson() const;
    static InitializeResult FromJson(const nlohmann::json& j);
};

// ── InputRequired (MRTR) ──

struct InputRequiredParams {
    nlohmann::json input_requests;

    nlohmann::json ToJson() const;
};

struct InputRequiredResult : Result {
    InputRequiredParams input_requests;
    std::optional<std::string> request_state;

    nlohmann::json ToJson() const;
    static InputRequiredResult FromJson(const nlohmann::json& j);
};

// ── Sampling [deprecated] ──

struct CreateMessageResult : Result {
    std::string model;
    std::string role;
    Content content;

    nlohmann::json ToJson() const;
    static CreateMessageResult FromJson(const nlohmann::json& j);
};

// ── Roots [deprecated] ──

struct ListRootsResult : Result {
    std::vector<Root> roots;
    // Root type placeholder
    nlohmann::json ToJson() const;
    static ListRootsResult FromJson(const nlohmann::json& j);
};

struct Root {
    std::string uri;
    std::optional<std::string> name;

    nlohmann::json ToJson() const;
    static Root FromJson(const nlohmann::json& j);
};

// ── Elicitation ──

struct ElicitResult : Result {
    nlohmann::json result;

    nlohmann::json ToJson() const;
    static ElicitResult FromJson(const nlohmann::json& j);
};

template <typename T>
struct ElicitResultT : ElicitResult {
    T value;
};

// ── Cache ──

struct CacheHint {
    std::optional<int64_t> ttl_ms;
    std::optional<std::string> cache_scope;  // "public" | "private" | string
};

struct CacheConfig {
    std::optional<CacheMode> default_mode;
    std::optional<int64_t> default_ttl_ms;
    std::string partition;
};

enum class CacheMode {
    Use,
    Bypass,
    Refresh,
};

} // namespace mcp
