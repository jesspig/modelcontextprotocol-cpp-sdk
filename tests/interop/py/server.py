import os
import sys

import uvicorn

from mcp.server.mcpserver import MCPServer


server = MCPServer("t23-official-python-server", version="1.0.0")


@server.tool()
async def test_simple_text() -> str:
    return "hello from official-python-server"


@server.tool()
async def add_numbers(a: int, b: int) -> str:
    return str(a + b)


@server.resource("test://static-text")
async def static_text() -> str:
    return "static text from official-python-server"


@server.prompt()
async def test_simple_prompt() -> str:
    return "prompt from official-python-server"


app = server.streamable_http_app(
    streamable_http_path="/mcp",
    json_response=os.environ.get("MCP_JSON_RESPONSE") == "1",
)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 32211
    print(f"MCP official Python server listening on {port}", file=sys.stderr, flush=True)
    uvicorn.run(app, host="127.0.0.1", port=port, log_level="warning")
