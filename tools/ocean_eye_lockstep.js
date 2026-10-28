// ocean_eye_lockstep.js — the node (wasm) host for tools/ocean_eye_block.cpp,
// the same procedure as tools/ocean_eye_lockstep.cpp: each --blocks entry is
// an Emscripten MODULARIZE build of the block (one per Ocean build variant),
// every variant BLOCK:EYE is stepped --chunk steps in turn (order rotating),
// each rep restarts from the snapshot, round -1 is a discarded warm-up. The
// step time is measured inside the module (blk_run), so no JS call overhead.
//
//   node tools/ocean_eye_lockstep.js --blocks plain_blk.js,eye_blk.js --variants 1:0,1:1,1:0 \
//        --pop 3000 --reps 7 --warm 5 --steps 100 --chunk 1
'use strict';
const path = require('path');

function median(v) {
    const s = [...v].sort((a, b) => a - b), n = s.length;
    return n === 0 ? 0 : n % 2 ? s[(n - 1) / 2] : 0.5 * (s[n / 2 - 1] + s[n / 2]);
}

async function main() {
    const a = { blocks: '', variants: '0:0', label: '', seed: 42, pop: 300, w: 1920, h: 1080, preset: 0,
                evolve: 0, 'evolve-eye': 1, reps: 7, warm: 10, steps: 100, chunk: 1 };
    const argv = process.argv.slice(2);
    for (let i = 0; i < argv.length; i += 2) {
        const k = argv[i].replace(/^--/, '');
        if (!(k in a) || i + 1 >= argv.length) { console.error('unknown or incomplete argument: ' + argv[i]); process.exit(2); }
        a[k] = typeof a[k] === 'number' ? Number(argv[i + 1]) : argv[i + 1];
    }
    if (a.chunk < 1 || a.steps % a.chunk) { console.error('--steps must be a multiple of --chunk'); process.exit(2); }
    const blocks = [];
    for (const p of a.blocks.split(',')) blocks.push(await require(path.resolve(p))());
    const vars = [], base = blocks.map(() => -1);
    for (const v of a.variants.split(',')) {
        const [b, eye] = v.split(':').map(Number);
        const m = blocks[b];
        if (eye && !m._blk_has_eye()) { console.error(`block ${b} has no eye`); process.exit(2); }
        if (base[b] < 0) {   // every variant is a clone of its block's base world
            base[b] = m._blk_new(a.seed, a.pop, a.w, a.h, a.preset, a.evolve, a['evolve-eye']);
            if (base[b] < 0) process.exit(4);
        }
        vars.push({ b, eye, h: m._blk_clone(base[b]), m });
    }
    const V = vars.length;
    const ms = vars.map(() => []), popm = vars.map(() => []), prof = vars.map(() => new Array(9).fill(0));
    for (let r = -1; r < a.reps; ++r) {
        for (let j = 0; j < V; ++j) {
            const x = vars[(r + 1 + j) % V];
            x.m._blk_reset(x.h, x.eye);
            if (a.warm > 0) x.m._blk_run(x.h, a.warm);
            x.m._blk_clear_counts(x.h);
        }
        const t = new Array(V).fill(0);
        for (let k = 0; k < a.steps / a.chunk; ++k)
            for (let j = 0; j < V; ++j) {
                const vi = (k + j) % V, x = vars[vi];
                t[vi] += x.m._blk_run(x.h, a.chunk);
            }
        if (r < 0) continue;
        let line = `rep ${r}:`;
        for (let vi = 0; vi < V; ++vi) {
            const x = vars[vi];
            ms[vi].push(1e3 * t[vi] / a.steps);
            popm[vi].push(x.m._blk_fish_steps(x.h) / a.steps);
            for (let k = 0; k < 9; ++k) prof[vi][k] += x.m._blk_prof(x.h, k);
            line += ` v${vi} ${ms[vi][ms[vi].length - 1].toFixed(4)} ms`;
        }
        console.log(line);
    }
    console.log(`summary label=${a.label} pop0=${a.pop} world=${a.w}x${a.h} preset=${a.preset} evolve=${a.evolve}` +
                ` steps=${a.steps} chunk=${a.chunk} reps=${a.reps}`);
    for (let vi = 0; vi < V; ++vi) {
        const x = vars[vi], m = ms[vi], med = median(m), lo = Math.min(...m), hi = Math.max(...m), pm = median(popm[vi]);
        console.log(`  v${vi} block=${x.b}(${x.m._blk_has_eye() ? 'eye' : 'plain'}) eye=${x.eye} median_ms=${med.toFixed(4)}` +
                    ` min=${lo.toFixed(4)} max=${hi.toFixed(4)} spread=${(100 * (hi - lo) / med).toFixed(1)}%` +
                    ` pop=${pm.toFixed(0)} us_per_fish_step=${(1e3 * med / pm).toFixed(3)}`);
        if (x.m._blk_has_prof()) {
            const P = prof[vi], fs = P[8] > 0 ? P[8] : 1, pc = (n, d) => (100 * n / d).toFixed(1);
            console.log(`    prof: build ${pc(P[0], P[3])}% fish-phase ${pc(P[1], P[3])}% serial ${pc(P[2], P[3])}%` +
                        ` | fish phase: sensors ${pc(P[4], P[1])}% ff ${pc(P[5], P[1])}% move+metab ${pc(P[6], P[1])}%` +
                        ` | ns/fish-step: sensors ${(P[4] / fs).toFixed(0)} ff ${(P[5] / fs).toFixed(0)}` +
                        ` move ${(P[6] / fs).toFixed(0)} total ${(P[3] / fs).toFixed(0)}`);
        }
    }
    for (let vi = 1; vi < V; ++vi) {
        const rat = ms[vi].map((v, r) => v / ms[0][r]);
        console.log(`  ratio v${vi}/v0: median ${median(rat).toFixed(4)} [min ${Math.min(...rat).toFixed(4)} max ${Math.max(...rat).toFixed(4)}]`);
    }
}
main();
