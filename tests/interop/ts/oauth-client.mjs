import http from 'node:http';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import { UnauthorizedError } from '@modelcontextprotocol/sdk/client/auth.js';

const url = process.argv[2];
const callbackPort = Number(process.argv[3] ?? 32399);
const result = { direction: 'ts-client-oauth', target: url, checks: {} };
const record = (name, ok, detail) => {
  result.checks[name] = { ok: Boolean(ok), detail: String(detail) };
  console.log(`CHECK ${name} ${ok ? 'PASS' : 'FAIL'} ${detail}`);
};

const callbackState = { code: null, state: null, iss: null };
const callbackServer = http.createServer((req, res) => {
  const parsed = new URL(req.url, `http://127.0.0.1:${callbackPort}`);
  callbackState.code = parsed.searchParams.get('code');
  callbackState.state = parsed.searchParams.get('state');
  callbackState.iss = parsed.searchParams.get('iss');
  res.writeHead(200, { 'Content-Length': '0' });
  res.end();
});
await new Promise(resolve => callbackServer.listen(callbackPort, '127.0.0.1', resolve));

let tokens;
let clientInfo;
let verifier;
const provider = {
  get redirectUrl() { return `http://127.0.0.1:${callbackPort}/callback`; },
  get clientMetadata() {
    return { redirect_uris: [this.redirectUrl], client_name: 't23-official-ts-oauth-client', grant_types: ['authorization_code'], response_types: ['code'] };
  },
  clientInformation() { return clientInfo; },
  saveClientInformation(info) { clientInfo = info; },
  tokens() { return tokens; },
  saveTokens(next) { tokens = next; },
  redirectToAuthorization(authUrl) {
    fetch(authUrl).catch(() => {});
  },
  saveCodeVerifier(next) { verifier = next; },
  codeVerifier() { return verifier; },
};

const client = new Client({ name: 't23-official-ts-oauth-client', version: '1.0.0' }, { capabilities: {} });
let transport;

try {
  let connected = false;
  for (let attempt = 0; attempt < 3 && !connected; attempt++) {
    transport = new StreamableHTTPClientTransport(new URL(url), { authProvider: provider });
    try {
      await client.connect(transport);
      connected = true;
    } catch (err) {
      if (!(err instanceof UnauthorizedError)) throw err;
      for (let i = 0; i < 60 && !callbackState.code; i++) await new Promise(r => setTimeout(r, 100));
      if (!callbackState.code) throw new Error('authorization callback did not yield a code');
      await transport.finishAuth(callbackState.code);
    }
  }
  record('authenticated_connect', connected, `connected=${connected}`);
  record('pkce_verifier', Boolean(verifier && verifier.length >= 43), `verifierLength=${verifier?.length ?? 0}`);
  record('authorization_code', Boolean(callbackState.code), `code=${callbackState.code ? 'present' : 'absent'} state=${callbackState.state ? 'present' : 'absent'} iss=${callbackState.iss ?? 'n/a'}`);
  record('token_acquired', Boolean(tokens?.access_token), `tokenType=${tokens?.token_type ?? 'n/a'} scope=${tokens?.scope ?? 'n/a'}`);

  const tools = await client.listTools();
  record('bearer_tools_list', tools.tools.length > 0, `count=${tools.tools.length}`);
  const call = await client.callTool({ name: 'test_simple_text', arguments: {} });
  const text = (call.content ?? []).map(c => c.text ?? '').join('');
  record('bearer_tools_call', Boolean(text), `text=${text.slice(0, 100)}`);
} catch (err) {
  record('harness', false, `${err?.name ?? 'Error'}: ${String(err?.message ?? err).slice(0, 300)}`);
} finally {
  try { await client.close(); } catch {}
  callbackServer.close();
  console.log('RESULT_JSON:' + JSON.stringify(result));
}
