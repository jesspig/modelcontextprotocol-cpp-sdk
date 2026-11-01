# Authorization and Extensions Reference

This page is the **authoritative field table** for the authorization layer (OAuth, PKCE, DPoP, token caches) and the extension layer (server card, apps, triggers, skill verification); every entry is checked against the headers. Facade signatures live in the [Server reference](/en/reference/server) and [Client reference](/en/reference/client), transport options in the [Transports reference](/en/reference/transports), extension switch semantics in [Extension switches](/en/learn/extensions), the single authoritative era table in [Protocol versions](/en/learn/protocol-versions), and `Skill`/`SkillResource` fields in the [Types reference](/en/reference/types). No procedural steps on this page. Types and functions under `detail/` are not user API: they are named only where they explain behaviour and never listed in interface tables. In the evidence column, `hpp` means the header named at the head of that subsection and `cpp` its matching implementation file.

## OAuth client provider

### `OAuthClientOptions` fields

`include/mcp/client/auth/OAuthClientProvider.hpp:28-47`, implemented in `src/client/auth/OAuthClientProvider.cpp`; `AuthorizationCodeResult` (`hpp:22-26`) holds `code`, `state` (`std::string`) and `iss` (`std::optional<std::string>`, `nullopt`).

| Field | Type | Default | Notes | Evidence |
|---|---|---|---|---|
| `server_url` | `std::string` | empty | MCP server URL, the base for metadata discovery and the resource indicator | `hpp:29`, `cpp:459-463` |
| `redirect_uri` | `std::string` | empty | Authorization callback address, used by registration, the authorization URL and code exchange | `hpp:30`, `cpp:780`, `cpp:802`, `cpp:841` |
| `client_id` | `std::optional<std::string>` | `nullopt` | When empty and the server advertises a registration endpoint, dynamic registration runs; when the value itself is an http(s) URL it is fetched as a client metadata document | `hpp:31`, `cpp:401-410`, `cpp:771-780` |
| `client_secret` | `std::optional<std::string>` | `nullopt` | Only required by the `client_credentials` branch when no private-key assertion is used | `hpp:32`, `cpp:446-450` |
| `scopes` | `std::vector<std::string>` | empty | Requested scopes; `StepUpAuthorization` appends in place | `hpp:33`, `cpp:1067-1077` |
| `scope_filter` | `std::function<std::vector<std::string>(const std::vector<std::string>&)>` | unset | Called with `scopes` as the input on every scope resolution; unset means use them verbatim | `hpp:34`, `cpp:642-645` |
| `token_cache` | `std::shared_ptr<ITokenCache>` | `nullopt` | Construction substitutes an `InMemoryTokenCache` when empty | `hpp:35`, `cpp:389-394` |
| `resource` | `std::optional<std::string>` | `nullopt` | RFC 8707 resource indicator; falls back to the normalized `server_url` when unset | `hpp:36`, `cpp:634-641` |
| `application_type` | `std::string` | `"native"` | Dynamic-registration body field; an empty string is sent as `native` | `hpp:37`, `cpp:775-780`, `cpp:1259-1260` |
| `enable_dpop` | `bool` | `false` | When on, token-endpoint requests carry a `DPoP` header and `Authorization` switches to the `DPoP` scheme | `hpp:38`, `cpp:366-368`, `cpp:1022` |
| `private_key_pem` | `std::optional<std::string>` | `nullopt` | Signing key for `private_key_jwt` assertions; only effective when the server advertises that auth method | `hpp:39`, `cpp:711-723` |
| `signing_algorithm` | `std::string` | `"ES256"` | Assertion algorithm name; anything other than `ES256` throws `InvalidParams` while building the assertion | `hpp:40`, `cpp:726-728` |
| `client_assertion_clock_seconds` | `std::function<int64_t()>` | unset | Clock source for assertion `iat`/`exp`; unset uses `dpop::NowSeconds` | `hpp:41`, `cpp:749-751` |
| `allowed_authorization_server_origins` | `std::vector<std::string>` | empty | Authorization-server origin allow-list, checked against the issuer and each endpoint; empty means no narrowing | `hpp:42`, `cpp:555-584` |
| `authorization_redirect_handler` | `std::function<void(std::string_view url)>` | unset | Invoked with the full authorization URL so the caller can open a browser; does not influence the flow outcome | `hpp:44`, `cpp:818-819` |
| `authorization_code_callback` | `std::function<std::optional<AuthorizationCodeResult>()>` | unset | Mandatory, otherwise the authorization flow returns `false` immediately; an empty result, a `state` mismatch or an `iss` mismatch all fail | `hpp:45`, `cpp:820-829` |
| `token_update_handler` | `std::function<void(const TokenContainer&)>` | unset | Invoked every time tokens land in the cache | `hpp:46`, `cpp:625-632` |

### `OAuthClientProvider` public methods

Same header, `hpp:49-86`; private nested types and members are not user API and are omitted.

| Method | Signature | Return semantics | Notes | Evidence |
|---|---|---|---|---|
| Construct/destruct | `explicit OAuthClientProvider(OAuthClientOptions options)` / `~OAuthClientProvider()` | — | Options are taken by value; the token cache is self-created by default | `hpp:51-52`, `cpp:389-396` |
| `IsSecureEndpoint` | `static bool IsSecureEndpoint(std::string_view url)` | `bool` | https check, loopback hosts excepted | `hpp:54`, `cpp:586-588` |
| `IsSameOrigin` | `static bool IsSameOrigin(std::string_view lhs, std::string_view rhs)` | `bool` | Origin (scheme+host+port) comparison; `false` if either side yields no origin | `hpp:56`, `cpp:590-594` |
| `Authenticate` | `bool Authenticate()` | whether a usable token exists | Discover metadata → register / fetch document as needed → fresh cache returns true → refresh expired tokens, with `invalid_grant`/`invalid_client` falling back to the full authorization flow; cached tokens with a mismatched resource are cleared first | `hpp:58`, `cpp:398-424` |
| `AuthenticateClientCredentials` | `bool AuthenticateClientCredentials()` | whether tokens were stored | Requires `client_id`, plus `client_secret` when no assertion is used; never runs the code flow | `hpp:60`, `cpp:426-457` |
| `GetAccessToken` | `std::string GetAccessToken()` | token string, empty when none | When nearing expiry with a refresh token, joins coalesced refreshing (concurrent callers share one refresh); refresh failure throws `InternalError` | `hpp:62`, `cpp:987-996`, `cpp:960-985` |
| `RefreshTokens` | `bool RefreshTokens()` | whether the refresh succeeded | Refreshes directly, no coalescing; both `invalid_grant` and transient failure return `false` | `hpp:64`, `cpp:886-888` |
| `Revoke` | `void Revoke()` | void | POSTs only when a revocation endpoint exists (non-https throws `InvalidParams`); clears the cache regardless | `hpp:66`, `cpp:998-1009`, `cpp:533-542` |
| `IsAuthenticated` / `HasToken` | `bool IsAuthenticated() const` / `bool HasToken() const` | `bool` | The former requires an unexpired token, the latter only a non-empty access token; both check resource matching | `hpp:68-69`, `cpp:1010-1016` |
| `GetAuthorizationHeader` | `std::string GetAuthorizationHeader() const` | `DPoP <token>` or `Bearer <token>`, empty when none | Read-only, never triggers a refresh; the scheme name follows `enable_dpop` | `hpp:71`, `cpp:1018-1024` |
| `BuildDPoPProof` | `std::string BuildDPoPProof(std::string_view htm, std::string_view htu, std::optional<std::string_view> access_token = std::nullopt)` | proof JWT; empty when `enable_dpop` is false | Lazily generates the P-256 key on first use and attaches the recorded nonce | `hpp:73-75`, `cpp:1026-1036` |
| `ApplyDPoPProofHeader` | `void ApplyDPoPProofHeader(const HttpRequestHeaderContext& context, std::map<std::string, std::string>& headers)` | void | Builds the proof from the context method and URL into `headers["DPoP"]`; leaves headers untouched when dpop is off, the method is missing or the URL is unparseable | `hpp:77-78`, `cpp:1043-1058` |
| `ObserveDPoPNonce` / `SetDPoPNonce` | `void ObserveDPoPNonce(std::string_view dpop_nonce)` / `void SetDPoPNonce(std::string nonce)` | void | The former ignores empty strings and anything over 1024 bytes; the latter overwrites unconditionally, ignoring the dpop switch | `hpp:80-82`, `cpp:1061-1065`, `cpp:1038-1041` |
| `StepUpAuthorization` | `bool StepUpAuthorization(const std::vector<std::string>& additional_scopes)` | whether re-authorization succeeded | Append scopes → `Revoke()` → rerun the authorization flow, which drops the current tokens | `hpp:84`, `cpp:1067-1081` |
| `HandleAuthChallenge` | `bool HandleAuthChallenge(std::string_view www_authenticate)` | whether metadata was rebuilt from it | Recognizes only `resource_metadata="…"`; headers over 4096 bytes, non-https URLs, documents over 65536 bytes, resource mismatch or an authorization server outside the allow-list all return `false` | `hpp:86`, `cpp:1083-1160` |

### PKCE helpers and the OpenSSL dependency boundary

The `pkce` namespace in `hpp:180-188`:

| Symbol | Signature | Notes | Evidence |
|---|---|---|---|
| `GenerateCodeVerifier` | `std::string GenerateCodeVerifier()` | 32 random bytes → base64url; the flow also uses it for `state` | `hpp:182`, `cpp:289-309`, `cpp:793-795` |
| `ComputeCodeChallenge` | `std::string ComputeCodeChallenge(std::string_view code_verifier)` | `S256`: SHA-256 then base64url | `hpp:184`, `cpp:311-314` |
| `Base64UrlEncode` | `std::string Base64UrlEncode(std::string_view input)` | Unpadded base64url | `hpp:186`, `cpp:285-287` |

| Capability | With OpenSSL | Without OpenSSL | Evidence |
|---|---|---|---|
| PKCE random source | `RAND_bytes`, throws `std::runtime_error` on failure | `BCryptGenRandom` on Windows, otherwise `std::random_device` byte by byte | `src/client/auth/OAuthClientProvider.cpp:291-305` |
| PKCE digest | built-in SHA-256, never OpenSSL | identical, always the built-in implementation | `src/client/auth/OAuthClientProvider.cpp:311-314` |
| DPoP keys, proofs, verification | OpenSSL EC/ECDSA | throws `McpError` (see the DPoP subsection) | `src/ext/DPoP.cpp:192-537`, `538-573` |
| `private_key_jwt` assertion signing | OpenSSL EC | same path throws `McpError` | `src/client/auth/OAuthClientProvider.cpp:725-761`, `src/ext/DPoP.cpp:562-571` |
| DPoP nonce and assertion `jti` random source | `RAND_bytes` | `std::random_device` (no Windows branch) | `src/ext/DPoP.cpp:147-160` |
| TLS | enabled | TLS disabled | `cmake/FetchDependencies.cmake:15-40` |

Build side: `MCP_WITH_OPENSSL` takes `AUTO` (default, used only when found), `ON` (configure fails if not found) or `OFF` (forced off) (`cmake/FetchDependencies.cmake:7-16`, `27-38`); the `MCP_HAVE_OPENSSL` define and linking are applied to `mcp-transport`/`mcp-client`/`mcp-ext` by the single INTERFACE target `mcp-openssl` (`CMakeLists.txt:227-238`); only OpenSSL 3.x/4.x are supported and an unsupported major version fails configure rather than silently disabling TLS (`cmake/OpenSslVersion.cmake:7-11`).

### Wiring into the HTTP client transport

No other public header references the provider; the three hooks are wired by the caller through `HttpClientTransportOptions`. The in-repo example connects only two of them (`examples/conformance/client/main.cpp:1289-1297`) and `auth_challenge_handler` has no example wiring point.

| Transport option field | Corresponding provider method | Evidence |
|---|---|---|
| `auth_challenge_handler` | `HandleAuthChallenge` | `include/mcp/transport/StreamableHttpClientTransport.hpp:50` |
| `apply_per_request_headers` | `ApplyDPoPProofHeader` | `StreamableHttpClientTransport.hpp:51-53` |
| `observe_response_challenge` | `ObserveDPoPNonce` | `StreamableHttpClientTransport.hpp:54-55` |

## DPoP

### Public symbols

`include/mcp/ext/DPoP.hpp`, namespace `mcp::ext::dpop`; implemented in `src/ext/DPoP.cpp`.

| Symbol | Signature/type | Default | Notes | Evidence |
|---|---|---|---|---|
| `EcP256Jwk` | `struct { std::string x; std::string y; std::string d; }` | three empty strings | P-256 JWK where `d` is the private scalar; `CreateProof` requires all three | `hpp:12-16`, `cpp:385-395` |
| `GenerateKey` | `EcP256Jwk GenerateKey()` | — | Generates a P-256 key | `hpp:18`, `cpp:349-378` |
| `ComputeThumbprint` | `std::string ComputeThumbprint(std::string_view x, std::string_view y)` | — | SHA-256 over `{"crv":"P-256","kty":"EC","x":…,"y":…}` then base64url; throws `InvalidParams` when x or y is empty; independent of OpenSSL | `hpp:20`, `cpp:96-101` |
| `PublicJwkJson` | `std::string PublicJwkJson(const EcP256Jwk& key)` | — | Emits only `crv`/`kty`/`x`/`y`, never `d`; independent of OpenSSL | `hpp:22`, `cpp:103-110` |
| `CreateProof` | `std::string CreateProof(const EcP256Jwk& key, std::string_view htm, std::string_view htu, std::optional<std::string_view> access_token = std::nullopt, std::optional<std::string_view> nonce = std::nullopt, std::optional<int64_t> iat_override_seconds = std::nullopt)` | three `nullopt` | Produces the DPoP JWT: header `typ=dpop+jwt`, `alg=ES256`, embedded `jwk`; payload `htm`/`htu`/`jti`/`iat`, plus `ath` when an access token is given | `hpp:24-29`, `cpp:380-428` |
| `VerifyProof` | `bool VerifyProof(std::string_view proof, const DPoPVerifyOptions& options, JtiReplayCache& replay)` | — | Checks `typ`/`alg`, `htm`/`htu` (with `alternate_htu` allowed), `ath`, nonce, the time window, the ECDSA signature and jti replay; returns `false` on mismatch but throws when OpenSSL is absent | `hpp:56-58`, `cpp:430-537` |
| `IsNonceFresh` / `NowSeconds` | `bool IsNonceFresh(int64_t iat_seconds, int64_t now_seconds, int64_t window_seconds = 300)` / `int64_t NowSeconds()` | `300` | `false` when `iat` is later than now or the window is negative; the time source is `system_clock` seconds; neither needs OpenSSL | `hpp:31-33`, `cpp:112-122` |
| `GenerateNonce` | `std::string GenerateNonce(std::size_t random_bytes = 16)` | `16` (0 becomes 16, over 64 clamps to 64) | base64url random string | `hpp:60`, `cpp:147-160` |

### `DPoPVerifyOptions` fields and `JtiReplayCache`

| Symbol | Signature/type | Default | Notes | Evidence |
|---|---|---|---|---|
| `htm` / `htu` / `access_token` | `std::string` ×3 | empty strings | Expected method and URL, plus the access token used to compare `ath` | `hpp:36-38` |
| `alternate_htu` | `std::optional<std::string>` | `nullopt` | Lets one proof match a second URL | `hpp:39` |
| `expected_nonce` | `std::optional<std::string>` | `nullopt` | When non-empty the proof must carry the same nonce | `hpp:40` |
| `window_seconds` | `int64_t` | `300` | `iat` freshness window | `hpp:41` |
| `now_seconds` | `int64_t` | `0` | `0` means call `NowSeconds()` | `hpp:42` |
| Constructor | `explicit JtiReplayCache(int64_t window_seconds = 300)` | `300` (negative clamps to 0) | Each lookup also prunes entries beyond the window | `hpp:47`, `cpp:124-125` |
| `CheckAndStore` / `Clear` | `bool CheckAndStore(std::string_view jti, int64_t iat_seconds, int64_t now_seconds)` / `void Clear()` | — | Empty or already-seen `jti` returns `false`, otherwise the entry is recorded and `true` is returned; `Clear` empties the table; both are locked | `hpp:48-49`, `cpp:127-145` |

### Behaviour without OpenSSL

::: danger
With OpenSSL disabled, `enable_dpop` and `private_key_pem` are **not degraded but unavailable**: `GenerateKey`, `CreateProof` and `VerifyProof` throw `McpError(InternalError, "DPoP requires OpenSSL EC support, which is not available in this build")`, and client-assertion signing uses the same EC routine family. There is no way to complete DPoP on built-in code alone.
:::

| Group | Behaviour | Evidence |
|---|---|---|
| `GenerateKey`, `CreateProof`, `VerifyProof` | Throw `McpError`, no fallback path | `src/ext/DPoP.cpp:540-559` |
| `ComputeThumbprint`, `PublicJwkJson`, `IsNonceFresh`, `NowSeconds`, `JtiReplayCache`, `GenerateNonce` | Defined outside the EC section and remain callable; digests use the built-in SHA-256 and the random source falls back | `src/ext/DPoP.cpp:96-160` |

## Token caches

### `TokenContainer` and `ITokenCache`

`include/mcp/client/auth/TokenCache.hpp` (the authorization cache contract ships with client/auth), implemented in `src/client/auth/OAuthClientProvider.cpp`.

| Symbol | Signature/type | Default | Notes | Evidence |
|---|---|---|---|---|
| `access_token` / `refresh_token` / `scopes` | `std::string` / `std::string` / `std::vector<std::string>` | empty/empty/empty | An empty refresh token makes refreshing impossible | `hpp:12-14` |
| `expires_at` plus checks | `int64_t`; `bool IsExpired() const` / `bool WillExpireSoon(int64_t margin_ms = 60000) const` | `0`; `60000` | Absolute expiry timestamp in milliseconds; expiry is considered near 60 seconds early by default | `hpp:15`, `hpp:19-20`, `cpp:318-329` |
| `token_type` / `resource` | `std::string` / `std::optional<std::string>` | `"Bearer"` / `nullopt` | The provider matches `resource` against the request; a mismatch counts as having no token | `hpp:16-17`, `cpp:509-513` |
| `StoreTokens` / `ClearTokens` | `virtual void StoreTokens(const TokenContainer&) = 0` / `virtual void ClearTokens() = 0` | — | Persist tokens / drop them | `hpp:26`, `hpp:28` |
| `GetTokens` | `virtual std::optional<TokenContainer> GetTokens() = 0` | — | Non-const member | `hpp:27` |
| `StoreBoundIssuer` | `virtual void StoreBoundIssuer(const std::string& issuer)` | empty body (overridable) | Issuer-bound persistence anchor; the provider writes the issuer it first discovers and thereafter rejects any issuer change | `hpp:30`, `cpp:515-531` |
| `GetBoundIssuer` | `virtual std::optional<std::string> GetBoundIssuer()` | `nullopt` (overridable) | Reads the bound issuer back; the default means no persistence | `hpp:31` |
| `OAuthMetadata` | `struct`: issuer, authorization/token/revocation/registration/jwks/resource endpoints and four `*_supported` lists; `static std::optional<OAuthMetadata> Discover(std::string_view server_url)` | empty strings/lists | The last four endpoints and `resource` are `optional`; `Discover` fetches `/.well-known/oauth-authorization-server` and returns empty on failure | `hpp:47-61`, `cpp:1226` |
| `OAuthTokenResponse` | `struct`: `access_token`/`refresh_token`/`expires_in`/`token_type`/`scope` | `expires_in = 3600`, `token_type = "Bearer"` | Token-endpoint payload carrier | `hpp:64-70` |
| `ClientRegistrationInfo::Register` | `static std::optional<ClientRegistrationInfo> Register(std::string_view registration_endpoint, std::string_view redirect_uri, std::string_view client_name, std::string_view application_type = "native")` | `"native"` | Dynamic client registration entry point | `hpp:79-83` |

### Built-in implementations

| Symbol | Signature/type | Notes | Evidence |
|---|---|---|---|
| `InMemoryTokenCache` | `class : public ITokenCache` | In-process implementation, overrides all five interfaces including issuer binding, internally locked | `TokenCache.hpp:34-45` |
| `FileTokenCache` construct/destruct | `explicit FileTokenCache(std::filesystem::path cache_path)` / `~FileTokenCache() override` | The constructor loads from disk (a missing file means an empty cache); destructor persistence failures are logged as Error only, never thrown | `include/mcp/storage/FileTokenCache.hpp:13-14`, `src/client/FileTokenCache.cpp:78-90` |
| `StoreTokens` / `GetTokens` / `ClearTokens` | `override` | Every write is flushed synchronously; with no tokens the flush deletes the file | `FileTokenCache.hpp:16-18`, `cpp:92-107`, `cpp:236-241` |
| `StoreBoundIssuer` / `GetBoundIssuer` | `override` | The anchor lives in `<cache_path>.issuer`; an empty string removes that file, write failures are logged as Error | `FileTokenCache.hpp:19-20`, `cpp:109-136`, `cpp:47-51` |
| Permission semantics (POSIX) | Before loading, `stat` requires the current uid as owner and all group/other bits clear; otherwise the load is refused with an Error log | Refusal equals an empty cache and throws nothing | `cpp:53-72`, `cpp:173` |
| Permission semantics (Windows) | The whole cache is protected per user through DPAPI (`CryptProtectData`/`CryptUnprotectData`); decryption failure discards the cache | Never plaintext JSON on disk; writes go through the internal owner-only atomic secure writer | `cpp:19-45`, `cpp:145-152`, `cpp:221-232` |

## Extension types

### Extension and identifier constants

| Constant | Value | Meaning | Evidence |
|---|---|---|---|
| `ext::kServerCardExtensionId` | `"io.modelcontextprotocol/server-card"` | server card extension id | `include/mcp/ext/ServerCard.hpp:14` |
| `ext::kAppsExtensionId` | `"io.modelcontextprotocol/ui"` | apps extension id | `include/mcp/ext/Apps.hpp:11` |
| `ext::kAppsMimeType` | `"text/html;profile=mcp-app"` | ui resource MIME type | `Apps.hpp:12` |
| `ext::kAppsUriScheme` | `"ui://"` | ui resource uri prefix, used both by `ValidateUiMeta` and by the `resources/read` gate | `Apps.hpp:13` |
| `kTasksExtensionId` | `"io.modelcontextprotocol/tasks"` | tasks extension id (capability layer) | `include/mcp/Capabilities.hpp:43` |
| `kSkillsExtensionId` | `"io.modelcontextprotocol/skills"` | skills extension id (capability layer) | `Capabilities.hpp:44` |

::: info
Triggers have no extension id constant: the only outward identifier is the notification name `notifications/triggers/event` (`include/mcp/Methods.hpp:76`), and the server-side emission path additionally requires `enable_triggers_events` (`include/mcp/server/ServerOptions.hpp:88`).
:::

### Server card

| Symbol | Signature/type | Default | Notes | Evidence |
|---|---|---|---|---|
| `ServerCardEndpoint` | `struct { std::string url; std::map<std::string,std::string> headers; std::map<std::string,std::string> variables; }` | empty string/tables | One endpoint; `headers` and `variables` are passed through as-is | `ServerCard.hpp:16-20` |
| `ServerCardRepository` | `struct { std::optional<std::string> url; }` | `nullopt` | Repository wrapper, yet `ServerCard::repository` is a plain string and does not use this struct | `ServerCard.hpp:22-24`, `ServerCard.hpp:32` |
| Required fields | `name`, `version` (`std::string`) | empty | Empty values fail validation | `ServerCard.hpp:27-28`, `src/ext/ServerCard.cpp:17-21` |
| Optional fields | `description`, `title`, `website_url`, `repository`, `meta` (the first four `std::optional<std::string>`, `meta` a `std::optional<JsonValue>`) | all `nullopt` | Serialization writes `description` as an empty string when absent and omits the other optional fields | `ServerCard.hpp:29-36`, `src/ext/ServerCard.cpp:37` |
| List fields | `icons` (`std::vector<Icon>`), `endpoints` (`std::vector<ServerCardEndpoint>`), `supported_protocol_versions` (`std::vector<std::string>`) | empty | An empty `endpoints` list fails validation | `ServerCard.hpp:31-35`, `src/ext/ServerCard.cpp:22-28` |
| `ValidateCard` | `void ValidateCard(const ServerCard& card)` | — | Requires non-empty name/version/endpoints and an http/https url per endpoint; violations throw `InvalidParams` | `ServerCard.hpp:39`, `src/ext/ServerCard.cpp:15-30` |
| `SerializeCard` | `JsonValue SerializeCard(const ServerCard& card)` | — | Calls `ValidateCard` first; emitted keys are `name`/`version`/`description`/`endpoints`/`supportedProtocolVersions` | `ServerCard.hpp:41`, `src/ext/ServerCard.cpp:32-65` |

### Apps

| Symbol | Signature/type | Default | Notes | Evidence |
|---|---|---|---|---|
| `UiMeta` | `struct { std::string resource_uri; std::vector<std::string> csp; std::vector<std::string> permissions; }` | empty string/tables/tables | The ui declaration carried in `_meta` | `Apps.hpp:15-19` |
| `ValidateUiMeta` | `void ValidateUiMeta(const UiMeta& meta)` | — | `resource_uri` must be non-empty and start with `ui://`, otherwise `InvalidParams` is thrown; `csp`/`permissions` are not validated | `Apps.hpp:21`, `src/ext/Apps.cpp:7-15` |

### Triggers

| Symbol | Signature/type | Default | Notes | Evidence |
|---|---|---|---|---|
| `TriggerRegistration` | `struct { std::vector<std::string> types; std::string webhook_url; }` | empty table/string | The subscribed types and callback address behind one id | `include/mcp/ext/Triggers.hpp:15-18` |
| `TriggerRegistry::Register` | `void Register(const std::string& id, std::vector<std::string> types, const std::string& webhook_url)` | — | Empty id or empty types throws `InvalidParams`; re-registering an id overwrites it | `Triggers.hpp:22-24`, `src/ext/Triggers.cpp:47-61` |
| `Unregister` / `Has` / `Empty` / `Size` | `bool Unregister(const std::string&)` / `bool Has(const std::string&) const` / `bool Empty() const` / `std::size_t Size() const` | — | All locked; `Unregister` reports whether an entry was actually removed | `Triggers.hpp:25-28`, `src/ext/Triggers.cpp:63-81` |
| `Match` | `std::vector<std::pair<std::string, TriggerRegistration>> Match(const std::string& type) const` | — | Returns every registration subscribed to the event type | `Triggers.hpp:29-30`, `src/ext/Triggers.cpp:83-91` |
| `SerializeTriggerEvent` | `JsonValue SerializeTriggerEvent(const std::string& type, const JsonValue& data)` | — | Fixed two keys `{"type":…,"data":…}` | `Triggers.hpp:36`, `src/ext/Triggers.cpp:98-103` |
| `PostTriggerWebhook` | `void PostTriggerWebhook(const std::string& webhook_url, const std::string& body, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))` | `5000ms` | POSTs JSON, up to 3 attempts with backoff doubling from 100ms; total failure logs a Warning only (URL and error text redacted) and never throws | `Triggers.hpp:38-41`, `src/ext/Triggers.cpp:105-133` |

## Skill verification

### Constants and functions

`include/mcp/client/SkillVerify.hpp`, implemented in `src/client/SkillVerify.cpp`.

| Symbol | Signature/type | Default | Notes | Evidence |
|---|---|---|---|---|
| `kMaxSkillFiles` | `constexpr size_t` | `512` | Maximum file count in one skill manifest (exceeding it is an error, no truncation) | `hpp:12`, `cpp:60-63` |
| `kMaxSkillTotalBytes` | `constexpr int64_t` | `16 * 1024 * 1024` | Maximum accumulated bytes per skill; a negative `size` fails immediately | `hpp:13`, `cpp:65-77` |
| `CheckSkillLimits` | `bool CheckSkillLimits(const Skill& skill, std::string& error_out)` | — | Only applies to the manifest form (`vector<SkillResource>`); the string form (directory read) passes through | `hpp:15`, `cpp:57-78` |
| `VerifyFileBytes` | `bool VerifyFileBytes(const SkillResource& expected, std::string_view bytes, std::string& error_out)` | — | Byte counts must match exactly and `digest` must be a `sha256:` prefix (case-insensitive) plus the hex digest | `hpp:16`, `cpp:79-96` |
| `VerifyFrontmatter` | `bool VerifyFrontmatter(const Skill& advertised, const JsonValue& fetched_frontmatter, std::string& error_out)` | — | Advertised and fetched frontmatter must be deeply equal, must be an object with a string `name`, and that `name` must equal the skill-name segment derived from the uri | `hpp:17`, `cpp:98-117` |
| `IsApprovalStillValid` | `bool IsApprovalStillValid(const Skill& approved, const Skill& current)` | — | There is no time-based validity: the approval holds only while uri, frontmatter, meta and resources are all equal, and any change invalidates it | `hpp:18`, `cpp:119-124` |
| `ApprovalKey` | `std::string ApprovalKey(const std::string& host_label, const Skill& skill)` | — | `host_label` + `0x1F` + `skill.uri`, intended as the primary key of the caller's approval table | `hpp:19`, `cpp:126-130` |

### The uri convention shared with server-side `RegisterSkill`

| Convention | Fact | Evidence |
|---|---|---|
| Entry uri shape | Must look like `<scheme>://<authority>/<path segment>/SKILL.md`: ends with `/SKILL.md`, contains `://`, has a non-empty authority and at least one path segment after it | `src/server/McpServer.cpp:125-140`, `908-913` |
| Failure mode | `RegisterSkill` throws `McpError(InvalidParams, "invalid skill uri: …")` for non-conforming uris; registering the same uri twice also throws | `McpServer.cpp:908-925` |
| Expected frontmatter `name` | Client-side verification takes the uri, strips the trailing `/SKILL.md`, strips `scheme://` and the authority, and uses the last remaining path segment as the skill name, which must equal `frontmatter.name` | `src/client/SkillVerify.cpp:28-38`, `98-116` |
| Consequence | Registration name and verification name derive from the same uri shape: one path segment more or less makes an approved skill fail the name check; the registration API and directory-read switch are in the [Server reference](/en/reference/server) | `McpServer.cpp:129-140`, `SkillVerify.cpp:28-38` |

## Protocol constant index

The era column is a symbol index only; [Protocol versions](/en/learn/protocol-versions) remains the single authority for the predicates and extra gates.

### Request method names in the `methods` namespace

| Constant | Wire value | Era | Evidence |
|---|---|---|---|
| `kInitialize` | `"initialize"` | legacy only | `include/mcp/Methods.hpp:8` |
| `kPing` | `"ping"` | legacy only | `Methods.hpp:9` |
| `kListTools` / `kCallTool` | `"tools/list"` / `"tools/call"` | both eras | `Methods.hpp:11-12` |
| `kListResources` / `kReadResource` | `"resources/list"` / `"resources/read"` | both eras | `Methods.hpp:14-15` |
| `kReadDirectory` | `"resources/directory/read"` | modern only | `Methods.hpp:16` |
| `kListResourceTemplates` | `"resources/templates/list"` | both eras | `Methods.hpp:17` |
| `kSubscribeResource` / `kUnsubscribeResource` | `"resources/subscribe"` / `"resources/unsubscribe"` | legacy only | `Methods.hpp:18-19` |
| `kListPrompts` / `kGetPrompt` | `"prompts/list"` / `"prompts/get"` | both eras | `Methods.hpp:21-22` |
| `kComplete` | `"completion/complete"` | both eras | `Methods.hpp:24` |
| `kListRoots` | `"roots/list"` | legacy only | `Methods.hpp:26` |
| `kCreateMessage` | `"sampling/createMessage"` | legacy only | `Methods.hpp:28` |
| `kElicit` | `"elicitation/create"` | legacy only | `Methods.hpp:30` |
| `kDiscover` | `"server/discover"` | modern only | `Methods.hpp:32` |
| `kSubscribe` | `"subscriptions/listen"` | modern only | `Methods.hpp:34` |
| `kGetTask` / `kUpdateTask` / `kCancelTask` | `"tasks/get"` / `"tasks/update"` / `"tasks/cancel"` | both eras (task switch required) | `Methods.hpp:36-38` |
| `kGetTaskPayload` / `kLegacyGetTaskPayload` | both `"tasks/result"` | legacy only; `kLegacy*` is a same-value alias | `Methods.hpp:40`, `43` |
| `kListTasks` / `kLegacyListTasks` | both `"tasks/list"` | legacy only; as above | `Methods.hpp:41`, `44` |
| `kListSkills` / `kGetSkill` | `"skills/list"` / `"skills/get"` | modern only | `Methods.hpp:46-47` |
| `kSetLoggingLevel` | `"logging/setLevel"` | legacy only | `Methods.hpp:49` |
| `kListExtensions` | `"server/extensions/list"` | both eras | `Methods.hpp:51` |
| `kExtensionMethodPrefix` | `"ext/"` | not a method: extension method-name prefix | `Methods.hpp:52` |

### Notification names in the `notifications` namespace

| Constant | Wire value | Era | Evidence |
|---|---|---|---|
| `kInitialized` | `"notifications/initialized"` | legacy only | `Methods.hpp:58` |
| `kCancelled` | `"notifications/cancelled"` | both eras | `Methods.hpp:59` |
| `kResourceUpdated` / `kResourceListChanged` | `"notifications/resources/updated"` / `"…/list_changed"` | both eras | `Methods.hpp:60-61` |
| `kToolListChanged` / `kPromptListChanged` | `"notifications/tools/list_changed"` / `"notifications/prompts/list_changed"` | both eras | `Methods.hpp:62-63` |
| `kMessage` / `kProgress` | `"notifications/message"` / `"notifications/progress"` | both eras | `Methods.hpp:64`, `66` |
| `kRootsListChanged` | `"notifications/roots/list_changed"` | legacy only; the client additionally requires a negotiated version of at least `2025-06-18` | `Methods.hpp:67` |
| `kElicitationComplete` | `"notifications/elicitation/complete"` | legacy only | `Methods.hpp:68` |
| `kSubscriptionsAcknowledged` | `"notifications/subscriptions/acknowledged"` | modern only | `Methods.hpp:69` |
| `kTaskStatus` / `kTaskWorking` / `kTaskCompleted` / `kTaskFailed` / `kTaskCancelled` / `kTaskInputRequired` | `"notifications/tasks/status"`, `"…/working"`, `"…/completed"`, `"…/failed"`, `"…/cancelled"`, `"…/input_required"` | both eras | `Methods.hpp:70-75` |
| `kTriggersEvent` | `"notifications/triggers/event"` | both eras | `Methods.hpp:76` |

### Protocol version constants

| Constant | Value | Notes | Evidence |
|---|---|---|---|
| `kLatestProtocolVersion` | `"2026-07-28"` | Newest entry of the support table | `include/mcp/ProtocolVersion.hpp:7` |
| `kLegacyProtocolVersion` | `"2025-11-25"` | Legacy-era representative, the fallback target of `initialize` negotiation | `ProtocolVersion.hpp:8` |
| `kDefaultNegotiatedProtocolVersion` | `"2025-03-26"` | Default negotiated version when the peer declares none | `ProtocolVersion.hpp:9` |
| `kModernProtocolVersion` | `"2026-07-28"` | Modern-era baseline value | `ProtocolVersion.hpp:10` |
| `kJsonRpcVersion` | `"2.0"` | Envelope version | `ProtocolVersion.hpp:11` |
| `kProtocolVersions` | `{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"}` | The five supported versions; lexicographic order equals chronological order | `ProtocolVersion.hpp:13-19` |
| `IsModernProtocolVersion` | `constexpr bool IsModernProtocolVersion(std::string_view version) noexcept` | Lexicographic `>= kLatestProtocolVersion` (the predicate is not `kModernProtocolVersion`; both currently hold the same value) | `ProtocolVersion.hpp:21-23` |

Task-shaped guides: [/en/howto/add-oauth-to-a-client](/en/howto/add-oauth-to-a-client), [/en/howto/protect-an-http-server](/en/howto/protect-an-http-server).

## Source files behind this page

- `include/mcp/client/auth/OAuthClientProvider.hpp`, `include/mcp/client/auth/TokenCache.hpp`, `include/mcp/storage/FileTokenCache.hpp`, `include/mcp/client/SkillVerify.hpp`
- `include/mcp/ext/DPoP.hpp`, `include/mcp/ext/ServerCard.hpp`, `include/mcp/ext/Apps.hpp`, `include/mcp/ext/Triggers.hpp`, `include/mcp/Capabilities.hpp`
- `include/mcp/Methods.hpp`, `include/mcp/ProtocolVersion.hpp`, `include/mcp/McpTypes.hpp`, `include/mcp/transport/StreamableHttpClientTransport.hpp`, `include/mcp/server/ServerOptions.hpp`
- `src/client/auth/OAuthClientProvider.cpp`, `src/client/FileTokenCache.cpp`, `src/client/SkillVerify.cpp`, `src/ext/DPoP.cpp`, `src/ext/ServerCard.cpp`, `src/ext/Apps.cpp`, `src/ext/Triggers.cpp`, `src/server/McpServer.cpp`
- `cmake/FetchDependencies.cmake`, `cmake/OpenSslVersion.cmake`, `CMakeLists.txt`
