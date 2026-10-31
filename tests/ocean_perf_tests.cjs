// tests/ocean_perf_tests.cjs: the render profiler's state machines (web/ocean_perf.js), in node.
//   node tests/ocean_perf_tests.cjs
// The CPU ring (no clock read when off, raw rAF intervals, overflow), the
// summaries, and the GPU timer (EXT_disjoint_timer_query_webgl2) against a fake
// GL: an absent extension, delayed availability, a full pool, a disjoint event,
// context loss and restoration, a refused nested query, an aborted submission,
// ns -> ms without 32-bit truncation. No browser, no wasm.
"use strict";
const path = require("path");
const Perf = require(path.join(__dirname, "..", "web", "ocean_perf.js"));

let fails = 0, checks = 0;
function ok(cond, what, info) {
  checks++;
  if (!cond) { fails++; console.log("  FAIL " + what + (info !== undefined ? "  " + JSON.stringify(info) : "")); }
  else console.log("  ok   " + what);
}
function section(name) { console.log("== " + name); }

/* a clock that counts its readings */
function clock() {
  const c = { t: 1000, reads: 0 };
  c.now = () => { c.reads++; return c.t; };
  return c;
}

/* a fake WebGL2 context with EXT_disjoint_timer_query_webgl2 */
function fakeGL(o = {}) {
  const AV = 0x8867, RES = 0x8866;
  const gl = {
    QUERY_RESULT_AVAILABLE: AV, QUERY_RESULT: RES,
    polls: 0, latency: o.latency === undefined ? 1 : o.latency, gen: 0, lost: false, disjoint: false,
    active: null, created: 0, extCalls: 0, noExt: !!o.noExt, nsFor: o.nsFor || ((q) => 1e6 * q.id),
    canvas: new EventTarget(), ext: null, stale: 0, earlyReads: 0,
    getExtension(name) {
      gl.extCalls++;
      if (name !== "EXT_disjoint_timer_query_webgl2" || gl.noExt || gl.lost) return null;
      gl.ext = { TIME_ELAPSED_EXT: 0x88bf, GPU_DISJOINT_EXT: 0x8fbb, gen: gl.gen };
      return gl.ext;
    },
    isContextLost() { return gl.lost; },
    createQuery() { gl.created++; return { id: gl.created, gen: gl.gen, readyAt: Infinity, ns: 0 }; },
    beginQuery(target, q) {
      if (gl.active) throw new Error("nested beginQuery");
      if (q.gen !== gl.gen) { gl.stale++; throw new Error("a query of a lost context"); }
      gl.active = q;
    },
    endQuery() {
      if (!gl.active) throw new Error("endQuery without beginQuery");
      gl.active.readyAt = gl.polls + gl.latency; gl.active.ns = gl.nsFor(gl.active); gl.active = null;
    },
    getQueryParameter(q, p) {
      if (q.gen !== gl.gen) { gl.stale++; return null; }
      if (p === AV) return gl.polls >= q.readyAt;
      if (p === RES) { if (!(gl.polls >= q.readyAt)) { gl.earlyReads++; throw new Error("QUERY_RESULT before QUERY_RESULT_AVAILABLE"); } return q.ns; }
      return null;
    },
    getParameter(p) { if (p === 0x8fbb) { const d = gl.disjoint; gl.disjoint = false; return d; } return null; },
    lose() { gl.lost = true; gl.gen++; gl.canvas.dispatchEvent(new Event("webglcontextlost")); },
    restore() { gl.lost = false; gl.canvas.dispatchEvent(new Event("webglcontextrestored")); }
  };
  return gl;
}

/* ---------------- the disabled profiler ---------------- */
section("disabled (the page's default)");
{
  const D = Perf.disabled, c = clock();
  D.beginFrame(1, 1, "live", false); D.recordStep(3, 1);
  const tok = D.beginCpu(Perf.SPAN.sync);
  ok(tok === 0 && D.lap(Perf.SPAN.sync, tok) === 0, "beginCpu / lap return 0");
  D.endCpu(Perf.SPAN.sync, tok); D.count(Perf.K.simN, 5); D.beginSubmission(); D.endSubmission(true); D.endFrame(5, 1, false);
  ok(D.active === false && D.enabled === false && D.drain() === null, "records nothing, drains nothing");
  ok(D.suspend(() => 42) === 42, "suspend runs its function");
  ok(Perf.forPage(false) === D && Perf.forPage(true).enabled === true, "forPage: disabled unless asked");
  ok(c.reads === 0, "no clock involved");
}

/* ---------------- CPU spans ---------------- */
section("CPU: no clock read when off; frame-only reads two a frame");
{
  const c = clock(), P = Perf.create({ now: c.now });
  P.configure({ cpu: false, gpu: "off" });
  for (let f = 0; f < 5; f++) {
    P.beginFrame(f * 16, 1, "live", false);
    let t = P.beginCpu(); t = P.lap(Perf.SPAN.sync, t); P.endCpu(Perf.SPAN.hud, t);
    P.endFrame(10, f, false);
  }
  ok(c.reads === 0, "cpu off, gpu off: zero clock reads", c.reads);
  ok(P.active === false, "not active");
  P.configure({ cpu: "frame" });
  for (let f = 0; f < 5; f++) {
    P.beginFrame(f * 16, 1, "live", false);
    let t = P.beginCpu(); ok(t === 0, "frame-only: span token is 0 (frame " + f + ")"); t = P.lap(Perf.SPAN.sync, t); P.endCpu(Perf.SPAN.hud, t);
    P.endFrame(10, f, false);
  }
  ok(c.reads === 10, "frame-only: two clock reads a frame", c.reads);
}

section("CPU: spans, laps, repeats, raw rAF intervals before the clamp");
{
  const c = clock(), P = Perf.create({ now: c.now });
  P.configure({ cpu: true });
  P.beginFrame(1000, 7, "live", false);
  P.recordStep(4.5, 2);
  let t = P.beginCpu(); c.t += 1.25; t = P.lap(Perf.SPAN.sync, t); c.t += 0.5; t = P.lap(Perf.SPAN.snapshot, t);
  let u = P.beginCpu(); c.t += 0.25; P.endCpu(Perf.SPAN.snapshot, u);   /* a second snapshot: adds */
  P.count(Perf.K.calls, 9); P.add(Perf.K.geneUploads, 1); P.add(Perf.K.geneUploads, 1);
  c.t += 2; P.endFrame(300, 41, false);
  P.beginFrame(1250, 7, "turbo", true);   /* a 250 ms stall: kept raw */
  P.endFrame(300, 41, true);
  const cap = P.drain(), NS = Perf.SPANS.length, NK = Perf.COUNTERS.length;
  ok(cap.frames === 2 && cap.valid && !cap.overflow, "two frames, valid");
  ok(Number.isNaN(cap.raw[0]) && cap.raw[1] === 250, "raw rAF interval 250 ms, unclamped", Array.from(cap.raw));
  ok(cap.cpu[Perf.SPAN.step] === 4.5, "step: the page's own timing");
  ok(cap.cpu[Perf.SPAN.sync] === 1.25, "sync 1.25 ms");
  ok(cap.cpu[Perf.SPAN.snapshot] === 0.75, "snapshot 0.5 + 0.25 ms (repeats add)");
  ok(Number.isNaN(cap.cpu[Perf.SPAN.submit]), "a span that did not run: NaN, not 0");
  ok(cap.cpu[Perf.SPAN.frameCpu] === 4, "frameCpu: the whole callback", cap.cpu[Perf.SPAN.frameCpu]);
  ok(cap.cnt[Perf.K.calls] === 9 && cap.cnt[Perf.K.geneUploads] === 2 && cap.cnt[Perf.K.stepsReq] === 2, "counters: set and add");
  ok(cap.cnt[Perf.K.epoch] === 7 && cap.cnt[Perf.K.tick] === 41 && cap.cnt[Perf.K.simN] === 300, "metadata: epoch, tick, n");
  ok(cap.cnt[NK + Perf.K.turbo] === 2 && cap.cnt[NK + Perf.K.paused] === 1 && cap.cnt[NK + Perf.K.hidden] === 1, "frame 2: turbo, paused, hidden");
  ok(cap.cnt[Perf.K.submitted] === 0, "no submission: submitted 0");
  ok(NS === 15, "fifteen spans");
}

section("CPU: the ring overflows explicitly");
{
  const c = clock(), P = Perf.create({ now: c.now, frames: 8 });
  P.configure({ cpu: true });
  for (let f = 0; f < 11; f++) { P.beginFrame(100 * f, 1, "live", false); P.endFrame(f, f, false); }
  const cap = P.drain();
  ok(cap.overflow === true && cap.valid === false, "overflow: the capture is invalid");
  ok(cap.frames === 8 && cap.recorded === 11, "the last 8 of 11 kept");
  ok(cap.ts[0] === 300 && cap.ts[7] === 1000, "in time order", Array.from(cap.ts));
  P.resetCapture();
  ok(P.drain().frames === 0 && P.drain().valid === true, "resetCapture starts over");
}

section("CPU: an unfinished frame is closed by the next");
{
  const c = clock(), P = Perf.create({ now: c.now });
  P.configure({ cpu: true });
  P.beginFrame(0, 1, "live", false);   /* never ended: the frame threw */
  P.beginFrame(16, 1, "live", false); P.endFrame(1, 1, false);
  ok(P.drain().frames === 2, "both frames kept");
}

section("summaries and export");
{
  const c = clock(), P = Perf.create({ now: c.now });
  P.configure({ cpu: true });
  for (let f = 0; f <= 100; f++) {
    P.beginFrame(1000 + f * 20, 1, "live", false);
    let t = P.beginCpu(); c.t += f === 50 ? 9 : 1; P.lap(Perf.SPAN.sync, t);
    P.beginSubmission(); P.endSubmission(f % 2 === 0);
    if (f === 10) P.markDiagnostic();
    P.endFrame(500, f, false);
  }
  const cap = P.drain(), s = Perf.summarize(cap);
  ok(s.frames === 101 && s.diagnosticFrames === 1, "101 frames, 1 diagnostic");
  ok(Math.abs(s.wallMs - 2000) < 1e-9, "wall 2 s");
  ok(Math.abs(s.fps - 25) < 1e-9, "fps: 50 successful submissions / 2 s = 25", s.fps);
  ok(Math.abs(s.rafFps - 50) < 1e-9 && s.raf.p50 === 20, "rAF cadence 50 Hz, p50 20 ms");
  ok(s.spans.sync.n === 100 && s.spans.sync.max === 9 && s.spans.sync.p50 === 1, "sync percentiles, the diagnostic frame left out");
  ok(Math.abs(s.stepsPerSec - 50) < 1e-9, "steps/s from ticks over wall time", s.stepsPerSec);
  const ex = Perf.exportCapture(cap), lines = ex.framesCsv.trim().split("\n");
  ok(lines.length === 102 && lines[0].split(",").length === 3 + Perf.SPANS.length + Perf.COUNTERS.length, "frames.csv: header + 101 rows, every column");
  ok(lines[1].split(",")[2] === "", "an absent value is an empty cell, never 0");
  const q = Perf.quantiles([5, 1, 4, 2, 3]);
  ok(q.p50 === 3 && q.max === 5 && q.mean === 3, "quantiles");
}

/* ---------------- GPU timer ---------------- */
section("GPU: the extension absent");
{
  const gl = fakeGL({ noExt: true }), T = new Perf.GpuTimer(gl);
  ok(!T.supported(), "unsupported");
  ok(T.begin(1, 0) === false && T.st.unsupported === 1, "begin refuses, counted");
  T.end(); T.poll();
  ok(T.results().n === 0, "no result, never a 0 ms one");
  const c = clock(), P = Perf.create({ now: c.now });
  P.attachGL(gl); P.configure({ cpu: true, gpu: "frame", sampleEvery: 1 });
  P.beginFrame(0, 1, "live", false); P.beginSubmission(); P.endSubmission(true); P.endFrame(1, 1, false);
  const s = Perf.summarize(P.drain());
  ok(s.gpu.supported === false && s.gpu.stats.unsupported === 1 && Object.keys(s.gpu.labels).length === 0 && s.gpu.validFraction === 0,
     "profiler: GPU unavailable is a status, no samples", s.gpu);
}

section("GPU: delayed availability, read only when available, ns -> ms untruncated");
{
  const gl = fakeGL({ latency: 3, nsFor: (q) => 5e9 + 123 * q.id }), T = new Perf.GpuTimer(gl);
  ok(T.begin(10, 0), "begin"); T.end();
  for (let i = 0; i < 2; i++) { gl.polls++; T.poll(); }
  ok(T.results().n === 0 && T.out.length === 1, "not available after 2 polls: still outstanding");
  gl.polls++; T.poll();
  const r = T.results();
  ok(r.n === 1 && r.status[0] === 0 && r.frame[0] === 10, "available on the 3rd poll");
  ok(Math.abs(r.ms[0] - 5000.000123) < 1e-9, "5e9 + 123 ns = 5000.000123 ms (no 32-bit truncation)", r.ms[0]);
  ok(gl.earlyReads === 0, "QUERY_RESULT never read before available");
  ok(T.pool.length === 1, "the query returns to the pool");
}

section("GPU: a full pool skips the sample, never waits");
{
  const gl = fakeGL({ latency: 100 }), T = new Perf.GpuTimer(gl, { maxQueries: 4 });
  for (let i = 0; i < 6; i++) { if (T.begin(i, 0)) T.end(); }
  ok(T.st.issued === 4 && T.st.skippedPool === 2 && gl.created === 4, "4 issued, 2 skipped, 4 created");
  gl.polls += 100; T.poll();
  ok(T.results().n === 4 && T.begin(7, 0), "after the pool drains, sampling resumes"); T.end();
  ok(gl.created === 4, "pooled queries reused");
}

section("GPU: a disjoint event discards everything outstanding");
{
  const gl = fakeGL({ latency: 1 }), T = new Perf.GpuTimer(gl);
  for (let i = 0; i < 3; i++) { T.begin(i, 0); T.end(); }
  gl.polls++; gl.disjoint = true;
  T.poll();
  const r = T.results();
  ok(r.n === 3 && Array.from(r.status).every((s) => s === 1) && r.ms.every((m) => Number.isNaN(m)), "3 discarded as disjoint, no time", Array.from(r.status));
  ok(T.st.ok === 0 && T.st.disjointEvents === 1 && T.st.discardedDisjoint === 3, "none accepted");
  T.begin(9, 0); T.end(); gl.polls++; T.poll();
  ok(T.results().status[3] === 0, "the next one is valid again");
}

section("GPU: a nested query is refused");
{
  const gl = fakeGL(), T = new Perf.GpuTimer(gl);
  ok(T.begin(1, 0) === true && T.begin(1, 1) === false && T.st.refusedNest === 1, "the second begin is refused");
  T.end(); gl.polls++; T.poll();
  ok(T.results().n === 1 && T.results().status[0] === 0, "the outer one completes");
}

section("GPU: a submission that throws invalidates its query");
{
  const gl = fakeGL(), c = clock(), P = Perf.create({ now: c.now });
  P.attachGL(gl); P.configure({ cpu: true, gpu: "frame", sampleEvery: 1 });
  P.beginFrame(0, 1, "live", false); P.beginSubmission();
  try { try { throw new Error("render failed"); } finally { P.endSubmission(false); } } catch (e) { /* the page sees it */ }
  P.endFrame(1, 1, false);
  gl.polls++; P.beginFrame(16, 1, "live", false); P.endFrame(1, 1, false);
  const cap = P.drain(), g = cap.gpu;
  ok(g.results.n === 1 && g.results.status[0] === 2 && g.stats.aborted === 1, "status aborted, never a time");
  ok(cap.cnt[Perf.K.submitted] === 0, "submitted 0 for that frame");
  ok(gl.active === null, "no query left open");
}

section("GPU: context loss and restoration");
{
  const gl = fakeGL({ latency: 5 }), c = clock(), P = Perf.create({ now: c.now });
  P.attachGL(gl); P.configure({ cpu: true, gpu: "frame", sampleEvery: 1 });
  for (let f = 0; f < 3; f++) { P.beginFrame(f * 16, 1, "live", false); P.beginSubmission(); P.endSubmission(true); P.endFrame(1, f, false); }
  const ext0 = gl.ext;
  gl.lose();
  ok(P.pending() === 0, "outstanding queries dropped on loss");
  ok(P.capabilities().gpuTimer === false && P.capabilities().timerGeneration === 1, "unsupported while lost; generation 1");
  P.beginFrame(48, 1, "live", false); P.beginSubmission(); P.endSubmission(true); P.endFrame(1, 3, false);   /* while lost */
  gl.restore();
  ok(gl.ext !== ext0 && P.capabilities().gpuTimer === true, "the extension re-acquired after restoration");
  const created0 = gl.created;
  for (let f = 4; f < 7; f++) { gl.polls++; P.beginFrame(f * 16, 1, "live", false); P.beginSubmission(); P.endSubmission(true); P.endFrame(1, f, false); }
  gl.polls += 10; P.beginFrame(200, 1, "live", false); P.endFrame(1, 8, false);
  const cap = P.drain(), r = cap.gpu.results, lostN = Array.from(r.status).filter((s) => s === 3).length;
  ok(lostN === 3 && cap.gpu.stats.discardedLost === 3, "the 3 of the lost context: status lost", Array.from(r.status));
  ok(gl.created === created0 + 3, "fresh queries after restoration (none reused)");
  ok(gl.stale === 0, "no handle of the lost context touched");
  const okN = Array.from(r.status).filter((s) => s === 0).length, gens = new Set(Array.from(r.generation).filter((g, i) => r.status[i] === 0));
  ok(okN === 3 && gens.size === 1 && gens.has(1), "3 valid samples, all of generation 1");
}

section("GPU: one loss reported twice (the poll, then the event) is one generation");
{
  const gl = fakeGL({ latency: 5 }), c = clock(), P = Perf.create({ now: c.now });
  P.attachGL(gl); P.configure({ cpu: true, gpu: "frame", sampleEvery: 1 });
  P.beginFrame(0, 1, "live", false); P.beginSubmission(); P.endSubmission(true); P.endFrame(1, 0, false);
  gl.lost = true; gl.gen++;   /* lost, the event not yet delivered */
  P.beginFrame(16, 1, "live", false); P.endFrame(1, 1, false);   /* the poll sees isContextLost() */
  gl.canvas.dispatchEvent(new Event("webglcontextlost"));
  ok(P.capabilities().timerGeneration === 1, "generation 1, not 2", P.capabilities().timerGeneration);
  gl.restore(); gl.canvas.dispatchEvent(new Event("webglcontextrestored"));   /* a duplicate restore is ignored too */
  ok(P.capabilities().gpuTimer === true && P.drain().gpu.stats.discardedLost === 1, "restored once; the lost query counted once");
}

section("GPU: a late result never crosses into the next capture");
{
  const gl = fakeGL({ latency: 10 }), c = clock(), P = Perf.create({ now: c.now });
  P.attachGL(gl); P.configure({ cpu: true, gpu: "frame", sampleEvery: 1 });
  /* capture A: one query, still pending when it is drained (the drain timed out) */
  P.beginFrame(0, 1, "live", false); P.beginSubmission(); P.endSubmission(true); P.endFrame(1, 0, false);
  P.stopSampling();
  const A = P.drain();
  ok(A.gpu.complete === false && A.gpu.pending === 1 && Perf.summarize(A).gpu.complete === false, "A: drained with a query outstanding: reported incomplete", A.gpu);
  /* capture B starts before A's query completes */
  P.resetCapture(); P.configure({ cpu: true, gpu: "frame", sampleEvery: 1 });
  for (let f = 0; f < 3; f++) { gl.polls += 4; P.beginFrame(100 + f * 16, 1, "live", false); P.beginSubmission(); P.endSubmission(true); P.endFrame(1, f, false); }
  gl.polls += 20; P.beginFrame(200, 1, "live", false); P.endFrame(1, 3, false);
  const B = P.drain(), st = B.gpu.stats;
  ok(st.issued === 3 && st.ok === 3 && B.gpu.results.n === 3, "B: its own 3 queries, no more", st);
  ok(st.droppedEarlier === 1, "A's late result dropped, not recorded in B");
  ok(Array.from(B.gpu.results.frame).every((f) => f < 4), "B's results are B's frames", Array.from(B.gpu.results.frame));
  ok(B.gpu.complete === true, "B complete");
}

section("GPU: sampling and draw scopes");
{
  const gl = fakeGL(), c = clock(), P = Perf.create({ now: c.now });
  P.attachGL(gl); P.configure({ cpu: true, gpu: "frame", sampleEvery: 3 });
  for (let f = 0; f < 9; f++) { gl.polls++; P.beginFrame(f * 16, 1, "live", false); P.beginSubmission(); P.endSubmission(true); P.endFrame(1, f, false); }
  gl.polls += 5; P.beginFrame(200, 1, "live", false); P.endFrame(1, 9, false);
  let cap = P.drain();
  ok(cap.gpu.stats.issued === 3 && cap.gpu.results.n === 3, "every 3rd submission: 3 of 9");
  /* draws: one query per watched object, never nested, no frame query */
  P.resetCapture(); P.configure({ cpu: true, gpu: "draws", sampleEvery: 1 });
  const objs = ["fishBody", "floor", "snow"].map((l) => {
    const o = { label: l, onBeforeRender() { o.b = (o.b || 0) + 1; }, onAfterRender() { o.a = (o.a || 0) + 1; } };
    P.watch(o, l);
    return o;
  });
  P.watch(objs[0], "fishBody");   /* twice: chained once */
  P.beginFrame(300, 1, "live", false); P.beginSubmission();
  objs.forEach((o) => { o.onBeforeRender(); o.onAfterRender(); });
  P.endSubmission(true); P.endFrame(1, 10, false);
  objs.forEach((o) => { o.onBeforeRender(); o.onAfterRender(); });   /* outside a submission: nothing */
  gl.polls += 5; P.beginFrame(316, 1, "live", false); P.endFrame(1, 11, false);
  cap = P.drain();
  const labels = Array.from(cap.gpu.results.label).map((i) => Perf.LABELS[i]);
  ok(labels.join(",") === "fishBody,floor,snow", "three scopes, labelled", labels);
  ok(objs.every((o) => o.b === 2 && o.a === 2), "the objects' own callbacks still run (chained once)");
  ok(cap.cnt[Perf.K.gpuIssued] === 3, "gpuIssued 3");
  const s = Perf.summarize(cap);
  ok(s.gpu.labels.fishBody && s.gpu.labels.fishBody.scopes.n === 1, "summary per label");
}

section("visual random");
{
  const a = Perf.visualRandom(2463534242), b = Perf.visualRandom(2463534242), c = Perf.visualRandom(1);
  const xa = [], xb = [], xc = [];
  for (let i = 0; i < 1000; i++) { xa.push(a()); xb.push(b()); xc.push(c()); }
  ok(xa.every((x, i) => x === xb[i]), "the same seed, the same sequence");
  ok(xa.some((x, i) => x !== xc[i]), "another seed, another sequence");
  ok(xa.every((x) => x >= 0 && x < 1), "in [0, 1)");
}

console.log((fails ? "FAILED " + fails + " of " : "all ") + checks + " checks" + (fails ? "" : " passed"));
process.exit(fails ? 1 : 0);
