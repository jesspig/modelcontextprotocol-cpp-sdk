#pragma once

#include <mcp/JsonRpc.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace mcp { namespace detail {

enum class HttpRequestPurpose {
    MessagePost,
    ListenStream,
    ResumeStream,
    SessionDelete,
    ToolAnnotationsRefresh,
};

struct HttpRequestHeaderContext {
    std::string_view http_method;
    std::string_view url;
    HttpRequestPurpose purpose = HttpRequestPurpose::MessagePost;
    std::optional<RequestId> request_id;
    int attempt_index = 0;
};

struct HttpResponseChallengeContext {
    int status_code = 0;
    std::string_view www_authenticate;
    std::string_view dpop_nonce;
};

}} // namespace mcp::detail
