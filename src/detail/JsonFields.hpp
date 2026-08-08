#pragma once

// JsonFields.hpp — JSON field name constants shared by core serializers

namespace mcp {
namespace detail {

inline constexpr char kMeta[] = "_meta";
inline constexpr char kResultType[] = "resultType";
inline constexpr char kName[] = "name";
inline constexpr char kTitle[] = "title";
inline constexpr char kDescription[] = "description";
inline constexpr char kMimeType[] = "mimeType";
inline constexpr char kIcons[] = "icons";
inline constexpr char kAnnotations[] = "annotations";
inline constexpr char kCacheHint[] = "cacheHint";
inline constexpr char kUri[] = "uri";
inline constexpr char kType[] = "type";
inline constexpr char kNextCursor[] = "nextCursor";
inline constexpr char kTaskId[] = "taskId";
inline constexpr char kRequestState[] = "requestState";
inline constexpr char kJsonrpc[] = "jsonrpc";
inline constexpr char kContent[] = "content";
inline constexpr char kId[] = "id";
inline constexpr char kData[] = "data";
inline constexpr char kMessage[] = "message";
inline constexpr char kText[] = "text";
inline constexpr char kArguments[] = "arguments";
inline constexpr char kRole[] = "role";
inline constexpr char kRoots[] = "roots";
inline constexpr char kError[] = "error";
inline constexpr char kCode[] = "code";
inline constexpr char kInputResponses[] = "inputResponses";
inline constexpr char kMethod[] = "method";
inline constexpr char kCapabilities[] = "capabilities";
inline constexpr char kResult[] = "result";
inline constexpr char kParams[] = "params";
inline constexpr char kTools[] = "tools";
inline constexpr char kServerInfo[] = "serverInfo";
inline constexpr char kInputSchema[] = "inputSchema";
inline constexpr char kReadTimeoutMs[] = "readTimeoutMs";
inline constexpr char kInstructions[] = "instructions";
inline constexpr char kLevel[] = "level";
inline constexpr char kStopReason[] = "stopReason";
inline constexpr char kOpenWorldHint[] = "openWorldHint";
inline constexpr char kOutputSchema[] = "outputSchema";
inline constexpr char kPriority[] = "priority";
inline constexpr char kRequestedSchema[] = "requestedSchema";
inline constexpr char kAudience[] = "audience";
inline constexpr char kDestructiveHint[] = "destructiveHint";
inline constexpr char kBlob[] = "blob";
inline constexpr char kResources[] = "resources";
inline constexpr char kProtocolVersion[] = "protocolVersion";
inline constexpr char kMessages[] = "messages";
inline constexpr char kNotifications[] = "notifications";
inline constexpr char kIdempotentHint[] = "idempotentHint";
inline constexpr char kReadOnlyHint[] = "readOnlyHint";
inline constexpr char kLastModified[] = "lastModified";
inline constexpr char kReason[] = "reason";
inline constexpr char kExtensions[] = "extensions";
inline constexpr char kPrompts[] = "prompts";
inline constexpr char kMode[] = "mode";
inline constexpr char kSampling[] = "sampling";
inline constexpr char kElicitation[] = "elicitation";
inline constexpr char kStructuredContent[] = "structuredContent";
inline constexpr char kExperimental[] = "experimental";
inline constexpr char kProgressToken[] = "progressToken";
inline constexpr char kInputRequests[] = "inputRequests";
inline constexpr char kResource[] = "resource";
inline constexpr char kListChanged[] = "listChanged";

// ResultType enum value strings
inline constexpr char kInputRequiredValue[] = "input_required";
inline constexpr char kCompleteValue[] = "complete";

} // namespace detail
} // namespace mcp
