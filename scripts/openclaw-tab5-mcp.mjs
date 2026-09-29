#!/usr/bin/env node
/**
 * OpenClaw ↔ tab5 MCP bridge.
 *
 * Speaks MCP JSON-RPC over stdio (what OpenClaw's mcp.servers expects)
 * and proxies tools/* to the tab5 firmware's local WebSocket MCP endpoint
 * (ws://<tab5-ip>:8080/ws).
 *
 * Env:
 *   TAB5_WS_URL  default ws://192.168.88.101:8080/ws
 *   TAB5_MCP_LOG 1 to log bridge traffic to stderr
 */

const WS_URL = process.env.TAB5_WS_URL || "ws://192.168.88.101:8080/ws";
const LOG = process.env.TAB5_MCP_LOG === "1";

function log(...args) {
  if (LOG) console.error("[tab5-mcp]", ...args);
}

/** @type {WebSocket | null} */
let ws = null;
/** @type {Map<number|string, (msg: any) => void>} */
const pending = new Map();
let nextLocalId = 1;
const outbound = [];

function connect(attempt = 0) {
  return new Promise((resolve, reject) => {
    const sock = new WebSocket(WS_URL);
    const timer = setTimeout(() => {
      try { sock.close(); } catch {}
      if (attempt < 3) {
        setTimeout(() => connect(attempt + 1).then(resolve, reject), 400 * (attempt + 1));
      } else {
        reject(new Error(`tab5 ws connect timeout: ${WS_URL}`));
      }
    }, 5000);

    sock.addEventListener("open", () => {
      clearTimeout(timer);
      ws = sock;
      log("ws open", WS_URL, "attempt", attempt);
      while (outbound.length) {
        const line = outbound.shift();
        sock.send(line);
      }
      resolve(sock);
    });

    sock.addEventListener("message", (ev) => {
      let msg;
      try {
        msg = JSON.parse(String(ev.data));
      } catch (err) {
        log("bad ws json", err);
        return;
      }
      // Firmware may wrap as {type:"mcp",payload:{...}} — unwrap.
      if (msg && msg.type === "mcp" && msg.payload) msg = msg.payload;
      if (msg && msg.id !== undefined && pending.has(msg.id)) {
        const resolvePending = pending.get(msg.id);
        pending.delete(msg.id);
        resolvePending(msg);
      } else {
        log("ws unsolicited", msg);
      }
    });

    sock.addEventListener("close", () => {
      clearTimeout(timer);
      if (ws === sock) ws = null;
      log("ws closed");
      for (const [, fn] of pending) {
        fn({ jsonrpc: "2.0", id: null, error: { code: -32000, message: "tab5 ws closed" } });
      }
      pending.clear();
      // Reconnect lazily on next request.
    });

    sock.addEventListener("error", (ev) => {
      clearTimeout(timer);
      log("ws error", ev.message || ev, "attempt", attempt);
      if (!ws && attempt >= 3) reject(new Error("tab5 ws error"));
    });
  });
}

async function ensureWs() {
  if (ws && ws.readyState === WebSocket.OPEN) return ws;
  return connect();
}

function sendToWs(obj) {
  const line = JSON.stringify(obj);
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(line);
  } else {
    outbound.push(line);
  }
}

function writeStdout(obj) {
  process.stdout.write(JSON.stringify(obj) + "\n");
}

function sendRequest(method, params) {
  return new Promise(async (resolve, reject) => {
    try {
      await ensureWs();
    } catch (err) {
      reject(err);
      return;
    }
    const id = nextLocalId++;
    pending.set(id, (msg) => {
      if (msg.error) reject(Object.assign(new Error(msg.error.message || "tool error"), { rpc: msg.error }));
      else resolve(msg.result);
    });
    sendToWs({ jsonrpc: "2.0", id, method, params: params || {} });
    setTimeout(() => {
      if (pending.has(id)) {
        pending.delete(id);
        reject(new Error(`tab5 timeout for ${method}`));
      }
    }, 20000);
  });
}

/** Normalize device tools/call result into MCP content shape if needed. */
function normalizeToolResult(result) {
  if (!result || typeof result !== "object") {
    return { content: [{ type: "text", text: String(result ?? "") }] };
  }
  if (Array.isArray(result.content)) return result;
  // Device Call() already returns {content:[...]}; anything else is wrapped.
  return { content: [{ type: "text", text: JSON.stringify(result) }] };
}

async function handleClientMessage(msg) {
  const { id, method, params } = msg || {};

  // Notifications: acknowledge silently (device also ignores notifications).
  if (method && method.startsWith("notifications/")) return;

  if (id === undefined || id === null) {
    log("no-id message", method);
    return;
  }

  try {
    if (method === "initialize") {
      // Local handshake so OpenClaw gets a stable MCP identity even if the
      // device is briefly offline; tools still require a live device.
      writeStdout({
        jsonrpc: "2.0",
        id,
        result: {
          protocolVersion: params?.protocolVersion || "2024-11-05",
          capabilities: { tools: { listChanged: true } },
          serverInfo: { name: "tab5", version: "0.1.0" },
        },
      });
      return;
    }

    if (method === "ping") {
      writeStdout({ jsonrpc: "2.0", id, result: {} });
      return;
    }

    if (method === "tools/list") {
      const result = await sendRequest("tools/list", {
        ...params,
        // Always include user-only tools so OpenClaw sees the full surface.
        withUserTools: true,
      });
      writeStdout({ jsonrpc: "2.0", id, result });
      return;
    }

    if (method === "tools/call") {
      const result = await sendRequest("tools/call", params);
      writeStdout({ jsonrpc: "2.0", id, result: normalizeToolResult(result) });
      return;
    }

    // Forward any other method the device understands.
    const result = await sendRequest(method, params);
    writeStdout({ jsonrpc: "2.0", id, result });
  } catch (err) {
    writeStdout({
      jsonrpc: "2.0",
      id,
      error: {
        code: err?.rpc?.code ?? -32000,
        message: err?.message || String(err),
      },
    });
  }
}

// MCP stdio: one JSON object per line.
let buf = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  buf += chunk;
  let idx;
  while ((idx = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, idx).trim();
    buf = buf.slice(idx + 1);
    if (!line) continue;
    let msg;
    try {
      msg = JSON.parse(line);
    } catch (err) {
      log("bad stdin json", err, line.slice(0, 200));
      continue;
    }
    handleClientMessage(msg);
  }
});

process.stdin.on("end", () => process.exit(0));
process.on("SIGINT", () => process.exit(0));
process.on("SIGTERM", () => process.exit(0));

log("bridge ready", WS_URL);
