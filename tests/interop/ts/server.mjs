import http from 'node:http';
import { randomUUID } from 'node:crypto';
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StreamableHTTPServerTransport } from '@modelcontextprotocol/sdk/server/streamableHttp.js';
import { isInitializeRequest } from '@modelcontextprotocol/sdk/types.js';
import { z } from 'zod';

const port = Number(process.argv[2] ?? 32210);
const sessions = new Map();

function wire(tag, obj) {
  const safe = JSON.stringify(obj).replace(/(Authorization|Cookie|Set-Cookie)"\s*:\s*"[^"]*"/gi, '"$1":"<REDACTED>"');
  console.error(`WIRE ${tag} ${safe}`);
}

function buildServer() {
  const server = new McpServer({ name: 't23-official-ts-server', version: '1.0.0' });
  server.registerTool('test_simple_text', { description: 'Simple text tool for interop' }, async () => ({
    content: [{ type: 'text', text: 'hello from official-ts-server' }]
  }));
  server.registerTool('add_numbers', { description: 'Adds two numbers', inputSchema: { a: z.number(), b: z.number() } }, async (args) => ({
    content: [{ type: 'text', text: String(Number(args?.a ?? 0) + Number(args?.b ?? 0)) }]
  }));
  server.registerTool('error_tool', { description: 'Always returns an error result' }, async () => ({
    content: [{ type: 'text', text: 'intentional tool error' }], isError: true
  }));
  server.registerTool('strict_region', { description: 'Rejects invalid region values', inputSchema: { region: z.enum(['us-east1']) } }, async ({ region }) => ({
    content: [{ type: 'text', text: region }]
  }));
  server.registerResource('static-text', 'test://static-text', { description: 'Static text resource' }, async () => ({
    contents: [{ uri: 'test://static-text', text: 'static text from official-ts-server', mimeType: 'text/plain' }]
  }));
  server.registerPrompt('test_simple_prompt', { description: 'Simple prompt' }, async () => ({
    messages: [{ role: 'user', content: { type: 'text', text: 'prompt from official-ts-server' } }]
  }));
  return server;
}

const serverHandler = async (req, res) => {
  try {
    let body;
    if (req.method === 'POST') {
      const chunks = [];
      for await (const chunk of req) chunks.push(chunk);
      body = chunks.length ? JSON.parse(Buffer.concat(chunks).toString()) : undefined;
      wire('POST', { url: req.url, headers: { 'mcp-session-id': req.headers['mcp-session-id'], accept: req.headers.accept }, body });
    } else {
      wire(req.method, { url: req.url, headers: { 'mcp-session-id': req.headers['mcp-session-id'] } });
    }
    const sessionId = req.headers['mcp-session-id'];
    let transport;
    if (sessionId && sessions.has(sessionId)) {
      transport = sessions.get(sessionId).transport;
    } else if (!sessionId && body && isInitializeRequest(body)) {
      transport = new StreamableHTTPServerTransport({
        sessionIdGenerator: () => randomUUID(),
        onsessioninitialized: sid => {
          wire('SESSION_INIT', { sessionId: sid });
          const server = buildServer();
          sessions.set(sid, { transport, server });
          server.connect(transport);
        }
      });
    } else {
      res.statusCode = 400;
      res.setHeader('Content-Type', 'application/json');
      res.end(JSON.stringify({ jsonrpc: '2.0', error: { code: -32000, message: 'Bad Request: invalid session' }, id: null }));
      return;
    }
    await transport.handleRequest(req, res, body);
  } catch (err) {
    wire('ERROR', { message: String(err?.message ?? err) });
    if (!res.headersSent) {
      res.statusCode = 500;
      res.setHeader('Content-Type', 'application/json');
      res.end(JSON.stringify({ jsonrpc: '2.0', error: { code: -32603, message: 'Internal server error' }, id: null }));
    }
  }
};

const httpServer = http.createServer(serverHandler);
httpServer.listen(port, '127.0.0.1', () => console.error(`MCP official TS server listening on ${port}`));

process.on('SIGTERM', async () => {
  for (const { transport } of sessions.values()) { try { await transport.close(); } catch {} }
  httpServer.close(() => process.exit(0));
});
