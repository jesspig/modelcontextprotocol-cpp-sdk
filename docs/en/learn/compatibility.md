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
| `ServerOptions::TasksImplicitlyEnabled()` | 0.3.7 | set `enable_tasks_extension=true` explicitly | with the protocol |
| `ServerOptions::SkillsImplicitlyEnabled()` | 0.3.7 | set `enable_skills_extension=true` explicitly | with the protocol |

## 0.3.13 review

- Every `[[deprecated]]` attribute in the public headers was checked: `ResourceNotFound` carries `since 0.3.11, removed in 0.4.0` and is replaced by `InvalidParams`; the deprecation window spans more than one minor (0.3.11 → 0.3.13), has not expired, and the implementation is retained.
- `Ping`, `SetSamplingHandler`, `SetRootsHandler`, `SetLegacyUrlElicitationHandler`, `TasksImplicitlyEnabled`, and `SkillsImplicitlyEnabled` are driven by protocol revisions or earlier library versions; the registry marks them "with the protocol", and each replacement matches the attribute verbatim. All implementations are retained.
- **The 0.3.13 candidate adds no deprecation and deletes nothing on expiry**; no old API was silently removed for cleanup. `tests/compat/expected_breaks.txt` is byte-for-byte identical to 0.3.12, with entries only decreasing.
- The itemized review is in `.handoff/evidence/T-24/deprecation-review.txt`; migration comparisons are in the [0.3.13 migration notes](/en/migration/0.3.13).

## Expiry checks

Before a release, verify item by item: every expiry version has been removed, this page and `docs/{zh,en}/migration/` agree, and `mcp-compat-smoke` still passes; a release must not ship while any check fails.
