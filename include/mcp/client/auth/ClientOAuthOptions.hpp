#pragma once

#include <string>
#include <vector>
#include <functional>

namespace mcp {

struct ClientOAuthOptions {
    std::string redirect_uri;
    std::string client_id;
    std::string client_secret;
    std::string client_metadata_document_uri;
    std::vector<std::string> scopes;
    std::function<void(std::string_view)> authorization_redirect_handler;
};

} // namespace mcp
