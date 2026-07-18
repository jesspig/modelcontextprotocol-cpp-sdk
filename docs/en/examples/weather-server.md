# Weather Server Example

A more realistic MCP server that wraps an external weather API.

Source: [`examples/WeatherServer/`](https://github.com/modelcontextprotocol/cpp-sdk/tree/main/examples/WeatherServer)

## Features

- **Tool**: `get_forecast` — fetches weather forecast for a location
- **Tool**: `get_alerts` — fetches weather alerts for a region

## Running

```bash
cmake --preset debug
cmake --build --preset debug
build/debug/examples/WeatherServer/WeatherServer
```

## Key Concepts

The WeatherServer demonstrates:

1. **External API integration** — calling HTTP endpoints from tool handlers
2. **Structured content** — returning JSON data alongside text responses
3. **Error handling** — returning structured errors for invalid locations
