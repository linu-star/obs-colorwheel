// Parse Resolve-exported .cube LUTs and extract the transfer functions.
const fs = require("fs");
const path = require("path");

const dir = "D:\\Users\\Desktop";
const files = fs.readdirSync(dir).filter(f => /^0\d_.*\.cube$/i.test(f)).sort();

function parseCube(p) {
  const txt = fs.readFileSync(p, "latin1");
  let n = 0, vals = null, k = 0;
  const lines = txt.split(/\r?\n/);
  for (const raw of lines) {
    const s = raw.trim();
    if (!s || s.startsWith("#")) continue;
    if (/^TITLE/i.test(s)) continue;
    const m = /^LUT_3D_SIZE\s+(\d+)/i.exec(s);
    if (m) { n = +m[1]; vals = new Float32Array(n * n * n * 3); continue; }
    if (/^LUT_1D_SIZE/i.test(s)) { throw new Error("1D LUT in " + p); }
    if (/^DOMAIN_/i.test(s)) continue;
    const parts = s.split(/\s+/);
    if (parts.length !== 3) continue;
    const a = +parts[0], b = +parts[1], c = +parts[2];
    if (!Number.isFinite(a)) continue;
    if (!vals) continue;
    vals[k++] = a; vals[k++] = b; vals[k++] = c;
  }
  return { n, vals, count: k / 3 };
}

const idx = (n, r, g, b) => (r + g * n + b * n * n) * 3;
const at = (cube, r, g, b) => {
  const o = idx(cube.n, r, g, b);
  return [cube.vals[o], cube.vals[o + 1], cube.vals[o + 2]];
};

const T = [0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0];
const fmt = v => (Math.abs(v) < 1e-6 ? 0 : v).toFixed(4);

for (const f of files) {
  const p = path.join(dir, f);
  const cube = parseCube(p);
  const tag = f.slice(0, 2) + " " + f.slice(3, f.indexOf(".cube"));
  console.log(`\n===== ${tag}   n=${cube.n}  entries=${cube.count} =====`);

  console.log("  neutral axis (r=g=b): input -> output");
  console.log("    " + T.map(t => {
    const i = Math.round(t * (cube.n - 1));
    const v = at(cube, i, i, i);
    return `${t.toFixed(3)}:${fmt(v[0])}/${fmt(v[1])}/${fmt(v[2])}`;
  }).join("  "));

  // maximum deviation from identity, sampled on a coarse grid
  let maxd = 0, maxat = null;
  const step = 8;
  for (let b = 0; b < cube.n; b += step)
    for (let g = 0; g < cube.n; g += step)
      for (let r = 0; r < cube.n; r += step) {
        const v = at(cube, r, g, b);
        const inr = r / (cube.n - 1), ing = g / (cube.n - 1), inb = b / (cube.n - 1);
        const d = Math.max(Math.abs(v[0] - inr), Math.abs(v[1] - ing), Math.abs(v[2] - inb));
        if (d > maxd) { maxd = d; maxat = [inr, ing, inb]; }
      }
  console.log(`  max |out-in| = ${maxd.toFixed(5)} at in=${maxat.map(x => x.toFixed(3)).join(",")}`);

  console.log("  pure-R axis (input r,0,0) -> out R / G / B");
  console.log("    " + T.map(t => {
    const i = Math.round(t * (cube.n - 1));
    const v = at(cube, i, 0, 0);
    return `${t.toFixed(2)}:${fmt(v[0])}/${fmt(v[1])}/${fmt(v[2])}`;
  }).join("  "));
}
