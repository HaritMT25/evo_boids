// ocean_turbo_handoff.js — the 3-D Ocean page's Turbo hand-off, in node, against
// the real module (web/evo_ocean.{js,wasm}, two and more instances) and the
// real worker (web/ocean_worker.js, in a vm context with a worker's globals),
// through the helpers the page and the worker share (web/ocean_handoff.js).
// No npm, no browser:
//
//   node tests/ocean_turbo_handoff.js        (from anywhere; exit 0 = all pass)
//
// It checks:
//   * the hand-off with unequal memories, both ways, with plenty of free space
//     in the receiver (main's takeMemory, kept here as legacyTake, is shown
//     failing on the same images first);
//   * memory growth forced inside oc_summary / oc_families / oc_inspect /
//     oc_sensor_names, on both sides, while Turbo samples the dashboards;
//   * an exception in a chunk: with the world whole (it comes back), and out
//     of oc_step (the page keeps its own copy); then a second Turbo session,
//     in a fresh worker and in the failed one; a refused hand-back (the page
//     can't grow) resumes and hands back on the next Stop;
//   * that after a round trip with live edits (on the page before, in the
//     worker, on the page after) N further steps are bit-identical to an
//     uninterrupted run with the same edits at the same frames (a fingerprint
//     of every oc_* export: positions, depths, bodies, colours, ids, food,
//     predators, stats, the summary, every family line, the appearance
//     genome's rows (its owner-bound side array lives in module memory and
//     must come across whole), and oc_inspect of every fish: its numbers,
//     hidden neurons and wiring);
//   * Randomize (web/ocean_config.js draw) against oc_init at small and
//     default world sizes, and the refusals of imported values the sheet
//     would otherwise round or drop.
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm'), crypto = require('crypto');

const ROOT = path.resolve(__dirname, '..'), WEB = path.join(ROOT, 'web');
const WASM = path.join(WEB, 'evo_ocean.wasm');
const create = require(path.join(WEB, 'evo_ocean.js'));
const HO = require(path.join(WEB, 'ocean_handoff.js'));
require(path.join(WEB, 'ocean_config.js'));
const CFG = globalThis.EvoOceanConfig;
const MB = 1 << 20;

let failures = 0, checks = 0;
function ok(cond, what) {
    checks++;
    if (cond) console.log('  ok   ' + what);
    else { failures++; console.log('  FAIL ' + what); }
}
function section(t) { console.log('\n' + t); }

async function mk(seed, cfg, w, h, pop) {
    const M = await create({ locateFile: p => p.endsWith('.wasm') ? WASM : p });
    const json = cfg && Object.keys(cfg).length ? JSON.stringify(cfg) : '';
    const r = HO.withCString(M, json, p => M._oc_init(seed, w || 1920, h || 1080, 240, pop === undefined ? 300 : pop, p));
    if (!r) throw new Error('oc_init: ' + HO.cString(M, M._oc_last_error()));
    return M;
}
function frame(M) { M._oc_sync(); return HO.f64(M, M._oc_stats(), M._oc_stats_len())[1]; }
function setParam(M, key, v) {
    const r = HO.withCString(M, key, p => M._oc_set_param(p, v));
    if (!r) throw new Error('oc_set_param ' + key + ': ' + HO.cString(M, M._oc_last_error()));
}
/* grow M's memory through its own allocator (a block as big as the memory:
 * it doubles), as an export that allocates does when the heap is full */
function growInside(M) { const p = M._malloc(HO.buffer(M).byteLength); if (!p) throw new Error('malloc'); M._free(p); }
/* grow it by a MiB: the buffer is replaced (every view cut before is
 * detached), as with any growth, without doubling the memory each time */
function growMiB(M) { M.wasmMemory.grow(16); }
function memMB(M) { return (HO.buffer(M).byteLength / MB).toFixed(2); }

/* every oc_* export of the world now, hashed */
function fingerprint(M) {
    M._oc_sync();
    const h = crypto.createHash('sha256');
    const put = (Ctor, ptr, len) => { if (len > 0) h.update(Buffer.from(new Ctor(HO.buffer(M), ptr, len).slice().buffer)); };
    const n = M._oc_creature_count(), fc = M._oc_food_count(), pc = M._oc_predator_count();
    put(Float64Array, M._oc_stats(), M._oc_stats_len());
    put(Float32Array, M._oc_creature_segments(), n * 12);
    put(Float32Array, M._oc_creature_depth(), n);
    put(Float32Array, M._oc_creature_vz(), n);
    put(Float32Array, M._oc_body_params(), n * 2);
    put(Uint8Array, M._oc_creature_colors(), n * 3);
    put(Float64Array, M._oc_creature_ids(), n);
    put(Float32Array, M._oc_food_xy(), fc * 2);
    put(Float32Array, M._oc_food_depth(), fc);
    put(Float32Array, M._oc_predator_data(), pc * 3);
    put(Float32Array, M._oc_predator_depth(), pc);
    if (M._oc_creature_appearance) {   /* the appearance genome: its owner-bound side array, in module memory */
        h.update(Buffer.from(new Int32Array([M._oc_appearance_count(), M._oc_appearance_flags(), M._oc_appearance_broken()]).buffer));
        put(Float32Array, M._oc_creature_appearance(), M._oc_appearance_count() * M._oc_appearance_stride());
    }
    h.update(Buffer.from(new Float64Array(HO.f64(M, M._oc_summary(), M._oc_summary_len())).buffer));
    const k = M._oc_families(1 << 20);
    h.update(Buffer.from(new Float64Array(HO.f64(M, M._oc_families_data(), 2 * k)).buffer));
    const nH = M._oc_num_hidden(), nS = M._oc_num_senses(), nA = M._oc_num_actions();
    for (let i = 0; i < n; i++) {
        if (!M._oc_inspect(i)) throw new Error('oc_inspect ' + i);
        put(Float64Array, M._oc_inspect_info(), M._oc_inspect_len());
        put(Float32Array, M._oc_inspect_sensors(), nS);
        put(Float32Array, M._oc_inspect_hidden(), nH);
        put(Float32Array, M._oc_inspect_actions(), nA);
        put(Float32Array, M._oc_inspect_conns(), M._oc_inspect_conn_count() * 5);
    }
    return h.digest('hex');
}
/* the appearance genome rows of this sync, with their count and state */
function appRows(M) {
    M._oc_sync();
    const n = M._oc_appearance_count();
    return { n, flags: M._oc_appearance_flags(), broken: M._oc_appearance_broken(),
             rows: n ? Array.from(new Float32Array(HO.buffer(M), M._oc_creature_appearance(), n * M._oc_appearance_stride())) : [] };
}
/* compare a and b over `steps` further steps, one at a time */
function sameFor(a, b, steps) {
    let fa = fingerprint(a), fb = fingerprint(b);
    if (fa !== fb) return 'differ at frame ' + frame(a) + ' / ' + frame(b);
    for (let s = 0; s < steps; s++) {
        a._oc_step(1); b._oc_step(1);
        fa = fingerprint(a); fb = fingerprint(b);
        if (fa !== fb) return 'differ at frame ' + frame(a) + ' (step ' + (s + 1) + ' after the hand-off)';
    }
    return '';
}

/* main's takeMemory (web/ocean.js / web/ocean_worker.js at 9a351d8), for the record */
function legacyTake(M, src) {
    const heap = () => HO.buffer(M);
    const need = src.byteLength, have = heap().byteLength;
    if (have < need) {
        const p = M._malloc(need - have + 65536);
        if (p) M._free(p);
        if (heap().byteLength < need) throw new Error('could not grow the memory to ' + need + ' bytes');
    }
    const dst = new Uint8Array(heap());
    dst.set(new Uint8Array(src));
    if (dst.length > need) dst.fill(0, need);
}
function throws(fn) { try { fn(); return ''; } catch (e) { return String(e && e.message || e); } }

/* web/ocean_worker.js in a vm context: a dedicated worker's globals, its
 * MessageChannel a macrotask per message (as in a browser), importScripts
 * loading ocean_handoff.js into the context and handing it the module factory. */
function makeWorker() {
    const inbox = [], waiters = [];
    function Chan() {
        const p1 = { onmessage: null };
        this.port1 = p1;
        this.port2 = { postMessage: d => setImmediate(() => { if (p1.onmessage) p1.onmessage({ data: d }); }) };
    }
    const ctx = {
        console, performance, setTimeout, clearTimeout, TextEncoder, TextDecoder, WebAssembly,
        MessageChannel: Chan, onmessage: null,
        postMessage: (m) => { inbox.push(m); pump(); }
    };
    ctx.self = ctx;
    ctx.importScripts = function () {
        for (const u of arguments) {
            if (/ocean_handoff\.js$/.test(u)) vm.runInContext(fs.readFileSync(u, 'utf8'), ctx, { filename: u });
            else if (/evo_ocean\.js$/.test(u)) ctx.createEvoOcean = create;
            else throw new Error('importScripts: ' + u);
        }
    };
    vm.createContext(ctx);
    const src = path.join(WEB, 'ocean_worker.js');
    vm.runInContext(fs.readFileSync(src, 'utf8'), ctx, { filename: src });
    function pump() {
        for (let w = 0; w < waiters.length; w++) {
            const i = inbox.findIndex(waiters[w].pred);
            if (i < 0) continue;
            const m = inbox[i];
            /* drop the stats and samples before it: a later wait wants later ones */
            for (let j = i; j >= 0; j--) if (j === i || inbox[j].type === 'stats') inbox.splice(j, 1);
            const wt = waiters.splice(w, 1)[0];
            clearTimeout(wt.timer);
            wt.resolve(m);
            w--;
        }
    }
    return {
        ctx,
        get M() { return ctx.M; },
        send(m) { setImmediate(() => ctx.onmessage({ data: m })); },
        wait(pred, what, ms) {
            return new Promise((resolve, reject) => {
                const wt = { pred, resolve };
                wt.timer = setTimeout(() => {
                    waiters.splice(waiters.indexOf(wt), 1);
                    reject(new Error('timed out waiting for ' + what + '; inbox: ' +
                                     inbox.map(m => m.type + (m.message ? '(' + m.message + ')' : '')).join(',')));
                }, ms || 20000);
                waiters.push(wt);
                pump();
            });
        },
        rows() { const r = []; for (let i = inbox.length - 1; i >= 0; i--) if (inbox[i].type === 'rec') r.unshift(...inbox.splice(i, 1)[0].rows); return r; },
        errors() { return inbox.filter(m => m.type === 'error'); }
    };
}
const startMsg = (mem) => ({ cmd: 'start', js: path.join(WEB, 'evo_ocean.js'), wasm: WASM,
                             handoff: path.join(WEB, 'ocean_handoff.js'), mem, recEvery: 60, famK: 8 });
const isType = (t) => (m) => m.type === t;
const statsAt = (f) => (m) => m.type === 'stats' && m.frame >= f;

/* --------------------------------------------------------------------- */
async function roundTrip(label, cfg) {
    section('Round trip, live edits, unequal memories both ways: ' + label);
    const SEED = 20261029;
    const edits = [];   /* [frame, key, value]: replayed on the reference */
    const page = await mk(SEED, cfg);
    /* the page grew once (a big allocation, freed): a big memory, mostly free */
    const p = page._malloc(40 * MB); page._free(p);
    page._oc_step(150);
    setParam(page, 'foodEnergy', 7); edits.push([frame(page), 'foodEnergy', 7]);
    page._oc_step(90);
    const entryFrame = frame(page), entryPrint = fingerprint(page);
    const imgA = HO.buffer(page).slice(0);
    console.log('  page ' + memMB(page) + ' MiB at frame ' + entryFrame + ' -> a fresh worker');
    {   /* main's code on the same image, into a fresh module like the worker's */
        const fresh = await mk(1);
        const err = throws(() => legacyTake(fresh, imgA));
        ok(err !== '', 'main\'s takeMemory fails page -> worker (' + (err || 'it did not') + ')');
    }
    const w = makeWorker();
    w.send(startMsg(imgA));
    const first = await w.wait(isType('stats'), 'the first stats');
    ok(first.frame === entryFrame, 'the worker took the page\'s world (frame ' + first.frame + ')');
    ok(+memMB(w.M) >= +(imgA.byteLength / MB).toFixed(2), 'the worker grew to the image exactly (' + memMB(w.M) + ' MiB)');
    /* the module's own views are now stale (a grow from outside), and nothing reads them */
    ok(w.M.HEAPU8.buffer !== w.M.wasmMemory.buffer, 'the module\'s cached HEAPU8 is stale after the grow (why ocean_handoff.js does its own UTF-8)');

    /* growth forced inside every allocating export the worker calls while sampling */
    const W = w.M, realSu = W._oc_summary, realFam = W._oc_families;
    let grown = 0;
    W._oc_summary = function () { if (grown < 2) { growMiB(W); grown++; } return realSu(); };
    W._oc_families = function (k) { if (grown >= 2 && grown < 4) { growMiB(W); grown++; } return realFam(k); };

    w.send({ cmd: 'set', key: 'kBuoy', value: 0.5 });
    const s1 = await w.wait(isType('set'), 'the kBuoy reply');
    ok(s1.ok && s1.frame >= entryFrame, 'kBuoy 0.5 applied in the worker at frame ' + s1.frame);
    edits.push([s1.frame, 'kBuoy', 0.5]);
    w.send({ cmd: 'set', key: 'kBuoy', value: -0.5 });
    const s2 = await w.wait(isType('set'), 'the refused reply');
    ok(!s2.ok && /kBuoy/.test(s2.error), 'a refused live value is answered with the engine\'s reason: "' + s2.error + '"');
    await w.wait(statsAt(s1.frame + 600), 'frame ' + (s1.frame + 600));
    w.send({ cmd: 'set', key: 'maxSpeed', value: 140 });
    const s3 = await w.wait(isType('set'), 'the maxSpeed reply');
    ok(s3.ok, 'maxSpeed 140 applied in the worker at frame ' + s3.frame);
    edits.push([s3.frame, 'maxSpeed', 140]);
    if (cfg.patternMutation === undefined && W._oc_creature_appearance && label !== 'the default world') {
        w.send({ cmd: 'set', key: 'patternMutation', value: 0.1 });   /* an appearance knob, live in Turbo */
        const s4 = await w.wait(isType('set'), 'the patternMutation reply');
        ok(s4.ok, 'patternMutation 0.1 (the appearance genome) applied in the worker at frame ' + s4.frame);
        edits.push([s4.frame, 'patternMutation', 0.1]);
    }
    await w.wait(statsAt(s3.frame + 600), 'frame ' + (s3.frame + 600));
    ok(grown === 4, 'the worker\'s memory grew inside oc_summary and oc_families while sampling (' + grown + ' times, now ' + memMB(W) + ' MiB)');
    const rows = w.rows();
    ok(rows.length > 5 && rows.every(r => r.st.length === 15 && r.su.length === 22 && r.fam.length > 0 && r.st[1] % 60 === 0 &&
                                         r.su.every(Number.isFinite)),
       rows.length + ' dashboard samples arrived whole (stats, summary, family lines; frames on the 60-step grid)');
    W._oc_summary = realSu; W._oc_families = realFam;
    /* the worker now has the bigger memory; the page has lots of free space but less room */
    w.send({ cmd: 'stop' });
    const back = await w.wait(isType('memory'), 'the memory');
    console.log('  worker ' + (back.mem.byteLength / MB).toFixed(2) + ' MiB -> the page (' + memMB(page) + ' MiB, mostly free)');
    ok(back.mem.byteLength > HO.buffer(page).byteLength, 'the image coming back is bigger than the page\'s memory');
    {   /* main's code on the same image, into a module grown and freed as the page's was */
        const twin = await mk(SEED, cfg);
        const q = twin._malloc(40 * MB); twin._free(q);
        const err = throws(() => legacyTake(twin, back.mem));
        ok(err !== '', 'main\'s takeMemory fails worker -> page (' + (err || 'it did not') + ')');
    }
    HO.take(page, back.mem, "the page's");
    const backFrame = frame(page);
    ok(backFrame > s3.frame, 'the page took the world back at frame ' + backFrame + ' (' + back.totalSteps + ' Turbo steps)');
    if (page._oc_creature_appearance) {
        const a = appRows(page), b = appRows(W);   /* the worker, stopped: its world is the image */
        ok(a.n > 0 && a.n === page._oc_creature_count() && !a.broken && a.flags === b.flags && a.n === b.n &&
           a.rows.length === b.rows.length && a.rows.every((v, i) => Object.is(v, b.rows[i])),
           'the appearance genome came across whole: ' + a.n + ' fish x ' + page._oc_appearance_stride() + ' floats, flags ' + a.flags +
           ', bit-identical to the worker\'s, its side array still bound (broken 0)');
    }
    page._oc_step(45);
    setParam(page, 'metabolicRate', 1.6); edits.push([frame(page), 'metabolicRate', 1.6]);
    page._oc_step(15);

    /* the reference: the same world, uninterrupted, the same edits at the same frames */
    const ref = await mk(SEED, cfg);
    for (const [f, key, v] of edits) { ref._oc_step(f - frame(ref)); setParam(ref, key, v); }
    ref._oc_step(frame(page) - frame(ref));
    const diff = sameFor(page, ref, 300);
    ok(diff === '', 'after the round trip, 300 further steps are bit-identical to the uninterrupted run' + (diff ? ': ' + diff : ''));
    ok(w.errors().length === 0, 'no worker errors');
    return { page, entryFrame, entryPrint };
}

async function failures_() {
    section('An exception in a chunk -> recovery -> a second Turbo session');
    const SEED = 777;
    const page = await mk(SEED);
    page._oc_step(120);

    /* (a) a throw outside oc_step: the world is whole and comes back */
    let w = makeWorker();
    w.send(startMsg(HO.buffer(page).slice(0)));
    await w.wait(isType('stats'), 'the first stats');
    const realFam = w.M._oc_families;
    let n = 0;
    w.M._oc_families = function (k) { if (++n === 3) throw new TypeError('injected: a sampling bug'); return realFam(k); };
    const e1 = await w.wait(isType('error'), 'the error');
    ok(!!e1.mem && /injected/.test(e1.message), 'a throw while sampling stops the run with the world attached ("' + e1.message + '")');
    ok(vm.runInContext('running', w.ctx) === false, 'the worker is no longer "running" (main left it true and ignored the next start)');
    HO.take(page, e1.mem, "the page's");
    const f1 = frame(page);
    const ref = await mk(SEED);
    ref._oc_step(f1);
    ok(sameFor(page, ref, 100) === '', 'the world it handed back (frame ' + f1 + ') is the uninterrupted run\'s, bit for bit, 100 steps on');
    /* the same worker again: a start always begins afresh */
    w.M._oc_families = realFam;
    w.send(startMsg(HO.buffer(page).slice(0)));
    const again = await w.wait(isType('stats'), 'stats from the failed worker');
    ok(again.frame === frame(page), 'the failed worker takes a new start (no wedge)');
    w.send({ cmd: 'stop' });
    const m2 = await w.wait(isType('memory'), 'its memory');
    HO.take(page, m2.mem, "the page's");
    ref._oc_step(frame(page) - frame(ref));
    ok(sameFor(page, ref, 50) === '', 'and its round trip is bit-identical (frame ' + frame(page) + ')');

    /* (b) a throw out of oc_step: nothing of the worker's world is trusted */
    w = makeWorker();
    const before = frame(page), beforePrint = fingerprint(page);
    w.send(startMsg(HO.buffer(page).slice(0)));
    await w.wait(isType('stats'), 'the first stats');
    const realStep = w.M._oc_step;
    let k = 0;
    w.M._oc_step = function (s) { if (++k === 4) throw new WebAssembly.RuntimeError('injected: a trap mid-step'); return realStep(s); };
    const e2 = await w.wait(isType('error'), 'the error');
    ok(!e2.mem, 'a throw out of oc_step stops the run without a world ("' + e2.message + '")');
    ok(frame(page) === before && fingerprint(page) === beforePrint, 'the page still has its own copy, as Turbo took it (frame ' + before + ')');

    /* (c) a fresh worker: the second session works and round-trips exactly */
    w = makeWorker();
    w.send(startMsg(HO.buffer(page).slice(0)));
    const st = await w.wait(isType('stats'), 'stats');
    await w.wait(statsAt(st.frame + 400), 'progress');
    w.send({ cmd: 'stop' });
    const m3 = await w.wait(isType('memory'), 'the memory');
    HO.take(page, m3.mem, "the page's");
    ref._oc_step(frame(page) - frame(ref));
    ok(sameFor(page, ref, 100) === '', 'a second Turbo session in a fresh worker round-trips bit-identically (frame ' + frame(page) + ')');

    /* (d) the page can't take the world back: the worker keeps it and resumes */
    const big = makeWorker();
    big.send(startMsg(HO.buffer(page).slice(0)));
    const bs = await big.wait(isType('stats'), 'stats');
    big.M.wasmMemory.grow(160);   /* the worker's memory outgrows the page's (+10 MiB) */
    await big.wait(statsAt(bs.frame + 200), 'progress');
    big.send({ cmd: 'stop' });
    const m4 = await big.wait(isType('memory'), 'the memory');
    const realGrow = page.wasmMemory.grow;
    page.wasmMemory.grow = function () { throw new RangeError('injected: out of address space'); };
    const frameBefore = frame(page), printBefore = fingerprint(page);
    const err = throws(() => HO.take(page, m4.mem, "the page's"));
    ok(/could not grow/.test(err) && frame(page) === frameBefore && fingerprint(page) === printBefore,
       'a hand-back the page can\'t grow for throws and leaves the page\'s world untouched (' + err + ')');
    delete page.wasmMemory.grow;
    ok(page.wasmMemory.grow === realGrow, '(the page\'s grow restored)');
    big.send({ cmd: 'resume' });
    const rs = await big.wait(isType('stats'), 'stats after resume');
    const resumedFrom = rs.frame;
    await big.wait(statsAt(resumedFrom + 300), 'progress after resume');
    ok(true, 'the worker resumed its world (frame ' + resumedFrom + ' on)');
    big.send({ cmd: 'stop' });
    const m5 = await big.wait(isType('memory'), 'the memory, again');
    HO.take(page, m5.mem, "the page's");
    ref._oc_step(frame(page) - frame(ref));
    ok(sameFor(page, ref, 100) === '', 'the next Stop hands it back, bit-identical to the uninterrupted run (frame ' + frame(page) + ')');

    /* (e) a set with no world, a stop with no run: answered, never dropped */
    const idle = makeWorker();
    idle.send({ cmd: 'set', key: 'foodEnergy', value: 6 });
    const r = await idle.wait(isType('set'), 'a reply');
    ok(r.ok === false && /not running/.test(r.error), 'a live setting with no run is answered ok=false ("' + r.error + '")');
    idle.send({ cmd: 'stop' });
    const r2 = await idle.wait(isType('error'), 'a reply');
    ok(/not running/.test(r2.message), 'a stop with no run is answered ("' + r2.message + '")');
}

async function growthPage() {
    section('Memory growth inside the exports the page calls (summary, families, inspection, names)');
    const M = await mk(5), twin = await mk(5);
    M._oc_step(200); twin._oc_step(200);
    M._oc_sync(); twin._oc_sync();
    const len = M._oc_summary_len();
    const real = { su: M._oc_summary, fam: M._oc_families, insp: M._oc_inspect, names: M._oc_sensor_names };
    let viaMalloc = true;   /* the first growth through the allocator, the rest by a MiB */
    const grow = () => { if (viaMalloc) { viaMalloc = false; growInside(M); } else growMiB(M); };
    M._oc_summary = function () { grow(); return real.su(); };
    ok(/detached/.test(throws(() => new Float64Array(HO.buffer(M), M._oc_summary(), len))),
       'main\'s summary expression (the buffer taken before the call) fails when the call grows the memory');
    M._oc_families = function (k) { grow(); return real.fam(k); };
    const a = HO.row(M, 8, M._oc_stats_len()), b = HO.row(twin, 8, twin._oc_stats_len());
    ok(JSON.stringify(a) === JSON.stringify(b) && a.st.length === 15,
       'a dashboard row (ocean_handoff.js row) is whole and right with the memory growing in oc_summary and oc_families');
    M._oc_inspect = function (i) { grow(); return real.insp(i); };
    M._oc_sensor_names = function () { grow(); return real.names(); };
    /* the page's order (web/ocean.js inspect): the names first, then oc_inspect, then the views */
    const names = HO.cString(M, M._oc_sensor_names());
    const okI = M._oc_inspect(3);
    const info = HO.f64(M, M._oc_inspect_info(), M._oc_inspect_len());
    twin._oc_inspect(3);
    const info2 = HO.f64(twin, twin._oc_inspect_info(), twin._oc_inspect_len());
    ok(okI && names.split(',').length === M._oc_num_senses() && JSON.stringify(info) === JSON.stringify(info2),
       'the fish card\'s numbers and the sense names read right with the memory growing in oc_inspect and oc_sensor_names');
    Object.assign(M, { _oc_summary: real.su, _oc_families: real.fam, _oc_inspect: real.insp, _oc_sensor_names: real.names });
    ok(sameFor(M, twin, 50) === '', 'the growth changed nothing in the world (50 steps, bit-identical to the twin)');
}

async function randomize() {
    section('Randomize (web/ocean_config.js draw) against oc_init');
    let s = 12345;
    const rand = () => { s = (s + 0x6D2B79F5) | 0; let t = Math.imul(s ^ (s >>> 15), 1 | s); t ^= t + Math.imul(t ^ (t >>> 7), 61 | t); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
    const M = await mk(1);
    const e1 = JSON.parse(HO.cString(M, M._oc_e1_preset()));
    const tryInit = (pick, w, h) => HO.withCString(M, JSON.stringify(pick.set), p => M._oc_init(7, w, h, 240, pick.population, p));
    let legacyBad = 0;
    for (let i = 0; i < 100; i++) if (!tryInit(CFG.draw(rand, 300, e1), 300, 300)) legacyBad++;
    ok(legacyBad > 0, 'without the world\'s size, draws for a 300 x 300 world are refused (' + legacyBad + ' of 100)');
    for (const [w, h] of [[100, 100], [130, 400], [300, 300], [301, 301], [1920, 1080], [1080, 1920], [5000, 2000]]) {
        let bad = 0, torus = 0, why = '';
        for (let i = 0; i < 150; i++) {
            const pick = CFG.draw(rand, 300, e1, [w, h]);
            if (pick.set.toroidal) torus++;
            if (!tryInit(pick, w, h)) { bad++; why = HO.cString(M, M._oc_last_error()); }
        }
        ok(bad === 0, w + ' x ' + h + ': 150 draws, all accepted (' + torus + ' toroidal)' + (why ? ' — ' + why : ''));
    }
}

async function imports() {
    section('Imported values the sheet would round or drop are refused by oc_init (web/ocean.js importWorld)');
    const M = await mk(1);
    for (const cfg of [{ maxPopulation: 0.5 }, { eye: 2 }, { deepCurrent: 2 }, { foodDepthMode: 2 }, { eyeCode: 32 },
                       { steerFrame: 2 }, { currentMode: 2 }, { minPopulation: -0.5 }]) {
        const r = HO.withCString(M, JSON.stringify(cfg), p => M._oc_init(1, 1920, 1080, 240, 300, p));
        ok(!r && HO.cString(M, M._oc_last_error()) !== '', JSON.stringify(cfg) + ' refused: ' + HO.cString(M, M._oc_last_error()));
    }
    /* Classic's switches used to be read only as JSON booleans, anything else
     * passed over ({"toroidal":1} ran with walls while the sheet showed it on).
     * The engine now refuses a switch that is not true / false (ocean_config.hpp
     * readOceanJson), and the page refuses it before that (importWorld). */
    const one = HO.withCString(M, '{"toroidal":1}', p => M._oc_init(1, 1920, 1080, 240, 50, p));
    const why = HO.cString(M, M._oc_last_error());
    const yes = HO.withCString(M, '{"toroidal":true}', p => M._oc_init(1, 1920, 1080, 240, 50, p));
    ok(one === 0 && /toroidal/.test(why) && yes === 1,
       'the engine refuses {"toroidal":1} ("' + why + '") and takes {"toroidal":true}');
    ok(CFG.KEYS.eyeCode[1].max === 31, 'the eye-code field runs 0..31 (the engine\'s eyeCodeValid; cloak bits 8 and 16)');
    const neg = HO.withCString(M, JSON.stringify({ kBuoy: -0.5 }), p => M._oc_init(1, 1920, 1080, 240, 300, p));
    ok(neg === 1 && CFG.NEG_ON_RESTART.kBuoy, 'kBuoy -0.5 starts a new world (so the sheet keeps it for a Restart)');
    const live = HO.withCString(M, 'kBuoy', p => M._oc_set_param(p, -0.5));
    ok(live === 0, 'and a running world refuses it live: "' + HO.cString(M, M._oc_last_error()) + '"');
    for (const k of Object.keys(CFG.NEG_ON_RESTART)) {
        const r = HO.withCString(M, JSON.stringify({ [k]: -1 }), p => M._oc_init(1, 1920, 1080, 240, 300, p));
        ok(r === 1 && CFG.LIVE[k], k + ' -1: a new world takes it (NEG_ON_RESTART, a live key)');
    }
}

(async () => {
    const t0 = Date.now();
    await growthPage();
    await randomize();
    await imports();
    await roundTrip('the default world', {});
    const probe = await mk(1);
    const e1 = JSON.parse(HO.cString(probe, probe._oc_e1_preset()));
    await roundTrip('E1 physics, 3 predators with their economy, patchy food',
                    Object.assign({}, e1, { predatorCount: 3, predatorEconomy: true, predatorMeal: 7, predatorMaxPopulation: 12,
                                            foodPatches: 5, foodDepthMode: 1, bodyModel: true }));
    await failures_();
    console.log('\n' + (checks - failures) + ' / ' + checks + ' checks passed in ' + ((Date.now() - t0) / 1000).toFixed(1) + ' s');
    process.exit(failures ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(2); });
