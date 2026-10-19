'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const { execFileSync } = require('node:child_process');
const { createStream96Routes, byteRange, publicAddress, Mp3Frames } = require('./stream96_routes');

test('Range and IP boundaries reject malformed and private destinations', () => {
  assert.deepEqual(byteRange('bytes=20-29', 100), { first: 20, last: 29, partial: true });
  assert.deepEqual(byteRange('bytes=-10', 100), { first: 90, last: 99, partial: true });
  assert.equal(byteRange('bytes=20-', 100).last, 99);
  for (const h of ['bytes=100-', 'bytes=9-8', 'bytes=-0', 'bytes=1-2,4-5', 'bytes=x-', 'bytes=999999999999999999999-'])
    assert.throws(() => byteRange(h, 100));
  for (const ip of ['127.0.0.1', '10.3.2.1', '169.254.169.254', '172.18.0.1', '192.168.88.100',
    '100.64.0.1', '0.0.0.0', '224.0.0.1', '::1', '::ffff:127.0.0.1', 'fe80::1',
    '2001:db8::1', '2001:20::1', '2002:7f00:1::1', '64:ff9b::7f00:1']) assert.equal(publicAddress(ip), false, ip);
  for (const ip of ['1.1.1.1', '8.8.8.8', '2606:4700:4700::1111']) assert.equal(publicAddress(ip), true, ip);
});

test('installer preserves the current relay and backs up exactly once', () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tab5-stream96-install-'));
  try {
    const file = path.join(root, 'relay.js');
    const source = `const http = require('http');
const TOKEN = 'test-token-kept';
const DATA_DIR = '/data';
function tunnelUrl() { return ''; }
async function existingSongRoute(req, res) { return false; }
const server = http.createServer(async (req, res) => {
  if (await existingSongRoute(req, res)) return;
  res.end('existing response');
});\n`;
    fs.writeFileSync(file, source);
    fs.copyFileSync(path.join(__dirname, 'stream96_routes.js'), path.join(root, 'stream96_routes.js'));
    const install = () => execFileSync(process.execPath, [path.join(__dirname, 'enable_stream96.js'), file]);
    install();
    const updated = fs.readFileSync(file, 'utf8');
    assert.ok(updated.includes("const TOKEN = 'test-token-kept'"));
    assert.ok(updated.includes("res.end('existing response')"));
    assert.ok(updated.indexOf('await handleMuseStream96(req, res)') < updated.indexOf('await existingSongRoute(req, res)'));
    const backups = fs.readdirSync(root).filter(f => f.endsWith('.bak'));
    assert.equal(backups.length, 1);
    assert.equal(fs.readFileSync(path.join(root, backups[0]), 'utf8'), source);
    install(); assert.equal(fs.readFileSync(file, 'utf8'), updated);
    assert.equal(fs.readdirSync(root).filter(f => f.endsWith('.bak')).length, 1);
    fs.writeFileSync(file, '/* unsupported server layout */');
    assert.throws(install);
    assert.equal(fs.readFileSync(file, 'utf8'), '/* unsupported server layout */');
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

test('real ffmpeg conversion preserves L/R, caches and serves exact Range bytes', async t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tab5-stream96-'));
  const token = 'local-test-token-0123456789';
  fs.mkdirSync(path.join(root, 'audio'));
  const source = path.join(root, 'audio', 'stereo.mp3');
  execFileSync('ffmpeg', ['-v', 'error', '-f', 'lavfi', '-i',
    'aevalsrc=0.2*sin(2*PI*440*t)|0.2*sin(2*PI*880*t):s=48000:d=4',
    '-c:a', 'libmp3lame', '-b:a', '320k', source]);
  const handler = createStream96Routes({ dataDir: root, token, selfOrigins: ['https://relay.example'] });
  const server = http.createServer(async (req, res) => { if (!await handler(req, res)) { res.writeHead(404); res.end(); } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => {
    await handler.close(); server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
    fs.rmSync(root, { recursive: true, force: true });
  });
  const base = `http://127.0.0.1:${server.address().port}`;
  const route = `/podcast/${token}/stream96?url=${encodeURIComponent(`https://relay.example/audio/${token}/stereo.mp3`)}&live=0`;
  const [a, b] = await Promise.all([fetch(base + route), fetch(base + route)]);
  assert.equal(a.status, 200); assert.equal(b.status, 200);
  const audio = Buffer.from(await a.arrayBuffer());
  assert.deepEqual(Buffer.from(await b.arrayBuffer()), audio);
  assert.equal(Number(a.headers.get('content-length')), audio.length);
  assert.equal(a.headers.get('x-audio-channels'), '2');
  const encoded = path.join(root, 'out.mp3'); fs.writeFileSync(encoded, audio);
  const probe = JSON.parse(execFileSync('ffprobe', ['-v', 'error', '-show_streams', '-of', 'json', encoded]));
  assert.equal(probe.streams[0].sample_rate, '44100');
  assert.equal(probe.streams[0].channels, 2); assert.equal(probe.streams[0].bit_rate, '96000');
  // Validate every frame including arbitrary input chunk boundaries.
  const frames = new Mp3Frames(1000000); frames.resume();
  const validated = new Promise((resolve, reject) => { frames.on('end', resolve); frames.on('error', reject); });
  for (let i = 0; i < audio.length; i += 79) frames.write(audio.subarray(i, i + 79));
  frames.end(); await validated; assert.ok(frames.frames > 100);
  const pcm = execFileSync('ffmpeg', ['-v', 'error', '-i', encoded, '-f', 's16le', '-acodec', 'pcm_s16le', 'pipe:1']);
  function power(channel, hz) {
    let re = 0, im = 0;
    for (let i = 4410; i < 44100; ++i) {
      const sample = pcm.readInt16LE(i * 4 + channel * 2), angle = 2 * Math.PI * hz * i / 44100;
      re += sample * Math.cos(angle); im += sample * Math.sin(angle);
    }
    return re * re + im * im;
  }
  assert.ok(power(0, 440) > power(0, 880) * 1000, 'left must remain left');
  assert.ok(power(1, 880) > power(1, 440) * 1000, 'right must remain right');
  for (const [range, start, end] of [['bytes=1000-1999', 1000, 2000], ['bytes=19000-', 19000, audio.length]]) {
    const r = await fetch(base + route, { headers: { Range: range } });
    assert.equal(r.status, 206); assert.equal(r.headers.get('content-range'), `bytes ${start}-${end - 1}/${audio.length}`);
    assert.deepEqual(Buffer.from(await r.arrayBuffer()), audio.subarray(start, end));
  }
  const head = await fetch(base + route, { method: 'HEAD' });
  assert.equal(head.status, 200); assert.equal(head.headers.get('content-length'), String(audio.length));
  assert.equal((await head.arrayBuffer()).byteLength, 0);
  const badRange = await fetch(base + route, { headers: { Range: 'bytes=999999999-' } });
  assert.equal(badRange.status, 416); assert.equal(badRange.headers.get('content-range'), `bytes */${audio.length}`);
  assert.equal(fs.readdirSync(path.join(root, 'audio96-cache')).filter(f => f.endsWith('.mp3')).length, 1);

  assert.equal((await fetch(base + route.replace(token, 'wrong'))).status, 404);
  assert.equal((await fetch(base + route.replace(`/podcast/${token}`, '/tab5/podcast'), { headers: { 'cf-ray': 'test' } })).status, 404);
  for (const sourceUrl of ['http://127.0.0.1/private', 'http://[::1]/private', 'http://169.254.169.254/token']) {
    const r = await fetch(`${base}/podcast/${token}/stream96?live=0&url=${encodeURIComponent(sourceUrl)}`);
    assert.equal(r.status, 403);
  }
  for (const sourceUrl of ['file:///etc/passwd', 'https://relay.example/podcast/x/stream96?live=0', 'http://a:b@public.example/x']) {
    assert.equal((await fetch(`${base}/podcast/${token}/stream96?live=0&url=${encodeURIComponent(sourceUrl)}`)).status, 400);
  }
  const live = await fetch(base + route.replace('live=0', 'live=1'));
  assert.equal(live.status, 200); assert.equal(live.headers.get('accept-ranges'), 'none');
  assert.equal(live.headers.get('content-length'), null);
  assert.deepEqual(Buffer.from(await live.arrayBuffer()), audio);
  const missing = route.replace('stereo.mp3', 'absent.mp3');
  assert.equal((await fetch(base + missing)).status, 404);
});

test('preparing survives a disconnected waiter; live disconnect releases its slot', async t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tab5-stream96-wait-'));
  const token = 'local-test-token-0123456789';
  fs.mkdirSync(path.join(root, 'audio'));
  execFileSync('ffmpeg', ['-v', 'error', '-f', 'lavfi', '-i', 'sine=frequency=440:duration=4',
    '-c:a', 'libmp3lame', path.join(root, 'audio', 'one.mp3')]);
  const wrapper = path.join(root, 'ffmpeg-slow');
  fs.writeFileSync(wrapper, '#!/bin/sh\nexec ffmpeg -re "$@"\n', { mode: 0o700 });
  const handler = createStream96Routes({ dataDir: root, token, selfOrigins: ['https://relay.example'],
    maxJobs: 1, prepareMs: 10, ffmpeg: wrapper });
  const server = http.createServer(async (req, res) => { await handler(req, res); });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => {
    await handler.close(); server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
    fs.rmSync(root, { recursive: true, force: true });
  });
  const route = `http://127.0.0.1:${server.address().port}/podcast/${token}/stream96?url=${encodeURIComponent(`https://relay.example/audio/${token}/one.mp3`)}&live=0`;
  const first = await fetch(route);
  assert.equal(first.status, 503); assert.equal((await first.json()).code, 'preparing');
  let result;
  for (let i = 0; i < 60; ++i) {
    await new Promise(r => setTimeout(r, 100));
    result = await fetch(route);
    if (result.status === 200) break;
    assert.equal(result.status, 503); await result.arrayBuffer();
  }
  assert.equal(result.status, 200); await result.arrayBuffer();
  const controller = new AbortController();
  const live = await fetch(route.replace('live=0', 'live=1'), { signal: controller.signal });
  assert.equal(live.status, 200);
  controller.abort();
  let second;
  for (let i = 0; i < 30; ++i) {
    await new Promise(r => setTimeout(r, 100));
    second = await fetch(route.replace('live=0', 'live=1'));
    if (second.status === 200) break;
    assert.equal(second.status, 503); await second.arrayBuffer();
  }
  assert.equal(second.status, 200); await second.body.cancel();
});
