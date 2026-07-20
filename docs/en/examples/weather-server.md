# Weather Server Example

A more realistic MCP server that demonstrates multi-tool registration with simulated weather data.

Source: [`examples/WeatherServer/`](https://github.com/modelcontextprotocol/cpp-sdk/tree/main/examples/WeatherServer)

## Features

- **Tool**: `get_alerts` — returns simulated weather alerts for a US state
- **Tool**: `get_forecast` — returns simulated weather forecast with location parameters

## Running

```bash
cmake --preset debug
cmake --build --preset debug
build/debug/examples/WeatherServer/WeatherServer
```

## Key Concepts

The WeatherServer demonstrates:

1. **Multiple tool registration** — registering two distinct tools (`get_alerts`, `get_forecast`) with different signatures and logic
2. **Tool parameters** — accessing `arguments` from `CallToolRequestParams` to read input values (`state`, `latitude`, `longitude`)
3. **Text-based responses** — returning `TextContent` results from tool handlers
4. **Default fallback** — handling missing data with a default response instead of structured errors
