/* Ask a running OBS whether it registered our filter kind.
 * usage: node obs_kinds.cjs <port> [filterKindSubstring]
 */
const WebSocket = require("C:\\Users\\qiusen\\.dsh\\dsh_wechat\\tools\\rea\\node_modules\\ws");

const PORT = process.argv[2] || "4456";
const NEEDLE = (process.argv[3] || "colorwheel").toLowerCase();

const ws = new WebSocket(`ws://127.0.0.1:${PORT}`);
let nextId = 1;
const pending = new Map();

function req(type) {
  return new Promise((resolve, reject) => {
    const requestId = String(nextId++);
    pending.set(requestId, { resolve, reject });
    ws.send(JSON.stringify({ op: 6, d: { requestType: type, requestId, requestData: {} } }));
    setTimeout(() => {
      if (pending.has(requestId)) {
        pending.delete(requestId);
        reject(new Error(type + " timeout"));
      }
    }, 10000);
  });
}

ws.on("message", (raw) => {
  let m;
  try {
    m = JSON.parse(raw.toString());
  } catch {
    return;
  }
  if (m.op === 0) {
    ws.send(JSON.stringify({ op: 1, d: { rpcVersion: 1 } }));
    return;
  }
  if (m.op === 2) {
    (async () => {
      const v = await req("GetVersion");
      console.log(`obs=${v.obsVersion} websocket=${v.obsWebSocketVersion}`);
      const k = await req("GetSourceFilterKindList");
      const kinds = k.sourceFilterKinds || [];
      const hits = kinds.filter((x) => x.toLowerCase().includes(NEEDLE));
      console.log(`filter kinds total=${kinds.length}`);
      console.log(`match(${NEEDLE})=${hits.length ? hits.join(",") : "NONE"}`);
      ws.close();
      process.exit(hits.length ? 0 : 1);
    })().catch((e) => {
      console.error("ERR " + e.message);
      process.exit(2);
    });
    return;
  }
  if (m.op === 7) {
    const p = pending.get(m.d.requestId);
    if (!p) return;
    pending.delete(m.d.requestId);
    if (m.d.requestStatus.result) p.resolve(m.d.responseData || {});
    else p.reject(new Error(m.d.requestStatus.comment));
  }
});

ws.on("error", (e) => {
  console.error("WS " + e.message);
  process.exit(3);
});
