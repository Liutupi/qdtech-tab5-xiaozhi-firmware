// node tools/ir_scan.mjs [seconds] — count IR receiver edges on candidate GPIOs via the Tab5 MCP WebSocket.
const seconds = Number(process.argv[2] || 12);
const ws = new WebSocket(`ws://${process.env.TAB5_HOST || "192.168.88.101"}:8080/ws`);
const t = setTimeout(() => { console.log("TIMEOUT"); process.exit(1); }, (seconds + 15) * 1000);
ws.addEventListener("error", () => { console.log("CONNECT FAILED"); process.exit(1); });
ws.addEventListener("open", () => ws.send(JSON.stringify({ jsonrpc: "2.0", id: 1, method: "tools/call",
  params: { name: "self.ir.scan", arguments: { seconds } } })));
ws.addEventListener("message", (e) => {
  let m; try { m = JSON.parse(String(e.data)); } catch { return; }
  if (m?.type === "mcp") m = m.payload;
  if (m?.id !== 1) return;
  clearTimeout(t);
  console.log(JSON.stringify(m.result ?? m.error));
  process.exit(0);
});
