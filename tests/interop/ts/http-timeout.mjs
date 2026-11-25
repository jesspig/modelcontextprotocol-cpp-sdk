const url = process.argv[2];
let ok = false;
let detail;
try {
  await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
    body: '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}',
    signal: AbortSignal.timeout(500)
  });
  ok = false;
  detail = 'request unexpectedly completed';
} catch (err) {
  ok = err?.name === 'TimeoutError' || String(err?.message ?? '').toLowerCase().includes('timeout');
  detail = `${err?.name ?? 'Error'}: ${String(err?.message ?? err).slice(0, 160)}`;
}
console.log(`CHECK timeout ${ok ? 'PASS' : 'FAIL'} ${detail}`);
console.log('RESULT_JSON:' + JSON.stringify({ direction: 'ts-client-http-timeout', ok, detail }));
process.exit(ok ? 0 : 1);
