"use strict";
/* evo_boids 3-D Ocean: the TURBO worker — steps the world headless at full
 * speed, nothing drawn (web/ocean.js; Classic's is web/worker.js).
 *
 * Hand-off is the module's whole linear memory. The page and this worker run
 * the SAME single-threaded module (web/evo_ocean.{js,wasm}), and everything
 * the world is — every C++ object, the allocator's state, the rng — lives in
 * its linear memory; between calls the only other state, the stack pointer,
 * is the same in every instance. So a byte copy of the memory, from the page
 * to here and back, carries the run on exactly, step for step (no checkpoint
 * format needed; the Ocean has none). The receiving memory is grown to the
 * sender's size first (through the module's own allocator, so its heap views
 * follow) and anything past the sender's end is zeroed, as fresh pages are.
 *
 * Protocol:
 *   in : {cmd:'start', js, wasm, mem: ArrayBuffer, recEvery, famK}
 *        -> load the module (once), take the memory, run in chunks
 *   in : {cmd:'set', key, value}   -> oc_set_param (a live setting, mid-Turbo)
 *   in : {cmd:'rec', every}        -> the dashboards' sampling interval
 *   in : {cmd:'stop'}              -> finish the chunk, post the memory back
 *   out: {type:'stats', frame, pop, food, pred, stepsPerSec, totalSteps} ~4x/s
 *   out: {type:'rec', rows}        -> dashboard samples (web/ocean_dash.js row())
 *   out: {type:'set', key, value, ok, error}
 *   out: {type:'memory', mem: ArrayBuffer, totalSteps}
 *   out: {type:'error', message}
 * The step is oc_step's fixed dt: no wall-clock time reaches the world. */
var M = null, modulePromise = null;
var running = false, stopRequested = false;
var chunkSteps = 60, totalSteps = 0, samples = [], lastStats = 0;
var recEvery = 60, famK = 8, rows = [];
var pump = new MessageChannel();   /* not throttled like timers in a background tab */
pump.port1.onmessage = runChunk;
function scheduleChunk() { pump.port2.postMessage(0); }

function heap() { return M.wasmMemory ? M.wasmMemory.buffer : M.HEAPU8.buffer; }
function ensureModule(msg) {
  if (!modulePromise) {
    importScripts(msg.js);
    modulePromise = createEvoOcean({
      locateFile: function (p) { return p.slice(-5) === ".wasm" ? msg.wasm : p; },
      mainScriptUrlOrBlob: msg.js
    }).then(function (m) { M = m; return m; });
  }
  return modulePromise;
}
/* Take a whole memory image: grow to its size through the allocator (so the
 * module's views follow), copy it in, zero the rest. */
function takeMemory(src) {
  var need = src.byteLength, have = heap().byteLength;
  if (have < need) {
    var p = M._malloc(need - have + 65536);
    if (p) M._free(p);
    if (heap().byteLength < need) throw new Error("could not grow the worker's memory to " + need + " bytes");
  }
  var dst = new Uint8Array(heap());
  dst.set(new Uint8Array(src));
  if (dst.length > need) dst.fill(0, need);
}
function cString(ptr) { return ptr ? M.UTF8ToString(ptr) : ""; }

onmessage = function (e) {
  var msg = e.data || {};
  if (msg.cmd === "start") {
    if (running) return;
    recEvery = msg.recEvery || 60;
    famK = msg.famK || 8;
    var p;
    try { p = ensureModule(msg); }
    catch (err) { postMessage({ type: "error", message: "the Ocean module did not load: " + err }); return; }
    p.then(function () {
      try { takeMemory(msg.mem); }
      catch (err) { postMessage({ type: "error", message: String(err && err.message || err) }); return; }
      totalSteps = 0; samples = []; chunkSteps = 60; lastStats = 0; rows = [];
      running = true; stopRequested = false;
      postStats(performance.now());   /* the page flips to 'turbo' on the first stats */
      scheduleChunk();
    }, function (err) { postMessage({ type: "error", message: "the Ocean module did not load: " + err }); });
  } else if (msg.cmd === "stop") {
    stopRequested = true;   /* honoured at the next chunk boundary */
  } else if (msg.cmd === "rec") {
    recEvery = msg.every || recEvery;
  } else if (msg.cmd === "set") {
    if (!M) return;
    var key = String(msg.key), ok = 0, n = M.lengthBytesUTF8(key) + 1, ptr = M._malloc(n);
    M.stringToUTF8(key, ptr, n);
    ok = M._oc_set_param(ptr, +msg.value);
    M._free(ptr);
    postMessage({ type: "set", key: key, value: +msg.value, ok: !!ok, error: ok ? "" : cString(M._oc_last_error()) });
  }
};

/* the frame counter, from the stats (oc_stats needs an oc_sync) */
function frameNow() { M._oc_sync(); return new Float64Array(heap(), M._oc_stats(), M._oc_stats_len())[1]; }
/* one dashboard sample: the same row web/ocean_dash.js builds on the page */
function sample() {
  M._oc_sync();
  var st = Array.prototype.slice.call(new Float64Array(heap(), M._oc_stats(), M._oc_stats_len()));
  var su = Array.prototype.slice.call(new Float64Array(heap(), M._oc_summary(), M._oc_summary_len()));
  var k = M._oc_families(famK);
  var fam = Array.prototype.slice.call(new Float64Array(heap(), M._oc_families_data(), 2 * k));
  rows.push({ st: st, su: su, fam: fam });
}
function runChunk() {
  if (!running) return;
  if (stopRequested) { finishStop(); return; }
  var t0 = performance.now(), left = chunkSteps;
  /* step to each sampling boundary exactly, so the samples land on the frames
   * the page would sample */
  var f = frameNow();
  while (left > 0) {
    var toNext = recEvery - (f % recEvery);
    var n = Math.min(left, toNext);
    M._oc_step(n);
    left -= n; f += n;
    if (f % recEvery === 0) sample();
  }
  var t1 = performance.now(), elapsed = Math.max(1, t1 - t0);
  totalSteps += chunkSteps;
  samples.push([t0, chunkSteps]);
  while (samples.length > 1 && samples[0][0] < t1 - 1000) samples.shift();
  chunkSteps = Math.max(10, Math.min(20000, Math.round(chunkSteps * 150 / elapsed)));
  if (t1 - lastStats >= 250) postStats(t1);
  scheduleChunk();
}
function postRows() {
  if (!rows.length) return;
  postMessage({ type: "rec", rows: rows });
  rows = [];
}
function postStats(now) {
  lastStats = now;
  postRows();
  var sps = 0;
  if (samples.length) {
    var span = (now - samples[0][0]) / 1000, sum = 0;
    for (var i = 0; i < samples.length; i++) sum += samples[i][1];
    if (span > 0.05) sps = sum / span;
  }
  postMessage({ type: "stats", frame: frameNow(), pop: M._oc_creature_count(), food: M._oc_food_count(),
                pred: M._oc_predator_count(), stepsPerSec: Math.round(sps), totalSteps: totalSteps });
}
function finishStop() {
  running = false;
  stopRequested = false;
  postRows();
  var copy = heap().slice(0);
  postMessage({ type: "memory", mem: copy, totalSteps: totalSteps }, [copy]);
}
