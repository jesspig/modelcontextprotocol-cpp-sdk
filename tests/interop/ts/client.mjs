import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';

const url = process.argv[2];
const result = { direction: 'ts-client', target: url, checks: {} };

function record(name, ok, detail) {
  result.checks[name] = { ok, detail };
  console.log(`CHECK ${name} ${ok ? 'PASS' : 'FAIL'} ${detail}`);
}

const transport = new StreamableHTTPClientTransport(new URL(url));
const client = new Client({ name: 't23-official-ts-client', version: '1.0.0' }, { capabilities: {} });

try {
  await client.connect(transport);
  const version = client.getServerVersion();
  record('initialize', !!version, `server=${version?.name ?? 'n/a'} capabilities=${JSON.stringify(client.getServerCapabilities())}`);

  const tools = await client.listTools();
  record('tools_list', tools.tools.length > 0, `count=${tools.tools.length}`);

  const call = await client.callTool({ name: 'test_simple_text', arguments: {} });
  const text = (call.content ?? []).filter(c => c.type === 'text').map(c => c.text).join('|');
  record('tools_call', text.length > 0, `text=${text.slice(0, 120)}`);

  const resources = await client.listResources();
  record('resources_list', resources.resources.length > 0, `count=${resources.resources.length}`);
  const read = await client.readResource({ uri: 'test://static-text' });
  record('resources_read', read.contents.length > 0, `blocks=${read.contents.length}`);

  const prompts = await client.listPrompts();
  record('prompts_list', prompts.prompts.length > 0, `count=${prompts.prompts.length}`);
  const prompt = await client.getPrompt({ name: 'test_simple_prompt', arguments: {} });
  record('prompts_get', (prompt.messages ?? []).length > 0, `messages=${(prompt.messages ?? []).length}`);

  try {
    await client.callTool({ name: 'definitely_missing_tool', arguments: {} });
    record('error_mapping', false, 'missing tool unexpectedly succeeded');
  } catch (err) {
    record('error_mapping', true, `code=${err?.code ?? 'n/a'} message=${String(err?.message ?? err).slice(0, 160)}`);
  }

  const sessionId = transport.sessionId ?? null;
  record('session_header', !!sessionId, `sessionId=${sessionId ? 'present' : 'absent'}`);
} catch (err) {
  record('harness', false, `${err?.name ?? 'Error'}: ${String(err?.message ?? err).slice(0, 240)}`);
} finally {
  try { await client.close(); } catch (err) { record('close', false, String(err?.message ?? err)); }
  console.log('RESULT_JSON:' + JSON.stringify(result));
}
