# Installation and Integration

Build this repository from scratch, then consume the SDK from your own C++17 project. Every library is static; there are no runtime DLL dependencies.

## Requirements

| Dependency | Minimum | Notes |
|---|---|---|
| CMake | 3.28 | `cmake_minimum_required(VERSION 3.28...4.2)`, see `CMakeLists.txt` |
| Generator | Ninja | The presets declare `generator: "Ninja"` (`CMakePresets.json`); only Ninja is validated by this repository |
| C++ standard | C++17 | Every library sets `target_compile_features(... PUBLIC cxx_std_17)` |
| Compiler | MSVC / Clang / GCC / clang-cl | Clang is auto-detected on Windows and Linux (top-level `CMakeLists.txt`); with MSVC only, pass `-DCMAKE_CXX_COMPILER=cl` explicitly |
| Platforms | Windows, Linux, macOS | All three build in CI |
| OpenSSL | Optional | Controlled by `MCP_WITH_OPENSSL`, see below |

## Building this repository from scratch

```bash
git clone --recurse-submodules https://github.com/jesspig/modelcontextprotocol-cpp-sdk.git
cd modelcontextprotocol-cpp-sdk
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

- If you forgot `--recurse-submodules`, run `git submodule update --init --recursive`: googletest lives in `third_party/googletest` and is only needed when `MCP_BUILD_TESTS=ON`.
- The presets write to `build/debug` and `build/release` and pin `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, `MCP_BUILD_TESTS=ON`, `MCP_BUILD_EXAMPLES=OFF`.
- Single case: `ctest --preset debug -R 'McpServerTest.CaseName'`, or run `build/debug/tests/unit/mcp-core-tests --gtest_filter='McpServerTest.*'` directly.
- Check the registered case count with `ctest -N`.

## Consuming the SDK from your project

### Option 1: FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/jesspig/modelcontextprotocol-cpp-sdk.git
    GIT_TAG        0.3.13
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp-client mcp-server)
```

Prefer a released tag (`0.3.13`) over a branch in `GIT_TAG`; use `master` only for the development line. `kSdkVersion`, the CMake project version and the package version file all report `0.3.13`. Behavior differences from 0.3.12 are in the [0.3.13 release notes](/en/migration/0.3.13). Dependencies propagate PUBLIC, so linking `mcp-client` or `mcp-server` is enough — do not hand-write the whole chain.

### Option 2: install, then find_package

```bash
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix <your-install-prefix>
```

```cmake
find_package(mcp CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE mcp::mcp-client)
```

Installation exports `mcpTargets.cmake` with the package version file (`COMPATIBILITY SameMinorVersion`; the version comes from `kSdkVersion` in `include/mcp/McpVersion.hpp`). For 0.x, only same-minor requests no higher than the installed version are accepted: with 0.3.13 installed, requests for 0.3.12/0.3.13 succeed and 0.4.0 is rejected. `find_package` calls `find_dependency(Threads)` and `find_dependency(re2 CONFIG)` for you and loads the installed rapidyaml static archive through the `mcp::ryml` imported target; when the library was built with OpenSSL it also calls `find_dependency(OpenSSL)` and re-validates at consumer time that the major version is 3 or 4 (`cmake/mcpConfig.cmake.in`). Exported targets carry the `mcp::` namespace (`CMakeLists.txt`).

## Static library inventory

| Target | Responsibility | Depends on |
|---|---|---|
| `mcp-core` | JSON-RPC, types, error codes, logging, spans | — |
| `mcp-transport` | Transports, platform IO, public process API | `mcp-core` |
| `mcp-protocol` | Session engine and dual-era codecs | `mcp-transport` |
| `mcp-server` | Server facade | `mcp-protocol`, `mcp-ext` |
| `mcp-client` | Client facade | `mcp-protocol`, `mcp-ext` |
| `mcp-http` | HTTP server substrate, session and event stores | `mcp-transport`, `mcp-ext` |
| `mcp-ext` | apps, server card, DPoP, triggers extensions | `mcp-core`, `mcp-transport` |

All seven targets are `STATIC` (`CMakeLists.txt` and the `add_library(mcp-* STATIC)` lines).

## Build switches

| Switch | Default | Effect |
|---|---|---|
| `MCP_BUILD_TESTS` | `OFF` (`ON` in the presets) | Unit, integration and conformance test targets |
| `MCP_BUILD_EXAMPLES` | `OFF` | Programs under `examples/` |
| `MCP_BUILD_CONFORMANCE` | `OFF` | Server fixture for the official conformance suite (`examples/conformance/server`) |
| `MCP_BUILD_DOCS_SNIPPETS` | `OFF` | Syntax-check `docs/snippets/*.cpp` with `/Zs` or `-fsyntax-only` |
| `MCP_WERROR` | `OFF` | Warnings become errors; CI always enables it |
| `MCP_WITH_OPENSSL` | `AUTO` | See below |
| `MCP_UNITY_BUILD` | `ON` | Jumbo build; batch size `MCP_UNITY_BATCH_SIZE`, per-batch memory cap `MCP_UNITY_MEM_MB` |

## OpenSSL is optional

`MCP_WITH_OPENSSL` has three modes and interacts with `find_package(OpenSSL)` (`cmake/FetchDependencies.cmake`):

- `AUTO`: enabled when found, disabled otherwise.
- `ON`: configure fails when OpenSSL is missing; no silent downgrade.
- `OFF`: forced off even when installed.

When enabled, the single INTERFACE target `mcp-openssl` defines `MCP_HAVE_OPENSSL` and links `mcp-transport`/`mcp-client`/`mcp-ext`. **Never use `__has_include` to guess whether OpenSSL is available** — a visible header does not mean the macro is defined, let alone that OpenSSL is linked. Versions 3.x and 4.x are supported; an out-of-range major version fails configure. When disabled: TLS is unavailable (`wss` WebSocket, HTTPS SSE), the OAuth PKCE random source falls back to `BCryptGenRandom` (Windows) or `std::random_device` (POSIX), SHA-256 stays built in, and both `private_key_jwt` assertions with a configured `private_key_pem` and DPoP proofs throw `McpError(InternalError)`.

System installation examples: `vcpkg install openssl`, `apt install libssl-dev`, `brew install openssl@3`. In CI pin the major version explicitly; do not use the rolling `brew install openssl` alias.

## Distribution pitfalls

- **`-march=native` is on by default**: it applies when not in CI and not on Apple or Windows (flags at `cmake/CompilerOptions.cmake`; the flag block exists only in the non-Windows Clang/GCC branches), so binaries are not portable across machines. CI mode is decided by the `CI` environment variable (`cmake/Platform.cmake`). Build release artifacts in CI, or remove the flag yourself.
- **Release enables LTO automatically** (Clang ThinLTO, MSVC LTCG, GCC IPO); slow local Release links are expected.
- **Unity build defaults to ON** for core/server/client/http/ext but OFF for protocol and transport. Check a target's `UNITY_BUILD` before adding a `.cpp` — anonymous namespaces or duplicate static symbols will collide.

## Verification

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Expect configure to print `[mcp] SDK version: 0.3.13` plus the OpenSSL decision, the build to complete without errors, and `ctest` to pass in full. Measured baselines are 1242 registered cases on Windows clang-cl (T-20) / 1243 after the T-21 fix, and 1266 cases on WSL2/Linux Release and sanitizer builds (T-22); the 1037 figure belongs to the released 0.3.12 tag and must not be used to judge regressions in this release. To verify linking only, build a minimal executable that includes `<mcp/client/McpClient.hpp>` and links `mcp-client`.

## Next steps

- Run one end-to-end round trip after installing: [/en/start/quickstart](/en/start/quickstart)
- The first thing a server does: [/en/howto/register-a-tool](/en/howto/register-a-tool)
- Client-side transport and auth choices: [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server), [/en/howto/add-oauth-to-a-client](/en/howto/add-oauth-to-a-client)
- Choosing a transport: [Transports](/en/learn/transports)
- Signatures and fields: [Server reference](/en/reference/server)

## Source files behind this page

- `CMakeLists.txt`, `CMakePresets.json`, `cmake/CompilerOptions.cmake`, `cmake/FetchDependencies.cmake`, `cmake/BuildOptimization.cmake`, `cmake/Lto.cmake`, `cmake/Platform.cmake`, `cmake/mcpConfig.cmake.in`, `include/mcp/McpVersion.hpp`
