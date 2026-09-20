# Getting SDK Internal Logs

Internal logs of the MCP C++ SDK are disabled by default and can be enabled on demand for troubleshooting.

## Option 1: Environment Variable (Fastest)

Set `MCP_LOG_LEVEL` (0-5, 0=Off, 4=Debug) before launching the process; logs are written straight to `stderr`:

```bash
MCP_LOG_LEVEL=4 ./your_mcp_server
```

Levels: `Off=0` `Error=1` `Warning=2` `Info=3` `Debug=4` `Trace=5`. Use `4` for general debugging.

## Option 2: Programmatic Hook (Integrate with Your Own Logging System)

Capture structured `LogRecord`s through the global hook and forward them to your own logging backend:

```cpp
#include <mcp/Log.hpp>

mcp::SetLogLevel(mcp::LogLevel::Debug);
mcp::SetLogHandler([](const mcp::LogRecord& r) {
    my_logger.log(static_cast<int>(r.level),
                  std::string(r.tag),
                  std::string(r.message));
});
// To restore the default stderr output:
// mcp::SetLogHandler(nullptr);
```

Notes:

- The hook is invoked **synchronously** on the thread emitting the log (including IO threads); the callback must be thread-safe.
- **Do not call `Close()` inside the hook**, otherwise it may self-join and deadlock.
- Exceptions thrown inside the hook are swallowed by the SDK and never propagate.

## Configurable Bind Address

`HttpServer` now supports `HttpServerOptions::bind_host` (and `StreamableHttpServerOptions::host`) to specify the listening address, accepting IPv4/IPv6 literals (e.g. `127.0.0.1` / `::1` / `::`, optionally wrapped in `[]`); an empty string means listening on all IPv4 interfaces only (the default behavior).
