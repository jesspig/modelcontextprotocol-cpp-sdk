---
layout: home

hero:
  name: MCP C++ SDK
  text: The Model Context Protocol in C++17
  tagline: "A complete manual from build to debugging — tools, resources, prompts, elicitation, tasks, subscriptions, across both protocol eras."
  actions:
    - theme: brand
      text: Five-minute start
      link: /en/start/quickstart
    - theme: alt
      text: Installation
      link: /en/start/installation
    - theme: alt
      text: View on GitHub
      link: https://github.com/jesspig/modelcontextprotocol-cpp-sdk

features:
  - title: I want to expose a tool
    details: "The full registration surface — description, title, input schema, behaviour annotations, icons — and the obligation behind structured output."
    link: /en/howto/register-a-tool
  - title: I want bad arguments blocked
    details: "Declare inputSchema and the SDK validates for you, answering is_error with invalid arguments; plus the hand-written defensive style."
    link: /en/howto/validate-tool-input
  - title: I want to serve resources and prompts
    details: "Static resources, URI templates, subscriptions and list_changed, plus prompts with argument specs and the completion callback."
    link: /en/howto/serve-a-resource
  - title: I need to ask the user
    details: "Server-initiated elicitation in form and url modes, session addressing and the three terminal actions."
    link: /en/howto/collect-user-input
  - title: I have long-running work
    details: "Task-backed execution, progress reporting and protocol logging — task_store, era-aware routing, pollable results."
    link: /en/howto/run-long-tasks
  - title: I want an HTTP server
    details: "Assembling Streamable HTTP, session and event stores, stateless mode and resumption after disconnect."
    link: /en/howto/run-a-streamable-http-server
  - title: I want to put it on the internet
    details: "Bearer verification, challenge frames and the metadata endpoint, DPoP, Host/Origin allowlists and size limits."
    link: /en/howto/protect-an-http-server
  - title: I want to connect to a server
    details: "Client assembly over the five transports, connect modes and negotiation, close timing and in-flight settlement."
    link: /en/howto/connect-to-a-server
  - title: My client needs auth
    details: "OAuth authorization code with PKCE, client credentials, token caching and automatic renewal, DPoP proof wiring."
    link: /en/howto/add-oauth-to-a-client
  - title: My request is stuck
    details: "The real semantics of cancellation and timeouts, error-code typing and remapping, and how to order your debugging."
    link: /en/howto/cancel-a-request
  - title: I need the two eras explained
    details: "The method and notification matrix across eras, error-code remapping and negotiation fallback — the single authoritative page."
    link: /en/learn/protocol-versions
  - title: I just want signatures
    details: "The reference layer is tables only: methods, every Options field, defaults, throws and the matching source files."
    link: /en/reference/
---
