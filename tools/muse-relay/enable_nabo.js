// Add the NABO daily push routes to an existing relay without replacing its other features.
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
if (original.split(anchor).length !== 2 || !original.includes('const TOKEN = loadToken();')) {
  throw new Error('Unexpected deployed relay structure; leave it unchanged and inspect first.');
}
const extension = `${marker}
const handleNaboPush = require('./nabo_routes').createNaboRoutes({ dataDir: DATA_DIR, token: TOKEN });

${anchor}
  if (await handleNaboPush(req, res)) return;`;
const updated = original.replace(anchor, extension);
const temporary = file + `.nabo-next-${crypto.randomUUID()}.js`;
const backup = file + `.bak-nabo-${new Date().toISOString().replace(/[:.]/g, '-')}`;
try {
  fs.writeFileSync(temporary, updated, { mode: fs.statSync(file).mode });
  execFileSync(process.execPath, ['--check', temporary]);
  fs.copyFileSync(file, backup, fs.constants.COPYFILE_EXCL);
  fs.renameSync(temporary, file);
  console.log('NABO daily push extension installed. Backup:', backup);
} finally {
  if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
}
