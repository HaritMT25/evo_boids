// appearance_vectors.mjs — reference vectors for the appearance genome's C++
// construction (src/ocean/appearance_model.hpp), computed by the accepted
// prototype's own law script.
//
//   node tools/appearance_vectors.mjs <path/to/gallery_v3.html> > tests/ocean_appearance_vectors.txt
//
// It loads gallery v3's pure law script (the <script> that publishes
// globalThis.FL) straight from the HTML and prints:
//   G ...   the gallery's own exported construction vectors
//           (FL.constructionFixtures(): four founders at seed 20261028 and four
//           offspring of the first two at seed 29, with their 160 draws and the
//           P1 / M1 group branches), every float as its bit pattern;
//   H ...   hashes of long runs of the same construction: N founders from
//           MT(seed) (63 draws each), then G generations of random mating
//           (parents by MT.uint, 160 draws per offspring) under a config —
//           one hash per generation over every individual's 63 float bits and
//           its 36 group branches.
// tests/ocean_tests.cpp ("appearance gallery") replays the same procedures
// with PortableMT19937 and the C++ construction and must reproduce every
// float and every hash. The gallery's sha256 is printed in the header.
import fs from "node:fs";
import vm from "node:vm";
import crypto from "node:crypto";

const path = process.argv[2];
if (!path) { console.error("usage: node tools/appearance_vectors.mjs <gallery_v3.html>"); process.exit(2); }
const html = fs.readFileSync(path);
const sha = crypto.createHash("sha256").update(html).digest("hex");
const text = html.toString("utf8");
// the law script: the <script> block whose body defines root.FL
const blocks = [...text.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]);
const law = blocks.find(b => b.includes("root.FL = {"));
if (!law) { console.error("no law script (root.FL) in " + path); process.exit(1); }
vm.runInThisContext(law, { filename: "gallery_v3.law.js" });
const FL = globalThis.FL;

const fb = new Float32Array(1), ub = new Uint32Array(fb.buffer);
const bitsOf = x => { fb[0] = x; return ub[0]; };
const hex = x => bitsOf(x).toString(16).padStart(8, "0");
const out = [];
out.push(`# appearance vectors: gallery_v3.html sha256 ${sha}; node ${process.version}`);
out.push(`# versions ${JSON.stringify(FL.VERSIONS)}`);

// ---- G: the gallery's exported construction vectors ------------------------------
const fx = FL.constructionFixtures();
const c = fx.config;
out.push(`# G founder <63 floats>; G offspring <cfg mut phMut novel mMut mNovel> <A 63> <B 63> <160 draws as k (draw = k*2^-24)> <child 63> <25 modes> <11 morph modes>`);
for (const f of fx.founders) out.push("G founder " + f.map(hex).join(" "));
for (const k of fx.offspring) {
  out.push("G offspring " + [c.mut, c.phMut, c.novel, c.mMut, c.mNovel].map(hex).join(" ") + " " +
    fx.parentA.map(hex).join(" ") + " " + fx.parentB.map(hex).join(" ") + " " +
    k.draws.map(d => Math.round(d * 16777216)).join(" ") + " " + k.child.map(hex).join(" ") + " " +
    k.modes.join(" ") + " " + k.morphModes.join(" "));
}

// ---- H: long runs, hashed per generation --------------------------------------------
function H32() { return { a: 0x811c9dc5, b: 0x01234567 }; }
function add(h, u) { h.a = Math.imul(h.a ^ u, 16777619) >>> 0; h.b = (Math.imul(h.b, 0x9E3779B1) + u) >>> 0; }
const hstr = h => h.a.toString(16).padStart(8, "0") + h.b.toString(16).padStart(8, "0");
function hashPop(pop) {
  const h = H32();
  for (const G of pop) {
    for (let j = 0; j < FL.NF; j++) add(h, bitsOf(G.g[j]));
    if (G.modes) { for (const m of G.modes) add(h, m); for (const m of G.mmodes) add(h, m); }
  }
  return hstr(h);
}
// config sets: [name, mut, phMut, novel, mMut, mNovel] (float-exact values except the default's .05)
const CFGS = {
  def: [0.025, 0.030, 0.050, 0.025, 0.050],
  zero: [0, 0, 0, 0, 0],
  max: [0.25, 0.25, 0.25, 0.25, 0.25],
  mix: [0.0625, 0.125, 0.125, 0.0, 0.0],
};
out.push(`# H <name> <seed> <N> <gens> <cfg 5 floats> <hash of the founders> <hash per generation 1..gens>`);
function run(name, seed, N, gens, cfgName) {
  const v = CFGS[cfgName];
  const cfg = Object.assign({}, FL.INH, { rule: "v3", mut: v[0], phMut: v[1], novel: v[2], mMut: v[3], mNovel: v[4], colourMean: "satw" });
  const rng = FL.MT(seed);
  let pop = [];
  for (let i = 0; i < N; i++) pop.push(FL.founder(rng));
  const hs = [hashPop(pop)];
  for (let g = 1; g <= gens; g++) {
    const next = [];
    for (let i = 0; i < N; i++) {
      const a = rng.uint(N - 1), b = rng.uint(N - 1);
      next.push(FL.inherit(pop[a], pop[b], cfg, rng));
    }
    pop = next;
    hs.push(hashPop(pop));
  }
  out.push(`H ${name} ${seed} ${N} ${gens} ${v.map(hex).join(" ")} ${hs.join(" ")}`);
}
run("def_20261028", 20261028, 4096, 40, "def");
run("def_29", 29, 4096, 40, "def");
run("def_7", 7, 4096, 8, "def");
run("zero_8", 8, 512, 24, "zero");
run("max_9", 9, 512, 24, "max");
run("mix_10", 10, 512, 24, "mix");
// E: the CPU law itself (src/ocean/appearance_eval.hpp flankRGB vs FL.flankAt): 64 founders of
// MT(777), lambda cycling over four values, 48 flank sites each from MT(778)
{
  out.push(`# E <founder index of MT(777)> <lambda> <u> <v> <r> <g> <b>  (FL.flankAt: unlit, intrinsic)`);
  const rng = FL.MT(777), pts = FL.MT(778), lams = [FL.LAMBDA, 3.2, 5.5, 7.0], c = [0, 0, 0];
  for (let i = 0; i < 64; i++) {
    const G = FL.founder(rng); G.lam = lams[i % 4];
    const D = FL.decode(G);
    for (let k = 0; k < 48; k++) {
      const u = -0.15 + 1.25 * pts.unit(), v = pts.unit();
      FL.flankAt(D, u, v, c);
      out.push(`E ${i} ${G.lam.toPrecision(17)} ${u.toPrecision(17)} ${v.toPrecision(17)} ${c.map(x => x.toPrecision(17)).join(" ")}`);
    }
  }
}
// identical parents: one founder mated with itself (the bit-identity rule) under zero mutation
{
  const rng = FL.MT(41), A = FL.founder(rng);
  const zero = Object.assign({}, FL.INH, { rule: "v3", mut: 0, phMut: 0, novel: 0, mMut: 0, mNovel: 0, colourMean: "satw" });
  const C = FL.inherit(A, A, zero, rng);
  let same = 0; for (let j = 0; j < FL.NF; j++) same += bitsOf(C.g[j]) === bitsOf(A.g[j]);
  out.push(`# identical parents, zero mutation: ${same} of ${FL.NF} floats equal to the parent's`);
}
process.stdout.write(out.join("\n") + "\n");
