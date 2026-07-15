# Resources

Resources expose structured data that clients can read. They are analogous to GET endpoints in a REST API.

## Registering a Resource

```cpp
server->RegisterResource(
    "config",
    "file:///app/config.json",
    ResourceOptions{}.Description("Application configuration").MimeType("application/json"),
    [](const std::string& uri) -> ReadResourceResult {
        ReadResourceResult result;
        TextResourceContents contents;
        contents.uri = uri;
        contents.text = R"({"debug": true, "port": 8080})";
        contents.mime_type = "application/json";
        result.contents.push_back(contents);
        return result;
    });
```

## Resource Templates

Resource templates use URI patterns with `{variables}`:

```cpp
server->RegisterResourceTemplate(
    "user-data",
    "file:///users/{userId}/profile",
    ResourceOptions{}.Description("User profile data"),
    [](const std::string& uri,
       const std::map<std::string, std::string>& vars) -> ReadResourceResult {
        auto userId = vars.at("userId");
        // Fetch user data...
    });
```

## Resource Annotations

Resource annotations control visibility and priority:

```cpp
ResourceOptions opts;
opts.description = "Sensitive configuration";
opts.mime_type = "application/json";
// annotations are set indirectly through MCP protocol
```
