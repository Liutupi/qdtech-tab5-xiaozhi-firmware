// Tab5 inbox relay: Muse (cloud agent) pushes messages over MCP, Tab5 polls them over the LAN.
//
//   Muse  --HTTPS (Cloudflare quick tunnel)-->  POST /mcp/<token>   (MCP streamable HTTP, JSON replies)
//   Tab5  --LAN HTTP----------------------->   GET  /tab5/inbox    (rejected when it arrives via the tunnel)
//
// No npm dependencies (Node 18+). State lives in DATA_DIR (mount a host folder there).
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const PORT = Number(process.env.PORT || 8787);
const DATA_DIR = process.env.DATA_DIR || '/data';
const INBOX_FILE = path.join(DATA_DIR, 'inbox.json');
const TOKEN_FILE = path.join(DATA_DIR, 'token.txt');
const TUNNEL_LOG = process.env.TUNNEL_LOG || path.join(DATA_DIR, 'tunnel.log');
const MAX_MESSAGES = 50;
const MAX_TITLE = 40;
const MAX_BODY = 600;
const MAX_REQUEST_BYTES = 64 * 1024;

fs.mkdirSync(DATA_DIR, { recursive: true });

function loadToken() {
  if (process.env.MUSE_TOKEN) return process.env.MUSE_TOKEN.trim();
  try {
    const t = fs.readFileSync(TOKEN_FILE, 'utf8').trim();
    if (t.length >= 16) return t;
  } catch {}
  const t = crypto.randomBytes(18).toString('base64url');
  fs.writeFileSync(TOKEN_FILE, t + '\n', { mode: 0o600 });
  return t;
}
const TOKEN = loadToken();
const DISCOVERY_TOPIC = 'tab5muse-' + crypto.createHash('sha256').update(TOKEN).digest('hex').slice(0, 20);

function loadInbox() {
  try {
    const data = JSON.parse(fs.readFileSync(INBOX_FILE, 'utf8'));
    if (Array.isArray(data.messages) && Number.isInteger(data.next_id)) return data;
  } catch {}
  return { next_id: 1, messages: [] };
}
let inbox = loadInbox();

function saveInbox() {
  const tmp = INBOX_FILE + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(inbox, null, 1));
  fs.renameSync(tmp, INBOX_FILE);
}

function clip(text, max) {
  const chars = Array.from(String(text ?? '').replace(/\r/g, '').trim());
  return chars.length > max ? chars.slice(0, max - 1).join('') + '…' : chars.join('');
}

function localTime(date = new Date()) {
  // Asia/Shanghai, formatted YYYY-MM-DD HH:MM without depending on the container TZ.
  const t = new Date(date.getTime() + 8 * 3600 * 1000);
  const p = (n) => String(n).padStart(2, '0');
  return `${t.getUTCFullYear()}-${p(t.getUTCMonth() + 1)}-${p(t.getUTCDate())} ${p(t.getUTCHours())}:${p(t.getUTCMinutes())}`;
}

function pushMessage({ title, body, from }) {
  const msg = {
    id: inbox.next_id++,
    title: clip(title, MAX_TITLE),
    body: clip(body, MAX_BODY),
    from: clip(from || 'Muse', 16),
    time: localTime(),
  };
  if (!msg.title && !msg.body) throw new Error('title or body is required');
  if (!msg.title) msg.title = clip(msg.body, 18);
  inbox.messages.push(msg);
  if (inbox.messages.length > MAX_MESSAGES) inbox.messages = inbox.messages.slice(-MAX_MESSAGES);
  saveInbox();
  console.log('push', JSON.stringify({ id: msg.id, title: msg.title, from: msg.from, bytes: msg.body.length }));
  return msg;
}

function tunnelUrl() {
  try {
    const text = fs.readFileSync(TUNNEL_LOG, 'utf8');
    const all = text.match(/https:\/\/[a-z0-9-]+\.trycloudflare\.com/g);
    return all ? all[all.length - 1] : '';
  } catch {
    return '';
  }
}

// ---------------------------------------------------------------- MCP (JSON-RPC)
const TOOLS = [
  {
    name: 'tab5_push',
    description:
      'Push a short message to the user\'s Tab5 desk display (M5Stack Tab5, Chinese UI). ' +
      'It appears in the Tab5 "Muse" inbox app and can be read aloud on request. ' +
      'Write in Simplified Chinese unless asked otherwise. Keep the title short (<= 20 characters is best, max 40) ' +
      'and the body concise (plain text, max 600 characters, no Markdown tables or links).',
    inputSchema: {
      type: 'object',
      properties: {
        title: { type: 'string', description: 'Short headline, e.g. "今日文献速览".' },
        body: { type: 'string', description: 'Plain-text content, up to 600 characters. Use line breaks for lists.' },
        from: { type: 'string', description: 'Optional sender label shown on Tab5 (default "Muse").' },
      },
      required: ['title', 'body'],
    },
  },
  {
    name: 'tab5_inbox_list',
    description: 'List the most recent messages already pushed to the Tab5 inbox (to avoid duplicates).',
    inputSchema: {
      type: 'object',
      properties: { limit: { type: 'integer', minimum: 1, maximum: 20, description: 'Default 5.' } },
    },
  },
];

function toolResult(text, isError = false) {
  return { content: [{ type: 'text', text }], isError };
}

function callTool(name, args = {}) {
  if (name === 'tab5_push') {
    const msg = pushMessage(args);
    return toolResult(`已推送到 Tab5 收件箱 (#${msg.id} ${msg.time})：${msg.title}`);
  }
  if (name === 'tab5_inbox_list') {
    const limit = Math.min(20, Math.max(1, Number(args.limit) || 5));
    const recent = inbox.messages.slice(-limit).reverse();
    return toolResult(
      recent.length
        ? recent.map((m) => `#${m.id} ${m.time} [${m.from}] ${m.title}\n${m.body}`).join('\n\n')
        : 'Tab5 收件箱还没有消息。'
    );
  }
  return null;
}

function handleRpc(msg) {
  if (!msg || msg.jsonrpc !== '2.0' || typeof msg.method !== 'string') {
    return { jsonrpc: '2.0', id: msg?.id ?? null, error: { code: -32600, message: 'Invalid Request' } };
  }
  const isNotification = msg.id === undefined || msg.id === null;
  const reply = (result) => (isNotification ? null : { jsonrpc: '2.0', id: msg.id, result });
  const fail = (code, message) => (isNotification ? null : { jsonrpc: '2.0', id: msg.id, error: { code, message } });
  switch (msg.method) {
    case 'initialize':
      return reply({
        protocolVersion: msg.params?.protocolVersion || '2025-06-18',
        capabilities: { tools: { listChanged: false } },
        serverInfo: { name: 'tab5-inbox', version: '1.0.0' },
        instructions:
          'Use tab5_push to send the user short, useful updates (Chinese) to their Tab5 desk display. ' +
          'Check tab5_inbox_list first when running a recurring task so the same item is not sent twice.',
      });
    case 'ping':
      return reply({});
    case 'tools/list':
      return reply({ tools: TOOLS });
    case 'tools/call': {
      const name = msg.params?.name;
      try {
        const result = callTool(name, msg.params?.arguments || {});
        return result ? reply(result) : fail(-32602, `Unknown tool: ${name}`);
      } catch (e) {
        return reply(toolResult(`推送失败：${e.message}`, true));
      }
    }
    case 'resources/list':
      return reply({ resources: [] });
    case 'prompts/list':
      return reply({ prompts: [] });
    default:
      if (msg.method.startsWith('notifications/')) return null;
      return fail(-32601, `Method not found: ${msg.method}`);
  }
}

function inboxResponse(url) {
  const since = Number(url.searchParams.get('since') || 0);
  const limit = Math.min(20, Math.max(1, Number(url.searchParams.get('limit')) || 12));
  const messages = inbox.messages.filter((m) => m.id > since).slice(-limit).reverse();
  const base = tunnelUrl();
  return {
    latest_id: inbox.next_id - 1,
    messages,
    tunnel: base,
    mcp_url: base ? `${base}/mcp/${TOKEN}` : '',
    discovery_topic: DISCOVERY_TOPIC,
  };
}

// ---------------------------------------------------------------- HTTP
function send(res, status, body, headers = {}) {
  const payload = typeof body === 'string' ? body : JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': typeof body === 'string' ? 'text/plain; charset=utf-8' : 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
    ...headers,
  });
  res.end(payload);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (c) => {
      size += c.length;
      if (size > MAX_REQUEST_BYTES) {
        reject(new Error('too large'));
        req.destroy();
      } else chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

function tokenOk(candidate) {
  const a = Buffer.from(String(candidate || ''));
  const b = Buffer.from(TOKEN);
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

// Requests that come through Cloudflare carry these headers; the Tab5 LAN endpoints must not be public.
function viaTunnel(req) {
  return Boolean(req.headers['cf-connecting-ip'] || req.headers['cf-ray']);
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const parts = url.pathname.split('/').filter(Boolean);
  try {
    if (url.pathname === '/health') return send(res, 200, { ok: true, messages: inbox.messages.length });

    if (parts[0] === 'mcp') {
      if (!tokenOk(parts[1])) return send(res, 404, 'not found');
      if (req.method === 'GET') return send(res, 405, 'SSE stream not supported', { Allow: 'POST, DELETE' });
      if (req.method === 'DELETE') return send(res, 200, {});
      if (req.method !== 'POST') return send(res, 405, 'method not allowed', { Allow: 'POST' });
      let parsed;
      try {
        parsed = JSON.parse(await readBody(req));
      } catch {
        return send(res, 400, { jsonrpc: '2.0', id: null, error: { code: -32700, message: 'Parse error' } });
      }
      const batch = Array.isArray(parsed);
      const replies = (batch ? parsed : [parsed]).map(handleRpc).filter(Boolean);
      const headers = {};
      if (!batch && parsed?.method === 'initialize') headers['Mcp-Session-Id'] = crypto.randomUUID();
      if (replies.length === 0) {
        res.writeHead(202);
        return res.end();
      }
      return send(res, 200, batch ? replies : replies[0], headers);
    }

    // Plain HTTP push (same token): POST /push/<token> {"title","body","from"}
    if (parts[0] === 'push' && req.method === 'POST') {
      if (!tokenOk(parts[1])) return send(res, 404, 'not found');
      const msg = pushMessage(JSON.parse(await readBody(req)));
      return send(res, 200, { ok: true, message: msg });
    }

    // Same inbox feed for a Tab5 that can only reach the NAS through the public tunnel.
    if (parts[0] === 'inbox' && req.method === 'GET') {
      if (!tokenOk(parts[1])) return send(res, 404, 'not found');
      return send(res, 200, inboxResponse(url));
    }

    if (parts[0] === 'tab5') {
      if (viaTunnel(req)) return send(res, 404, 'not found');
      if (parts[1] === 'inbox' && req.method === 'GET') {
        return send(res, 200, inboxResponse(url));
      }
      if (parts[1] === 'info' && req.method === 'GET') {
        const base = tunnelUrl();
        return send(res, 200, { tunnel: base, mcp_url: base ? `${base}/mcp/${TOKEN}` : '', messages: inbox.messages.length });
      }
    }
    return send(res, 404, 'not found');
  } catch (e) {
    console.error('request failed', req.method, url.pathname, e.message);
    return send(res, 500, { ok: false, error: e.message });
  }
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`tab5 inbox relay listening on :${PORT}, ${inbox.messages.length} stored message(s)`);
  console.log(`MCP path: /mcp/${TOKEN}`);
});

// Print the full public MCP URL once the tunnel reports it.
// A quick-tunnel hostname changes whenever cloudflared restarts. Publish only the hostname
// (never the token) to an ntfy.sh topic derived from the token, so a Tab5 that already knows
// the token can find the relay again without any manual step.
let announced = '';
let published = '';
async function publishHost(base) {
  const host = base.replace(/^https:\/\//, '');
  try {
    const r = await fetch(`https://ntfy.sh/${DISCOVERY_TOPIC}`, { method: 'POST', body: host });
    if (r.ok) {
      published = base;
      console.log('discovery published', host);
    } else console.log('discovery publish failed', r.status);
  } catch (e) {
    console.log('discovery publish failed', e.message);
  }
}
setInterval(() => {
  const base = tunnelUrl();
  if (base && base !== announced) {
    announced = base;
    console.log(`PUBLIC MCP URL (give this to Muse): ${base}/mcp/${TOKEN}`);
    console.log(`Tab5 inbox URL: ${base}/inbox/${TOKEN}`);
  }
  if (base && base !== published) publishHost(base);
}, 5000).unref();
// Re-publish every 6 h so the topic never expires on ntfy.sh (12 h cache).
setInterval(() => { published = ''; }, 6 * 3600 * 1000).unref();
