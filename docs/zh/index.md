---
layout: home

hero:
  name: MCP C++ SDK
  text: C++17 实现的模型上下文协议
  tagline: 构建 MCP 服务器和客户端，支持完整协议、双时代 WireCodec 和零成本抽象。
  actions:
    - theme: brand
      text: 快速开始
      link: /zh/guide/quick-start
    - theme: alt
      text: 在 GitHub 查看
      link: https://github.com/modelcontextprotocol/cpp-sdk

features:
  - title: 完整协议
    details: 工具、资源、提示词、诱导、MRTR、任务、订阅——所有 MCP 原语在两个协议时代均受支持。
  - title: 双传输层
    details: Stdio、Streamable HTTP、SSE、WebSocket 和 InMemory 传输。易于扩展为自定义传输。
  - title: 双时代 WireCodec
    details: 在 2025-11-25（旧版）和 2026-07-28（现代）协议版本之间自动版本协商。
  - title: C++17 原生
    details: 无 GC，无臃肿框架。使用 std::variant、智能指针、asio 和 nlohmann-json 实现类型安全、零成本抽象。
  - title: 一级就绪
    details: 216 个测试、122 个一致性测试、OAuth PKCE、持久化存储后端、MessageFilter 管道。
  - title: 跨平台
    details: Windows（MSVC、clang-cl）、Linux（GCC、Clang）、macOS（Clang）。所有平台使用单一 CMake 预设。
---
