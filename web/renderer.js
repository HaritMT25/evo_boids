"use strict";
/* evo_boids wasm renderer — canvas 2D front-end.
 * World: 1920x1080, 1 unit = 1 px, origin top-left, +y down (canvas-native).
 * No interpolation, no trails, no wrap ghosts. Typed-array views are re-created
 * after every _evo_sync (memory growth invalidates buffers). */
(function () {
  var WORLD_W = 1920, WORLD_H = 1080, TAU = Math.PI * 2;
  var RADII = [5, 3.5, 3, 2.5, 2, 1.5];
  var SEG_ALPHA = [255, 225, 195, 165, 135, 105].map(function (a) { return a / 255; });
  var CUR_TAIL = "rgba(110,170,215," + (60 / 255).toFixed(4) + ")";
  var CUR_HEAD = "rgba(150,205,235," + (130 / 255).toFixed(4) + ")";
  var RING_STYLE = "rgba(255,235,120," + (230 / 255).toFixed(4) + ")";

  var Module = null;
  var paused = false;
  var simSpeed = 1;
  var selectedId = null;

  var canvas = document.getElementById("canvas");
  var ctx = canvas.getContext("2d", { alpha: false });
  function el(id) { return document.getElementById(id); }
  var errEl = el("err");

  function showError(msg) { errEl.textContent = msg; }
  function clearError() { errEl.textContent = ""; }

  function intVal(id, def) {
    var v = parseInt(el(id).value, 10);
    return Number.isFinite(v) ? v : def;
  }
  function floatVal(id, def) {
    var v = parseFloat(el(id).value);
    return Number.isFinite(v) ? v : def;
  }

  /* Build config JSON with only the non-default keys. */
  function readForm() {
    var cfg = {};
    var predators = intVal("predators", 0);
    var foodPatches = intVal("foodPatches", 0);
    var currentStrength = floatVal("currentStrength", 0);
    if (predators !== 0) cfg.predatorCount = predators;
    if (el("predatorEconomy").checked) cfg.predatorEconomy = true;
    if (currentStrength !== 0) cfg.currentStrength = currentStrength;
    if (foodPatches !== 0) cfg.foodPatches = foodPatches;
    return {
      seed: intVal("seed", 42) >>> 0,
      population: intVal("population", 300),
      cfg: cfg
    };
  }

  function restart() {
    if (!Module) return false;
    clearError();
    var f = readForm();
    var ptr = 0;
    if (Object.keys(f.cfg).length > 0) {
      var json = JSON.stringify(f.cfg);
      var size = Module.lengthBytesUTF8(json) + 1;
      ptr = Module._malloc(size);
      Module.stringToUTF8(json, ptr, size);
    }
    var ok = Module._evo_init(f.seed, WORLD_W, WORLD_H, f.population, ptr);
    if (ptr) Module._free(ptr);
    selectedId = null;
    if (!ok) showError("evo_init failed — check parameters");
    return !!ok;
  }

  /* ---- frame snapshot: views created fresh after every _evo_sync ---- */
  function snapshot() {
    var n = Module._evo_creature_count();
    var fc = Module._evo_food_count();
    var pc = Module._evo_predator_count();
    var f32 = Module.HEAPF32.buffer;
    var s = {
      n: n, fc: fc, pc: pc,
      segs: new Float32Array(f32, Module._evo_creature_segments(), n * 12),
      colors: new Uint8Array(Module.HEAPU8.buffer, Module._evo_creature_colors(), n * 3),
      ids: new Float64Array(Module.HEAPF64.buffer, Module._evo_creature_ids(), n),
      food: new Float32Array(f32, Module._evo_food_xy(), fc * 2),
      preds: new Float32Array(f32, Module._evo_predator_data(), pc * 3),
      stats: new Float64Array(Module.HEAPF64.buffer, Module._evo_stats(), 9),
      curActive: Module._evo_current_active() !== 0,
      curGrid: null, curCols: 0, curRows: 0
    };
    if (s.curActive) {
      s.curCols = Module._evo_current_cols();
      s.curRows = Module._evo_current_rows();
      s.curGrid = new Float32Array(f32, Module._evo_current_grid(), s.curCols * s.curRows * 2);
    }
    return s;
  }

  function draw(s) {
    ctx.fillStyle = "#0F1E19";
    ctx.fillRect(0, 0, WORLD_W, WORLD_H);

    if (s.curActive) {
      ctx.lineWidth = 1;
      for (var j = 0; j < s.curRows; j++) {
        for (var i = 0; i < s.curCols; i++) {
          var k = (j * s.curCols + i) * 2;
          var vx = s.curGrid[k], vy = s.curGrid[k + 1];
          if (vx * vx + vy * vy < 1) continue;
          var x0 = 32 + 64 * i, y0 = 32 + 64 * j;
          var x1 = x0 + vx * 0.45, y1 = y0 + vy * 0.45;
          var grad = ctx.createLinearGradient(x0, y0, x1, y1);
          grad.addColorStop(0, CUR_TAIL);
          grad.addColorStop(1, CUR_HEAD);
          ctx.strokeStyle = grad;
          ctx.beginPath();
          ctx.moveTo(x0, y0);
          ctx.lineTo(x1, y1);
          ctx.stroke();
        }
      }
    }

    ctx.fillStyle = "#97C632";
    ctx.beginPath();
    for (var fi = 0; fi < s.fc; fi++) {
      var fx = s.food[2 * fi], fy = s.food[2 * fi + 1];
      ctx.moveTo(fx + 2.5, fy);
      ctx.arc(fx, fy, 2.5, 0, TAU);
    }
    ctx.fill();

    for (var c = 0; c < s.n; c++) {
      var rgb = s.colors[3 * c] + "," + s.colors[3 * c + 1] + "," + s.colors[3 * c + 2];
      for (var seg = 0; seg < 6; seg++) {
        ctx.fillStyle = "rgba(" + rgb + "," + SEG_ALPHA[seg] + ")";
        ctx.beginPath();
        ctx.arc(s.segs[12 * c + 2 * seg], s.segs[12 * c + 2 * seg + 1], RADII[seg], 0, TAU);
        ctx.fill();
      }
    }

    for (var p = 0; p < s.pc; p++) {
      ctx.fillStyle = s.preds[3 * p + 2] > 0 ? "rgb(150,60,60)" : "rgb(220,50,50)";
      ctx.beginPath();
      ctx.arc(s.preds[3 * p], s.preds[3 * p + 1], 6.5, 0, TAU);
      ctx.fill();
    }

    if (selectedId !== null) {
      var idx = -1;
      for (var q = 0; q < s.n; q++) if (s.ids[q] === selectedId) { idx = q; break; }
      if (idx < 0) {
        selectedId = null;
      } else {
        ctx.strokeStyle = RING_STYLE;
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.arc(s.segs[12 * idx], s.segs[12 * idx + 1], 16, 0, TAU);
        ctx.stroke();
      }
    }

    var st = s.stats, fmt = function (v) { return String(Math.round(v)); };
    ctx.font = "14px monospace";
    ctx.fillStyle = "rgba(200,200,200,0.78)";
    ctx.fillText(
      "pop " + fmt(st[0]) + "  food " + fmt(st[1]) + "  pred " + fmt(st[2]) +
      "  frame " + fmt(st[3]) + "  births " + fmt(st[4]) + "  deaths " + fmt(st[5]) +
      "  gLen " + st[8].toFixed(1) + "  speed " + simSpeed + "x" + (paused ? " [PAUSED]" : ""),
      10, 18);
    ctx.fillText("[Space] pause  [Up/Down] speed  [R] restart  [click] inspect", 10, 36);
  }

  function frame() {
    if (Module) {
      if (!paused) Module._evo_step(simSpeed);
      Module._evo_sync();
      draw(snapshot());
    }
    requestAnimationFrame(frame);
  }

  /* ---- input ---- */
  canvas.addEventListener("click", function (e) {
    if (!Module) return;
    var rect = canvas.getBoundingClientRect();
    var wx = (e.clientX - rect.left) * (canvas.width / rect.width);
    var wy = (e.clientY - rect.top) * (canvas.height / rect.height);
    var n = Module._evo_creature_count();
    var segs = new Float32Array(Module.HEAPF32.buffer, Module._evo_creature_segments(), n * 12);
    var ids = new Float64Array(Module.HEAPF64.buffer, Module._evo_creature_ids(), n);
    var best = -1, bestD2 = 25 * 25;
    for (var i = 0; i < n; i++) {
      var dx = segs[12 * i] - wx, dy = segs[12 * i + 1] - wy;
      var d2 = dx * dx + dy * dy;
      if (d2 <= bestD2) { bestD2 = d2; best = i; }
    }
    selectedId = best >= 0 ? ids[best] : null;
  });

  window.addEventListener("keydown", function (e) {
    var t = e.target;
    if (t && (t.tagName === "INPUT" || t.tagName === "BUTTON" || t.tagName === "SELECT")) return;
    if (e.code === "Space") {
      paused = !paused;
      e.preventDefault();
    } else if (e.key === "ArrowUp" || e.key === "+" || e.key === "=") {
      simSpeed = Math.min(50, simSpeed + 1);
      e.preventDefault();
    } else if (e.key === "ArrowDown" || e.key === "-") {
      simSpeed = Math.max(1, simSpeed - 1);
      e.preventDefault();
    } else if (e.key === "r" || e.key === "R") {
      el("seed").value = String((Math.random() * 0x100000000) >>> 0);
      restart();
    }
  });

  el("restart").addEventListener("click", function () { restart(); });

  /* ---- boot ---- */
  if (typeof createEvoBoids !== "function") {
    showError("evo_boids.js not found — build the wasm module into web/ first");
    return;
  }
  createEvoBoids().then(function (m) {
    Module = m;
    if (restart()) clearError();
    requestAnimationFrame(frame);
  }).catch(function (err) {
    showError("failed to load wasm module: " + err);
  });
})();
