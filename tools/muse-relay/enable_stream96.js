#!/usr/bin/env node
'use strict';
// Run on the NAS against its current relay.js; never replace it with a repo copy.
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const file = path.resolve(process.argv[2] || '/data/relay.js');
const source = fs.readFileSync(file, 'utf8');
if (source.includes('// Muse stream96 extension v1')) {
  console.log('stream96 is already mounted; check its module and run the acceptance tests.');
  process.exit(0);
}
const marker = /^const server = http\.createServer\(async \(req, res\) => \{/m;
if (!marker.test(source) || !/const TOKEN =/.test(source) || !/const DATA_DIR =/.test(source) ||
    !/function tunnelUrl\(/.test(source))
  throw new Error('Unrecognized relay structure; mount createStream96Routes manually, before podcast catch-all.');
const moduleFile = path.join(path.dirname(file), 'stream96_routes.js');
if (!fs.existsSync(moduleFile)) throw new Error('Copy stream96_routes.js next to relay.js first.');
const extension = `// Muse stream96 extension v1
const handleMuseStream96 = require('./stream96_routes').createStream96Routes({
  dataDir: DATA_DIR, token: TOKEN,
  selfOrigins: () => [tunnelUrl(), process.env.AUDIO96_PUBLIC_ORIGIN].filter(Boolean),
});
`;
const updated = source.replace(marker, match => extension + '\n' + match + '\n  if (await handleMuseStream96(req, res)) return;');
const temporary = file + '.stream96.tmp.js';
const backup = file + '.before-stream96-' + new Date().toISOString().replace(/[:.]/g, '-') + '.bak';
fs.writeFileSync(temporary, updated, { flag: 'wx', mode: fs.statSync(file).mode & 0o777 });
try {
  execFileSync(process.execPath, ['--check', temporary], { stdio: 'pipe' });
  fs.copyFileSync(file, backup, fs.constants.COPYFILE_EXCL);
  fs.renameSync(temporary, file);
  console.log('Mounted stream96 before existing handlers. Backup: ' + backup);
} finally { fs.rmSync(temporary, { force: true }); }
