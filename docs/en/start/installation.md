# Installation and Integration

How to build this repository from scratch and how to consume the SDK from your own C++17 project. Every library is static; there are no runtime DLL dependencies.

## Requirements

| Dependency | Minimum | Notes |
|---|---|---|
| CMake | 3.28 | `cmake_minimum_required(VERSION 3.28...4.2)`, see `CMakeLists.txt:1` |
| Generator | Ninja | The presets declare `generator: "Ninja"` (`CMakePresets.json`); only Ninja is validated by this repository |
| C++ standard | C++17 | Every library sets `target_compile_features(... PUBLIC cxx_std_17)` |
| Compiler | MSVC / Clang / GCC / clang-cl | Clang is auto-detected on Windows and Linux (`cmake/CompilerOptions.cmake:17`); with MSVC only, pass `-DCMAKE_CXX_COMPILER=cl` explicitly |
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
    GIT_TAG        0.3.10
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp-client mcp-server)
```

Prefer a tag (`0.3.10`) over a branch in `GIT_TAG`; use `master` only when you want the development line. Dependencies propagate PUBLIC, so linking `mcp-client` or `mcp-server` is enough — do not hand-write the whole chain.

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

Installation exports `mcpTargets.cmake` with the package version file (`COMPATIBILITY SameMajorVersion`; the version comes from `kSdkVersion` in `include/mcp/McpVersion.hpp`) and a pkg-config description. `find_package` calls `find_dependency(Threads)` for you and `find_dependency(OpenSSL)` when the library was built with OpenSSL (`cmake/mcpConfig.cmake.in`). Exported targets carry the `mcp::` namespace (`CMakeLists.txt:323`).

## Static library inventory

| Target | Responsibility | Depends on |
|---|---|---|
| `mcp-core` | JSON-RPC, types, error codes, logging, spans | — |
| `mcp-transport` | Transports, platform IO, public process API | `mcp-core` |
| `mcp-protocol` | Session engine and dual-era codecs | `mcp-transport` |
| `mcp-server` | Server facade | `mcp-protocol` |
| `mcp-client` | Client facade | `mcp-protocol` |
| `mcp-http` | HTTP server substrate, session and event stores | `mcp-transport` |
| `mcp-ext` | apps, server card, DPoP, triggers extensions | `mcp-core` |

All seven targets are `STATIC` (`CMakeLists.txt:90-97` and the `add_library(mcp-* STATIC)` lines).

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

`MCP_WITH_OPENSSL` has three modes and interacts with `find_package(OpenSSL)` (`cmake/FetchDependencies.cmake:7-27`):

- `AUTO`: enabled when found, disabled otherwise.
- `ON`: configure fails when OpenSSL is missing; no silent downgrade.
- `OFF`: forced off even when installed.

When enabled, the single INTERFACE target `mcp-openssl` defines `MCP_HAVE_OPENSSL` and links `mcp-transport`/`mcp-client`/`mcp-ext`. **Never use `__has_include` to guess whether OpenSSL is available** — a visible header does not mean the macro is defined, let alone that OpenSSL is linked. Versions 3.x and 4.x are supported; an out-of-range major version fails configure. When disabled: TLS is unavailable (`wss` WebSocket, HTTPS SSE), the OAuth PKCE random source falls back to `BCryptGenRandom` (Windows) or `std::random_device` (POSIX), SHA-256 stays built in, and both `private_key_jwt` assertions with a configured `private_key_pem` and DPoP proofs throw `McpError(InternalError)`.

System installation examples: `vcpkg install openssl`, `apt install libssl-dev`, `brew install openssl@3`. In CI pin the major version explicitly; do not use the rolling `brew install openssl` alias.

## Distribution pitfalls

- **`-march=native` is on by default**: it applies when not in CI and not on Apple (flags at `cmake/CompilerOptions.cmake:23-24` and `55-56`), so binaries are not portable across machines. CI mode is decided by the `CI` environment variable (`cmake/Platform.cmake:8-11`). Build release artifacts in CI, or remove the flag yourself.
- **Release enables LTO automatically** (Clang ThinLTO, MSVC LTCG, GCC IPO); slow local Release links are expected.
- **Unity build defaults to ON** for core/server/client/http but OFF for protocol and transport. Check a target's `UNITY_BUILD` before adding a `.cpp` — anonymous namespaces or duplicate static symbols will collide.

## Verification

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Expect: configure prints `[mcp] SDK version: 0.3.10` plus the OpenSSL decision; the build completes without errors; `ctest` passes in full (currently 18 test targets, 982 registered cases on Windows). To verify linking only, build a minimal executable that just includes `<mcp/client/McpClient.hpp>` and links `mcp-client`.

## Next steps

- First server and client: [Quick start](/en/start/quickstart)
- Choosing a transport: [Transports](/en/learn/transports)
- Signatures and fields: [Server reference](/en/reference/server)

## Source files behind this page

- `CMakeLists.txt`, `CMakePresets.json`, `cmake/CompilerOptions.cmake`, `cmake/FetchDependencies.cmake`, `cmake/BuildOptimization.cmake`, `cmake/Lto.cmake`, `cmake/Platform.cmake`, `cmake/mcpConfig.cmake.in`, `include/mcp/McpVersion.hpp`
