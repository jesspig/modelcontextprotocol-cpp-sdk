---
layout: home

hero:
  name: MCP C++ SDK
  text: Model Context Protocol for C++17
  tagline: Build MCP servers and clients with full protocol support, dual-era WireCodec, and zero-cost abstractions.
  actions:
    - theme: brand
      text: Get Started
      link: /en/guide/quick-start
    - theme: alt
      text: View on GitHub
      link: https://github.com/modelcontextprotocol/cpp-sdk

features:
  - title: Full Protocol
    details: Tools, Resources, Prompts, Elicitation, MRTR, Tasks, Subscriptions — all MCP primitives supported across both protocol eras.
  - title: Dual Transports
    details: Stdio, Streamable HTTP, SSE, WebSocket, and InMemory transports. Easy to extend with custom transports.
  - title: Dual-Era WireCodec
    details: Automatic version negotiation between 2025-11-25 (legacy) and 2026-07-28 (modern) protocol versions.
  - title: C++17 Native
    details: No GC, no heavy frameworks. Uses std::variant and smart pointers for type-safe, zero-cost abstractions.
  - title: Tier 1 Ready
    details: 445 tests (including 116 conformance), OAuth PKCE, persistent storage backends, MessageFilter pipeline.
  - title: Cross-Platform
    details: Windows (MSVC, clang-cl), Linux (GCC, Clang), macOS (Clang). Unified CMake presets for all platforms (debug/release).
---
