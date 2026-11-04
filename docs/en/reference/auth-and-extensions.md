# Authorization and Extensions Reference

This page is the **authoritative field table** for the authorization layer (OAuth, PKCE, DPoP, token caches) and the extension layer (server card, apps, triggers, skill verification); every entry is checked against the headers. Facade signatures live in the [Server reference](/en/reference/server) and [Client reference](/en/reference/client), transport options in the [Transports reference](/en/reference/transports), extension switch semantics in [Extension switches](/en/learn/extensions), the single authoritative era table in [Protocol versions](/en/learn/protocol-versions), and `Skill`/`SkillResource` fields in the [Types reference](/en/reference/types). No procedural steps on this page. Types and functions under `detail/` are not user API: they are named only where they explain behaviour and never listed in interface tables.

## OAuth client provider

### `OAuthClientOptions` fields

Declared in `include/mcp/client/auth/OAuthClientProvider.hpp`, implemented in `src/client/auth/OAuthClientProvider.cpp`; `AuthorizationCodeResult` holds `code`, `state` (`std::string`) and `iss` (`std::optional<std::string>`, `nullopt`).

| Field | Type | Default | Notes |
|---|---|---|---|
| `server_url` | `std::string` | empty | MCP server URL, the base for metadata discovery and the resource indicator |
| `redirect_uri` | `std::string` | empty | Authorization callback address, used by registration, the authorization URL and code exchange |
| `client_id` | `std::optional<std::string>` | `nullopt` | When empty and the server advertises a registration endpoint, dynamic registration runs; when the value itself is an http(s) URL it is fetched as a client metadata document |
| `client_secret` | `std::optional<std::string>` | `nullopt` | Only required by the `client_credentials` branch when no private-key assertion is used |
| `scopes` | `std::vector<std::string>` | empty | Requested scopes; `StepUpAuthorization` appends in place |
| `scope_filter` | `std::function<std::vector<std::string>(const std::vector<std::string>&)>` | unset | Called with `scopes` as the input on every scope resolution; unset means use them verbatim |
| `token_cache` | `std::shared_ptr<ITokenCache>` | `nullopt` | Construction substitutes an `InMemoryTokenCache` when empty |
| `resource` | `std::optional<std::string>` | `nullopt` | RFC 8707 resource indicator; falls back to the normalized `server_url` when unset |
| `application_type` | `std::string` | `"native"` | Dynamic-registration body field; an empty string is sent as `native` |
| `enable_dpop` | `bool` | `false` | When on, token-endpoint requests carry a `DPoP` header and `Authorization` switches to the `DPoP` scheme |
| `private_key_pem` | `std::optional<std::string>` | `nullopt` | Signing key for `private_key_jwt` assertions; only effective when the server advertises that auth method |
| `signing_algorithm` | `std::string` | `"ES256"` | Assertion algorithm name; anything other than `ES256` throws `InvalidParams` while building the assertion |
| `client_assertion_clock_seconds` | `std::function<int64_t()>` | unset | Clock source for assertion `iat`/`exp`; unset uses `dpop::NowSeconds` |
| `allowed_authorization_server_origins` | `std::vector<std::string>` | empty | Authorization-server origin allow-list, checked against the issuer and each endpoint; empty means no narrowing |
| `authorization_redirect_handler` | `std::function<void(std::string_view url)>` | unset | Invoked with the full authorization URL so the caller can open a browser; does not influence the flow outcome |
| `authorization_code_callback` | `std::function<std::optional<AuthorizationCodeResult>()>` | unset | Mandatory, otherwise the authorization flow returns `false` immediately; an empty result, a `state` mismatch or an `iss` mismatch all fail |
| `token_update_handler` | `std::function<void(const TokenContainer&)>` | unset | Invoked every time tokens land in the cache |

### `OAuthClientProvider` public methods

Declared in the same header; private nested types and members are not user API and are omitted.

| Method | Signature | Return semantics | Notes |
|---|---|---|---|
| Construct/destruct | `explicit OAuthClientProvider(OAuthClientOptions options)` / `~OAuthClientProvider()` | — | Options are taken by value; the token cache is self-created by default |
| `IsSecureEndpoint` | `static bool IsSecureEndpoint(std::string_view url)` | `bool` | https check, loopback hosts excepted |
| `IsSameOrigin` | `static bool IsSameOrigin(std::string_view lhs, std::string_view rhs)` | `bool` | Origin (scheme+host+port) comparison; `false` if either side yields no origin |
| `Authenticate` | `bool Authenticate()` | whether a usable token exists | Discover metadata → register / fetch document as needed → fresh cache returns true → refresh expired tokens, with `invalid_grant`/`invalid_client` falling back to the full authorization flow; cached tokens with a mismatched resource are cleared first |
| `AuthenticateClientCredentials` | `bool AuthenticateClientCredentials()` | whether tokens were stored | Requires `client_id`, plus `client_secret` when no assertion is used; never runs the code flow |
| `GetAccessToken` | `std::string GetAccessToken()` | token string, empty when none | When nearing expiry with a refresh token, joins coalesced refreshing (concurrent callers share one refresh); refresh failure throws `InternalError` |
| `RefreshTokens` | `bool RefreshTokens()` | whether the refresh succeeded | Refreshes directly, no coalescing; both `invalid_grant` and transient failure return `false` |
| `Revoke` | `void Revoke()` | void | POSTs only when a revocation endpoint exists (non-https throws `InvalidParams`); clears the cache regardless |
| `IsAuthenticated` / `HasToken` | `bool IsAuthenticated() const` / `bool HasToken() const` | `bool` | The former requires an unexpired token, the latter only a non-empty access token; both check resource matching |
| `GetAuthorizationHeader` | `std::string GetAuthorizationHeader() const` | `DPoP <token>` or `Bearer <token>`, empty when none | Read-only, never triggers a refresh; the scheme name follows `enable_dpop` |
| `BuildDPoPProof` | `std::string BuildDPoPProof(std::string_view htm, std::string_view htu, std::optional<std::string_view> access_token = std::nullopt)` | proof JWT; empty when `enable_dpop` is false | Lazily generates the P-256 key on first use and attaches the recorded nonce |
| `ApplyDPoPProofHeader` | `void ApplyDPoPProofHeader(const HttpRequestHeaderContext& context, std::map<std::string, std::string>& headers)` | void | Builds the proof from the context method and URL into `headers["DPoP"]`; leaves headers untouched when dpop is off, the method is missing or the URL is unparseable |
| `ObserveDPoPNonce` / `SetDPoPNonce` | `void ObserveDPoPNonce(std::string_view dpop_nonce)` / `void SetDPoPNonce(std::string nonce)` | void | The former ignores empty strings and anything over 1024 bytes; the latter overwrites unconditionally, ignoring the dpop switch |
| `StepUpAuthorization` | `bool StepUpAuthorization(const std::vector<std::string>& additional_scopes)` | whether re-authorization succeeded | Append scopes → `Revoke()` → rerun the authorization flow, which drops the current tokens |
| `HandleAuthChallenge` | `bool HandleAuthChallenge(std::string_view www_authenticate)` | whether metadata was rebuilt from it | Recognizes only `resource_metadata="…"`; headers over 4096 bytes, non-https URLs, documents over 65536 bytes, resource mismatch or an authorization server outside the allow-list all return `false` |

### PKCE helpers and the OpenSSL dependency boundary

The `pkce` namespace in the same header:

| Symbol | Signature | Notes |
|---|---|---|
| `GenerateCodeVerifier` | `std::string GenerateCodeVerifier()` | 32 random bytes → base64url; the flow also uses it for `state` |
| `ComputeCodeChallenge` | `std::string ComputeCodeChallenge(std::string_view code_verifier)` | `S256`: SHA-256 then base64url |
| `Base64UrlEncode` | `std::string Base64UrlEncode(std::string_view input)` | Unpadded base64url |

| Capability | With OpenSSL | Without OpenSSL |
|---|---|---|
| PKCE random source | `RAND_bytes`, throws `std::runtime_error` on failure | `BCryptGenRandom` on Windows, otherwise `std::random_device` byte by byte |
| PKCE digest | built-in SHA-256, never OpenSSL | identical, always the built-in implementation |
| DPoP keys, proofs, verification | OpenSSL EC/ECDSA | throws `McpError` (see the DPoP subsection) |
| `private_key_jwt` assertion signing | OpenSSL EC | same path throws `McpError` |
| DPoP nonce and assertion `jti` random source | `RAND_bytes` | `std::random_device` (no Windows branch) |
| TLS | enabled | TLS disabled |

The PKCE and assertion branches are in `src/client/auth/OAuthClientProvider.cpp`, the DPoP side in `src/ext/DPoP.cpp`, and the TLS switch in `cmake/FetchDependencies.cmake`.

Build side: `MCP_WITH_OPENSSL` takes `AUTO` (default, used only when found), `ON` (configure fails if not found) or `OFF` (forced off), see `cmake/FetchDependencies.cmake`; the `MCP_HAVE_OPENSSL` define and linking are applied to `mcp-transport`/`mcp-client`/`mcp-ext` by the single INTERFACE target `mcp-openssl`, see `CMakeLists.txt`; only OpenSSL 3.x/4.x are supported and an unsupported major version fails configure rather than silently disabling TLS, see `cmake/OpenSslVersion.cmake`.

### Wiring into the HTTP client transport

No other public header references the provider; the three hooks are wired by the caller through `HttpClientTransportOptions`. The in-repo example connects only two of them (`examples/conformance/client/main.cpp`) and `auth_challenge_handler` has no example wiring point.

| Transport option field | Corresponding provider method |
|---|---|
| `auth_challenge_handler` | `HandleAuthChallenge` |
| `apply_per_request_headers` | `ApplyDPoPProofHeader` |
| `observe_response_challenge` | `ObserveDPoPNonce` |

All three fields are declared in `include/mcp/transport/StreamableHttpClientTransport.hpp`.

## DPoP

### Public symbols

`include/mcp/ext/DPoP.hpp`, namespace `mcp::ext::dpop`; implemented in `src/ext/DPoP.cpp`.

| Symbol | Signature/type | Default | Notes |
|---|---|---|---|
| `EcP256Jwk` | `struct { std::string x; std::string y; std::string d; }` | three empty strings | P-256 JWK where `d` is the private scalar; `CreateProof` requires all three |
| `GenerateKey` | `EcP256Jwk GenerateKey()` | — | Generates a P-256 key |
| `ComputeThumbprint` | `std::string ComputeThumbprint(std::string_view x, std::string_view y)` | — | SHA-256 over `{"crv":"P-256","kty":"EC","x":…,"y":…}` then base64url; throws `InvalidParams` when x or y is empty; independent of OpenSSL |
| `PublicJwkJson` | `std::string PublicJwkJson(const EcP256Jwk& key)` | — | Emits only `crv`/`kty`/`x`/`y`, never `d`; independent of OpenSSL |
| `CreateProof` | `std::string CreateProof(const EcP256Jwk& key, std::string_view htm, std::string_view htu, std::optional<std::string_view> access_token = std::nullopt, std::optional<std::string_view> nonce = std::nullopt, std::optional<int64_t> iat_override_seconds = std::nullopt)` | three `nullopt` | Produces the DPoP JWT: header `typ=dpop+jwt`, `alg=ES256`, embedded `jwk`; payload `htm`/`htu`/`jti`/`iat`, plus `ath` when an access token is given |
| `VerifyProof` | `bool VerifyProof(std::string_view proof, const DPoPVerifyOptions& options, JtiReplayCache& replay)` | — | Checks `typ`/`alg`, `htm`/`htu` (with `alternate_htu` allowed), `ath`, nonce, the time window, the ECDSA signature and jti replay; returns `false` on mismatch but throws when OpenSSL is absent |
| `IsNonceFresh` / `NowSeconds` | `bool IsNonceFresh(int64_t iat_seconds, int64_t now_seconds, int64_t window_seconds = 300)` / `int64_t NowSeconds()` | `300` | `false` when `iat` is later than now or the window is negative; the time source is `system_clock` seconds; neither needs OpenSSL |
| `GenerateNonce` | `std::string GenerateNonce(std::size_t random_bytes = 16)` | `16` (0 becomes 16, over 64 clamps to 64) | base64url random string |

### `DPoPVerifyOptions` fields and `JtiReplayCache`

| Symbol | Signature/type | Default | Notes |
|---|---|---|---|
| `htm` / `htu` / `access_token` | `std::string` ×3 | empty strings | Expected method and URL, plus the access token used to compare `ath` |
| `alternate_htu` | `std::optional<std::string>` | `nullopt` | Lets one proof match a second URL |
| `expected_nonce` | `std::optional<std::string>` | `nullopt` | When non-empty the proof must carry the same nonce |
| `window_seconds` | `int64_t` | `300` | `iat` freshness window |
| `now_seconds` | `int64_t` | `0` | `0` means call `NowSeconds()` |
| Constructor | `explicit JtiReplayCache(int64_t window_seconds = 300)` | `300` (negative clamps to 0) | Each lookup also prunes entries beyond the window |
| `CheckAndStore` / `Clear` | `bool CheckAndStore(std::string_view jti, int64_t iat_seconds, int64_t now_seconds)` / `void Clear()` | — | Empty or already-seen `jti` returns `false`, otherwise the entry is recorded and `true` is returned; `Clear` empties the table; both are locked |

### Behaviour without OpenSSL

::: danger
With OpenSSL disabled, `enable_dpop` and `private_key_pem` are **not degraded but unavailable**: `GenerateKey`, `CreateProof` and `VerifyProof` throw `McpError(InternalError, "DPoP requires OpenSSL EC support, which is not available in this build")`, and client-assertion signing uses the same EC routine family. There is no way to complete DPoP on built-in code alone.
:::

| Group | Behaviour |
|---|---|
| `GenerateKey`, `CreateProof`, `VerifyProof` | Throw `McpError`, no fallback path |
| `ComputeThumbprint`, `PublicJwkJson`, `IsNonceFresh`, `NowSeconds`, `JtiReplayCache`, `GenerateNonce` | Defined outside the EC section and remain callable; digests use the built-in SHA-256 and the random source falls back |

Both builds come from the same `src/ext/DPoP.cpp`, split by `#if defined(MCP_HAVE_OPENSSL)` sections.

## Token caches

### `TokenContainer` and `ITokenCache`

`include/mcp/client/auth/TokenCache.hpp` (the authorization cache contract ships with client/auth), implemented in `src/client/auth/OAuthClientProvider.cpp`.

| Symbol | Signature/type | Default | Notes |
|---|---|---|---|
| `access_token` / `refresh_token` / `scopes` | `std::string` / `std::string` / `std::vector<std::string>` | empty/empty/empty | An empty refresh token makes refreshing impossible |
| `expires_at` plus checks | `int64_t`; `bool IsExpired() const` / `bool WillExpireSoon(int64_t margin_ms = 60000) const` | `0`; `60000` | Absolute expiry timestamp in milliseconds; expiry is considered near 60 seconds early by default |
| `token_type` / `resource` | `std::string` / `std::optional<std::string>` | `"Bearer"` / `nullopt` | The provider matches `resource` against the request; a mismatch counts as having no token |
| `StoreTokens` / `ClearTokens` | `virtual void StoreTokens(const TokenContainer&) = 0` / `virtual void ClearTokens() = 0` | — | Persist tokens / drop them |
| `GetTokens` | `virtual std::optional<TokenContainer> GetTokens() = 0` | — | Non-const member |
| `StoreBoundIssuer` | `virtual void StoreBoundIssuer(const std::string& issuer)` | empty body (overridable) | Issuer-bound persistence anchor; the provider writes the issuer it first discovers and thereafter rejects any issuer change |
| `GetBoundIssuer` | `virtual std::optional<std::string> GetBoundIssuer()` | `nullopt` (overridable) | Reads the bound issuer back; the default means no persistence |
| `OAuthMetadata` | `struct`: issuer, authorization/token/revocation/registration/jwks/resource endpoints and four `*_supported` lists; `static std::optional<OAuthMetadata> Discover(std::string_view server_url)` | empty strings/lists | The last four endpoints and `resource` are `optional`; `Discover` fetches `/.well-known/oauth-authorization-server` and returns empty on failure |
| `OAuthTokenResponse` | `struct`: `access_token`/`refresh_token`/`expires_in`/`token_type`/`scope` | `expires_in = 3600`, `token_type = "Bearer"` | Token-endpoint payload carrier |
| `ClientRegistrationInfo::Register` | `static std::optional<ClientRegistrationInfo> Register(std::string_view registration_endpoint, std::string_view redirect_uri, std::string_view client_name, std::string_view application_type = "native")` | `"native"` | Dynamic client registration entry point |

### Built-in implementations

| Symbol | Signature/type | Notes |
|---|---|---|
| `InMemoryTokenCache` | `class : public ITokenCache` | In-process implementation, overrides all five interfaces including issuer binding, internally locked |
| `FileTokenCache` construct/destruct | `explicit FileTokenCache(std::filesystem::path cache_path)` / `~FileTokenCache() override` | The constructor loads from disk (a missing file means an empty cache); destructor persistence failures are logged as Error only, never thrown |
| `StoreTokens` / `GetTokens` / `ClearTokens` | `override` | Every write is flushed synchronously; with no tokens the flush deletes the file |
| `StoreBoundIssuer` / `GetBoundIssuer` | `override` | The anchor lives in `<cache_path>.issuer`; an empty string removes that file, write failures are logged as Error |
| Permission semantics (POSIX) | Before loading, `stat` requires the current uid as owner and all group/other bits clear; otherwise the load is refused with an Error log | Refusal equals an empty cache and throws nothing |
| Permission semantics (Windows) | The whole cache is protected per user through DPAPI (`CryptProtectData`/`CryptUnprotectData`); decryption failure discards the cache | Never plaintext JSON on disk; writes go through the internal owner-only atomic secure writer |

`InMemoryTokenCache` shares the header above; `FileTokenCache` is declared in `include/mcp/storage/FileTokenCache.hpp` and its flushing and permission checks are in `src/client/FileTokenCache.cpp`.

## Extension types

### Extension and identifier constants

| Constant | Value | Meaning |
|---|---|---|
| `ext::kServerCardExtensionId` | `"io.modelcontextprotocol/server-card"` | server card extension id |
| `ext::kAppsExtensionId` | `"io.modelcontextprotocol/ui"` | apps extension id |
| `ext::kAppsMimeType` | `"text/html;profile=mcp-app"` | ui resource MIME type |
| `ext::kAppsUriScheme` | `"ui://"` | ui resource uri prefix, used both by `ValidateUiMeta` and by the `resources/read` gate |
| `kTasksExtensionId` | `"io.modelcontextprotocol/tasks"` | tasks extension id (capability layer) |
| `kSkillsExtensionId` | `"io.modelcontextprotocol/skills"` | skills extension id (capability layer) |

The card and apps ids are in `include/mcp/ext/ServerCard.hpp` and `include/mcp/ext/Apps.hpp`; the tasks and skills ids in `include/mcp/Capabilities.hpp`.

::: info
Triggers have no extension id constant: the only outward identifier is the notification name `notifications/triggers/event` (`include/mcp/Methods.hpp`), and the server-side emission path additionally requires `enable_triggers_events` (`include/mcp/server/ServerOptions.hpp`).
:::

### Server card

| Symbol | Signature/type | Default | Notes |
|---|---|---|---|
| `ServerCardEndpoint` | `struct { std::string url; std::map<std::string,std::string> headers; std::map<std::string,std::string> variables; }` | empty string/tables | One endpoint; `headers` and `variables` are passed through as-is |
| `ServerCardRepository` | `struct { std::optional<std::string> url; }` | `nullopt` | Repository wrapper, yet `ServerCard::repository` is a plain string and does not use this struct |
| Required fields | `name`, `version` (`std::string`) | empty | Empty values fail validation |
| Optional fields | `description`, `title`, `website_url`, `repository`, `meta` (the first four `std::optional<std::string>`, `meta` a `std::optional<JsonValue>`) | all `nullopt` | Serialization writes `description` as an empty string when absent and omits the other optional fields |
| List fields | `icons` (`std::vector<Icon>`), `endpoints` (`std::vector<ServerCardEndpoint>`), `supported_protocol_versions` (`std::vector<std::string>`) | empty | An empty `endpoints` list fails validation |
| `ValidateCard` | `void ValidateCard(const ServerCard& card)` | — | Requires non-empty name/version/endpoints and an http/https url per endpoint; violations throw `InvalidParams` |
| `SerializeCard` | `JsonValue SerializeCard(const ServerCard& card)` | — | Calls `ValidateCard` first; emitted keys are `name`/`version`/`description`/`endpoints`/`supportedProtocolVersions` |

Declared in `include/mcp/ext/ServerCard.hpp`; validation and serialization are in `src/ext/ServerCard.cpp`.

### Apps

| Symbol | Signature/type | Default | Notes |
|---|---|---|---|
| `UiMeta` | `struct { std::string resource_uri; std::vector<std::string> csp; std::vector<std::string> permissions; }` | empty string/tables/tables | The ui declaration carried in `_meta` |
| `ValidateUiMeta` | `void ValidateUiMeta(const UiMeta& meta)` | — | `resource_uri` must be non-empty and start with `ui://`, otherwise `InvalidParams` is thrown; `csp`/`permissions` are not validated |

Declared in `include/mcp/ext/Apps.hpp`; validation is in `src/ext/Apps.cpp`.

### Triggers

| Symbol | Signature/type | Default | Notes |
|---|---|---|---|
| `TriggerRegistration` | `struct { std::vector<std::string> types; std::string webhook_url; }` | empty table/string | The subscribed types and callback address behind one id |
| `TriggerRegistry::Register` | `void Register(const std::string& id, std::vector<std::string> types, const std::string& webhook_url)` | — | Empty id or empty types throws `InvalidParams`; re-registering an id overwrites it |
| `Unregister` / `Has` / `Empty` / `Size` | `bool Unregister(const std::string&)` / `bool Has(const std::string&) const` / `bool Empty() const` / `std::size_t Size() const` | — | All locked; `Unregister` reports whether an entry was actually removed |
| `Match` | `std::vector<std::pair<std::string, TriggerRegistration>> Match(const std::string& type) const` | — | Returns every registration subscribed to the event type |
| `SerializeTriggerEvent` | `JsonValue SerializeTriggerEvent(const std::string& type, const JsonValue& data)` | — | Fixed two keys `{"type":…,"data":…}` |
| `PostTriggerWebhook` | `void PostTriggerWebhook(const std::string& webhook_url, const std::string& body, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))` | `5000ms` | POSTs JSON, up to 3 attempts with backoff doubling from 100ms; total failure logs a Warning only (URL and error text redacted) and never throws |

Declared in `include/mcp/ext/Triggers.hpp`; the registry and webhook delivery are in `src/ext/Triggers.cpp`.

## Skill verification

### Constants and functions

`include/mcp/client/SkillVerify.hpp`, implemented in `src/client/SkillVerify.cpp`.

| Symbol | Signature/type | Default | Notes |
|---|---|---|---|
| `kMaxSkillFiles` | `constexpr size_t` | `512` | Maximum file count in one skill manifest (exceeding it is an error, no truncation) |
| `kMaxSkillTotalBytes` | `constexpr int64_t` | `16 * 1024 * 1024` | Maximum accumulated bytes per skill; a negative `size` fails immediately |
| `CheckSkillLimits` | `bool CheckSkillLimits(const Skill& skill, std::string& error_out)` | — | Only applies to the manifest form (`vector<SkillResource>`); the string form (directory read) passes through |
| `VerifyFileBytes` | `bool VerifyFileBytes(const SkillResource& expected, std::string_view bytes, std::string& error_out)` | — | Byte counts must match exactly and `digest` must be a `sha256:` prefix (case-insensitive) plus the hex digest |
| `VerifyFrontmatter` | `bool VerifyFrontmatter(const Skill& advertised, const JsonValue& fetched_frontmatter, std::string& error_out)` | — | Advertised and fetched frontmatter must be deeply equal, must be an object with a string `name`, and that `name` must equal the skill-name segment derived from the uri |
| `IsApprovalStillValid` | `bool IsApprovalStillValid(const Skill& approved, const Skill& current)` | — | There is no time-based validity: the approval holds only while uri, frontmatter, meta and resources are all equal, and any change invalidates it |
| `ApprovalKey` | `std::string ApprovalKey(const std::string& host_label, const Skill& skill)` | — | `host_label` + `0x1F` + `skill.uri`, intended as the primary key of the caller's approval table |

### The uri convention shared with server-side `RegisterSkill`

| Convention | Fact |
|---|---|
| Entry uri shape | Must look like `<scheme>://<authority>/<path segment>/SKILL.md`: ends with `/SKILL.md`, contains `://`, has a non-empty authority and at least one path segment after it |
| Failure mode | `RegisterSkill` throws `McpError(InvalidParams, "invalid skill uri: …")` for non-conforming uris; registering the same uri twice also throws |
| Expected frontmatter `name` | Client-side verification takes the uri, strips the trailing `/SKILL.md`, strips `scheme://` and the authority, and uses the last remaining path segment as the skill name, which must equal `frontmatter.name` |
| Consequence | Registration name and verification name derive from the same uri shape: one path segment more or less makes an approved skill fail the name check; the registration API and directory-read switch are in the [Server reference](/en/reference/server) |

The shape check is `IsSkillEntryUri` in `src/server/McpServer.cpp`; the name comparison is in `src/client/SkillVerify.cpp`.

## Protocol constant index

The era column is a symbol index only; [Protocol versions](/en/learn/protocol-versions) remains the single authority for the predicates and extra gates. Every method and notification constant is declared in `include/mcp/Methods.hpp`.

### Request method names in the `methods` namespace

| Constant | Wire value | Era |
|---|---|---|
| `kInitialize` | `"initialize"` | legacy only |
| `kPing` | `"ping"` | legacy only |
| `kListTools` / `kCallTool` | `"tools/list"` / `"tools/call"` | both eras |
| `kListResources` / `kReadResource` | `"resources/list"` / `"resources/read"` | both eras |
| `kReadDirectory` | `"resources/directory/read"` | modern only |
| `kListResourceTemplates` | `"resources/templates/list"` | both eras |
| `kSubscribeResource` / `kUnsubscribeResource` | `"resources/subscribe"` / `"resources/unsubscribe"` | legacy only |
| `kListPrompts` / `kGetPrompt` | `"prompts/list"` / `"prompts/get"` | both eras |
| `kComplete` | `"completion/complete"` | both eras |
| `kListRoots` | `"roots/list"` | legacy only |
| `kCreateMessage` | `"sampling/createMessage"` | legacy only |
| `kElicit` | `"elicitation/create"` | legacy only |
| `kDiscover` | `"server/discover"` | modern only |
| `kSubscribe` | `"subscriptions/listen"` | modern only |
| `kGetTask` / `kUpdateTask` / `kCancelTask` | `"tasks/get"` / `"tasks/update"` / `"tasks/cancel"` | both eras (task switch required) |
| `kGetTaskPayload` / `kLegacyGetTaskPayload` | both `"tasks/result"` | legacy only; `kLegacy*` is a same-value alias |
| `kListTasks` / `kLegacyListTasks` | both `"tasks/list"` | legacy only; as above |
| `kListSkills` / `kGetSkill` | `"skills/list"` / `"skills/get"` | modern only |
| `kSetLoggingLevel` | `"logging/setLevel"` | legacy only |
| `kListExtensions` | `"server/extensions/list"` | both eras |
| `kExtensionMethodPrefix` | `"ext/"` | not a method: extension method-name prefix |

### Notification names in the `notifications` namespace

| Constant | Wire value | Era |
|---|---|---|
| `kInitialized` | `"notifications/initialized"` | legacy only |
| `kCancelled` | `"notifications/cancelled"` | both eras |
| `kResourceUpdated` / `kResourceListChanged` | `"notifications/resources/updated"` / `"…/list_changed"` | both eras |
| `kToolListChanged` / `kPromptListChanged` | `"notifications/tools/list_changed"` / `"notifications/prompts/list_changed"` | both eras |
| `kMessage` / `kProgress` | `"notifications/message"` / `"notifications/progress"` | both eras |
| `kRootsListChanged` | `"notifications/roots/list_changed"` | legacy only; the client additionally requires a negotiated version of at least `2025-06-18` |
| `kElicitationComplete` | `"notifications/elicitation/complete"` | legacy only |
| `kSubscriptionsAcknowledged` | `"notifications/subscriptions/acknowledged"` | modern only |
| `kTaskStatus` / `kTaskWorking` / `kTaskCompleted` / `kTaskFailed` / `kTaskCancelled` / `kTaskInputRequired` | `"notifications/tasks/status"`, `"…/working"`, `"…/completed"`, `"…/failed"`, `"…/cancelled"`, `"…/input_required"` | both eras |
| `kTriggersEvent` | `"notifications/triggers/event"` | both eras |

### Protocol version constants

| Constant | Value | Notes |
|---|---|---|
| `kLatestProtocolVersion` | `"2026-07-28"` | Newest entry of the support table |
| `kLegacyProtocolVersion` | `"2025-11-25"` | Legacy-era representative, the fallback target of `initialize` negotiation |
| `kDefaultNegotiatedProtocolVersion` | `"2025-03-26"` | Default negotiated version when the peer declares none |
| `kModernProtocolVersion` | `"2026-07-28"` | Modern-era baseline value |
| `kJsonRpcVersion` | `"2.0"` | Envelope version |
| `kProtocolVersions` | `{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"}` | The five supported versions; lexicographic order equals chronological order |
| `IsModernProtocolVersion` | `constexpr bool IsModernProtocolVersion(std::string_view version) noexcept` | Lexicographic `>= kLatestProtocolVersion` (the predicate is not `kModernProtocolVersion`; both currently hold the same value) |

All declared in `include/mcp/ProtocolVersion.hpp`.

Task-shaped guides: [/en/howto/add-oauth-to-a-client](/en/howto/add-oauth-to-a-client), [/en/howto/protect-an-http-server](/en/howto/protect-an-http-server).

## Source files behind this page

- `include/mcp/client/auth/OAuthClientProvider.hpp`, `include/mcp/client/auth/TokenCache.hpp`, `include/mcp/storage/FileTokenCache.hpp`, `include/mcp/client/SkillVerify.hpp`
- `include/mcp/ext/DPoP.hpp`, `include/mcp/ext/ServerCard.hpp`, `include/mcp/ext/Apps.hpp`, `include/mcp/ext/Triggers.hpp`, `include/mcp/Capabilities.hpp`
- `include/mcp/Methods.hpp`, `include/mcp/ProtocolVersion.hpp`, `include/mcp/McpTypes.hpp`, `include/mcp/transport/StreamableHttpClientTransport.hpp`, `include/mcp/server/ServerOptions.hpp`
- `src/client/auth/OAuthClientProvider.cpp`, `src/client/FileTokenCache.cpp`, `src/client/SkillVerify.cpp`, `src/ext/DPoP.cpp`, `src/ext/ServerCard.cpp`, `src/ext/Apps.cpp`, `src/ext/Triggers.cpp`, `src/server/McpServer.cpp`
- `cmake/FetchDependencies.cmake`, `cmake/OpenSslVersion.cmake`, `CMakeLists.txt`
