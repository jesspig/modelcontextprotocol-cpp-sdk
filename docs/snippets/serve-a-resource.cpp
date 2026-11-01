#include <mcp/Content.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <map>
#include <memory>
#include <string>
#include <utility>

// region full
using namespace mcp;

void ServeAResource() {
    auto pair = InMemoryTransport::CreatePair();

    ServerOptions options;
    options.server_info = Implementation{"DocsResourceServer", "1.0.0"};

    auto server = McpServer::Create(std::move(pair.server), options);

    server->RegisterResource("release-notes", "docs://release-notes",
        ResourceOptions{}
            .Title("Release Notes")
            .Description("Notes for the newest published release")
            .MimeType("text/plain"),
        [](const std::string& uri) -> ReadResourceResult {
            TextResourceContents text;
            text.uri = uri;
            text.mime_type = "text/plain";
            text.text = "1.0.0 - first published release.";

            ReadResourceResult result;
            result.contents.push_back(ResourceContents{std::move(text)});
            return result;
        });

    server->RegisterResourceTemplate("user-profile", "docs://users/{userId}/profile",
        ResourceOptions{}
            .Title("User Profile")
            .Description("One profile document per user id")
            .MimeType("application/json"),
        [](const std::string& uri,
           const std::map<std::string, std::string>& vars) -> ReadResourceResult {
            TextResourceContents text;
            text.uri = uri;
            text.mime_type = "application/json";
            text.text = "{\"id\":\"" + vars.at("userId") + "\"}";

            ReadResourceResult result;
            result.contents.push_back(ResourceContents{std::move(text)});
            return result;
        });

    server->SendResourceListChanged();
    server->SendResourceUpdated("docs://release-notes");
}
// endregion
