// NABO daily push: one-way pushes to the Tab5 「每日推送」 page.
//
//   sender --> POST /nabo/<token>/push          {"id","ts","source","priority","mood","title","body"}
//   Tab5  --> GET  /nabo/<token>/inbox?since=   items changed after a revision (or an ISO time)
//
// Pushing the same id again replaces the item; {"id","deleted":true} withdraws it (kept as a
// tombstone so a Tab5 that already has it drops it too). Read, done, learned and favourite
// state live on the Tab5 only. Items older than RETAIN_DAYS, and anything past MAX_ITEMS, are
// pruned. State is /data/nabo_inbox.json; Muse's inbox and podcast files are never touched.
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const SOURCES = ['nabo/fortune', 'nabo/study/paper', 'nabo/study/word', 'nabo/study/point', 'nabo/todo'];
const PRIORITIES = ['high', 'normal', 'low'];
const MOODS = ['idle', 'alert', 'rain', 'happy', 'thinking'];
const DEFAULT_MOOD = { 'nabo/fortune': 'happy', 'nabo/todo': 'alert' };
const MAX_REQUEST_BYTES = 16 * 1024;
const MAX_TITLE = 40;
const MAX_BODY = 600;
const MAX_FORTUNE_BODY = 6000;
const MAX_ITEMS = 500;
const RETAIN_DAYS = 30;
const DAY_MS = 24 * 3600 * 1000;
const ID_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._:-]{0,79}$/u;
// ISO 8601 with an explicit offset, so the Tab5 files every item under the right day.
const TS_PATTERN = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(:\d{2}(\.\d{1,3})?)?(Z|[+-]\d{2}:\d{2})$/u;
const TODO_TIME = /^([01]?\d|2[0-3]):([0-5]\d) /u;

class PushError extends Error {
  constructor(status, code) { super(code); this.status = status; this.code = code; }
}

function createNaboRoutes({ dataDir, token, now = Date.now, log = console.log }) {
  const FILE = path.join(dataDir, 'nabo_inbox.json');
  let store = load();

  function load() {
    try {
      const data = JSON.parse(fs.readFileSync(FILE, 'utf8'));
      if (Number.isInteger(data.rev) && Array.isArray(data.items)) return data;
    } catch (error) {
      if (error.code !== 'ENOENT') log('nabo inbox unreadable, starting empty:', error.message);
    }
    return { rev: 0, items: [] };
  }
  function save() {
    const temporary = `${FILE}.${process.pid}.tmp`;
    fs.writeFileSync(temporary, JSON.stringify(store));
    fs.renameSync(temporary, FILE);
  }

  function tokenOk(value) {
    const a = Buffer.from(String(value || '')), b = Buffer.from(token);
    return a.length === b.length && crypto.timingSafeEqual(a, b);
  }
  function send(res, status, body) {
    if (res.destroyed || res.writableEnded) return true;
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(body));
    return true;
  }
  function readBody(req) {
    return new Promise((resolve, reject) => {
      let size = 0;
      const chunks = [];
      req.on('data', chunk => {
        size += chunk.length;
        if (size > MAX_REQUEST_BYTES) {
          reject(new PushError(413, 'too_large'));
          req.destroy();
        } else chunks.push(chunk);
      });
      req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
      req.on('error', reject);
    });
  }

  function text(value, max, field, required) {
    if (value === undefined || value === null || value === '') {
      if (required) throw new PushError(400, `${field}_required`);
      return '';
    }
    if (typeof value !== 'string') throw new PushError(400, `bad_${field}`);
    const trimmed = value.trim();
    if ([...trimmed].length > max) throw new PushError(400, `${field}_too_long`);
    return trimmed;
  }

  // Returns the stored item, or throws a PushError naming the first bad field.
  function clean(input) {
    if (!input || typeof input !== 'object' || Array.isArray(input)) throw new PushError(400, 'bad_json');
    if (typeof input.id !== 'string' || !ID_PATTERN.test(input.id)) throw new PushError(400, 'bad_id');
    if (input.deleted === true) return { id: input.id, deleted: true };
    if (!SOURCES.includes(input.source)) throw new PushError(400, 'bad_source');
    const priority = input.priority === undefined ? 'normal' : input.priority;
    if (!PRIORITIES.includes(priority)) throw new PushError(400, 'bad_priority');
    const mood = input.mood === undefined ? DEFAULT_MOOD[input.source] || 'thinking' : input.mood;
    if (!MOODS.includes(mood)) throw new PushError(400, 'bad_mood');
    let ts = input.ts;
    if (ts === undefined) ts = new Date(now()).toISOString();
    if (typeof ts !== 'string' || !TS_PATTERN.test(ts) || Number.isNaN(Date.parse(ts)))
      throw new PushError(400, 'bad_ts');
    const fortune = input.source === 'nabo/fortune';
    const item = {
      id: input.id, ts, source: input.source, priority, mood,
      title: text(input.title, MAX_TITLE, 'title', true),
      body: text(input.body, fortune ? MAX_FORTUNE_BODY : MAX_BODY, 'body', fortune),
    };
    if (fortune) {
      let parsed;
      try {
        parsed = JSON.parse(item.body);
      } catch {
        throw new PushError(400, 'fortune_body_not_json');
      }
      if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed))
        throw new PushError(400, 'fortune_body_not_object');
    }
    // The Tab5 shows a leading "HH:MM " as the to-do's time; send it pre-split as well.
    if (input.source === 'nabo/todo') {
      const match = TODO_TIME.exec(item.title);
      if (match) item.time = `${match[1].padStart(2, '0')}:${match[2]}`;
    }
    return item;
  }

  function prune() {
    const cutoff = now() - RETAIN_DAYS * DAY_MS;
    let items = store.items.filter(item => item.updated_at >= cutoff &&
      (item.deleted || Date.parse(item.ts) >= cutoff));
    if (items.length > MAX_ITEMS) {
      items = items.slice().sort((a, b) => a.updated_at - b.updated_at).slice(-MAX_ITEMS)
        .sort((a, b) => a.rev - b.rev);
    }
    store.items = items;
  }

  function push(input) {
    const item = clean(input);
    const existing = store.items.findIndex(stored => stored.id === item.id);
    if (item.deleted && existing < 0) return { id: item.id, deleted: true, rev: store.rev, changed: false };
    store.rev += 1;
    const stored = Object.assign(item, { rev: store.rev, updated_at: now() });
    if (existing >= 0) store.items.splice(existing, 1);
    store.items.push(stored);
    prune();
    save();
    log('nabo push', JSON.stringify({ id: stored.id, source: stored.source, deleted: Boolean(stored.deleted),
      rev: stored.rev, bytes: stored.body ? Buffer.byteLength(stored.body) : 0 }));
    return { id: stored.id, rev: stored.rev, deleted: Boolean(stored.deleted), changed: true };
  }

  // since=<rev> (an integer) or since=<ISO time>; returns items in revision order.
  function inbox(url) {
    const since = url.searchParams.get('since') || '0';
    const limit = Math.min(200, Math.max(1, Number(url.searchParams.get('limit')) || 100));
    let changed;
    if (/^\d+$/u.test(since)) {
      changed = store.items.filter(item => item.rev > Number(since));
    } else {
      const time = Date.parse(since);
      if (Number.isNaN(time)) throw new PushError(400, 'bad_since');
      changed = store.items.filter(item => item.updated_at > time);
    }
    const page = changed.slice(0, limit);
    const out = page.map(({ updated_at, ...item }) => item);
    return { ok: true, rev: page.length ? page[page.length - 1].rev : store.rev, latest_rev: store.rev,
      more: changed.length > page.length, items: out };
  }

  async function handle(req, res) {
    const url = new URL(req.url, 'http://localhost');
    const parts = url.pathname.split('/').filter(Boolean);
    if (parts[0] !== 'nabo') return false;
    if (!tokenOk(parts[1])) return send(res, 404, { ok: false, error: 'not_found' });
    try {
      if (parts[2] === 'inbox' && parts.length === 3 && req.method === 'GET') {
        prune();
        return send(res, 200, inbox(url));
      }
      if (parts[2] === 'push' && parts.length === 3 && req.method === 'POST') {
        let body;
        const raw = await readBody(req);
        try {
          body = JSON.parse(raw);
        } catch {
          throw new PushError(400, 'bad_json');
        }
        return send(res, 200, { ok: true, ...push(body) });
      }
      return send(res, 404, { ok: false, error: 'not_found' });
    } catch (error) {
      if (error instanceof PushError) return send(res, error.status, { ok: false, error: error.code });
      log('nabo route error:', error && error.message);
      return send(res, 500, { ok: false, error: 'internal' });
    }
  }
  return handle;
}

module.exports = { createNaboRoutes, SOURCES, PRIORITIES, MOODS };
