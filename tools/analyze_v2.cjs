// Focused analysis of the v2 exports.
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

// ---------- 03 gamma v2 ----------
{
  const p = find("03_gamma_v2");
  const c = parseCube(p);
  console.log("===== 03_gamma_v2 =====");
  // separability: for pure R input, how much does G/B move?
  let leak = 0, atIn = 0;
  for (let i = 0; i < c.n; i++) {
    const v = at(c, i, 0, 0);
    if (Math.abs(v[1]) > leak) { leak = Math.abs(v[1]); atIn = i / (c.n - 1); }
  }
  console.log(`  cross-channel leak on pure-R axis: max G = ${leak.toFixed(6)} at in=${atIn.toFixed(3)}`);
  // fit exponent e in out = in^(1/e) on the neutral axis
  let sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0;
  for (let i = 1; i < c.n; i++) {
    const x = Math.log(i / (c.n - 1));
    const v = at(c, i, i, i)[0];
    if (v <= 0 || v >= 1) continue;
    const y = Math.log(v);
    sx += x; sy += y; sxx += x * x; sxy += x * y; cnt++;
  }
  const slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
  console.log(`  fitted power: out = in^${slope.toFixed(5)}  => exponent e = ${(1 / slope).toFixed(4)}  (n=${cnt})`);
  console.log("  neutral axis:", [0, 8, 16, 24, 32, 40, 48, 56, 64].map(i => `${(i / 64).toFixed(3)}:${at(c, i, i, i)[0].toFixed(4)}`).join(" "));
}

// ---------- 05 contrast ----------
{
  const p = find("05_contrast_v2") || find("05_contrast");
  const c = parseCube(p);
  console.log("\n===== 05_contrast (v2 identical to v1) =====");
  console.log("  fine neutral axis, in = 0.28 .. 0.60:");
  for (let i = 18; i <= 39; i++)
    console.log(`    in=${(i / 64).toFixed(4)}  out=${at(c, i, i, i)[0].toFixed(6)}`);
}

// ---------- 06 lift red ----------
{
  const p = find("06_lift_red_v2") || find("06_lift_red");
  const c = parseCube(p);
  console.log("\n===== 06_lift_red (v2 identical to v1) =====");
  const v0 = at(c, 0, 0, 0), v1 = at(c, 64, 0, 0);
  console.log(`  pure-R axis: black=${v0.map(x => x.toFixed(4)).join("/")}  white=${v1.map(x => x.toFixed(4)).join("/")}`);
  // slope per channel on the pure-R axis
  for (let ch = 0; ch < 3; ch++) {
    const a = at(c, 0, 0, 0)[ch], b = at(c, 64, 0, 0)[ch];
    console.log(`    channel ${"RGB"[ch]}: ${a.toFixed(4)} + ${((b - a) / 1).toFixed(4)} * in`);
  }
}
