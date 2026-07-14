# What is MCP?

The [Model Context Protocol](https://modelcontextprotocol.io) is an open standard that lets you build servers that expose data and functionality to LLM applications in a secure, standardized way. Think of it like a USB-C port for AI — a universal interface between AI applications and the tools/data they need.

## Core Concepts

**Server** — Provides capabilities (tools, resources, prompts) to connected clients.

**Client** — Connects to servers to discover and invoke their capabilities on behalf of an LLM.

**Transport** — The underlying communication layer (stdio, HTTP, WebSocket) that carries JSON-RPC messages.

## Primitives

| Primitive  | Direction     | Description                                      |
|------------|---------------|--------------------------------------------------|
| Tools      | Client → LLM | Functions the LLM can call to perform actions     |
| Resources  | Client → LLM | Structured data the LLM can read                  |
| Prompts    | Server → LLM | Templates for common interaction patterns         |
| Elicitation| Server → LLM | Request user input (form or URL navigation)       |

## Protocol Versions

The SDK supports two protocol eras:

- **2025-11-25** (legacy): Uses `initialize` handshake, standalone server-to-client requests for sampling/roots/elicitation.
- **2026-07-28** (current): Stateless `server/discover`, per-request `_meta` envelope, MRTR (`InputRequiredResult`), `subscriptions/listen`.
