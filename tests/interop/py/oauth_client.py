import asyncio
import json
import sys
import threading
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from mcp import ClientSession
from mcp.client.auth import OAuthClientProvider
from mcp.client.streamable_http import create_mcp_http_client, streamable_http_client
from mcp.shared.auth import AuthorizationCodeResult, OAuthClientMetadata


class MemoryStorage:
    def __init__(self):
        self.tokens = None
        self.client_info = None

    async def get_tokens(self):
        return self.tokens

    async def set_tokens(self, tokens):
        self.tokens = tokens

    async def get_client_info(self):
        return self.client_info

    async def set_client_info(self, info):
        self.client_info = info


class Callback(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    state = {}

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        Callback.state["code"] = params.get("code", [None])[0]
        Callback.state["state"] = params.get("state", [None])[0]
        Callback.state["iss"] = params.get("iss", [None])[0]
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


def describe(err):
    detail = f"{type(err).__name__}: {str(err)[:240]}"
    sub = getattr(err, "exceptions", None)
    if sub:
        detail += " || " + " ; ".join(f"{type(x).__name__}: {str(x)[:200]}" for x in sub)
    return detail


async def main(url, callback_port):
    result = {"direction": "python-client-oauth", "target": url, "checks": {}}

    def record(name, ok, detail):
        result["checks"][name] = {"ok": bool(ok), "detail": str(detail)}
        print(f"CHECK {name} {'PASS' if ok else 'FAIL'} {detail}", flush=True)

    server = ThreadingHTTPServer(("127.0.0.1", callback_port), Callback)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    storage = MemoryStorage()
    redirect_uri = f"http://127.0.0.1:{callback_port}/callback"
    metadata = OAuthClientMetadata(
        redirect_uris=[redirect_uri],
        client_name="t23-official-python-oauth-client",
        token_endpoint_auth_method="client_secret_post",
    )

    async def redirect_handler(auth_url):
        await asyncio.to_thread(lambda: urllib.request.urlopen(auth_url, timeout=10).read())

    async def callback_handler():
        for _ in range(80):
            if Callback.state.get("code"):
                return AuthorizationCodeResult(
                    code=Callback.state["code"],
                    state=Callback.state.get("state"),
                    iss=Callback.state.get("iss"),
                )
            await asyncio.sleep(0.1)
        raise RuntimeError("authorization callback did not yield a code")

    provider = OAuthClientProvider(
        server_url=url,
        client_metadata=metadata,
        storage=storage,
        redirect_handler=redirect_handler,
        callback_handler=callback_handler,
    )

    try:
        http_client = create_mcp_http_client(auth=provider)
        async with streamable_http_client(url, http_client=http_client) as (read, write):
            async with ClientSession(read, write) as session:
                init = await session.initialize()
                record("authenticated_connect", bool(init), f"protocol={getattr(init, 'protocol_version', 'n/a')}")
                record("token_acquired", bool(storage.tokens and getattr(storage.tokens, "access_token", None)), f"tokenType={getattr(storage.tokens, 'token_type', 'n/a')}")
                record("authorization_code", bool(Callback.state.get("code")), f"code={'present' if Callback.state.get('code') else 'absent'} iss={Callback.state.get('iss')}")
                tools = await session.list_tools()
                record("bearer_tools_list", len(tools.tools) > 0, f"count={len(tools.tools)}")
                call = await session.call_tool("test_simple_text", {})
                text = "".join(getattr(c, "text", "") or "" for c in call.content)
                record("bearer_tools_call", bool(text), f"text={text[:100]}")
    except BaseException as err:
        record("harness", False, describe(err))
    server.shutdown()
    print("RESULT_JSON:" + json.dumps(result, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 32398))
