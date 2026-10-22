// Add the NABO daily push routes to an existing relay without replacing its other features.
// The hook goes after every extension hook already at the top of the request handler, so
// an existing Nabo extension keeps first pick of its own /nabo/... requests.
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execFileSync } = require('child_process');
const file = process.argv[2] || '/data/relay.js';
const original = fs.readFileSync(file, 'utf8');
const marker = '// NABO daily push extension v1';
const moduleDir = path.dirname(file);
execFileSync(process.execPath, ['--check', path.join(moduleDir, 'nabo_routes.js')]);
if (typeof require(path.join(moduleDir, 'nabo_routes.js')).createNaboRoutes !== 'function') {
  throw new Error('NABO module cannot be loaded; leave relay unchanged.');
}
if (original.includes(marker)) {
  console.log('NABO daily push extension is already present; no relay rewrite.');
  process.exit(0);
}
const anchor = 'const server = http.createServer(async (req, res) => {';
const routing = "\n  const url = new URL(req.url, 'http://localhost');";
const start = original.indexOf(anchor);
const hookAt = start < 0 ? -1 : original.indexOf(routing, start);
if (original.split(anchor).length !== 2 || hookAt < 0 || !original.includes('const TOKEN = loadToken();')) {
  throw new Error('Unexpected deployed relay structure; leave it unchanged and inspect first.');
}
// Everything between the anchor and the relay's own routing must be extension hooks.
const between = original.slice(start + anchor.length, hookAt).split('\n').map(line => line.trim()).filter(Boolean);
if (!between.every(line => /^if \(await handle\w+\(req, res\)\) return;$/u.test(line))) {
  throw new Error('Request handler starts with unexpected code; leave it unchanged and inspect first.');
}
const updated = original.slice(0, start) +
  `${marker}\nconst handleNaboPush = require('./nabo_routes').createNaboRoutes({ dataDir: DATA_DIR, token: TOKEN });\n\n` +
  original.slice(start, hookAt) + '\n  if (await handleNaboPush(req, res)) return;' + original.slice(hookAt);
const temporary = file + `.nabo-next-${crypto.randomUUID()}.js`;
const backup = file + `.bak-nabo-${new Date().toISOString().replace(/[:.]/g, '-')}`;
try {
  fs.writeFileSync(temporary, updated, { mode: fs.statSync(file).mode });
  execFileSync(process.execPath, ['--check', temporary]);
  fs.copyFileSync(file, backup, fs.constants.COPYFILE_EXCL);
  fs.renameSync(temporary, file);
  console.log('NABO daily push extension installed after', between.length, 'existing hook(s). Backup:', backup);
} finally {
  if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
}
