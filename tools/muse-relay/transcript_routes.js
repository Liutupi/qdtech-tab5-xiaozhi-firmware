'use strict';
const fs = require('fs');
const crypto = require('crypto');

const fail = (status, message) => Object.assign(new Error(message), { status });

function validateCues(value) {
  if (!Array.isArray(value) || !value.length || value.length > 200)
    throw fail(400, 'cues must contain 1..200 timed sentences');
  let previousEnd = 0, bytes = 0;
  return value.map(cue => {
    if (!cue || !Number.isInteger(cue.start_ms) || !Number.isInteger(cue.end_ms) ||
        cue.start_ms < previousEnd || cue.start_ms >= cue.end_ms || cue.end_ms > 14400000 ||
        typeof cue.text !== 'string' || !cue.text.trim() || cue.text.includes('\0') ||
        (cue.kind !== undefined && !['speech', 'music'].includes(cue.kind)))
      throw fail(400, 'each cue needs ordered, non-overlapping millisecond times and spoken text');
    const size = Buffer.byteLength(cue.text);
    bytes += size;
    if (size > 1536 || bytes > 9000)
      throw fail(400, 'cue text exceeds 1536 bytes per sentence or 9000 bytes in total');
    previousEnd = cue.end_ms;
    return { start_ms: cue.start_ms, end_ms: cue.end_ms, text: cue.text,
      ...(cue.kind !== undefined ? { kind: cue.kind } : {}) };
  });
}

function createTranscriptRoutes({ token, tokenOk, audioFile, getPodcasts, savePodcasts, send }) {
  let running = 0;
  return async (req, res) => {
    const url = new URL(req.url, 'http://localhost');
    if (url.pathname !== '/podcast/transcript' || req.method !== 'POST') return false;
    let acquired = false;
    try {
      if (!tokenOk(/^Bearer (.+)$/u.exec(req.headers.authorization || '')?.[1]))
        throw fail(401, 'invalid bearer token');
      if (running >= 2) throw fail(429, 'transcript upload busy');
      if (Number(req.headers['content-length'] || 0) > 32768)
        throw fail(413, 'transcript exceeds 32 KiB');
      ++running; acquired = true;
      req.setTimeout(15000, () => req.destroy());
      let body = '';
      // Keep UTF-8 intact across request chunks and bound bytes independently.
      let bytes = 0; const parts = [];
      for await (const piece of req.iterator({ destroyOnReturn: false })) {
        bytes += piece.length;
        if (bytes > 32768) throw fail(413, 'transcript exceeds 32 KiB');
        parts.push(piece);
      }
      body = Buffer.concat(parts).toString('utf8');
      let value;
      try { value = JSON.parse(body); } catch { throw fail(400, 'invalid transcript JSON'); }
      if (!value || !/^[0-9a-f]{64}$/u.test(value.audio_sha256))
        throw fail(400, 'audio_sha256 must match the uploaded playback MP3 receipt');
      let cueInput = value.cues;
      if (value.lines !== undefined) {
        if (!Array.isArray(value.lines)) throw fail(400, 'lines must be a timed transcript array');
        cueInput = value.lines.map(line => {
          if (!line || !Number.isFinite(line.start) || !Number.isFinite(line.end) ||
              line.start < 0 || line.start >= line.end || line.end > 14400 ||
              !['speech', 'music'].includes(line.kind))
            throw fail(400, 'lines need second timestamps and speech/music kind');
          return { start_ms: Math.round(line.start * 1000), end_ms: Math.round(line.end * 1000),
            text: line.text, kind: line.kind };
        });
      }
      const cues = validateCues(cueInput);
      const id = url.searchParams.get('episode_id');
      const episode = getPodcasts().episodes.find(e => String(e.id) === id);
      if (!episode) throw fail(404, 'episode_id not found');
      const previousUrl = episode.audio_url;
      let pathname;
      try { pathname = new URL(previousUrl).pathname; } catch { throw fail(409, 'upload episode audio first'); }
      const prefix = `/audio/${token}/`;
      if (!pathname.startsWith(prefix)) throw fail(409, 'episode audio must be hosted by this relay');
      const filename = pathname.slice(prefix.length), audio = audioFile(filename);
      if (!audio) throw fail(409, 'episode audio is missing');
      const before = await fs.promises.stat(audio.file);
      const hash = crypto.createHash('sha256');
      for await (const chunk of fs.createReadStream(audio.file)) hash.update(chunk);
      if (hash.digest('hex') !== value.audio_sha256)
        throw fail(409, 'transcript belongs to a different audio file');
      const after = await fs.promises.stat(audio.file);
      if (before.ino !== after.ino || before.size !== after.size || before.mtimeMs !== after.mtimeMs ||
          episode.audio_url !== previousUrl || !getPodcasts().episodes.includes(episode))
        throw fail(409, 'episode audio changed; retry with the current audio receipt');
      const previous = episode.transcript;
      episode.transcript = { audio_filename: filename, audio_sha256: value.audio_sha256, cues };
      try { savePodcasts(); }
      catch (error) {
        if (previous === undefined) delete episode.transcript;
        else episode.transcript = previous;
        throw error;
      }
      console.log('muse transcript', JSON.stringify({ episode_id: episode.id, cue_count: cues.length }));
      send(res, 200, { ok: true, episode_id: episode.id, cue_count: cues.length });
    } catch (error) {
      send(res, error.status || 500, { ok: false, error: error.message });
    } finally { if (acquired) --running; }
    return true;
  };
}

module.exports = { createTranscriptRoutes, validateCues };
