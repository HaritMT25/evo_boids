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
  /* Drawn radius = RADII[seg] * bodyScale(bodySize), floored at MIN_RADIUS so a
   * small body still has a visible tail. bodySize is core-clamped to [0.5, 2],
   * but its realised standing spread is narrow (sd ~0.08 about 1.0), so a plain
   * multiply would separate two typical neighbours by a couple of tenths of a
   * pixel. tanh gives slope BODY_SPAN*BODY_GAIN = 2 where the population
   * actually sits and saturates at +/-BODY_SPAN, so the rails stay sane — a 2.0
   * body draws 1.4x, not 2x. tanh(0) is exactly 0, so a default 1.0 body
   * multiplies by exactly 1.0 and draws the pixels it always did. */
  var BODY_GAIN = 5, BODY_SPAN = 0.4, MIN_RADIUS = 1;
  /* Same formula as SfmlRenderer::drawCreatures — the two frontends must match. */
  function bodyScale(b) {
    if (!(b > 0)) return 1;  /* absent buffer (older wasm blob) or NaN */
    return 1 + BODY_SPAN * Math.tanh(BODY_GAIN * (b - 1));
  }
  /* evo_inspect_info(): id, speed, foodReserves, age, bodySize, bodyElong, generation. */
  var INSPECT_INFO_LEN = 7;
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
    ["population", [
      ["maxPopulation", 0, { step: 1, min: 0, label: "population cap (0 = none)" }],
      ["minPopulation", 50, { step: 1, min: 0, label: "reseed floor" }]
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
      ["genomeMinLength", 16], ["genomeMaxLength", 80],
      ["reproMaxDistance", -1, { step: 0.5, label: "repro max distance (-1 = off)",
                                 title: "genetic reproduction gate; -1 disables it" }],
      ["reproSoftWidth", 0, { step: 0.5, min: 0, label: "repro soft width (0 = hard gate)",
                              title: "acceptance ramp above the gate distance; needs the gate on" }],
      ["reproGracePeriod", 0, { step: 10, min: 0, label: "repro grace (0 = off)",
                                title: "young founders skip the gate; needs the gate on" }],
      ["bodyMutation", 0.1, { step: 0.01, min: 0, label: "body mutation (body model on)",
                              title: "uniform half-width per body parameter per birth" }]
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
  /* Keys the running world accepts mid-run (evo_set_param's whitelist in
   * wasm/evo_wasm.cpp). Editing one applies immediately; every other key
   * applies on Restart. */
  var LIVE = { maxPopulation: 1, minPopulation: 1, targetFood: 1, foodSpawnBase: 1,
               foodSpawnAmp: 1, foodEnergy: 1, metabolicRate: 1, moveCost: 1, geneCost: 1,
               maxAge: 1, reproEnergy: 1, newbornEnergy: 1, mutationMultiplier: 1,
               minSpeed: 1, maxSpeed: 1 };

  /* Hover info per parameter: d = what it is, t = suggested values (docs/evo_boids_flags.md). */
  var TIPS = {
    seed: { d: "RNG seed — same seed + same settings replays the run bit-for-bit.", t: "try: any uint32; keep it to reproduce a run" },
    population: { d: "Starting creature count (the world reseeds below a floor of 50).", t: "try: 200-800; >2000 gets slow in-browser" },
    predators: { d: "Number of scripted hunters chasing prey (0 = none).", t: "try: 3-4 with predator economy on" },
    foodPatches: { d: "Cluster food into N spatial patches (0 = uniform scatter).", t: "try: 5-6; patchy food makes currents matter" },
    currentStrength: { d: "Peak ocean-flow speed in world units/sec (0 = still water).", t: "try: 80 with eddies = a lively ocean" },
    predatorEconomy: { d: "Predators eat, breed and starve (Stage B) instead of being immortal.", t: "try: on with 3-4 predators" },
    toroidal: { d: "Senses wrap across the world seam — no walls, no sensory dead zone at edges.", t: "try: on; cures the wall-hugger phenotype" },
    reproMaxDistance: { d: "Reproductive isolation: parents farther apart than this in wiring space cannot breed (-1 = off).", t: "try: 18 bites gently, 14 hard; below 12 breeding stops" },
    reproSoftWidth: { d: "Softens the isolation gate into a ramp: past the gate distance breeding fades out over this width instead of stopping dead (0 = hard cliff).", t: "try: 4-5 with the gate at 14-16; needs the gate on" },
    reproGracePeriod: { d: "Founders — the first generation and every reseeded refill — ignore the gate while younger than this, so a fresh genome can breed once (0 = off).", t: "try: 60; 5 or below does nothing (breeding already needs age > 5)" },
    bodyModel: { d: "Heritable body size + elongation: bigger = slower and costlier, longer = turns worse.", t: "try: on; founders start at 1.0 and bodies drift from births" },
    bodyMutation: { d: "Body-model only: uniform jitter half-width added to each inherited body parameter.", t: "try: 0.1 default; 0 freezes drift, 0.3 explores fast" },
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
    fastTanh: { d: "~1.25x faster tanh approximation — changes the numbers.", t: "try: off to reproduce native runs" },
    maxPopulation: { d: "Birth cap: no births while the population is at or above it (0 = none). Applies live.", t: "try: 0; the frame-budget governor may set a lower one" },
    minPopulation: { d: "Reseed floor: random founders are added when the population falls below it. Applies live.", t: "try: 50 (the default)" },
    governor: { d: "When frames take too long: first lower the speed, then remove creatures at random (no trait favoured) and cap births at what this machine can run at 60 fps. The status line reports each step; a governed run no longer replays from its seed.", t: "try: on; off to see the uncapped run (it may stutter)" }
  };

  var ASSETS = window.EVO_ASSETS;
  var Module = null;
  var threaded = false;      /* which module EvoLoader picked */
  var threadWhy = "";
  var moduleUrls = null;     /* {js, wasm} of the loaded module, for the Turbo worker */
  var STATS_LEN = 9;         /* replaced by _evo_stats_len() once loaded */
  var paused = false;
  var simSpeed = 1;
  var selectedId = null;

  var canvas = document.getElementById("canvas");
  var ctx = canvas.getContext("2d", { alpha: false });
  var bgImage = new Image();  /* background texture — shared with the native app (img/rocks.jpg) */
  bgImage.src = ASSETS.background;
  function el(id) { return document.getElementById(id); }
  var errEl = el("err");
  var statusEl = el("status");

  /* The wasm heap. In the threaded module it is a growable SharedArrayBuffer,
   * and a growth triggered on another thread leaves Module.HEAP* pointing at
   * the old, shorter buffer — so views are always cut from wasmMemory.buffer,
   * which reflects the current length. Re-created per frame, never cached. */
  function heap() {
    return Module.wasmMemory ? Module.wasmMemory.buffer : Module.HEAPU8.buffer;
  }

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
      /* A lower bound the spinner has no reason to go below: either evo_init
         refuses the value rather than clamping it (bodyMutation), or everything
         under the bound is a synonym for it (a <=0 knob that means "off"). */
      if (opt.min !== undefined) input.min = String(opt.min);
      input.value = String(def);
    }
    input.id = "adv_" + key;
    var text = document.createElement("span");
    text.className = "lt";
    text.textContent = opt.label || key;
    if (LIVE[key]) {
      var tag = document.createElement("span");
      tag.className = "livetag";
      tag.textContent = "live";
      text.appendChild(tag);
      input.addEventListener("change", function () { applyLive(key, def, input); });
    }
    label.appendChild(text);  /* name left, control in the right-aligned column */
    label.appendChild(input);
    attachTip(label, key);
    return label;
  }
  /* Push one live-capable knob into the running world. maxPopulation goes
   * through the governor so a user cap and the frame-budget cap combine. */
  function applyLive(key, def, input) {
    if (!Module || turboState !== "live") return;
    var v = parseFloat(input.value);
    if (!Number.isFinite(v)) v = def;
    if (key === "maxPopulation") { userCap = Math.max(0, Math.round(v)); applyCap(); return; }
    if (!Module._evo_set_param) return;
    var ok = withCString(key, function (ptr) { return Module._evo_set_param(ptr, v); });
    if (ok) showNote("(" + key + " = " + v + ", live)");
    else showError(Module.UTF8ToString(Module._evo_last_error()));
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
    if (el("bodyModel").checked) cfg.bodyModel = true;
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
    if (guardTurbo()) return false;
    clearError();
    var f = readForm();
    WORLD_W = DEFAULT_W; WORLD_H = DEFAULT_H;
    if (canvas.width !== WORLD_W) canvas.width = WORLD_W;
    if (canvas.height !== WORLD_H) canvas.height = WORLD_H;
    return initWorld(f.seed, f.population, f.cfg);
  }
  function initWorld(seed, population, cfg) {
    var ok;
    if (Object.keys(cfg).length > 0) {
      ok = withCString(JSON.stringify(cfg), function (ptr) {
        return Module._evo_init(seed, WORLD_W, WORLD_H, population, ptr);
      });
    } else {
      ok = Module._evo_init(seed, WORLD_W, WORLD_H, population, 0);
    }
    selectedId = null;
    userCap = cfg.maxPopulation || 0;
    autoCap = 0;
    resetGovernor();
    if (!ok) showError("evo_init failed: " + Module.UTF8ToString(Module._evo_last_error()));
    return !!ok;
  }

  /* ---- per-frame data: resolve selection + _evo_inspect BEFORE creating views ---- */
  function resolveSelection() {
    if (selectedId === null) return -1;
    var n = Module._evo_creature_count();
    var ids = new Float64Array(heap(), Module._evo_creature_ids(), n);
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
      segs: new Float32Array(heap(), Module._evo_creature_segments(), n * 12),
      /* N*2 alongside segs, same creature order; absent from pre-0.24 blobs */
      body: Module._evo_body_params
              ? new Float32Array(heap(), Module._evo_body_params(), n * 2)
              : null,
      colors: new Uint8Array(heap(), Module._evo_creature_colors(), n * 3),
      food: new Float32Array(heap(), Module._evo_food_xy(), fc * 2),
      preds: new Float32Array(heap(), Module._evo_predator_data(), pc * 3),
      stats: new Float64Array(heap(), Module._evo_stats(), STATS_LEN),
      curActive: Module._evo_current_active() !== 0,
      curGrid: null, curCols: 0, curRows: 0
    };
    if (s.curActive) {
      s.curCols = Module._evo_current_cols();
      s.curRows = Module._evo_current_rows();
      s.curGrid = new Float32Array(heap(), Module._evo_current_grid(),
                                   s.curCols * s.curRows * 2);
    }
    if (selIdx >= 0) {
      s.info = new Float64Array(heap(), Module._evo_inspect_info(), INSPECT_INFO_LEN);
      s.sensors = new Float32Array(heap(), Module._evo_inspect_sensors(), 41);
      s.hidden = new Float32Array(heap(), Module._evo_inspect_hidden(), 12);
      s.actions = new Float32Array(heap(), Module._evo_inspect_actions(), 12);
      s.connCount = Module._evo_inspect_conn_count();
      s.conns = new Float32Array(heap(), Module._evo_inspect_conns(), s.connCount * 5);
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
  /* Canvas x of the drawer's left edge (WORLD_W when it is closed or off the
   * canvas). The drawer is a fixed overlay on the right; the inspector's
   * default home is the canvas's top-right, so it has to step aside. */
  function drawerLeftInCanvas() {
    if (!drawerEl.classList.contains("open")) return WORLD_W;
    var cr = canvas.getBoundingClientRect();
    if (cr.width <= 0) return WORLD_W;
    var x = (drawerEl.getBoundingClientRect().left - cr.left) * (canvas.width / cr.width);
    return Math.min(WORLD_W, x);
  }
  function drawInspector(s) {
    var pw = 340, ph = 560, py = 44;
    /* top-right by default; with the drawer open, just left of its edge
     * (follows it while it slides), never off the canvas's left side */
    var px = Math.max(12, Math.min(WORLD_W - pw - 12, drawerLeftInCanvas() - pw - 12));
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
    /* Body params get their own line at the panel foot — appended to the title
     * they would run past the 340px panel. Always drawn: the page has no view of
     * cfg.bodyModel, and a fixed 1.00 / 1.00 is itself the readout for a run
     * with the model off. */
    ctx.font = "11px monospace";
    ctx.fillStyle = "rgba(190,205,210," + (220 / 255).toFixed(4) + ")";
    ctx.fillText("body " + s.info[4].toFixed(2) + "   elong " + s.info[5].toFixed(2) +
                 "   gen " + Math.round(s.info[6]),
                 px + 12, py + ph - 8);
    ctx.fillStyle = "rgba(150,175,180," + (200 / 255).toFixed(4) + ")";
    var hy = yTop - 18 + 11; /* text top at yTop-18, ~11px to baseline */
    ctx.fillText("in", xS - 8, hy);
    ctx.fillText("hidden", xH - 20, hy);
    ctx.fillText("out", xA - 12, hy);
    ctx.font = "10px monospace";
    ctx.fillStyle = "rgba(190,205,210," + (220 / 255).toFixed(4) + ")";
    for (i = 0; i < 12; i++) ctx.fillText(ACTION_NAMES[i], xA + 8, colY(i, 12, yTop, yBot) + 3);
  }

  /* Scene layer (background, currents, food, creatures, predators): WebGL2
   * when available (web/gl_renderer.js; one instanced draw per kind), else the
   * 2D path below. Both read the same constants, so they draw the same scene;
   * ?gl=0 forces 2D. Overlays (ring, HUD, inspector) are always 2D, on top. */
  var LOOK = {
    radii: RADII, segAlpha: SEG_ALPHA, minRadius: MIN_RADIUS, bodyScale: bodyScale,
    clear: [0x0F / 255, 0x1E / 255, 0x19 / 255],
    food: [0x97 / 255, 0xC6 / 255, 0x32 / 255],
    predDigesting: [150 / 255, 60 / 255, 60 / 255],
    predHunting: [220 / 255, 50 / 255, 50 / 255],
    curTail: [110 / 255, 170 / 255, 215 / 255, 60 / 255],
    curHead: [150 / 255, 205 / 255, 235 / 255, 130 / 255]
  };
  var glScene = null;
  if (window.EvoGL && new URLSearchParams(location.search).get("gl") !== "0") {
    glScene = EvoGL.create(WORLD_W, WORLD_H, bgImage, LOOK);
  }

  function draw(s) {
    if (glScene) glScene.resize(WORLD_W, WORLD_H);  /* world size changes on checkpoint load */
    var layer = glScene ? glScene.draw(s, WORLD_W, WORLD_H) : null;
    if (layer) ctx.drawImage(layer, 0, 0);
    else drawScene2D(s);
    drawOverlays(s);
  }

  function drawScene2D(s) {
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
      var bs = bodyScale(s.body ? s.body[2 * c] : 1);
      for (var seg = 0; seg < 6; seg++) {
        ctx.fillStyle = "rgba(" + rgb + "," + SEG_ALPHA[seg] + ")";
        ctx.beginPath();
        ctx.arc(s.segs[12 * c + 2 * seg], s.segs[12 * c + 2 * seg + 1],
                Math.max(MIN_RADIUS, RADII[seg] * bs), 0, TAU);
        ctx.fill();
      }
    }

    for (var p = 0; p < s.pc; p++) {
      ctx.fillStyle = s.preds[3 * p + 2] > 0 ? "rgb(150,60,60)" : "rgb(220,50,50)";
      ctx.beginPath();
      ctx.arc(s.preds[3 * p], s.preds[3 * p + 1], 6.5, 0, TAU);
      ctx.fill();
    }
  }

  function drawOverlays(s) {
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
      "  gLen " + st[8].toFixed(1) +
      (STATS_LEN > 10 ? "  gen " + fmt(st[10]) + " (avg " + st[9].toFixed(1) + ")" : "") +
      "  speed " + simSpeed + "x" + (paused ? " [PAUSED]" : ""),
      10, 18);
    ctx.fillText("[Space] pause  [Up/Down] speed  [R] restart  [click] inspect", 10, 36);

    if (s.selIdx >= 0) drawInspector(s); /* last: above HUD */
  }

  /* ---- frame budget: degrade agent count rather than stutter ----
   * workMs = step + sync + draw for one frame. When its moving average stays
   * over BUDGET_MS (of a 16.7 ms frame), first the speed steps back toward 1x;
   * at 1x a target count is sized from the measured cost per creature, the
   * surplus is removed uniformly at random (_evo_cull_to: favours no trait),
   * and a birth cap (maxPopulation) holds it there. A birth cap alone is too
   * slow — it only bites as creatures die, which takes up to maxAge (120 sim
   * seconds). With sustained headroom and the population pressing on the cap,
   * the cap is raised again. A governed run no longer replays from its seed;
   * the status line says so, and the governor can be switched off. */
  var BUDGET_MS = 12;
  var MIN_CAP = 60;
  /* Starting population per module (README.md, "Benchmarks"). */
  var DEFAULT_POP_SINGLE = 300, DEFAULT_POP_THREADED = 600;
  var workEma = 0, overFrames = 0, underFrames = 0;
  var userCap = 0, autoCap = 0;
  var governorOn = true;
  var govNote = "";
  var governed = false;   /* the governor has changed this run */
  /* Frames ignored after a (re)start: the first ~2 s include JIT warm-up and
   * buffer growth, which would otherwise read as "this machine is too slow". */
  var WARMUP_FRAMES = 120, warmup = WARMUP_FRAMES;
  function resetGovernor() {
    workEma = 0; overFrames = 0; underFrames = 0; govNote = ""; governed = false;
    warmup = WARMUP_FRAMES;
  }
  function applyCap() {
    if (!Module || !Module._evo_set_param) return;
    var eff = userCap > 0 && autoCap > 0 ? Math.min(userCap, autoCap) : (userCap || autoCap);
    withCString("maxPopulation", function (ptr) { return Module._evo_set_param(ptr, eff); });
  }
  function govern(workMs, pop) {
    if (warmup > 0) { warmup--; return; }
    workEma = workEma ? workEma * 0.9 + workMs * 0.1 : workMs;
    if (!governorOn || paused || turboState !== "live" || bench) return;
    if (workEma > BUDGET_MS) { overFrames++; underFrames = 0; }
    else if (workEma < BUDGET_MS * 0.5) { underFrames++; overFrames = 0; }
    else { overFrames = 0; underFrames = 0; }
    if (overFrames >= 30) {
      overFrames = 0;
      if (simSpeed > 1) {
        setSpeed(simSpeed - 1);
        govNote = "speed lowered to " + simSpeed + "x to hold 60 fps";
      } else if (!autoCap || pop <= autoCap * 1.02) {
        /* only tighten once the population has come down to the last cap */
        var target = Math.max(MIN_CAP, Math.floor(pop * (BUDGET_MS * 0.8) / workEma));
        if (!autoCap || target < autoCap) {
          autoCap = target;
          applyCap();
          var removed = Module._evo_cull_to ? Module._evo_cull_to(autoCap) : 0;
          selectedId = null;  /* the inspected creature may be gone */
          governed = true;
          govNote = (removed ? "removed " + removed + " at random, " : "") +
                    "capped at " + autoCap + " to hold 60 fps";
        }
      }
    } else if (underFrames >= 180 && autoCap && pop >= autoCap * 0.95) {
      underFrames = 0;
      autoCap = Math.round(autoCap * 1.15);
      applyCap();
      govNote = "population cap raised to " + autoCap;
    }
  }

  /* ---- status line (DOM, readable at any canvas scale) ---- */
  var fpsFrames = 0, fpsT0 = 0, fps = 0;
  var stepEma = 0;   /* ms per sim step, for the status line */
  function updateStatus(st) {
    if (!statusEl) return;
    var mode = threaded
      ? "threaded × " + (Module._evo_threads ? Module._evo_threads() : "?")
      : "single-threaded";
    var parts = [
      fps + " fps",
      Math.round(st[0]) + " agents",
      STATS_LEN > 10 ? "gen " + Math.round(st[10]) + " (avg " + st[9].toFixed(1) + ")" : "",
      stepEma ? stepEma.toFixed(2) + " ms/step" : "",
      mode
    ];
    var cap = userCap > 0 && autoCap > 0 ? Math.min(userCap, autoCap) : (userCap || autoCap);
    if (cap) parts.push("cap " + cap + (autoCap && cap === autoCap ? " (auto)" : ""));
    if (govNote) parts.push(govNote);
    if (governed) parts.push("governed run: not replayable from its seed");
    statusEl.textContent = parts.filter(Boolean).join("  ·  ");
    statusEl.title = threaded ? "running the pthreads module: fast, NOT reproducible from the seed"
                              : "running the single-threaded module (" + threadWhy + "): reproducible from the seed";
  }

  function frame(now) {
    if (Module) {
      var t0 = performance.now();
      var stepped = 0;
      if (turboState === "live" && !paused) {
        stepped = bench ? 1 : simSpeed;
        Module._evo_step(stepped);
      }
      var t1 = performance.now();
      /* while not 'live' the main sim is frozen: re-sync repacks the same
       * state, so this keeps drawing the last frame (never stale-black) */
      Module._evo_sync();
      var s = snapshot(turboState === "live" ? resolveSelection() : -1);
      draw(s);
      if (turboState !== "live") drawTurboOverlay();
      var t2 = performance.now();
      if (stepped) {
        var per = (t1 - t0) / stepped;
        stepEma = stepEma ? stepEma * 0.9 + per * 0.1 : per;
      }
      govern(t2 - t0, s.n);
      if (bench) benchFrame(now, t1 - t0, t2 - t1, s.n);
      fpsFrames++;
      if (!fpsT0) fpsT0 = now;
      if (now - fpsT0 >= 500) {
        fps = Math.round(fpsFrames * 1000 / (now - fpsT0));
        fpsFrames = 0;
        fpsT0 = now;
        updateStatus(s.stats);
      }
    }
    requestAnimationFrame(frame);
  }

  /* ---- ?bench: agents at a stable 60 fps, measured in this browser ----
   * Pins the population at N (reseed floor = birth cap = N, so the count holds
   * at N), steps once per frame, warms up, then records frame intervals, step
   * time and draw time; repeats for growing N until the frame rate falls under
   * 30. Results go to an overlay, the console, and window.EVO_BENCH.
   * Keep the tab visible while it runs (hidden tabs throttle rAF).
   * ?bench&threads=0 measures the single-threaded module on an isolated page. */
  var bench = /[?&]bench(=|&|$)/.test(location.search) ? { results: [] } : null;
  var BENCH_NS = [250, 500, 1000, 1500, 2000, 3000, 4000, 6000, 8000, 10000, 12000, 16000];
  var BENCH_WARM = 90, BENCH_MEASURE = 300;
  function benchNext() {
    var b = bench;
    if (b.i >= BENCH_NS.length) { benchDone(); return; }
    b.n = BENCH_NS[b.i];
    initWorld(42, b.n, { minPopulation: b.n, maxPopulation: b.n });
    b.frame = 0; b.prev = 0; b.iv = []; b.step = []; b.draw = []; b.pop = [];
  }
  function quantile(a, q) {
    var s = a.slice().sort(function (x, y) { return x - y; });
    return s[Math.min(s.length - 1, Math.floor(q * s.length))];
  }
  function mean(a) { var t = 0; for (var i = 0; i < a.length; i++) t += a[i]; return t / a.length; }
  function benchFrame(now, stepMs, drawMs, pop) {
    var b = bench;
    if (b.done) return;
    b.frame++;
    if (b.frame > BENCH_WARM) {
      if (b.prev) b.iv.push(now - b.prev);
      b.step.push(stepMs); b.draw.push(drawMs); b.pop.push(pop);
    }
    b.prev = now;
    if (b.iv.length < BENCH_MEASURE) return;
    var r = {
      agents: Math.round(mean(b.pop)),
      fps: +(1000 / mean(b.iv)).toFixed(1),
      p95FrameMs: +quantile(b.iv, 0.95).toFixed(2),
      stepMsMedian: +quantile(b.step, 0.5).toFixed(3),
      drawMsMedian: +quantile(b.draw, 0.5).toFixed(3)
    };
    /* The lightest load (the first N) shows the display's own refresh rate.
     * On a display at or above 60 Hz "stable" means 60 fps; on a slower one
     * (some laptops drop to ~57 Hz on battery or when idle) 60 fps cannot be
     * measured, so it means holding the display rate instead, and the report
     * says which. */
    if (!b.displayFps) b.displayFps = r.fps;
    r.stable60 = b.displayFps >= 58
      ? r.fps >= 58 && r.p95FrameMs <= 20
      : r.fps >= 0.97 * b.displayFps && r.p95FrameMs <= 1.2 * 1000 / b.displayFps;
    b.results.push(r);
    benchShow();
    if (r.fps < 30) { benchDone(); return; }
    b.i++;
    benchNext();
  }
  function benchEnv() {
    return {
      userAgent: navigator.userAgent,
      hardwareConcurrency: navigator.hardwareConcurrency,
      crossOriginIsolated: self.crossOriginIsolated === true,
      module: threaded ? "threaded" : "single-threaded",
      threads: Module._evo_threads ? Module._evo_threads() : 1,
      renderer: glScene ? "webgl2" : "canvas2d",
      canvas: canvas.width + "x" + canvas.height,
      devicePixelRatio: window.devicePixelRatio
    };
  }
  function benchShow() {
    var out = el("benchout");
    if (!out) {
      out = document.createElement("pre");
      out.id = "benchout";
      document.body.appendChild(out);
    }
    var lines = ["evo_boids ?bench   " + JSON.stringify(benchEnv()), "",
                 "agents     fps   p95 frame ms   step ms   draw ms   stable 60"];
    bench.results.forEach(function (r) {
      lines.push(String(r.agents).padStart(6) + String(r.fps).padStart(8) +
                 String(r.p95FrameMs).padStart(15) + String(r.stepMsMedian).padStart(10) +
                 String(r.drawMsMedian).padStart(10) + "   " + (r.stable60 ? "yes" : "no"));
    });
    if (bench.done) {
      var best = bench.results.filter(function (r) { return r.stable60; }).pop();
      var at = bench.displayFps >= 58 ? "a stable 60 fps"
             : "the display's " + bench.displayFps + " Hz (it cannot show 60 fps)";
      lines.push("", "max agents at " + at + ": " + (best ? best.agents : "none measured"));
    } else {
      lines.push("", "measuring " + bench.n + " agents…");
    }
    out.textContent = lines.join("\n");
  }
  function benchDone() {
    bench.done = true;
    window.EVO_BENCH = { env: benchEnv(), displayFps: bench.displayFps, results: bench.results };
    console.log("EVO_BENCH " + JSON.stringify(window.EVO_BENCH));
    benchShow();
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
    if (guardTurbo()) return;
    var str = Module.UTF8ToString(Module._evo_checkpoint());
    var frameNo = Math.round(new Float64Array(heap(), Module._evo_stats(), STATS_LEN)[3]);
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
    warmup = WARMUP_FRAMES;  /* a load is a restart as far as frame timing goes */
    workEma = 0;
    var w = Module._evo_width(), h = Module._evo_height();
    if (w !== WORLD_W || h !== WORLD_H) {
      WORLD_W = w; WORLD_H = h;
      canvas.width = w; canvas.height = h;
    }
    showNote("(checkpoint loaded)");
  }

  /* ---- TURBO (headless worker) mode ----
   * live -> entering -> turbo -> exiting -> live. Handoff both ways is by
   * checkpoint string (bit-exact through the shim), so the trajectory is
   * preserved. While not 'live' the main module is frozen: the rAF loop stops
   * stepping but keeps re-syncing/drawing the same frame. */
  var turboState = "live";
  var turboWorker = null;
  var turboStats = null;

  function guardTurbo() {
    if (turboState === "live") return false;
    showNote("exit turbo first");
    return true;
  }
  function updateLocks() {
    var tb = el("turbobtn");
    tb.textContent = (turboState === "live" || turboState === "entering") ? "Turbo" : "Turbo ✓";
    tb.disabled = turboState === "entering" || turboState === "exiting";
    var locked = turboState !== "live";
    ["restart", "savecp", "loadcp", "pausebtn", "slower", "faster"].forEach(function (id) {
      el(id).classList.toggle("locked", locked);
    });
  }
  function onTurboMessage(e) {
    var msg = e.data || {};
    if (msg.type === "stats") {
      turboStats = msg;
      if (turboState === "entering") { turboState = "turbo"; updateLocks(); }
    } else if (msg.type === "checkpoint") {
      if (turboState !== "exiting") return;
      turboState = "live";
      loadCheckpointText(msg.json); /* reuses the width/height re-read + selection clear */
      setPaused(false);             /* turbo implies running; come back un-paused */
      updateLocks();
      showNote("(turbo: +" + (turboStats ? turboStats.totalSteps : 0) + " steps)");
    } else if (msg.type === "error") {
      showError("turbo: " + (msg.message || "unknown error"));
      turboState = "live"; /* main module still holds its last state; nothing lost */
      updateLocks();
    }
  }
  function enterTurbo() {
    if (turboState !== "live" || !Module) return;
    turboState = "entering";
    turboStats = null;
    selectedId = null; /* ids will be stale by exit */
    updateLocks();
    if (!turboWorker) { /* created once, reused across sessions */
      turboWorker = new Worker(ASSETS.worker);
      turboWorker.onmessage = onTurboMessage;
      turboWorker.onerror = function (ev) {
        showError("turbo worker failed: " + (ev.message || "could not load worker.js"));
        turboState = "live";
        updateLocks();
      };
    }
    turboWorker.postMessage({ cmd: "start",
                              checkpoint: Module.UTF8ToString(Module._evo_checkpoint()),
                              js: moduleUrls.js, wasm: moduleUrls.wasm,
                              threads: Module._evo_threads ? Module._evo_threads() : 1 });
  }
  function exitTurbo() {
    if (turboState !== "turbo") return;
    turboState = "exiting";
    updateLocks();
    turboWorker.postMessage({ cmd: "stop" });
  }
  function drawTurboOverlay() {
    var pw = 560, ph = 170;
    var px = (WORLD_W - pw) / 2, py = (WORLD_H - ph) / 2;
    ctx.fillStyle = "rgba(10,22,30," + (205 / 255).toFixed(4) + ")";
    ctx.fillRect(px, py, pw, ph);
    ctx.strokeStyle = "rgba(90,140,150," + (220 / 255).toFixed(4) + ")";
    ctx.lineWidth = 1;
    ctx.strokeRect(px + 0.5, py + 0.5, pw - 1, ph - 1);
    var cx = px + pw / 2;
    ctx.textAlign = "center";
    ctx.font = "28px monospace";
    ctx.fillStyle = "rgba(220,235,240," + (235 / 255).toFixed(4) + ")";
    ctx.fillText("TURBO — headless", cx, py + 48);
    ctx.font = "16px monospace";
    ctx.fillStyle = "rgba(200,215,220," + (220 / 255).toFixed(4) + ")";
    if (turboStats) {
      ctx.fillText("frame " + turboStats.frame + "   pop " + turboStats.pop +
                   "   food " + turboStats.food + "   pred " + turboStats.pred, cx, py + 92);
      ctx.fillText(turboStats.stepsPerSec + " steps/s   (+" + turboStats.totalSteps +
                   " steps this session)", cx, py + 122);
    } else {
      ctx.fillText("starting…", cx, py + 92);
    }
    if (turboState === "exiting") {
      ctx.font = "13px monospace";
      ctx.fillStyle = "rgba(150,175,180," + (200 / 255).toFixed(4) + ")";
      ctx.fillText("stopping — syncing back to live…", cx, py + 150);
    }
    ctx.textAlign = "left"; /* restore default for the HUD */
  }

  /* ---- input ---- */
  canvas.addEventListener("click", function (e) {
    if (!Module || turboState !== "live") return;
    var rect = canvas.getBoundingClientRect();
    var wx = (e.clientX - rect.left) * (canvas.width / rect.width);
    var wy = (e.clientY - rect.top) * (canvas.height / rect.height);
    var n = Module._evo_creature_count();
    var segs = new Float32Array(heap(), Module._evo_creature_segments(), n * 12);
    var ids = new Float64Array(heap(), Module._evo_creature_ids(), n);
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
    /* sim-mutating shortcuts are locked while turbo owns the trajectory
     * (Escape/drawer above stays live) */
    if (turboState !== "live") {
      if (e.code === "Space" || e.key === "ArrowUp" || e.key === "ArrowDown" ||
          e.key === "+" || e.key === "=" || e.key === "-" ||
          e.key === "r" || e.key === "R") {
        showNote("exit turbo first");
        e.preventDefault();
      }
      return;
    }
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
  ["seed", "population", "toroidal", "bodyModel", "predators", "foodPatches", "currentStrength", "predatorEconomy", "governor"]
    .forEach(function (id) {
      var l = el(id).closest("label");
      if (l) attachTip(l, id);
    });

  function btn(id, fn) {
    el(id).addEventListener("click", function () { fn(); this.blur(); });
  }
  btn("restart", restart);
  btn("pausebtn", function () { if (guardTurbo()) return; setPaused(!paused); });
  btn("slower", function () { if (guardTurbo()) return; setSpeed(simSpeed - 1); });
  btn("faster", function () { if (guardTurbo()) return; setSpeed(simSpeed + 1); });
  btn("turbobtn", function () {
    if (turboState === "live") enterTurbo();
    else if (turboState === "turbo") exitTurbo();
  });
  btn("advreset", resetAdvanced);
  btn("savecp", saveCheckpoint);
  btn("loadcp", function () { if (guardTurbo()) return; el("cpfile").click(); });
  el("cpfile").addEventListener("change", function () {
    var file = this.files && this.files[0];
    this.value = "";
    if (!file || !Module || guardTurbo()) return;
    file.text().then(function (text) {
      loadCheckpointText(text);
      /* the file's own cap becomes the user cap; the governor starts fresh */
      var cap = 0;
      try { cap = (JSON.parse(text).config || {}).maxPopulation || 0; } catch (e) { /* load reported it */ }
      userCap = cap;
      autoCap = 0;
      el("adv_maxPopulation").value = String(cap);
      resetGovernor();
    }, function (err) { showError("could not read file: " + err); });
  });
  el("governor").addEventListener("change", function () {
    governorOn = this.checked;
    if (!governorOn && autoCap) { autoCap = 0; applyCap(); }
    govNote = "";
    this.blur();
  });

  buildAdvancedUI();
  setSpeed(simSpeed);

  /* ---- boot ---- */
  if (!window.EvoLoader || !ASSETS) {
    showError("loader.js / EVO_ASSETS missing from the page");
    return;
  }
  EvoLoader.load().then(function (r) {
    Module = r.Module;
    threaded = r.threaded;
    threadWhy = r.why;
    moduleUrls = { js: r.js, wasm: r.wasm };
    if (Module._evo_stats_len) STATS_LEN = Module._evo_stats_len();
    /* Thread count: ?nthreads=N, else every pre-spawned worker plus this
     * thread (EvoLoader.poolThreads() — the -sPTHREAD_POOL_SIZE formula). */
    var nt = parseInt(new URLSearchParams(location.search).get("nthreads"), 10);
    if (threaded) Module._evo_set_threads(nt > 0 ? Math.min(nt, EvoLoader.poolThreads()) : EvoLoader.poolThreads());
    /* The single-threaded module starts with fewer creatures (see the
     * benchmark in README.md); either default can be changed in the drawer. */
    el("population").value = String(threaded ? DEFAULT_POP_THREADED : DEFAULT_POP_SINGLE);
    el("turbobtn").title = threaded
      ? "run headless in a worker at full speed — no rendering, keeps running in background tabs; the hand-off is exact, but a threaded run is not reproducible"
      : "run headless in a worker at full speed — no rendering, keeps running in background tabs; toggling is bit-exact";
    el("modeline").textContent = (threaded
      ? "module: threaded (" + Module._evo_threads() + " threads) — fast, not reproducible from the seed"
      : "module: single-threaded — reproducible from the seed (" + threadWhy + ")") +
      "; drawing: " + (glScene ? "WebGL2" : "canvas 2D");
    if (bench) {
      bench.i = 0;
      benchNext();
      clearError();
    } else if (restart()) {
      clearError();
    }
    requestAnimationFrame(frame);
  }).catch(function (err) {
    showError("failed to load wasm module: " + err);
  });
})();
