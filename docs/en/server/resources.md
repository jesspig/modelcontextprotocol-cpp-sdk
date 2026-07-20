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

::: note
`ResourceOptions` is accepted by the registration API but `description` and `mime_type` are currently not propagated to the protocol-level resource exposed via `resources/list`. The `Resource` struct returned in `ListResourcesResult` only includes `uri` and `name`.
:::
