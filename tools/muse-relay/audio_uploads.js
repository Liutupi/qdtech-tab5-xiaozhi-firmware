'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

// Short, retryable requests: nothing is published until all bytes are durable
// and the complete file's SHA256 and MP3 prefix have been verified.
function createChunkUploads({ audioDir, token, tokenOk, validAudioName, publicAudioUrl,
  getPodcasts, savePodcasts, send }) {
  const root = path.join(audioDir, '.uploads');
  const CHUNK = 512 * 1024, MAX = 32 * 1024 * 1024, TTL = 24 * 3600 * 1000;
  const busy = new Set();
  fs.mkdirSync(root, { recursive: true });
  const fail = (status, message) => Object.assign(new Error(message), { status });
  const digestOk = value => typeof value === 'string' && /^[a-f0-9]{64}$/u.test(value);
  const validId = id => typeof id === 'string' && /^[a-f0-9]{32}$/u.test(id);
  const folder = id => path.join(root, id);
  const manifestFile = id => path.join(folder(id), 'manifest.json');
  function log(event, values = {}) {
    console.log('muse audio', JSON.stringify({ event, ...values }));
  }
  function save(m) {
    const file = manifestFile(m.upload_id), temporary = file + '.tmp';
    fs.writeFileSync(temporary, JSON.stringify(m), { mode: 0o600 });
    fs.renameSync(temporary, file);
  }
  function load(id) {
    if (!validId(id)) throw fail(404, 'upload not found');
    try { return JSON.parse(fs.readFileSync(manifestFile(id), 'utf8')); }
    catch { throw fail(404, 'upload not found'); }
  }
  function findEpisode(id) {
    if (id === null) return null;
    const episode = getPodcasts().episodes.find(e => String(e.id) === String(id));
    if (!episode) throw fail(404, 'episode_id not found');
    return episode;
  }
  function expected(m, index) { return Math.min(CHUNK, m.size - index * CHUNK); }
  function status(m) {
    const missing = [], received = [];
    let receivedBytes = 0;
    if (m.state !== 'completed') {
      for (let index = 0; index < m.chunk_count; index++) {
        let ok = false;
        try {
          ok = Boolean(m.chunks[index]) &&
            fs.statSync(path.join(folder(m.upload_id), `${index}.part`)).size === expected(m, index);
        } catch {}
        if (ok) { received.push(index); receivedBytes += expected(m, index); }
        else missing.push(index);
      }
    } else receivedBytes = m.size;
    return { ok: true, upload_id: m.upload_id, state: m.state, filename: m.filename,
      size: m.size, sha256: m.sha256, episode_id: m.episode_id, chunk_size: CHUNK,
      chunk_count: m.chunk_count, received_bytes: receivedBytes, received, missing,
      ...(m.state === 'completed' ? { result: { ...m.result,
        audio_url: publicAudioUrl(m.result.filename) } } : {}) };
  }
  async function jsonBody(req) {
    let size = 0; const pieces = [];
    for await (const piece of req.iterator({ destroyOnReturn: false })) {
      size += piece.length;
      if (size > 4096) throw fail(413, 'JSON body exceeds 4096 bytes');
      pieces.push(piece);
    }
    try { return JSON.parse(Buffer.concat(pieces).toString()); }
    catch { throw fail(400, 'invalid JSON body'); }
  }
  function cleanup() {
    let active = 0;
    for (const id of fs.readdirSync(root)) {
      if (!validId(id) || busy.has(id)) continue;
      let m;
      try { m = load(id); } catch { continue; }
      if (Date.now() - m.updated_at > TTL) fs.rmSync(folder(id), { recursive: true });
      else if (m.state !== 'completed') active++;
    }
    return active;
  }
  async function start(req) {
    const value = await jsonBody(req);
    if (!value || typeof value !== 'object' || !validAudioName(value.filename) ||
        !Number.isSafeInteger(value.size) || value.size < 3 || value.size > MAX ||
        !digestOk(value.sha256))
      throw fail(400, 'supply filename (.mp3), size (3..32 MiB), and lowercase sha256');
    const episodeId = value.episode_id ?? null;
    if (episodeId !== null && (!Number.isSafeInteger(episodeId) || episodeId < 1))
      throw fail(400, 'episode_id must be a positive integer');
    findEpisode(episodeId);
    const id = crypto.createHmac('sha256', token)
      .update(JSON.stringify([value.filename, value.size, value.sha256, episodeId]))
      .digest('hex').slice(0, 32);
    try { return status(load(id)); }
    catch (error) { if (error.status !== 404) throw error; }
    if (cleanup() >= 8) throw fail(429, 'too many unfinished uploads; resume an existing upload');
    fs.mkdirSync(folder(id), { recursive: true, mode: 0o700 });
    const m = { upload_id: id, filename: value.filename, size: value.size,
      sha256: value.sha256, episode_id: episodeId, chunk_count: Math.ceil(value.size / CHUNK),
      chunks: {}, state: 'receiving', updated_at: Date.now() };
    save(m); log('created', { upload_id: id, size: m.size, episode_id: episodeId });
    return status(m);
  }
  async function put(req, m, index) {
    if (!Number.isSafeInteger(index) || index < 0 || index >= m.chunk_count)
      throw fail(400, 'invalid chunk index');
    if (m.state === 'completed') { req.resume(); return status(m); }
    const sha256 = req.headers['x-chunk-sha256'];
    if (!digestOk(sha256)) throw fail(400, 'X-Chunk-SHA256 is required');
    const wanted = expected(m, index);
    if (req.headers['content-length'] && Number(req.headers['content-length']) !== wanted)
      throw fail(400, 'chunk length does not match expected bytes');
    const temporary = path.join(folder(m.upload_id), `.next-${crypto.randomUUID()}`);
    const hash = crypto.createHash('sha256'); let size = 0, handle;
    req.setTimeout(60000, () => req.destroy());
    try {
      handle = await fs.promises.open(temporary, 'wx', 0o600);
      for await (const piece of req.iterator({ destroyOnReturn: false })) {
        size += piece.length;
        if (size > wanted) throw fail(413, 'chunk exceeds expected size');
        hash.update(piece);
        await writeAll(handle, piece);
      }
      if (size !== wanted || hash.digest('hex') !== sha256)
        throw fail(400, 'chunk size or SHA256 mismatch');
      if (m.chunks[index] && m.chunks[index].sha256 !== sha256)
        throw fail(409, 'a different chunk is already stored at this index');
      await handle.sync(); await handle.close(); handle = null;
      await fs.promises.rename(temporary, path.join(folder(m.upload_id), `${index}.part`));
      m.chunks[index] = { sha256, size }; m.updated_at = Date.now(); save(m);
      log('chunk_saved', { upload_id: m.upload_id, index, bytes: size });
      return { ok: true, upload_id: m.upload_id, index, bytes: size, sha256 };
    } catch (error) { error.received_bytes = size; throw error; }
    finally {
      if (handle) await handle.close().catch(() => {});
      await fs.promises.unlink(temporary).catch(() => {});
    }
  }
  async function writeAll(handle, piece) {
    let offset = 0;
    while (offset < piece.length) {
      const result = await handle.write(piece, offset, piece.length - offset);
      if (!result.bytesWritten) throw new Error('audio write failed');
      offset += result.bytesWritten;
    }
  }
  async function complete(req, m) {
    await jsonBody(req);
    if (m.state === 'completed') return status(m).result;
    const missing = status(m).missing;
    if (missing.length) throw fail(409, `missing chunks: ${missing.join(',')}`);
    const episode = findEpisode(m.episode_id);
    const temporary = path.join(audioDir, `.assemble-${m.upload_id}.part`);
    const hash = crypto.createHash('sha256'); let size = 0, prefix = Buffer.alloc(0), handle;
    const started = Date.now();
    try {
      handle = await fs.promises.open(temporary, 'w', 0o600);
      for (let index = 0; index < m.chunk_count; index++) {
        const chunkHash = crypto.createHash('sha256'); let chunkSize = 0;
        for await (const piece of fs.createReadStream(path.join(folder(m.upload_id), `${index}.part`))) {
          size += piece.length; chunkSize += piece.length; chunkHash.update(piece); hash.update(piece);
          if (size > m.size) throw fail(400, 'assembled audio exceeds expected size');
          if (prefix.length < 3) prefix = Buffer.concat([prefix, piece.subarray(0, 3 - prefix.length)]);
          await writeAll(handle, piece);
        }
        if (chunkSize !== expected(m, index) || chunkHash.digest('hex') !== m.chunks[index].sha256)
          throw fail(400, `stored chunk ${index} is corrupt`);
      }
      if (size !== m.size || hash.digest('hex') !== m.sha256)
        throw fail(400, 'complete audio SHA256 mismatch');
      if (!(prefix.toString() === 'ID3' || (prefix[0] === 0xff && (prefix[1] & 0xe0) === 0xe0)))
        throw fail(400, 'unsupported MP3 header');
      await handle.sync(); await handle.close(); handle = null;
      const filename = `${m.filename.slice(0, -4).slice(0, 140)}-${m.sha256.slice(0, 12)}.mp3`;
      await fs.promises.rename(temporary, path.join(audioDir, filename));
      const audio_url = publicAudioUrl(filename);
      if (episode) {
        const previous = episode.audio_url, previousTranscript = episode.transcript;
        try {
          episode.audio_url = audio_url;
          if (previousTranscript?.audio_sha256 !== m.sha256) delete episode.transcript;
          savePodcasts();
        } catch (error) {
          episode.audio_url = previous;
          if (previousTranscript !== undefined) episode.transcript = previousTranscript;
          throw error;
        }
      }
      const result = { ok: true, filename, size, sha256: m.sha256, audio_url,
        episode_id: m.episode_id, processing_ms: Date.now() - started };
      m.result = result; m.state = 'completed'; m.updated_at = Date.now(); save(m);
      // Keep the small receipt for lost-response retries, release temporary chunks.
      for (let index = 0; index < m.chunk_count; index++)
        await fs.promises.unlink(path.join(folder(m.upload_id), `${index}.part`)).catch(() => {});
      log('completed', { upload_id: m.upload_id, size, episode_id: m.episode_id,
        processing_ms: result.processing_ms });
      return result;
    } finally {
      if (handle) await handle.close().catch(() => {});
      await fs.promises.unlink(temporary).catch(() => {});
    }
  }
  return async function handle(req, res) {
    const url = new URL(req.url, 'http://localhost');
    const parts = url.pathname.split('/').filter(Boolean);
    if (parts[0] !== 'podcast' || parts[1] !== 'audio' || parts[2] !== 'uploads') return false;
    let held = null;
    const started = Date.now();
    try {
      const auth = /^Bearer (.+)$/u.exec(req.headers.authorization || '');
      if (!tokenOk(auth?.[1])) throw fail(401, 'invalid bearer token');
      let result;
      if (parts.length === 4 && parts[3] === 'client' && req.method === 'GET') {
        send(res, 200, fs.readFileSync(path.join(__dirname, 'upload_audio.py'), 'utf8'),
          { 'Content-Type': 'text/x-python; charset=utf-8' });
        return true;
      } else if (parts.length === 3 && req.method === 'POST') result = await start(req);
      else if (validId(parts[3])) {
        const id = parts[3];
        if (parts.length === 4 && req.method === 'GET') result = status(load(id));
        else {
          if (busy.has(id) || busy.size >= 2) throw fail(429, 'upload busy; retry shortly');
          busy.add(id); held = id;
          const m = load(id);
          if (parts.length === 6 && parts[4] === 'chunks' && req.method === 'PUT' && /^\d+$/u.test(parts[5]))
            result = await put(req, m, Number(parts[5]));
          else if (parts.length === 5 && parts[4] === 'complete' && req.method === 'POST')
            result = await complete(req, m);
          else throw fail(405, 'unsupported upload operation');
        }
      } else throw fail(404, 'upload not found');
      send(res, 200, result);
    } catch (error) {
      log('failed', { upload_id: validId(parts[3]) ? parts[3] : null, method: req.method,
        status: error.status || 500, error: error.message, received_bytes: error.received_bytes,
        duration_ms: Date.now() - started });
      send(res, error.status || 500, { ok: false, error: error.message },
        error.status === 429 ? { 'Retry-After': '2' } : {});
    } finally { if (held) busy.delete(held); }
    return true;
  };
}
module.exports = { createChunkUploads };
