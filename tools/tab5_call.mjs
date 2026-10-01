// node tools/tab5_call.mjs <tool> '<json args>' [timeout_s] — call a Tab5 MCP tool over the LAN WebSocket.
const [tool, args = "{}", timeout = "40"] = process.argv.slice(2);
const ws = new WebSocket(`ws://${process.env.TAB5_HOST || "192.168.88.101"}:8080/ws`);
const t = setTimeout(() => { console.log("TIMEOUT"); process.exit(1); }, Number(timeout) * 1000);
ws.addEventListener("error", () => { console.log("CONNECT FAILED"); process.exit(1); });
ws.addEventListener("open", () => ws.send(JSON.stringify({ jsonrpc: "2.0", id: 1, method: "tools/call",
  params: { name: tool, arguments: JSON.parse(args) } })));
ws.addEventListener("message", (e) => {
  let m; try { m = JSON.parse(String(e.data)); } catch { return; }
  if (m?.type === "mcp") m = m.payload;
  if (m?.id !== 1) return;
  clearTimeout(t);
  const r = m.result ?? m.error;
  console.log(r?.content ? r.content.map((c) => c.text).join("\n") : JSON.stringify(r));
  process.exit(0);
});
