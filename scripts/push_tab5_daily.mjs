#!/usr/bin/env node
// Send a verified daily brief to the Tab5's local MCP WebSocket endpoint.
// Usage: node scripts/push_tab5_daily.mjs /path/to/brief.json
// Input: {"date":"YYYY-MM-DD","items":[{"title":"...","body":"...",
//         "source_name":"...","source_url":"https://...","published_at":"YYYY-MM-DD"}, ...]}

import { readFile } from "node:fs/promises";

// TAB5_WS_URL wins; otherwise TAB5_HOST (IP or mDNS name) is used; the old fixed IP is the last fallback.
const endpoint = process.env.TAB5_WS_URL ||
  (process.env.TAB5_HOST ? `ws://${process.env.TAB5_HOST}:8080/ws` : "ws://192.168.88.101:8080/ws");
const attempts = Number(process.env.TAB5_PUSH_ATTEMPTS || 4);
const path = process.argv[2];

function fail(message) {
  console.error(message);
  process.exitCode = 1;
}

function localDate() {
  return new Intl.DateTimeFormat("en-CA", {
    timeZone: "Asia/Shanghai", year: "numeric", month: "2-digit", day: "2-digit",
  }).format(new Date());
}

function validate(data, supported) {
  if (data?.date !== localDate()) throw new Error("brief date is not today in Asia/Shanghai");
  if (!Array.isArray(data.items) || data.items.length !== 3)
    throw new Error("exactly three cards are required");
  for (const [index, item] of data.items.entries()) {
    for (const [field, limit] of [["title", 14], ["body", 36]]) {
      const value = item?.[field];
      if (typeof value !== "string" || !value.trim() ||
          [...value].length > limit || /[\x00-\x1f\x7f]/u.test(value)) {
        throw new Error(`card ${index + 1} ${field} must be plain text of 1-${limit} characters`);
      }
      const missing = [...new Set([...value].filter((character) => !supported.has(character)))];
      if (missing.length)
        throw new Error(`card ${index + 1} ${field} has missing font glyphs: ${missing.join("")}`);
    }
    if (typeof item.source_name !== "string" || !item.source_name.trim())
      throw new Error(`card ${index + 1} needs a source name`);
    let source;
    try { source = new URL(item.source_url); } catch {
      throw new Error(`card ${index + 1} needs a source URL`);
    }
    if (source.protocol !== "https:" || !source.hostname.includes("."))
      throw new Error(`card ${index + 1} needs a public HTTPS source`);
    const published = Date.parse(`${item.published_at}T00:00:00+08:00`);
    const age = Date.now() - published;
    if (!/^\d{4}-\d{2}-\d{2}$/u.test(item.published_at) || !Number.isFinite(published) ||
        age < -86400000 || age > 60 * 86400000)
      throw new Error(`card ${index + 1} source must be dated within 60 days`);
  }
}

function callDevice(socket, method, params, id) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      socket.removeEventListener("message", onMessage);
      reject(new Error(`${method} timed out`));
    }, 12000);
    function onMessage(event) {
      let message;
      try { message = JSON.parse(String(event.data)); } catch { return; }
      if (message?.type === "mcp") message = message.payload;
      if (message?.id !== id) return;
      clearTimeout(timer);
      socket.removeEventListener("message", onMessage);
      if (message.error) reject(new Error(message.error.message || `${method} failed`));
      else resolve(message.result);
    }
    socket.addEventListener("message", onMessage);
    socket.send(JSON.stringify({ jsonrpc: "2.0", id, method, params }));
  });
}

async function pushOnce(brief) {
  let socket;
  try {
    socket = new WebSocket(endpoint);
    await Promise.race([
      new Promise((resolve, reject) => {
        socket.addEventListener("open", resolve, { once: true });
        socket.addEventListener("error", () => reject(new Error("device connection failed")), { once: true });
      }),
      new Promise((_, reject) => setTimeout(() => reject(new Error("device connection timed out")), 5000)),
    ]);
    const args = Object.fromEntries(brief.items.flatMap((item, i) =>
      [[`title${i}`, item.title], [`body${i}`, item.body]]));
    const result = await callDevice(socket, "tools/call", {
      name: "self.daily.set_cards", arguments: args,
    }, 1);
    const text = (result?.content || []).filter((part) => part.type === "text")
      .map((part) => part.text).join(" ");
    if (result?.isError || !text.includes("Daily cards updated"))
      throw new Error(`device did not confirm card update: ${text.slice(0, 200)}`);
  } finally {
    socket?.close();
  }
}

try {
  if (!path) throw new Error("pass a JSON brief file path");
  const raw = await readFile(path, "utf8");
  if (Buffer.byteLength(raw) > 3072) throw new Error("brief file exceeds 3 KB");
  const brief = JSON.parse(raw);
  const glyphs = await readFile(new URL("./tab5_dynamic_symbols.txt", import.meta.url), "utf8");
  const supported = new Set([...glyphs, ...Array.from({ length: 96 }, (_, i) => String.fromCharCode(i + 32))]);
  validate(brief, supported);
  // The device may be asleep, roaming or rebooting at 08:00: retry with growing pauses.
  let lastError;
  for (let attempt = 1; attempt <= attempts; ++attempt) {
    try {
      await pushOnce(brief);
      console.log(JSON.stringify({ ok: true, date: brief.date, cards: 3, device: endpoint, attempt }));
      lastError = undefined;
      break;
    } catch (error) {
      lastError = error;
      console.error(`attempt ${attempt}/${attempts} failed: ${error.message || error}`);
      if (attempt < attempts) await new Promise((r) => setTimeout(r, 5000 * 3 ** (attempt - 1)));
    }
  }
  if (lastError) throw lastError;
} catch (error) {
  fail(error.message || String(error));
}
