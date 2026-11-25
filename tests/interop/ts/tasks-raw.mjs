import { z } from 'zod';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';

const url = process.argv[2];
const result = { direction: 'ts-client-tasks-raw', target: url, checks: {} };
const record = (name, ok, detail) => {
  result.checks[name] = { ok: Boolean(ok), detail: String(detail) };
  console.log(`CHECK ${name} ${ok ? 'PASS' : 'FAIL'} ${detail}`);
};

const transport = new StreamableHTTPClientTransport(new URL(url));
const client = new Client({ name: 't23-official-ts-tasks-raw', version: '1.0.0' }, { capabilities: {} });
const raw = request => client.request(request, z.any());

try {
  await client.connect(transport);

  const created = await raw({ method: 'tools/call', params: { name: 'slow_compute', arguments: { seconds: 1, label: 't23' }, task: { ttl: 60000 } } });
  const taskId = created?.task?.taskId ?? created?.taskId ?? null;
  record('task_create', Boolean(taskId), `taskId=${taskId ?? 'none'} status=${created?.task?.status ?? created?.status ?? 'n/a'} keys=${Object.keys(created ?? {}).join(',')}`);
  record('task_id_propagation', Boolean(created?.task?.taskId && created?.taskId === created?.task?.taskId), `top=${created?.taskId ?? 'n/a'} nested=${created?.task?.taskId ?? 'n/a'}`);

  let status = null;
  for (let i = 0; i < 15; i++) {
    const got = await raw({ method: 'tasks/get', params: { taskId } });
    status = got?.status ?? null;
    if (status && status !== 'working' && status !== 'input_required') break;
    await new Promise(resolve => setTimeout(resolve, 200));
  }
  record('task_get', Boolean(status), `status=${status ?? 'n/a'}`);
  record('task_terminal', status === 'completed', `status=${status ?? 'n/a'}`);

  const listed = await raw({ method: 'tasks/list', params: {} });
  record('task_list', Array.isArray(listed?.tasks), `count=${listed?.tasks?.length ?? 'n/a'}`);

  const payload = await raw({ method: 'tasks/result', params: { taskId } });
  record('task_result', Boolean(payload), `keys=${Object.keys(payload ?? {}).join(',')}`);

  const cancelled = await raw({ method: 'tasks/cancel', params: { taskId } });
  record('task_cancel', Boolean(cancelled), `status=${cancelled?.status ?? 'n/a'}`);
} catch (err) {
  record('harness', false, `${err?.name ?? 'Error'}: ${String(err?.message ?? err).slice(0, 300)}`);
} finally {
  try { await client.close(); } catch {}
  console.log('RESULT_JSON:' + JSON.stringify(result));
}
