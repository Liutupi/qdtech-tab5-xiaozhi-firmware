'use strict';
// Mount before the existing /podcast catch-all. Node 18+, ffmpeg with libmp3lame.
// No URL, token, upstream error text, or ffmpeg stderr is logged by this module.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const http = require('node:http');
const https = require('node:https');
const dns = require('node:dns').promises;
const net = require('node:net');
const { spawn } = require('node:child_process');
const { Transform, Writable } = require('node:stream');
const { pipeline } = require('node:stream/promises');

function failure(status, code) { return Object.assign(new Error(code), { status, code }); }
function publicAddress(value) {
  if (net.isIP(value) === 4) {
    const [a, b, c] = value.split('.').map(Number);
    return !(a === 0 || a === 10 || a === 127 || a >= 224 ||
      (a === 100 && b >= 64 && b <= 127) || (a === 169 && b === 254) ||
      (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) ||
      (a === 192 && b === 0) || (a === 192 && b === 88 && c === 99) ||
      (a === 198 && (b === 18 || b === 19 || (b === 51 && c === 100))) ||
      (a === 203 && b === 0 && c === 113));
  }
  // Only global unicast IPv6; exclude documentation, transition and special-use ranges.
  if (net.isIP(value) !== 6) return false;
  const first = parseInt(value.split(':')[0], 16);
  const second = parseInt(value.split(':')[1] || '0', 16);
  return first >= 0x2000 && first <= 0x3fff && first !== 0x2002 &&
    !(first === 0x2001 && (second < 0x200 || second === 0xdb8)) && first !== 0x3fff;
}

function byteRange(header, size) {
  if (!header) return { first: 0, last: size - 1, partial: false };
  const m = /^bytes=(\d*)-(\d*)$/.exec(header);
  if (!m || (!m[1] && !m[2])) throw failure(416, 'range_not_satisfiable');
  let first = m[1] ? Number(m[1]) : Math.max(0, size - Number(m[2]));
  let last = m[1] && m[2] ? Math.min(size - 1, Number(m[2])) : size - 1;
  if (!Number.isSafeInteger(first) || !Number.isSafeInteger(last) || first < 0 ||
      first >= size || last < first || (!m[1] && Number(m[2]) === 0))
    throw failure(416, 'range_not_satisfiable');
  return { first, last, partial: true };
}

// Validate every emitted frame, rather than trusting an average bitrate field.
class Mp3Frames extends Transform {
  constructor(limit) { super(); this.pending = Buffer.alloc(0); this.bytes = 0; this.frames = 0; this.limit = limit; }
  _transform(chunk, encoding, callback) {
    try {
      this.bytes += chunk.length;
      if (this.bytes > this.limit) throw failure(502, 'output_too_large');
      const data = Buffer.concat([this.pending, chunk]);
      let offset = 0;
      while (offset + 4 <= data.length) {
        const a = data[offset], b = data[offset + 1], c = data[offset + 2], d = data[offset + 3];
        if (a !== 255 || (b & 0xfe) !== 0xfa || (c >> 4) !== 7 ||
            ((c >> 2) & 3) !== 0 || (d >> 6) === 3)
          throw failure(502, 'invalid_output_format');
        const length = 313 + ((c >> 1) & 1);
        if (offset + length > data.length) break;
        offset += length;
        ++this.frames;
      }
      if (offset) this.push(data.subarray(0, offset));
      this.pending = Buffer.from(data.subarray(offset));
      callback();
    } catch (e) { callback(e); }
  }
  _flush(callback) {
    callback(this.pending.length || !this.frames ? failure(502, 'incomplete_output') : null);
  }
}

function createStream96Routes(options) {
  const { dataDir, token, selfOrigins = () => [], ffmpeg = process.env.FFMPEG || 'ffmpeg' } = options;
  if (!dataDir || typeof token !== 'string' || token.length < 16) throw new Error('stream96 configuration required');
  const cacheDir = path.join(dataDir, 'audio96-cache');
  const maxJobs = options.maxJobs ?? 2;
  const maxBytes = options.maxBytes ?? 512 * 1024 * 1024;
  const maxFile = options.maxFile ?? 128 * 1024 * 1024;
  const maxInput = options.maxInput ?? 256 * 1024 * 1024;
  const retainMs = options.retainMs ?? 24 * 3600 * 1000;
  const prepareMs = Math.min(options.prepareMs ?? 19000, 19000);
  const jobMs = options.jobMs ?? 15 * 60 * 1000;
  const jobs = new Map(), readers = new Map(), recentErrors = new Map();
  const controllers = new Set();
  let active = 0, reserved = 0, requests = 0, closing = false;
  // Serialize disk reservation and eviction; conversion itself stays concurrent.
  let diskLock = Promise.resolve();
  const ready = fs.promises.mkdir(cacheDir, { recursive: true, mode: 0o700 }).then(async () => {
    for (const name of await fs.promises.readdir(cacheDir))
      if (/^\.[a-f0-9]{64}-[a-f0-9-]+\.part$/.test(name))
        await fs.promises.unlink(path.join(cacheDir, name)).catch(() => {});
  });
  function authenticated(candidate) {
    const a = Buffer.from(candidate), b = Buffer.from(token);
    return a.length === b.length && crypto.timingSafeEqual(a, b);
  }
  function send(res, status, code, headers = {}) {
    if (res.destroyed || res.writableEnded) return;
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8',
      'Cache-Control': 'private, no-store', ...headers });
    res.end(JSON.stringify({ ok: false, code }));
  }
  function checkedUrl(text) {
    if (typeof text !== 'string' || Buffer.byteLength(text) > 2048) throw failure(400, 'bad_request');
    let u;
    try { u = new URL(text); } catch { throw failure(400, 'bad_request'); }
    if (!['http:', 'https:'].includes(u.protocol) || u.username || u.password || u.hash ||
        /\/stream96(?:\/|$)/i.test(u.pathname)) throw failure(400, 'bad_request');
    return u;
  }
  async function openSource(initial, signal) {
    let u = checkedUrl(initial);
    for (let redirect = 0; redirect <= 5; ++redirect) {
      signal.throwIfAborted();
      // Own uploaded audio is read locally, never fetched from a private IP.
      const origins = typeof selfOrigins === 'function' ? selfOrigins() : selfOrigins;
      if (origins.includes(u.origin)) {
        const p = u.pathname.split('/');
        if (p.length !== 4 || p[1] !== 'audio' || !authenticated(p[2]) ||
            !/^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,155}\.mp3$/.test(p[3]))
          throw failure(403, 'source_forbidden');
        let root;
        try { root = await fs.promises.realpath(path.join(dataDir, 'audio')); }
        catch { throw failure(404, 'source_not_found'); }
        let file;
        try { file = await fs.promises.realpath(path.join(root, p[3])); }
        catch { throw failure(404, 'source_not_found'); }
        if (path.dirname(file) !== root) throw failure(403, 'source_forbidden');
        const stat = await fs.promises.stat(file);
        if (!stat.isFile() || stat.size > maxInput) throw failure(502, 'upstream_error');
        return fs.createReadStream(file, { signal });
      }
      const hostname = u.hostname.replace(/^\[|\]$/g, '');
      let records;
      try {
        records = net.isIP(hostname) ? [{ address: hostname, family: net.isIP(hostname) }]
          : await dns.lookup(hostname, { all: true, verbatim: true });
      } catch { throw failure(502, 'upstream_error'); }
      if (!records.length || records.some(r => !publicAddress(r.address))) throw failure(403, 'source_forbidden');
      const headers = { 'User-Agent': 'Mozilla/5.0 ESP32 Radio', 'Accept': 'audio/*,*/*;q=0.8',
        'Accept-Encoding': 'identity', 'Icy-MetaData': '0' };
      if (/(^|\.)music\.126\.net$/i.test(hostname)) headers.Referer = 'https://music.163.com/';
      const response = await new Promise((resolve, reject) => {
        const request = (u.protocol === 'https:' ? https : http).get(u, {
          headers, signal, agent: false,
          // Pin the checked address for this request: no second DNS lookup.
          lookup: (_host, opts, callback) => opts.all ? callback(null, records)
            : callback(null, records[0].address, records[0].family),
        }, resolve);
        request.setTimeout(15000, () => request.destroy(failure(504, 'timeout')));
        request.on('error', reject);
      });
      if ([301, 302, 303, 307, 308].includes(response.statusCode)) {
        const location = response.headers.location;
        response.destroy();
        if (!location || redirect === 5) throw failure(502, 'upstream_error');
        u = checkedUrl(new URL(location, u).href);
        continue;
      }
      if (response.statusCode !== 200) {
        const status = response.statusCode;
        response.destroy();
        throw failure(status === 404 ? 404 : 502, status === 404 ? 'source_not_found' : 'upstream_error');
      }
      if (Number(response.headers['content-length'] || 0) > maxInput) {
        response.destroy(); throw failure(502, 'upstream_error');
      }
      return response;
    }
    throw failure(502, 'upstream_error');
  }
  async function convert(sourceUrl, destination, controller, live) {
    const { signal } = controller;
    let child, input, killTimer;
    const timeout = setTimeout(() => controller.abort(failure(504, 'timeout')), live ? 6 * 3600 * 1000 : jobMs);
    const abortChild = () => {
      input?.destroy();
      if (child && child.exitCode === null && child.signalCode === null) {
        child.kill('SIGTERM');
        killTimer = setTimeout(() => child.kill('SIGKILL'), 2000);
        killTimer.unref();
      }
    };
    signal.addEventListener('abort', abortChild, { once: true });
    try {
      input = await openSource(sourceUrl, signal);
      signal.throwIfAborted();
      child = spawn(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin',
        '-protocol_whitelist', 'pipe', '-i', 'pipe:0', '-map', '0:a:0', '-vn', '-sn', '-dn',
        '-map_metadata', '-1', '-c:a', 'libmp3lame', '-b:a', '96k', '-ar', '44100', '-ac', '2',
        '-write_xing', '0', '-id3v2_version', '0', '-write_id3v1', '0', '-flush_packets', '1',
        '-f', 'mp3', 'pipe:1'], { stdio: ['pipe', 'pipe', 'ignore'], shell: false });
      const exited = new Promise((resolve, reject) => {
        child.once('error', () => reject(failure(502, 'encoder_unavailable')));
        child.once('close', code => code === 0 ? resolve() : reject(failure(502, 'upstream_error')));
      });
      let received = 0;
      const limit = new Transform({ transform(chunk, enc, cb) {
        received += chunk.length;
        cb(!live && received > maxInput ? failure(502, 'input_too_large') : null, chunk);
      } });
      const tasks = [pipeline(input, limit, child.stdin, { signal }),
        pipeline(child.stdout, new Mp3Frames(live ? Number.MAX_SAFE_INTEGER : maxFile), destination, { signal }), exited];
      try { await Promise.all(tasks); }
      catch (e) {
        const reason = signal.aborted && signal.reason?.status ? signal.reason : e;
        controller.abort(reason); await Promise.allSettled(tasks); throw reason;
      }
    } finally {
      clearTimeout(timeout);
      signal.removeEventListener('abort', abortChild);
      if (signal.aborted) abortChild();
      clearTimeout(killTimer);
    }
  }
  function reserveDisk() {
    const run = diskLock.then(async () => {
      let total = 0, files = 0;
      const entries = [];
      for (const name of await fs.promises.readdir(cacheDir)) {
        if (!/^[a-f0-9]{64}\.mp3$/.test(name)) continue;
        const file = path.join(cacheDir, name), stat = await fs.promises.stat(file);
        total += stat.size; ++files; entries.push({ file, size: stat.size, time: stat.mtimeMs });
      }
      entries.sort((a, b) => a.time - b.time);
      for (const e of entries) {
        if (!readers.has(e.file) && Date.now() - e.time >= retainMs) {
          await fs.promises.unlink(e.file); total -= e.size; --files;
        }
      }
      if (total + reserved + maxFile > maxBytes || files >= 256) throw failure(503, 'busy');
      reserved += maxFile;
    });
    diskLock = run.catch(() => {});
    return run;
  }
  async function cached(source) {
    const key = crypto.createHash('sha256').update('mp3-cbr96-44100-stereo-v1\0' + source).digest('hex');
    const file = path.join(cacheDir, key + '.mp3');
    let existing;
    try { existing = await fs.promises.stat(file); } catch {}
    if (existing?.isFile() && existing.size > 0) return file;
    if (jobs.has(key)) return jobs.get(key);
    if (closing || active >= maxJobs) throw failure(503, 'busy');
    const error = recentErrors.get(key);
    if (error && Date.now() - error.time < 2000) throw error.value;
    ++active;
    const job = (async () => {
      const controller = new AbortController();
      controllers.add(controller);
      const temporary = path.join(cacheDir, `.${key}-${crypto.randomUUID()}.part`);
      let disk = false, output;
      try {
        await reserveDisk(); disk = true;
        output = fs.createWriteStream(temporary, { flags: 'wx', mode: 0o600 });
        // Source validation may fail before pipeline takes stream ownership.
        output.on('error', () => {});
        await new Promise((resolve, reject) => { output.once('open', resolve); output.once('error', reject); });
        await convert(source, output, controller, false);
        await fs.promises.rename(temporary, file);
        recentErrors.delete(key);
        return file;
      } catch (e) {
        recentErrors.set(key, { time: Date.now(), value: e });
        if (recentErrors.size > 64) recentErrors.delete(recentErrors.keys().next().value);
        throw e;
      } finally {
        output?.destroy();
        await fs.promises.unlink(temporary).catch(() => {});
        if (disk) reserved -= maxFile;
        controllers.delete(controller); --active; jobs.delete(key);
      }
    })();
    jobs.set(key, job);
    return job;
  }
  const audioHeaders = { 'Content-Type': 'audio/mpeg', 'Cache-Control': 'private, no-store',
    'X-Audio-Codec': 'mp3', 'X-Audio-Bitrate': '96000', 'X-Audio-Sample-Rate': '44100', 'X-Audio-Channels': '2' };
  async function serveFile(req, res, file) {
    readers.set(file, (readers.get(file) || 0) + 1);
    try {
      const size = (await fs.promises.stat(file)).size;
      let range;
      try { range = byteRange(req.headers.range, size); }
      catch (e) { send(res, 416, e.code, { 'Content-Range': `bytes */${size}` }); return; }
      const now = new Date();
      await fs.promises.utimes(file, now, now);
      const headers = { ...audioHeaders, 'Accept-Ranges': 'bytes', 'X-Audio-Seekable': '1',
        'Content-Length': range.last - range.first + 1 };
      if (range.partial) headers['Content-Range'] = `bytes ${range.first}-${range.last}/${size}`;
      res.writeHead(range.partial ? 206 : 200, headers);
      if (req.method === 'HEAD') { res.end(); return; }
      await pipeline(fs.createReadStream(file, { start: range.first, end: range.last }), res);
    } finally {
      const remaining = readers.get(file) - 1;
      if (remaining) readers.set(file, remaining); else readers.delete(file);
      const now = new Date();
      await fs.promises.utimes(file, now, now).catch(() => {});
    }
  }
  async function handler(req, res) {
    let u;
    try { u = new URL(req.url, 'http://localhost'); } catch { return false; }
    const parts = u.pathname.split('/');
    const lan = u.pathname === '/tab5/podcast/stream96';
    const cloud = parts.length === 4 && parts[1] === 'podcast' && parts[3] === 'stream96';
    if (!lan && !cloud) return false;
    if ((cloud && !authenticated(parts[2])) ||
        (lan && (req.headers['cf-connecting-ip'] || req.headers['cf-ray']))) {
      send(res, 404, 'not_found'); return true;
    }
    if (!['GET', 'HEAD'].includes(req.method)) { send(res, 405, 'method_not_allowed', { Allow: 'GET, HEAD' }); return true; }
    if (closing || requests >= 32) { send(res, 503, 'busy', { 'Retry-After': '2' }); return true; }
    ++requests;
    try {
      const sources = u.searchParams.getAll('url'), modes = u.searchParams.getAll('live');
      if (sources.length !== 1 || modes.length !== 1 || !['0', '1'].includes(modes[0])) throw failure(400, 'bad_request');
      const source = checkedUrl(sources[0]).href;
      await ready;
      if (modes[0] === '1') {
        if (req.method === 'HEAD') throw failure(405, 'live_head_not_supported');
        if (req.headers.range) throw failure(416, 'range_not_satisfiable');
        if (active >= maxJobs) throw failure(503, 'busy');
        ++active;
        const controller = new AbortController();
        controllers.add(controller);
        const disconnect = () => controller.abort();
        res.once('close', disconnect);
        // Send success headers only when the encoder produced a valid MP3 frame.
        const output = new Writable({ write(chunk, enc, cb) {
          clearTimeout(started);
          if (!res.headersSent) res.writeHead(200, { ...audioHeaders, 'Accept-Ranges': 'none', 'X-Audio-Seekable': '0' });
          if (res.write(chunk)) cb(); else res.once('drain', cb);
        }, final(cb) { res.end(cb); } });
        const started = setTimeout(() => controller.abort(failure(504, 'timeout')), 19000);
        try { await convert(source, output, controller, true); }
        catch (e) { controller.abort(e); throw e; }
        finally {
          clearTimeout(started); res.removeListener('close', disconnect);
          controllers.delete(controller); --active;
        }
      } else {
        let timer;
        try {
          const file = await Promise.race([cached(source), new Promise((_, reject) => {
            timer = setTimeout(() => reject(failure(503, 'preparing')), prepareMs);
          })]);
          if (!res.destroyed) await serveFile(req, res, file);
        } finally { clearTimeout(timer); }
      }
    } catch (e) {
      if (res.headersSent) res.destroy();
      else send(res, e.status || 502, e.code && e.status ? e.code : 'upstream_error',
        e.status === 503 ? { 'Retry-After': '2' } : {});
    } finally { --requests; }
    return true;
  }
  handler.close = async () => {
    closing = true;
    for (const c of controllers) c.abort();
    await Promise.allSettled([...jobs.values()]);
  };
  return handler;
}
module.exports = { createStream96Routes, byteRange, publicAddress, Mp3Frames };
