"use strict";
/* evo_boids wasm renderer — canvas 2D front-end.
 * World: 1 unit = 1 px, origin top-left, +y down (canvas-native).
 * No interpolation, no trails, no wrap ghosts. Typed-array views are re-created
 * after every _evo_sync/_evo_inspect (memory growth invalidates buffers). */
(function () {
  var DEFAULT_W = 1920, DEFAULT_H = 1080, TAU = Math.PI * 2;
  var WORLD_W = DEFAULT_W, WORLD_H = DEFAULT_H; /* may change on checkpoint load */
  var RADII = [5, 3.5, 3, 2.5, 2, 1.5];
  var SEG_ALPHA = [255, 225, 195, 165, 135, 105].map(function (a) { return a / 255; });
  var CUR_TAIL = "rgba(110,170,215," + (60 / 255).toFixed(4) + ")";
  var CUR_HEAD = "rgba(150,205,235," + (130 / 255).toFixed(4) + ")";
  var RING_STYLE = "rgba(255,235,120," + (230 / 255).toFixed(4) + ")";
  var ACTION_NAMES = ["steerX", "steerY", "moveFwd", "moveRnd", "speed", "signal",
                      "oscPer", "resp", "share", "repro", "fight", "ignore"];

  /* Advanced config, grouped: [sectionName, [[key, default, {step,title,label,options}], ...]] */
  var ADV_SECTIONS = [
    ["food", [
      ["targetFood", 600], ["foodSpawnBase", 3], ["foodSpawnAmp", 2], ["foodEnergy", 5],
      ["foodPatchRadius", 120], ["foodDrift", false]
    ]],
    ["energy & life", [
      ["metabolicRate", 1.5], ["moveCost", 0, { step: 0.00001 }],
      ["geneCost", 0, { step: 0.001 }], ["maxAge", 120],
      ["reproEnergy", 20], ["newbornEnergy", 25]
    ]],
    ["motion", [
      ["minSpeed", 30], ["maxSpeed", 150], ["maxTurnRate", 4], ["triangularMotion", false]
    ]],
    ["perception & social", [
      ["perceptionRadius", 80], ["interactRadius", 20],
      ["maxNeighbors", 0, { step: 1, title: "0 = metric; N = topological cap" }]
    ]],
    ["genome", [
      ["mutationMultiplier", 1, { step: 0.1 }], ["genomeInitMin", 24], ["genomeInitMax", 48],
      ["genomeMinLength", 16], ["genomeMaxLength", 80]
    ]],
    ["predators", [
      ["predatorSpeed", 110], ["predatorPerception", 150], ["predatorCaptureRadius", 14],
      ["predatorMetabolism", 2], ["predatorReproEnergy", 60], ["predatorMaxAge", 0]
    ]],
    ["currents", [
      ["currentMode", 0, { options: ["eddies", "gyre"] }],
      ["currentEddies", 5], ["currentCore", 150], ["currentDrift", 1]
    ]],
    ["numerics", [
      ["fastTanh", false, { label: "fast tanh (changes the numbers)" }]
    ]]
  ];
  /* Flat view: readAdvanced/resetAdvanced iterate every key regardless of section. */
  var ADV = ADV_SECTIONS.reduce(function (acc, sec) { return acc.concat(sec[1]); }, []);

  /* Hover info per parameter: d = what it is, t = suggested values (docs/evo_boids_flags.md). */
  var TIPS = {
    seed: { d: "RNG seed — same seed + same settings replays the run bit-for-bit.", t: "try: any uint32; keep it to reproduce a run" },
    population: { d: "Starting creature count (the world reseeds below a floor of 50).", t: "try: 200-800; >2000 gets slow in-browser" },
    predators: { d: "Number of scripted hunters chasing prey (0 = none).", t: "try: 3-4 with predator economy on" },
    foodPatches: { d: "Cluster food into N spatial patches (0 = uniform scatter).", t: "try: 5-6; patchy food makes currents matter" },
    currentStrength: { d: "Peak ocean-flow speed in world units/sec (0 = still water).", t: "try: 80 with eddies = a lively ocean" },
    predatorEconomy: { d: "Predators eat, breed and starve (Stage B) instead of being immortal.", t: "try: on with 3-4 predators" },
    toroidal: { d: "Senses wrap across the world seam — no walls, no sensory dead zone at edges.", t: "try: on; cures the wall-hugger phenotype" },
    targetFood: { d: "Baseline food count the world regrows toward (hard cap 2x).", t: "try: 300 scarcity, 1200 abundance" },
    foodSpawnBase: { d: "Mean food items spawned per step.", t: "try: 2-12; higher = richer world" },
    foodSpawnAmp: { d: "Sinusoidal swing of food abundance around the base rate.", t: "try: 0 for a steady food supply" },
    foodEnergy: { d: "Energy gained per food item eaten.", t: "try: 3-8 around the default 5" },
    foodPatchRadius: { d: "Spread of food around each patch centre.", t: "try: 60 tight, 240 loose" },
    foodDrift: { d: "Food is advected with the current into drifting filaments.", t: "try: on — pairs well with currents" },
    metabolicRate: { d: "Fixed energy burned per second by every creature.", t: "try: toward 3 for harsher starvation" },
    moveCost: { d: "Kinetic cost: energy/sec = F x speed^2 — makes thrust expensive.", t: "try: 0.00007 -> cruise ~82; 0 = free" },
    geneCost: { d: "Extra energy/sec per gene — parsimony pressure against bloat.", t: "try: 0.005 makes big genomes costly" },
    maxAge: { d: "Age in seconds at which a creature dies of old age.", t: "try: 60-240 around the default 120" },
    reproEnergy: { d: "Energy a parent needs before it can reproduce.", t: "try: 15-40; higher = slower breeding" },
    newbornEnergy: { d: "Starting energy of a newborn child.", t: "try: 15-35 around the default 25" },
    minSpeed: { d: "Minimum swim speed.", t: "try: 0 when moveCost > 0 (drifting allowed)" },
    maxSpeed: { d: "Maximum swim speed — the sprint ceiling.", t: "try: 150; sprinting escapes predators at 110" },
    maxTurnRate: { d: "Turn ceiling (rad/s) at zero drive — triangular motion only.", t: "try: 2-6 around the default 4" },
    triangularMotion: { d: "Couple drive and turn: fast = straight, slow = agile.", t: "try: on for strike-vs-reorient bouts" },
    perceptionRadius: { d: "Sensing radius for neighbours, food and predators.", t: "try: 160 = high-density schooling regime" },
    interactRadius: { d: "Range for social interactions (share / fight).", t: "try: 10-40 around the default 20" },
    maxNeighbors: { d: "Attend to only the N nearest neighbours; 0 = everyone in range.", t: "try: 7 — starlings track ~6-7" },
    mutationMultiplier: { d: "Multiplier on the per-birth mutation rate.", t: "try: 0.5 conservative, 2 fast drift" },
    genomeInitMin: { d: "Low end of the initial gene-count range.", t: "try: init 24-48 inside bounds 16-80" },
    genomeInitMax: { d: "High end of the initial gene-count range.", t: "try: init 24-48 inside bounds 16-80" },
    genomeMinLength: { d: "Hard lower bound on gene count.", t: "try: 16 (the default)" },
    genomeMaxLength: { d: "Hard upper bound on gene count.", t: "try: 80 (the default)" },
    predatorSpeed: { d: "Predator cruise speed.", t: "try: 110 vs prey max 150 = escapable" },
    predatorPerception: { d: "Predator's prey-detection radius; above prey's 80 = ambush edge.", t: "try: 80 for symmetric sight" },
    predatorCaptureRadius: { d: "Contact distance at which a prey is caught.", t: "try: 10-20 around the default 14" },
    predatorMetabolism: { d: "Predator energy burn per second (economy on); prey burn 1.5.", t: "try: 2-3; higher = must hunt more" },
    predatorReproEnergy: { d: "Energy a predator needs to bud a child (economy on).", t: "try: 40-80; higher = slower breeding" },
    predatorMaxAge: { d: "Predator death age; 0 = immortal until starved.", t: "try: 0, or 120 to age them out" },
    currentMode: { d: "eddies = moving vortices; gyre = two counter-rotating basins.", t: "try: eddies with strength 80" },
    currentEddies: { d: "Number of vortices in eddies mode.", t: "try: 3-8 around the default 5" },
    currentCore: { d: "Eddy radius — the distance of peak flow speed.", t: "try: 100-250 around the default 150" },
    currentDrift: { d: "How fast the flow field evolves (0 = static).", t: "try: 0.5-2 around the default 1" },
    fastTanh: { d: "~1.25x faster tanh approximation — changes the numbers.", t: "try: off to reproduce native runs" }
  };

  var Module = null;
  var paused = false;
  var simSpeed = 1;
  var selectedId = null;

  var canvas = document.getElementById("canvas");
  var ctx = canvas.getContext("2d", { alpha: false });
  var bgImage = new Image();  /* background texture (web-only art, 1920x1080) */
  bgImage.src = "rocks.jpg";
  function el(id) { return document.getElementById(id); }
  var errEl = el("err");

  function showError(msg) { errEl.textContent = msg; errEl.style.color = "#e05a5a"; }
  function showNote(msg) { errEl.textContent = msg; errEl.style.color = "#8fa89a"; }
  function clearError() { errEl.textContent = ""; }

  function intVal(id, def) {
    var v = parseInt(el(id).value, 10);
    return Number.isFinite(v) ? v : def;
  }
  function floatVal(id, def) {
    var v = parseFloat(el(id).value);
    return Number.isFinite(v) ? v : def;
  }

  /* ---- advanced settings UI (generated from ADV_SECTIONS) ---- */
  function makeAdvInput(a) {
    var key = a[0], def = a[1], opt = a[2] || {};
    var label = document.createElement("label");
    if (opt.title) label.title = opt.title;
    var input;
    if (typeof def === "boolean") {
      input = document.createElement("input");
      input.type = "checkbox";
      input.checked = def;
    } else if (opt.options) {
      input = document.createElement("select");
      opt.options.forEach(function (name, i) {
        var o = document.createElement("option");
        o.value = String(i);
        o.textContent = i + " " + name;
        input.appendChild(o);
      });
      input.value = String(def);
    } else {
      input = document.createElement("input");
      input.type = "number";
      input.step = opt.step !== undefined ? String(opt.step) : "any";
      input.value = String(def);
    }
    input.id = "adv_" + key;
    var text = document.createElement("span");
    text.className = "lt";
    text.textContent = opt.label || key;
    label.appendChild(text);  /* name left, control in the right-aligned column */
    label.appendChild(input);
    attachTip(label, key);
    return label;
  }
  function buildAdvancedUI() {
    var root = el("advgrid"); /* now a vertical stack of section fieldsets */
    ADV_SECTIONS.forEach(function (sec) {
      var fs = document.createElement("fieldset");
      var lg = document.createElement("legend");
      lg.textContent = sec[0];
      fs.appendChild(lg);
      var grid = document.createElement("div");
      grid.className = "agrid";
      sec[1].forEach(function (a) { grid.appendChild(makeAdvInput(a)); });
      fs.appendChild(grid);
      root.appendChild(fs);
    });
  }
  function resetAdvanced() {
    ADV.forEach(function (a) {
      var input = el("adv_" + a[0]);
      if (typeof a[1] === "boolean") input.checked = a[1];
      else input.value = String(a[1]);
    });
  }
  function readAdvanced(cfg) {
    ADV.forEach(function (a) {
      var key = a[0], def = a[1], input = el("adv_" + key);
      if (typeof def === "boolean") {
        if (input.checked !== def) cfg[key] = input.checked;
      } else {
        var v = parseFloat(input.value);
        if (!Number.isFinite(v)) v = def;
        if (v !== def) cfg[key] = v;
      }
    });
  }

  /* Build config JSON with only the non-default keys. */
  function readForm() {
    var cfg = {};
    var predators = intVal("predators", 0);
    var foodPatches = intVal("foodPatches", 0);
    var currentStrength = floatVal("currentStrength", 0);
    if (el("toroidal").checked) cfg.toroidal = true;
    if (predators !== 0) cfg.predatorCount = predators;
    if (el("predatorEconomy").checked) cfg.predatorEconomy = true;
    if (currentStrength !== 0) cfg.currentStrength = currentStrength;
    if (foodPatches !== 0) cfg.foodPatches = foodPatches;
    readAdvanced(cfg);
    return { seed: intVal("seed", 42) >>> 0, population: intVal("population", 300), cfg: cfg };
  }

  function withCString(str, fn) {
    var size = Module.lengthBytesUTF8(str) + 1;
    var ptr = Module._malloc(size);
    Module.stringToUTF8(str, ptr, size);
    var r = fn(ptr);
    Module._free(ptr);
    return r;
  }

  function restart() {
    if (!Module) return false;
    clearError();
    var f = readForm();
    WORLD_W = DEFAULT_W; WORLD_H = DEFAULT_H;
    if (canvas.width !== WORLD_W) canvas.width = WORLD_W;
    if (canvas.height !== WORLD_H) canvas.height = WORLD_H;
    var ok;
    if (Object.keys(f.cfg).length > 0) {
      ok = withCString(JSON.stringify(f.cfg), function (ptr) {
        return Module._evo_init(f.seed, WORLD_W, WORLD_H, f.population, ptr);
      });
    } else {
      ok = Module._evo_init(f.seed, WORLD_W, WORLD_H, f.population, 0);
    }
    selectedId = null;
    if (!ok) showError("evo_init failed — check parameters");
    return !!ok;
  }

  /* ---- per-frame data: resolve selection + _evo_inspect BEFORE creating views ---- */
  function resolveSelection() {
    if (selectedId === null) return -1;
    var n = Module._evo_creature_count();
    var ids = new Float64Array(Module.HEAPF64.buffer, Module._evo_creature_ids(), n);
    var idx = -1;
    for (var i = 0; i < n; i++) if (ids[i] === selectedId) { idx = i; break; }
    if (idx >= 0 && !Module._evo_inspect(idx)) idx = -1;
    if (idx < 0) selectedId = null;
    return idx;
  }

  function snapshot(selIdx) {
    var n = Module._evo_creature_count();
    var fc = Module._evo_food_count();
    var pc = Module._evo_predator_count();
    var s = {
      n: n, fc: fc, pc: pc, selIdx: selIdx,
      segs: new Float32Array(Module.HEAPF32.buffer, Module._evo_creature_segments(), n * 12),
      colors: new Uint8Array(Module.HEAPU8.buffer, Module._evo_creature_colors(), n * 3),
      food: new Float32Array(Module.HEAPF32.buffer, Module._evo_food_xy(), fc * 2),
      preds: new Float32Array(Module.HEAPF32.buffer, Module._evo_predator_data(), pc * 3),
      stats: new Float64Array(Module.HEAPF64.buffer, Module._evo_stats(), 9),
      curActive: Module._evo_current_active() !== 0,
      curGrid: null, curCols: 0, curRows: 0
    };
    if (s.curActive) {
      s.curCols = Module._evo_current_cols();
      s.curRows = Module._evo_current_rows();
      s.curGrid = new Float32Array(Module.HEAPF32.buffer, Module._evo_current_grid(),
                                   s.curCols * s.curRows * 2);
    }
    if (selIdx >= 0) {
      s.info = new Float64Array(Module.HEAPF64.buffer, Module._evo_inspect_info(), 4);
      s.sensors = new Float32Array(Module.HEAPF32.buffer, Module._evo_inspect_sensors(), 41);
      s.hidden = new Float32Array(Module.HEAPF32.buffer, Module._evo_inspect_hidden(), 12);
      s.actions = new Float32Array(Module.HEAPF32.buffer, Module._evo_inspect_actions(), 12);
      s.connCount = Module._evo_inspect_conn_count();
      s.conns = new Float32Array(Module.HEAPF32.buffer, Module._evo_inspect_conns(), s.connCount * 5);
    }
    return s;
  }

  /* ---- neural inspector panel (drawn last, above HUD) ---- */
  function colY(i, n, yTop, yBot) {
    return n <= 1 ? (yTop + yBot) / 2 : yTop + (yBot - yTop) * i / (n - 1);
  }
  function activationColor(v) {
    var m = Math.min(1, Math.abs(v));
    var lo = 45;
    var hot = lo + Math.round((235 - lo) * m);
    var mid = Math.round(lo + (v >= 0 ? 60 : 90) * (1 - m));
    return v >= 0 ? "rgb(" + hot + "," + mid + "," + lo + ")"
                  : "rgb(" + lo + "," + mid + "," + hot + ")";
  }
  function drawInspector(s) {
    var px = WORLD_W - 340 - 12, py = 44, pw = 340, ph = 560;
    ctx.fillStyle = "rgba(10,22,30," + (205 / 255).toFixed(4) + ")";
    ctx.fillRect(px, py, pw, ph);
    ctx.strokeStyle = "rgba(90,140,150," + (220 / 255).toFixed(4) + ")";
    ctx.lineWidth = 1;
    ctx.strokeRect(px + 0.5, py + 0.5, pw - 1, ph - 1);
    var xS = px + 30, xH = px + 170, xA = px + 310;
    var yTop = py + 46, yBot = py + ph - 22;
    var i;

    /* 1. connections first, under nodes */
    ctx.lineWidth = 1;
    for (var k = 0; k < s.connCount; k++) {
      var o = k * 5;
      var srcSen = s.conns[o] !== 0;
      var srcNum = s.conns[o + 1] | 0;
      var sinkAct = s.conns[o + 2] !== 0;
      var sinkNum = s.conns[o + 3] | 0;
      var w = s.conns[o + 4];
      var ax = srcSen ? xS : xH;
      var ay = srcSen ? colY(srcNum, 41, yTop, yBot) : colY(srcNum, 12, yTop, yBot);
      var bx = sinkAct ? xA : xH;
      var by = colY(sinkNum, 12, yTop, yBot);
      var srcAct = srcSen ? s.sensors[srcNum] : s.hidden[srcNum];
      var mag = Math.min(1, Math.abs(w) * Math.abs(srcAct) * 1.5);
      var alpha = ((25 + Math.round(180 * mag)) / 255).toFixed(4);
      ctx.strokeStyle = (w >= 0 ? "rgba(120,200,130," : "rgba(210,120,120,") + alpha + ")";
      ctx.beginPath(); ctx.moveTo(ax, ay); ctx.lineTo(bx, by); ctx.stroke();
    }

    /* 2. nodes */
    function node(x, y, r, v) {
      ctx.fillStyle = activationColor(v);
      ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); ctx.fill();
      ctx.strokeStyle = "rgba(0,0,0," + (160 / 255).toFixed(4) + ")";
      ctx.lineWidth = 1;
      ctx.stroke();
    }
    for (i = 0; i < 41; i++) node(xS, colY(i, 41, yTop, yBot), 3, s.sensors[i]);
    for (i = 0; i < 12; i++) node(xH, colY(i, 12, yTop, yBot), 5, s.hidden[i]);
    for (i = 0; i < 12; i++) node(xA, colY(i, 12, yTop, yBot), 5, s.actions[i]);

    /* 3. text */
    ctx.font = "13px monospace";
    ctx.fillStyle = "rgba(220,235,240," + (235 / 255).toFixed(4) + ")";
    ctx.fillText("Fish #" + Math.round(s.info[0]) + "   spd " + Math.round(s.info[1]) +
                 "   E " + Math.round(s.info[2]) + "   age " + Math.round(s.info[3]),
                 px + 12, py + 19);
    ctx.font = "11px monospace";
    ctx.fillStyle = "rgba(150,175,180," + (200 / 255).toFixed(4) + ")";
    var hy = yTop - 18 + 11; /* text top at yTop-18, ~11px to baseline */
    ctx.fillText("in", xS - 8, hy);
    ctx.fillText("hidden", xH - 20, hy);
    ctx.fillText("out", xA - 12, hy);
    ctx.font = "10px monospace";
    ctx.fillStyle = "rgba(190,205,210," + (220 / 255).toFixed(4) + ")";
    for (i = 0; i < 12; i++) ctx.fillText(ACTION_NAMES[i], xA + 8, colY(i, 12, yTop, yBot) + 3);
  }

  function draw(s) {
    ctx.fillStyle = "#0F1E19";
    ctx.fillRect(0, 0, WORLD_W, WORLD_H);
    /* Background texture under everything, cover-scaled to the world (works at
     * any image resolution and after a checkpoint changes the world size); the
     * flat clear colour stays as the fallback while it loads or if missing. */
    if (bgImage.complete && bgImage.naturalWidth > 0) {
      var bs = Math.max(WORLD_W / bgImage.naturalWidth, WORLD_H / bgImage.naturalHeight);
      var bw = bgImage.naturalWidth * bs, bh = bgImage.naturalHeight * bs;
      ctx.drawImage(bgImage, (WORLD_W - bw) / 2, (WORLD_H - bh) / 2, bw, bh);
    }

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
          ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke();
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

    if (s.selIdx >= 0) {
      ctx.strokeStyle = RING_STYLE;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(s.segs[12 * s.selIdx], s.segs[12 * s.selIdx + 1], 16, 0, TAU);
      ctx.stroke();
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

    if (s.selIdx >= 0) drawInspector(s); /* last: above HUD */
  }

  function frame() {
    if (Module) {
      if (!paused) Module._evo_step(simSpeed);
      Module._evo_sync();
      draw(snapshot(resolveSelection()));
    }
    requestAnimationFrame(frame);
  }

  /* ---- control state helpers ---- */
  function setPaused(p) {
    paused = p;
    el("pausebtn").textContent = paused ? "Resume" : "Pause";
  }
  function setSpeed(n) {
    simSpeed = Math.max(1, Math.min(50, n));
    el("speedlabel").textContent = "speed " + simSpeed + "x";
  }

  /* ---- checkpoint save/load ---- */
  function saveCheckpoint() {
    if (!Module) return;
    var str = Module.UTF8ToString(Module._evo_checkpoint());
    var frameNo = Math.round(new Float64Array(Module.HEAPF64.buffer, Module._evo_stats(), 9)[3]);
    var blob = new Blob([str], { type: "application/json" });
    var url = URL.createObjectURL(blob);
    var a = document.createElement("a");
    a.href = url;
    a.download = "evo_checkpoint_f" + frameNo + ".json";
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  }
  function loadCheckpointText(text) {
    var ok = withCString(text, function (ptr) { return Module._evo_load_checkpoint(ptr); });
    if (!ok) {
      showError("checkpoint load failed: " + Module.UTF8ToString(Module._evo_last_error()));
      return;
    }
    selectedId = null;
    var w = Module._evo_width(), h = Module._evo_height();
    if (w !== WORLD_W || h !== WORLD_H) {
      WORLD_W = w; WORLD_H = h;
      canvas.width = w; canvas.height = h;
    }
    showNote("(checkpoint loaded)");
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
    /* Escape always closes the drawer, regardless of what has focus —
     * blurring the focused control so the sim shortcuts work again. */
    if (e.key === "Escape") {
      setDrawer(false);
      if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
      return;
    }
    var t = e.target;
    if (t && (t.tagName === "INPUT" || t.tagName === "BUTTON" ||
              t.tagName === "SELECT" || t.tagName === "SUMMARY")) return;
    if (e.code === "Space") {
      setPaused(!paused);
      e.preventDefault();
    } else if (e.key === "ArrowUp" || e.key === "+" || e.key === "=") {
      setSpeed(simSpeed + 1);
      e.preventDefault();
    } else if (e.key === "ArrowDown" || e.key === "-") {
      setSpeed(simSpeed - 1);
      e.preventDefault();
    } else if (e.key === "r" || e.key === "R") {
      el("seed").value = String((Math.random() * 0x100000000) >>> 0);
      restart();
    }
  });

  /* ---- side drawer ---- */
  var drawerEl = el("drawer"), menuBtn = el("menubtn");
  function setDrawer(open) {
    drawerEl.classList.toggle("open", open);
    menuBtn.textContent = open ? "✕" : "☰";
    if (!open) hideTip();
  }
  menuBtn.addEventListener("click", function () {
    setDrawer(!drawerEl.classList.contains("open"));
    this.blur();
  });

  /* ---- shared hover tooltip (one fixed div, styled in index.html) ---- */
  var tipEl = document.createElement("div");
  tipEl.id = "tooltip";
  var tipDesc = document.createElement("div");
  tipDesc.className = "tipd";
  var tipTry = document.createElement("div");
  tipTry.className = "tipt";
  tipEl.appendChild(tipDesc);
  tipEl.appendChild(tipTry);
  document.body.appendChild(tipEl);
  var tipTimer = 0;
  function hideTip() {
    clearTimeout(tipTimer);
    tipEl.style.display = "none";
  }
  function showTip(labelEl, tip) {
    tipDesc.textContent = tip.d;
    tipTry.textContent = tip.t;
    tipEl.style.display = "block";
    /* to the LEFT of the drawer (outside its clipping), centred on the label */
    var dr = drawerEl.getBoundingClientRect();
    var lr = labelEl.getBoundingClientRect();
    var left = Math.max(6, dr.left - 10 - tipEl.offsetWidth);
    var top = lr.top + lr.height / 2 - tipEl.offsetHeight / 2;
    top = Math.max(6, Math.min(top, window.innerHeight - tipEl.offsetHeight - 6));
    tipEl.style.left = left + "px";
    tipEl.style.top = top + "px";
  }
  function attachTip(labelEl, key) {
    var tip = TIPS[key];
    if (!tip) return;
    labelEl.addEventListener("mouseenter", function () {
      clearTimeout(tipTimer);
      tipTimer = setTimeout(function () { showTip(labelEl, tip); }, 150);
    });
    labelEl.addEventListener("mouseleave", hideTip);
  }
  drawerEl.addEventListener("scroll", hideTip);
  /* basic (static-HTML) parameter labels, resolved via their input ids */
  ["seed", "population", "toroidal", "predators", "foodPatches", "currentStrength", "predatorEconomy"]
    .forEach(function (id) {
      var l = el(id).closest("label");
      if (l) attachTip(l, id);
    });

  function btn(id, fn) {
    el(id).addEventListener("click", function () { fn(); this.blur(); });
  }
  btn("restart", restart);
  btn("pausebtn", function () { setPaused(!paused); });
  btn("slower", function () { setSpeed(simSpeed - 1); });
  btn("faster", function () { setSpeed(simSpeed + 1); });
  btn("advreset", resetAdvanced);
  btn("savecp", saveCheckpoint);
  btn("loadcp", function () { el("cpfile").click(); });
  el("cpfile").addEventListener("change", function () {
    var file = this.files && this.files[0];
    this.value = "";
    if (!file || !Module) return;
    file.text().then(loadCheckpointText,
                     function (err) { showError("could not read file: " + err); });
  });

  buildAdvancedUI();
  setSpeed(simSpeed);

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
