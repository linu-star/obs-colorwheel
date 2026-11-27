// Residual test: my model vs the LUTs DaVinci actually exported.
// This is the "compare against Resolve" check that can be done offline.
const fs = require("fs");
const path = require("path");
const dir = "D:\\Users\\Desktop";

function parse(p) {
  const t = fs.readFileSync(p, "latin1");
  let n = 0, v = null, k = 0;
  for (const raw of t.split(/\r?\n/)) {
    const s = raw.trim();
    if (!s || s[0] === "#" || /^TITLE/i.test(s)) continue;
    const m = /^LUT_3D_SIZE\s+(\d+)/i.exec(s);
    if (m) { n = +m[1]; v = new Float32Array(n * n * n * 3); continue; }
    if (/^DOMAIN_/i.test(s) || /^LUT_1D/i.test(s)) continue;
    const q = s.split(/\s+/);
    if (q.length !== 3 || !v) continue;
    const a = +q[0], b = +q[1], c = +q[2];
    if (!Number.isFinite(a)) continue;
    v[k++] = a; v[k++] = b; v[k++] = c;
  }
  return { n, v };
}
function diagOf(prefix) {
  const f = fs.readdirSync(dir).filter(x => x.toLowerCase().startsWith(prefix) && /\.cube$/i.test(x))[0];
  if (!f) return null;
  const c = parse(path.join(dir, f));
  const d = [];
  for (let i = 0; i < c.n; i++) {
    const o = (i + i * c.n + i * c.n * c.n) * 3;
    d.push([i / (c.n - 1), c.v[o]]);
  }
  return d;
}

const clamp = v => (v < 0 ? 0 : v > 1 ? 1 : v);
const PIVOT = 0.435;
const OFFSET_UNIT = 0.0073327;

/* ---- the model exactly as implemented in obs-colorwheel.c ---- */
const fLift = ui => { const L = 2 * ui; return x => clamp(L + (1 - L) * x); };
const fGain = ui => { const k = ui; return x => clamp(k * x); };
const fOffset = ui => { const o = (ui - 25) * OFFSET_UNIT; return x => clamp(x + o); };
const fGamma = ui => {
  const e = ui >= 0 ? 1 + 4 * ui : 1 / (1 - 4 * ui);
  return x => (x <= 0 ? 0 : Math.pow(x, 1 / e));
};
function contrastK(ui) {
  const d = ui - 1;
  if (d === 0) return 0;
  if (d > 0) {
    if (d <= 0.5) return d / 0.5 * 9.48;
    if (ui >= 2) return 528;
    return 9.48 * Math.pow(528 / 9.48, (d - 0.5) / 0.5);
  }
  const a = -d;
  if (a > 1) return -528;
  return -a / 0.5 * 9.48;
}
// normalised logistic, exactly as the shader computes it
const fContrast = ui => {
  const k = contrastK(ui);
  if (Math.abs(k) < 1e-3) return x => x;
  const s = t => 1 / (1 + Math.exp(-k * (t - PIVOT)));
  const s0 = s(0), inv = 1 / (s(1) - s(0));
  return x => { const v = (s(x) - s0) * inv; return v < 0 ? 0 : v > 1 ? 1 : v; };
};

const cases = [
  ["01_lift",           "暗部 0.204", fLift(0.204)],
  ["01_lift_v2",        "暗部 0.500", fLift(0.5)],
  ["02_gain",           "亮部 1.600", fGain(1.6)],
  ["04_offset_v2",      "偏移 50.00", fOffset(50)],
  ["03_gamma_v3",       "中灰 0.250", fGamma(0.25)],
  ["05_contrast_150",   "对比 1.500", fContrast(1.5)],
  ["05_contrast_v2",    "对比 2.000", fContrast(2.0)],
];

console.log("模型 vs 达芬奇导出的 LUT（中性轴，65 点）");
console.log("case                setting      maxerr   meanerr   最大误差出现在");
console.log("─".repeat(78));
for (const [prefix, label, f] of cases) {
  const d = diagOf(prefix);
  if (!d) { console.log(`${prefix.padEnd(18)} ${label}  (LUT not found)`); continue; }
  let mx = 0, sum = 0, at = 0;
  for (const [x, y] of d) {
    const e = Math.abs(f(x) - y);
    if (e > mx) { mx = e; at = x; }
    sum += e;
  }
  const ok = mx < 0.01 ? "OK " : mx < 0.05 ? "~  " : "XX ";
  console.log(`${prefix.padEnd(18)} ${label.padEnd(12)} ${mx.toFixed(5)}  ${(sum / d.length).toFixed(5)}   in=${at.toFixed(3)}   ${ok}`);
}
