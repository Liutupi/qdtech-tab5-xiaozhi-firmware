#!/usr/bin/env node
// Play today's NetEase recommendations on Tab5, one authorized MP3 URL at a time.
// On NAS, use its existing NeteaseCloudMusicApi module and login cookie file.
// NETEASE_API_BASE is an optional alternative for a compatible HTTP service.

import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const apiBase = process.env.NETEASE_API_BASE?.replace(/\/+$/u, "");
const cookie = process.env.NETEASE_COOKIE || "";
const moduleRoot = process.env.NETEASE_MODULE_ROOT;
const cookieFile = process.env.NETEASE_COOKIE_FILE ||
  (moduleRoot ? join(moduleRoot, "netease-cookie.txt") : "");
const deviceUrl = process.env.TAB5_WS_URL ||
  `ws://${process.env.TAB5_HOST || "192.168.88.101"}:8080/ws`;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function requireLocalApi(value) {
  const url = new URL(value);
  if (!["http:", "https:"].includes(url.protocol)) throw new Error("NETEASE_API_BASE must be HTTP(S)");
  return url;
}

export function parseRecommendations(json) {
  if (json?.code !== 200) throw new Error("NetEase daily recommendations were rejected; check login");
  const rows = json?.data?.dailySongs || json?.recommend;
  if (!Array.isArray(rows) || !rows.length) throw new Error("No daily recommended songs were returned");
  const seen = new Set();
  return rows.flatMap((row) => {
    const id = String(row?.id || "");
    if (!/^\d+$/u.test(id) || seen.has(id) || !row?.name) return [];
    seen.add(id);
    return [{ id, title: String(row.name), artist: (row.ar || row.artists || [])
      .map((artist) => artist.name).filter(Boolean).join(" / "),
      durationMs: Number(row.dt || row.duration || 0) }];
  });
}

export function parsePlayableUrl(json) {
  if (json?.code !== 200) return null;
  const item = json?.data?.[0];
  if (!item?.url || (item.type && item.type.toLowerCase() !== "mp3") ||
      item.freeTrialInfo || item.freeTrialPrivilege?.listenType) return null;
  try {
    const url = new URL(item.url);
    return ["http:", "https:"].includes(url.protocol) ? url.href : null;
  } catch { return null; }
}

async function apiGet(path) {
  if (moduleRoot) {
    const requireFromNas = createRequire(join(moduleRoot, "package.json"));
    const api = requireFromNas("NeteaseCloudMusicApi");
    const auth = cookie || readFileSync(cookieFile, "utf8").trim();
    if (!auth) throw new Error("NetEase login cookie is empty on NAS");
    let result;
    if (path === "recommend/songs") {
      result = await api.recommend_songs({ cookie: auth, os: "pc" });
    } else if (path.startsWith("song/url/v1?")) {
      const id = new URL(path, "http://localhost/").searchParams.get("id");
      result = await api.song_url_v1({ id, level: "standard", cookie: auth, os: "pc" });
    } else if (path.startsWith("lyric?")) {
      const id = new URL(path, "http://localhost/").searchParams.get("id");
      result = await api.lyric({ id, cookie: auth, os: "pc" });
    } else {
      throw new Error("Unsupported NetEase API path");
    }
    return result.body;
  }
  const response = await fetch(new URL(path, `${apiBase}/`), {
    headers: cookie ? { Cookie: cookie } : {},
    signal: AbortSignal.timeout(12000),
  });
  if (!response.ok) throw new Error(`NetEase API returned HTTP ${response.status}`);
  return response.json();
}

async function connectDevice() {
  const Socket = globalThis.WebSocket ||
    createRequire(join(moduleRoot || process.cwd(), "package.json"))("ws");
  const socket = new Socket(deviceUrl);
  await Promise.race([
    new Promise((resolve, reject) => {
      socket.addEventListener("open", resolve, { once: true });
      socket.addEventListener("error", () => reject(new Error("Tab5 connection failed")), { once: true });
    }),
    sleep(5000).then(() => { throw new Error("Tab5 connection timed out"); }),
  ]);
  return socket;
}

let nextId = 0;
async function callDevice(socket, name, args = {}) {
  const id = ++nextId;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => finish(new Error(`${name} timed out`)), 15000);
    function finish(error, result) {
      clearTimeout(timer);
      socket.removeEventListener("message", onMessage);
      socket.removeEventListener("close", onClose);
      if (error) reject(error); else resolve(result);
    }
    function onClose() { finish(new Error("Tab5 disconnected")); }
    function onMessage(event) {
      let message;
      try { message = JSON.parse(String(event.data)); } catch { return; }
      if (message.type === "mcp") message = message.payload;
      if (message?.id !== id) return;
      if (message.error) return finish(new Error(message.error.message || name));
      const result = message.result;
      if (result?.isError) return finish(new Error(`${name} failed`));
      finish(null, (result?.content || []).filter((part) => part.type === "text")
        .map((part) => part.text).join(" "));
    }
    socket.addEventListener("message", onMessage);
    socket.addEventListener("close", onClose, { once: true });
    socket.send(JSON.stringify({ jsonrpc: "2.0", id, method: "tools/call",
      params: { name, arguments: args } }));
  });
}

async function musicState(socket, legacy, expectedStation = "") {
  const text = await callDevice(socket, legacy ? "self.radio.get_status" : "self.music.get_status");
  const status = JSON.parse(text);
  const state = status?.state;
  if (legacy && state === "playing" && expectedStation && status.station !== expectedStation)
    return "stopped";
  if (!["stopped", "playing", "ended", "unavailable"].includes(state))
    throw new Error("Tab5 returned an unknown music state");
  return state;
}

async function playDaily() {
  if (!moduleRoot && !apiBase)
    throw new Error("Set NETEASE_MODULE_ROOT for NAS or NETEASE_API_BASE for an HTTP service");
  if (apiBase) requireLocalApi(apiBase);
  const songs = parseRecommendations(await apiGet("recommend/songs"));
  console.log(`Today's recommendations: ${songs.length} songs`);
  const socket = await connectDevice();
  let skipped = 0;
  try {
    let legacy = false;
    try {
      await musicState(socket, false);
    } catch {
      legacy = true;
      await musicState(socket, true);
      console.log("Using current firmware: song completion is estimated from duration");
    }
    for (const [index, song] of songs.entries()) {
      let url;
      try {
        url = parsePlayableUrl(await apiGet(`song/url/v1?id=${song.id}&level=standard`));
      } catch (error) {
        console.error(`Song ${index + 1}: link request failed: ${error.message}`);
      }
      if (!url) {
        skipped++;
        console.log(`Skipping unavailable song ${index + 1}: ${song.title}`);
        continue;
      }
      let lyrics = "";
      try {
        const data = await apiGet(`lyric?id=${song.id}`);
        const lrc = data?.lrc?.lyric;
        if (typeof lrc === "string" && Buffer.byteLength(lrc) <= 16000) lyrics = lrc;
      } catch { /* Playback does not depend on lyrics. */ }
      const response = await callDevice(socket, "self.music.play_url", {
        title: song.title, artist: song.artist, url, lyrics, song_id: song.id,
      });
      if (response.includes("NOT started")) throw new Error(`Tab5 rejected song ${index + 1}`);
      console.log(`Playing ${index + 1}/${songs.length}: ${song.title} — ${song.artist}`);
      const expectedStation = song.title + (song.artist ? ` - ${song.artist}` : "");
      const startedAt = Date.now();
      let state = "stopped";
      for (let attempt = 0; attempt < 30; attempt++) {
        state = await musicState(socket, legacy, expectedStation);
        if (state !== "stopped") break;
        await sleep(1500);
      }
      if (state === "stopped") throw new Error("Tab5 did not start the song");
      while (state === "playing") {
        await sleep(1500);
        state = await musicState(socket, legacy, expectedStation);
      }
      if (legacy && state === "stopped" && song.durationMs > 0 &&
          Date.now() - startedAt >= song.durationMs * 0.85) state = "ended";
      if (state === "stopped") {
        console.log("Playback stopped on Tab5; ending daily recommendations");
        return;
      }
      if (state === "unavailable") {
        skipped++;
        console.log(`Skipping interrupted song ${index + 1}`);
      }
    }
    console.log(`Daily recommendations finished; ${skipped} unavailable song(s) skipped`);
  } finally {
    socket.close();
  }
}

if (process.argv[1] && import.meta.url === new URL(process.argv[1], "file:").href) {
  playDaily().catch((error) => {
    console.error(error.message || String(error));
    process.exitCode = 1;
  });
}
