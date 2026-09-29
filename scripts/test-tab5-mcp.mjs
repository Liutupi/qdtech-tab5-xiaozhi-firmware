#!/usr/bin/env node
/**
 * Smoke-test the OpenClaw ↔ tab5 MCP bridge against a live device.
 *
 * Usage:
 *   node scripts/test-tab5-mcp.mjs
 *   TAB5_WS_URL=ws://192.168.88.101:8080/ws node scripts/test-tab5-mcp.mjs
 */

import { spawn } from "node:child_process";

const bridge = new URL("./openclaw-tab5-mcp.mjs", import.meta.url).pathname;
const child = spawn("node", [bridge], {
  env: { ...process.env, TAB5_MCP_LOG: "1" },
  stdio: ["pipe", "pipe", "inherit"],
});

let buf = "";
const pending = new Map();

child.stdout.setEncoding("utf8");
child.stdout.on("data", (chunk) => {
  buf += chunk;
  let idx;
  while ((idx = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, idx).trim();
    buf = buf.slice(idx + 1);
    if (!line) continue;
    const msg = JSON.parse(line);
    if (msg.id != null && pending.has(msg.id)) {
      pending.get(msg.id)(msg);
      pending.delete(msg.id);
    }
  }
});

function request(method, params) {
  return new Promise((resolve, reject) => {
    const id = Math.floor(Math.random() * 1e9);
    pending.set(id, (msg) =>
      msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result)
    );
    child.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
    setTimeout(() => {
      if (pending.has(id)) {
        pending.delete(id);
        reject(new Error(`timeout ${method}`));
      }
    }, 15000);
  });
}

const init = await request("initialize", {
  protocolVersion: "2024-11-05",
  capabilities: {},
  clientInfo: { name: "tab5-mcp-test", version: "0.1.0" },
});
console.log("initialize:", JSON.stringify(init, null, 2));

child.stdin.write(
  JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n"
);

const tools = await request("tools/list", { withUserTools: true });
console.log(
  "tools/list:",
  tools.tools?.length ?? 0,
  "tools ->",
  (tools.tools || []).map((t) => t.name).join(", ")
);

if (tools.tools?.length) {
  const name = "self.get_system_info";
  const target = tools.tools.find((t) => t.name === name) || tools.tools[0];
  const result = await request("tools/call", {
    name: target.name,
    arguments: {},
  });
  console.log("tools/call", target.name, "->", JSON.stringify(result).slice(0, 400));
}

child.kill();
console.log("OK");
