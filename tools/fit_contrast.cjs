// Fit the contrast transfer curve measured at UI 2.000 (=100%, the slider max).
const fs = require("fs");
const path = require("path");
const dir = "D:\\Users\\Desktop";

function parseCube(p) {
  const txt = fs.readFileSync(p, "latin1");
  let n = 0, v = null, k = 0;
  for (const raw of txt.split(/\r?\n/)) {
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
const f = fs.readdirSync(dir).filter(x => x.toLowerCase().startsWith("05_contrast_v2") && /\.cube$/i.test(x))[0];
const c = parseCube(path.join(dir, f));
const n = c.n;
const diag = [];
for (let i = 0; i < n; i++) {
  const o = (i + i * n + i * n * n) * 3;
  diag.push([i / (n - 1), c.v[o]]);
}

const maxerr = (pred) => {
  let m = 0;
  for (const [x, y] of diag) m = Math.max(m, Math.abs(pred(x) - y));
  return m;
};
const clamp = v => v < 0 ? 0 : v > 1 ? 1 : v;

// A: power around a free pivot  out = p*(x/p)^C
let bestA = { e: 1e9 };
for (let p = 0.30; p <= 0.55; p += 0.0025)
  for (let C = 1; C <= 60; C += 0.25) {
    const e = maxerr(x => clamp(p * Math.pow(x / p, C)));
    if (e < bestA.e) bestA = { e, p, C };
  }

// B: affine with free pivot and clip
let bestB = { e: 1e9 };
for (let p = 0.30; p <= 0.55; p += 0.0025)
  for (let C = 1; C <= 120; C += 0.5) {
    const e = maxerr(x => clamp((x - p) * C + p));
    if (e < bestB.e) bestB = { e, p, C };
  }

// C: affine around 0.5 (axis not the stated pivot)
let bestC = { e: 1e9 };
for (let p = 0.30; p <= 0.60; p += 0.0025)
  for (let C = 1; C <= 120; C += 0.5) {
    const e = maxerr(x => clamp(0.5 + (x - p) * C));
    if (e < bestC.e) bestC = { e, p, C };
  }

// D: power in linear light, display-domain output
const s2l = v => (v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4));
const l2s = v => (v <= 0.0031308 ? v * 12.92 : 1.055 * Math.pow(v, 1 / 2.4) - 0.055);
let bestD = { e: 1e9 };
for (let p = 0.10; p <= 0.30; p += 0.002)
  for (let C = 1; C <= 12; C += 0.05) {
    const e = maxerr(x => clamp(l2s(p * Math.pow(s2l(x) / p, C))));
    if (e < bestD.e) bestD = { e, p, C };
  }

const fmt = o => `maxerr=${o.e.toFixed(5)}  pivot=${o.p !== undefined ? o.p.toFixed(4) : "-"}  C=${o.C !== undefined ? o.C.toFixed(3) : "-"}`;
console.log(`samples: ${diag.length}`);
console.log("A  power  p*(x/p)^C        ", fmt(bestA));
console.log("B  affine (x-p)*C+p        ", fmt(bestB));
console.log("C  affine 0.5+(x-p)*C      ", fmt(bestC));
console.log("D  linear-light power      ", fmt(bestD));
console.log("\nmeasured vs best-A vs best-B:");
for (let i = 0; i < n; i += 6) {
  const [x, y] = diag[i];
  const a = clamp(bestA.p * Math.pow(x / bestA.p, bestA.C));
  const b = clamp((x - bestB.p) * bestB.C + bestB.p);
  console.log(`  in=${x.toFixed(3)}  meas=${y.toFixed(4)}  A=${a.toFixed(4)}  B=${b.toFixed(4)}`);
}
