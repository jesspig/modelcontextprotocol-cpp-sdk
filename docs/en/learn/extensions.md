# Extensions

An extension is an optional add-on to protocol capability: it occupies a key in the capability table, a set of methods on the wire, and a switch in the implementation. This page maps the switch family onto protocol extension identifiers, shows what capability derivation produces, and explains how entry points behave when a switch is off. Types and field tables live in [/en/reference/auth-and-extensions](/en/reference/auth-and-extensions).

## How Extensions Appear in the Protocol

Capability structs carry exactly one extension-related member: a map from a reverse-DNS identifier to that extension's configuration object (free-form JSON). Server capabilities are derived from registration plus switches, client capabilities from options, and both ends negotiate through the same map. Method names are not hidden inside the extension — an extension either brings its own methods or changes the behaviour of existing ones; for the era each method belongs to, see [/en/learn/protocol-versions](/en/learn/protocol-versions).

## Only Four Switches Reach the Wire

Of the eight server-side switches, only four write an extension key into the capability table:

| Wire identifier | Server switch | Declaration condition | Declared value |
|-----------------|---------------|-----------------------|----------------|
| `io.modelcontextprotocol/tasks` | `enable_tasks_extension` | declared whenever the switch is true | empty settings object |
| `io.modelcontextprotocol/skills` | `enable_skills_extension` | true **and** at least one skill registered **and** the resources capability present | settings object including the directory-read flag |
| `io.modelcontextprotocol/ui` | `enable_apps_extension` | declared whenever the switch is true | empty object |
| `io.modelcontextprotocol/server-card` | `enable_server_card` | declared whenever the switch is true | empty object |

The remaining switches (etag cache, progressive discovery, file input, triggers/events) have no wire identifier at all: they change only what this SDK does and declare nothing to the peer. Calling them "extensions" creates confusion whenever negotiation is discussed — the peer cannot see them in the capability table and therefore cannot depend on them.

Skills are the one case where the declaration condition is stricter than the switch: switch on but register no skill, or lack the resources capability, and the skills key never appears. That is not a defect but the direct consequence of "capabilities are derived from registration" (see [/en/learn/architecture](/en/learn/architecture)).

## Server-Side Switch Family

| Switch | Effect when on | Entry-point behaviour when off | Wire identifier |
|--------|----------------|-------------------------------|-----------------|
| `enable_tasks_extension` | declares the tasks extension; the three task methods and task-shaped tool calls become live | task methods reject as method not found | yes |
| `enable_skills_extension` | declares the skills extension when the conditions above hold; skill listing, retrieval and directory reading become live | the three skill methods reject as method not found | yes |
| `enable_apps_extension` | declares the ui extension; `ui://` resource reads take the normal path and validate their metadata | `ui://` resource reads reject as handler error; non-`ui://` resources are unaffected | yes |
| `enable_server_card` | declares the server-card extension; the card accessor returns the explicit or default card after validation | the card accessor throws handler error; the HTTP card route is gated separately by the same-named transport switch | yes |
| `enable_etag_cache` | list-shaped results carry a content hash and unchanged hits fall back to the local stored copy | results carry no hash and conditional requests are not sent | no |
| `enable_progressive_discovery` | the tool list supports query filtering and paging | the tool list returns the cached full set | no |
| `enable_file_input` | local and resource references inside tool arguments are sliced by range and small files inlined; remote addresses pass through | arguments pass through verbatim | no |
| `enable_triggers_events` | the trigger registry matches by type, delivers the event notification through subscriptions and calls webhooks | the emit call returns immediately and the event is dropped | no |

Trigger webhooks retry internally (up to three attempts with growing backoff) and a final failure logs a warning only, leaving the already-delivered event notification intact — a different posture from the "failure is an error" behaviour elsewhere, so treat it separately.

## Client-Side Switch Family

| Switch | Effect when on | Entry-point behaviour when off | Wire identifier |
|--------|----------------|-------------------------------|-----------------|
| `enable_tasks_extension` | participates in tasks-extension declaration; the input-submission and poll-to-completion entry points become live | those two entry points throw method not found locally, while the remaining task entry points still send | yes (the capability map must carry the key too) |
| `enable_skills_extension` | skill listing, retrieval and directory reading become live | all three throw method not found locally | yes |
| `enable_progressive_discovery` | tool-search requests carry query and page size | paging aggregation still works, but the two parameters are not sent | no |
| `enable_etag_cache` (on by default) | read entry points send conditional requests and reuse stored copies for unchanged responses | no conditions attached, stored copies unread, requests sent as usual | no |

The client-side tasks switch has a smaller footprint than it looks: it only blocks "submit input" and "poll to completion". Fetching a task, cancelling one, and issuing a tool call as a task still send requests. The real backstop is on the response side — in the modern era, receiving a task result without having declared the tasks extension throws handler error. See the next section.

## What the Entry Points Return When a Switch Is Off

Two kinds of rejection carry different meanings and must not be interchanged:

- **Method not found**: this method should not exist on this connection. Applies when an extension switch is off, when the era allow-list does not contain the method, or when the method was never wired up at all — tool, skill and task entry points plus completion without a registered callback all land here, on both the server side and in client-local guards.
- **Handler error**: the method exists but this execution was refused. Applies when the method is perfectly legitimate in general and only the switch says it should not be touched here: `ui://` resource reads are the typical case, since resource reading is in the allow-list in every era and disabling the apps switch only fails that uri family. The card accessor behaves the same way, and it was never a wire method — just a local accessor.

A third rejection is easy to confuse with these: **missing required client capability**. In the modern era the server checks, while handling task methods, whether the peer declared the tasks extension, and refuses with this code plus a description of the required capability if it did not; sampling input requests run the same check. That is a negotiation gap, not a switch being off.

## Implicit Enablement Has Been Removed

Setting only a task store no longer declares the tasks extension and no longer logs a warning; skills never had an implicit path. Both now require the switch to be set explicitly. Two "implicitly enabled" accessor helpers remain on the options struct, but they are marked deprecated and have no call site anywhere in production code — they are compatibility residue for external callers, not something to reason about behaviourally.

## Skills: uri Shape and Directory Reading

A skill entry's uri must have the shape `<scheme>://<name>/SKILL.md`: a non-empty scheme part, a non-empty first path segment, and the fixed file name at the end. No specific scheme is mandated — `skill://` is a convention, not a requirement. A malformed uri or a duplicate uri at registration throws invalid params and never enters the registry.

Directory reading is a separate flag inside the skills extension declaration and must be enabled explicitly; while it is off, the directory-read entry point answers invalid params. Its effect is to treat a skill uri minus the trailing file name as a directory prefix and list the children beneath it — so the directory structure is determined entirely by uri naming, not by a second registration API.

Whether skill content is trustworthy is not decided by server-side shape validation: limit checks, file digest comparison, frontmatter consistency review and approval-snapshot re-validation are provided as separate client-side helpers.

## When Switches Take Effect

Both server and client copy options by value at construction, so mutating your own options object afterwards has no effect. The etag cache switch is the one exception in shape: it is process-global state, and constructing any server instance with it enabled turns the read/write path on for the whole process — multiple instances in one process cannot therefore hold different values.

## Further Reading

- [/en/reference/auth-and-extensions](/en/reference/auth-and-extensions) — extension settings types and switch field tables
- [/en/learn/protocol-versions](/en/learn/protocol-versions) — extension methods against the era allow-lists
- [/en/learn/architecture](/en/learn/architecture) — capabilities derived from registration
- [/en/learn/transports](/en/learn/transports) — how statelessness limits multi-round-trip
- [/en/reference/server](/en/reference/server) — server signatures for registration and switch wiring
