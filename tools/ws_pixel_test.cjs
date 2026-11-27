/* End-to-end pixel test for obs-colorwheel.
 *
 * Talks to OBS over obs-websocket, sets known filter parameters, grabs the real
 * rendered frame as BMP, and compares it pixel-by-pixel against the same model
 * the CPU engine uses.  This is the layer the unit tests could never cover:
 * shader mistakes, unbound uniforms, wrong uniform packing, colour-space and
 * 16F issues all show up here and nowhere else.
 *
 * usage: node ws_pixel_test.cjs <port> <password>
 */
const path = require("path");
const crypto = require("crypto");
const WebSocket = require("C:\\Users\\qiusen\\.dsh\\dsh_wechat\\tools\\rea\\node_modules\\ws");

const PORT = process.argv[2] || "4455";
const PASS = process.argv[3] || "";
const HOST = "127.0.0.1";

/* ------------------------------------------------------------------ client */
class Obs {
  constructor() {
    this.ws = null;
    this.nextId = 1;
    this.pending = new Map();
    this.ready = null;
  }
  connect() {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(`ws://${HOST}:${PORT}`);
      this.ws = ws;
      const timer = setTimeout(() => reject(new Error("connect timeout")), 15000);
      ws.on("error", (e) => { clearTimeout(timer); reject(e); });
      ws.on("message", (raw) => {
        let m;
        try { m = JSON.parse(raw.toString()); } catch { return; }
        if (m.op === 0) {
          let auth;
          if (m.d.authentication) {
            const { challenge, salt } = m.d.authentication;
            const secret = crypto.createHash("sha256").update(PASS + salt).digest("base64");
            auth = crypto.createHash("sha256").update(secret + challenge).digest("base64");
          }
          ws.send(JSON.stringify({ op: 1, d: { rpcVersion: 1, authentication: auth } }));
        } else if (m.op === 2) {
          clearTimeout(timer);
          resolve();
        } else if (m.op === 7) {
          const p = this.pending.get(m.d.requestId);
          if (!p) return;
          this.pending.delete(m.d.requestId);
          if (m.d.requestStatus.result) p.resolve(m.d.responseData || {});
          else p.reject(new Error(`${m.d.requestType}: ${m.d.requestStatus.comment}`));
        }
      });
    });
  }
  req(type, data = {}) {
    const requestId = String(this.nextId++);
    return new Promise((resolve, reject) => {
      this.pending.set(requestId, { resolve, reject });
      this.ws.send(JSON.stringify({ op: 6, d: { requestType: type, requestId, requestData: data } }));
      setTimeout(() => {
        if (this.pending.has(requestId)) {
          this.pending.delete(requestId);
          reject(new Error(`${type} timeout`));
        }
      }, 20000);
    });
  }
  close() { try { this.ws.close(); } catch {} }
}

/* -------------------------------------------------------------------- bmp */
function decodeBmp(buf) {
  const off = buf.readUInt32LE(10);
  const w = buf.readInt32LE(18);
  const rawH = buf.readInt32LE(22);
  const bpp = buf.readUInt16LE(28);
  const topDown = rawH < 0;
  const H = Math.abs(rawH);
  if (bpp !== 24 && bpp !== 32) throw new Error("unsupported bmp bpp " + bpp);
  const rowSize = Math.floor((bpp * w + 31) / 32) * 4;
  const px = (x, y) => {
    const yy = topDown ? y : H - 1 - y;
    const o = off + yy * rowSize + x * (bpp / 8);
    return [buf[o + 2], buf[o + 1], buf[o]];
  };
  return { w, h: H, px };
}

/* --------------------------------------------------- the model (mirrors C) */
const OFF_UNIT = 0.0073327;
const LIFT_MUL = 2.0;
const PIVOT = 0.435;
function gammaExp(ui) { return ui >= 0 ? 1 + 4 * ui : 1 / (1 - 4 * ui); }
function contrastK(ui) {
  const d = ui - 1;
  if (d === 0) return 0;
  if (d > 0) {
    if (d <= 0.5) return d / 0.5 * 9.48;
    if (ui >= 2) return 528;
    return 9.48 * Math.pow(528 / 9.48, (d - 0.5) / 0.5);
  }
  const a = -d;
  return a > 1 ? -528 : -a / 0.5 * 9.48;
}
const spow = (v, e) => (v < 0 ? -Math.pow(-v, e) : Math.pow(v, e));

function toneChannel(p, ch, x) {
  const off = (p.offset[ch] - 25) * OFF_UNIT;
  const g = gammaExp(p.gamma[ch]);
  const k = p.gain[ch];
  const l = p.lift[ch] * LIFT_MUL;
  let v = x + off;
  v = v * (k - l) + l;
  v = spow(v, 1 / g);
  const ck = contrastK(p.contrast);
  if (ck !== 0) {
    const s = (t) => 1 / (1 + Math.exp(-ck * (t - PIVOT)));
    const s0 = s(0), inv = 1 / (s(1) - s(0));
    v = (s(v) - s0) * inv;
  }
  return v;
}
const NEUTRAL = {
  lift: [0, 0, 0], gamma: [0, 0, 0], gain: [1, 1, 1], offset: [25, 25, 25],
  contrast: 1, pivot: 0.435, sat: 50, hue: 50, lumMix: 100,
};

/* ------------------------------------------------------------------- main */
(async () => {
  const obs = new Obs();
  await obs.connect();

  const inputs = (await obs.req("GetInputList")).inputs || [];
  let src = null, filt = null, best = -1;
  for (const inp of inputs) {
    let fl;
    try { fl = (await obs.req("GetSourceFilterList", { sourceName: inp.inputName })).filters || []; }
    catch { continue; }
    const f = fl.find((x) => x.filterKind === "obs_colorwheel_filter");
    if (!f) continue;
    /* Prefer a still image source: the capture card source may have no signal
     * (0x0) or show live content, and the sample coordinates assume the chart. */
    let score = 1;
    if (inp.inputKind === "image_source") score = 10;
    if (/chart|测试/i.test(inp.inputName)) score += 5;
    if (score > best) { best = score; src = inp.inputName; filt = f.filterName; }
  }
  if (!src) { console.log("SKIP: no source with obs_colorwheel_filter found"); obs.close(); process.exit(2); }
  console.log(`source="${src}"  filter="${filt}"`);

  // chart is 1920x1080; sample where the layout is known to be flat colour
  const SAMPLES = [
    [60, 120, "white bar"], [300, 120, "yellow bar"], [900, 120, "magenta bar"],
    [1700, 120, "blue bar"], [960, 350, "grey ramp mid"], [960, 510, "step 128"],
    [100, 510, "step 0"], [960, 950, "near-neutral ramp"], [1200, 700, "skin mid"],
  ];

  async function shot() {
    const r = await obs.req("GetSourceScreenshot", {
      sourceName: src, imageFormat: "bmp",
    });
    /* obs-websocket returns a data URL: "data:image/bmp;base64,...." */
    const b64 = String(r.imageData).replace(/^data:[^,]*,/, "");
    return decodeBmp(Buffer.from(b64, "base64"));
  }
  async function setParams(p) {
    const s = {
      pg_liftR: p.lift[0], pg_liftG: p.lift[1], pg_liftB: p.lift[2],
      pg_gammaR: p.gamma[0], pg_gammaG: p.gamma[1], pg_gammaB: p.gamma[2],
      pg_gainR: p.gain[0], pg_gainG: p.gain[1], pg_gainB: p.gain[2],
      pg_offR: p.offset[0], pg_offG: p.offset[1], pg_offB: p.offset[2],
      pg_contrast: p.contrast, pg_pivot: p.pivot,
      pg_sat: p.sat, pg_hue: p.hue, pg_lummix: p.lumMix,
    };
    await obs.req("SetSourceFilterSettings", {
      sourceName: src, filterName: filt, filterSettings: s, overlay: true,
    });
    await new Promise((r) => setTimeout(r, 400));
  }

  await setParams(NEUTRAL);
  const base = await shot();
  console.log(`frame ${base.w}x${base.h}`);

  const cases = [
    ["neutral", NEUTRAL, 2],
    ["lift R +0.20", { ...NEUTRAL, lift: [0.2, 0, 0] }, 4],
    ["gain 1.50", { ...NEUTRAL, gain: [1.5, 1.5, 1.5] }, 4],
    ["gamma 0.30", { ...NEUTRAL, gamma: [0.3, 0.3, 0.3] }, 4],
    ["offset 35", { ...NEUTRAL, offset: [35, 35, 35] }, 4],
    ["contrast 1.50", { ...NEUTRAL, contrast: 1.5 }, 6],
  ];

  let fails = 0;
  for (const [name, p, tol] of cases) {
    await setParams(p);
    const img = await shot();
    let worst = 0, worstAt = "";
    for (const [x, y, label] of SAMPLES) {
      const sx = Math.min(x, img.w - 1), sy = Math.min(y, img.h - 1);
      const got = img.px(sx, sy);
      const inPix = base.px(sx, sy);
      const exp = inPix.map((v, ch) => {
        const t = toneChannel(p, ch, v / 255);
        return Math.max(0, Math.min(255, Math.round(t * 255)));
      });
      const d = Math.max(Math.abs(got[0] - exp[0]), Math.abs(got[1] - exp[1]), Math.abs(got[2] - exp[2]));
      if (d > worst) { worst = d; worstAt = `${label}(${sx},${sy}) got=${got} exp=${exp}`; }
    }
    const ok = worst <= tol;
    if (!ok) fails++;
    console.log(`${ok ? "PASS" : "FAIL"}  ${name.padEnd(14)} maxdiff=${String(worst).padStart(3)} (tol ${tol})  ${ok ? "" : worstAt}`);
  }

  obs.close();
  console.log(`\n${fails === 0 ? "PIXEL TEST PASSED" : fails + " CASE(S) FAILED"}`);
  process.exit(fails === 0 ? 0 : 1);
})().catch((e) => { console.error("ERROR:", e.message); process.exit(3); });
