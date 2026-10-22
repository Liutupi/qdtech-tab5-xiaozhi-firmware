'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { execFileSync } = require('child_process');
const { createNaboRoutes } = require('./nabo_routes');

const TOKEN = 'relay-token-0123456789';
const NOW = Date.parse('2026-10-22T08:00:00+08:00');

async function withServer(options, fn) {
  const dir = options.dir || fs.mkdtempSync(path.join(os.tmpdir(), 'nabo-'));
  const clock = { now: options.now || NOW };
  const logs = [];
  const handle = createNaboRoutes({ dataDir: dir, token: TOKEN, now: () => clock.now, log: (...a) => logs.push(a.join(' ')) });
  const server = http.createServer(async (req, res) => {
    if (!(await handle(req, res))) { res.writeHead(418); res.end(); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}/nabo/${TOKEN}`;
  try {
    await fn({ base, dir, clock, logs, root: `http://127.0.0.1:${server.address().port}` });
  } finally {
    server.close();
    if (!options.dir) fs.rmSync(dir, { recursive: true, force: true });
  }
}

const push = (base, body) => fetch(`${base}/push`, {
  method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
});
const inbox = async (base, since = '') => (await fetch(`${base}/inbox${since ? `?since=${encodeURIComponent(since)}` : ''}`)).json();

const fortuneBody = JSON.stringify({
  score: 4, summary: '稳中有进', yi: ['整理文献'], ji: ['熬夜'], color: '雾蓝', number: 7,
  bars: { 事业科研: 4, 健康: 3, 家庭: 4, 财运: 3, 心情: 4 }, nabo: '记得带伞',
  modules: { tarot: [{ pos: '过去', name: '星星', upright: true, meaning: '保持耐心' }] },
});

test('push then pull: items keep their fields and get a revision', async () => {
  await withServer({}, async ({ base }) => {
    let r = await push(base, { id: 'nabo-todo-1', ts: '2026-10-22T07:45:00+08:00', source: 'nabo/todo',
      priority: 'high', title: '9:30 提交病例讨论 PPT' });
    assert.strictEqual(r.status, 200);
    assert.deepStrictEqual(await r.json(), { ok: true, id: 'nabo-todo-1', rev: 1, deleted: false, changed: true });
    r = await push(base, { id: 'nabo-fortune', ts: '2026-10-22T07:00:00+08:00', source: 'nabo/fortune',
      title: '今日运势', body: fortuneBody });
    assert.strictEqual(r.status, 200);
    const all = await inbox(base);
    assert.strictEqual(all.latest_rev, 2);
    assert.strictEqual(all.more, false);
    assert.deepStrictEqual(all.items.map(i => i.id), ['nabo-todo-1', 'nabo-fortune']);
    const todo = all.items[0];
    assert.strictEqual(todo.time, '09:30');
    assert.strictEqual(todo.mood, 'alert');
    assert.ok(!('updated_at' in todo));
    assert.strictEqual(all.items[1].mood, 'happy');
    assert.strictEqual(all.items[1].priority, 'normal');
    assert.deepStrictEqual(JSON.parse(all.items[1].body).bars.事业科研, 4);
    assert.deepStrictEqual((await inbox(base, '1')).items.map(i => i.id), ['nabo-fortune']);
    assert.deepStrictEqual((await inbox(base, '2')).items, []);
  });
});

test('same id replaces, deleted leaves a tombstone, unknown delete is a no-op', async () => {
  await withServer({}, async ({ base }) => {
    await push(base, { id: 'w1', source: 'nabo/study/word', title: '今日 2 个词', body: 'sepsis  脓毒症' });
    await push(base, { id: 'w1', source: 'nabo/study/word', title: '今日 3 个词', body: 'sepsis  脓毒症\nlactate  乳酸' });
    let all = await inbox(base);
    assert.strictEqual(all.items.length, 1);
    assert.strictEqual(all.items[0].title, '今日 3 个词');
    assert.strictEqual(all.items[0].rev, 2);
    const r = await (await push(base, { id: 'w1', deleted: true })).json();
    assert.strictEqual(r.rev, 3);
    all = await inbox(base, '2');
    assert.deepStrictEqual(all.items, [{ id: 'w1', deleted: true, rev: 3 }]);
    const none = await (await push(base, { id: 'never-pushed', deleted: true })).json();
    assert.strictEqual(none.changed, false);
    assert.strictEqual((await inbox(base)).latest_rev, 3);
  });
});

test('validation rejects bad fields without storing anything', async () => {
  await withServer({}, async ({ base }) => {
    const cases = [
      [{ source: 'nabo/todo', title: 'x' }, 'bad_id'],
      [{ id: 'a', source: 'nabo/review', title: 'x' }, 'bad_source'],
      [{ id: 'a', source: 'nabo/todo', title: 'x', priority: 'urgent' }, 'bad_priority'],
      [{ id: 'a', source: 'nabo/todo', title: 'x', mood: 'sad' }, 'bad_mood'],
      [{ id: 'a', source: 'nabo/todo', title: 'x', ts: '2026-10-22 07:00' }, 'bad_ts'],
      [{ id: 'a', source: 'nabo/todo' }, 'title_required'],
      [{ id: 'a', source: 'nabo/todo', title: '字'.repeat(41) }, 'title_too_long'],
      [{ id: 'a', source: 'nabo/study/point', title: 'x', body: '字'.repeat(601) }, 'body_too_long'],
      [{ id: 'a', source: 'nabo/fortune', title: 'x', body: '{not json' }, 'fortune_body_not_json'],
      [{ id: 'a', source: 'nabo/fortune', title: 'x', body: '[1,2]' }, 'fortune_body_not_object'],
      [{ id: 'a', source: 'nabo/fortune', title: 'x' }, 'body_required'],
      [{ id: 'bad id!', source: 'nabo/todo', title: 'x' }, 'bad_id'],
    ];
    for (const [body, error] of cases) {
      const r = await push(base, body);
      assert.strictEqual(r.status, 400, JSON.stringify(body));
      assert.strictEqual((await r.json()).error, error);
    }
    const raw = await fetch(`${base}/push`, { method: 'POST', body: '{oops' });
    assert.strictEqual((await raw.json()).error, 'bad_json');
    const big = await fetch(`${base}/push`, { method: 'POST', body: 'x'.repeat(17 * 1024) });
    assert.strictEqual(big.status, 413);
    assert.strictEqual((await big.json()).error, 'too_large');
    assert.strictEqual((await inbox(base)).latest_rev, 0);
  });
});

// 418 is the test server's "not handled here": the relay's older Nabo extension and its own
// routes must still see every request this module does not own.
test('anything but the two routes falls through to the rest of the relay', async () => {
  await withServer({}, async ({ base, root }) => {
    for (const [url, method] of [
      [`${root}/nabo/wrong/inbox`, 'GET'], [`${root}/nabo/wrong/push`, 'POST'],
      [`${base}/push`, 'GET'], [`${base}/inbox`, 'POST'],
      [`${base}/publish`, 'POST'], [`${base}/inbox/extra`, 'GET'], [`${root}/nabo/radio`, 'POST'],
      [`${root}/nabo`, 'GET'], [`${root}/inbox/${TOKEN}`, 'GET'],
    ]) {
      const response = await fetch(url, { method, body: method === 'POST' ? '{}' : undefined });
      assert.strictEqual(response.status, 418, `${method} ${url}`);
    }
    assert.strictEqual((await fetch(`${base}/inbox?since=yesterday`)).status, 400);
  });
});

test('since may be an ISO time; state survives a restart', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nabo-keep-'));
  try {
    await withServer({ dir }, async ({ base, clock }) => {
      await push(base, { id: 'p1', source: 'nabo/study/paper', title: '文献一' });
      clock.now += 60 * 1000;
      await push(base, { id: 'p2', source: 'nabo/study/paper', title: '文献二' });
      const later = await inbox(base, new Date(NOW + 30 * 1000).toISOString());
      assert.deepStrictEqual(later.items.map(i => i.id), ['p2']);
    });
    await withServer({ dir, now: NOW + 120 * 1000 }, async ({ base }) => {
      const all = await inbox(base);
      assert.deepStrictEqual(all.items.map(i => i.id), ['p1', 'p2']);
      await push(base, { id: 'p3', source: 'nabo/study/point', title: '知识点' });
      assert.strictEqual((await inbox(base)).latest_rev, 3);
    });
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('items older than 30 days are pruned and the list is capped', async () => {
  await withServer({}, async ({ base, clock }) => {
    await push(base, { id: 'old', ts: '2026-09-01T08:00:00+08:00', source: 'nabo/todo', title: '旧事' });
    await push(base, { id: 'new', source: 'nabo/todo', title: '新事' });
    assert.deepStrictEqual((await inbox(base)).items.map(i => i.id), ['new']);
    for (let i = 0; i < 510; ++i) {
      clock.now += 1000;
      await push(base, { id: `n${i}`, source: 'nabo/todo', title: `事 ${i}` });
    }
    const page = await inbox(base);
    assert.strictEqual(page.items.length, 100);
    assert.strictEqual(page.more, true);
    assert.strictEqual(page.items[0].id, 'n10');
    const rest = await inbox(base, String(page.rev));
    assert.strictEqual(rest.items[0].rev, page.rev + 1);
  });
});

test('the push log never contains the token or the body text', async () => {
  await withServer({}, async ({ base, logs }) => {
    await push(base, { id: 'secret-check', source: 'nabo/study/point', title: '标题', body: '正文内容' });
    const all = logs.join('\n');
    assert.ok(!all.includes(TOKEN));
    assert.ok(!all.includes('正文内容'));
  });
});

test('enable_nabo installs once into the deployed relay', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nabo-enable-'));
  try {
    for (const name of ['relay.js', 'nabo_routes.js', 'home_routes.js', 'audio_routes.js', 'audio_uploads.js', 'transcript_routes.js'])
      fs.copyFileSync(path.join(__dirname, name), path.join(dir, name));
    const home = () => execFileSync(process.execPath, [path.join(__dirname, 'enable_home.js'), path.join(dir, 'relay.js')]).toString();
    const run = () => execFileSync(process.execPath, [path.join(__dirname, 'enable_nabo.js'), path.join(dir, 'relay.js')]).toString();
    assert.match(home(), /installed/);
    // Stand-in for the older Nabo extension that publishes the daily radio.
    const anchor = 'const server = http.createServer(async (req, res) => {';
    const file = path.join(dir, 'relay.js');
    fs.writeFileSync(file, fs.readFileSync(file, 'utf8').replace(anchor,
      `const handleNaboRadio = async () => false;\n${anchor}\n  if (await handleNaboRadio(req, res)) return;`));
    assert.match(run(), /installed after 3 existing hook/);
    assert.match(run(), /already present/);
    const relay = fs.readFileSync(file, 'utf8');
    assert.strictEqual(relay.split('handleNaboPush(req, res)').length, 2);
    assert.strictEqual(relay.split('handleTab5Home(req, res)').length, 2);
    const at = name => relay.indexOf(`if (await ${name}(req, res)) return;`);
    for (const earlier of ['handleNaboRadio', 'handleTab5Home', 'handleMuseAudio'])
      assert.ok(at(earlier) >= 0 && at(earlier) < at('handleNaboPush'), earlier);
    assert.ok(at('handleNaboPush') < relay.indexOf("const url = new URL(req.url, 'http://localhost');",
      relay.indexOf(anchor)));
    execFileSync(process.execPath, ['--check', file]);
    assert.ok(fs.readdirSync(dir).some(name => name.startsWith('relay.js.bak-nabo-')));
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('enable_nabo refuses a request handler it does not recognise', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nabo-refuse-'));
  try {
    for (const name of ['relay.js', 'nabo_routes.js', 'audio_routes.js', 'audio_uploads.js', 'transcript_routes.js'])
      fs.copyFileSync(path.join(__dirname, name), path.join(dir, name));
    const file = path.join(dir, 'relay.js');
    const anchor = 'const server = http.createServer(async (req, res) => {';
    const before = fs.readFileSync(file, 'utf8').replace(anchor, `${anchor}\n  if (req.url.startsWith('/nabo/')) return;`);
    fs.writeFileSync(file, before);
    assert.throws(() => execFileSync(process.execPath, [path.join(__dirname, 'enable_nabo.js'), file],
      { stdio: 'pipe' }));
    assert.strictEqual(fs.readFileSync(file, 'utf8'), before);
    assert.ok(!fs.readdirSync(dir).some(name => name.startsWith('relay.js.bak-nabo-')));
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
