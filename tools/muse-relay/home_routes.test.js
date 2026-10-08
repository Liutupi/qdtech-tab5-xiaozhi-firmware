'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { execFileSync } = require('child_process');
const { createHomeRoutes } = require('./home_routes');

const TOKEN = 'relay-token-0123456789';
const HA_TOKEN = 'ha-secret-token';

function fakeHa() {
  const calls = [];
  const fetchImpl = async (url, options) => {
    calls.push({ url, options, body: options.body ? JSON.parse(options.body) : undefined });
    if (options.headers.Authorization !== `Bearer ${HA_TOKEN}`) return new Response('', { status: 401 });
    if (url.endsWith('/api/template')) {
      return new Response(JSON.stringify([
        { id: 'switch.bedroom_pc', name: '卧室电脑', state: 'off', area: '卧室', brightness: null },
        { id: 'light.monitor_bar', name: '显示器挂灯', state: 'on', area: '卧室', brightness: 128 },
        { id: 'media_player.tv', name: '客厅电视', state: 'off', area: '客厅' },
      ]), { status: 200 });
    }
    return new Response('[]', { status: 200 });
  };
  return { calls, fetchImpl };
}

async function withServer(options, fn) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'home-'));
  if (options.token !== false) fs.writeFileSync(path.join(dir, 'ha_token.txt'), HA_TOKEN + '\n');
  if (options.scenes) fs.writeFileSync(path.join(dir, 'home_scenes.json'), JSON.stringify(options.scenes));
  const handle = createHomeRoutes({ dataDir: dir, token: TOKEN, haUrl: 'http://ha.test:8123',
    fetchImpl: options.fetchImpl });
  const server = http.createServer(async (req, res) => {
    if (!(await handle(req, res))) { res.writeHead(418); res.end(); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  try {
    await fn(base, dir);
  } finally {
    server.close();
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

const post = (url, body, headers = {}) => fetch(url, {
  method: 'POST', headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(body),
});

test('lists devices by room with scenes and never returns the HA token', async () => {
  const ha = fakeHa();
  const scenes = { scenes: [{ id: 'bedroom_pc', name: '卧室电脑', on: [{ entity_id: 'switch.bedroom_pc' }],
    off: [{ entity_id: 'switch.bedroom_pc' }, { entity_id: 'bogus.thing' }] }] };
  await withServer({ fetchImpl: ha.fetchImpl, scenes }, async base => {
    const response = await fetch(`${base}/home/${TOKEN}/devices`);
    const text = await response.text();
    assert.strictEqual(response.status, 200);
    assert.ok(!text.includes(HA_TOKEN));
    const body = JSON.parse(text);
    // Rooms, then names, in pinyin order: 客厅 (kè) before 卧室 (wò); 卧室电脑 before 显示器挂灯.
    assert.deepStrictEqual(body.devices.map(d => d.id), ['media_player.tv', 'switch.bedroom_pc', 'light.monitor_bar']);
    assert.strictEqual(body.devices[2].brightness, 128);
    assert.ok(!('brightness' in body.devices[1]));
    assert.strictEqual(body.scenes[0].off.length, 1);  // unknown domain dropped
    assert.strictEqual(ha.calls[0].url, 'http://ha.test:8123/api/template');
  });
});

test('rejects a wrong relay token and unknown home paths', async () => {
  const ha = fakeHa();
  await withServer({ fetchImpl: ha.fetchImpl }, async base => {
    assert.strictEqual((await fetch(`${base}/home/wrong/devices`)).status, 404);
    assert.strictEqual((await fetch(`${base}/other`)).status, 418);
    assert.strictEqual(ha.calls.length, 0);
  });
});

test('controls a device on the LAN and maps brightness', async () => {
  const ha = fakeHa();
  await withServer({ fetchImpl: ha.fetchImpl }, async base => {
    let r = await post(`${base}/home/${TOKEN}/control`, { entity_id: 'light.monitor_bar', action: 'brightness', value: 140 });
    assert.strictEqual(r.status, 200);
    assert.strictEqual(ha.calls[0].url, 'http://ha.test:8123/api/services/light/turn_on');
    assert.deepStrictEqual(ha.calls[0].body, { entity_id: 'light.monitor_bar', brightness_pct: 100 });
    r = await post(`${base}/home/${TOKEN}/control`, { entity_id: 'switch.bedroom_pc', action: 'brightness' });
    assert.strictEqual(r.status, 400);
    r = await post(`${base}/home/${TOKEN}/control`, { entity_id: 'lock.front_door', action: 'off' });
    assert.strictEqual(r.status, 400);  // locks are not exposed
  });
});

test('tunnel control needs the paired home key; the relay token alone only reads', async () => {
  const ha = fakeHa();
  await withServer({ fetchImpl: ha.fetchImpl }, async (base, dir) => {
    const tunnel = { 'cf-connecting-ip': '203.0.113.9' };
    const on = { entity_id: 'switch.bedroom_pc', action: 'on' };
    let r = await post(`${base}/home/${TOKEN}/control`, on, tunnel);
    assert.strictEqual(r.status, 403);
    assert.strictEqual((await fetch(`${base}/home/${TOKEN}/devices`, { headers: tunnel })).status, 200);
    assert.ok(!ha.calls.some(c => c.url.includes('/api/services/')));
    // First pairing hands out the key once.
    r = await post(`${base}/home/${TOKEN}/pair`, {}, tunnel);
    assert.strictEqual(r.status, 200);
    const { key } = await r.json();
    assert.ok(key.length >= 32);
    assert.strictEqual(fs.statSync(path.join(dir, 'home_key.txt')).mode & 0o777, 0o600);
    assert.strictEqual((await post(`${base}/home/${TOKEN}/pair`, {}, tunnel)).status, 409);
    assert.strictEqual((await post(`${base}/home/wrong/pair`, {}, tunnel)).status, 404);
    r = await post(`${base}/home/${TOKEN}/control`, on, { ...tunnel, 'X-Home-Key': 'x'.repeat(key.length) });
    assert.strictEqual(r.status, 403);
    r = await post(`${base}/home/${TOKEN}/control`, on, { ...tunnel, 'X-Home-Key': key });
    assert.strictEqual(r.status, 200);
  });
});

test('pairing closes 30 minutes after the relay starts', async () => {
  const ha = fakeHa();
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'home-pair-'));
  let clock = 1000;
  const handle = createHomeRoutes({ dataDir: dir, token: TOKEN, haUrl: 'http://ha.test', fetchImpl: ha.fetchImpl,
    now: () => clock });
  const server = http.createServer(async (req, res) => { await handle(req, res); });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    clock += 31 * 60 * 1000;
    const r = await post(`http://127.0.0.1:${server.address().port}/home/${TOKEN}/pair`, {});
    assert.strictEqual(r.status, 403);
    assert.ok(!fs.existsSync(path.join(dir, 'home_key.txt')));
  } finally {
    server.close();
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('scene runs every step and reports the ones that failed', async () => {
  const ha = fakeHa();
  const scenes = { scenes: [{ id: 'cinema', name: '家庭影院',
    on: [{ entity_id: 'switch.living_pc' }, { entity_id: 'media_player.tv' }],
    off: [{ entity_id: 'media_player.tv' }, { entity_id: 'switch.living_pc' }] }] };
  const failing = async (url, options) => url.includes('media_player') && url.includes('turn_on')
    ? new Response('', { status: 500 }) : ha.fetchImpl(url, options);
  await withServer({ fetchImpl: failing, scenes }, async base => {
    let r = await post(`${base}/home/${TOKEN}/scene`, { id: 'cinema', action: 'on' });
    assert.strictEqual(r.status, 207);
    assert.deepStrictEqual((await r.json()).failed, [{ entity_id: 'media_player.tv', error: 'ha_http_500' }]);
    r = await post(`${base}/home/${TOKEN}/scene`, { id: 'cinema', action: 'off' });
    assert.strictEqual(r.status, 200);
    assert.deepStrictEqual(ha.calls.slice(-2).map(c => c.url.split('/api/services/')[1]),
      ['media_player/turn_off', 'switch/turn_off']);
    assert.strictEqual((await post(`${base}/home/${TOKEN}/scene`, { id: 'nope', action: 'on' })).status, 404);
  });
});

test('missing or rejected HA token gives a clear error', async () => {
  const ha = fakeHa();
  await withServer({ fetchImpl: ha.fetchImpl, token: false }, async base => {
    const r = await fetch(`${base}/home/${TOKEN}/devices`);
    assert.strictEqual(r.status, 503);
    assert.strictEqual((await r.json()).error, 'ha_token_missing');
  });
  await withServer({ fetchImpl: ha.fetchImpl }, async (base, dir) => {
    fs.writeFileSync(path.join(dir, 'ha_token.txt'), 'revoked');
    const r = await fetch(`${base}/home/${TOKEN}/devices`);
    assert.strictEqual((await r.json()).error, 'ha_token_rejected');
  });
});

test('enable_home installs once into the deployed relay', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'home-enable-'));
  try {
    for (const name of ['relay.js', 'home_routes.js', 'audio_routes.js', 'audio_uploads.js', 'transcript_routes.js'])
      fs.copyFileSync(path.join(__dirname, name), path.join(dir, name));
    const run = () => execFileSync(process.execPath, [path.join(__dirname, 'enable_home.js'), path.join(dir, 'relay.js')]).toString();
    assert.match(run(), /installed/);
    assert.match(run(), /already present/);
    const relay = fs.readFileSync(path.join(dir, 'relay.js'), 'utf8');
    assert.strictEqual(relay.split('handleTab5Home(req, res)').length, 2);
    execFileSync(process.execPath, ['--check', path.join(dir, 'relay.js')]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('noise filter hides sub-controls; ?all=1 returns everything', async () => {
  const ha = fakeHa();
  const original = ha.fetchImpl;
  ha.fetchImpl = async (url, options) => url.endsWith('/api/template')
    ? new Response(JSON.stringify([
      { id: 'switch.pc', name: 'LKRQ 卧室电脑开机 开关', state: 'off', area: '' },
      { id: 'switch.ac_eco', name: 'Mi Gentle Air Condition ECO节能模式', state: 'off', area: '' },
      { id: 'light.ac_led', name: 'Mi Gentle Air Condition 指示灯', state: 'on', area: '' },
      { id: 'button.tv_info', name: '客厅的小米电视 信息', state: 'unknown', area: '' },
      { id: 'climate.ac', name: 'Mi Gentle Air Condition 空调', state: 'off', area: '' },
    ]), { status: 200 }) : original(url, options);
  await withServer({ fetchImpl: ha.fetchImpl }, async base => {
    const shown = (await (await fetch(`${base}/home/${TOKEN}/devices`)).json()).devices.map(d => d.id);
    assert.deepStrictEqual(shown.sort(), ['climate.ac', 'switch.pc']);
    const all = (await (await fetch(`${base}/home/${TOKEN}/devices?all=1`)).json()).devices;
    assert.strictEqual(all.length, 5);
  });
});

test('scene definitions can be written (also via tunnel) and are validated', async () => {
  const ha = fakeHa();
  await withServer({ fetchImpl: ha.fetchImpl }, async (base, dir) => {
    const put = (body, headers = {}) => fetch(`${base}/home/${TOKEN}/scenes`, {
      method: 'PUT', headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(body) });
    const scenes = [{ id: 'cinema', name: '家庭影院', room: '客厅',
      on: [{ entity_id: 'scene.cinema_on', action: 'on' }], off: [{ entity_id: 'scene.cinema_off', action: 'on' }] }];
    let r = await put({ scenes }, { 'cf-connecting-ip': '203.0.113.9' });
    assert.strictEqual(r.status, 200);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(path.join(dir, 'home_scenes.json'), 'utf8')).scenes, scenes);
    assert.strictEqual(ha.calls.length, 0);  // writing a definition switches nothing
    r = await put({ scenes: [{ id: 'Bad Id', name: 'x', on: [{ entity_id: 'switch.a' }] }] });
    assert.strictEqual(r.status, 400);
    r = await put({ scenes: [{ id: 'x', name: 'x', on: [{ entity_id: 'lock.door' }] }] });
    assert.strictEqual(r.status, 400);
    assert.strictEqual((await fetch(`${base}/home/wrong/scenes`, { method: 'PUT', body: '{}' })).status, 404);
    // Off of an HA scene entity runs its turn_on (scenes have no off).
    r = await post(`${base}/home/${TOKEN}/scene`, { id: 'cinema', action: 'off' });
    assert.strictEqual(r.status, 200);
    assert.strictEqual(ha.calls.at(-1).url, 'http://ha.test:8123/api/services/scene/turn_on');
    assert.deepStrictEqual(ha.calls.at(-1).body, { entity_id: 'scene.cinema_off' });
  });
});

test('compact tsv format for the Tab5', async () => {
  const ha = fakeHa();
  const scenes = { scenes: [{ id: 'cinema', name: '家庭影院', room: '客厅',
    on: [{ entity_id: 'scene.a', action: 'on' }], off: [] }] };
  await withServer({ fetchImpl: ha.fetchImpl, scenes }, async base => {
    const text = await (await fetch(`${base}/home/${TOKEN}/devices?format=tsv`)).text();
    const lines = text.trim().split('\n');
    assert.strictEqual(lines[0], 'V\t1');
    assert.ok(lines.includes('D\tlight.monitor_bar\t显示器挂灯\tlight\ton\t卧室\t128\t-\t-'));
    assert.strictEqual(lines.at(-1), 'S\tcinema\t家庭影院\t客厅\t1\t0');
    const pair = await (await post(`${base}/home/${TOKEN}/pair?format=tsv`, {})).text();
    assert.match(pair, /^K\t[A-Za-z0-9_-]{32}\n$/u);
  });
});
