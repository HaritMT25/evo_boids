"use strict";
/* evo_boids Ocean preview: the page controller (web/ocean.html).
 *
 * Runs the Ocean world (web/evo_ocean.{js,wasm}, factory createEvoOcean,
 * single-threaded; the C API is PLAN_ocean_preview.md §4) and draws it in the
 * deep tank (web/ocean_scene.js, window.EvoOceanScene). It builds the tank's
 * per-frame snapshot itself, the same object renderer.js hands scene3d.js,
 * plus the depths: s.depth (per fish), s.foodDepth, s.predDepth.
 *
 * Every pointer the module returns is valid until the next oc_sync/oc_init,
 * and memory can grow, so the typed-array views are cut fresh from the heap
 * after every sync and never cached.
 *
 * URL: ?seed=42 &pop=300 &w=1920 &h=1080 (1080 x 1920 on a portrait screen)
 *      &depth=240 &cfg={"key":value,...}
 *      (SimConfig + OceanConfig keys, as oc_init takes them) &speed=1
 *      ?mock=1 loads ocean_mock.js from beside the page instead of the engine:
 *      a JS stand-in for page development, not part of web/.
 * Keys: space pause, + / - steps per frame, F follow a random fish (again:
 *       another one), Esc stop following, R a new world (new seed), shift+R
 *       replay this one, C back to the full view, E / Q or PageUp / PageDown
 *       (held) move the view up / down in depth, . one step while paused.
 *       Click a fish to mark it. The camera orbits freely, below the
 *       horizontal too (look up at the surface from the floor).
 * Pitch: s.vz from oc_creature_vz when the module has it, else estimated
 *       per fish id from depth change over distance swum (vzEstimate).
 *
 * window.EvoOcean: a small handle for the console and the tests
 *   (state(), set(name, v) -> oc_set_param, module). */
(function () {
  var A = window.EVO_ASSETS;
  var P = new URLSearchParams(location.search);
  var MOCK = P.get("mock") === "1";
  var DT = 0.016;                      /* oc_step's fixed step */
  var SPEEDS = [1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64];
  var STEP_BUDGET_MS = 11;             /* of a 16.7 ms frame: what stepping may take */
  var STATS = { pop: 0, frame: 1, births: 2, deaths: 3, reseeds: 4, depthMean: 5, depthSd: 6, hist0: 7 };
  var REQUIRED = ["oc_init", "oc_reset", "oc_step", "oc_sync", "oc_creature_count", "oc_creature_segments",
                  "oc_creature_depth", "oc_body_params", "oc_creature_colors", "oc_creature_ids",
                  "oc_food_count", "oc_food_xy", "oc_food_depth", "oc_predator_count", "oc_predator_data",
                  "oc_predator_depth", "oc_stats", "oc_stats_len", "oc_width", "oc_height", "oc_depth_max"];

  /* the Classic tank's body-size curve (renderer.js bodyScale): the two must match */
  function bodyScale(b) { return b > 0 ? 1 + 0.4 * Math.tanh(5 * (b - 1)) : 1; }

  function el(id) { return document.getElementById(id); }
  var canvas = el("tank"), msgEl = el("msg");
  function say(text) { msgEl.hidden = false; msgEl.className = "chip"; msgEl.textContent = text; }
  function fail(what, detail) {
    msgEl.hidden = false; msgEl.className = "chip err";
    msgEl.innerHTML = "";
    var b = document.createElement("b"); b.textContent = what;
    msgEl.appendChild(b);
    if (detail) msgEl.appendChild(document.createTextNode("\n" + detail));
    console.error("ocean: " + what + (detail ? " — " + detail : ""));
  }

  function num(key, def, lo, hi) {
    var v = parseFloat(P.get(key));
    if (!isFinite(v)) return def;
    return Math.max(lo, Math.min(hi, v));
  }
  /* a portrait screen gets the same-area world turned on its side (1080 x
   * 1920), as Classic shapes its world to a phone: the same food density and
   * crowding, and a tank that fills the screen instead of a strip across it */
  var portrait = innerHeight > innerWidth * 1.15;
  var INIT = {
    seed: num("seed", 42, 0, 4294967295) >>> 0,
    pop: Math.round(num("pop", 300, 0, 20000)),
    w: Math.round(num("w", portrait ? 1080 : 1920, 100, 20000)),
    h: Math.round(num("h", portrait ? 1920 : 1080, 100, 20000)),
    depth: num("depth", 240, 10, 5000),
    cfg: P.get("cfg") || ""
  };

  /* ---------------- the module ---------------- */
  var SIMD_PROBE = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0,
                                   10, 10, 1, 8, 0, 65, 0, 253, 15, 253, 98, 11]);   /* loader.js's */
  function loadScript(src) {
    return new Promise(function (resolve, reject) {
      var s = document.createElement("script");
      s.src = src;
      s.onload = resolve;
      s.onerror = function () { reject(new Error("could not load " + src)); };
      document.head.appendChild(s);
    });
  }
  function abs(u) { return new URL(u, location.href).href; }
  function loadModule() {
    if (!MOCK) {
      var simd = false;
      try { simd = typeof WebAssembly === "object" && WebAssembly.validate(SIMD_PROBE); } catch (e) { /* none */ }
      if (!simd) return Promise.reject(new Error("this browser has no WebAssembly SIMD, which the simulation needs (Chrome 91+, Firefox 89+ or Safari 16.4+)"));
    }
    var js = abs(MOCK ? A.mock : A.ocean), wasm = abs(A.oceanWasm);
    return loadScript(js).then(function () {
      if (typeof createEvoOcean !== "function") throw new Error(js + " loaded but defines no createEvoOcean()");
      return createEvoOcean({ locateFile: function (path) { return path.slice(-5) === ".wasm" ? wasm : path; } });
    });
  }

  var M = null;
  function heap() { return M.wasmMemory ? M.wasmMemory.buffer : M.HEAPU8.buffer; }
  function u8() { return new Uint8Array(heap()); }
  function withCString(str, fn) {
    if (!str) return fn(0);
    var bytes = new TextEncoder().encode(str), ptr = M._malloc(bytes.length + 1);
    var h = u8(); h.set(bytes, ptr); h[ptr + bytes.length] = 0;
    try { return fn(ptr); } finally { M._free(ptr); }
  }
  function cString(ptr) {
    if (!ptr) return "";
    if (M.UTF8ToString) return M.UTF8ToString(ptr);
    var h = u8(), end = ptr;
    while (h[end]) end++;
    return new TextDecoder().decode(h.subarray(ptr, end));
  }
  function lastError() { return M._oc_last_error ? cString(M._oc_last_error()) : "(no oc_last_error)"; }

  var W = 0, H = 0, D = 0, statsLen = 0;
  function readWorld() {
    W = M._oc_width(); H = M._oc_height(); D = M._oc_depth_max();
    statsLen = M._oc_stats_len();
  }
  function initWorld() {
    var ok = withCString(INIT.cfg, function (ptr) {
      return M._oc_init(INIT.seed, INIT.w, INIT.h, INIT.depth, INIT.pop, ptr);
    });
    if (!ok) throw new Error("oc_init failed: " + lastError());
    readWorld();
  }

  /* ---------------- per-frame data ---------------- */
  /* Called right after oc_sync: views over this sync's buffers only. */
  function snapshot() {
    var buf = heap();
    var n = M._oc_creature_count(), fc = M._oc_food_count(), pc = M._oc_predator_count();
    return {
      n: n, fc: fc, pc: pc, selIdx: -1,
      segs: new Float32Array(buf, M._oc_creature_segments(), n * 12),
      depth: new Float32Array(buf, M._oc_creature_depth(), n),
      ids: new Float64Array(buf, M._oc_creature_ids(), n),
      body: new Float32Array(buf, M._oc_body_params(), n * 2),
      colors: new Uint8Array(buf, M._oc_creature_colors(), n * 3),
      food: new Float32Array(buf, M._oc_food_xy(), fc * 2),
      foodDepth: new Float32Array(buf, M._oc_food_depth(), fc),
      preds: new Float32Array(buf, M._oc_predator_data(), pc * 3),
      predDepth: new Float32Array(buf, M._oc_predator_depth(), pc),
      stats: new Float64Array(buf, M._oc_stats(), statsLen),
      /* the fish's pitch (s.vz, + = sinking, tan of the angle): the engine's
       * oc_creature_vz when it has one, else estimated (vzEstimate) */
      vz: HAS_VZ ? new Float32Array(buf, M._oc_creature_vz(), n) : null,
      curActive: false, curGrid: null, curCols: 0, curRows: 0   /* §4 exports no current grid */
    };
  }

  /* Without oc_creature_vz: each fish's pitch from how its depth changed over
   * the distance its head swam since the last frame (wrapping), smoothed, and
   * keyed by id so births and deaths don't shuffle it. Same units as the
   * engine's vz (depth per unit of horizontal travel). */
  var HAS_VZ = false;
  function vzBufs(cap) {
    return { n: 0, ids: new Float64Array(cap), x: new Float32Array(cap), y: new Float32Array(cap),
             d: new Float32Array(cap), vz: new Float32Array(cap) };
  }
  var estPrev = vzBufs(0), estNext = vzBufs(0);   /* last frame's, and the set being written */
  function vzEstimate(s, stepped) {
    var n = s.n, prev = estPrev, at = new Map();
    for (var j = 0; j < prev.n; j++) at.set(prev.ids[j], j);
    if (estNext.ids.length < n) estNext = vzBufs(Math.max(256, n * 2));
    var B = estNext;
    for (var c = 0; c < n; c++) {
      var id = s.ids[c], x = s.segs[12 * c], y = s.segs[12 * c + 1], d = s.depth[c], v = 0;
      var k = at.get(id);
      if (k !== undefined) {
        v = prev.vz[k];
        if (stepped) {
          var dx = x - prev.x[k], dy = y - prev.y[k];
          if (dx > W / 2) dx -= W; else if (dx < -W / 2) dx += W;
          if (dy > H / 2) dy -= H; else if (dy < -H / 2) dy += H;
          var h = Math.hypot(dx, dy);
          if (h > 0.05) v += (Math.max(-1.2, Math.min(1.2, (d - prev.d[k]) / h)) - v) * 0.25;
        }
      }
      B.ids[c] = id; B.x[c] = x; B.y[c] = y; B.d[c] = d; B.vz[c] = v;
    }
    B.n = n;
    estNext = estPrev; estPrev = B;   /* B stays intact until the frame after next */
    return B.vz.subarray(0, n);
  }

  /* ---------------- state ---------------- */
  var scene = null, lastS = null;
  var paused = false, stepOnce = false;
  var speedIdx = Math.max(0, SPEEDS.indexOf(Math.round(num("speed", 1, 1, 64))));
  var stepMs = 0;                 /* ema of one oc_step, ms */
  var lastSteps = 0;              /* steps actually run last frame (the budget may cut the ask) */
  var selectedId = null, wantFollow = false;
  var animT = 0, lastNow = 0;
  var fpsFrames = 0, fpsT = 0, fps = 0;

  function speed() { return SPEEDS[speedIdx]; }
  function stepsThisFrame() {
    var ask = speed();
    if (ask <= 1 || !(stepMs > 0)) return ask;
    return Math.max(1, Math.min(ask, Math.floor(STEP_BUDGET_MS / stepMs)));
  }

  function indexOfId(s, id) {
    if (id === null) return -1;
    for (var i = 0; i < s.n; i++) if (s.ids[i] === id) return i;
    return -1;
  }
  function randomFish(s, notId) {
    if (!s || !s.n) return -1;
    var i = Math.floor(Math.random() * s.n);
    if (s.n > 1 && s.ids[i] === notId) i = (i + 1 + Math.floor(Math.random() * (s.n - 1))) % s.n;
    return i;
  }
  /* F: follow a random fish (or, the first time, the one already marked) */
  function follow() {
    var s = lastS;
    if (!s || !s.n) return;
    var idx = !scene.following() ? indexOfId(s, selectedId) : -1;
    if (idx < 0) idx = randomFish(s, selectedId);
    selectedId = s.ids[idx];
    wantFollow = true;
    if (!scene.following()) scene.setFollow(true);
    dirtyHud = true;
  }
  function unfollow() {
    wantFollow = false;
    if (scene.following()) scene.setFollow(false);
    selectedId = null;
    dirtyHud = true;
  }
  function onFollowChange(on) {
    el("b-follow").setAttribute("aria-pressed", on ? "true" : "false");
    /* the user dragged/refit the camera away: stop wanting it */
    if (!on) wantFollow = false;
  }
  /* R: a new world (new seed); Shift+R: replay this one from the start.
   * Either way the camera stays where it is — C is the key that moves it. */
  function reset(replay) {
    if (scene.following()) scene.setFollow(false);
    wantFollow = false; selectedId = null;
    var w0 = W, h0 = H, d0 = D;
    if (replay) {
      if (!M._oc_reset()) { fail("oc_reset failed", lastError()); return; }
      readWorld();
    } else {
      INIT.seed = (Math.random() * 4294967296) >>> 0;
      try { initWorld(); } catch (err) { fail("oc_init failed", lastError()); return; }
      try {
        var u = new URL(location.href);
        u.searchParams.set("seed", String(INIT.seed));
        history.replaceState(null, "", u);   /* a reload keeps this world */
      } catch (err) { /* file:// or a sandboxed frame: the seed is in the HUD */ }
    }
    if (W !== w0 || H !== h0 || D !== d0) scene.setWorld(W, H, D);
    estPrev.n = 0;   /* ids start over: forget the old fish's pitch */
    rates.reset();
    stepMs = 0;
    dirtyHud = true;
  }
  function setPaused(on) {
    paused = on;
    el("b-pause").setAttribute("aria-pressed", on ? "true" : "false");
    el("b-pause").textContent = on ? "Run" : "Pause";
    dirtyHud = true;
  }
  function changeSpeed(d) {
    speedIdx = Math.max(0, Math.min(SPEEDS.length - 1, speedIdx + d));
    dirtyHud = true;
  }

  /* ---------------- the HUD ---------------- */
  var dirtyHud = true, hudT = 0;
  var histRows = [];
  function buildHist() {
    var box = el("hist");
    for (var i = 0; i < 8; i++) {
      var row = document.createElement("div"); row.className = "row";
      var lab = document.createElement("span");
      var track = document.createElement("div"); track.className = "track";
      var bar = document.createElement("div"); bar.className = "bar";
      /* light fades and cools with depth, like the tank */
      var t = i / 7;
      bar.style.background = "oklch(" + (0.84 - 0.30 * t).toFixed(3) + " " + (0.15 - 0.04 * t).toFixed(3) + " " + (150 + 90 * t).toFixed(0) + ")";
      var pct = document.createElement("b");
      track.appendChild(bar); row.appendChild(lab); row.appendChild(track); row.appendChild(pct);
      box.appendChild(row);
      histRows.push({ lab: lab, bar: bar, pct: pct });
    }
  }
  function labelHist() {
    for (var i = 0; i < 8; i++) histRows[i].lab.textContent = Math.round(D * i / 8);
    histRows[0].lab.title = "surface";
  }
  /* births / reseeds per 1000 frames, from a sliding window of samples
   * (the counters are cumulative; a drop means the world was reset) */
  var rates = {
    q: [],
    reset: function () { this.q.length = 0; },
    push: function (frame, births, reseeds) {
      var q = this.q, last = q[q.length - 1];
      if (last && (frame < last.f || births < last.b || reseeds < last.r)) q.length = 0;
      if (!last || frame !== last.f) q.push({ f: frame, b: births, r: reseeds });
      while (q.length > 2 && q[q.length - 1].f - q[0].f > 6000) q.shift();
    },
    per1k: function (key) {
      var q = this.q;
      if (q.length < 2) return null;
      var a = q[0], b = q[q.length - 1], df = b.f - a.f;
      return df > 0 ? (b[key] - a[key]) * 1000 / df : null;
    }
  };
  function fmtInt(v) { return Math.round(v).toLocaleString("en-US"); }
  function setText(id, html) { var e = el(id); if (e._v !== html) { e._v = html; e.innerHTML = html; } }
  function updateHud(s, now) {
    if (!dirtyHud && now - hudT < 250) return;
    dirtyHud = false; hudT = now;
    var st = s.stats, g = function (k) { return STATS[k] < st.length ? st[STATS[k]] : NaN; };
    var frame = g("frame"), births = g("births"), reseeds = g("reseeds");
    rates.push(frame, births, reseeds);
    var rb = rates.per1k("b"), rr = rates.per1k("r");
    /* <small class="x">: extras a narrow screen leaves out */
    setText("h-pop", fmtInt(s.n) + ' <small class="x">· ' + fmtInt(s.fc) + " food · " + s.pc + " pred</small>");
    setText("h-frame", fmtInt(frame) + ' <small class="x">· ' + (frame * DT / 60).toFixed(1) + " min</small>");
    setText("h-seed", String(INIT.seed));
    setText("h-births", fmtInt(births) + (rb !== null ? ' <small class="x">· ' + rb.toFixed(1) + "/1k fr</small>" : ""));
    setText("h-reseeds", fmtInt(reseeds) + (rr !== null ? ' <small class="x">· ' + rr.toFixed(1) + "/1k fr</small>" : ""));
    setText("h-deaths", fmtInt(g("deaths")));
    setText("h-depth", g("depthMean").toFixed(0) + " ± " + g("depthSd").toFixed(0) + ' <small class="x">of ' + D.toFixed(0) + "</small>");
    /* steps run per frame, and what was asked when the step budget cut it */
    var ask = speed(), ran = paused ? ask : lastSteps;
    setText("h-speed", ran + (ran !== ask ? "<small>/" + ask + "</small>" : "") +
            ' <small><span class="x">steps/frame </span>· ' + fps + " fps</small>");
    for (var i = 0; i < 8; i++) {
      var k = STATS.hist0 + i, v = k < st.length ? st[k] : 0;
      v = isFinite(v) ? v : 0;
      histRows[i].bar.style.width = Math.min(100, v * 200).toFixed(1) + "%";   /* full track = 50 % */
      histRows[i].pct.textContent = Math.round(v * 100) + "%";
    }
    var state = el("h-state");
    state.className = "state" + (paused ? " paused" : "");
    state.textContent = paused ? "paused" : (scene.following() ? "following a fish" : "");
    var sel = el("h-sel"), idx = indexOfId(s, selectedId);
    if (idx >= 0) {
      var c = "rgb(" + s.colors[3 * idx] + "," + s.colors[3 * idx + 1] + "," + s.colors[3 * idx + 2] + ")";
      setText("h-sel", '<i style="background:' + c + '"></i>fish #' + selectedId + " · depth " + s.depth[idx].toFixed(0));
    } else if (sel._v !== "") { sel._v = ""; sel.innerHTML = ""; }
  }

  /* ---------------- the loop ---------------- */
  function frame(now) {
    requestAnimationFrame(frame);
    var dt = lastNow ? Math.min(0.1, (now - lastNow) / 1000) : 0;
    lastNow = now;
    var steps = 0;
    if (!paused || stepOnce) {
      steps = stepOnce ? 1 : stepsThisFrame();
      stepOnce = false;
      var t0 = performance.now();
      M._oc_step(steps);
      var per = (performance.now() - t0) / steps;
      stepMs = stepMs ? stepMs * 0.9 + per * 0.1 : per;
      if (!paused) animT += dt * Math.min(steps, 2);
    }
    lastSteps = steps;
    M._oc_sync();
    var s = snapshot();
    if (!HAS_VZ) s.vz = vzEstimate(s, steps > 0);
    /* Q/E, PageDown/PageUp held: the view sinks or rises, the whole depth in ~2 s */
    var vdir = 0;
    for (var key in heldDepthKeys) vdir += heldDepthKeys[key];
    if (vdir) scene.moveView(Math.sign(vdir) * D * 0.55 * dt);
    /* the marked fish, by id; a followed fish that died hands over to another */
    var idx = indexOfId(s, selectedId);
    if (idx < 0 && selectedId !== null) {
      if (wantFollow && s.n) { idx = randomFish(s, null); selectedId = s.ids[idx]; }
      else selectedId = null;
      dirtyHud = true;
    }
    s.selIdx = idx;
    lastS = s;
    scene.render(s, { animT: animT, dtSim: steps * DT, selIdx: idx });
    fpsFrames++;
    if (now - fpsT >= 1000) { fps = Math.round(fpsFrames * 1000 / (now - fpsT)); fpsFrames = 0; fpsT = now; }
    updateHud(s, now);
  }

  /* ---------------- input ---------------- */
  /* the keys that move the view in depth, while held: +1 up, -1 down */
  var DEPTH_KEYS = { e: 1, pageup: 1, q: -1, pagedown: -1 };
  var heldDepthKeys = {};
  function wireInput() {
    document.addEventListener("keyup", function (e) { delete heldDepthKeys[String(e.key).toLowerCase()]; });
    window.addEventListener("blur", function () { heldDepthKeys = {}; });
    document.addEventListener("keydown", function (e) {
      if (e.ctrlKey || e.metaKey || e.altKey) return;
      var k = e.key, lk = String(k).toLowerCase();
      if (DEPTH_KEYS[lk]) { heldDepthKeys[lk] = DEPTH_KEYS[lk]; e.preventDefault(); return; }
      if (k === " " || k === "Spacebar") { setPaused(!paused); e.preventDefault(); }
      else if (k === "+" || k === "=") changeSpeed(1);
      else if (k === "-" || k === "_") changeSpeed(-1);
      else if (k === "f" || k === "F") follow();
      else if (k === "r" || k === "R") reset(e.shiftKey);
      else if (k === "c" || k === "C") { wantFollow = false; scene.resetView(true); }
      else if (k === "Escape") unfollow();
      else if (k === "." && paused) stepOnce = true;
      else return;
      dirtyHud = true;
    });
    function btn(id, fn) { el(id).addEventListener("click", function () { fn(); this.blur(); }); }
    btn("b-pause", function () { setPaused(!paused); });
    btn("b-slower", function () { changeSpeed(-1); });
    btn("b-faster", function () { changeSpeed(1); });
    btn("b-follow", function () { if (scene.following()) unfollow(); else follow(); });
    btn("b-reset", function () { reset(false); });
    /* a short tap or click marks the fish nearest it on screen; empty water
     * lets go. A drag, pinch or long press belongs to the camera. */
    var tap = null;
    canvas.addEventListener("pointerdown", function (e) {
      tap = e.isPrimary && e.button === 0 ? { x: e.clientX, y: e.clientY, t: performance.now(), id: e.pointerId } : null;
    });
    canvas.addEventListener("pointercancel", function () { tap = null; });
    canvas.addEventListener("pointerup", function (e) {
      if (!tap || e.pointerId !== tap.id) return;
      var moved = Math.hypot(e.clientX - tap.x, e.clientY - tap.y), held = performance.now() - tap.t;
      tap = null;
      if (moved > 8 || held > 600 || !lastS) return;
      var best = scene.pick(lastS, e.clientX, e.clientY, e.pointerType === "mouse" ? 18 : 30);
      if (best < 0 && scene.following()) return;   /* a stray tap doesn't drop a follow */
      selectedId = best >= 0 ? lastS.ids[best] : null;
      dirtyHud = true;
    });
    window.addEventListener("resize", function () { scene.resize(); placeInsets(); });
  }
  /* on a narrow screen the HUD and the buttons sit over the tank: frame it between them */
  function placeInsets() {
    var narrow = window.matchMedia("(max-width: 700px)").matches;
    if (!narrow) { scene.setInsets({ top: 0, right: 0, bottom: 0, left: 0 }); return; }
    var top = el("hud").getBoundingClientRect().bottom + 6;
    var bottom = innerHeight - el("btns").getBoundingClientRect().top + 6;
    scene.setInsets({ top: Math.round(top), right: 0, bottom: Math.round(bottom), left: 0 });
  }

  /* ---------------- start ---------------- */
  el("h-tag").textContent = MOCK ? "mock engine" : "preview";
  if (MOCK) el("h-tag").classList.add("mock");
  buildHist();
  say(MOCK ? "Starting the ocean (mock engine)…" : "Starting the ocean…");
  var sceneReady = window.EvoOceanScene ? EvoOceanScene.load() : Promise.reject(new Error("ocean_scene.js missing"));
  Promise.all([loadModule().catch(function (e) {
    var why = String(e && e.message || e);
    if (/could not load/.test(why) && !MOCK)
      why += "\nThe Ocean module is web/evo_ocean.js + .wasm, built by wasm/build_ocean.sh.";
    throw new Error(why);
  }), sceneReady]).then(function (r) {
    M = r[0];
    var missing = REQUIRED.filter(function (f) { return typeof M["_" + f] !== "function"; });
    if (missing.length) throw new Error("the module lacks exports: " + missing.join(", "));
    if (!M.wasmMemory && !M.HEAPU8) throw new Error("the module exposes neither wasmMemory nor HEAPU8");
    if (!M._malloc && INIT.cfg) throw new Error("?cfg= needs _malloc/_free exported");
    HAS_VZ = typeof M._oc_creature_vz === "function";   /* optional: the pitch, else estimated */
    initWorld();
    scene = EvoOceanScene.create(canvas, {
      tankDepth: D,
      coarse: window.matchMedia("(pointer: coarse)").matches,
      reducedMotion: window.matchMedia("(prefers-reduced-motion: reduce)").matches,
      bodyScale: bodyScale, onFollowChange: onFollowChange
    });
    if (!scene) throw new Error("WebGL2 is unavailable, and the Ocean preview has only the 3D tank");
    scene.setWorld(W, H, D);
    labelHist();
    wireInput();
    placeInsets();
    msgEl.hidden = true;
    window.EvoOcean = {
      module: M,
      scene: scene,
      /* the steepest diver (largest |vz|) right now: {idx, id, vz, x, y, depth} */
      steepest: function (sinking) {
        var s = lastS, best = -1, bv = 0;
        if (!s || !s.vz) return null;
        for (var i = 0; i < s.n; i++) {
          var v = sinking ? s.vz[i] : Math.abs(s.vz[i]);
          if (v > bv) { bv = v; best = i; }
        }
        return best < 0 ? null : { idx: best, id: s.ids[best], vz: s.vz[best], x: s.segs[12 * best], y: s.segs[12 * best + 1],
                                   x1: s.segs[12 * best + 2], y1: s.segs[12 * best + 3], depth: s.depth[best] };
      },
      set: function (name, v) {
        if (!M._oc_set_param) return false;
        var ok = withCString(String(name), function (p) { return M._oc_set_param(p, +v); });
        if (!ok) console.warn("oc_set_param(" + name + "): " + lastError());
        return !!ok;
      },
      state: function () {
        var s = lastS;
        if (!s) return null;
        var lo = Infinity, hi = -Infinity;
        for (var i = 0; i < s.n; i++) { lo = Math.min(lo, s.depth[i]); hi = Math.max(hi, s.depth[i]); }
        return { n: s.n, fc: s.fc, pc: s.pc, W: W, H: H, D: D, paused: paused, speed: speed(), steps: lastSteps,
                 hasVz: HAS_VZ, view: scene.view(),
                 stepMs: stepMs, fps: fps, following: scene.following(), selectedId: selectedId, selIdx: s.selIdx,
                 stats: Array.prototype.slice.call(s.stats), depthMin: s.n ? lo : 0, depthMax: s.n ? hi : 0 };
      }
    };
    requestAnimationFrame(frame);
  }).catch(function (e) {
    fail("The Ocean preview could not start.", String(e && e.message || e));
  });
})();
