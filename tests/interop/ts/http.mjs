import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import { LoggingMessageNotificationSchema } from '@modelcontextprotocol/sdk/types.js';

const base = process.argv[2];
const jsonBase = process.argv[3];
const url = `${base}/mcp`;
const result = { direction: 'ts-client-http', target: url, checks: {} };
const record = (name, ok, detail) => {
  result.checks[name] = { ok: Boolean(ok), detail: String(detail) };
  console.log(`CHECK ${name} ${ok ? 'PASS' : 'FAIL'} ${detail}`);
};

const initBody = JSON.stringify({
  jsonrpc: '2.0', id: 1, method: 'initialize',
  params: { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 't23-http-probe', version: '1.0.0' } }
});

try {
  const jsonResp = await fetch(`${jsonBase}/mcp`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
    body: initBody
  });
  const jsonText = await jsonResp.text();
  record('json_response', jsonResp.headers.get('content-type')?.includes('application/json') && jsonText.includes('protocolVersion'), `status=${jsonResp.status} contentType=${jsonResp.headers.get('content-type')}`);

  const sseResp = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Accept: 'text/event-stream' },
    body: initBody.replace('"id": 1', '"id": 2')
  });
  const sseText = await sseResp.text();
  record('sse_response', sseResp.headers.get('content-type')?.includes('text/event-stream') && sseText.includes('data:'), `status=${sseResp.status} contentType=${sseResp.headers.get('content-type')}`);

  const transport1 = new StreamableHTTPClientTransport(new URL(url));
  const client = new Client({ name: 't23-http-client', version: '1.0.0' }, { capabilities: {} });
  const notifications = [];
  client.setNotificationHandler(LoggingMessageNotificationSchema, msg => notifications.push(msg));
  await client.connect(transport1);
  const tools = await client.listTools();
  record('session_lifecycle', Boolean(transport1.sessionId), `sessionId=${transport1.sessionId ?? 'absent'} tools=${tools.tools.length}`);

  try { await client.callTool({ name: 'test_tool_with_logging', arguments: {} }); } catch {}
  await new Promise(r => setTimeout(r, 300));
  record('notification', notifications.length > 0, `count=${notifications.length}`);

  const page = await client.listResources({ cursor: '0' });
  record('pagination', Array.isArray(page.resources), `count=${page.resources?.length ?? 'n/a'} nextCursor=${page.nextCursor ?? 'none'}`);

  await client.close();

  const transport2 = new StreamableHTTPClientTransport(new URL(url));
  const client2 = new Client({ name: 't23-http-client-reconnected', version: '1.0.0' }, { capabilities: {} });
  await client2.connect(transport2);
  const tools2 = await client2.listTools();
  record('disconnect_reconnect', Boolean(transport2.sessionId) && tools2.tools.length > 0, `sessionId=${transport2.sessionId ?? 'absent'} tools=${tools2.tools.length}`);
  await client2.close();
} catch (err) {
  record('harness', false, `${err?.name ?? 'Error'}: ${String(err?.message ?? err).slice(0, 300)}`);
}
console.log('RESULT_JSON:' + JSON.stringify(result));
