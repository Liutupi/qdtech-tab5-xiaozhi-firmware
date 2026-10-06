// Add the audio module to an existing relay without replacing its other features.
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execFileSync } = require('child_process');
const file = process.argv[2] || '/data/relay.js';
const original = fs.readFileSync(file, 'utf8');
const marker = '// Muse MP3 upload extension v1';
const moduleDir = path.dirname(file);
for (const name of ['audio_routes.js', 'audio_uploads.js', 'transcript_routes.js']) {
  execFileSync(process.execPath, ['--check', path.join(moduleDir, name)]);
}
fs.accessSync(path.join(moduleDir, 'upload_audio.py'), fs.constants.R_OK);
if (typeof require(path.join(moduleDir, 'audio_routes.js')).createAudioRoutes !== 'function') {
  throw new Error('Audio modules cannot be loaded; leave relay unchanged.');
}
if (original.includes(marker)) {
  console.log('Muse audio upload extension is already present; no relay rewrite.');
  process.exit(0);
}
const anchor = 'const server = http.createServer(async (req, res) => {';
if (original.split(anchor).length !== 2 ||
    !original.includes('function savePodcasts(') ||
    !original.includes('function podcastResponse(') ||
    !original.includes('let podcasts =')) {
  throw new Error('Unexpected deployed relay structure; leave it unchanged and inspect first.');
}
const extension = `${marker}
const handleMuseAudio = require('./audio_routes').createAudioRoutes({
  dataDir: DATA_DIR, token: TOKEN, tunnelUrl, getPodcasts: () => podcasts, savePodcasts,
});
const originalMusePodcastResponse = podcastResponse;
podcastResponse = function(url) {
  const result = originalMusePodcastResponse(url);
  result.episodes = result.episodes.map(episode => ({
    ...episode, audio_url: handleMuseAudio.refreshAudioUrl(episode.audio_url),
  }));
  return result;
};

${anchor}
  if (await handleMuseAudio(req, res)) return;`;
const updated = original.replace(anchor, extension);
const temporary = file + `.audio-next-${crypto.randomUUID()}.js`;
const backup = file + `.bak-audio-upload-${new Date().toISOString().replace(/[:.]/g, '-')}`;
try {
  fs.writeFileSync(temporary, updated, { mode: fs.statSync(file).mode });
  execFileSync(process.execPath, ['--check', temporary]);
  fs.copyFileSync(file, backup, fs.constants.COPYFILE_EXCL);
  fs.renameSync(temporary, file);
  console.log('Muse upload extension installed.');
  console.log('Backup:', backup);
  console.log('Before SHA256:', crypto.createHash('sha256').update(original).digest('hex'));
  console.log('After SHA256:', crypto.createHash('sha256').update(updated).digest('hex'));
} finally {
  if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
}
