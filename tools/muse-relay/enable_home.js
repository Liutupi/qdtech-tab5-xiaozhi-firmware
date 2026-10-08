// Add the Tab5 米家中控 routes to an existing relay without replacing its other features.
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execFileSync } = require('child_process');
const file = process.argv[2] || '/data/relay.js';
const original = fs.readFileSync(file, 'utf8');
const marker = '// Tab5 home control extension v1';
const moduleDir = path.dirname(file);
execFileSync(process.execPath, ['--check', path.join(moduleDir, 'home_routes.js')]);
if (typeof require(path.join(moduleDir, 'home_routes.js')).createHomeRoutes !== 'function') {
  throw new Error('Home module cannot be loaded; leave relay unchanged.');
}
if (original.includes(marker)) {
  console.log('Home control extension is already present; no relay rewrite.');
  process.exit(0);
}
const anchor = 'const server = http.createServer(async (req, res) => {';
if (original.split(anchor).length !== 2 || !original.includes('const TOKEN = loadToken();')) {
  throw new Error('Unexpected deployed relay structure; leave it unchanged and inspect first.');
}
const extension = `${marker}
const handleTab5Home = require('./home_routes').createHomeRoutes({ dataDir: DATA_DIR, token: TOKEN });

${anchor}
  if (await handleTab5Home(req, res)) return;`;
const updated = original.replace(anchor, extension);
const temporary = file + `.home-next-${crypto.randomUUID()}.js`;
const backup = file + `.bak-home-${new Date().toISOString().replace(/[:.]/g, '-')}`;
try {
  fs.writeFileSync(temporary, updated, { mode: fs.statSync(file).mode });
  execFileSync(process.execPath, ['--check', temporary]);
  fs.copyFileSync(file, backup, fs.constants.COPYFILE_EXCL);
  fs.renameSync(temporary, file);
  console.log('Home control extension installed. Backup:', backup);
} finally {
  if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
}
