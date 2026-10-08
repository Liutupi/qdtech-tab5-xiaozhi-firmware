// Tab5 米家中控: Home Assistant (Xiaomi Miot Auto) devices and combined scenes over the relay.
//
//   Tab5  --LAN HTTP-->  GET  /home/<token>/devices[?all=1] devices by room + scenes
//                        POST /home/<token>/control         {"entity_id","action","value"?}
//                        POST /home/<token>/scene           {"id","action":"on"|"off"}
//   setup ------------>  PUT  /home/<token>/scenes          {"scenes":[...]} (definitions only)
//   Tab5 (once) ------>  POST /home/<token>/pair            -> {"key"} first device after a restart
//
// The Home Assistant long-lived token stays on the NAS in /data/ha_token.txt (read on each
// request, never logged or returned). Control from the LAN needs only the relay token. Through
// the public tunnel it also needs the home key (X-Home-Key) that only the paired Tab5 holds:
// the relay token alone (which Muse also knows) can read the device list but switch nothing.
// Pairing hands out the key once, within PAIR_WINDOW_MS of a relay start; delete
// /data/home_key.txt and restart the relay to pair again. Combined scenes (e.g. 卧室电脑 = PC + speaker +
// monitor light) live in /data/home_scenes.json and can be edited without a restart.
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

// Domains shown on the Tab5 and the services each action maps to.
const ACTIONS = {
  light: { on: 'turn_on', off: 'turn_off', toggle: 'toggle', brightness: 'turn_on' },
  switch: { on: 'turn_on', off: 'turn_off', toggle: 'toggle' },
  fan: { on: 'turn_on', off: 'turn_off', toggle: 'toggle', percentage: 'set_percentage' },
  climate: { on: 'turn_on', off: 'turn_off', temperature: 'set_temperature', hvac_mode: 'set_hvac_mode' },
  media_player: { on: 'turn_on', off: 'turn_off', toggle: 'toggle', volume: 'volume_set' },
  cover: { on: 'open_cover', off: 'close_cover', stop: 'stop_cover', position: 'set_cover_position' },
  humidifier: { on: 'turn_on', off: 'turn_off', toggle: 'toggle' },
  vacuum: { on: 'start', off: 'return_to_base', stop: 'stop' },
  script: { on: 'turn_on', off: 'turn_off' },
  scene: { on: 'turn_on' },
  button: { on: 'press' },
};
const DOMAINS = Object.keys(ACTIONS);
const MAX_BODY = 4096;
const MAX_STEPS = 20;
const CACHE_MS = 3000;
const HA_TIMEOUT_MS = 8000;
const MAX_SCENES = 24;
const PAIR_WINDOW_MS = 30 * 60 * 1000;
// Xiaomi Miot exposes every MIoT property; these sub-controls only clutter a wall panel
// (?all=1 still returns them). Main switches, lights, climate and players stay.
const NOISE = /信息|指示灯|提示音|物理控制锁|Alarm|充电保护|过度用电|最大功率|倒计时|状态，|睡眠模式|标准睡眠|ECO|摆风|柔风|干燥功能|辅热|安全远程|高水位|洗烘联动|一键智能洗|蒸汽除菌|智能推荐|凌动开关|背灯|Ambient Light|氛围灯$|空调 开关|洗衣机 开关/u;
// Short wall-panel labels: Miot names are "<English model> <MIoT service>"; the room is shown
// separately, so keep the meaningful Chinese part.
const KNOWN = [
  [/AI Speaker Pro/iu, '小爱音箱 Pro'], [/AI Speaker Play/iu, '小爱音箱 Play'], [/AI Speaker/iu, '小爱音箱'],
  [/Scent Diffuser/iu, '香薰机'], [/Smart Plug/iu, '智能插座'], [/Screen Light Bar/iu, '显示器挂灯'],
  [/Smart bulb/iu, '灯泡'], [/IR TV Control/iu, '电视 (红外)'], [/^网关 Light$/u, '网关夜灯'],
];
function shortLabel(device) {
  for (const [pattern, label] of KNOWN) if (pattern.test(device.name)) return label;
  if (device.domain === 'climate') return '空调';
  let label = device.name.replace(/^[\x20-\x7e]+(?=[^\x20-\x7e])/u, '')   // leading English model
    .replace(/\s*播放控制$/u, '').replace(/\s*开关(?=-|$)/u, '').replace(/-/gu, ' ').trim();
  return label || device.name;
}
function isNoise(device) {
  return device.domain === 'button' || NOISE.test(device.name);
}

// One template call returns every controllable entity with its room; Jinja runs inside HA.
const DEVICES_TEMPLATE = `{% set ns = namespace(o=[]) %}{% for s in states
 if s.domain in ${JSON.stringify(DOMAINS)} and not (s.attributes.hidden | default(false)) %}
{% set ns.o = ns.o + [{'id': s.entity_id, 'name': s.name, 'state': s.state,
 'area': area_name(s.entity_id) or '', 'brightness': s.attributes.brightness,
 'temperature': s.attributes.temperature, 'current_temperature': s.attributes.current_temperature,
 'hvac_modes': s.attributes.hvac_modes, 'percentage': s.attributes.percentage,
 'volume': s.attributes.volume_level}] %}{% endfor %}{{ ns.o | tojson }}`;

function createHomeRoutes({ dataDir, token, haUrl, fetchImpl = globalThis.fetch, now = Date.now }) {
  const TOKEN = token;
  const HA_URL = String(haUrl || process.env.HA_URL || readText(path.join(dataDir, 'ha_url.txt')) ||
    'http://172.18.0.1:8123').replace(/\/+$/, '');
  const TOKEN_FILE = path.join(dataDir, 'ha_token.txt');
  const SCENES_FILE = path.join(dataDir, 'home_scenes.json');
  const KEY_FILE = path.join(dataDir, 'home_key.txt');
  const startedAt = now();
  let cache = null;

  function keyOk(req) {
    const expected = readText(KEY_FILE);
    if (!expected) return false;
    const a = Buffer.from(String(req.headers['x-home-key'] || '')), b = Buffer.from(expected);
    return a.length === b.length && crypto.timingSafeEqual(a, b);
  }

  function tokenOk(value) {
    const a = Buffer.from(String(value || '')), b = Buffer.from(TOKEN);
    return a.length === b.length && crypto.timingSafeEqual(a, b);
  }
  function viaTunnel(req) {
    return Boolean(req.headers['cf-connecting-ip'] || req.headers['cf-ray']);
  }
  function send(res, status, body) {
    if (res.destroyed || res.writableEnded) return true;
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(body));
    return true;
  }
  function sendText(res, status, text) {
    if (res.destroyed || res.writableEnded) return true;
    res.writeHead(status, { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(text);
    return true;
  }
  function readBody(req) {
    return new Promise((resolve, reject) => {
      let size = 0;
      const chunks = [];
      req.on('data', chunk => {
        size += chunk.length;
        if (size > MAX_BODY) {
          reject(Object.assign(new Error('body too large'), { status: 413 }));
          req.destroy();
        } else chunks.push(chunk);
      });
      req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
      req.on('error', reject);
    });
  }

  class HomeError extends Error {
    constructor(status, code) { super(code); this.status = status; this.code = code; }
  }

  async function ha(method, apiPath, body) {
    const haToken = readText(TOKEN_FILE);
    if (!haToken) throw new HomeError(503, 'ha_token_missing');
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), HA_TIMEOUT_MS);
    let response;
    try {
      response = await fetchImpl(HA_URL + apiPath, {
        method,
        headers: { Authorization: `Bearer ${haToken}`, 'Content-Type': 'application/json' },
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: controller.signal,
      });
    } catch {
      throw new HomeError(502, 'ha_unreachable');
    } finally {
      clearTimeout(timer);
    }
    if (response.status === 401 || response.status === 403) throw new HomeError(502, 'ha_token_rejected');
    if (!response.ok) throw new HomeError(502, `ha_http_${response.status}`);
    return response;
  }

  function loadScenes() {
    try {
      const data = JSON.parse(fs.readFileSync(SCENES_FILE, 'utf8'));
      const list = Array.isArray(data.scenes) ? data.scenes : [];
      return list.filter(s => s && typeof s.id === 'string' && typeof s.name === 'string')
        .map(s => ({
          id: s.id, name: s.name, room: typeof s.room === 'string' ? s.room : '',
          on: validSteps(s.on), off: validSteps(s.off),
        }));
    } catch {
      return [];
    }
  }
  function cleanScenes(list) {
    if (!Array.isArray(list) || list.length > MAX_SCENES) return null;
    const seen = new Set();
    const out = [];
    for (const s of list) {
      if (!s || typeof s.id !== 'string' || !/^[a-z0-9_]{1,32}$/u.test(s.id) || seen.has(s.id)) return null;
      if (typeof s.name !== 'string' || !s.name.trim() || s.name.length > 20) return null;
      const on = validSteps(s.on), off = validSteps(s.off);
      if ((Array.isArray(s.on) && on.length !== s.on.length) || (Array.isArray(s.off) && off.length !== s.off.length) ||
          on.length + off.length === 0) return null;
      seen.add(s.id);
      out.push({ id: s.id, name: s.name.trim(), room: typeof s.room === 'string' ? s.room.slice(0, 12) : '',
        on: on.map(stepJson), off: off.map(stepJson) });
    }
    return out;
  }
  function stepJson(step) {
    const out = { entity_id: step.entity_id };
    if (step.action) out.action = step.action;
    if (step.value !== undefined) out.value = step.value;
    return out;
  }
  function validSteps(steps) {
    if (!Array.isArray(steps)) return [];
    return steps.slice(0, MAX_STEPS).filter(step => step && typeof step.entity_id === 'string' &&
      /^[a-z_]+\.[a-z0-9_]+$/u.test(step.entity_id) && ACTIONS[step.entity_id.split('.')[0]]?.[step.action || 'on']);
  }

  async function devices() {
    if (cache && now() - cache.at < CACHE_MS) return cache.devices;
    const response = await ha('POST', '/api/template', { template: DEVICES_TEMPLATE });
    let list;
    try {
      list = JSON.parse(await response.text());
    } catch {
      throw new HomeError(502, 'ha_bad_response');
    }
    const devicesList = (Array.isArray(list) ? list : []).map(d => {
      const out = { id: d.id, name: d.name, domain: String(d.id).split('.')[0], state: d.state, area: d.area || '' };
      out.label = shortLabel(out);
      for (const key of ['brightness', 'temperature', 'current_temperature', 'hvac_modes', 'percentage', 'volume'])
        if (d[key] !== null && d[key] !== undefined) out[key] = d[key];
      return out;
    }).sort((a, b) => a.area.localeCompare(b.area, 'zh') || a.name.localeCompare(b.name, 'zh'));
    cache = { at: now(), devices: devicesList };
    return devicesList;
  }

  function serviceData(domain, action, value) {
    const data = {};
    const number = Number(value);
    if (action === 'brightness') data.brightness_pct = clamp(number, 1, 100);
    else if (action === 'percentage') data.percentage = clamp(number, 0, 100);
    else if (action === 'temperature') data.temperature = clamp(number, 16, 32);
    else if (action === 'volume') data.volume_level = clamp(number, 0, 100) / 100;
    else if (action === 'position') data.position = clamp(number, 0, 100);
    else if (action === 'hvac_mode') {
      if (typeof value !== 'string' || !/^[a-z_]{2,20}$/u.test(value)) return null;
      data.hvac_mode = value;
    }
    return Object.values(data).some(v => Number.isNaN(v)) ? null : data;
  }

  async function control(entityId, action, value) {
    if (typeof entityId !== 'string' || !/^[a-z_]+\.[a-z0-9_]+$/u.test(entityId))
      throw new HomeError(400, 'bad_entity');
    const domain = entityId.split('.')[0];
    const service = ACTIONS[domain]?.[action];
    if (!service) throw new HomeError(400, 'unsupported_action');
    const data = serviceData(domain, action, value);
    if (!data) throw new HomeError(400, 'bad_value');
    await ha('POST', `/api/services/${domain}/${service}`, { entity_id: entityId, ...data });
    cache = null;
  }

  async function handle(req, res) {
    const url = new URL(req.url, 'http://localhost');
    const parts = url.pathname.split('/').filter(Boolean);
    if (parts[0] !== 'home') return false;
    if (!tokenOk(parts[1])) return send(res, 404, 'not found');
    try {
      if (parts[2] === 'devices' && parts.length === 3 && req.method === 'GET') {
        const list = await devices();
        const shown = url.searchParams.get('all') === '1' ? list : list.filter(d => !isNoise(d));
        if (url.searchParams.get('format') === 'tsv') return sendText(res, 200, toTsv(shown, loadScenes()));
        return send(res, 200, { ok: true, devices: shown, scenes: loadScenes() });
      }
      // Scene definitions may be written from anywhere with the relay token; running a
      // scene (or any control) still only works from the LAN.
      if (parts[2] === 'scenes' && parts.length === 3 && req.method === 'PUT') {
        let body;
        try {
          body = JSON.parse(await readBody(req));
        } catch (error) {
          return send(res, error.status || 400, { ok: false, error: error.status ? 'too_large' : 'bad_json' });
        }
        const scenes = cleanScenes(body && body.scenes);
        if (!scenes) return send(res, 400, { ok: false, error: 'bad_scenes' });
        const temporary = `${SCENES_FILE}.${process.pid}.tmp`;
        fs.writeFileSync(temporary, JSON.stringify({ scenes }, null, 2));
        fs.renameSync(temporary, SCENES_FILE);
        return send(res, 200, { ok: true, scenes: loadScenes() });
      }
      if (parts[2] === 'pair' && parts.length === 3 && req.method === 'POST') {
        if (readText(KEY_FILE)) return send(res, 409, { ok: false, error: 'already_paired' });
        if (now() - startedAt > PAIR_WINDOW_MS) return send(res, 403, { ok: false, error: 'pairing_closed' });
        const key = crypto.randomBytes(24).toString('base64url');
        fs.writeFileSync(KEY_FILE, key + '\n', { mode: 0o600, flag: 'wx' });
        if (url.searchParams.get('format') === 'tsv') return sendText(res, 200, `K\t${key}\n`);
        return send(res, 200, { ok: true, key });
      }
      if (req.method !== 'POST' || parts.length !== 3 || !['control', 'scene'].includes(parts[2]))
        return send(res, 404, { ok: false, error: 'not_found' });
      // Switching things on and off stays inside the home network.
      if (viaTunnel(req) && !keyOk(req)) return send(res, 403, { ok: false, error: 'home_key_required' });
      let body;
      try {
        body = JSON.parse(await readBody(req));
      } catch (error) {
        return send(res, error.status || 400, { ok: false, error: error.status ? 'too_large' : 'bad_json' });
      }
      if (parts[2] === 'control') {
        await control(body.entity_id, body.action, body.value);
        return send(res, 200, { ok: true });
      }
      const scene = loadScenes().find(s => s.id === body.id);
      if (!scene) return send(res, 404, { ok: false, error: 'unknown_scene' });
      if (body.action !== 'on' && body.action !== 'off') return send(res, 400, { ok: false, error: 'bad_action' });
      const failed = [];
      for (const step of scene[body.action]) {
        try {
          await control(step.entity_id, step.action || body.action, step.value);
        } catch (error) {
          failed.push({ entity_id: step.entity_id, error: error.code || 'failed' });
        }
      }
      return send(res, failed.length ? 207 : 200, { ok: failed.length === 0, failed });
    } catch (error) {
      if (error instanceof HomeError) return send(res, error.status, { ok: false, error: error.code });
      console.error('home route error:', error && error.message);
      return send(res, 500, { ok: false, error: 'internal' });
    }
  }
  return handle;
}

// Compact line format for the Tab5 (parsed without cJSON, whose nodes would land in scarce
// internal RAM). Fields are tab-separated; tabs and newlines inside values become spaces.
//   D <id> <label> <domain> <state> <area> <brightness 0-255|-> <target °C|-> <current °C|->
//   S <id> <name> <room> <has on 0|1> <has off 0|1>
function toTsv(devices, scenes) {
  const clean = value => String(value ?? '').replace(/[\t\r\n]+/gu, ' ').slice(0, 80);
  const num = value => Number.isFinite(value) ? String(value) : '-';
  const lines = ['V\t1'];
  for (const d of devices)
    lines.push(['D', d.id, d.label || d.name, d.domain, d.state, d.area, num(d.brightness), num(d.temperature),
      num(d.current_temperature)].map(clean).join('\t'));
  for (const s of scenes)
    lines.push(['S', s.id, s.name, s.room, s.on.length ? 1 : 0, s.off.length ? 1 : 0].map(clean).join('\t'));
  return lines.join('\n') + '\n';
}

function readText(file) {
  try {
    return fs.readFileSync(file, 'utf8').trim();
  } catch {
    return '';
  }
}
function clamp(value, lo, hi) {
  return Number.isFinite(value) ? Math.min(hi, Math.max(lo, value)) : NaN;
}

module.exports = { createHomeRoutes, DOMAINS, isNoise, shortLabel };
