# Deprecation Registry

This page is the deprecation registry required by the compatibility constitution: every deprecated item lists its starting version, replacement, and removal version, and failing to delete it at the removal version is a breach. Compatibility classes are defined in the [0.3.11 release notes](/en/migration/0.3.11).

## Registration rules

- A deprecated item keeps its implementation and is marked `[[deprecated("since X.Y.Z, removed in A.B.C; <replacement>")]]`; the deprecation window spans at least one minor release.
- Adding or removing an entry requires updating this page and the matching migration or release notes in the same change, keeping the Chinese and English sides in sync.
- Deprecations driven by a protocol revision rather than a library version are marked "with the protocol" and carry no concrete version number.

## Current entries

| API | Since | Replacement | Removal version |
|---|---|---|---|
| `mcp::McpErrorCode::ResourceNotFound` | 0.3.11 | `McpErrorCode::InvalidParams` (the modern era no longer sends `-32002`) | 0.4.0 |
| `McpClient::Ping()` | 2026-07-28 protocol revision | none (the protocol removes the method) | with the protocol |
| `McpClient::SetSamplingHandler()` | SEP-2577 | `SetElicitationHandler()` | with the protocol |
| `McpClient::SetRootsHandler()` | SEP-2577 | none (roots are deprecated in 2026-07-28) | with the protocol |
| `McpClient::SetLegacyUrlElicitationHandler()` | 0.3.9 | `SetUrlElicitationHandler()` (three-state result) | with the protocol |
| `ServerOptions::TasksImplicitlyEnabled()` | 0.3.9 | set `enable_tasks_extension=true` explicitly | with the protocol |
| `ServerOptions::SkillsImplicitlyEnabled()` | 0.3.9 | set `enable_skills_extension=true` explicitly | with the protocol |

## Expiry checks

Before a release, verify item by item: every expiry version has been removed, this page and `docs/{zh,en}/migration/` agree, and `mcp-compat-smoke` still passes; a release must not ship while any check fails.
