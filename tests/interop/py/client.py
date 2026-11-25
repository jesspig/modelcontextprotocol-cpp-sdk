import asyncio
import json
import sys

from mcp import ClientSession
from mcp.client.streamable_http import (
    create_mcp_http_client,
    streamable_http_client as streamablehttp_client,
)


def describe(err):
    detail = f"{type(err).__name__}: {str(err)[:240]}"
    sub = getattr(err, "exceptions", None)
    if sub:
        detail += " || " + " ; ".join(
            f"{type(x).__name__}: {str(x)[:200]}" for x in sub
        )
    return detail


async def run_session(read, write, captured, record):
    try:
        async with ClientSession(read, write) as session:
            init = await session.initialize()
            record("initialize", bool(init), f"protocol={getattr(init, 'protocol_version', 'n/a')} server={getattr(getattr(init, 'server_info', None), 'name', 'n/a')}")

            tools = await session.list_tools()
            record("tools_list", len(tools.tools) > 0, f"count={len(tools.tools)}")

            call = await session.call_tool("test_simple_text", {})
            text = "".join(getattr(c, "text", "") or "" for c in call.content)
            record("tools_call", bool(text), f"text={text[:120]}")

            resources = await session.list_resources()
            record("resources_list", len(resources.resources) > 0, f"count={len(resources.resources)}")
            read_res = await session.read_resource("test://static-text")
            record("resources_read", len(read_res.contents) > 0, f"blocks={len(read_res.contents)}")

            prompts = await session.list_prompts()
            record("prompts_list", len(prompts.prompts) > 0, f"count={len(prompts.prompts)}")
            prompt = await session.get_prompt("test_simple_prompt", {})
            record("prompts_get", len(prompt.messages) > 0, f"messages={len(prompt.messages)}")

            try:
                await session.call_tool("definitely_missing_tool", {})
                record("error_mapping", False, "missing tool unexpectedly succeeded")
            except Exception as err:
                code = getattr(getattr(err, "error", None), "code", "n/a")
                record("error_mapping", True, f"code={code} message={str(err)[:160]}")

            sid = captured.get("session_id")
            record("session_header", bool(sid), f"sessionId={'present' if sid else 'absent'}")
    except BaseException as err:
        record("harness", False, describe(err))


async def main(url):
    result = {"direction": "python-client", "target": url, "checks": {}}

    def record(name, ok, detail):
        result["checks"][name] = {"ok": bool(ok), "detail": str(detail)}
        print(f"CHECK {name} {'PASS' if ok else 'FAIL'} {detail}")

    captured = {}
    http_client = create_mcp_http_client()

    async def capture_response(response):
        sid = response.headers.get("mcp-session-id")
        if sid:
            captured["session_id"] = sid

    http_client.event_hooks["response"].append(capture_response)

    try:
        async with streamablehttp_client(url, http_client=http_client) as (read, write):
            await run_session(read, write, captured, record)
    except BaseException as err:
        record("harness", False, describe(err))
    print("RESULT_JSON:" + json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1]))
