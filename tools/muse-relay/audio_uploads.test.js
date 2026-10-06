'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn, spawnSync } = require('node:child_process');
const { createAudioRoutes } = require('./audio_routes');
const sha = data => crypto.createHash('sha256').update(data).digest('hex');
const token = 'fixture-token-never-used-in-production';

async function fixture(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'muse-audio-test-'));
  const f = { dir, saved: 0, base: 'https://relay.example',
    podcasts: { episodes: [{ id: 4, title: 'Existing episode', audio_url: '' }] } };
  f.open = async () => {
    f.route = createAudioRoutes({ dataDir: dir, token, tunnelUrl: () => f.base,
      getPodcasts: () => f.podcasts, savePodcasts: () => { f.saved++; } });
    f.server = http.createServer(async (req, res) => {
      if (!await f.route(req, res)) { res.writeHead(404); res.end(); }
    });
    await new Promise(resolve => f.server.listen(0, '127.0.0.1', resolve));
    f.port = f.server.address().port;
  };
  f.close = async () => new Promise(resolve => f.server.close(resolve));
  await f.open();
  t.after(async () => { await f.close(); fs.rmSync(dir, { recursive: true }); });
  f.request = (method, route, data, headers = {}, raw = false) => new Promise((resolve, reject) => {
    let body = Buffer.isBuffer(data) ? data : data === undefined ? null : Buffer.from(JSON.stringify(data));
    const req = http.request({ host: '127.0.0.1', port: f.port, method, path: route,
      headers: { Authorization: `Bearer ${token}`, ...(body ? { 'Content-Length': body.length } : {}), ...headers } }, res => {
      const chunks = [];
      res.on('data', chunk => chunks.push(chunk));
      res.on('end', () => {
        const bytes = Buffer.concat(chunks);
        resolve({ status: res.statusCode, headers: res.headers,
          body: raw || !bytes.length ? bytes : JSON.parse(bytes.toString()) });
      });
      res.on('error', reject);
    });
    req.on('error', reject); req.end(body);
  });
  f.begin = async (audio, extra = {}) => {
    const response = await f.request('POST', '/podcast/audio/uploads',
      { filename: 'episode.mp3', size: audio.length, sha256: sha(audio), episode_id: 4, ...extra });
    assert.equal(response.status, 200);
    const state = response.body;
    return { state, route: '/podcast/audio/uploads/' + state.upload_id };
  };
  f.chunk = (upload, audio, index, header = {}) => {
    const chunk = audio.subarray(index * upload.state.chunk_size, (index + 1) * upload.state.chunk_size);
    return f.request('PUT', upload.route + '/chunks/' + index, chunk,
      { 'X-Chunk-SHA256': sha(chunk), ...header });
  };
  return f;
}

test('resume after restart and lost response; complete once, refresh URLs, preserve range playback', async t => {
  const f = await fixture(t), audio = Buffer.concat([Buffer.from('ID3'), crypto.randomBytes(1600000)]);
  const upload = await f.begin(audio), again = await f.begin(audio);
  assert.equal(again.state.upload_id, upload.state.upload_id);
  assert.equal((await f.request('POST', upload.route + '/complete', {})).status, 409);
  assert.equal((await f.chunk(upload, audio, 0)).status, 200);
  assert.equal((await f.chunk(upload, audio, 0)).status, 200); // same chunk after an ACK was lost
  await f.close(); await f.open();
  const resumed = (await f.request('GET', upload.route)).body;
  assert.deepEqual(resumed.received, [0]);
  assert.equal(resumed.received_bytes, upload.state.chunk_size);
  for (const index of resumed.missing.reverse()) assert.equal((await f.chunk(upload, audio, index)).status, 200);
  const done = await f.request('POST', upload.route + '/complete', {});
  assert.equal(done.status, 200); assert.equal(done.body.sha256, sha(audio));
  assert.equal(done.body.size, audio.length); assert.equal(done.body.episode_id, 4);
  assert.equal(f.saved, 1); assert.equal(f.podcasts.episodes[0].audio_url, done.body.audio_url);
  assert.deepEqual(fs.readFileSync(path.join(f.dir, 'audio', done.body.filename)), audio);
  assert.equal((await f.request('POST', upload.route + '/complete', {})).status, 200);
  assert.equal(f.saved, 1);
  f.base = 'https://new-tunnel.example';
  assert.ok((await f.request('GET', upload.route)).body.result.audio_url.startsWith(f.base));
  const url = new URL(done.body.audio_url);
  const ranged = await f.request('GET', url.pathname, undefined, { Range: 'bytes=1-31' }, true);
  assert.equal(ranged.status, 206); assert.deepEqual(ranged.body, audio.subarray(1, 32));
  const head = await f.request('HEAD', url.pathname, undefined, {}, true);
  assert.equal(head.status, 200); assert.equal(Number(head.headers['content-length']), audio.length);
});

test('authorization, bounded metadata, mismatched chunks and whole hashes never publish audio', async t => {
  const f = await fixture(t), audio = Buffer.concat([Buffer.from('ID3'), crypto.randomBytes(600000)]);
  assert.equal((await f.request('POST', '/podcast/audio/uploads', {}, { Authorization: 'Bearer wrong' })).status, 401);
  assert.equal((await f.request('POST', '/podcast/audio/uploads', { filename: '../bad.mp3' })).status, 400);
  assert.equal((await f.request('POST', '/podcast/audio/uploads', Buffer.alloc(5000))).status, 413);
  const upload = await f.begin(audio, { sha256: '0'.repeat(64) });
  assert.equal((await f.chunk(upload, audio, 0, { 'X-Chunk-SHA256': '1'.repeat(64) })).status, 400);
  assert.deepEqual((await f.request('GET', upload.route)).body.received, []);
  for (const index of upload.state.missing) assert.equal((await f.chunk(upload, audio, index)).status, 200);
  assert.equal((await f.request('POST', upload.route + '/complete', {})).status, 400);
  assert.equal(f.podcasts.episodes[0].audio_url, ''); assert.equal(f.saved, 0);
  assert.deepEqual(fs.readdirSync(path.join(f.dir, 'audio')).filter(n => n.endsWith('.mp3')), []);
});

test('aborted chunk cleans partial data and can be retried without discarding committed chunks', async t => {
  const f = await fixture(t), audio = Buffer.concat([Buffer.from('ID3'), crypto.randomBytes(700000)]);
  const upload = await f.begin(audio);
  assert.equal((await f.chunk(upload, audio, 1)).status, 200);
  await new Promise(resolve => {
    const req = http.request({ host: '127.0.0.1', port: f.port, method: 'PUT', path: upload.route + '/chunks/0',
      headers: { Authorization: `Bearer ${token}`, 'Content-Length': upload.state.chunk_size,
        'X-Chunk-SHA256': sha(audio.subarray(0, upload.state.chunk_size)) } });
    req.on('error', resolve); req.write(audio.subarray(0, 100)); setTimeout(() => req.destroy(), 20);
  });
  for (let attempt = 0; attempt < 50; attempt++) {
    const response = await f.chunk(upload, audio, 0);
    if (response.status === 429) { await new Promise(resolve => setTimeout(resolve, 10)); continue; }
    assert.equal(response.status, 200); break;
  }
  const state = (await f.request('GET', upload.route)).body;
  assert.deepEqual(state.missing, []);
  assert.equal((await f.request('POST', upload.route + '/complete', {})).status, 200);
  assert.equal(f.saved, 1);
});

test('corrupt disk chunks preserve the old episode and multipart remains compatible', async t => {
  const f = await fixture(t), audio = Buffer.from('ID3test-payload');
  f.podcasts.episodes[0].audio_url = 'https://old.example/old.mp3';
  const upload = await f.begin(audio);
  assert.equal((await f.chunk(upload, audio, 0)).status, 200);
  fs.writeFileSync(path.join(f.dir, 'audio', '.uploads', upload.state.upload_id, '0.part'), Buffer.alloc(audio.length));
  assert.equal((await f.request('POST', upload.route + '/complete', {})).status, 400);
  assert.equal(f.podcasts.episodes[0].audio_url, 'https://old.example/old.mp3');
  assert.equal(f.saved, 0);
  const boundary = 'test-boundary';
  const body = Buffer.concat([Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="file"; filename="legacy.mp3"\r\n\r\n`),
    audio, Buffer.from(`\r\n--${boundary}--\r\n`)]);
  assert.equal((await f.request('POST', '/podcast/audio', body,
    { 'Content-Type': 'multipart/form-data; boundary=' + boundary })).status, 200);
  assert.equal(f.saved, 0); // test audio without an episode never alters a published episode
});

test('downloadable Python client resumes an existing upload and saves a matching receipt', async t => {
  const f = await fixture(t), audio = Buffer.concat([Buffer.from('ID3'), crypto.randomBytes(1100000)]);
  const script = await f.request('GET', '/podcast/audio/uploads/client', undefined, {}, true);
  assert.equal(script.status, 200); assert.ok(script.body.includes(Buffer.from('class Uploader:')));
  const upload = await f.begin(audio);
  assert.equal((await f.chunk(upload, audio, 0)).status, 200);
  const file = path.join(f.dir, 'episode.mp3'), receipt = path.join(f.dir, 'receipt.json');
  fs.writeFileSync(file, audio);
  const child = spawn('python3', [path.join(__dirname, 'upload_audio.py'), file, '--episode-id', '4',
    '--result-file', receipt], { env: { ...process.env, MUSE_MCP_URL: `http://127.0.0.1:${f.port}/mcp/${token}` } });
  let output = '', error = '';
  child.stdout.on('data', value => { output += value; });
  child.stderr.on('data', value => { error += value; });
  const code = await new Promise(resolve => child.on('exit', resolve));
  assert.equal(code, 0, error); assert.ok(!output.includes('Chunk 1/'));
  assert.ok(!output.includes(token)); assert.ok(output.includes('Chunk 2/'));
  const result = JSON.parse(fs.readFileSync(receipt));
  assert.equal(result.size, audio.length); assert.equal(result.sha256, sha(audio));
  assert.equal(result.episode_id, 4); assert.equal(f.saved, 1);
});

test('speech client uploads a smaller 24 kHz mono copy, preserving the original and failed conversion', async t => {
  if (spawnSync('ffmpeg', ['-version']).status !== 0 || spawnSync('ffprobe', ['-version']).status !== 0) {
    t.skip('ffmpeg/ffprobe unavailable'); return;
  }
  const f = await fixture(t), source = path.join(f.dir, 'source.mp3'), receipt = path.join(f.dir, 'speech.json');
  const transcript = path.join(f.dir, 'spoken.srt');
  fs.writeFileSync(transcript, '1\n00:00:00,100 --> 00:00:01,800\n今天的电台。\n');
  const generated = spawnSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-nostdin',
    '-f', 'lavfi', '-i', 'sine=frequency=440:duration=2', '-ac', '2', '-ar', '44100',
    '-c:a', 'libmp3lame', '-b:a', '128k', source]);
  assert.equal(generated.status, 0, generated.stderr.toString());
  const original = fs.readFileSync(source);
  async function run(file) {
    const child = spawn('python3', [path.join(__dirname, 'upload_audio.py'), file, '--episode-id', '4',
      '--speech-optimize', '--result-file', receipt, '--transcript', transcript],
      { env: { ...process.env, MUSE_MCP_URL: `http://127.0.0.1:${f.port}/mcp/${token}` } });
    let output = ''; child.stdout.on('data', v => { output += v; }); child.stderr.on('data', v => { output += v; });
    return { code: await new Promise(resolve => child.on('exit', resolve)), output };
  }
  const uploaded = await run(source);
  assert.equal(uploaded.code, 0, uploaded.output); assert.ok(!uploaded.output.includes(token));
  assert.deepEqual(fs.readFileSync(source), original);
  const result = JSON.parse(fs.readFileSync(receipt)), stored = path.join(f.dir, 'audio', result.filename);
  assert.ok(result.size < original.length / 2); assert.equal(f.saved, 2);
  assert.equal(f.podcasts.episodes[0].transcript.audio_sha256, result.sha256);
  const probed = spawnSync('ffprobe', ['-v', 'error', '-show_entries',
    'stream=sample_rate,channels,bit_rate:format=duration', '-of', 'json', stored]);
  const media = JSON.parse(probed.stdout);
  assert.equal(media.streams[0].sample_rate, '24000'); assert.equal(media.streams[0].channels, 1);
  assert.equal(probed.status, 0, probed.stderr.toString());
  assert.equal(media.streams[0].bit_rate, '48000');
  assert.ok(Math.abs(Number(media.format.duration) - 2) < 0.15, JSON.stringify(media));
  const previous = f.podcasts.episodes[0].audio_url;
  const invalid = path.join(f.dir, 'broken.mp3'); fs.writeFileSync(invalid, 'ID3invalid');
  assert.notEqual((await run(invalid)).code, 0);
  assert.equal(f.saved, 2); assert.equal(f.podcasts.episodes[0].audio_url, previous);
});

test('timed transcript authentication, audio binding, validation, restart and audio replacement', async t => {
  const f = await fixture(t), audio = Buffer.from('ID3spoken-audio');
  const upload = await f.begin(audio);
  assert.equal((await f.chunk(upload, audio, 0)).status, 200);
  const done = (await f.request('POST', upload.route + '/complete', {})).body;
  const route = '/podcast/transcript?episode_id=4';
  const cues = [{ start_ms: 0, end_ms: 900, text: '早上好。' },
    { start_ms: 1100, end_ms: 2200, text: '这是今天的电台。' }];
  const value = { audio_sha256: done.sha256, cues };
  assert.equal((await f.request('POST', route, value, { Authorization: 'Bearer wrong' })).status, 401);
  assert.equal((await f.request('POST', route, { ...value, audio_sha256: sha('other') })).status, 409);
  for (const bad of [[], [...cues, { start_ms: 500, end_ms: 2500, text: 'overlap' }],
    [{ start_ms: -1, end_ms: 5, text: 'negative' }], [{ start_ms: 1.5, end_ms: 5, text: 'fraction' }],
    [{ start_ms: 0, end_ms: 5, text: 'x'.repeat(1537) }], [{ start_ms: 0, end_ms: 5, text: '\0' }]]) {
    assert.equal((await f.request('POST', route, { ...value, cues: bad })).status, 400);
  }
  assert.equal(f.saved, 1); assert.equal(f.podcasts.episodes[0].transcript, undefined);
  assert.equal((await f.request('POST', route, value)).status, 200);
  assert.deepEqual(f.podcasts.episodes[0].transcript,
    { audio_filename: done.filename, audio_sha256: done.sha256, cues });
  assert.equal(f.podcasts.episodes[0].title, 'Existing episode');
  await f.close(); await f.open();
  assert.equal((await f.request('POST', route, value)).status, 200); // safe retry
  const timeline = { date: '2026-10-06', duration: 2.2, audio_sha256: done.sha256, lines: [
    { start: 0, end: 0.9, kind: 'speech', text: '早上好。' },
    { start: 1.1, end: 2.2, kind: 'music', text: '♪《歌名》— 歌手' }] };
  assert.equal((await f.request('POST', route, timeline)).status, 200);
  assert.deepEqual(f.podcasts.episodes[0].transcript.cues, [
    { start_ms: 0, end_ms: 900, kind: 'speech', text: '早上好。' },
    { start_ms: 1100, end_ms: 2200, kind: 'music', text: '♪《歌名》— 歌手' }]);
  const next = Buffer.from('ID3new-spoken-audio'), second = await f.begin(next);
  assert.equal((await f.chunk(second, next, 0)).status, 200);
  assert.equal((await f.request('POST', second.route + '/complete', {})).status, 200);
  assert.equal(f.podcasts.episodes[0].transcript, undefined);
  assert.equal((await f.request('POST', route, value)).status, 409);
});

test('Python client attaches SRT to the actual uploaded audio and rejects invalid timing before upload', async t => {
  const f = await fixture(t), file = path.join(f.dir, 'spoken.mp3'), subtitles = path.join(f.dir, 'spoken.srt');
  fs.writeFileSync(file, 'ID3actual-spoken-file');
  fs.writeFileSync(subtitles, '1\n00:00:00,000 --> 00:00:01,100\n早上好。\n\n2\n00:00:01,500 --> 00:00:02,300\n第二句。\n');
  const run = async (caption = subtitles) => {
    const child = spawn('python3', [path.join(__dirname, 'upload_audio.py'), file, '--episode-id', '4',
      '--transcript', caption], { env: { ...process.env, MUSE_MCP_URL: `http://127.0.0.1:${f.port}/mcp/${token}` } });
    let output = ''; child.stdout.on('data', v => { output += v; }); child.stderr.on('data', v => { output += v; });
    return { code: await new Promise(resolve => child.on('exit', resolve)), output };
  };
  const good = await run();
  assert.equal(good.code, 0, good.output); assert.ok(!good.output.includes(token));
  assert.equal(f.podcasts.episodes[0].transcript.audio_sha256, sha(fs.readFileSync(file)));
  assert.deepEqual(f.podcasts.episodes[0].transcript.cues, [
    { start_ms: 0, end_ms: 1100, text: '早上好。' }, { start_ms: 1500, end_ms: 2300, text: '第二句。' }]);
  const native = path.join(f.dir, 'spoken.timeline.json');
  fs.writeFileSync(native, JSON.stringify({ date: '2026-10-06', duration: 2.3, lines: [
    { start: 0, end: 1.1, kind: 'speech', text: '早上好。' },
    { start: 1.5, end: 2.3, kind: 'music', text: '♪《歌名》— 歌手' }] }));
  const nativeResult = await run(native);
  assert.equal(nativeResult.code, 0, nativeResult.output);
  assert.equal(f.podcasts.episodes[0].transcript.cues[1].kind, 'music');
  assert.equal(f.podcasts.episodes[0].transcript.cues[1].start_ms, 1500);
  const saved = f.saved, previous = JSON.stringify(f.podcasts);
  fs.writeFileSync(subtitles, '1\n00:00:01,000 --> 00:00:00,100\n坏时间。\n');
  assert.notEqual((await run()).code, 0); assert.equal(f.saved, saved);
  assert.equal(JSON.stringify(f.podcasts), previous);
});
