# Protocol Versions

This page is the **only** place in the documentation that maintains the dual-era differences: every other page links back here instead of copying the tables. The SDK supports two protocol eras through one façade and two codec implementations, so any statement about whether a capability is available has to name the era.

## The Five Supported Versions

| Version | Era | Notes |
|---------|-----|-------|
| `2024-11-05` | Legacy | first stable handshake shape |
| `2025-03-26` | Legacy | default negotiated value when the declaration is missing |
| `2025-06-18` | Legacy | lower bound for the roots-changed notification |
| `2025-11-25` | Legacy | upper bound of the legacy era and the fallback for an unknown `initialize` version |
| `2026-07-28` | Modern | the only version on the modern side |

The supported list defaults to these five and can be narrowed by a server-side version allow-list, which simultaneously affects the `server/discover` answer, the `initialize` candidate set and inbound `_meta` validation.

## How the Era Is Decided

The decision is a lexicographic comparison of the version string: greater than or equal to `2026-07-28` means modern, anything else means legacy. Because versions are ISO dates, lexicographic order matches chronological order, so "modern" is currently equivalent to "exactly `2026-07-28`" — only one version sits on the modern side. This predicate is the single era switch; every branch inside the SDK goes through it and there is no second test.

The negotiated result selects the codec: the modern era uses the 2026 codec, everything else the 2025 codec. The codec is swapped as a unit once negotiation completes, so changing versions is safe while the message loop is running.

## Handshake Shape

| Item | Legacy era | Modern era |
|------|------------|------------|
| Entry point | `initialize` | `server/discover` |
| Required follow-up | client must send `notifications/initialized` | none — each request carries its own context |
| Behaviour before handshake | every other request is rejected as "server not initialized" | the state does not exist |
| Where the version is declared | `params.protocolVersion` | the version field inside per-request `_meta` |
| Can it negotiate into the modern era | no | yes |

The last row deserves expanding: `initialize` **never** negotiates a modern version. A client declaring a modern version gets `2025-11-25` back; a non-empty declaration missing from the supported list also gets `2025-11-25`; declaring no version at all gets the default `2025-03-26`. The modern era is reachable through `server/discover` only — and conversely, when `_meta` shows a legacy version the server rejects `server/discover` as "method not found".

## Dual-Era Availability Matrix: Requests

Each codec holds its own method allow-list; a request outside its era is judged "not in era" and rejected as method not found. The table below maps one row per allow-list member; the final column lists gates this SDK adds on top of the allow-list.

| Method | Legacy | Modern | Extra gate in this SDK |
|--------|--------|--------|------------------------|
| `tools/list` | available | available | the progressive-discovery switch decides cached-full vs query-and-page |
| `tools/call` | available | available | task execution mode works in both eras, but the modern era needs the peer to declare the tasks extension |
| `resources/list` | available | available | none |
| `resources/read` | available | available | the `ui://` prefix is gated by the apps switch |
| `resources/templates/list` | available | available | none |
| `prompts/list` | available | available | none |
| `prompts/get` | available | available | none |
| `completion/complete` | available | available | rejected as method not found when no completion callback is set |
| `server/extensions/list` | available | available | the answer is built from already-declared extensions |
| `initialize` | available | rejected | in the modern era only an un-negotiated session may pass |
| `ping` | available | rejected | the client entry point is marked deprecated |
| `resources/subscribe` | available | rejected | the modern era carries this through the subscription stream |
| `resources/unsubscribe` | available | rejected | same |
| `logging/setLevel` | available | rejected | in the modern era the client entry point throws a protocol violation instead of sending |
| `roots/list` | available | rejected | the client entry point is marked deprecated |
| `sampling/createMessage` | available | rejected | in the modern era it only appears as a multi-round-trip input request |
| `elicitation/create` | available | rejected | the modern era carries this through multi-round-trip |
| `tasks/get` | available | available | needs the tasks switch; the modern era adds a task-name header check and a capability-declaration test |
| `tasks/update` | available | available | same |
| `tasks/cancel` | available | available | same |
| `tasks/result` | available | rejected | none |
| `tasks/list` | available | rejected | none |
| `server/discover` | rejected | available | rejected when `_meta` shows a legacy version |
| `subscriptions/listen` | rejected | available | needs the subscriptions master switch (on by default) |
| `skills/list` | rejected | available | needs the skills switch and the resources capability |
| `skills/get` | rejected | available | same |
| `resources/directory/read` | rejected | available | also requires directory reading to be declared |

That covers 27 method names. Three easy misreadings: the three task methods appear in both allow-lists and are therefore a **dual-era intersection**, not "legacy only"; `tasks/result` and `tasks/list` are the legacy-only pair; `server/extensions/list` is a member of the shared set, and the earlier claim that it "belongs to no era" was wrong.

## Dual-Era Availability Matrix: Notifications

| Membership | Notification names |
|------------|--------------------|
| Shared by both eras (14) | `notifications/cancelled`, `notifications/progress`, `notifications/message`, `notifications/resources/updated`, `notifications/resources/list_changed`, `notifications/tools/list_changed`, `notifications/prompts/list_changed`, `notifications/tasks/status`, `notifications/tasks/working`, `notifications/tasks/completed`, `notifications/tasks/failed`, `notifications/tasks/cancelled`, `notifications/tasks/input_required`, `notifications/triggers/event` |
| Legacy only (3) | `notifications/initialized`, `notifications/elicitation/complete`, `notifications/roots/list_changed` |
| Modern only (1) | `notifications/subscriptions/acknowledged` |

That covers 18 notification names. The six task notifications and the trigger-event notification are all in the shared set (the server does send them), so the earlier account — "task notifications belong to 2025 only and the remaining five are in no era's set" — was wrong. The modern era adds a shape rule: a notification must not carry `id`, `result` or `error`, otherwise it is invalid. The roots-changed notification additionally requires a negotiated version of at least `2025-06-18` on the client side.

## Where `_meta` Lives and When It Is Required

Inside a request object the metadata is a field parallel to `params`, and it is folded into `params._meta` on the way out — both eras share that wire shape, and they differ on whether it is mandatory:

| Item | Legacy era | Modern era |
|------|------------|------------|
| Must it be present | not required | required for everything except `server/discover`; absence makes the request invalid |
| Backfill marker | the concept does not exist | the view records "folded in from the top level this time", separating native from synthetic |
| What outgoing requests write | nothing | three items: version, client info, client capabilities |
| Meaning for stateless design | relies on remembered session state | requests carry their own context, and the handshake is the only call with no precondition |

`server/discover` is the sole exemption: it is precisely the bootstrap call that establishes the context.

## The `resultType` Field in Responses

Result encoding in the legacy era is the identity function: the object goes on the wire unchanged. The modern era requires the result object to contain `resultType`, and a missing one makes the response invalid; outgoing encoding stamps a "complete" marker automatically and hoists the two cache-hint fields (time-to-live and scope) to the top level. The five list-shaped responses (tools, resources, resource templates, prompts, skills) must furthermore have `resultType` set to "complete" — a list result may not be a partial multi-round-trip result.

## Reverse Requests: `elicitation_id` and Multi-Round-Trip

Server-initiated user input collection takes two entirely different paths depending on the era, and `elicitation_id` belongs to the legacy side only:

- Legacy: the server sends `elicitation/create` directly. URL mode must carry an `elicitation_id`, and a missing one is rejected as invalid params; after answering, the client also sends `notifications/elicitation/complete`. The field is serialized only for non-form modes, and never at all on the modern side.
- Modern: `elicitation/create` is not in the allow-list. The server's collection entry point instead raises an "input required" signal, places the collection request into the multi-round-trip input-request set, and the client supplies the input and retries within the same request-response chain. URL mode neither generates nor requires an `elicitation_id`, and no completion notification is sent.

Sampling follows the same rule: the legacy era allows a direct server request, the modern era only allows it as a multi-round-trip input request, and the server first confirms the peer declared the sampling capability — otherwise it answers "missing required client capability".

## Error Code Remapping

Three transport-internal error codes are remapped to internal error (`-32603`) on the way out in the modern era, with a warning logged: request timeout (`-32001`), connection refused (`-32003`) and TLS handshake failed (`-32004`). The point is to stop disguising internal faults as protocol errors. Four genuine protocol codes pass through unchanged: header mismatch (`-32020`), missing required client capability (`-32021`), unsupported protocol version (`-32022`) and URL elicitation required (`-32042`). The unsupported-version case attaches a `data.supported` array so the client can perform a corrective retry; `-32042` is currently only registered in the pass-through whitelist and has no throw site inside the SDK. The legacy era performs no remapping.

## The Three Client Connect Modes

| Mode | Behaviour | Resulting era |
|------|-----------|---------------|
| Auto (default) | probes `server/discover`, then falls back or throws depending on the transport category | modern on success, legacy on fallback |
| Legacy | forces the `initialize` handshake | legacy |
| Pin | sends no handshake request at all and builds an empty discovery result from the pinned version | modern |

Pin mode skips negotiation, which also skips learning the server's capabilities and info — the capability table comes out empty, so it should only be used when the peer's behaviour is already known. The fallback rules for a failed probe are classified by transport; see [/en/learn/transports](/en/learn/transports).

## Obligations When Adding a Protocol Method

- Change both allow-lists: adding to only one makes the other side reject the method as "not in era", which surfaces as method not found.
- The notification allow-lists work the same way, and modern-era notification bodies must not carry response fields.
- A reverse request that could be sent directly in the legacy era must be reworked as a multi-round-trip input request in the modern era, or the peer cannot answer it.
- Methods with task semantics need the modern-era header validation and the client capability-declaration check.
- Any change to the response shape must still satisfy the modern-era `resultType` rule, and list-shaped methods may not return partial results.

## Further Reading

- [/en/reference/errors](/en/reference/errors) — the full error code table and data payload shapes
- [/en/reference/types](/en/reference/types) — request and result type field tables
- [/en/reference/client](/en/reference/client) — connect modes and negotiation entry signatures
- [/en/learn/transports](/en/learn/transports) — probe fallback classified by transport
- [/en/learn/extensions](/en/learn/extensions) — how extension switches stack on top of the allow-lists
- [/en/howto/cancel-a-request](/en/howto/cancel-a-request) — cancellation landing in different places per era
- [/en/howto/collect-user-input](/en/howto/collect-user-input) — how asking the user changes shape across eras
