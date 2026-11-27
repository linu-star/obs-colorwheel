// Decide the gamma model: per-channel power vs luma-coupled power.
const fs = require("fs");
const path = require("path");
const dir = "D:\\Users\\Desktop";

function parseCube(p) {
  const txt = fs.readFileSync(p, "latin1");
  let n = 0, vals = null, k = 0;
  for (const raw of txt.split(/\r?\n/)) {
    const s = raw.trim();
    if (!s || s.startsWith("#") || /^TITLE/i.test(s)) continue;
    const m = /^LUT_3D_SIZE\s+(\d+)/i.exec(s);
    if (m) { n = +m[1]; vals = new Float32Array(n * n * n * 3); continue; }
    if (/^DOMAIN_/i.test(s) || /^LUT_1D/i.test(s)) continue;
    const q = s.split(/\s+/);
    if (q.length !== 3 || !vals) continue;
    const a = +q[0], b = +q[1], c = +q[2];
    if (!Number.isFinite(a)) continue;
    vals[k++] = a; vals[k++] = b; vals[k++] = c;
  }
  return { n, vals };
}
const at = (c, r, g, b) => { const o = ((r + g * c.n + b * c.n * c.n) * 3); return [c.vals[o], c.vals[o + 1], c.vals[o + 2]]; };
const find = (pat) => { const f = fs.readdirSync(dir).filter(x => x.toLowerCase().startsWith(pat.toLowerCase()) && /\.cube$/i.test(x)); return f.length ? path.join(dir, f[0]) : null; };

const c = parseCube(find("03_gamma_v2"));
const n = c.n;
const toIdx = v => Math.round(v * (n - 1));
const Y = v => 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
const E = 3.0;

const tests = [
  [0.25, 0, 0], [0.5, 0, 0], [0.75, 0, 0],
  [0.5, 0.5, 0], [0, 0, 0.5], [0.8, 0.2, 0.1],
  [0.2, 0.8, 0.1], [1, 1, 1],
];

console.log("input            actual(R/G/B)          per-channel            luma-coupled");
console.log("                                                              f(Y)+(c-Y)");
for (const t of tests) {
  const a = at(c, toIdx(t[0]), toIdx(t[1]), toIdx(t[2]));
  const per = t.map(v => Math.pow(v, 1 / E));
  const y = Y(t);
  const fy = Math.pow(Math.max(y, 0), 1 / E);
  const lum = t.map(v => fy + (v - y));
  const f = arr => arr.map(v => v.toFixed(4)).join(" ");
  const err = (m) => Math.max(...a.map((v, i) => Math.abs(v - m[i])));
  console.log(
    `(${t.map(v => v.toFixed(2)).join(",")})`.padEnd(17) +
    f(a).padEnd(23) + f(per).padEnd(23) + f(lum)
  );
  console.log(
    `                 maxerr:  per-channel ${err(per).toFixed(4)}   luma-coupled ${err(lum).toFixed(4)}`
  );
}
