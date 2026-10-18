"use strict";
/* evo_boids TURBO worker — steps the sim headless at full speed, no rendering.
 * Classic worker (no modules). Protocol:
 *   in : {cmd:'start', checkpoint: string, js, wasm, threads}
 *                                          -> load the module the page uses
 *                                             (once), load checkpoint, run
 *                                             chunked loop
 *   in : {cmd:'stop'}                      -> finish current chunk, post the
 *                                             checkpoint back, go idle
 *   out: {type:'stats', frame, pop, food, pred, stepsPerSec, totalSteps} ~4x/s
 *   out: {type:'checkpoint', json}
 *   out: {type:'error', message}
 * Never calls _evo_sync or any render export; dt is fixed inside _evo_step —
 * no wall-clock time is ever passed to the sim.
 *
 * The page passes the module script/wasm URLs it loaded, so a threaded page
 * runs Turbo threaded too (a dedicated worker inherits the page's cross-origin
 * isolation). If the threaded module cannot start here, the single-threaded
 * one would need its own URLs, so the error is reported and the page keeps its
 * state — nothing is lost. */

var Module = null;
var modulePromise = null;
var running = false;
var stopRequested = false;
var chunkSteps = 60;   /* adaptive: targets ~200 ms of stepping per chunk */
var totalSteps = 0;
var startFrame = 0;
var samples = [];      /* [chunkStartMs, steps] pairs, rolling ~1 s window */
var lastStats = 0;

/* MessageChannel pump: schedules the next chunk as a plain macrotask (worker
 * timers can be throttled in background tabs; a port message is not). A 'stop'
 * posted by the page queues BEFORE the pump's next self-message, so the
 * onmessage handler below always gets to set stopRequested first, and the
 * flag check at the top of runChunk honours it before any further stepping. */
var pump = new MessageChannel();
pump.port1.onmessage = runChunk;
function scheduleChunk() { pump.port2.postMessage(0); }

function ensureModule(msg) {
  if (!modulePromise) {
    var js = msg.js || "evo_boids.js", wasm = msg.wasm || "evo_boids.wasm";
    importScripts(js);
    modulePromise = createEvoBoids({
      locateFile: function (path) { return path.slice(-5) === ".wasm" ? wasm : path; },
      mainScriptUrlOrBlob: js
    }).then(function (m) {
      Module = m;
      if (msg.threads && Module._evo_set_threads) Module._evo_set_threads(msg.threads);
      return m;
    });
  }
  return modulePromise;
}

function withCString(str, fn) {
  var size = Module.lengthBytesUTF8(str) + 1;
  var ptr = Module._malloc(size);
  Module.stringToUTF8(str, ptr, size);
  var r = fn(ptr);
  Module._free(ptr);
  return r;
}

onmessage = function (e) {
  var msg = e.data || {};
  if (msg.cmd === "start") {
    if (running) return;
    var p;
    try { p = ensureModule(msg); }
    catch (err) { postMessage({ type: "error", message: "wasm load failed: " + err }); return; }
    p.then(
      function () { startRun(String(msg.checkpoint)); },
      function (err) { postMessage({ type: "error", message: "wasm load failed: " + err }); }
    );
  } else if (msg.cmd === "stop") {
    stopRequested = true; /* honoured at the next chunk boundary */
  }
};

function startRun(checkpoint) {
  var ok = withCString(checkpoint, function (ptr) { return Module._evo_load_checkpoint(ptr); });
  if (!ok) {
    postMessage({ type: "error", message: Module.UTF8ToString(Module._evo_last_error()) });
    return;
  }
  /* absolute frame counter comes from the checkpoint; we advance it locally
   * (frame = startFrame + steps run) since _evo_stats needs _evo_sync */
  startFrame = 0;
  try {
    var cp = JSON.parse(checkpoint);
    if (cp && typeof cp.frame === "number") startFrame = cp.frame;
  } catch (err) { /* keep 0: frame becomes steps-this-session */ }
  totalSteps = 0;
  samples = [];
  chunkSteps = 60;
  lastStats = 0;
  running = true;
  stopRequested = false;
  postStats(performance.now()); /* immediate: the page flips to 'turbo' on first stats */
  scheduleChunk();
}

function runChunk() {
  if (!running) return;
  if (stopRequested) { finishStop(); return; }
  var t0 = performance.now();
  Module._evo_step(chunkSteps);
  var t1 = performance.now();
  var elapsed = Math.max(1, t1 - t0);
  totalSteps += chunkSteps;
  samples.push([t0, chunkSteps]);
  while (samples.length > 1 && samples[0][0] < t1 - 1000) samples.shift();
  chunkSteps = Math.max(30, Math.min(20000, Math.round(chunkSteps * 200 / elapsed)));
  if (t1 - lastStats >= 250) postStats(t1);
  scheduleChunk();
}

function postStats(now) {
  lastStats = now;
  var sps = 0;
  if (samples.length) {
    var span = (now - samples[0][0]) / 1000;
    var sum = 0;
    for (var i = 0; i < samples.length; i++) sum += samples[i][1];
    if (span > 0.05) sps = sum / span;
  }
  postMessage({
    type: "stats",
    frame: startFrame + totalSteps,
    pop: Module._evo_creature_count(),
    food: Module._evo_food_count(),
    pred: Module._evo_predator_count(),
    stepsPerSec: Math.round(sps),
    totalSteps: totalSteps
  });
}

function finishStop() {
  running = false;
  stopRequested = false;
  postMessage({ type: "checkpoint", json: Module.UTF8ToString(Module._evo_checkpoint()) });
}
