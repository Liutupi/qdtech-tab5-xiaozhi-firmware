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
const MUSIC_FILE = path.join(DATA_DIR, 'music.json');
const PODCAST_FILE = path.join(DATA_DIR, 'podcasts.json');
const TOKEN_FILE = path.join(DATA_DIR, 'token.txt');
const TUNNEL_LOG = process.env.TUNNEL_LOG || path.join(DATA_DIR, 'tunnel.log');
const MAX_MESSAGES = 50;
const MAX_TITLE = 40;
const MAX_BODY = 600;
const MAX_REQUEST_BYTES = 64 * 1024;
// Daily Muse music-radio episodes (title + script + track list, optional audio URL).
const MAX_EPISODES = 30;
const MAX_SCRIPT = 3000;
const MAX_TRACKS = 30;
const PODCAST_SENDERS = ['每日电台'];
// NAS NetEase music API (xiaozhi-netease-nabo, host network) that turns a song name
// into a full-length playable URL — the same lookup daily recommendations use.
const MUSIC_API_URL = (process.env.MUSIC_API_URL || 'http://172.18.0.1:3099').replace(/\/+$/, '');

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
let podcasts = loadPodcasts();
if (!podcasts) {
  // First start with podcast support: seed from daily-radio posts already in the inbox.
  podcasts = { next_id: 1, episodes: [] };
  for (const m of inbox.messages.filter(isRadioPost))
    podcasts.episodes.push({ id: podcasts.next_id++, title: m.title, script: m.body, tracks: parseTracks(m.body),
                             audio_url: '', from: m.from, time: m.time });
  savePodcasts();
}
let music = null;
try { music = JSON.parse(fs.readFileSync(MUSIC_FILE, 'utf8')); } catch {}

function queueMusic(value) {
  const title = String(value?.title || '').slice(0, 120);
  const artist = String(value?.artist || '').slice(0, 120);
  const url = String(value?.url || '');
  const song_id = String(value?.song_id || '');
  if (!/^https?:\/\//.test(url) || url.length > 1200 || !/^\d{0,20}$/.test(song_id))
    throw new Error('invalid music command');
  music = { id: crypto.randomUUID(), issued_at: Date.now(), title, artist, url,
            song_id, continuous: value?.continuous === true };
  fs.writeFileSync(MUSIC_FILE + '.tmp', JSON.stringify(music));
  fs.renameSync(MUSIC_FILE + '.tmp', MUSIC_FILE);
  console.log('music queued', JSON.stringify({ id: music.id, title, artist }));
  return music.id;
}

function loadPodcasts() {
  try {
    const data = JSON.parse(fs.readFileSync(PODCAST_FILE, 'utf8'));
    if (Array.isArray(data.episodes) && Number.isInteger(data.next_id)) return data;
  } catch {}
  return null;
}

function savePodcasts() {
  const tmp = PODCAST_FILE + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(podcasts, null, 1));
  fs.renameSync(tmp, PODCAST_FILE);
}

// "1.《歌名》— 歌手" / "2. 《歌名》 - 歌手" lines from a plain-text daily radio post.
function parseTracks(text) {
  const tracks = [];
  for (const line of String(text || '').split('\n')) {
    const m = line.match(/^\s*\d{1,2}\s*[.、)）]\s*《([^》]{1,80})》\s*(?:[—–\-－:：]+\s*(.{0,120}))?$/);
    if (m) tracks.push({ title: m[1].trim(), artist: (m[2] || '').trim() });
  }
  return tracks.slice(0, MAX_TRACKS);
}

function cleanTracks(value) {
  if (!Array.isArray(value)) return [];
  return value
    .map((t) => (typeof t === 'string' ? { title: t } : t || {}))
    .map((t) => ({ title: clip(t.title, 80), artist: clip(t.artist, 120) }))
    .filter((t) => t.title)
    .slice(0, MAX_TRACKS);
}

function publishEpisode({ title, script, tracks, audio_url, from }, time = localTime()) {
  const audio = String(audio_url || '').trim();
  if (audio && (!/^https?:\/\//.test(audio) || audio.length > 1200)) throw new Error('audio_url must be an http(s) URL');
  const episode = {
    id: podcasts.next_id++,
    title: clip(title, MAX_TITLE),
    script: clip(script, MAX_SCRIPT),
    tracks: cleanTracks(tracks),
    audio_url: audio,
    from: clip(from || 'Muse', 16),
    time,
  };
  if (!episode.title) throw new Error('title is required');
  if (!episode.script && !episode.tracks.length && !episode.audio_url)
    throw new Error('script, tracks or audio_url is required');
  if (!episode.tracks.length) episode.tracks = parseTracks(episode.script);
  podcasts.episodes.push(episode);
  if (podcasts.episodes.length > MAX_EPISODES) podcasts.episodes = podcasts.episodes.slice(-MAX_EPISODES);
  savePodcasts();
  console.log('podcast', JSON.stringify({ id: episode.id, title: episode.title, tracks: episode.tracks.length,
    script: episode.script.length, audio: Boolean(episode.audio_url) }));
  return episode;
}

// Older daily-radio pushes (plain tab5_push from "每日电台") also become episodes.
function isRadioPost(msg) {
  return PODCAST_SENDERS.includes(msg.from) || /音乐电台|音乐播客|音乐博客/.test(msg.title);
}

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
  if (isRadioPost(msg)) {
    try {
      // Keep the full text for the episode script (the inbox copy is clipped at 600).
      publishEpisode({ title: msg.title, script: clip(body, MAX_SCRIPT), from: msg.from }, msg.time);
    } catch (e) {
      console.error('podcast from push failed', e.message);
    }
  }
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
    name: 'tab5_podcast_publish',
    description:
      'Publish one episode of the user\'s daily music radio / music podcast to the Tab5 "Muse 电台" card. ' +
      'Use this (not tab5_push) for the daily music radio. Write in Simplified Chinese. ' +
      'The script is shown in full on the Tab5 (plain text, line breaks allowed, up to 3000 characters): ' +
      'an opening, a short story or comment for each song, and a closing. ' +
      'List the songs in tracks in play order; the user taps a song to play it. ' +
      'audio_url is optional: an http(s) MP3 of a narrated episode the Tab5 can stream.',
    inputSchema: {
      type: 'object',
      properties: {
        title: { type: 'string', description: 'Episode title, e.g. "10月06日·每日音乐电台" (max 40 characters).' },
        script: { type: 'string', description: 'Episode script / show notes, plain text, up to 3000 characters.' },
        tracks: {
          type: 'array',
          description: 'Songs in play order (max 30).',
          items: {
            type: 'object',
            properties: { title: { type: 'string' }, artist: { type: 'string' } },
            required: ['title'],
          },
        },
        audio_url: { type: 'string', description: 'Optional http(s) URL of a narrated MP3 for this episode.' },
        from: { type: 'string', description: 'Optional sender label (default "Muse").' },
      },
      required: ['title', 'script'],
    },
  },
  {
    name: 'tab5_podcast_list',
    description: 'List the most recent music-radio episodes already on the Tab5 (to avoid duplicates).',
    inputSchema: {
      type: 'object',
      properties: { limit: { type: 'integer', minimum: 1, maximum: 10, description: 'Default 3.' } },
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
  if (name === 'tab5_podcast_publish') {
    const ep = publishEpisode(args);
    return toolResult(`已发布到 Tab5 Muse 电台 (#${ep.id} ${ep.time})：${ep.title}，${ep.tracks.length} 首歌`);
  }
  if (name === 'tab5_podcast_list') {
    const limit = Math.min(10, Math.max(1, Number(args.limit) || 3));
    const recent = podcasts.episodes.slice(-limit).reverse();
    return toolResult(
      recent.length
        ? recent.map((e) => `#${e.id} ${e.time} ${e.title}（${e.tracks.length} 首）`).join('\n')
        : 'Tab5 Muse 电台还没有节目。'
    );
  }
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
          'Use tab5_podcast_publish for the daily music radio / music podcast episode. ' +
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
    podcast_latest_id: podcasts.next_id - 1,
    messages,
    tunnel: base,
    mcp_url: base ? `${base}/mcp/${TOKEN}` : '',
    discovery_topic: DISCOVERY_TOPIC,
  };
}

// Resolve one episode track to a playable full-length NetEase URL for the Tab5.
async function podcastTrack(url) {
  const episodeId = Number(url.searchParams.get('episode'));
  const index = Number(url.searchParams.get('index'));
  const episode = podcasts.episodes.find((e) => e.id === episodeId);
  if (!episode || !Number.isInteger(index) || index < 0 || index >= episode.tracks.length)
    return { status: 404, body: { ok: false, message: 'track not found' } };
  const track = episode.tracks[index];
  const query = `${MUSIC_API_URL}/stream_pcm?song=${encodeURIComponent(track.title)}` +
    `&artist=${encodeURIComponent(track.artist || '')}`;
  const base = { episode: episodeId, index, count: episode.tracks.length, title: track.title, artist: track.artist };
  try {
    const r = await fetch(query, { signal: AbortSignal.timeout(15000) });
    const j = await r.json().catch(() => ({}));
    const playable = String(j.url || j.audio_url || '');
    if (!r.ok || !j.success || !/^https?:\/\//.test(playable)) {
      console.log('podcast track unavailable', JSON.stringify({ episode: episodeId, index, title: track.title }));
      return { status: 404, body: { ok: false, message: j.message || 'song not found', ...base } };
    }
    console.log('podcast track', JSON.stringify({ episode: episodeId, index, title: j.title || track.title }));
    return { status: 200, body: { ok: true, ...base, title: j.title || track.title,
      artist: j.artist || track.artist, url: playable, song_id: String(j.song_id || ''),
      duration_ms: Number(j.duration_ms) || 0 } };
  } catch (e) {
    console.error('podcast track lookup failed', e.message);
    return { status: 502, body: { ok: false, message: 'music service unavailable', ...base } };
  }
}

function podcastResponse(url) {
  const limit = Math.min(10, Math.max(1, Number(url.searchParams.get('limit')) || 6));
  return { latest_id: podcasts.next_id - 1, episodes: podcasts.episodes.slice(-limit).reverse() };
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
    if (url.pathname === '/health')
      return send(res, 200, { ok: true, messages: inbox.messages.length, episodes: podcasts.episodes.length });

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

    if (parts[0] === 'podcast' && req.method === 'GET') {
      if (!tokenOk(parts[1])) return send(res, 404, 'not found');
      if (parts[2] === 'track') {
        const r = await podcastTrack(url);
        return send(res, r.status, r.body);
      }
      return send(res, 200, podcastResponse(url));
    }

    // Only the local music container may queue playback; Tab5 fetches it through the tunnel.
    if (parts[0] === 'music' && req.method === 'GET') {
      if (!tokenOk(parts[1])) return send(res, 404, 'not found');
      return send(res, 200, music && Date.now() - music.issued_at < 90000 ? music : {});
    }

    if (parts[0] === 'tab5') {
      if (viaTunnel(req)) return send(res, 404, 'not found');
      if (parts[1] === 'music' && req.method === 'GET')
        return send(res, 200, music && Date.now() - music.issued_at < 90000 ? music : {});
      if (parts[1] === 'music' && req.method === 'POST') {
        if (!['127.0.0.1', '::1', '::ffff:127.0.0.1', '172.18.0.1', '::ffff:172.18.0.1'].includes(req.socket.remoteAddress))
          return send(res, 404, 'not found');
        const body = await readBody(req);
        if (Buffer.byteLength(body) > 2048) return send(res, 413, 'too large');
        return send(res, 200, { ok: true, id: queueMusic(JSON.parse(body)) });
      }
      if (parts[1] === 'inbox' && req.method === 'GET') {
        return send(res, 200, inboxResponse(url));
      }
      if (parts[1] === 'podcast' && req.method === 'GET') {
        if (parts[2] === 'track') {
          const r = await podcastTrack(url);
          return send(res, r.status, r.body);
        }
        return send(res, 200, podcastResponse(url));
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
