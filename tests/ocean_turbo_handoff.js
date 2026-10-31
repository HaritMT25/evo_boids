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
//     would otherwise round or drop;
//   * the page's live edits around Turbo (web/ocean_handoff.js ledger, as
//     web/ocean.js uses it): dependent edits acknowledged in Turbo and then
//     a worker failure that loses the world (minSpeed 0, maxSpeed 300,
//     minSpeed 200 come back in that order, bit-identical to applying them
//     directly; main's key-order recovery ran 30-300), and an unanswered
//     edit with a newer one held after Stop (the newer wins: field, runCfg
//     and world agree; main's ran the older one);
//   * a repeated key in ?cfg= or a setup file (parseFirst / cfgJson): the
//     page and oc_init accept or refuse the same documents and run the same
//     world (JSON.parse's page did neither).
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
/* M's world after `steps` more steps (60), fingerprinted: what a setting changes that the first
 * frame does not show (a radius, a gate, a switch that only steers) shows by then */
function stepped(M, steps) { M._oc_step(steps === undefined ? 60 : steps); return fingerprint(M); }
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

/* main's Turbo recovery (web/ocean.js turboFailed / turboBack / flushHeld at
 * fb976b2), for the record: the live keys where runCfg differs from the
 * entry config, in object-key order, skipping held keys; then every
 * unanswered edit copied over the held ones; then all of it in key order */
function legacyRecovery(runCfg, entryCfg, held, inflight) {
    const heldLive = Object.assign({}, held);
    if (runCfg) Object.keys(Object.assign({}, runCfg, entryCfg)).forEach(k => {
        const a = Object.prototype.hasOwnProperty.call(runCfg, k) ? runCfg[k] : CFG.KEYS[k] && CFG.KEYS[k][0];
        const b = Object.prototype.hasOwnProperty.call(entryCfg, k) ? entryCfg[k] : CFG.KEYS[k] && CFG.KEYS[k][0];
        if (CFG.LIVE[k] && a !== b && !(k in heldLive)) heldLive[k] = a;
    });
    inflight.forEach(([k, v]) => { heldLive[k] = v; });
    return Object.keys(heldLive).map(k => [k, heldLive[k]]);
}
/* the page's liveApplied: runCfg keeps what differs from the default */
function liveApplied(runCfg, key, v) { if (v === CFG.KEYS[key][0]) delete runCfg[key]; else runCfg[key] = v; }
/* the page's replayEdits (web/ocean.js) of a ledger plan on its own world: each
 * edit with oc_set_param, in order; runCfg follows what is accepted */
function replayOnPage(M, plan, runCfg) {
    return plan.map(e => {
        const r = HO.withCString(M, e.key, p => M._oc_set_param(p, e.num));
        if (r) liveApplied(runCfg, e.key, e.num);
        return r ? '' : e.key + ' ' + e.num + ': ' + HO.cString(M, M._oc_last_error());
    });
}
/* a worker whose oc_step traps on its `at`th chunk (its world is lost) */
function trapAt(w, at) {
    const real = w.M._oc_step;
    let k = 0;
    w.M._oc_step = function (s) { if (++k === at) throw new WebAssembly.RuntimeError('injected: a trap mid-step'); return real(s); };
}

/* web/ocean_worker.js in a vm context: a dedicated worker's globals, its
 * MessageChannel a macrotask per message (as in a browser), importScripts
 * loading ocean_handoff.js into the context and handing it the module factory.
 * opts.out: where its messages go instead of the inbox (the page's Worker
 * below); kill(): terminated, it neither runs nor posts again. */
function makeWorker(opts) {
    const inbox = [], waiters = [], out = opts && opts.out;
    let dead = false;
    function Chan() {
        const p1 = { onmessage: null };
        this.port1 = p1;
        this.port2 = { postMessage: d => setImmediate(() => { if (!dead && p1.onmessage) p1.onmessage({ data: d }); }) };
    }
    const ctx = {
        console, performance, setTimeout, clearTimeout, TextEncoder, TextDecoder, WebAssembly,
        MessageChannel: Chan, onmessage: null,
        postMessage: (m) => { if (dead) return; if (out) { out(m); return; } inbox.push(m); pump(); }
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
        send(m) { setImmediate(() => { if (!dead) ctx.onmessage({ data: m }); }); },
        kill() { dead = true; },
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

/* ---------------------------------------------------------------------
 * The page itself. web/ocean_handoff.js, web/ocean_config.js,
 * web/ocean_dash.js and web/ocean.js, as they are and in ocean.html's order,
 * in one vm context over a small DOM: the elements ocean.html declares (made
 * on first use) and the ones the page makes, events that bubble, listeners
 * whose exceptions are reported, not thrown (as a browser's). The real
 * module (a fresh instance per page, its oc_init / oc_set_param calls
 * logged) and the real worker (makeWorker) behind the page's Worker, whose
 * messages to the page can be held and let through in their order (a slow
 * channel, as a busy page or worker makes it). Stubs only where the page
 * draws (the 3-D scene, the charts) and schedules (requestAnimationFrame:
 * the test runs a frame when it wants one). The page's lexical state is
 * read through an accessor appended inside its closure (__oceanPage), with
 * its start-up's URL-import block, a verbatim slice, as a function of the
 * query (urlImport); each marker must occur exactly once.
 * --------------------------------------------------------------------- */
const PAGE_IDS = (() => {
    const m = new Map(), html = fs.readFileSync(path.join(WEB, 'ocean.html'), 'utf8');
    for (const x of html.matchAll(/<([a-zA-Z][\w-]*)\b[^>]*?\sid="([^"]+)"/g)) if (!m.has(x[2])) m.set(x[2], x[1]);
    return m;
})();
function onceIn(src, marker, file) {
    const i = src.indexOf(marker);
    if (i < 0 || src.indexOf(marker, i + 1) >= 0) throw new Error(file + ': ' + JSON.stringify(marker) + ' is not there exactly once');
    return i;
}
const PAGE_SRC = (() => {
    const src = fs.readFileSync(path.join(WEB, 'ocean.js'), 'utf8');
    const a = onceIn(src, '    var rawCfg = P.get("cfg") || ""', 'web/ocean.js');
    const b = onceIn(src, '    scene = EvoOceanScene.create(canvas, {', 'web/ocean.js');
    const end = onceIn(src, '\n})();\n', 'web/ocean.js');
    if (!(a < b && b < end)) throw new Error('web/ocean.js: the markers are out of order');
    const access = '\n  globalThis.__oceanPage = {\n' +
        '    get M() { return M; }, get turboState() { return turboState; }, get turboWorker() { return turboWorker; },\n' +
        '    get runCfg() { return runCfg; }, get initArgs() { return initArgs; }, get live() { return live; },\n' +
        '    get W() { return W; }, get H() { return H; }, get D() { return D; }, get WALLS() { return WALLS; },\n' +
        '    getVal: getVal, importWorld: importWorld, wiringNow: wiringNow, frame: frame,\n' +
        '    urlImport: function (P) {\n' + src.slice(a, b) + '    }\n  };';
    return src.slice(0, end) + access + src.slice(end);
})();
const SCENE_STUB = `window.EvoOceanScene = { load: function () { return Promise.resolve(); }, create: function () {
  var fixed = { following: false, flashlight: false, quality: 3, flashlightLevel: 0, pick: -1, project: null, view: null, degrade: false };
  return new Proxy({}, { get: function (t, k) { return function () { return fixed[k]; }; } });
} };`;
const CHARTS_STUB = `(function () {
  var charts = [];
  function Chart(box, kind) { this.box = box; this.kind = kind; this.data = undefined; charts.push(this); }
  Chart.prototype.draw = function (d) { this.data = d; };
  window.EvoCharts = { T: { series: ["#1", "#2", "#3", "#4", "#5", "#6", "#7", "#8"], other: "#9" }, Chart: Chart,
    lines: "lines", stack: "stack", phase: "phase", heat: "heat", legend: function () {}, tip: function () {},
    seq: function () { return "#0"; }, esc: function (s) { return String(s); }, tipRow: function () { return ""; },
    fmtTime: function (s) { return s + " s"; }, charts: charts };
})();`;

function makeDom(page) {
    const byId = new Map();
    const parse = (s) => {
        const m = /^([a-zA-Z][\w-]*)?((?:[#.][\w-]+)*)$/.exec(s);
        if (!m) throw new Error('the test DOM has no selector ' + JSON.stringify(s));
        const parts = m[2].match(/[#.][\w-]+/g) || [];
        return { tag: (m[1] || '').toUpperCase(), id: (parts.find(x => x[0] === '#') || '#').slice(1),
                 cls: parts.filter(x => x[0] === '.').map(x => x.slice(1)) };
    };
    const one = (e, p) => (!p.tag || e.tagName === p.tag) && (!p.id || e._id === p.id) && p.cls.every(c => e._cls.has(c));
    const matches = (e, sel) => sel.split(',').some(alt => {
        const chain = alt.trim().split(/\s+/).map(parse);
        if (!one(e, chain[chain.length - 1])) return false;
        let k = chain.length - 2;
        for (let n = e.parentNode; n && k >= 0; n = n.parentNode) if (one(n, chain[k])) k--;
        return k < 0;
    });
    function* walk(e) { for (const c of e.children) { yield c; yield* walk(c); } }
    function fireOn(n, ev) {
        ev.currentTarget = n;
        for (const f of (n._ls[ev.type] || []).slice()) {
            try { f.call(n, ev); } catch (e) { page.uncaught.push(e); }   /* a browser reports it and goes on */
        }
    }
    class Ev {
        constructor(type, o) { this.type = type; this.bubbles = !!(o && o.bubbles); this.defaultPrevented = false; this.target = null; }
        preventDefault() { this.defaultPrevented = true; }
        stopPropagation() { this.stopped = true; }
    }
    const ctx2d = new Proxy({}, { get: (t, k) => (k in t ? t[k] : k === 'measureText' ? () => ({ width: 0 }) : () => {}),
                                  set: (t, k, v) => { t[k] = v; return true; } });
    let head = null;
    class El {
        constructor(tag) {
            this.tagName = this.nodeName = String(tag).toUpperCase();
            this.children = []; this.parentNode = null; this.style = {}; this.dataset = {}; this.attrs = {};
            this._cls = new Set(); this._ls = {}; this._id = ''; this._text = ''; this._html = ''; this._auto = false;
            this.hidden = false; this.disabled = false; this.checked = false; this.value = ''; this.type = '';
            this.width = 0; this.height = 0; this.tabIndex = 0; this.scrollTop = 0;
        }
        get id() { return this._id; }
        set id(v) { this._id = String(v); byId.set(this._id, this); }
        get className() { return Array.from(this._cls).join(' '); }
        set className(v) { this._cls = new Set(String(v).split(/\s+/).filter(Boolean)); }
        get classList() {
            const s = this._cls;
            return { add: (...c) => c.forEach(x => s.add(x)), remove: (...c) => c.forEach(x => s.delete(x)),
                     toggle: (c, on) => { if (on === undefined) on = !s.has(c); if (on) s.add(c); else s.delete(c); return !!on; },
                     contains: c => s.has(c) };
        }
        get textContent() { return this._text; }
        set textContent(v) { for (const c of this.children) c.parentNode = null; this.children = []; this._text = String(v); this._html = ''; }
        get innerHTML() { return this._html; }
        set innerHTML(v) {   /* the elements in it, flat (the page reads them by id, class or tag) */
            v = String(v);
            for (const c of this.children) c.parentNode = null;
            this.children = []; this._html = v; this._text = v.replace(/<[^>]*>/g, '');
            for (const m of v.matchAll(/<([a-zA-Z][\w-]*)([^>]*)>/g)) {
                const c = new El(m[1]), cl = /\sclass=(['"])(.*?)\1/.exec(m[2]), id = /\sid=(['"])(.*?)\1/.exec(m[2]);
                if (cl) c.className = cl[2];
                if (id) c.id = id[2];
                this.appendChild(c);
            }
        }
        get firstChild() { return this.firstElementChild; }
        get firstElementChild() {
            if (!this.children.length && this._auto) this.appendChild(new El('i'));   /* ocean.html's own child */
            return this.children[0] || null;
        }
        get parentElement() { return this.parentNode; }
        get offsetWidth() { return 0; } get offsetHeight() { return 0; } get clientWidth() { return 0; } get clientHeight() { return 0; }
        appendChild(c) {
            if (c.parentNode) c.parentNode.removeChild(c);
            c.parentNode = this;
            this.children.push(c);
            if (this === head && c.tagName === 'SCRIPT') page.onScript(c);
            return c;
        }
        removeChild(c) { const i = this.children.indexOf(c); if (i >= 0) this.children.splice(i, 1); c.parentNode = null; return c; }
        remove() { if (this.parentNode) this.parentNode.removeChild(this); }
        setAttribute(k, v) { this.attrs[k] = String(v); if (k === 'id') this.id = v; else if (k === 'class') this.className = v; }
        getAttribute(k) { return Object.prototype.hasOwnProperty.call(this.attrs, k) ? this.attrs[k] : null; }
        removeAttribute(k) { delete this.attrs[k]; }
        addEventListener(t, f) { (this._ls[t] = this._ls[t] || []).push(f); }
        removeEventListener(t, f) { const l = this._ls[t] || [], i = l.indexOf(f); if (i >= 0) l.splice(i, 1); }
        dispatchEvent(ev) {
            if (!ev.target) ev.target = this;
            for (let n = this; n; n = ev.bubbles && !ev.stopped ? n.parentNode : null) fireOn(n, ev);
            return !ev.defaultPrevented;
        }
        click() { this.dispatchEvent(new Ev('click', { bubbles: true })); }
        focus() { doc.activeElement = this; }
        blur() { if (doc.activeElement === this) doc.activeElement = null; }
        getBoundingClientRect() { return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0, x: 0, y: 0 }; }
        getContext() { return ctx2d; }
        setPointerCapture() {}
        releasePointerCapture() {}
        querySelectorAll(sel) { const out = []; for (const e of walk(this)) if (matches(e, sel)) out.push(e); return out; }
        querySelector(sel) {
            for (const e of walk(this)) if (matches(e, sel)) return e;
            if (!this._auto) return null;
            const p = parse(sel.split(',')[0].trim().split(/\s+/).pop());   /* ocean.html's own child */
            const c = new El(p.tag || 'div');
            if (p.cls.length) c.className = p.cls.join(' ');
            if (p.id) c.id = p.id;
            c._auto = true;
            return this.appendChild(c);
        }
        closest(sel) { for (let n = this; n; n = n.parentNode) if (matches(n, sel)) return n; return null; }
    }
    const docEl = new El('html'), body = new El('body');
    head = new El('head');
    docEl.appendChild(head); docEl.appendChild(body);
    const doc = {
        documentElement: docEl, head, body, activeElement: null, hidden: false, _ls: {},
        createElement: t => new El(t),
        getElementById(id) {
            let e = byId.get(id);
            if (!e && PAGE_IDS.has(id)) { e = new El(PAGE_IDS.get(id)); e._auto = true; e.id = id; body.appendChild(e); }
            return e || null;
        },
        querySelector: s => docEl.querySelector(s),
        querySelectorAll: s => docEl.querySelectorAll(s),
        addEventListener(t, f) { (this._ls[t] = this._ls[t] || []).push(f); },
        removeEventListener() {},
        dispatchEvent(ev) { fireOn(doc, ev); return true; }
    };
    return { doc, Ev, fireOn };
}

const tick = () => new Promise(r => setImmediate(r));
async function until(pred, what, ms) {
    const t0 = Date.now();
    while (!pred()) {
        if (Date.now() - t0 > (ms || 30000)) throw new Error('timed out waiting for ' + what);
        await new Promise(r => setTimeout(r, 2));
    }
}
const unref = (t) => { if (t && t.unref) t.unref(); return t; };

/* A page at ocean.html?query, started (or refused: page.failed). */
async function bootPage(query) {
    const page = { hooks: {}, workers: [], uncaught: [], logs: [], inits: [], sets: [], blobs: [], raf: null };
    const { doc, Ev, fireOn } = makeDom(page);
    const loc = {};
    const setLoc = (u) => { const x = new URL(u); Object.assign(loc, { href: x.href, search: x.search, origin: x.origin, protocol: x.protocol, pathname: x.pathname }); };
    setLoc('http://ocean.test/ocean.html' + (query || ''));
    const storage = new Map(), winLs = {};
    const say = (level) => (...a) => page.logs.push([level, a.map(String).join(' ')]);
    const ctx = {
        document: doc, location: loc, navigator: { userAgent: 'node' },
        history: { replaceState: (s, t, u) => setLoc(String(u)) },
        localStorage: { getItem: k => (storage.has(k) ? storage.get(k) : null), setItem: (k, v) => storage.set(k, String(v)), removeItem: k => storage.delete(k) },
        matchMedia: () => ({ matches: false, addEventListener() {}, removeEventListener() {}, addListener() {} }),
        getComputedStyle: () => ({ fontFamily: 'sans-serif', getPropertyValue: () => '' }),
        requestAnimationFrame: (cb) => { page.raf = cb; return 1; }, cancelAnimationFrame() {},
        innerWidth: 1920, innerHeight: 1080, devicePixelRatio: 1, _ls: winLs,
        addEventListener: (t, f) => { (winLs[t] = winLs[t] || []).push(f); }, removeEventListener() {},
        dispatchEvent: (ev) => { fireOn(ctx, ev); return true; },
        Event: Ev, URL, URLSearchParams, ImageData: class {}, performance, WebAssembly, TextEncoder, TextDecoder,
        Blob: function (parts, o) { page.blobs.push(parts.map(String).join('')); return new Blob(parts, o); },   /* Save setup's file */
        console: { log: say('log'), info: say('info'), warn: say('warn'), error: say('error'), debug: say('debug') },
        setTimeout: (f, ms) => unref(setTimeout(f, ms)), clearTimeout, setInterval: (f, ms) => unref(setInterval(f, ms)), clearInterval,
        EVO_ASSETS: { ocean: 'evo_ocean.js', oceanWasm: 'evo_ocean.wasm', worker: 'ocean_worker.js', handoff: 'ocean_handoff.js', mock: 'ocean_mock.js' },
        Worker: function () { return page.newWorker(); }
    };
    ctx.window = ctx.self = ctx;
    vm.createContext(ctx);
    /* the module: ocean.html's script tag for it, loaded as the real one, its init and live calls logged */
    page.onScript = (s) => setImmediate(() => {
        if (!/evo_ocean\.js$/.test(s.src)) { if (s.onerror) s.onerror(); return; }
        ctx.createEvoOcean = () => create({ locateFile: p => p.endsWith('.wasm') ? WASM : p }).then((M) => {
            const init = M._oc_init, set = M._oc_set_param;
            M._oc_init = function (seed, w, h, d, pop, p) { page.inits.push(HO.cString(M, p)); return init.apply(this, arguments); };
            M._oc_set_param = function (p, v) {
                const key = HO.cString(M, p), r = set(p, v);
                if (r) page.sets.push([frame(M), key, v]);
                return r;
            };
            return M;
        });
        if (s.onload) s.onload();
    });
    /* the page's Worker: web/ocean_worker.js (makeWorker), its messages to the page through an
     * ordered channel the test can pause (they queue) and let through up to a message (release) */
    page.newWorker = () => {
        if (page.hooks.ctor) { const h = page.hooks.ctor; page.hooks.ctor = null; h(); }
        const sh = { onmessage: null, onerror: null, terminated: 0, toWorker: [], fromWorker: [], delivered: [], queue: [], paused: false };
        const deliver = (m) => { if (sh.terminated || !sh.onmessage) return; sh.delivered.push(m); sh.onmessage({ data: m }); };
        const w = makeWorker({ out: (m) => {
            if (sh.terminated) return;
            sh.fromWorker.push(m);
            if (sh.paused) sh.queue.push(m); else setImmediate(() => deliver(m));
        } });
        Object.defineProperty(sh, 'M', { get: () => w.M });
        sh.postMessage = (msg) => {
            if (msg && msg.cmd === 'start' && page.hooks.post) { const h = page.hooks.post; page.hooks.post = null; h(); }
            if (sh.terminated) return;
            sh.toWorker.push(msg);
            w.send(msg.cmd !== 'start' ? msg : Object.assign({}, msg, { js: path.join(WEB, 'evo_ocean.js'), wasm: WASM, handoff: path.join(WEB, 'ocean_handoff.js') }));
        };
        sh.terminate = () => { sh.terminated++; w.kill(); };
        sh.pause = () => { sh.paused = true; };
        sh.resume = () => { sh.paused = false; for (const m of sh.queue.splice(0)) deliver(m); };
        sh.queued = (pred) => sh.queue.some(pred);
        sh.release = async (pred, what) => {   /* everything queued up to and with the first message pred takes */
            await until(() => sh.queue.some(pred), what || 'a queued message');
            while (sh.queue.length) { const m = sh.queue.shift(); deliver(m); if (pred(m)) return m; }
            return null;
        };
        page.workers.push(sh);
        if (page.hooks.onWorker) page.hooks.onWorker(sh);
        return sh;
    };
    const run = (name, src) => vm.runInContext(src !== undefined ? src : fs.readFileSync(path.join(WEB, name), 'utf8'), ctx, { filename: path.join(WEB, name) });
    run('ocean_handoff.js'); run('ocean_scene_stub.js', SCENE_STUB); run('ocean_config.js');
    run('charts_stub.js', CHARTS_STUB); run('ocean_dash.js'); run('ocean.js', PAGE_SRC);
    page.ctx = ctx; page.doc = doc; page.Ev = Ev; page.T = ctx.__oceanPage; page.KEYS = ctx.EvoOceanConfig.KEYS;
    page.el = (id) => doc.getElementById(id);
    await until(() => (ctx.EvoApp && ctx.EvoApp.ready) || page.el('splashstate').className === 'error', 'the page to start', 60000);
    page.failed = page.el('splashstate').className === 'error' ? page.el('splashstate').textContent : '';
    page.M = page.T.M;
    page.click = (id) => page.el(id).click();
    page.msg = () => page.el('err').textContent;
    page.worker = () => page.workers[page.workers.length - 1];
    /* a field changed by hand: its input, then its change (the live ones apply on change) */
    page.edit = (key, v) => {
        const i = page.el('o_' + key);
        if (i.type === 'checkbox') i.checked = v === true || v === 1; else i.value = String(v);
        i.dispatchEvent(new Ev('input', { bubbles: true }));
        i.dispatchEvent(new Ev('change', { bubbles: true }));
    };
    page.note = (key) => { const r = page.el('f_' + key), n = r && r.querySelector('.pendnote'); return n ? n.textContent : ''; };
    page.form = () => JSON.stringify(Object.keys(page.KEYS).filter(k => page.el('o_' + k)).map(k => [k, page.T.getVal(k)]));
    /* Load setup's file input, given a file */
    page.loadFile = (file) => { const i = page.el('cpfile'); i.files = [file]; i.dispatchEvent(new Ev('change', { bubbles: true })); };
    page.state = () => JSON.stringify({ initArgs: page.T.initArgs, runCfg: page.T.runCfg, W: page.T.W, H: page.T.H, D: page.T.D,
                                        walls: page.T.WALLS, url: loc.href, form: page.form() });
    page.loc = loc;
    return page;
}
/* the Turbo controls, as they read: off ("live") or on */
function turboControls(p) {
    const locked = ['restart', 'savecp', 'loadcp', 'pausebtn', 'slower', 'faster', 'randombtn', 'randombig', 'newworld', 'replay']
        .filter(id => p.el(id).classList.contains('locked'));
    return { card: !p.el('turbo').hidden, button: !p.el('turbobtn').disabled, locked };
}
/* a world like the page's from an image of its memory (the control's start),
 * edits [frame, key, value] applied at their frames, stepped to toFrame */
async function controlRun(img, edits, toFrame) {
    const ref = await mk(1, {}, 300, 300, 0);
    HO.take(ref, img.slice(0), 'the control');
    for (const [f, key, v] of edits) {
        if (f < frame(ref)) throw new Error('control: an edit at frame ' + f + ' before ' + frame(ref));
        ref._oc_step(f - frame(ref)); setParam(ref, key, v);
    }
    if (toFrame < frame(ref)) throw new Error('control: frame ' + toFrame + ' before ' + frame(ref));
    ref._oc_step(toFrame - frame(ref));
    return ref;
}
/* the edits a page's worker applied: [frame, key, value] */
const workerEdits = (sh) => sh.fromWorker.filter(m => m.type === 'set' && m.ok).map(m => [m.frame, m.key, m.value]);
/* the worker's next oc_step traps (its world is lost: no memory comes with the error) */
function trapNext(sh) { sh.M._oc_step = function () { throw new WebAssembly.RuntimeError('injected: a trap mid-step'); }; }
const PAGE_Q = '?seed=4242&pop=60&w=1920&h=1080&walls=1';
/* a page in Turbo, its world's image and frame from just before */
async function pageInTurbo(q) {
    const p = await bootPage(q || PAGE_Q);
    const img = HO.buffer(p.M).slice(0), f0 = frame(p.M), sets0 = p.sets.length;
    p.click('turbobtn');
    await until(() => p.T.turboState === 'turbo', 'Turbo to start');
    const sh = p.worker();
    await until(() => sh.fromWorker.some(m => m.type === 'stats' && m.frame >= f0 + 120), 'Turbo to run a while');
    return { p, img, f0, sets0, sh };
}

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
     * readOceanJson), and the page leaves that to it (pageBools). */
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

/* The page's live edits around Turbo (web/ocean_handoff.js ledger, which
 * web/ocean.js turboBack / turboFailed / replayEdits use as they are): the edits
 * the page applies when the world comes back, or doesn't, in the order they
 * were made. Driven here as the page drives it, against the real worker and
 * the real module, with a worker failure that loses the world. */
async function liveLedger() {
    section('Live edits around Turbo: the page applies them in the order they were made (ledger)');
    {   /* the ledger alone: order, a newer edit never under an older one */
        const L = HO.ledger();
        L.start();
        L.sent('a', 1); L.sent('b', 2); L.answered('a', true); L.hold('a', 3);
        const kept = L.back(false).map(e => e.key + e.num + e.from).join(' ');
        ok(kept === 'b2sent a3held', 'with the worker\'s world: the unanswered and held edits, oldest first (' + kept + ')');
        L.start();
        L.sent('a', 1); L.answered('a', true); L.sent('a', 5); L.answered('a', false); L.sent('b', 2); L.hold('a', 3); L.hold('a', 4);
        const lost = L.back(true).map(e => e.key + e.num + e.from).join(' ');
        ok(lost === 'a1acked b2sent a3held a4held', 'without it: the acknowledged ones first, a refused one never, every held edit in order (' + lost + ')');
        L.start();
        L.hold('x', 1); L.hold('y', 2);
        const fl = L.takeHeld(); fl.forEach(e => L.sent(e.key, e.num, e.seq));
        L.hold('x', 9);
        const pendX = L.pending('x');
        const order = L.back(true).map(e => e.key + e.num).join(' ');
        ok(order === 'x1 y2 x9' && pendX === true && L.pending('x') === false,
           'a held edit flushed to the worker keeps its place (' + order + '); x is pending until the world is back');
        /* pending(key, seq): a NEWER edit of key, held or in flight, than the answered one */
        L.start();
        L.sent('k', 1); L.hold('k', 2);
        const refusedK = L.answered('k', false);
        const newerHeld = L.pending('k', refusedK.seq);
        L.back(false);
        L.start();
        L.hold('m', 1); L.sent('m', 2);
        const refusedM = L.answered('m', false);
        const olderHeld = L.pending('m', refusedM.seq);
        L.sent('m', 3);
        const newerSent = L.pending('m', refusedM.seq), notAfterItself = L.pending('m', 99);
        L.back(false);
        ok(newerHeld === true && olderHeld === false && newerSent === true && notAfterItself === false,
           'pending(key, seq): a newer held edit counts (' + newerHeld + '), an older held one does not (' + olderHeld +
           '), a newer one in flight does (' + newerSent + '), nothing after the newest (' + notAfterItself + ')');
    }

    const SEED = 4242;
    /* (#2) dependent edits, acknowledged in Turbo, then the worker dies without its world */
    {
        const page = await mk(SEED);
        page._oc_step(100);
        const entryFrame = frame(page), entryImg = HO.buffer(page).slice(0), entryCfg = {};
        const L = HO.ledger(), runCfg = {};
        L.start();
        const w = makeWorker();
        w.send(startMsg(entryImg.slice(0)));
        await w.wait(isType('stats'), 'the first stats');
        for (const [k, v] of [['minSpeed', 0], ['maxSpeed', 300], ['minSpeed', 200]]) {
            L.sent(k, v);
            w.send({ cmd: 'set', key: k, value: v });
            const r = await w.wait(isType('set'), 'the ' + k + ' reply');
            L.answered(r.key, !!r.ok);
            if (r.ok) liveApplied(runCfg, k, v);
            ok(r.ok, 'in Turbo: ' + k + ' ' + v + ' acknowledged');
        }
        trapAt(w, 2);
        const e = await w.wait(isType('error'), 'the error');
        ok(!e.mem && frame(page) === entryFrame, 'the worker dies without its world; the page has its copy from Turbo\'s start (frame ' + entryFrame + ')');
        /* main's recovery first, on a copy of the same world */
        const legacy = await mk(1);
        HO.take(legacy, entryImg.slice(0), 'the legacy');
        const lplan = legacyRecovery(runCfg, entryCfg, {}, []);
        const lres = lplan.map(([k, v]) => HO.withCString(legacy, k, p => legacy._oc_set_param(p, v)) ? '' : k + ' ' + v + ': ' + HO.cString(legacy, legacy._oc_last_error()));
        ok(lplan.map(x => x.join('=')).join(',') === 'minSpeed=200,maxSpeed=300' && lres[0] !== '' && lres[1] === '',
           'main\'s recovery (the config difference in key order) is refused minSpeed 200 against the old maximum (' + lres[0] + '), then sets the maximum: 30-300');
        /* the page's: the ledger's plan, replayed in order */
        const plan = L.back(true), pRun = Object.assign({}, entryCfg);
        const res = replayOnPage(page, plan, pRun);
        ok(plan.map(x => x.key + '=' + x.num).join(',') === 'minSpeed=0,maxSpeed=300,minSpeed=200' && res.every(x => x === ''),
           'the page replays them as the worker applied them (' + plan.map(x => x.key + '=' + x.num).join(', ') + '), each accepted');
        ok(pRun.minSpeed === 200 && pRun.maxSpeed === 300 && JSON.stringify(pRun) === JSON.stringify(runCfg),
           'runCfg after the recovery is what Turbo acknowledged (' + JSON.stringify(pRun) + ')');
        const ref = await mk(SEED);
        ref._oc_step(entryFrame);
        setParam(ref, 'minSpeed', 0); setParam(ref, 'maxSpeed', 300); setParam(ref, 'minSpeed', 200);
        const ref2 = await mk(1);
        HO.take(ref2, HO.buffer(ref).slice(0), 'the reference copy');
        ok(sameFor(page, ref, 150) === '', 'the recovered world runs 200-300: 150 steps bit-identical to the same edits applied directly');
        ok(sameFor(legacy, ref2, 150) !== '', 'and main\'s recovered world is another one (it runs 30-300)');
    }
    /* (#3) an edit sent in Turbo, not answered yet; Stop; a newer edit of it held; the worker dies */
    {
        const page = await mk(SEED);
        page._oc_step(100);
        const entryFrame = frame(page);
        const L = HO.ledger(), runCfg = {}, form = {};
        L.start();
        const w = makeWorker();
        w.send(startMsg(HO.buffer(page).slice(0)));
        await w.wait(isType('stats'), 'the first stats');
        L.sent('minSpeed', 10); form.minSpeed = 10;
        w.send({ cmd: 'set', key: 'minSpeed', value: 10 });   /* its answer: never read (the worker is dropped first) */
        /* Stop: "exiting"; the field changes again and is held */
        L.hold('minSpeed', 20); form.minSpeed = 20;
        trapAt(w, 1);
        await w.wait(isType('error'), 'the error');
        const lplan = legacyRecovery(null, {}, { minSpeed: 20 }, L.inflight());
        ok(lplan.length === 1 && lplan[0][1] === 10 && form.minSpeed === 20,
           'main\'s turboBack copies the unanswered 10 over the held 20: the world would run 10 under a field showing 20');
        const plan = L.back(true);
        const res = replayOnPage(page, plan, runCfg);
        ok(plan.map(x => x.num).join(',') === '10,20' && res.every(x => x === ''), 'the page applies 10, then the newer 20');
        ok(form.minSpeed === 20 && runCfg.minSpeed === 20, 'the field, runCfg and the world agree: minSpeed 20');
        const ref = await mk(SEED);
        ref._oc_step(entryFrame);
        setParam(ref, 'minSpeed', 20);
        ok(sameFor(page, ref, 150) === '', 'the world runs minSpeed 20: 150 steps bit-identical to that edit applied directly (frame ' + entryFrame + ')');
    }
}

/* a number too big for a double, in either occurrence of a repeated key (int, Ocean int, Ocean bool),
 * and in a float the engine clamps (perceptionRadius: only a finite one is raised to 1; -1e400 was
 * stored as 1, PR #34's review) — every one of them refused by oc_init */
const OVERFLOW_DOCS = ['{"maxPopulation":300,"maxPopulation":1e400}', '{"maxPopulation":1e400,"maxPopulation":300}',
                       '{"maxPopulation":300,"maxPopulation":-1e400}', '{"maxPopulation":-1e400,"maxPopulation":300}',
                       '{"appearance":3,"appearance":1e400}', '{"appearance":1e400,"appearance":3}',
                       '{"lightEnabled":true,"lightEnabled":1e400}', '{"lightEnabled":1e400,"lightEnabled":true}',
                       '{"perceptionRadius":-1e400}', '{"perceptionRadius":-1e400,"perceptionRadius":100}'];
/* A key twice in ?cfg= or a setup file: the page now follows the engine's
 * rule (web/ocean_handoff.js parseFirst / cfgJson, as web/ocean.js
 * importWorld / loadSetup use them): the first occurrence counts, every one
 * is checked. JSON.parse kept the last and dropped the rest unchecked. */
async function repeatedKeys() {
    section('A repeated key in ?cfg= or a setup file: the engine\'s rule on the page (parseFirst / cfgJson)');
    const same = ['{}', '[]', '{"a":1,"b":[1,2,{"c":"x\\"y\\\\"}],"d":{"e":null,"f":true,"g":false}}', '"\\u00e9\\ud83d\\ude00"',
                  '-0', '1e-7', ' {\n "a" : [ ] ,\t"b" : { } } ', '{"__proto__":{"x":1},"y":-12.5e3}', '[{"a":{"b":[[]]}}]'];
    const deq = (a, b) => JSON.stringify(a) === JSON.stringify(b) && (typeof a !== 'number' || Object.is(a, b));   /* -0 too */
    ok(same.every(t => { const p = HO.parseFirst(t); return deq(p.value, JSON.parse(t)) && !p.repeats.length; }),
       'parseFirst is JSON.parse on documents without a repeated key (' + same.length + ' of them, escapes and nesting)');
    const proto = HO.parseFirst('{"__proto__":{"x":1}}').value;
    ok(Object.getPrototypeOf(proto) === Object.prototype && Object.prototype.hasOwnProperty.call(proto, '__proto__'),
       'a "__proto__" key is an own property, as JSON.parse makes it (no prototype change)');
    ok(['{', '{"a":1,}', '[1,]', '{\'a\':1}', '{"a":01}', ''].every(t => throws(() => HO.parseFirst(t)) === throws(() => JSON.parse(t)) && throws(() => JSON.parse(t)) !== ''),
       'it refuses what JSON.parse refuses, with the same message');
    const nest = HO.parseFirst('{"a":1,"b":{"c":1,"c":2},"a":[3],"a":4}');
    ok(nest.value.a === 1 && nest.value.b.c === 1 && nest.repeats.map(r => r.path.join('.') + ':' + r.key + '=' + JSON.stringify(r.value)).join(' ') === 'b:c=2 :a=[3] :a=4',
       'a repeated key keeps its first value and lists the later ones with where they are');

    const M = await mk(1);
    const init = (json) => {
        const r = HO.withCString(M, json, p => M._oc_init(3, 1920, 1080, 240, 60, p));
        return { ok: r === 1, why: r ? '' : HO.cString(M, M._oc_last_error()), print: r ? stepped(M) : '' };
    };
    /* what cfgJson writes for finite values is JSON.stringify's, byte for byte (every sheet,
     * preset and finite import reaches oc_init as it did) */
    const finite = [{}, { a: 1, b: -0, c: 0.006, d: 1e21, e: -2.5e-7, f: true, g: null, h: 'x"y\\zé', i: [1, [2, { j: 3 }], null], k: {} },
                    CFG.BIG_TANK.cfg, JSON.parse(HO.cString(M, M._oc_e1_preset())), { walls: true, maxPopulation: 2147483647 }];
    ok(finite.every(c => !Object.keys(c).length ? HO.cfgJson(c) === '' : HO.cfgJson(c) === JSON.stringify(c)),
       'cfgJson writes finite values as JSON.stringify does (' + finite.length + ' configs: nesting, -0, exponents, escapes, the Big tank, E1)');
    /* the URL's text (URLSearchParams decodes it), and a setup file with it as its config */
    for (const doc of ['{"toroidal":1,"toroidal":true}', '{"toroidal":true,"toroidal":1}', '{"toroidal":false,"toroidal":true}',
                       '{"foodEnergy":5,"foodEnergy":9}', '{"maxPopulation":300,"maxPopulation":0.5}', '{"maxPopulation":0.5,"maxPopulation":300}']
                      .concat(OVERFLOW_DOCS)) {
        const direct = init(doc);
        const url = new URLSearchParams('cfg=' + encodeURIComponent(doc)).get('cfg');
        const pf = HO.parseFirst(url), top = pf.repeats.filter(r => !r.path.length);
        const page = init(HO.cfgJson(pf.value, top));
        const file = HO.parseFirst('{"format":"evo_ocean_setup","seed":3,"config":' + doc + '}');
        const fileCfg = init(HO.cfgJson(file.value.config, file.repeats.filter(r => r.path.length === 1 && r.path[0] === 'config')));
        const legacy = init(JSON.stringify(JSON.parse(url)));
        const agree = page.ok === direct.ok && page.print === direct.print && fileCfg.ok === direct.ok && fileCfg.print === direct.print;
        ok(agree, doc + ': the page (?cfg= and setup file) and oc_init agree: ' + (direct.ok ? 'accepted, the same world' : 'refused (' + direct.why + ')') +
           (legacy.ok !== direct.ok || legacy.print !== direct.print ? '; JSON.parse\'s page ' + (legacy.ok ? 'ran another world' : 'refused it') : ''));
    }
    const off = init('{"toroidal":false}'), twice = init(HO.cfgJson(HO.parseFirst('{"toroidal":false,"toroidal":true}').value,
                                                                     HO.parseFirst('{"toroidal":false,"toroidal":true}').repeats));
    ok(off.ok && twice.ok && off.print === twice.print, '{"toroidal":false,"toroidal":true} runs with walls on the page too (the first: false), as in oc_init');
}

/* ---- the page itself (bootPage): its live edits around Turbo ---- */

/* the three edits whose order matters: from 30-150, minSpeed 0, maxSpeed 10,
 * minSpeed 5 is valid only as a sequence (maxSpeed 10 is refused against 30) */
const SEQ3 = [['minSpeed', 0], ['maxSpeed', 10], ['minSpeed', 5]];
function edit3(p) { for (const [k, v] of SEQ3) p.edit(k, v); }
/* the page after the three edits: 5-10 in the world's bookkeeping and the fields, no note left */
function at5to10(p) {
    return p.T.runCfg.minSpeed === 5 && p.T.runCfg.maxSpeed === 10 && p.T.getVal('minSpeed') === 5 && p.T.getVal('maxSpeed') === 10 &&
           p.note('minSpeed') === '' && p.note('maxSpeed') === '' && JSON.stringify(p.T.live.held()) === '{}';
}
const speeds = (p) => 'runCfg ' + p.T.runCfg.minSpeed + '-' + p.T.runCfg.maxSpeed + ', fields ' + p.T.getVal('minSpeed') + '-' + p.T.getVal('maxSpeed') +
                      (p.note('minSpeed') || p.note('maxSpeed') ? ', notes "' + p.note('minSpeed') + '" / "' + p.note('maxSpeed') + '"' : '');
const setsTo = (sh) => sh.toWorker.filter(m => m.cmd === 'set').map(m => m.key + '=' + m.value).join(',');

/* (#1) Every held edit is applied, in order: edits made while Turbo is
 * starting or stopping (neither side owns the world) are held; main kept
 * only the newest of each key, so minSpeed 0, maxSpeed 10, minSpeed 5 lost
 * the 0, maxSpeed 10 was refused against 30 and the run went on at 5-150.
 * Through the page itself, entering, after a Stop, after a lost world and
 * after a hand-back the page could not take (Turbo resumes). */
async function pageHeld() {
    section('The page: every held live edit applies, in order (entering, Stop, a lost world, a resumed hand-back)');
    {   /* entering: the first stats held, so the worker has the world and the page does not know yet */
        const p = await bootPage(PAGE_Q);
        const img = HO.buffer(p.M).slice(0), f0 = frame(p.M);
        p.hooks.onWorker = (sh) => sh.pause();
        p.click('turbobtn');
        const sh = p.worker();
        await until(() => sh.queued(isType('stats')), 'the first stats');
        const entering = p.T.turboState;
        edit3(p);
        const notes = p.note('minSpeed') + ' | ' + p.note('maxSpeed');
        sh.resume();
        await until(() => p.T.turboState === 'turbo' && sh.delivered.filter(isType('set')).length === sh.toWorker.filter(m => m.cmd === 'set').length,
                    'the edits\' answers');
        ok(entering === 'entering' && /as soon as Turbo has started/.test(notes), 'entering: the edits are held ("' + notes + '")');
        ok(setsTo(sh) === 'minSpeed=0,maxSpeed=10,minSpeed=5' && sh.fromWorker.filter(isType('set')).every(m => m.ok),
           'entering: the worker receives all three in order, each accepted (' + setsTo(sh) + ')');
        p.click('turboexit');
        await until(() => p.T.turboState === 'live', 'the hand-back');
        ok(at5to10(p), 'entering, after Stop: 5-10 in runCfg and the fields, no note left (' + speeds(p) + ')');
        const ref = await controlRun(img, workerEdits(sh), frame(p.M));
        const d = sameFor(p.M, ref, 150);
        ok(f0 < frame(p.M) && d === '', 'entering: 150 further steps bit-identical to the edits applied at the worker\'s frames' + (d ? ': ' + d : ''));
    }
    {   /* a Stop: the memory held, the edits made while it comes */
        const { p, img, sets0, sh } = await pageInTurbo();
        sh.pause();
        p.click('turboexit');
        await until(() => sh.queued(isType('memory')), 'the memory');
        edit3(p);
        const notes = p.note('minSpeed') + ' | ' + p.note('maxSpeed');
        sh.resume();
        await until(() => p.T.turboState === 'live', 'the hand-back');
        ok(/as soon as Turbo hands the world back/.test(notes) && at5to10(p),
           'Stop: the three held edits apply on the world handed back, in order (' + speeds(p) + ')');
        const ref = await controlRun(img, workerEdits(sh).concat(p.sets.slice(sets0)), frame(p.M));
        const d = sameFor(p.M, ref, 150);
        ok(p.sets.slice(sets0).map(e => e[1] + '=' + e[2]).join(',') === 'minSpeed=0,maxSpeed=10,minSpeed=5' && d === '',
           'Stop: the page applied 0, 10, 5; 150 further steps bit-identical to that' + (d ? ': ' + d : ''));
    }
    {   /* a lost world: the worker traps (no memory comes back); the edits held while that is on its way */
        const { p, img, f0, sets0, sh } = await pageInTurbo();
        sh.pause();
        trapNext(sh);
        await until(() => sh.queued(isType('error')), 'the trap');
        p.click('turboexit');
        edit3(p);
        await sh.release(isType('error'), 'the error');
        await until(() => p.T.turboState === 'live', 'the recovery');
        ok(frame(p.M) === f0 && at5to10(p), 'a lost world: the page goes on from its own copy (frame ' + f0 + ') with all three, in order (' + speeds(p) + ')');
        const ref = await controlRun(img, p.sets.slice(sets0), f0);
        const d = sameFor(p.M, ref, 150);
        ok(d === '', 'a lost world: 150 further steps bit-identical to the three edits applied at frame ' + f0 + (d ? ': ' + d : ''));
    }
    {   /* a hand-back the page cannot take: Turbo resumes, then the held edits go to the worker */
        const { p, img, sh } = await pageInTurbo();
        sh.pause();
        p.click('turboexit');
        await until(() => sh.queued(isType('memory')), 'the memory');
        edit3(p);
        const H = p.ctx.EvoOceanHandoff, take = H.take;
        H.take = function () { H.take = take; throw new Error('injected: the page could not grow its memory'); };
        sh.resume();
        await until(() => p.T.turboState === 'turbo' && sh.toWorker.some(m => m.cmd === 'resume') &&
                          sh.delivered.filter(isType('set')).length === sh.toWorker.filter(m => m.cmd === 'set').length, 'the resumed run\'s answers');
        const order = sh.toWorker.filter(m => m.cmd !== 'rec').map(m => m.cmd === 'set' ? m.key + '=' + m.value : m.cmd).join(',');
        ok(order === 'start,stop,resume,minSpeed=0,maxSpeed=10,minSpeed=5' && sh.fromWorker.filter(isType('set')).every(m => m.ok),
           'a refused hand-back: the page resumes Turbo, then sends the three in order, each accepted (' + order + ')');
        p.click('turboexit');
        await until(() => p.T.turboState === 'live', 'the second hand-back');
        ok(at5to10(p), 'a refused hand-back: the next Stop returns 5-10 (' + speeds(p) + ')');
        const ref = await controlRun(img, workerEdits(sh), frame(p.M));
        const d = sameFor(p.M, ref, 150);
        ok(d === '', 'a refused hand-back: 150 further steps bit-identical to the edits at the worker\'s frames' + (d ? ': ' + d : ''));
        ok(p.uncaught.length === 0, 'no exception escaped the page\'s handlers' + (p.uncaught.length ? ': ' + p.uncaught[0] : ''));
    }
}

/* (#2) An answer to an older edit must not undo a newer one. The worker
 * refuses minSpeed 200 (the maximum is 150); before the refusal reaches the
 * page, Stop, and minSpeed 20 is held. main's handler asked only whether a
 * newer edit was in flight: it reset the field to 30 and cleared its note,
 * then the recovery applied the held 20 under a field showing 30. Through
 * the page's own handler, with the world handed back and with it lost; the
 * controls: no newer edit (the field goes back to 30), a newer one in
 * flight (it stays); and with the held three-edit sequence after it. */
async function pageRefusal() {
    section('The page: an older edit\'s refusal leaves a newer held or in-flight edit alone');
    const refusal = (m) => m.type === 'set' && !m.ok;
    for (const how of ['handed back', 'lost']) {
        const { p, img, f0, sets0, sh } = await pageInTurbo();
        sh.pause();
        p.edit('minSpeed', 200);
        await until(() => sh.queued(refusal), 'the refusal');
        if (how === 'lost') { trapNext(sh); await until(() => sh.queued(isType('error')), 'the trap'); }
        p.click('turboexit');
        if (how !== 'lost') await until(() => sh.queued(isType('memory')), 'the memory');
        p.edit('minSpeed', 20);
        const held = p.note('minSpeed');
        await sh.release(refusal, 'the refusal');
        const field = p.T.getVal('minSpeed'), note = p.note('minSpeed');
        ok(held !== '' && field === 20 && note === held,
           how + ': the old refusal leaves the held 20 in its field, its note unchanged (field ' + field + ', note "' + note + '")');
        sh.resume();
        await until(() => p.T.turboState === 'live', 'the world back');
        ok(p.T.runCfg.minSpeed === 20 && p.T.getVal('minSpeed') === 20 && p.note('minSpeed') === '',
           how + ': the world, runCfg and the field agree on 20, no note left (runCfg ' + p.T.runCfg.minSpeed + ', field ' + p.T.getVal('minSpeed') + ')');
        const edits = (how === 'lost' ? [] : workerEdits(sh)).concat(p.sets.slice(sets0)), at = frame(p.M);
        const ref = await controlRun(img, edits, at);
        const d = sameFor(p.M, ref, 150);
        ok((how !== 'lost' || at === f0) && d === '', how + ': 150 further steps bit-identical to minSpeed 20 applied where the page applied it (frame ' +
           at + ')' + (d ? ': ' + d : ''));
    }
    {   /* controls: no newer edit (back to 30); a newer one in flight (the field stays) */
        const { p, sh } = await pageInTurbo();
        sh.pause();
        p.edit('minSpeed', 200);
        await sh.release(refusal, 'the refusal');
        ok(p.T.getVal('minSpeed') === 30 && p.note('minSpeed') === '' && !('minSpeed' in p.T.runCfg) && /back to 30/.test(p.msg()),
           'no newer edit: the refused field goes back to 30 ("' + p.msg() + '")');
        p.edit('minSpeed', 200);
        await until(() => sh.queued(refusal), 'the second refusal');
        p.edit('minSpeed', 20);
        await sh.release(refusal, 'the refusal');
        const kept = p.T.getVal('minSpeed');
        sh.resume();
        await until(() => sh.delivered.filter(isType('set')).length === 3, 'the answer to 20');
        ok(kept === 20 && p.T.getVal('minSpeed') === 20 && p.T.runCfg.minSpeed === 20,
           'a newer edit in flight: the field stays 20 through the old refusal, then 20 applies (' + kept + ', runCfg ' + p.T.runCfg.minSpeed + ')');
    }
    {   /* with #1: a refused older edit, then the held three-edit sequence */
        const { p, img, sets0, sh } = await pageInTurbo();
        sh.pause();
        p.edit('minSpeed', 200);
        await until(() => sh.queued(refusal), 'the refusal');
        p.click('turboexit');
        await until(() => sh.queued(isType('memory')), 'the memory');
        edit3(p);
        await sh.release(refusal, 'the refusal');
        const fields = p.T.getVal('minSpeed') + '-' + p.T.getVal('maxSpeed');
        sh.resume();
        await until(() => p.T.turboState === 'live', 'the world back');
        ok(fields === '5-10' && at5to10(p), 'a refusal before the held 0, 10, 5: the fields stay 5-10 through it, and 5-10 is what runs (' + speeds(p) + ')');
        const ref = await controlRun(img, workerEdits(sh).concat(p.sets.slice(sets0)), frame(p.M));
        const d = sameFor(p.M, ref, 150);
        ok(d === '' && p.uncaught.length === 0, 'and 150 further steps are bit-identical to that' + (d ? ': ' + d : ''));
    }
}

/* (#3) Load setup reads the file asynchronously; the page checked who owns
 * the world before the read, not when it completed. A read started while
 * the page ran and finished during Turbo replaced the page's copy (the one
 * a lost Turbo world falls back to) and its seed and settings while the
 * worker went on with the old world. Through the page's own file-input
 * callback, the read completing while Turbo starts, runs and stops; then
 * the same file while the page runs. */
const SETUP99 = JSON.stringify({ format: 'evo_ocean_setup', seed: 99, population: 12, depth: 200, width: 800, height: 600,
                                 config: { minPopulation: 0, targetFood: 0, foodSpawnBase: 0, foodSpawnAmp: 0, foodEnergy: 9 } });
function deferredFile(text) {
    let resolve;
    const pr = new Promise((r) => { resolve = r; });
    return { file: { name: 'setup.json', text: () => pr }, resolve: () => resolve(text) };
}
async function pageAsyncSetup() {
    section('The page: a setup file whose read completes during Turbo is refused then, not loaded');
    for (const when of ['entering', 'turbo', 'exiting']) {
        const p = await bootPage(PAGE_Q);
        const f0 = frame(p.M), print0 = fingerprint(p.M), state0 = p.state(), inits0 = p.inits.length;
        const img = HO.buffer(p.M).slice(0);
        const f = deferredFile(SETUP99);
        p.loadFile(f.file);   /* the page runs: the read starts */
        if (when === 'entering') p.hooks.onWorker = (sh) => sh.pause();
        p.click('turbobtn');
        const sh = p.worker();
        if (when === 'entering') await until(() => sh.queued(isType('stats')), 'the first stats');
        else await until(() => p.T.turboState === 'turbo', 'Turbo');
        if (when === 'exiting') { sh.pause(); trapNext(sh); await until(() => sh.queued(isType('error')), 'the trap'); p.click('turboexit'); }
        const st = p.T.turboState;
        f.resolve();
        for (let i = 0; i < 4; i++) await tick();
        const kept = p.inits.length === inits0 && fingerprint(p.M) === print0 && p.state() === state0;
        ok(st === when && kept && /Stop Turbo first/.test(p.msg()),
           when + ': the read completing then is refused ("' + p.msg() + '"): no oc_init, the page\'s copy, seed, settings, size, sheet and address as they were');
        if (when === 'entering') { sh.resume(); await until(() => p.T.turboState === 'turbo', 'Turbo'); }
        if (when === 'exiting') sh.resume();
        else p.click('turboexit');
        await until(() => p.T.turboState === 'live', 'the world back');
        const at = frame(p.M), ref = await controlRun(img, [], at), d = sameFor(p.M, ref, 100);
        ok(d === '' && p.T.initArgs.seed === 4242 && (when === 'exiting' ? at === f0 : at > f0),
           when + ': the world that comes back (' + (when === 'exiting' ? 'lost: the page\'s copy' : 'handed back') + ', frame ' + at +
           ') is the original one, 100 steps bit-identical' + (d ? ': ' + d : ''));
    }
    {
        const p = await bootPage(PAGE_Q);
        const inits0 = p.inits.length, f = deferredFile(SETUP99);
        p.loadFile(f.file);
        f.resolve();
        await until(() => /Setup loaded/.test(p.msg()) || /refused|not/.test(p.msg()), 'the setup');
        ok(p.inits.length === inits0 + 1 && p.T.initArgs.seed === 99 && p.T.initArgs.population === 12 && p.T.W === 800 && p.T.H === 600 &&
           p.T.D === 200 && p.T.runCfg.foodEnergy === 9 && !p.T.WALLS,
           'while the page runs, the same file loads: seed 99, 12 fish, 800 x 600 x 200, foodEnergy 9 ("' + p.msg() + '")');
        ok(p.uncaught.length === 0, 'no exception escaped the page\'s handlers');
    }
}

/* (#4) A Turbo start that throws on the page (the Worker constructor, the
 * first postMessage, the copy of the memory) left main's page "entering"
 * for good: controls locked, no Stop. Each injected failure first makes the
 * held three-edit sequence (as an edit landing in that moment would be
 * held), so the recovery must apply it too; then a second start must work. */
async function pageStartFailures() {
    section('The page: a Turbo start that throws recovers (the Worker, its first message, the memory copy)');
    const injections = [
        ['the Worker constructor throws', (p) => { p.hooks.ctor = () => { edit3(p); throw new Error('injected: no worker'); }; }],
        ['the first postMessage throws', (p) => { p.hooks.post = () => { edit3(p); throw new Error('injected: could not post'); }; }],
        ['the memory copy throws', (p) => {
            const b = HO.buffer(p.M);
            Object.defineProperty(b, 'slice', { configurable: true, value: () => { delete b.slice; edit3(p); throw new RangeError('injected: out of memory'); } });
        }]
    ];
    for (const [what, arm] of injections) {
        const p = await bootPage(PAGE_Q);
        const img = HO.buffer(p.M).slice(0), f0 = frame(p.M), sets0 = p.sets.length, nW = p.workers.length;
        arm(p);
        p.click('turbobtn');
        for (let i = 0; i < 4; i++) await tick();
        const sh = p.workers.length > nW ? p.worker() : null, c = turboControls(p);
        ok(p.T.turboState === 'live' && p.T.turboWorker === null && !c.card && c.button && !c.locked.length && /Turbo could not start/.test(p.msg()),
           what + ': the page is live again, no worker, the Turbo card hidden, its button on, nothing locked ("' + p.msg() + '"; state ' +
           p.T.turboState + (c.locked.length ? ', locked ' + c.locked.join(' ') : '') + ')');
        if (sh) ok(sh.terminated === 1 && sh.onmessage === null && sh.onerror === null,
                   what + ': the worker it had made is terminated once, its handlers detached (' + sh.terminated + ')');
        ok(at5to10(p), what + ': the edits held in that moment apply, in order (' + speeds(p) + ')');
        const ref = await controlRun(img, p.sets.slice(sets0), f0), d = sameFor(p.M, ref, 50);
        ok(d === '', what + ': the page\'s world is its own, untouched but for those edits (50 steps bit-identical)' + (d ? ': ' + d : ''));
        if (p.T.turboState === 'live') {
            const f1 = frame(p.M);
            p.click('turbobtn');
            await until(() => p.T.turboState === 'turbo', 'the second start');
            await until(() => p.worker().fromWorker.some(m => m.type === 'stats' && m.frame >= f1 + 60), 'the second run');
            p.click('turboexit');
            await until(() => p.T.turboState === 'live', 'the second hand-back');
            ok(frame(p.M) > f1 && at5to10(p), what + ': a second Turbo starts and hands back (frame ' + f1 + ' -> ' + frame(p.M) + ')');
        } else ok(false, what + ': a second Turbo start (the page is stuck "' + p.T.turboState + '")');
        ok(p.uncaught.length === 0, what + ': no exception escaped the page\'s handlers' + (p.uncaught.length ? ' (' + p.uncaught[0] + ')' : ''));
    }
}

/* oc_init of doc as it is, on module M: {ok, why, print} */
function directInit(M, doc) {
    const r = HO.withCString(M, doc, ptr => M._oc_init(7, 1920, 1080, 240, 12, ptr));
    return { ok: r === 1, why: r ? '' : HO.cString(M, M._oc_last_error()), print: r ? stepped(M) : '' };
}
const PAGE_Q7 = '?seed=7&pop=12&w=1920&h=1080&walls=0';
/* A cfg document into a running page by one of its three ways in:
 * importWorld (as its callers call it: parseFirst, the top-level repeats),
 * the start-up's URL-import block (?cfg=; seed 7, 12 fish, 1920 x 1080),
 * Load setup (a version-1 file, the document its config). {ok, why} */
async function viaPage(p, how, doc) {
    if (how === 'importWorld') {
        const pf = p.ctx.EvoOceanHandoff.parseFirst(doc);
        const why = p.T.importWorld(7, 12, 240, pf.value, undefined, pf.repeats.filter(r => !r.path.length), [1920, 1080]);
        return { ok: why === '', why };
    }
    if (how === 'url') {
        try { p.T.urlImport(new URLSearchParams('seed=7&pop=12&w=1920&h=1080&cfg=' + encodeURIComponent(doc))); return { ok: true, why: '' }; }
        catch (e) { return { ok: false, why: String(e && e.message || e) }; }
    }
    p.el('err').textContent = '';
    p.loadFile({ name: 'setup.json', text: () => Promise.resolve('{"format":"evo_ocean_setup","seed":7,"population":12,"depth":240,' +
                                                                '"width":1920,"height":1080,"config":' + doc + '}') });
    await until(() => p.msg() !== '', 'the setup\'s answer');
    return { ok: /^Setup loaded/.test(p.msg()), why: p.msg() };
}
const PAGE_WAYS = ['importWorld', 'url', 'setup'];

/* (#5) A number too big for a double (1e400) parses to Infinity, which
 * JSON.stringify wrote as null on the page's way to oc_init:
 * {"maxPopulation":300,"maxPopulation":1e400} went as ...:null, which the
 * engine's whole-number check passes over, and ran with 300 (the other order
 * with the default). Now the infinity goes on as a number, and oc_init
 * refuses it as it does in its own JSON. Through the page's three ways in,
 * on a running page (a refusal changes nothing), against oc_init itself. */
async function pageOverflow() {
    section('The page: a number too big for a double reaches oc_init as a number, and is refused there');
    {
        const p = await bootPage('?seed=7&pop=12&w=1920&h=1080&cfg=' + encodeURIComponent(OVERFLOW_DOCS[0]));
        const sent = p.inits[0] || '';
        ok(/This link's settings were refused/.test(p.failed) && /maxPopulation/.test(p.failed) && /1e400/.test(sent) && !/null/.test(sent),
           'a link with ' + OVERFLOW_DOCS[0] + ' is refused at start-up ("' + (p.failed.split('\n').pop() || 'it started') + '"); oc_init got ' + sent);
    }
    const direct = await mk(1, {}, 300, 300, 0);
    const p = await bootPage(PAGE_Q7);
    /* the controls: valid repeats; and a float key's later occurrence, which oc_init does not read (nor
     * check: which occurrences it checks is the engine's), so the first runs, as the page shows and writes out */
    for (const doc of OVERFLOW_DOCS.concat(['{"maxPopulation":300,"maxPopulation":400}', '{"foodEnergy":5,"foodEnergy":9}',
                                            '{"lightEnabled":true,"lightEnabled":false}', '{"perceptionRadius":100,"perceptionRadius":-1e400}'])) {
        const d = directInit(direct, doc), bad = [];
        if (OVERFLOW_DOCS.includes(doc) && d.ok) bad.push('oc_init accepted it');   /* an overflow is never a value */
        for (const how of PAGE_WAYS) {
            const print0 = fingerprint(p.M), state0 = p.state(), n0 = p.inits.length;
            const r = await viaPage(p, how, doc), sent = p.inits.slice(n0);
            if (sent.some(s => /null/.test(s) || (/e400/.test(doc) && !/1e400/.test(s)))) bad.push(how + ' sent ' + sent.join(' | '));
            if (r.ok !== d.ok) bad.push(how + (r.ok ? ' accepted it' : ' refused it: ' + r.why));
            else if (!d.ok && (fingerprint(p.M) !== print0 || p.state() !== state0)) bad.push(how + ' changed the page');
            else if (d.ok && stepped(p.M) !== d.print) bad.push(how + ' ran another world');
        }
        ok(!bad.length, doc + ': ' + (d.ok ? 'accepted, the same world' : 'refused (' + d.why + ')') + ' by oc_init, importWorld, ?cfg= and a setup file' +
           (bad.length ? ' — ' + bad.join('; ') : ''));
    }
    ok(p.uncaught.length === 0, 'no exception escaped the page\'s handlers');
}

/* (#6) Which values an on / off key takes is the engine's rule, and only
 * the engine's: a SimConfig switch a JSON true / false, an Ocean switch
 * also the number 0 / 1, every occurrence checked (ocean_config.hpp
 * readOceanJson). main's importWorld had a narrower rule of its own (only
 * true / false for every switch), so {"lightEnabled":true,"lightEnabled":0}
 * or {"mouthGate":0,"mouthGate":1}, valid to oc_init, were refused on the
 * page; a version-1 setup took config.walls only as true. Every switch key,
 * every value (alone, after a valid one, before one), through the page's
 * three ways in, against oc_init itself: the same verdict, the same world,
 * the first value in the sheet, true / false in the page's bookkeeping. */
async function pageBools() {
    section('The page: on / off keys take what oc_init takes (SimConfig: true / false; the Ocean\'s: also 0 / 1)');
    const SIM = ['toroidal', 'bodyModel', 'triangularMotion', 'foodDrift', 'predatorEconomy', 'predatorInheritance', 'fastTanh'];
    const OCEAN = ['deepCurrent', 'lightEnabled', 'depthCostScaled', 'sizeDrift', 'sizeFight', 'mouthGate', 'walls'];
    const truth = (t) => t === 'true' || Number(t) === 1;
    /* [doc, the table's verdict, key, its first value's text] */
    const docs = (key, good, bad) => {
        const out = good.map(g => ['{"' + key + '":' + g + '}', true, key, g]);
        for (const b of bad)
            for (const form of [0, 1, 2]) {
                const kb = '"' + key + '":' + b, kg = '"' + key + '":true';
                out.push(['{' + (form === 0 ? kb : form === 1 ? kg + ',' + kb : kb + ',' + kg) + '}', false, key, form === 1 ? 'true' : b]);
            }
        return out;
    };
    const groups = SIM.map(k => [k + ' (SimConfig)', docs(k, ['true', 'false'], ['0', '1', '1.0', '-0', '2', '"true"', 'null'])])
        .concat(OCEAN.map(k => [k + ' (Ocean)', docs(k, ['true', 'false', '0', '1', '1.0', '-0'], ['2', '0.5', '-1', '"true"', 'null'])]))
        .concat([['mixed repeats', [['{"lightEnabled":true,"lightEnabled":0}', true, 'lightEnabled', 'true'],
                                    ['{"mouthGate":0,"mouthGate":1}', true, 'mouthGate', '0'],
                                    ['{"deepCurrent":1,"deepCurrent":false}', true, 'deepCurrent', '1']]],
                 ['walls with wrap-around senses', [['{"walls":true,"toroidal":true}', false, 'walls', 'true'],
                                                    ['{"walls":1,"toroidal":true}', false, 'walls', '1']]]]);
    const direct = await mk(1, {}, 300, 300, 0);
    const p = await bootPage(PAGE_Q7);
    const KEYS = p.KEYS;
    for (const [what, list] of groups) {
        const bad = [];
        let acc = 0, ref = 0;
        for (const [doc, table, key, first] of list) {
            const d = directInit(direct, doc);
            if (d.ok !== table) bad.push('oc_init ' + (d.ok ? 'accepts ' : 'refuses ') + doc + ' (the fixture says otherwise' + (d.why ? ': ' + d.why : '') + ')');
            d.ok ? acc++ : ref++;
            for (const how of PAGE_WAYS) {
                const print0 = fingerprint(p.M), state0 = p.state();
                const r = await viaPage(p, how, doc);
                if (r.ok !== d.ok) { bad.push(how + (r.ok ? ' accepted ' : ' refused ') + doc + (r.ok ? '' : ' (' + r.why + ')')); continue; }
                if (!d.ok) { if (fingerprint(p.M) !== print0 || p.state() !== state0) bad.push(how + ' changed the page on ' + doc); continue; }
                if (stepped(p.M) !== d.print) bad.push(how + ' ran another world for ' + doc);
                if (KEYS[key]) {
                    const want = truth(first), box = p.el('o_' + key).checked, a = p.T.initArgs.cfg[key], b = p.T.runCfg[key];
                    if (box !== want || a !== want || b !== want)
                        bad.push(how + ' ' + doc + ': the sheet ' + box + ', initArgs ' + JSON.stringify(a) + ', runCfg ' + JSON.stringify(b) + ' (want ' + want + ')');
                }
            }
        }
        ok(!bad.length, what + ': ' + list.length + ' documents (' + acc + ' accepted, ' + ref + ' refused by oc_init), the same through importWorld, ' +
           '?cfg= and a setup file' + (bad.length ? ' — ' + bad.length + ' differ: ' + bad.slice(0, 4).join('; ') : ''));
    }
    ok(p.uncaught.length === 0, 'no exception escaped the page\'s handlers');
}

/* Turbo dashboards. While Turbo owned the world, the Brains dashboard
 * showed "Waiting for the world…" (or the wiring from before Turbo) and the
 * readout and stat panel the page's frozen copy. Now the worker's stats
 * messages carry its oc_stats / oc_summary, and it answers the page's
 * wiring questions (asked only while Brains shows, every 2 s at most); after
 * the hand-back (or a lost world) the page's own world is the source again.
 * Through the page and the real dashboards (charts stubbed): the Brains
 * panel opened first during Turbo (an empty cache) and before it (a
 * cache of the page's world), the readout, a Stop and a lost world; and the
 * round trip stays bit-identical to a run without any of this. */
async function waitFor(pred, ms) {
    try { await until(pred, '', ms); return true; } catch (e) { return false; }
}
function hud(p, now) {   /* one frame of the page at time now: the readout and the stat panel */
    p.raf(now);
    const m = /frame ([\d,]+)/.exec(p.el('statlist').innerHTML);
    return { fish: Number(p.el('ro-fish').textContent.replace(/,/g, '')), frame: m ? Number(m[1].replace(/,/g, '')) : NaN,
             notes: p.el('statnotes').textContent };
}
const brainsNote = (p) => { const n = p.el('dgrid').querySelector('.cnote'); return n ? n.textContent : ''; };
async function wiringOf(img, f) {   /* oc_wiring of the world from img, stepped to frame f */
    const ref = await controlRun(img, [], f), nS = ref._oc_num_senses(), nA = ref._oc_num_actions(), n = ref._oc_wiring();
    const data = HO.f32(ref, ref._oc_wiring_data(), 2 * nS * nA);
    return { n, path: data.slice(0, nS * nA), direct: data.slice(nS * nA) };
}
async function pageDashboards() {
    section('The page: the readout and the Brains dashboard show the Turbo world, then the page\'s again');
    let now = 1e6;
    for (const end of ['handed back', 'lost']) {
        const p = await bootPage(PAGE_Q);
        const img = HO.buffer(p.M).slice(0), f0 = frame(p.M), dash = p.ctx.EvoOceanDash;
        p.click('readout');
        if (end === 'lost') {   /* Brains opened before Turbo: its cache holds the page's world */
            dash.showTab('brains');
            await waitFor(() => /^From \d+ fish;/.test(brainsNote(p)), 5000);
        }
        const before = brainsNote(p);
        p.click('turbobtn');
        await until(() => p.T.turboState === 'turbo', 'Turbo');
        const sh = p.worker();
        await until(() => sh.delivered.some(m => m.type === 'stats' && m.frame >= f0 + 300), 'Turbo to run a while');
        if (end !== 'lost') dash.showTab('brains');   /* opened first during Turbo: an empty cache */
        const first = brainsNote(p);
        const turboNote = await waitFor(() => / in the Turbo run/.test(brainsNote(p)), 8000);
        const got = sh.delivered.filter(isType('wiring')), w = got[got.length - 1];
        const shown = Number((/^From (\d+)/.exec(brainsNote(p)) || [])[1]);
        let same = false;
        if (w) {
            const ref = await wiringOf(img, w.frame);
            same = w.frame > f0 && ref.n === w.n && w.path.length === ref.path.length && w.path.every((v, i) => Object.is(v, ref.path[i])) &&
                   w.direct.every((v, i) => Object.is(v, ref.direct[i])) && got.some(m => m.n === shown);
        }
        ok(turboNote && same, end + ': the Brains panel ' + (end === 'lost' ? 'kept "' + before + '", then' : 'said "' + first + '", then') +
           ' shows the worker\'s wiring ("' + brainsNote(p) + '"; ' + got.length + ' asked for, the last at frame ' + (w ? w.frame : '–') +
           ' equal to oc_wiring of that frame\'s world)');
        const asks = sh.toWorker.filter(m => m.cmd === 'wiring').length;
        const stats = sh.delivered.filter(isType('stats')), last = stats[stats.length - 1];
        const h = hud(p, now += 1000);
        ok(h.frame === last.frame && h.frame > f0 && last.st && h.fish === last.st[0] && h.fish === last.pop && /its numbers/.test(h.notes),
           end + ': the readout and stat panel show the worker\'s world: frame ' + h.frame + ' (its latest stats ' + (last && last.frame) + ', Turbo took ' + f0 +
           '), ' + h.fish + ' fish');
        {   /* one question at a time: an answer held up (the channel held 5.5 s, as behind a long chunk) is not asked again */
            const q0 = sh.toWorker.filter(m => m.cmd === 'wiring').length;
            sh.pause();
            await new Promise(r => setTimeout(r, 5500));
            const q1 = sh.toWorker.filter(m => m.cmd === 'wiring').length;
            sh.resume();
            await waitFor(() => / in the Turbo run/.test(brainsNote(p)) && sh.delivered.filter(isType('wiring')).length > got.length, 5000);
            ok(q1 - q0 === 1, end + ': while an answer is held up the page asks once, not every 2 s (' + (q1 - q0) + ' questions in 5.5 s)');
        }
        dash.showTab('pop');   /* Brains hidden: no more questions */
        const asks2 = sh.toWorker.filter(m => m.cmd === 'wiring').length;
        await new Promise(r => setTimeout(r, 2600));
        ok(asks >= 1 && asks <= 3 && sh.toWorker.filter(m => m.cmd === 'wiring').length === asks2,
           end + ': the page asked for the wiring only while Brains showed (' + asks + ' times), not after');
        dash.showTab('brains');
        if (end === 'lost') {
            sh.pause(); trapNext(sh);
            await sh.release(isType('error'), 'the trap');
        } else p.click('turboexit');
        await until(() => p.T.turboState === 'live', 'the world back');
        const at = frame(p.M);
        const own = p.T.wiringNow();
        const back = await waitFor(() => /^From \d+ fish;/.test(brainsNote(p)), 5000);
        ok(back && own && !own.turbo && (end !== 'lost' || at === f0),
           end + ': back on the page, the Brains panel reads its own world again ("' + brainsNote(p) + '")');
        const h2 = hud(p, now += 1000);
        ok(h2.frame === frame(p.M) && !/Turbo/.test(h2.notes), end + ': and the readout too (frame ' + h2.frame + ')');
        if (end !== 'lost') {
            const ref = await controlRun(img, [], frame(p.M)), d = sameFor(p.M, ref, 150);
            ok(d === '', 'the round trip with the dashboards reading the worker is bit-identical to the run without (150 steps)' + (d ? ': ' + d : ''));
        }
        dash.close();
        ok(p.uncaught.length === 0 && !p.logs.some(l => l[0] === 'error'), end + ': no exception escaped the page, no console error');
    }
}

/* Deployed-audit PR #30-31, finding 2: ?walls=0 (or a version-2 setup's
 * "walls": false) deleted the cfg's own "walls", so a later "walls" in the
 * same cfg became the first occurrence and the one oc_init ran:
 * ?walls=0&cfg={"walls":false,"walls":true} ran glass walls under an
 * unchecked switch, with runCfg missing walls. walls= now writes the first
 * occurrence; the later ones still go to oc_init to be checked. Through the
 * link and a setup file, then Replay, Save setup and loading that file. */
async function pageWallsOverride() {
    section('The page: walls=0 and a setup\'s "walls": false win over a later "walls" in the cfg (deployed audit #30-31, finding 2)');
    const DUP = '{"walls":false,"walls":true}';
    const sides = (p) => ({ engine: p.M._oc_walls_enabled(), page: p.T.WALLS, box: p.el('o_walls').checked, run: p.T.runCfg.walls,
                            init: p.T.initArgs.cfg.walls });
    const off = (s) => s.engine === 0 && s.page === false && s.box === false && s.run === false && s.init === false;
    const show = (s) => JSON.stringify(s);
    const v2 = (walls, config) => '{"format":"evo_ocean_setup","version":2,"seed":7,"population":12,"depth":240,"width":1920,' +
                                  '"height":1080,"walls":' + walls + ',"config":' + config + '}';
    const load = async (p, text) => {
        p.el('err').textContent = '';
        p.loadFile({ name: 'setup.json', text: () => Promise.resolve(text) });
        await until(() => p.msg() !== '', 'the setup\'s answer');
        return p.msg();
    };
    for (const how of ['link', 'setup']) {
        const p = await bootPage(how === 'link' ? '?seed=7&pop=12&w=1920&h=1080&walls=0&cfg=' + encodeURIComponent(DUP) : PAGE_Q7);
        const said = how === 'link' ? p.failed : await load(p, v2('false', DUP));
        const s1 = sides(p), sent = p.inits[p.inits.length - 1];
        ok(!/refused|can't/.test(said) && off(s1) && /^\{"walls":false,.*"walls":true\}$/.test(sent),
           how + ': ' + (how === 'link' ? '?walls=0&cfg=' + DUP : 'a version-2 setup, walls false, config ' + DUP) +
           ' runs without walls, the switch off, runCfg / initArgs walls false (' + show(s1) + '; oc_init got ' + sent + ')');
        p.click('replay');
        const s2 = sides(p);
        ok(off(s2), how + ': Replay keeps it without walls (' + show(s2) + ')');
        p.click('savecp');
        const saved = JSON.parse(p.blobs[p.blobs.length - 1] || '{}');
        ok(saved.walls === false && !('walls' in (saved.config || {})), how + ': Save setup writes walls false (' + JSON.stringify({ walls: saved.walls, config: saved.config }) + ')');
        await load(p, p.blobs[p.blobs.length - 1]);
        const s3 = sides(p);
        ok(off(s3) && /^Setup loaded/.test(p.msg()), how + ': and that file loads without walls (' + show(s3) + ')');
    }
    {   /* the other way, and a later occurrence that is not a switch: still checked, still refused */
        const p = await bootPage(PAGE_Q7);
        await load(p, v2('true', '{"walls":true,"walls":false}'));
        const on = sides(p);
        ok(on.engine === 1 && on.box === true && on.run === true, 'a version-2 setup, walls true, config {"walls":true,"walls":false}: glass walls (' + show(on) + ')');
        const why = await load(p, v2('false', '{"walls":false,"walls":2}'));
        ok(/refused/.test(why) && /walls/.test(why) && sides(p).engine === 1, 'walls false over a later "walls": 2 is refused by oc_init, the world kept ("' + why + '")');
        let refused = '';
        try { p.T.urlImport(new URLSearchParams('seed=7&pop=12&w=1920&h=1080&walls=0&cfg=' + encodeURIComponent('{"walls":false,"walls":"x"}'))); }
        catch (e) { refused = String(e.message); }
        ok(/walls/.test(refused), 'and so is ?walls=0&cfg={"walls":false,"walls":"x"} ("' + refused + '")');
        ok(p.uncaught.length === 0, 'no exception escaped the page\'s handlers');
    }
}

/* Deployed-audit PR #30-31, finding 3: a link from before walls keeps
 * wrapping, a new visit gets glass walls. The page took a link with only
 * ?speed=2 (an Ocean key since before walls) for a new visit. The keys that
 * make a link the Ocean's are ocean.html's OCEAN_ONLY (its redirect's list),
 * and web/ocean.js uses that same list. */
async function pageFreshVisit() {
    section('The page: glass walls for a new visit, wrapping for a link from before them (deployed audit #30-31, finding 3)');
    const list = (src, name) => { const m = new RegExp('var ' + name + ' = (\\[[^\\]]*\\])').exec(src); return m ? JSON.parse(m[1]) : null; };
    const html = list(fs.readFileSync(path.join(WEB, 'ocean.html'), 'utf8'), 'OCEAN_ONLY');
    const js = list(fs.readFileSync(path.join(WEB, 'ocean.js'), 'utf8'), 'OCEAN_KEYS');
    ok(html && js && JSON.stringify(html) === JSON.stringify(js) && js.includes('speed'),
       'web/ocean.js\'s link keys are ocean.html\'s OCEAN_ONLY (' + JSON.stringify(js) + ')');
    for (const [q, want] of [['', 1], ['?seed=4242', 0], ['?speed=2', 0], ['?speed=2&colour=family', 0], ['?e1=1', 0],
                             ['?colour=family', 1], ['?guide=0', 1], ['?clarity=realistic', 1], ['?seed=4242&walls=1', 1]]) {
        const p = await bootPage(q);
        const got = p.failed ? 'refused: ' + p.failed : p.M._oc_walls_enabled();
        ok(got === want && p.el('o_walls').checked === !!want,
           (q || 'a bare address') + ': ' + (want ? 'glass walls' : 'wrapping, as before walls') + ' (walls ' + got + ')');
    }
}

/* Deployed-audit PR #30-31, finding 4: the sheet's oc_init arguments were
 * read rounded (getVal: int keys), so Restart with a tank width of 99.6 ran
 * width 100 under a field showing 99.6 (3840.5 ran 3841), where a link or a
 * setup with those numbers is refused. Now they are read as typed and
 * checked as such: refused with the reason, nothing changed; whole numbers
 * in range (and a fractional depth, which oc_init takes) run as shown. */
async function pageSizeFields() {
    section('The page: Restart checks the tank size, depth, fish and seed as typed (deployed audit #30-31, finding 4)');
    const p = await bootPage(PAGE_Q7);
    const bad = [];
    for (const [key, typed] of [['width', '99.6'], ['width', '3840.5'], ['height', '1080.25'], ['population', '12.5'], ['seed', '1.5'],
                                ['seed', '-1'], ['seed', '4294967296'], ['width', ''], ['depth', '9.5']]) {
        const was = p.el('o_' + key).value, print0 = fingerprint(p.M), n0 = p.inits.length, W0 = p.T.W;
        p.edit(key, typed);
        p.click('restart');
        const shown = p.el('o_' + key).value;
        if (p.inits.length !== n0 || fingerprint(p.M) !== print0 || p.T.W !== W0 || shown !== typed || !/Couldn't start that world/.test(p.msg()))
            bad.push(key + ' ' + JSON.stringify(typed) + ': ' + (p.inits.length !== n0 ? 'oc_init ran (width ' + p.T.W + ')' : 'shown ' + shown) + ' — "' + p.msg() + '"');
        p.edit(key, was);
    }
    ok(!bad.length, 'Restart refuses a tank size, fish count or seed that is not a whole number in range, as typed (99.6, 3840.5, ' +
       '1080.25, 12.5 fish, seed 1.5 / -1 / 2^32, an empty width, depth 9.5): no oc_init, the field as typed' + (bad.length ? ' — ' + bad.join('; ') : ''));
    p.edit('width', '3840'); p.edit('depth', '237.5');
    p.click('restart');
    ok(p.T.W === 3840 && p.T.D === 237.5 && /[?&]w=3840/.test(p.loc.href) && p.T.initArgs.depth === 237.5,
       'whole numbers in range run as shown: width 3840, depth 237.5 (a depth need not be whole) (' + p.T.W + ' x ' + p.T.H + ' x ' + p.T.D + ')');
    ok(p.uncaught.length === 0, 'no exception escaped the page\'s handlers');
}

/* PR #34 review: what the page writes out (Save setup's file, the address
 * a load or Restart writes) must start the world it ran. perceptionRadius
 * -1e400 (now reaching oc_init as a number) was raised to 1 by the
 * engine's clamp before its finite check, so it ran radius 1, while the
 * page kept -Infinity and wrote it out as null: reloaded, the default 100.
 * Now the clamp raises only a finite radius and oc_init refuses -inf. Here:
 * the review's file (refused, nothing changed); every key the engine reads
 * as a number at +-1e400 is refused or not read at all (so no accepted
 * setup holds a non-finite value that matters); and accepted setups,
 * clamped and repeated values among them, come back as the same world and
 * sheet from Save setup's file and from their address on a new page. */
async function pageRoundTrip() {
    section('The page: Save setup\'s file and the address start the world that ran (PR #34 review)');
    const setup = (config) => '{"format":"evo_ocean_setup","version":1,"seed":7,"population":12,"width":300,"height":300,"depth":240,' +
                              '"config":' + config + '}';
    const load = async (p, text) => {
        p.el('err').textContent = '';
        p.loadFile({ name: 'setup.json', text: () => Promise.resolve(text) });
        await until(() => p.msg() !== '', 'the setup\'s answer');
        return p.msg();
    };
    const REVIEW = '{"perceptionRadius":-1e400}';
    {   /* every number key the engine's readers know (SimConfig's and the Ocean's) */
        const keys = [...new Set(['src/core/checkpoint_io.hpp', 'src/ocean/ocean_config.hpp'].flatMap(f =>
            [...fs.readFileSync(path.join(ROOT, f), 'utf8').matchAll(/(?:floatOr|numOr|intOr|doubleOr|readFloat|readNum)\("([A-Za-z0-9_]+)"/g)].map(m => m[1])))];
        const M = await mk(1, {}, 300, 300, 0), base = directInit(M, '{}').print, bad = [];
        let refused = 0, unread = 0;
        for (const k of keys)
            for (const v of ['1e400', '-1e400']) {
                const r = directInit(M, '{"' + k + '":' + v + '}');
                if (!r.ok) refused++;
                else if (r.print === base) unread++;
                else bad.push(k + '=' + v);
            }
        ok(keys.length > 60 && !bad.length, keys.length + ' number keys the engine reads, at +-1e400: ' + refused + ' refused, ' + unread +
           ' accepted only where oc_init does not read the key (the world is {}\'s)' + (bad.length ? ' — accepted and run: ' + bad.join(' ') : ''));
    }
    const p = await bootPage(PAGE_Q7);
    {
        const print0 = fingerprint(p.M), state0 = p.state();
        const why = await load(p, setup(REVIEW));
        ok(/refused/.test(why) && /perceptionRadius/.test(why) && fingerprint(p.M) === print0 && p.state() === state0,
           'the review\'s setup (version 1, 300 x 300, perceptionRadius -1e400) is refused, the world, sheet and address kept ("' + why + '")');
    }
    const probe = await mk(1, {}, 300, 300, 0), e1 = HO.cString(probe, probe._oc_e1_preset());
    for (const config of ['{}', '{"perceptionRadius":0}', '{"perceptionRadius":0.5,"interactRadius":0}', '{"perceptionRadius":-5}',
                          '{"perceptionRadius":120}', '{"foodEnergy":5,"foodEnergy":9}', '{"lightEnabled":true,"lightEnabled":0}',
                          '{"mouthGate":1,"kMouth":1.5}', '{"walls":1}', '{"bodyLen":1e400}', '{"perceptionRadius":100,"perceptionRadius":-1e400}',
                          e1, REVIEW]) {
        const said = await load(p, setup(config));
        if (!/^Setup loaded/.test(said)) {
            ok(config === REVIEW, config + ': ' + (config === REVIEW ? 'refused (above)' : 'not loaded: "' + said + '"'));
            continue;
        }
        const print1 = stepped(p.M), form1 = p.form(), href = p.loc.href;
        p.click('savecp');
        const said2 = await load(p, p.blobs[p.blobs.length - 1]);
        const print2 = stepped(p.M), form2 = p.form();
        const fresh = await bootPage(new URL(href).search);
        const print3 = fresh.failed ? 'refused: ' + fresh.failed : stepped(fresh.M), form3 = fresh.failed ? '' : fresh.form();
        ok(/^Setup loaded/.test(said2) && print2 === print1 && form2 === form1 && print3 === print1 && form3 === form1,
           config.slice(0, 60) + (config.length > 60 ? '…' : '') + ': Save setup -> load, and its address -> a new page: the same world and sheet' +
           (print2 !== print1 ? ' — the saved file runs another world' : '') + (form2 !== form1 ? ' — the saved file shows another sheet' : '') +
           (print3 !== print1 ? ' — the address runs another world (' + print3.slice(0, 40) + ')' : '') + (form3 !== form1 && print3 === print1 ? ' — the address shows another sheet' : ''));
    }
    ok(p.uncaught.length === 0, 'no exception escaped the page\'s handlers');
}

/* Every section, in order; `node tests/ocean_turbo_handoff.js NAME...` runs only those. */
const SECTIONS = {
    repeatedKeys, liveLedger, growthPage, randomize, imports,
    roundTripDefault: () => roundTrip('the default world', {}),
    roundTripE1: async () => {
        const probe = await mk(1);
        const e1 = JSON.parse(HO.cString(probe, probe._oc_e1_preset()));
        await roundTrip('E1 physics, 3 predators with their economy, patchy food',
                        Object.assign({}, e1, { predatorCount: 3, predatorEconomy: true, predatorMeal: 7, predatorMaxPopulation: 12,
                                                foodPatches: 5, foodDepthMode: 1, bodyModel: true }));
    },
    failures: failures_,
    pageHeld, pageRefusal, pageAsyncSetup, pageStartFailures, pageOverflow, pageBools, pageDashboards,
    pageWallsOverride, pageFreshVisit, pageSizeFields, pageRoundTrip
};
const unhandled = [];
process.on('unhandledRejection', (e) => { unhandled.push(e); });
(async () => {
    const t0 = Date.now(), only = process.argv.slice(2);
    const bad = only.filter(n => !SECTIONS[n]);
    if (bad.length) throw new Error('no section ' + bad.join(', ') + ' (sections: ' + Object.keys(SECTIONS).join(', ') + ')');
    for (const name of Object.keys(SECTIONS)) if (!only.length || only.includes(name)) await SECTIONS[name]();
    ok(unhandled.length === 0, 'no unhandled promise rejection' + (unhandled.length ? ': ' + unhandled[0] : ''));
    console.log('\n' + (checks - failures) + ' / ' + checks + ' checks passed in ' + ((Date.now() - t0) / 1000).toFixed(1) + ' s');
    process.exit(failures ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(2); });
