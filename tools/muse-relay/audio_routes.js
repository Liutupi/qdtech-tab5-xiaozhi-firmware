'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { createChunkUploads } = require('./audio_uploads');
const { createTranscriptRoutes } = require('./transcript_routes');

function createAudioRoutes({ dataDir, token, tunnelUrl, getPodcasts, savePodcasts }) {
  const TOKEN = token;
  const AUDIO_DIR = path.join(dataDir, 'audio');
  const MAX_AUDIO_BYTES = 32 * 1024 * 1024;
  let audioUploads = 0;
  fs.mkdirSync(AUDIO_DIR, { recursive: true });
  function tokenOk(value) {
    const a = Buffer.from(String(value || '')), b = Buffer.from(TOKEN);
    return a.length === b.length && crypto.timingSafeEqual(a, b);
  }
  function send(res, status, body, headers = {}) {
    if (res.destroyed || res.writableEnded) return;
    const payload = typeof body === 'string' ? body : JSON.stringify(body);
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8',
      'Cache-Control': 'no-store', ...headers });
    res.end(payload);
  }
function sendHandled(res, status, body, headers) { send(res, status, body, headers); return true; }

function audioFile(filename) {
  if (!validAudioName(filename))
    return null;
  try {
    const file = fs.realpathSync(path.join(AUDIO_DIR, filename));
    const stat = fs.statSync(file);
    return path.dirname(file) === fs.realpathSync(AUDIO_DIR) && stat.isFile()
      ? { file, size: stat.size } : null;
  } catch { return null; }
}

function validAudioName(filename) {
  return typeof filename === 'string' && filename.length <= 160 &&
    /^[a-zA-Z0-9][a-zA-Z0-9_.-]*\.mp3$/u.test(filename);
}

// A single file field, streamed to disk with backpressure. The old MP3 and
// episode remain usable until the complete replacement is atomically renamed.
async function receiveAudio(req) {
  const type = req.headers['content-type'] || '';
  const match = /^multipart\/form-data;\s*boundary=(?:"([^"]+)"|([^;\s]+))$/iu.exec(type);
  const boundary = match?.[1] || match?.[2];
  if (!boundary || boundary.length > 100 || /[\r\n]/u.test(boundary))
    throw Object.assign(new Error('use multipart/form-data with one file field'), { status: 400 });
  if (Number(req.headers['content-length'] || 0) > MAX_AUDIO_BYTES + 8192)
    throw Object.assign(new Error('MP3 exceeds 32 MiB'), { status: 413 });
  const opening = Buffer.from(`--${boundary}\r\n`);
  const delimiter = Buffer.from(`\r\n--${boundary}`);
  const temporary = path.join(AUDIO_DIR, `.upload-${crypto.randomUUID()}.part`);
  let handle, filename, buffer = Buffer.alloc(0), prefix = Buffer.alloc(0);
  let stage = 'headers', total = 0, size = 0;
  req.setTimeout(60000, () => req.destroy());
  async function write(data) {
    if (!data.length) return;
    size += data.length;
    if (size > MAX_AUDIO_BYTES)
      throw Object.assign(new Error('MP3 exceeds 32 MiB'), { status: 413 });
    if (prefix.length < 3) prefix = Buffer.concat([prefix, data.subarray(0, 3 - prefix.length)]);
    let offset = 0;
    while (offset < data.length) {
      const result = await handle.write(data, offset, data.length - offset);
      if (!result.bytesWritten) throw new Error('MP3 write failed');
      offset += result.bytesWritten;
    }
  }
  try {
    for await (const chunk of req.iterator({ destroyOnReturn: false })) {
      total += chunk.length;
      if (total > MAX_AUDIO_BYTES + 8192)
        throw Object.assign(new Error('MP3 exceeds 32 MiB'), { status: 413 });
      buffer = Buffer.concat([buffer, chunk]);
      if (stage === 'headers') {
        const end = buffer.indexOf('\r\n\r\n');
        if (end < 0) {
          if (buffer.length > 4096) throw Object.assign(new Error('invalid file headers'), { status: 400 });
          continue;
        }
        if (end > 4096 || !buffer.subarray(0, opening.length).equals(opening))
          throw Object.assign(new Error('invalid multipart opening'), { status: 400 });
        const headers = buffer.subarray(opening.length, end).toString('utf8');
        const disposition = /^content-disposition:\s*form-data;\s*name="file";\s*filename="([^"]+)"\s*$/imu.exec(headers);
        filename = disposition?.[1];
        if (!validAudioName(filename))
          throw Object.assign(new Error('file must have a simple .mp3 filename'), { status: 400 });
        handle = await fs.promises.open(temporary, 'wx', 0o600);
        buffer = buffer.subarray(end + 4);
        stage = 'file';
      }
      if (stage === 'file') {
        const end = buffer.indexOf(delimiter);
        if (end >= 0) {
          if (buffer.length < end + delimiter.length + 2) continue;
          if (buffer.subarray(end + delimiter.length, end + delimiter.length + 2).toString() !== '--')
            throw Object.assign(new Error('send exactly one file field'), { status: 400 });
          await write(buffer.subarray(0, end));
          buffer = buffer.subarray(end + delimiter.length + 2);
          stage = 'done';
        } else {
          const count = Math.max(0, buffer.length - delimiter.length - 2);
          await write(buffer.subarray(0, count));
          buffer = buffer.subarray(count);
        }
      }
      if (stage === 'done' && buffer.length > 2)
        throw Object.assign(new Error('unexpected multipart data'), { status: 400 });
    }
    if (stage !== 'done' || (buffer.length && buffer.toString() !== '\r\n') ||
        size < 3 || !(prefix.toString() === 'ID3' ||
          (prefix[0] === 0xff && (prefix[1] & 0xe0) === 0xe0)))
      throw Object.assign(new Error('incomplete upload or unsupported MP3 header'), { status: 400 });
    await handle.sync();
    await handle.close(); handle = null;
    await fs.promises.rename(temporary, path.join(AUDIO_DIR, filename));
    return { filename, size };
  } catch (error) {
    error.received_bytes = size;
    throw error;
  } finally {
    if (handle) await handle.close().catch(() => {});
    await fs.promises.unlink(temporary).catch(() => {});
  }
}

function publicAudioUrl(filename) {
  if (!audioFile(filename)) throw new Error('MP3 file is not present in /data/audio');
  const base = tunnelUrl();
  if (!base) throw new Error('public tunnel is not ready');
  return `${base}/audio/${TOKEN}/${filename}`;
}


  const handleChunkUpload = createChunkUploads({ audioDir: AUDIO_DIR, token: TOKEN, tokenOk,
    validAudioName, publicAudioUrl, getPodcasts, savePodcasts, send });
  const handleTranscript = createTranscriptRoutes({ token: TOKEN, tokenOk, audioFile,
    getPodcasts, savePodcasts, send });

  async function handle(req, res) {
    if (await handleChunkUpload(req, res)) return true;
    if (await handleTranscript(req, res)) return true;
    const url = new URL(req.url, 'http://localhost');
    const parts = url.pathname.split('/').filter(Boolean);
    const recognized = (url.pathname === '/podcast/audio' && req.method === 'POST') ||
      (parts[0] === 'audio' && ['GET', 'HEAD'].includes(req.method));
    if (!recognized) return false;
    try {
    if (url.pathname === '/podcast/audio' && req.method === 'POST') {
      const auth = /^Bearer (.+)$/u.exec(req.headers.authorization || '');
      if (!tokenOk(auth?.[1])) return sendHandled(res, 401, { ok: false, error: 'invalid bearer token' });
      if (!tunnelUrl()) return sendHandled(res, 503, { ok: false, error: 'public tunnel is not ready' });
      const podcasts = getPodcasts();
      const episodeId = url.searchParams.get('episode_id');
      const episode = episodeId === null ? null : podcasts.episodes.find((e) => String(e.id) === episodeId);
      if (episodeId !== null && !episode)
        return sendHandled(res, 404, { ok: false, error: 'episode_id not found' });
      if (audioUploads >= 2) return sendHandled(res, 429, { ok: false, error: 'audio upload busy' });
      ++audioUploads;
      const started = Date.now();
      console.log('muse audio', JSON.stringify({ event: 'multipart_started', episode_id: episodeId }));
      try {
        const uploaded = await receiveAudio(req);
        const audio_url = publicAudioUrl(uploaded.filename);
        if (episode) {
          const previousUrl = episode.audio_url, previousTranscript = episode.transcript;
          episode.audio_url = audio_url;
          delete episode.transcript;
          try { savePodcasts(); }
          catch (error) {
            episode.audio_url = previousUrl;
            if (previousTranscript !== undefined) episode.transcript = previousTranscript;
            throw error;
          }
        }
        console.log('muse audio', JSON.stringify({ event: 'multipart_completed', ...uploaded,
          episode_id: episode?.id ?? null, duration_ms: Date.now() - started }));
        return sendHandled(res, 200, { ok: true, ...uploaded, audio_url, episode_id: episode?.id ?? null });
      } catch (error) {
        console.log('muse audio', JSON.stringify({ event: 'multipart_failed', episode_id: episodeId,
          error: error.message, received_bytes: error.received_bytes, duration_ms: Date.now() - started }));
        throw error;
      } finally { --audioUploads; }
    }

    if (parts[0] === 'audio' && ['GET', 'HEAD'].includes(req.method)) {
      if (!tokenOk(parts[1]) || parts.length !== 3) return sendHandled(res, 404, 'not found');
      const audio = audioFile(parts[2]);
      if (!audio) return sendHandled(res, 404, 'not found');
      let start = 0, end = audio.size - 1, partial = false;
      if (req.headers.range) {
        const match = /^bytes=(\d*)-(\d*)$/u.exec(req.headers.range);
        if (!match || (!match[1] && !match[2]))
          return sendHandled(res, 416, '', { 'Content-Range': `bytes */${audio.size}` });
        start = match[1] ? Number(match[1]) : Math.max(0, audio.size - Number(match[2]));
        end = match[1] && match[2] ? Math.min(end, Number(match[2])) : end;
        if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start > end ||
            start < 0 || start >= audio.size)
          return sendHandled(res, 416, '', { 'Content-Range': `bytes */${audio.size}` });
        partial = true;
      }
      const headers = { 'Content-Type': 'audio/mpeg', 'Content-Length': Math.max(0, end - start + 1),
        'Accept-Ranges': 'bytes', 'Cache-Control': 'private, no-store' };
      if (partial) headers['Content-Range'] = `bytes ${start}-${end}/${audio.size}`;
      res.writeHead(partial ? 206 : 200, headers);
      if (req.method === 'HEAD' || audio.size === 0) {
        res.end();
        return true;
      }
      const stream = fs.createReadStream(audio.file, { start, end });
      stream.on('error', () => res.destroy());
      res.on('close', () => stream.destroy());
      stream.pipe(res);
      return true;
    }


    } catch (error) {
      send(res, error.status || 500, { ok: false, error: error.message });
      return true;
    }
    return false;
  }
  // The HTTP handler returns true once it has handled a route.
  handle.publicAudioUrl = publicAudioUrl;
  handle.refreshAudioUrl = (value) => {
    try {
      const url = new URL(value);
      const prefix = `/audio/${TOKEN}/`;
      if (url.pathname.startsWith(prefix)) return publicAudioUrl(url.pathname.slice(prefix.length));
    } catch {}
    return value;
  };
  return handle;
}
module.exports = { createAudioRoutes };
