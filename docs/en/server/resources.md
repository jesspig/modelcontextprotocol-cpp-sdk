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
`ResourceOptions` fields (`description`, `title`, `mime_type`, `icons`) are propagated to the protocol-level `Resource` and `ResourceTemplate` structs returned via `resources/list` and `resources/templates/list`.
:::

## Change Notifications

The server notifies clients when a resource's contents or the resource list changes:

```cpp
// A single resource changed — delivered only to subscribers of that uri
server->SendResourceUpdated("file:///app/config.json");

// The resource list itself changed (resources added or removed)
server->SendResourceListChanged();
```

`SendResourceUpdated(uri)` publishes `notifications/resources/updated`, while `SendResourceListChanged()` publishes `notifications/resources/list_changed`. Delivery adapts to the negotiated protocol era:

- **2026-07-28 and later**: subscriptions made through `subscriptions/listen` carry an explicit filter, so a notification reaches only the subscribers whose filter matches.
- **2025 and earlier**: `resources/subscribe` has no filter to consult, so the notification is broadcast to the connection.
