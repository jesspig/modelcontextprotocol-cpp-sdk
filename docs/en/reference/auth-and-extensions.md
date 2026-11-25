# Authorization and Extensions Reference

This page is the **authoritative field table** for the authorization layer (OAuth, PKCE, DPoP, token caches) and the extension layer (server card, apps, triggers, skill verification); every entry is checked against the headers. Facade signatures live in the [Server reference](/en/reference/server) and [Client reference](/en/reference/client), transport options in the [Transports reference](/en/reference/transports), extension switch semantics in [Extension switches](/en/learn/extensions), the single authoritative era table in [Protocol versions](/en/learn/protocol-versions), and `Skill`/`SkillResource` fields in the [Types reference](/en/reference/types). No procedural steps on this page. Types and functions under `detail/` are not user API: they are named only where they explain behaviour and never listed in interface tables. The 0.3.13 OAuth/extension tightening and itemized migration are in the [0.3.13 release notes](/en/migration/0.3.13).

## OAuth client provider

### `OAuthClientOptions` fields

Declared in `include/mcp/client/auth/OAuthClientProvider.hpp`, implemented in `src/client/auth/OAuthClientProvider.cpp`; `AuthorizationCodeResult` holds `code`, `state` (`std::string`) and `iss` (`std::optional<std::string>`, `nullopt`).

| Field | Type | Default | Notes |
|---|---|---|---|
| `server_url` | `std::string` | empty | MCP server URL, the base for metadata discovery and the resource indicator |
| `redirect_uri` | `std::string` | empty | Authorization callback address, used by registration, the authorization URL and code exchange |
| `client_id` | `std::optional<std::string>` | `nullopt` | When empty and the server advertises a registration endpoint, dynamic registration runs; a URL-shaped value is fetched as a client metadata document only when it is HTTPS with a non-root path and no fragment or userinfo, otherwise authentication entry points reject it |
| `client_secret` | `std::optional<std::string>` | `nullopt` | Only required by the `client_credentials` branch when no private-key assertion is used |
| `scopes` | `std::vector<std::string>` | empty | Requested scopes; `StepUpAuthorization` appends in place |
| `scope_filter` | `std::function<std::vector<std::string>(const std::vector<std::string>&)>` | unset | Called with `scopes` as the input on every scope resolution; unset means use them verbatim |
| `token_cache` | `std::shared_ptr<ITokenCache>` | `nullptr` | Construction substitutes an `InMemoryTokenCache` when empty |
| `resource` | `std::optional<std::string>` | `nullopt` | RFC 8707 resource indicator; falls back to `server_url` when unset and is compared byte-for-byte with metadata `resource` |
| `application_type` | `std::string` | `"native"` | Dynamic-registration body field; an empty string is sent as `native` |
| `enable_dpop` | `bool` | `false` | When on, token-endpoint requests carry a `DPoP` header and `Authorization` switches to the `DPoP` scheme |
| `private_key_pem` | `std::optional<std::string>` | `nullopt` | Signing key for `private_key_jwt` assertions; only effective when the server advertises that auth method |
| `signing_algorithm` | `std::string` | `"ES256"` | Assertion algorithm name; anything other than `ES256` throws `InvalidParams` while building the assertion |
| `client_assertion_clock_seconds` | `std::function<int64_t()>` | unset | Clock source for assertion `iat`/`exp`; unset uses `dpop::NowSeconds` |
| `allowed_authorization_server_origins` | `std::vector<std::string>` | empty | Authorization-server origin allow-list, checked against the issuer and each endpoint; empty means no narrowing |
| `require_iss_when_advertised` | `bool` | `true` | Takes effect when the server advertises `authorization_response_iss_parameter_supported`: by default an authorization response with missing, empty or mismatched `iss` fails before code exchange; explicitly setting it to `false` is compatibility-only |
| `require_pkce_metadata` | `bool` | `true` | By default a metadata document missing `code_challenge_methods_supported` or lacking `S256` rejects the authorization-code flow (the challenge is never dropped); explicitly setting it to `false` restores the old flow |
| `allow_legacy_metadata_fallback` | `bool` | `false` | By default failed PRM/AS discovery fails closed; explicitly setting it to `true` re-enables direct RFC 8414 fallback and hard-coded `server_url` endpoints |
| `authorization_redirect_handler` | `std::function<void(std::string_view url)>` | unset | Invoked with the full authorization URL so the caller can open a browser; does not influence the flow outcome |
| `authorization_code_callback` | `std::function<std::optional<AuthorizationCodeResult>()>` | unset | Mandatory, otherwise the authorization flow returns `false` immediately; an empty result, a `state` mismatch or an `iss` mismatch all fail |
| `token_update_handler` | `std::function<void(const TokenContainer&)>` | unset | Invoked every time tokens land in the cache |
| `outbound_origin_allowlist` | `std::optional<std::vector<std::string>>` | `nullopt` | Explicitly appends authorized origins (`scheme://host[:port]`); together with `server_url`, `redirect_uri` and `allowed_authorization_server_origins` it forms the outbound allow-list, which metadata responses cannot expand |
| `loopback_port_exceptions` | `std::vector<std::pair<std::string, std::uint16_t>>` | empty | Adds exact loopback host+port exceptions; exceptions for configured loopback origins are derived automatically, every other loopback port is rejected |
| `dns_resolver` | `std::shared_ptr<mcp::detail::net::IUrlSecurityResolver>` | empty | Resolver injection point used before every outbound connection; empty uses the system `getaddrinfo`. Tests inject a fake resolver and never touch public DNS |
| `allow_cross_origin_redirect` | `bool` | `false` | Cross-origin redirects are rejected by default; enabling them still requires an allow-list hit and drops `Authorization`/`Cookie`/secret headers on origin changes |

### Outbound network security policy (SSRF)

Every OAuth outbound request (PRM, AS metadata, CIMD, DCR registration, token, refresh, revocation, challenge metadata; JWKS is parsed but never fetched) goes through one policy entry point:

1. **Origin allow-list**: only caller-configured scheme/host/port values are accepted, compared exactly after normalizing default ports (http 80, https 443); userinfo, `%`-encoded hosts, numeric/hex encoded addresses and invalid ports are rejected.
2. **Address policy after DNS**: all A/AAAA answers (at most 16) are resolved before connecting and each is checked; loopback, private, link-local, multicast, unspecified and reserved addresses plus IPv4-mapped IPv6 are rejected, and one bad answer rejects the whole set. Loopback is only allowed with an exact host+port exception.
3. **Rebinding binding**: the validated address set is bound to the actual connection; after connect and before sending any bytes, `getpeername()` must still report an address from that set, otherwise the connection is closed and `ProtocolViolation` is thrown. an internal `peer_address_override` field on the HTTP request spec is the test seam for peer observation.
4. **Per-hop redirects**: each hop re-runs scheme + origin allow-list + DNS + address policy; cross-origin redirects are rejected by default, exceeding the budget (5 hops) fails closed, and `Authorization`, `Cookie`, `client_secret`, `registration_access_token` and similar headers are dropped on origin changes. The HTTP client has no proxy path, so no proxy branch can bypass a validated address.

Old behavior → new behavior → affected callers: unknown origins and private addresses used to be reachable; they are now rejected before connect. Deployments needing explicit access must add the origin to `allowed_authorization_server_origins` or `outbound_origin_allowlist`, and local loopback fixtures use `loopback_port_exceptions` (or the exception derived from `server_url`). Cross-origin redirects used to be followed with only `Authorization` stripped; they are now rejected by default — callers that rely on them must both authorize the origin and set `allow_cross_origin_redirect = true`.

### Unified DCR `client_secret` resolution

`ResolveClientSecret()` is the single entry point with a fixed priority: explicit `client_secret` → the `client_secret` from the DCR registration response → a previously stored value isolated by issuer/client_id/resource. Empty values, control characters and values over 4096 bytes fail closed with `InvalidParams`; exceptions and log records name only the source category, never the value, and temporary copies are zeroed by the internal `SecureClear` helper on failure paths. Cache keys come from `SecretCacheKey(issuer, client_id, resource)`, so a secret cannot cross issuer or principal boundaries.

Old behavior → new behavior → affected callers: a DCR `client_secret` used to be parsed into the registration record but never used for client authentication; it now participates in `client_secret_post`/`client_secret_basic`, while an explicit secret still takes precedence. Invalid secrets used to be sent as empty values; they now throw `InvalidParams`, so old registration responses relying on an empty secret must supply a valid value or switch to `private_key_jwt`.

### `OAuthClientProvider` public methods

Declared in the same header; private nested types and members are not user API and are omitted.

| Method | Signature | Return semantics | Notes |
|---|---|---|---|
| Construct/destruct | `explicit OAuthClientProvider(OAuthClientOptions options)` / `~OAuthClientProvider()` | — | Options are taken by value; the token cache is self-created by default |
| `IsSecureEndpoint` | `static bool IsSecureEndpoint(std::string_view url)` | `bool` | https check, loopback hosts excepted |
| `IsSameOrigin` | `static bool IsSameOrigin(std::string_view lhs, std::string_view rhs)` | `bool` | Origin (scheme+host+port) comparison; `false` if either side yields no origin |
| `SecretCacheKey` | `static std::string SecretCacheKey(std::string_view issuer, std::string_view client_id, std::string_view resource)` | isolation key | Builds the secret cache key from issuer / client_id / resource |
| `ResolveClientSecret` | `std::optional<std::string> ResolveClientSecret() const` | resolved secret, `nullopt` when absent | Unified entry point; invalid values throw `InvalidParams` and messages never contain the secret |
| `Authenticate` | `bool Authenticate()` | whether a usable token exists | Strict metadata discovery (no legacy fallback by default) → register / fetch CIMD document as needed → fresh cache returns true → refresh expired tokens, with `invalid_grant`/`invalid_client` falling back to the full authorization flow; cached tokens with a mismatched resource are cleared first |
| `AuthenticateClientCredentials` | `bool AuthenticateClientCredentials()` | whether tokens were stored | Requires `client_id`, plus `client_secret` when no assertion is used; never runs the code flow |
| `GetAccessToken` | `std::string GetAccessToken()` | token string, empty when none | When nearing expiry with a refresh token, joins coalesced refreshing (concurrent callers share one refresh); refresh failure throws `InternalError` |
| `RefreshTokens` | `bool RefreshTokens()` | whether the refresh succeeded | Shares one single-flight with `GetAccessToken` / `Authenticate`; `invalid_grant` clears tokens and returns `false`, transient failure keeps the cache and waiters read the latest token |
| `Revoke` | `void Revoke()` | void | POSTs only when a revocation endpoint exists (non-https throws `InvalidParams`); clears the cache regardless |
| `IsAuthenticated` / `HasToken` | `bool IsAuthenticated() const` / `bool HasToken() const` | `bool` | The former requires an unexpired token, the latter only a non-empty access token; both check resource matching |
| `GetAuthorizationHeader` | `std::string GetAuthorizationHeader() const` | `DPoP <token>` or `Bearer <token>`, empty when none | Read-only, never triggers a refresh; the scheme name follows `enable_dpop` |
| `BuildDPoPProof` | `std::string BuildDPoPProof(std::string_view htm, std::string_view htu, std::optional<std::string_view> access_token = std::nullopt)` | proof JWT; empty when `enable_dpop` is false | Restores a validated P-256 key from the token cache (generates and persists one otherwise); reads the nonce from the bucket keyed by the normalized origin of `htu` and token_type |
| `ApplyDPoPProofHeader` | `void ApplyDPoPProofHeader(const HttpRequestHeaderContext& context, std::map<std::string, std::string>& headers)` | void | Builds the proof from the context method and URL into `headers["DPoP"]` and switches the current nonce bucket to that URL / token_type; leaves headers untouched when dpop is off, the method is missing or the URL is unparseable |
| `ObserveDPoPNonce` / `SetDPoPNonce` | `void ObserveDPoPNonce(std::string_view dpop_nonce)` / `void SetDPoPNonce(std::string nonce)` | void | Legacy entries delegate to the current bucket (set by the latest proof/context call); empty or >1024-byte observed values are ignored; the setter overwrites unconditionally and ignores the dpop switch |
| Context-aware nonce API | `void ObserveDPoPNonce(std::string_view url, std::string_view token_type, std::string_view dpop_nonce)` / `void SetDPoPNonceContext(std::string_view url, std::string_view token_type)` / `std::optional<std::string> GetDPoPNonce(std::string_view url, std::string_view token_type) const` | void / `optional<string>` | Observes and reads nonces in an explicit normalized-origin + token_type bucket; no cross-bucket reuse; host case and default port are normalized |
| `StepUpAuthorization` | `bool StepUpAuthorization(const std::vector<std::string>& additional_scopes)` | whether re-authorization succeeded | Append scopes → `Revoke()` → rerun the authorization flow, which drops the current tokens |
| `HandleAuthChallenge` | `bool HandleAuthChallenge(std::string_view www_authenticate)` | whether metadata was rebuilt from it | Recognizes only `resource_metadata="…"`; headers over 4096 bytes, non-https URLs, documents over 65536 bytes, a metadata `resource` that is not byte-for-byte equal to the configured resource identifier, or an authorization server outside the allow-list all return `false` |

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
| `GenerateKey` | `EcP256Jwk GenerateKey()` | — | Generates a P-256 key (requires OpenSSL) |
| `ValidateKey` | `bool ValidateKey(const EcP256Jwk& key)` | — | OpenSSL-independent: requires 32-byte x/y/d, scalar in `[1,n-1]`, a point on P-256, and `d·G == (x,y)`; returns `false` on mismatch |
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
| DPoP key extension | `virtual void StoreDpopKey(const ext::dpop::EcP256Jwk& key)` / `virtual std::optional<ext::dpop::EcP256Jwk> GetDpopKey()` / `virtual void ClearDpopKey()` | empty body / `nullopt` / empty body (all overridable) | All three have default implementations, so legacy custom `ITokenCache` implementations need no changes; the provider restores a validated key from here on restart |
| `OAuthMetadata` | `struct`: issuer, authorization/token/revocation/registration/jwks/resource endpoints and four `*_supported` lists; `static std::optional<OAuthMetadata> Discover(std::string_view server_url, bool allow_legacy_fallback = false, std::optional<std::string_view> expected_resource = std::nullopt)` | empty strings/lists | The last four endpoints and `resource` are `optional`; PRM must provide a `resource` byte-for-byte equal to the expected resource identifier or it is not used; direct RFC 8414 fallback is off by default |
| `OAuthTokenResponse` | `struct`: `access_token`/`refresh_token`/`expires_in`/`token_type`/`scope` | `expires_in = 3600`, `token_type = "Bearer"` | Token-endpoint payload carrier |
| `ClientRegistrationInfo::Register` | `static std::optional<ClientRegistrationInfo> Register(std::string_view registration_endpoint, std::string_view redirect_uri, std::string_view client_name, std::string_view application_type = "native")` | `"native"` | Dynamic client registration entry point |

### Built-in implementations

| Symbol | Signature/type | Notes |
|---|---|---|
| `InMemoryTokenCache` | `class : public ITokenCache` | In-process implementation, overrides all five interfaces including issuer binding, internally locked |
| `FileTokenCache` construct/destruct | `explicit FileTokenCache(std::filesystem::path cache_path)` / `~FileTokenCache() override` | The constructor loads from disk (a missing file means an empty cache); destructor persistence failures are logged as Error only, never thrown |
| `StoreTokens` / `GetTokens` / `ClearTokens` | `override` | Every write is flushed synchronously; with no tokens the flush deletes the file |
| `StoreBoundIssuer` / `GetBoundIssuer` | `override` | The anchor lives in `<cache_path>.issuer`; an empty string removes that file, write failures are logged as Error; reads reject symlinks, non-regular files, over-permissive modes, content over 4096 bytes, control characters and short reads, returning `nullopt` without clearing tokens or deleting the anchor |
| DPoP key persistence | `StoreDpopKey` / `GetDpopKey` / `ClearDpopKey` `override` | Written into the same JSON / DPAPI file as tokens; loading fully validates x/y/d curve and key-pair match, rejects corruption without overwriting the original field; Windows uses DPAPI as well |
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
| Optional fields | `description`, `title`, `website_url`, `repository`, `meta` (the first four `std::optional<std::string>`, `meta` a `std::optional<JsonValue>`) | all `nullopt` | `description` is still written as an empty string when absent; `title`/`website_url`/`repository`/`meta` are written only when set |
| List fields | `icons` (`std::vector<Icon>`), `endpoints` (`std::vector<ServerCardEndpoint>`), `supported_protocol_versions` (`std::vector<std::string>`) | empty | An empty `endpoints` list fails validation; `icons` is now emitted with the card |
| `ValidateCard` | `void ValidateCard(const ServerCard& card)` | — | Validates required fields, http(s) URLs (rejecting userinfo and malformed ports), icon `src`/`mimeType`/`sizes`/`theme`, endpoint header names/values, variable names/values and date-shaped `supportedProtocolVersions`; violations throw `InvalidParams` |
| `SerializeCard` | `JsonValue SerializeCard(const ServerCard& card)` | — | Calls `ValidateCard` first; fixed keys `name`/`version`/`description`/`icons`/`endpoints`/`supportedProtocolVersions`, other optional keys only when set |
| `ServerCardClientOptions` | `struct { std::optional<std::string> if_none_match; std::optional<ServerCard> cached_card; std::optional<std::size_t> max_response_bytes; std::chrono::milliseconds timeout{30000}; int max_redirects{5}; }` | no conditional header/cache, 1 MiB cap | `if_none_match` with `cached_card` drives conditional requests; a 304 without a cached card throws `InvalidParams` |
| `ServerCardFetchResult` | `struct { ServerCard card; std::string etag; bool from_cache{false}; }` | — | `from_cache=true` means a 304 hit and the caller's cached card was returned |
| `BuildServerCardDiscoveryUrl` | `std::string BuildServerCardDiscoveryUrl(std::string_view endpoint_url)` | — | Derives `<path>/server-card` from the endpoint scheme/host/port/path; strips query/fragment and rejects userinfo, non-HTTP(S) and malformed URLs |
| `IsServerCardJsonMediaType` / `ServerCardEtagMatches` | `bool` (case-insensitive media type/parameters; weak comparison and list syntax, `*` supported) | — | Reusable admission helpers for callers |
| `CanonicalizeServerCardBody` / `ComputeServerCardEtag` | `std::string` (canonical JSON / quoted SHA-256 strong ETag) | — | The same canonical serialization backs the server ETag and client comparison |
| `ParseServerCard` | `ServerCard ParseServerCard(std::string_view json)` | — | Parses every field, then calls `ValidateCard`; non-objects, wrong types and invalid fields throw `InvalidParams` |
| `FetchServerCard` | `ServerCardFetchResult FetchServerCard(std::string_view endpoint_url, const ServerCardClientOptions& options = {})` | — | GETs the discovery URL; accepts only a JSON media type and bounded body, follows same-origin redirects by default, and throws `McpError` on empty bodies or error statuses |

The client free functions above are `[[nodiscard]]`; callers that intentionally discard a result should write `(void)mcp::ext::FetchServerCard(endpoint)`.

The server-side API stays in `include/mcp/ext/ServerCard.hpp` with validation and serialization in `src/ext/ServerCard.cpp`; the client API is declared in `include/mcp/ext/ServerCardClient.hpp` and implemented in `src/ext/ServerCardClient.cpp`. `GetServerCard`, `ValidateCard`, `SerializeCard` and `GET /server-card` all remain. Serialization now includes optional fields that are set; older readers can ignore the extra keys. Cards carrying invalid optional fields are now rejected by both the new client and the server validator (old/new behaviour and how affected callers adapt are listed above).

### Apps

| Symbol | Signature/type | Default | Notes |
|---|---|---|---|
| `UiMeta` | `struct { std::string resource_uri; std::vector<std::string> csp; std::vector<std::string> permissions; }` | empty string/tables/tables | The ui declaration carried in `_meta` |
| `ValidateUiMeta` | `void ValidateUiMeta(const UiMeta& meta)` | — | `resource_uri` must be non-empty and start with `ui://`, otherwise `InvalidParams` is thrown; `csp`/`permissions` are not validated |

Declared in `include/mcp/ext/Apps.hpp`; validation is in `src/ext/Apps.cpp`. `UiMeta`, `ValidateUiMeta`, `kAppsExtensionId`, `kAppsMimeType` and `kAppsUriScheme` are unchanged.

### Apps host bridge

Declared in `include/mcp/ext/AppsHostBridge.hpp`, implemented in `src/ext/AppsHostBridge.cpp`. This section covers only the JSON message bridge between a host and a sandboxed UI; no iframe, WebView, DOM, script injection or UI renderer is delivered.

| Symbol | Signature/type | Default | Notes |
|---|---|---|---|
| `kAppsMessageSchemaVersion` | `std::string_view` | `"2026-07-28"` | The only supported schema version; malformed or missing values are rejected |
| `kAppsCapabilityTokenBytes` | `size_t` | `32` | Random bytes in a capability token; exposed as a 64-character hex string |
| `kDefaultAppsCapabilityTtl` | `std::chrono::milliseconds` | `60000` | Default capability lifetime |
| `AppsHostBridgeOptions` | `struct` | see below | `ui_meta` (from `UiMeta`: `resource_uri`/`csp`/`permissions`), `allowed_origins` (host-trusted origin allowlist), `host_allowed_methods` (explicit host method allowlist), `max_message_bytes`(65536), `max_depth`(32), `max_nodes`(10000), `max_string_bytes`(65536), `max_id_bytes`(128), `max_method_bytes`(128), `capability_ttl`(60s), `single_use_sensitive_methods`(`true`) |
| `AppsPostMessage` | `struct` | schema version preset, empty id/method/params | Carries only JSON values, schema version, optional `request_id`, `method` and `params`; no function/DOM/executable-code fields |
| `AppsMessageContext` | `struct` | `sandbox_verified=false`, other fields empty | Host-trusted context: `source_origin`, `target_origin`, `resource_uri`, `sandbox_verified` and `capability`, all supplied by the host |
| `AppsSandboxPolicy` | `struct` | all five deny flags `true`, `csp` empty | Exposes opaque origin, bans `allow-same-origin`, top navigation, popups, forms and downloads; `csp` comes verbatim from `UiMeta` |
| `AppsHostBridge::Create` | `static std::unique_ptr<AppsHostBridge> Create(AppsHostBridgeOptions)` | — | Validates `UiMeta`, budgets and origin/method syntax; effective permissions = `UiMeta::permissions` ∩ `host_allowed_methods`, empty means deny every method |
| `SerializeHostMessage` | `std::string SerializeHostMessage(const AppsPostMessage&, const AppsMessageContext&) const` | — | Host-to-app serialization; still checks trusted context, schema version, method permission and byte budget |
| `ParseAppMessage` | `AppsPostMessage ParseAppMessage(std::string_view, const AppsMessageContext&) const` | — | App-to-host parsing; checks total bytes before parsing, then origin, sandbox, capability, schema, method and id |
| `IsMethodAllowed` | `bool IsMethodAllowed(std::string_view, const AppsMessageContext&) const` | — | Read-only predicate; unknown methods, excess permissions and expired/consumed capabilities return `false` |
| `CreateCapability` | `std::string CreateCapability(const AppsMessageContext&) const` | — | Issues an opaque token bound to `resource_uri`, allowed origins, effective permissions, expiry, message byte cap and the single-use flag |
| `GetSandboxPolicy` | `AppsSandboxPolicy GetSandboxPolicy() const` | — | Returns fixed sandbox deny flags plus the CSP from `UiMeta` |
| `NormalizeOrigin` | `bool NormalizeOrigin(std::string_view, std::string&)` | — | Lowercases scheme/host, supplies the default port (80 for http, 443 for https) and emits `scheme://host:port`; rejects `*`, `null`, userinfo, paths, wildcards and non-HTTP(S) schemes |
| `IsAppsSensitiveMethod` | `bool IsAppsSensitiveMethod(std::string_view)` | — | `tools/call` and `resources/read` are sensitive: denied unless explicitly permitted, and require a single-use capability |

Trust boundary:

- `source_origin`, `target_origin`, `resource_uri` and `sandbox_verified` are read only from `AppsMessageContext`. Payload fields with those names, plus `permissions` or `capability`, are unknown top-level keys and are rejected; they never take part in an authorization decision.
- Origins are normalized and compared exactly on scheme/host/effective port; omitting a default port is equivalent to writing it explicitly. A wildcard or opaque target, an origin outside the allowlist, and an unproven sandbox are always rejected.
- Effective permissions are the intersection of `UiMeta::permissions` and `host_allowed_methods`. Sensitive methods are denied by default; with `single_use_sensitive_methods=true` the capability is single-use. Tokens use 32 unpredictable bytes, are compared in constant time, and are bound to resource/origins/permissions/expiry/byte cap; expiry, replay, cross-context use and permission changes are rejected.
- JSON parsing checks total bytes first, then depth/node/string budgets. Duplicate keys, non-finite numbers, overlong ids/methods and malformed schema versions throw `McpError` with fixed classifier messages that never echo payload content.

::: warning Old behaviour / new behaviour / how affected callers adapt
- Old behaviour: before 0.3.13 there was no Apps host bridge API; hosts assembled postMessage themselves with no single enforcement point for origin, sandbox or permissions.
- New behaviour: the new API fails closed by default; any message without a proven sandbox, with an unlisted origin, an unknown method or an unissued capability is rejected.
- How to adapt: hosts that assembled postMessage manually should populate `AppsMessageContext` on the host side (never from message payload), call `CreateCapability`, and route traffic through `SerializeHostMessage`/`ParseAppMessage`. `UiMeta`/`ValidateUiMeta`, the Apps MIME and URI constants, and the server `ui://` resource read gate are unchanged and need no migration.
:::

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
| `kDeleteTask` | `"tasks/delete"` | modern only; removes a task after the owner check |
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
| `kTasks` | `"notifications/tasks"` | modern only; `params` is a full `DetailedTask`, filterable by `taskIds` subscriptions |
| `kTaskStatus` / `kTaskWorking` / `kTaskCompleted` / `kTaskFailed` / `kTaskCancelled` / `kTaskInputRequired` | `"notifications/tasks/status"`, `"…/working"`, `"…/completed"`, `"…/failed"`, `"…/cancelled"`, `"…/input_required"` | legacy only; modern no longer sends or accepts these |
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
- `include/mcp/ext/DPoP.hpp`, `include/mcp/ext/ServerCard.hpp`, `include/mcp/ext/ServerCardClient.hpp`, `include/mcp/ext/Apps.hpp`, `include/mcp/ext/AppsHostBridge.hpp`, `include/mcp/ext/Triggers.hpp`, `include/mcp/Capabilities.hpp`
- `include/mcp/Methods.hpp`, `include/mcp/ProtocolVersion.hpp`, `include/mcp/McpTypes.hpp`, `include/mcp/transport/StreamableHttpClientTransport.hpp`, `include/mcp/server/ServerOptions.hpp`
- `src/client/auth/OAuthClientProvider.cpp`, `src/client/FileTokenCache.cpp`, `src/client/SkillVerify.cpp`, `src/ext/DPoP.cpp`, `src/ext/ServerCard.cpp`, `src/ext/ServerCardClient.cpp`, `src/ext/Apps.cpp`, `src/ext/AppsHostBridge.cpp`, `src/ext/Triggers.cpp`, `src/server/McpServer.cpp`
- `cmake/FetchDependencies.cmake`, `cmake/OpenSslVersion.cmake`, `CMakeLists.txt`
