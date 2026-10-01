#!/usr/bin/env node
// Tell the Tab5 where the Muse relay is: node set_tab5_url.mjs <MCP or inbox URL> [tab5-host]
// Runs on a computer in the Tab5's LAN (talks to the Tab5 MCP WebSocket on port 8080).
const [url, host = process.env.TAB5_HOST || "192.168.88.101"] = process.argv.slice(2);
if (!url || !/^https:\/\/\S+\/(mcp|inbox)\/\S+$/.test(url)) {
  console.error("usage: set_tab5_url.mjs https://xxx.trycloudflare.com/mcp/<token> [tab5-ip]");
  process.exit(2);
}
const socket = new WebSocket(`ws://${host}:8080/ws`);
const fail = (msg) => { console.error("FAILED:", msg); process.exit(1); };
const timer = setTimeout(() => fail("timed out"), 10000);
socket.addEventListener("error", () => fail(`cannot connect to Tab5 at ${host}:8080`));
socket.addEventListener("open", () => {
  socket.send(JSON.stringify({
    jsonrpc: "2.0", id: 1, method: "tools/call",
    params: { name: "self.muse.set_url", arguments: { url } },
  }));
});
socket.addEventListener("message", (event) => {
  let message;
  try { message = JSON.parse(String(event.data)); } catch { return; }
  if (message?.type === "mcp") message = message.payload;
  if (message?.id !== 1) return;
  clearTimeout(timer);
  const text = (message.result?.content || []).map((p) => p.text).join(" ");
  if (message.error || message.result?.isError || !/true/.test(text)) fail(JSON.stringify(message).slice(0, 300));
  console.log("OK: Tab5 now polls", url.replace(/\/(mcp|inbox)\/.*/, "/…"));
  socket.close();
  process.exit(0);
});
