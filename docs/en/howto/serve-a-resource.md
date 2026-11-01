# Serve a Resource

A resource is read-only data a client fetches by URI — closest in shape to a REST GET endpoint. This guide covers registering static and URI-template resources, how `resources/read` resolves a request, and how change notifications are delivered.

## Prerequisites

- You can already bring up a server instance: see [Quick Start](/en/start/quickstart).
- You have a `std::shared_ptr<ITransport>`. For an in-process round trip, `InMemoryTransport::CreatePair()` hands back a matched `.client` and `.server`.
- Signatures and field tables live only in the reference layer and are not repeated here: registration entry points and registration-time validation in [Server Facade Reference](/en/reference/server), `ResourceOptions`, `ReadResourceResult` and `ResourceLink` in [Type Reference](/en/reference/types).

## Step 1: Pick Static Resource or Template

| Shape of your data | Registration entry point | Appears in | Extra argument the handler receives |
|---|---|---|---|
| Few entries, each URI a full literal | `McpServer::RegisterResource` | `resources/list` | None, only the requested URI |
| Homogeneous, sharded by id or path | `McpServer::RegisterResourceTemplate` | `resources/templates/list` | The decoded variable map |

Both kinds land in one registry and share its conflict check: a duplicate name, or a duplicate URI / URI-template string, makes the later registration throw `McpError` with `InvalidParams` (`src/server/McpServer.cpp:805-814`, `:854-863`).

::: info
Any successful registration declares the `resources` capability and sets both `subscribe` and `list_changed` to true (`src/server/McpServer.cpp:1950-1954`). There is no way to turn either off on its own.
:::

## Step 2: Register a Static Resource

```cpp
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
```

- Every variant in `contents` needs its `uri` filled in by you — the SDK does not backfill it. One response may carry several entries.
- Text goes into `TextResourceContents::text`, binary into `BlobResourceContents::blob` as base64 per the protocol. The SDK neither decodes nor validates it (`src/core/Content.cpp:92-104`), so `mime_type` has to be correct on its own.
- `ResourceOptions` covers only `title` / `description` / `mime_type` / `icons`. The protocol fields `size` and `annotations` have no registration entry point, so a static list entry cannot carry them.

## Step 3: Write the URI Template and Read Its Variables

```cpp
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
```

Template syntax is a subset of RFC 6570, and every rule below is enforced at registration time:

| Aspect | Behaviour in this SDK |
|---|---|
| Operators | `{var}` `{+var}` `{#var}` `{.var}` `{/var}` `{;var}` `{?var}` `{&var}` supported; a trailing `*` is accepted and ignored |
| Variable names | `A-Za-z0-9_` only, with `.` grouping and `%XX` escapes allowed |
| Prefix modifiers | `{var:3}` not supported |
| Size limits | 2048 characters per template, 32 variables |
| Structural rejections | Unbalanced braces, more than one multi-segment variable, two adjacent variables with no literal separator |

Anything illegal throws `McpError` with a message shaped like `invalid resource template '<uri>': <reason>` (`src/server/McpServer.cpp:836-841`).

Variable values handed to the handler are **percent-decoded**: a request for `docs://users/a%20b/profile` gives `vars.at("userId")` equal to `a b`. A simple variable accepts only unreserved characters and `%XX`, so a bare `/`, `?` or `#` makes the whole template miss; use `{+var}` when one segment must carry reserved characters.

## Step 4: Understand Match Order and the Miss Shape

`resources/read` matches in two passes (`src/server/McpServer.cpp:2789-2809`): first every **non-template** entry is compared for exact URI equality in registration order, and only if none hits does it walk the templates in registration order.

- Static preference is independent of registration order: with a template registered first and a static entry second, a request for `test://x` still runs the static handler (`McpServerTest.ReadResourcePrefersStaticOverTemplate`).
- Total miss: `InvalidParams`, message `resource not found: <uri>`, with `data` echoing `{"uri":"<uri>"}` (`src/server/McpServer.cpp:2812-2819`).
- `McpErrorCode::ResourceNotFound` (`-32002`) appears only in the known-code table; the server's miss path never throws it, so the peer always receives `-32602`.

::: warning
A `McpError` thrown from your handler is encoded verbatim as a JSON-RPC error response. Any other exception collapses to `InternalError` with the fixed text `internal error`, and the original `what()` only reaches the log. This differs from the tool path — see [Error Codes and Model](/en/reference/errors).
:::

## Step 5: Notify Subscribers on Change

```cpp
server->SendResourceListChanged();
server->SendResourceUpdated("docs://release-notes");
```

- `SendResourceUpdated(uri)` is subscription-routed only: it reaches subscribers whose filter lists that URI. With an empty subscription table it returns immediately and sends nothing (`src/protocol/McpSessionHandler.cpp:979-988`).
- While the instance is running, registration emits a `list_changed` on its own (`src/server/McpServer.cpp:819-821`); call it by hand only after you add or drop entries yourself.
- Legacy `resources/subscribe` and the modern `resourceSubscriptions` of `subscriptions/listen` write into the same table, so `SendResourceUpdated` matches by URI in both eras. The delivery scope of `list_changed` splits by era (`src/server/McpServer.cpp:953-959`).

## Step 6: Reference Resources as Links and Slices

Use `ResourceLink` to hand back a reference instead of the contents: missing, non-string or empty `name` and `uri` make the parsing side throw `InvalidParams`, and a negative `size` is rejected the same way (`src/core/Content.cpp:237-264`). The name and URI you registered must be non-empty — the same constraints apply when the peer parses your link.

When a client only wants part of a file, it sends a `FileRef`: with `enable_file_input` on, a tool argument holding `{"uri","offset","length"}` is resolved through the very same two-pass match, the full resource is read, and then sliced. `offset` / `length` default to 0, and once present they must be integral (integer, or a double with an integral value) inside the int64 range, otherwise `DeserializeFailed` (`include/mcp/McpTypes.hpp:162-189`).

## Complete code

<<< @/snippets/serve-a-resource.cpp#full

## Verification

```bash
ctest --preset debug -R 'McpServerTest\.(RegisterResource|ReadResource|SendResourceUpdated)|UriTemplateTest\.'
```

All 31 cases should report `Passed`, with the final line `100% tests passed, 0 tests failed out of 31`. Three assertions confirm you are reading the same implementation:

| Case | Asserted value |
|---|---|
| `McpServerTest.ReadResourceTemplateValueIsPctDecoded` | `contents[0].text` equals `id=a b` |
| `McpServerTest.ReadResourcePrefersStaticOverTemplate` | `contents[0].text` equals `static` |
| `McpServerTest.ReadResourceMissCarriesUri` | error code `-32602`, `data.uri` equals `test://missing-resource` |

## Era differences

`resources/list`, `resources/templates/list`, `resources/read` and the `resources/updated` / `resources/list_changed` notifications exist in both eras. What differs is how a subscription is established (`resources/subscribe` is legacy-only) and the delivery scope of `list_changed` (broadcast in the legacy era, subscribers only in the modern era). See [Protocol Versions](/en/learn/protocol-versions).

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| Registration throws `resource already registered` or `resource uri already registered` | Static entries and templates share one conflict table | Rename or change the URI prefix; do not expect separate tables |
| Contents changed while idle and the client noticed nothing | With no subscriber, `SendResourceUpdated` silently drops | Follow up with `SendResourceListChanged()`, or have the client re-read |
| A template throws `invalid resource template` immediately | `{var:3}`, adjacent variables, or several multi-segment variables | Add a literal separator; keep at most one `{+var}` per template |
| A variable containing `/` never matches | Simple variables reject reserved characters | Use `{+var}`, or percent-encode on the client side |
| The peer gets `-32602` instead of `-32002` | The miss path deliberately uses `InvalidParams` | Branch on the `resource not found:` message prefix; `data.uri` is already echoed |
| A freshly registered resource is missing from the list | Pagination truncates at 100 entries per page | Page with `cursor`, or move the long tail into templates |

## Next steps

- Parameterised argument capture: [Serve a Prompt](/en/howto/serve-a-prompt)
- Accept a file slice as a tool argument: [Register a Tool](/en/howto/register-a-tool)
- Use a resource as the carrier for user input: [Collect User Input](/en/howto/collect-user-input)

## Source behind this page

- `include/mcp/server/McpServer.hpp`
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/McpTypes.hpp` (`ResourceOptions`, `ReadResourceResult`, `FileRef`, `SubscriptionFilter`)
- `include/mcp/Content.hpp` (`TextResourceContents`, `BlobResourceContents`, `ResourceLink`)
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp` (registration, two-pass `resources/read` matching, subscriptions and notifications)
- `src/protocol/McpSessionHandler.cpp` (`AddSubscription`, `NotifySubscribers`)
- `src/detail/UriTemplate.hpp` (RFC 6570 subset parsing and matching)
- `src/core/Content.cpp` (`ResourceContents` and `ResourceLink` serialization checks)
