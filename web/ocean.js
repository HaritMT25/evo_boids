"use strict";
/* evo_boids 3-D Ocean: the page controller (web/ocean.html).
 *
 * Runs the Ocean world (web/evo_ocean.{js,wasm}, factory createEvoOcean,
 * single-threaded; the C API is wasm/ocean_wasm.cpp's header) and draws it in
 * the deep tank (web/ocean_scene.js). Around it, the Classic page's controls
 * (web/renderer.js), so the two pages are one product:
 *   - the top bar (live readout -> the stat panel with the depth histogram;
 *     📊 dashboards, web/ocean_dash.js; the settings) and the toasts
 *   - the dock: pause, speed (steps per frame), Turbo, 🎲 Randomize, the
 *     flashlight, fit the view
 *   - the settings sheet, built from web/ocean_config.js: every key the
 *     Ocean takes, "live" ones (oc_set_param) applying at once, the rest on
 *     Restart ("Restart to apply"); settings that do nothing with the others as
 *     they are are dimmed with the reason (NEEDS); the Run tab: Randomize, New
 *     world / Replay, playback and view switches, save / load a setup
 *   - the fish card (click or tap a fish): oc_inspect's numbers and its live
 *     brain (oc_set_capture keeps its senses while a fish is selected)
 *   - Turbo: web/ocean_worker.js steps a copy of the module's memory at full
 *     speed; the copy comes back on Stop, so the run carries on exactly
 *     (web/ocean_handoff.js: the hand-off, shared with the worker)
 *   - the tour and the first-visit Turbo card (web/guide.js, steps below)
 * The flashlight (scene.setFlashlight) is drawing only: no engine call.
 *
 * Every pointer the module returns is valid until the next oc_sync / oc_init
 * (oc_inspect: until the next step or sync), and memory can grow — in any
 * export that allocates (oc_summary, oc_families, oc_inspect, oc_wiring, the
 * name lists, oc_sync itself) — so typed arrays are cut from the heap after
 * the call that fills them, and a frame's snapshot is cut again (fresh())
 * after anything that may have grown it.
 *
 * URL: ?seed=42 &pop=300 &depth=240 &w=1920 &h=1080 (1080 x 1920 on a
 *      portrait screen) &cfg={"key":value,...} (SimConfig + OceanConfig keys,
 *      as oc_init takes them; the settings show them) &speed=1 (steps per
 *      frame) &e1=1 (the E1 physics preset, oc_e1_preset, under &cfg)
 *      &colour=family|brain|appearance &guide=0|1 &intro=1 (web/guide.js)
 *      ?mock=1 loads ocean_mock.js from beside the page instead of the engine
 *      (a JS stand-in for page development, not part of web/).
 *      The link's setup goes to oc_init as it is; one it refuses stops the
 *      page with the engine's reason (and a link without it).
 *      A Restart writes seed / pop / depth / w / h / cfg back, so a reload, or
 *      the link on another screen, is the same world.
 *      A query with only Classic's keys (?view, ?threads, ...) goes to Classic
 *      before this runs: the site's root was Classic (ocean.html's head).
 * Keys: space pause, + / - (or arrows) steps per frame, F follow (the selected
 *       fish, else a random one; again: another), Esc close / stop following
 *       / let go, R a new world (new seed), shift+R replay this one, C fit the
 *       view, L the flashlight, . one step while paused, ? the keys legend.
 *       The camera, held (scene.setMove; by key position, never while a field
 *       has the focus): W / S forward / back along the view, A / D left /
 *       right, E / Q or PageUp / PageDown up / down, Shift faster. Following a
 *       fish they stay in the follow: W / S closer / farther, A / D circle it,
 *       E / Q raise / lower the camera around it.
 *
 * window.EvoOcean: a small handle for the console and the tests. */
(function () {
  /* the page is on its way to Classic (an old Classic link: ocean.html's head) */
  if (window.EVO_REDIRECT) return;
  var A = window.EVO_ASSETS, CFG = window.EvoOceanConfig;
  var P = new URLSearchParams(location.search);
  var MOCK = P.get("mock") === "1";
  var E1 = P.get("e1") === "1";
  var DT = 0.016;                      /* oc_step's fixed step */
  var SPEEDS = [1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64];
  var STEP_BUDGET_MS = 11;             /* of a 16.7 ms frame: what stepping may take (Hold 60 fps) */
  var STATS = { pop: 0, frame: 1, births: 2, deaths: 3, reseeds: 4, depthMean: 5, depthSd: 6, hist0: 7 };
  var REQUIRED = ["oc_init", "oc_reset", "oc_step", "oc_sync", "oc_creature_count", "oc_creature_segments",
                  "oc_creature_depth", "oc_body_params", "oc_creature_colors", "oc_creature_ids",
                  "oc_food_count", "oc_food_xy", "oc_food_depth", "oc_predator_count", "oc_predator_data",
                  "oc_predator_depth", "oc_stats", "oc_stats_len", "oc_width", "oc_height", "oc_depth_max"];
  var KEYS = CFG.KEYS, LIVE = CFG.LIVE, LABELS = CFG.LABELS;
  var ARGS = { seed: 1, population: 1, depth: 1 };   /* oc_init's own arguments, not cfg keys */
  var ACTION_LABELS = ["steer x", "steer y", "swim forward", "wander", "speed", "signal",
                       "oscillator period", "respond", "share food", "reproduce", "fight", "ignore", "steer z (depth)"];
  var ACTION_SHORT = ["steer x", "steer y", "forward", "wander", "speed", "signal",
                      "osc", "respond", "share", "breed", "fight", "ignore", "steer z"];

  /* the Classic tank's body-size curve (renderer.js bodyScale): the two must match */
  function bodyScale(b) { return b > 0 ? 1 + 0.4 * Math.tanh(5 * (b - 1)) : 1; }
  function el(id) { return document.getElementById(id); }
  var canvas = el("tank");
  var compactMQ = window.matchMedia("(max-width: 700px), (max-height: 560px), (pointer: coarse)");
  var coarseMQ = window.matchMedia("(pointer: coarse)");
  var wideMQ = window.matchMedia("(min-width: 960px)");
  var reducedMQ = window.matchMedia("(prefers-reduced-motion: reduce)");
  var lastPointer = "mouse";
  ["pointerover", "pointerdown"].forEach(function (type) {
    window.addEventListener(type, function (e) { lastPointer = e.pointerType || "mouse"; }, true);
  });
  function store(k, v) { try { if (v === undefined) return localStorage.getItem(k); localStorage.setItem(k, v); } catch (e) { /* private mode */ } return null; }

  /* ---------------- messages: the settings footer line, plus a toast ---------------- */
  var errEl = el("err"), toastsEl = el("toasts");
  function toast(msg, kind) {
    var t = document.createElement("div");
    t.className = "toast" + (kind ? " " + kind : "");
    t.textContent = msg;
    toastsEl.appendChild(t);
    while (toastsEl.children.length > 3) toastsEl.removeChild(toastsEl.firstChild);
    setTimeout(function () {
      t.classList.add("out");
      setTimeout(function () { if (t.parentNode) t.parentNode.removeChild(t); }, 220);
    }, kind === "error" ? 6000 : 3200);
  }
  function showError(msg) { errEl.textContent = msg; errEl.className = "error"; toast(msg, "error"); console.warn("ocean: " + msg); }
  function showNote(msg) { errEl.textContent = msg; errEl.className = ""; toast(msg); }
  function clearError() { errEl.textContent = ""; errEl.className = ""; }
  function splashSay(msg, isError) {
    var s = el("splashstate");
    s.textContent = msg;
    s.className = isError ? "error" : "";
    if (isError) el("splash").classList.add("failed");
  }

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
  var moduleUrls = null;   /* {js, wasm}: the Turbo worker loads the same module */
  function loadModule() {
    if (!MOCK) {
      var simd = false;
      try { simd = typeof WebAssembly === "object" && WebAssembly.validate(SIMD_PROBE); } catch (e) { /* none */ }
      if (!simd) return Promise.reject(new Error("this browser has no WebAssembly SIMD, which the simulation needs (Chrome 91+, Firefox 89+ or Safari 16.4+)"));
    }
    var js = abs(MOCK ? A.mock : A.ocean), wasm = abs(A.oceanWasm);
    moduleUrls = { js: js, wasm: wasm, handoff: abs(A.handoff || "ocean_handoff.js") };
    return loadScript(js).then(function () {
      if (typeof createEvoOcean !== "function") throw new Error(js + " loaded but defines no createEvoOcean()");
      return createEvoOcean({ locateFile: function (path) { return path.slice(-5) === ".wasm" ? wasm : path; } });
    });
  }

  var M = null, HO = window.EvoOceanHandoff;
  /* The memory as it is now, strings in and out: web/ocean_handoff.js's, which
   * never read the module's own cached views (a Turbo hand-off grows the
   * memory from outside, and those stay behind until the module grows it
   * itself). */
  function heap() { return HO.buffer(M); }
  function has(fn) { return !!M && typeof M["_" + fn] === "function"; }
  function withCString(str, fn) { return HO.withCString(M, str, fn); }
  function cString(ptr) { return HO.cString(M, ptr); }
  function lastError() { return M._oc_last_error ? cString(M._oc_last_error()) : "(no oc_last_error)"; }
  /* Take a whole memory image of this same module (the Turbo hand-off): grown
   * to its size by exactly the pages missing, copied in, the rest zeroed
   * (web/ocean_handoff.js take(); web/ocean_worker.js says why this carries
   * the run on exactly). Throws with this page's memory not overwritten. */
  function takeMemory(src) { HO.take(M, src, "the page's"); }

  var W = 0, H = 0, D = 0, statsLen = 0, HAS_VZ = false, HAS_EYE = false, HAS_APP = false;
  var LIGHT = { on: false, L0: 1, k: 0 };
  function readLight() {
    var on = has("oc_light_enabled") && !!M._oc_light_enabled();
    LIGHT = { on: on, L0: on ? M._oc_light_l0() : 1, k: on ? M._oc_light_kwater() : 0 };
    if (scene) scene.setLight(LIGHT);
  }
  function readWorld() {
    W = M._oc_width(); H = M._oc_height(); D = M._oc_depth_max();
    statsLen = M._oc_stats_len();
    readLight();
  }
  function e1Preset() {
    if (!has("oc_e1_preset")) return null;
    try { return JSON.parse(cString(M._oc_e1_preset())); } catch (e) { return null; }
  }

  /* ---------------- the world's settings ---------------- */
  var runCfg = {};           /* the running world's cfg (keys that differ from the default) */
  var initArgs = null;       /* the last successful oc_init's {seed, population, depth, cfg} (Replay) */
  var extraCfg = {};         /* cfg keys from ?cfg= or a setup file that the sheet has no field for */
  function cfgVal(key) { return Object.prototype.hasOwnProperty.call(runCfg, key) ? runCfg[key] : KEYS[key][0]; }
  /* World size for a (re)start: the native 1920 x 1080, or the same area on
   * its side on a portrait screen (the same food density and crowding, and a
   * tank that fills a phone), unless it is fixed: by ?w= / ?h=, or by a
   * loaded setup's size. A Restart writes the size into the address, so a
   * shared link is the same world on a screen the other way up. */
  var URL_W = parseFloat(P.get("w")), URL_H = parseFloat(P.get("h"));
  function side(v) { return Math.round(Math.max(100, Math.min(20000, v))); }
  var fixedW = isFinite(URL_W) ? side(URL_W) : null, fixedH = isFinite(URL_H) ? side(URL_H) : null;
  function isPortrait() { return innerHeight > innerWidth * 1.15; }
  function screenWorld() { return isPortrait() ? [1080, 1920] : [1920, 1080]; }
  function fittedWorld() {
    var f = screenWorld();
    return [fixedW !== null ? fixedW : f[0], fixedH !== null ? fixedH : f[1]];
  }
  /* "Fit the world to this screen": offered when the world has the other
   * orientation's size (the phone turned, or a link from a screen the other
   * way up); a size of its own (?w=300&h=300) stays as it is */
  function updateFitButton() {
    var f = screenWorld();
    el("fitbtn").hidden = !M || !(W === f[1] && H === f[0]);
  }
  function fitToScreen() {
    if (!M || guardTurbo()) return;
    var w0 = fixedW, h0 = fixedH;
    fixedW = fixedH = null;
    if (!restart()) { fixedW = w0; fixedH = h0; }
  }
  var worldReady = false;
  function initWorld(seed, population, depth, cfg, rawCfg) {
    var size = fittedWorld();
    var json = rawCfg !== undefined ? rawCfg : (Object.keys(cfg).length ? JSON.stringify(cfg) : "");
    var ok = withCString(json, function (ptr) { return M._oc_init(seed >>> 0, size[0], size[1], depth, population, ptr); });
    if (!ok) return false;
    initArgs = { seed: seed >>> 0, population: population, depth: depth, cfg: cfg };
    runCfg = Object.assign({}, cfg);
    clearFieldNotes();
    afterNewWorld();
    return true;
  }
  /* A setup from outside the sheet (the link's, a setup file's), started as
   * it is: oc_init sees its values themselves, so a refusal says why, and
   * only a setup it accepts goes into the sheet, whose fields would round or
   * drop what they can't hold (0.5 for a count, 2 for an on / off key or a
   * choice). On a refusal nothing changes: the running world, the sheet.
   * raw: the cfg as text instead (a ?cfg= that is not JSON). Returns "", or
   * why not. A switch must be true or false: the engine reads Classic's
   * switches (toroidal, bodyModel, ...) only as JSON booleans and passes over
   * anything else, so {"toroidal":1} would run without it under a sheet
   * showing it on. */
  function importWorld(seed, population, depth, cfg, raw) {
    var clean = {}, bad = [];
    Object.keys(cfg || {}).forEach(function (k) {
      if (ARGS[k]) return;
      clean[k] = cfg[k];
      if (KEYS[k] && typeof KEYS[k][0] === "boolean" && typeof cfg[k] !== "boolean")
        bad.push(k + " must be true or false (got " + JSON.stringify(cfg[k]) + ")");
    });
    if (bad.length) return bad.join("; ");
    if (!initWorld(seed, population, depth, clean, raw)) return lastError();
    resetForm();
    setVal("seed", seed >>> 0); setVal("population", population); setVal("depth", depth);
    cfgToForm(clean);
    updateNeeds(); updateTabDots(); setPending(false);
    var odd = Object.keys(clean).filter(function (k) {
      if (!KEYS[k]) return false;
      var a = getVal(k), b = clean[k], bool = typeof KEYS[k][0] === "boolean" || (KEYS[k][1] || {}).bit;
      return bool ? !!a !== !!b : a !== b;
    });
    if (odd.length) console.warn("ocean: the sheet shows " + odd.join(", ") + " differently from the setup it runs");
    return "";
  }
  /* a new world (oc_init or oc_reset), or one handed back by Turbo */
  function afterNewWorld(fromTurbo) {
    var w0 = W, h0 = H, d0 = D;
    lastS = null;   /* its views are the old world's: the next frame cuts new ones */
    readWorld();
    if (scene && (W !== w0 || H !== h0 || D !== d0)) { scene.setWorld(W, H, D); labelHist(); }
    applyDisplaySwitches();
    estPrev.n = 0;
    stepMs = 0;
    if (!fromTurbo) {
      selectedId = null; wantFollow = false;
      if (scene && scene.following()) scene.setFollow(false);
      if (window.EvoOceanDash) EvoOceanDash.reset();
      sampleBucket = -1;
      rates.reset();
    }
    dirtyHud = true;
    updateFitButton();
  }
  /* the shim's display switches: the colour mode is the shim's (memory), the
   * capture flag the world's, so both are set again after every new world and
   * every Turbo hand-off */
  function applyDisplaySwitches() {
    if (has("oc_set_colour_mode")) M._oc_set_colour_mode(colourBrain ? 0 : 1);
    if (scene && scene.setAppearance) scene.setAppearance(colourMode === "appearance");
    if (has("oc_set_capture")) M._oc_set_capture(selectedId !== null ? 1 : 0);
  }

  /* ---- the sheet (built from EvoOceanConfig.TABS) ---- */
  function decimals(x) { var s = String(x); var i = s.indexOf("."); return i < 0 ? (s.indexOf("e-") > 0 ? +s.split("e-")[1] : 0) : s.length - i - 1; }
  function makeStepper(input, key, min, max) {
    var wrap = document.createElement("span");
    wrap.className = "stepper";
    function button(sign, icon, label) {
      var b = document.createElement("button");
      b.type = "button"; b.tabIndex = -1;
      b.setAttribute("aria-label", label);
      b.innerHTML = "<svg class='i'><use href='#i-" + icon + "'/></svg>";
      b.addEventListener("click", function () {
        var st = CFG.BTN_STEP[key] || 1, v = parseFloat(input.value);
        if (!Number.isFinite(v)) v = 0;
        v = +(v + sign * st).toFixed(Math.max(decimals(st), 0) + 2);
        v = +v.toFixed(Math.max(decimals(st), decimals(v) > 6 ? 6 : decimals(v)));
        if (min !== undefined && v < min) v = min;
        if (max !== undefined && v > max) v = max;
        input.value = String(v);
        input.dispatchEvent(new Event("input", { bubbles: true }));
        input.dispatchEvent(new Event("change", { bubbles: true }));
      });
      return b;
    }
    wrap.appendChild(button(-1, "minus", "less"));
    wrap.appendChild(input);
    wrap.appendChild(button(1, "plus", "more"));
    return wrap;
  }
  function makeField(key) {
    var def = KEYS[key][0], opt = KEYS[key][1] || {}, id = "o_" + key;
    var row = document.createElement("div");
    row.className = "field";
    row.id = "f_" + key;
    var left = document.createElement("span");
    var name = document.createElement("label");
    name.className = "name";
    name.htmlFor = id;
    var lt = document.createElement("span");
    lt.className = "lt";
    lt.textContent = LABELS[key] || key;
    name.appendChild(lt);
    if (LIVE[key]) {
      var tag = document.createElement("span");
      tag.className = "livetag";
      tag.textContent = "live";
      tag.title = "applies to the running world at once";
      name.appendChild(tag);
    }
    var keyEl = document.createElement("span");
    keyEl.className = "key";
    keyEl.textContent = key + (CFG.HINTS[key] ? " · " + CFG.HINTS[key] : "");
    left.appendChild(name);
    left.appendChild(keyEl);
    var ctl = document.createElement("span");
    ctl.className = "ctl";
    var input;
    if (typeof def === "boolean" || opt.bit) {
      input = document.createElement("input");
      input.type = "checkbox";
      input.className = "switch";
      input.setAttribute("role", "switch");
      input.checked = !!def;
      ctl.appendChild(input);
    } else if (opt.options) {
      input = document.createElement("select");
      opt.options.forEach(function (label, i) {
        var o = document.createElement("option");
        o.value = String(i); o.textContent = label;
        input.appendChild(o);
      });
      input.value = String(def);
      ctl.appendChild(input);
    } else {
      input = document.createElement("input");
      input.type = "number";
      input.inputMode = "decimal";
      input.step = opt.step !== undefined ? String(opt.step) : "any";
      if (opt.min !== undefined) input.min = String(opt.min);
      if (opt.max !== undefined) input.max = String(opt.max);
      input.value = String(def);
      ctl.appendChild(makeStepper(input, key, opt.min, opt.max));
    }
    input.id = id;
    if (LIVE[key]) input.addEventListener("change", function () { applyLive(key); });
    row.appendChild(left);
    row.appendChild(ctl);
    attachTip(row, key);
    return row;
  }
  function getVal(key) {
    var def = KEYS[key][0], opt = KEYS[key][1] || {}, input = el("o_" + key);
    if (typeof def === "boolean") return input.checked;
    if (opt.bit) return input.checked ? 1 : 0;
    var v = opt.options ? parseInt(input.value, 10) : parseFloat(input.value);
    if (!Number.isFinite(v)) return def;
    if (opt.int) v = Math.round(v);
    return v;
  }
  function setVal(key, v) {
    var def = KEYS[key][0], opt = KEYS[key][1] || {}, input = el("o_" + key);
    if (!input) return;
    if (typeof def === "boolean" || opt.bit) input.checked = v === true || v === 1;
    else input.value = String(v);
  }
  function resetForm() { Object.keys(KEYS).forEach(function (k) { setVal(k, KEYS[k][0]); }); extraCfg = {}; clearFieldNotes(); }
  /* a flat cfg object into the sheet: known keys into their fields, the rest kept aside */
  function cfgToForm(cfg) {
    Object.keys(cfg || {}).forEach(function (k) {
      if (KEYS[k] && !ARGS[k]) setVal(k, cfg[k]);
      else if (!ARGS[k]) extraCfg[k] = cfg[k];
    });
  }
  function readForm() {
    var cfg = {};
    Object.keys(KEYS).forEach(function (k) {
      if (ARGS[k]) return;
      var v = getVal(k), def = KEYS[k][0];
      if (typeof def === "boolean" ? v !== def : v !== def) cfg[k] = v;
    });
    Object.keys(extraCfg).forEach(function (k) { if (!(k in cfg)) cfg[k] = extraCfg[k]; });
    return { seed: getVal("seed") >>> 0, population: getVal("population"), depth: getVal("depth"), cfg: cfg };
  }
  /* ---- live settings ----
   * A live field shows what the running world uses, or says why not:
   *   - a value below 0: oc_set_param changes a running world only to 0 or
   *     more; where a new world takes it (CFG.NEG_ON_RESTART: kBuoy, the
   *     food rates, energies, ages) it waits for the Restart, marked so; for
   *     the rest it is outside the setting's range and the field goes back
   *   - while Turbo is starting or stopping (neither side owns the world for
   *     that moment) it is held, marked so, and applied as soon as one does
   *   - one the engine refuses: the field goes back to the running value,
   *     with the engine's reason. */
  var heldLive = {};    /* key -> value: waiting for Turbo's hand-off */
  var inflight = [];    /* [key, value] sent to the Turbo worker, not answered yet */
  function numOf(key, v) { return typeof v === "boolean" ? (v ? 1 : 0) : v; }
  function valOf(key, num) { return typeof KEYS[key][0] === "boolean" ? num === 1 : num; }
  function shown(v) { return typeof v === "boolean" ? (v ? "on" : "off") : String(v); }
  /* the note under a field (why its value is not the running world's yet) */
  function fieldNote(key, text) {
    var row = el("f_" + key);
    if (!row) return;
    var n = row.querySelector(".pendnote");
    if (!n && !text) return;
    if (!n) { n = document.createElement("span"); n.className = "pendnote"; row.appendChild(n); }
    n.textContent = text;
  }
  function clearFieldNotes() {
    var notes = document.querySelectorAll(".field .pendnote");
    for (var i = 0; i < notes.length; i++) notes[i].textContent = "";
  }
  /* push one live knob into the running world (or the Turbo worker's) */
  function applyLive(key) {
    if (!M) return;
    var num = numOf(key, getVal(key));
    if (num < 0) {
      if (CFG.NEG_ON_RESTART[key]) {
        fieldNote(key, "Applies on Restart: a running world takes only 0 or more.");
        setPending(true);
        showNote((LABELS[key] || key) + " " + num + " applies on Restart (a running world takes only 0 or more).");
      } else refuse(key, "it must be 0 or more");
      return;
    }
    if (turboState === "entering" || turboState === "exiting") {
      heldLive[key] = num;
      fieldNote(key, turboState === "entering" ? "Applies as soon as Turbo has started." : "Applies as soon as Turbo hands the world back.");
      return;
    }
    fieldNote(key, "");
    sendLive(key, num);
  }
  function sendLive(key, num, quiet) {
    if (turboState === "turbo") {   /* Turbo owns the world: the worker applies it */
      inflight.push([key, num]);
      turboWorker.postMessage({ cmd: "set", key: key, value: num });
      return;
    }
    if (!has("oc_set_param")) return;
    var ok = withCString(key, function (p) { return M._oc_set_param(p, num); });
    if (ok) liveApplied(key, valOf(key, num), quiet);
    else refuse(key, lastError());
  }
  function liveApplied(key, v, quiet) {
    if (v === KEYS[key][0]) delete runCfg[key]; else runCfg[key] = v;
    if (key === "lightEnabled" || key === "L0" || key === "kWater") readLight();
    if (!quiet) showNote((LABELS[key] || key) + " is now " + shown(v) + " (live)");
  }
  /* a refused live value: the field goes back to what the world runs with
   * (keep: a newer edit of it is on its way, so the field stays) */
  function refuse(key, why, keep) {
    if (!keep) {
      setVal(key, cfgVal(key));
      fieldNote(key, "");
      updateNeeds(); updateTabDots();
    }
    showError((LABELS[key] || key) + ": " + why + (keep ? "" : " (back to " + shown(cfgVal(key)) + ")"));
  }
  /* the held edits, now that one side owns the world */
  function flushHeld(quiet) {
    var held = heldLive;
    heldLive = {};
    Object.keys(held).forEach(function (k) { fieldNote(k, ""); sendLive(k, held[k], quiet); });
  }

  var tabsEl = el("tabs"), panelsEl = el("panels"), currentTab = "run";
  function buildSettings() {
    CFG.TABS.forEach(function (tab) {
      var b = document.createElement("button");
      b.type = "button";
      b.id = "tab-" + tab.id;
      b.setAttribute("role", "tab");
      b.setAttribute("aria-controls", "panel-" + tab.id);
      b.textContent = tab.title;
      b.addEventListener("click", function () { setSheetTab(tab.id); });
      tabsEl.appendChild(b);
      if (!tab.groups) return;
      var panel = document.createElement("section");
      panel.className = "panel";
      panel.id = "panel-" + tab.id;
      panel.dataset.tab = tab.id;
      panel.setAttribute("role", "tabpanel");
      if (tab.id === "ocean") panel.appendChild(presetBox());
      tab.groups.forEach(function (g) {
        var h = document.createElement("h3");
        h.textContent = g[0];
        panel.appendChild(h);
        g[1].forEach(function (k) { panel.appendChild(makeField(k)); });
      });
      panelsEl.appendChild(panel);
    });
    el("panel-run").setAttribute("role", "tabpanel");
    attachTip(el("f_governor"), "governor");
    attachTip(el("f_colourBrain"), "colourBrain");
    attachTip(el("f_followLight"), "followLight");
    var saved = store("evo_boids.ocean.tab");
    setSheetTab(saved && el("panel-" + saved) ? saved : "run");
  }
  /* the Water (E1) tab's "Tuned E1 preset": its keys into the sheet */
  function presetBox() {
    var box = document.createElement("div");
    box.className = "preset";
    box.innerHTML = "<button type='button' class='btn' id='e1btn'><svg class='i'><use href='#i-restart'/></svg>Use the tuned E1 preset</button>" +
      "<p>The E1 physics as tuned: surface waves, a gentle deep current with food drifting in it, light that fades with depth, and depth costs scaled to each fish's metabolism. Puts its settings in the tabs; Restart applies them.</p>";
    box.querySelector("#e1btn").addEventListener("click", function () {
      var p = e1Preset();
      if (!p) { showError("this module has no E1 preset (oc_e1_preset)"); return; }
      cfgToForm(p);
      updateNeeds(); updateTabDots(); setPending(true);
      showNote("The E1 preset is in the settings. Restart to apply.");
      this.blur();
    });
    return box;
  }
  function setSheetTab(id) {
    currentTab = id;
    CFG.TABS.forEach(function (tab) {
      var on = tab.id === id;
      el("tab-" + tab.id).setAttribute("aria-selected", on ? "true" : "false");
      el("tab-" + tab.id).tabIndex = on ? 0 : -1;
      el("panel-" + tab.id).hidden = !on;
    });
    panelsEl.scrollTop = 0;
    hideTip();
    store("evo_boids.ocean.tab", id);
  }
  tabsEl.addEventListener("keydown", function (e) {
    if (e.key !== "ArrowRight" && e.key !== "ArrowLeft") return;
    var ids = CFG.TABS.map(function (t) { return t.id; });
    var i = (ids.indexOf(currentTab) + (e.key === "ArrowRight" ? 1 : -1) + ids.length) % ids.length;
    setSheetTab(ids[i]);
    el("tab-" + ids[i]).focus();
    e.preventDefault();
  });
  function updateTabDots() {
    CFG.TABS.forEach(function (tab) {
      if (!tab.groups) return;
      var changed = tab.groups.some(function (g) {
        return g[1].some(function (k) { return k !== "seed" && k !== "population" && getVal(k) !== KEYS[k][0]; });
      });
      var b = el("tab-" + tab.id), dot = b.querySelector(".dot");
      if (changed && !dot) { dot = document.createElement("span"); dot.className = "dot"; dot.title = "changed from the default"; b.appendChild(dot); }
      else if (!changed && dot) b.removeChild(dot);
    });
  }
  /* Settings that do nothing with the others as they are: dimmed, with the
   * reason under their name. Each rule: [keys, () => "" (fine) or the reason]. */
  function hasCurrent() { return getVal("currentStrength") > 0; }
  function hasPredators() { return getVal("predatorCount") > 0; }
  var NEEDS = [
    [["foodPatchRadius", "foodDepthMode"], function () { return getVal("foodPatches") > 0 ? "" : "needs food patches above 0"; }],
    [["foodDepthSpread"], function () { return getVal("foodPatches") > 0 && getVal("foodDepthMode") === 1 ? "" : "needs food patches, each at its own depth"; }],
    [["foodDrift", "currentMode", "currentDrift", "deepCurrent"], function () { return hasCurrent() ? "" : "needs a current (strength above 0)"; }],
    [["currentEddies", "currentCore"], function () { return hasCurrent() && getVal("currentMode") === 0 ? "" : "needs a current in the eddies shape"; }],
    [["predatorEconomy", "predatorSpeed", "predatorPerception", "predatorCaptureRadius", "predatorCooldown"],
      function () { return hasPredators() ? "" : "needs predators above 0"; }],
    [["predatorMeal", "predatorMetabolism", "predatorReproEnergy", "predatorMaxAge", "predatorMinPopulation",
      "predatorMaxPopulation", "predatorInheritance"],
      function () { return hasPredators() && getVal("predatorEconomy") ? "" : "needs predators, with their economy on"; }],
    [["predatorTraitMutation"], function () {
      return hasPredators() && getVal("predatorEconomy") && getVal("predatorInheritance") ? "" : "needs hunters inherit traits on"; }],
    [["maxTurnRate"], function () { return getVal("triangularMotion") ? "" : "needs fast-swims-straight on"; }],
    [["steerFrame"], function () { return getVal("triangularMotion") ? "no effect with fast-swims-straight on: it turns by steer x already" : ""; }],
    [["reproSoftWidth", "reproGracePeriod"], function () { return getVal("reproMaxDistance") >= 0 ? "" : "needs a breeding distance limit of 0 or more"; }],
    [["bodyMutation", "sizeFight"], function () { return getVal("bodyModel") ? "" : "needs evolving bodies on (without them every fish is size 1)"; }],
    [["sizeDrift"], function () {
      if (!getVal("bodyModel")) return "needs evolving bodies on (without them every fish is size 1)";
      return hasCurrent() || getVal("waveSpeed") > 0 ? "" : "needs a current or waves to drift in";
    }],
    [["kMouth"], function () { return getVal("mouthGate") ? "" : "needs the mouth gate on"; }],
    [["waveBand", "waveGravity"], function () { return getVal("waveSpeed") > 0 ? "" : "needs waves (strength above 0)"; }],
    [["L0", "kWater"], function () { return getVal("lightEnabled") ? "" : "needs the light field on"; }],
    [["depthCostScaled"], function () { return getVal("cHold") > 0 || getVal("cPress") > 0 ? "" : "needs a depth cost above 0"; }],
    [["eye", "eyeOnly"], function () { return HAS_EYE ? "" : "needs the eye build of the module; this page runs the plain Ocean build"; }],
    [["eyeCode"], function () {
      if (!HAS_EYE) return "needs the eye build of the module; this page runs the plain Ocean build";
      return getVal("eye") ? "" : "needs the compound eye on";
    }]
  ];
  function updateNeeds() {
    NEEDS.forEach(function (rule) {
      var why = rule[1]();
      rule[0].forEach(function (k) {
        var row = el("f_" + k);
        if (!row) return;
        var hint = row.querySelector(".needs");
        if (!hint) { hint = document.createElement("span"); hint.className = "needs"; row.appendChild(hint); }
        row.classList.toggle("inert", !!why);
        hint.textContent = why;
      });
    });
  }
  function setPending(on) {
    var b = el("restart");
    b.classList.toggle("pending", on);
    b.querySelector("span").textContent = on ? "Restart to apply" : "Restart";
  }
  function onEdit(e) {
    var t = e.target;
    updateNeeds();
    updateTabDots();
    if (!t || !t.id || t.id.indexOf("o_") !== 0 || LIVE[t.id.slice(2)]) return;   /* live ones apply by themselves */
    setPending(true);
  }

  /* ---- restart, new world, replay, randomize, setups ---- */
  function guardTurbo() {
    if (turboState === "live") return false;
    showNote("Stop Turbo first.");
    return true;
  }
  function restart() {
    if (!M || guardTurbo()) return false;
    clearError();
    var f = readForm();
    if (!initWorld(f.seed, f.population, f.depth, f.cfg)) {
      showError("Couldn't start that world: " + lastError());
      return false;
    }
    setPending(false);
    el("randnote").textContent = "";
    writeUrl();
    return true;
  }
  function newWorld() {
    if (!M || guardTurbo()) return;
    setVal("seed", (Math.random() * 0x100000000) >>> 0);
    if (restart()) toast("A new world, seed " + getVal("seed") + ".");
  }
  /* this world again from its start: oc_reset (the last init's seed and
   * config; live changes since are dropped), and the sheet back to them */
  function replay() {
    if (!M || guardTurbo() || !initArgs) return;
    if (!M._oc_reset()) { showError("oc_reset failed: " + lastError()); return; }
    resetForm();
    setVal("seed", initArgs.seed); setVal("population", initArgs.population); setVal("depth", initArgs.depth);
    cfgToForm(initArgs.cfg);
    runCfg = Object.assign({}, initArgs.cfg);
    updateNeeds(); updateTabDots(); setPending(false);
    afterNewWorld();
    toast("Replaying seed " + initArgs.seed + " from its start.");
  }
  function randomize() {
    if (!M || guardTurbo()) return;
    var pick = CFG.draw(Math.random, KEYS.population[0], e1Preset(), fittedWorld());   /* the size it will start at */
    resetForm();
    Object.keys(pick.set).forEach(function (k) { setVal(k, pick.set[k]); });
    setVal("population", pick.population);
    setVal("seed", (Math.random() * 0x100000000) >>> 0);
    updateNeeds(); updateTabDots();
    if (restart()) {
      var what = pick.population + " fish" + (pick.labels.length ? " · " + pick.labels.join(" · ") : " · the default world");
      el("randnote").textContent = "Picked: " + what + ". Every setting is in the tabs above.";
      toast("New world: " + what);
    }
  }
  /* a Restart writes the setup into the address, so a reload or a shared
   * link is this world: its size too, on a screen of any shape */
  function writeUrl() {
    if (!initArgs) return;
    try {
      var u = new URL(location.href);
      u.searchParams.set("seed", String(initArgs.seed));
      u.searchParams.set("w", String(W)); u.searchParams.set("h", String(H));
      if (initArgs.population !== KEYS.population[0]) u.searchParams.set("pop", String(initArgs.population)); else u.searchParams.delete("pop");
      if (initArgs.depth !== KEYS.depth[0]) u.searchParams.set("depth", String(initArgs.depth)); else u.searchParams.delete("depth");
      if (Object.keys(initArgs.cfg).length) u.searchParams.set("cfg", JSON.stringify(initArgs.cfg)); else u.searchParams.delete("cfg");
      u.searchParams.delete("e1");   /* its keys are in cfg now */
      history.replaceState(null, "", u);
    } catch (e) { /* file:// or a sandboxed frame */ }
  }
  /* Save setup: the running world's — its seed, size and settings, with the
   * live changes that applied since it started. Edits still waiting for a
   * Restart are not in it (they are not the running world's), and it says so. */
  function saveSetup() {
    if (!initArgs) return;
    var setup = { format: "evo_ocean_setup", version: 1, seed: initArgs.seed, population: initArgs.population,
                  depth: initArgs.depth, width: W, height: H, config: Object.assign({}, runCfg) };
    var url = URL.createObjectURL(new Blob([JSON.stringify(setup, null, 2)], { type: "application/json" }));
    var a = document.createElement("a");
    a.href = url; a.download = "evo_ocean_setup_seed" + initArgs.seed + ".json";
    document.body.appendChild(a); a.click(); a.remove();
    URL.revokeObjectURL(url);
    var waiting = el("restart").classList.contains("pending");
    toast("Saved this world's setup (seed " + initArgs.seed + ")" +
          (waiting ? ". Edits waiting for a Restart are not in it: Restart first to include them." : "."));
  }
  /* Load setup: that world from its start, at its size, exactly as saved
   * (importWorld: a value the engine refuses is reported, and nothing
   * changes). A setup without a size takes this screen's. */
  function loadSetup(text) {
    var s;
    try { s = JSON.parse(text); } catch (e) { showError("That file is not JSON: " + e.message); return; }
    if (!s || s.format !== "evo_ocean_setup" || !s.config || typeof s.config !== "object" || Array.isArray(s.config)) {
      showError("That file is not an Ocean setup (Save setup makes one)."); return;
    }
    var bad = [];
    function arg(name, def, ok) {
      if (s[name] === undefined || s[name] === null) return def;
      if (!ok(s[name])) bad.push(name + " " + JSON.stringify(s[name]));
      return s[name];
    }
    function whole(lo, hi) { return function (v) { return Number.isInteger(v) && v >= lo && v <= hi; }; }
    var seed = arg("seed", KEYS.seed[0], whole(0, 4294967295)), pop = arg("population", KEYS.population[0], whole(0, 100000));
    var depth = arg("depth", KEYS.depth[0], function (v) { return typeof v === "number" && isFinite(v); });
    var w = arg("width", null, whole(100, 100000)), h = arg("height", null, whole(100, 100000));
    if (bad.length) { showError("That setup can't start: " + bad.join(", ") + " (whole numbers in range)."); return; }
    var w0 = fixedW, h0 = fixedH;
    if (w !== null) fixedW = w;
    if (h !== null) fixedH = h;
    clearError();
    var why = importWorld(seed, pop, depth, s.config);
    if (why) {
      fixedW = w0; fixedH = h0;
      showError("That setup was refused: " + why);
      return;
    }
    el("randnote").textContent = "";
    writeUrl();
    showNote("Setup loaded: seed " + (seed >>> 0) + ", " + W + " × " + H + ", replaying from its start.");
  }

  /* ---------------- per-frame data ---------------- */
  /* Called right after oc_sync: views over this sync's buffers only. */
  function snapshot() {
    var buf = heap();
    var n = M._oc_creature_count(), fc = M._oc_food_count(), pc = M._oc_predator_count();
    return {
      buf: buf, n: n, fc: fc, pc: pc, selIdx: -1,
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
      vz: HAS_VZ ? new Float32Array(buf, M._oc_creature_vz(), n) : null,
      app: appearanceView(buf, n),
      curActive: false, curGrid: null, curCols: 0, curRows: 0
    };
  }
  /* s again, if anything grew the memory since it was cut (an export that
   * allocates: the summary, the family lines, the fish card's, the wiring, a
   * name list, a C string): the same buffers (only oc_sync and a new world
   * rewrite them, and both drop the snapshot), on the live memory */
  function fresh(s) {
    if (!s || s.buf === heap()) return s;   /* (the appearance rows: a view of this sync's too) */
    var t = snapshot();
    t.selIdx = s.selIdx;
    if (!HAS_VZ) t.vz = s.vz;   /* the estimate lives in JS */
    if (lastS === s) lastS = t;
    return t;
  }
  /* The appearance genome (oc_creature_appearance: 63 floats a fish, COSMETIC),
   * when this module exports it in a layout and laws web/ocean_appearance.js
   * knows, one row per fish of this sync; else null (the tank then draws the
   * family / brain colours). Views are made fresh after every sync. */
  function appearanceView(buf, n) {
    if (!HAS_APP || n <= 0 || M._oc_appearance_count() !== n) return null;
    var p = M._oc_creature_appearance();
    if (!p || (p & 3) || p + n * 63 * 4 > buf.byteLength) return null;
    return new Float32Array(buf, p, n * 63);
  }
  /* Without oc_creature_vz: each fish's pitch from how its depth changed over
   * the distance its head swam since the last frame, smoothed, keyed by id. */
  function vzBufs(cap) {
    return { n: 0, ids: new Float64Array(cap), x: new Float32Array(cap), y: new Float32Array(cap),
             d: new Float32Array(cap), vz: new Float32Array(cap) };
  }
  var estPrev = vzBufs(0), estNext = vzBufs(0);
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
    estNext = estPrev; estPrev = B;
    return B.vz.subarray(0, n);
  }

  /* ---------------- state ---------------- */
  var scene = null, lastS = null;
  var paused = false, stepOnce = false;
  var speedIdx = Math.max(0, SPEEDS.indexOf(Math.round(Math.max(1, Math.min(64, parseFloat(P.get("speed")) || 1)))));
  var stepMs = 0, lastSteps = 0;
  var selectedId = null, wantFollow = false, everSelected = false;
  var animT = 0, lastNow = 0;
  var fpsFrames = 0, fpsT = 0, fps = 0;
  var governorOn = true, colourBrain = false, colourMode = "family";
  var summary = null, summaryT = 0;   /* oc_summary, refreshed twice a second for the readout */
  function speed() { return SPEEDS[speedIdx]; }
  function stepsThisFrame() {
    var ask = speed();
    if (!governorOn || ask <= 1 || !(stepMs > 0)) return ask;
    return Math.max(1, Math.min(ask, Math.floor(STEP_BUDGET_MS / stepMs)));
  }
  function setPaused(on) {
    paused = on;
    var b = el("pausebtn");
    b.querySelector("use").setAttribute("href", paused ? "#i-play" : "#i-pause");
    b.setAttribute("aria-label", paused ? "play" : "pause");
    b.title = (paused ? "Play" : "Pause") + " (Space)";
    dirtyHud = true;
  }
  function changeSpeed(d) {
    speedIdx = Math.max(0, Math.min(SPEEDS.length - 1, speedIdx + d));
    el("speedlabel").textContent = speed() + "×";
    dirtyHud = true;
  }
  function indexOfId(s, id) {
    if (id === null || !s) return -1;
    for (var i = 0; i < s.n; i++) if (s.ids[i] === id) return i;
    return -1;
  }
  function randomFish(s, notId) {
    if (!s || !s.n) return -1;
    var i = Math.floor(Math.random() * s.n);
    if (s.n > 1 && s.ids[i] === notId) i = (i + 1 + Math.floor(Math.random() * (s.n - 1))) % s.n;
    return i;
  }
  function select(id) {
    var was = selectedId;
    selectedId = id;
    if (id !== null) everSelected = true;
    if ((was === null) !== (id === null) && M && has("oc_set_capture")) M._oc_set_capture(id !== null ? 1 : 0);
    dirtyHud = true;
  }
  /* F: follow the selected fish; again (or with none): a random one */
  function follow() {
    var s = fresh(lastS);
    if (!s || !s.n || turboState !== "live") return;
    var idx = !scene.following() ? indexOfId(s, selectedId) : -1;
    if (idx < 0) idx = randomFish(s, selectedId);
    select(s.ids[idx]);
    wantFollow = true;
    if (!scene.following()) scene.setFollow(true);
  }
  function unfollow() {
    wantFollow = false;
    if (scene.following()) scene.setFollow(false);
  }
  function onFollowChange(on) {
    el("followbtn").setAttribute("aria-pressed", on ? "true" : "false");
    el("followbtn").title = on ? "Stop following (F or Esc)" : "Follow this fish (F)";
    if (!on) wantFollow = false;
    if (on) toast(coarseMQ.matches ? "Following. Drag to circle around it."
                                   : "Following. Drag or A / D to circle it, W / S to come closer or back off; Esc stops.");
    updateKeysLegend(on);
    updateLight();
  }

  /* ---- the keys legend (a wide screen with a mouse; ocean.html #keys) ----
   * Shown until hidden with its × or ?, remembered. Its first line is what
   * W A S D do now: fly the camera, or move it around the followed fish. */
  var keysShown = store("evo_boids.ocean.keys") !== "0";
  function setKeysShown(on) {
    keysShown = !!on;
    document.body.classList.toggle("keys-on", keysShown);
    store("evo_boids.ocean.keys", keysShown ? "1" : "0");
  }
  function updateKeysLegend(following) {
    setHtml("keysmove", following
      ? "<span class='k'><kbd>W</kbd><kbd>S</kbd> closer / farther</span><span class='k'><kbd>A</kbd><kbd>D</kbd> circle the fish</span>" +
        "<span class='k'><kbd>Q</kbd><kbd>E</kbd> lower / higher</span>"
      : "<span class='k'><kbd>W</kbd><kbd>A</kbd><kbd>S</kbd><kbd>D</kbd> move</span><span class='k'><kbd>Q</kbd><kbd>E</kbd> down / up</span>" +
        "<span class='k'><kbd>Shift</kbd> faster</span>");
  }
  document.body.classList.toggle("keys-on", keysShown);
  updateKeysLegend(false);
  el("keysclose").addEventListener("click", function () {
    setKeysShown(false);
    toast("Keys hidden: ? shows them again.");
    this.blur();
  });

  /* ---- the flashlight (drawing only; scene.setFlashlight) ----
   * While following: on unless "Flashlight while following" is off. Else:
   * the dock button / L. The button shows the light as it is and toggles
   * whichever of the two is in charge. */
  var followLight = store("evo_boids.ocean.followLight") !== "0", lightAnywhere = false;
  function lightOn() { return scene && scene.following() ? followLight : lightAnywhere; }
  function updateLight() {
    var on = !!lightOn();
    if (scene) scene.setFlashlight(on);
    var b = el("lightbtn");
    b.setAttribute("aria-pressed", on ? "true" : "false");
    b.title = (on ? "Flashlight on (L)" : "Flashlight (L)") + ": clear light where you look; on by itself while you follow a fish";
  }
  function toggleLight() {
    if (scene && scene.following()) {
      followLight = !followLight;
      el("followLight").checked = followLight;
      store("evo_boids.ocean.followLight", followLight ? "1" : "0");
    } else {
      lightAnywhere = !lightAnywhere;
      if (lightAnywhere) toast("Flashlight on: true colours where you look, without the water's tint.");
    }
    updateLight();
  }

  /* ---------------- the readout and the stat panel ---------------- */
  var dirtyHud = true, hudT = 0, histRows = [];
  function buildHist() {
    var box = el("hist");
    for (var i = 0; i < 8; i++) {
      var row = document.createElement("div"); row.className = "row";
      var lab = document.createElement("span");
      var track = document.createElement("div"); track.className = "track";
      var bar = document.createElement("div"); bar.className = "bar";
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
  /* births / deaths per simulated second over a sliding window (the counters
   * are cumulative; a drop means a new world) */
  var rates = {
    q: [],
    reset: function () { this.q.length = 0; },
    push: function (frame, b, d) {
      var q = this.q, last = q[q.length - 1];
      if (last && (frame < last.f || b < last.b)) q.length = 0;
      if (!last || frame !== last.f) q.push({ f: frame, b: b, d: d });
      while (q.length > 2 && q[q.length - 1].f - q[0].f > 6000) q.shift();
    },
    perSec: function (key) {
      var q = this.q;
      if (q.length < 2) return null;
      var a = q[0], b = q[q.length - 1], df = (b.f - a.f) * DT;
      return df > 0 ? (b[key] - a[key]) / df : null;
    }
  };
  function fmtInt(v) { return Math.round(v).toLocaleString("en-US"); }
  function fmtTime(s) { s = Math.max(0, Math.round(s)); var m = Math.floor(s / 60); return m ? m + " min " + (s % 60) + " s" : s + " s"; }
  function setHtml(id, html) { var e = el(id); if (e && e._v !== html) { e._v = html; e.innerHTML = html; } }
  function setTxt(id, t) { var e = el(id); if (e && e._v !== t) { e._v = t; e.textContent = t; } }
  var statOpen = false;
  function setStatPanel(open) {
    statOpen = open;
    el("statpanel").hidden = !open;
    el("readout").setAttribute("aria-expanded", open ? "true" : "false");
    dirtyHud = true;
  }
  function updateHud(s, now) {
    if (!dirtyHud && now - hudT < 500) return;
    dirtyHud = false; hudT = now;
    if (has("oc_summary") && (!summary || now - summaryT > 450)) {
      summary = HO.f64(M, M._oc_summary(), M._oc_summary_len());   /* it allocates: the views after it */
      summaryT = now;
      s = fresh(s);
    }
    var st = s.stats, g = function (k) { return STATS[k] < st.length ? st[STATS[k]] : NaN; };
    var frame = g("frame");
    rates.push(frame, g("births"), g("deaths"));
    setTxt("ro-fish", fmtInt(s.n));
    setTxt("ro-gen", summary ? String(Math.round(summary[13])) : "–");
    setTxt("ro-depth", g("depthMean").toFixed(0));
    setTxt("ro-fps", String(fps));
    el("ro-fps").className = "num" + (fps && fps < 45 ? " warn" : "");
    if (!statOpen) return;
    var rb = rates.perSec("b"), rd = rates.perSec("d");
    var rows = [
      ["fish", fmtInt(s.n)],
      ["food", fmtInt(s.fc)],
      ["predators", String(s.pc)],
      ["time", fmtTime(frame * DT) + " <small>· frame " + fmtInt(frame) + "</small>"],
      ["seed", initArgs ? String(initArgs.seed) : "–"],
      ["births", fmtInt(g("births")) + (rb !== null ? " <small>· " + rb.toFixed(1) + "/s</small>" : "")],
      ["deaths", fmtInt(g("deaths")) + (rd !== null ? " <small>· " + rd.toFixed(1) + "/s</small>" : "")],
      ["founders added", fmtInt(g("reseeds"))]
    ];
    if (summary) {
      rows.push(["died", "<small>starved </small>" + fmtInt(summary[3]) + "<small> · old </small>" + fmtInt(summary[4]) + "<small> · caught </small>" + fmtInt(summary[2])]);
      rows.push(["generation", summary[12].toFixed(1) + " <small>avg · " + Math.round(summary[13]) + " max</small>"]);
      rows.push(["family lines", fmtInt(summary[19])]);
      rows.push(["neutral depth", (summary[14] * D).toFixed(0) + " <small>± " + (summary[15] * D).toFixed(0) + "</small>"]);
    }
    rows.push(["depth", g("depthMean").toFixed(0) + " ± " + g("depthSd").toFixed(0) + " <small>of " + D.toFixed(0) + "</small>"]);
    var ask = speed(), ran = paused ? ask : lastSteps;
    rows.push(["steps / frame", ran + (ran !== ask ? "<small>/" + ask + "</small>" : "") + " <small>· " + fps + " fps</small>"]);
    setHtml("statlist", rows.map(function (r) { return "<dt>" + r[0] + "</dt><dd>" + r[1] + "</dd>"; }).join(""));
    for (var i = 0; i < 8; i++) {
      var k = STATS.hist0 + i, v = k < st.length ? st[k] : 0;
      v = isFinite(v) ? v : 0;
      histRows[i].bar.style.width = Math.min(100, v * 200).toFixed(1) + "%";   /* full track = 50 % */
      histRows[i].pct.textContent = Math.round(v * 100) + "%";
    }
    var notes = [];
    if (paused) notes.push("Paused. Space or ▶ runs it; . steps once.");
    if (turboState !== "live") notes.push("Turbo is running: this picture waits for it.");
    if (scene && scene.following()) notes.push("Following a fish" + (lightOn() ? ", with the flashlight on." : "."));
    setTxt("statnotes", notes.join(" "));
  }

  /* ---------------- the fish card ---------------- */
  var cardEl = el("fishcard"), inetCanvas = el("inetc"), inetCtx = inetCanvas.getContext("2d");
  var portraitEl = el("iportrait"), portraitCtx = portraitEl.getContext("2d");
  var cardExpanded = window.innerWidth >= 600, cardCache = {}, cardFor = null, cardFlip = false;
  el("iexpand").setAttribute("aria-expanded", cardExpanded ? "true" : "false");
  var sensorNames = null, actionNames = null, netNodes = [], netHover = -1, uiFont = "";
  function prettyName(n) { return String(n).replace(/_/g, " ").toLowerCase(); }
  function brainShape() {
    if (!sensorNames) {
      sensorNames = has("oc_sensor_names") ? cString(M._oc_sensor_names()).split(",").map(prettyName) : [];
      actionNames = has("oc_action_names") ? cString(M._oc_action_names()).split(",").map(prettyName) : [];
    }
    return { nS: has("oc_num_senses") ? M._oc_num_senses() : sensorNames.length,
             nH: has("oc_num_hidden") ? M._oc_num_hidden() : 12,
             nA: has("oc_num_actions") ? M._oc_num_actions() : actionNames.length };
  }
  function cardText(id, v) { if (cardCache[id] !== v) { cardCache[id] = v; el(id).textContent = v; } }
  function setBar(id, f, tick) {
    f = Math.max(0, Math.min(1, f));
    var key = f.toFixed(3) + "|" + (tick === undefined ? "" : tick.toFixed(3));
    if (cardCache[id + "bar"] === key) return;
    cardCache[id + "bar"] = key;
    var bar = el(id);
    bar.firstElementChild.style.transform = "scaleX(" + f.toFixed(3) + ")";
    var t = bar.querySelector(".tick");
    if (t) t.style.left = "calc(" + ((tick || 0) * 100).toFixed(1) + "% - 1px)";
  }
  function showCard() {
    cardEl.hidden = false;
    document.body.classList.add("card-open");
    cardEl.classList.toggle("expanded", cardExpanded);
    el("iexpand").setAttribute("aria-expanded", cardExpanded ? "true" : "false");
    cardCache = {};
  }
  function hideCard() {
    if (cardEl.hidden) return;
    cardEl.hidden = true;
    cardFor = null; netHover = -1; cardFlip = false;
    cardEl.classList.remove("flip");
    document.body.classList.remove("card-open");
  }
  function placeCard(fx, fy) {
    var r = cardEl.getBoundingClientRect(), m = 24;
    if (fx < r.left - m || fx > r.right + m || fy < r.top - m || fy > r.bottom + m) return;
    var flip = window.innerWidth < 600 ? fy > window.innerHeight / 2 : fx > window.innerWidth / 2;
    if (flip === cardFlip) return;
    cardFlip = flip;
    cardEl.classList.toggle("flip", flip);
  }
  function drawPortrait(rgb, size, elong) {
    var dpr = window.devicePixelRatio || 1, w = 56, h = 40;
    if (portraitEl.width !== w * dpr) { portraitEl.width = w * dpr; portraitEl.height = h * dpr; }
    var g = portraitCtx;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    var k = 3.0 * bodyScale(size), len = 1 + 0.35 * Math.tanh(4 * (elong - 1));
    g.translate(w / 2 + 4, h / 2);
    g.scale(len, 1);
    g.fillStyle = "rgb(" + rgb.join(",") + ")";
    g.beginPath();
    g.moveTo(-9 * k / 1.2, 0);
    g.bezierCurveTo(-7 * k / 1.2, -4.2 * k / 1.2, 2, -4.4 * k / 1.2, 6, -1.4);
    g.lineTo(11, -5.5); g.lineTo(9.5, 0); g.lineTo(11, 5.5); g.lineTo(6, 1.4);
    g.bezierCurveTo(2, 4.4 * k / 1.2, -7 * k / 1.2, 4.2 * k / 1.2, -9 * k / 1.2, 0);
    g.fill();
    g.fillStyle = "rgba(4,14,15,0.9)";
    g.beginPath(); g.arc(-6.2 * k / 1.2, -1.2, 1.1, 0, Math.PI * 2); g.fill();
    g.setTransform(1, 0, 0, 1, 0, 0);
  }
  /* The card's swatch: the fish's skin pattern on its flank (unlit, from its
   * appearance genes; web/ocean_appearance.js), when the module exports it. */
  var swatchEl = el("iswatch");
  function drawSwatch(s, idx, size, elong) {
    var EA = window.EvoAppearance;
    if (!swatchEl || !EA || !s.app) { if (swatchEl) swatchEl.hidden = true; return; }
    var W = 96, H = 24, row = s.app.subarray(63 * idx, 63 * idx + 63);
    swatchEl.width = W; swatchEl.height = H;
    var ctx = swatchEl.getContext("2d");
    ctx.putImageData(new ImageData(EA.flankImage(row, EA.lambdaOf(size, elong), W, H), W, H), 0, 0);
    swatchEl.hidden = false;
  }
  /* oc_inspect(idx) right after the sync: its numbers, then the card */
  function inspect(idx) {
    if (!has("oc_inspect")) return null;
    var sh = brainShape();   /* first: the name lists allocate (once) */
    if (!M._oc_inspect(idx)) return null;
    var buf = heap(), nc = M._oc_inspect_conn_count();   /* after oc_inspect, which allocates */
    return {
      info: new Float64Array(buf, M._oc_inspect_info(), M._oc_inspect_len()),
      sensors: new Float32Array(buf, M._oc_inspect_sensors(), sh.nS),
      hidden: new Float32Array(buf, M._oc_inspect_hidden(), sh.nH),
      actions: new Float32Array(buf, M._oc_inspect_actions(), sh.nA),
      connCount: nc, conns: new Float32Array(buf, M._oc_inspect_conns(), nc * 5), shape: sh
    };
  }
  function updateCard(s) {
    var idx = s.selIdx, f = idx >= 0 && turboState === "live" ? inspect(idx) : null;
    if (!f) { hideCard(); return s; }
    s = fresh(s);   /* oc_inspect may have grown the memory */
    if (cardEl.hidden) showCard();
    var p = scene.project(s, idx);
    if (p) placeCard(p.x, p.y);
    var info = f.info, id = Math.round(info[0]);
    var rgb = [s.colors[3 * idx], s.colors[3 * idx + 1], s.colors[3 * idx + 2]];
    var pkey = id + "|" + rgb.join() + "|" + info[4].toFixed(2) + "|" + info[5].toFixed(2);
    if (cardFor !== pkey) { cardFor = pkey; drawPortrait(rgb, info[4], info[5]); drawSwatch(s, idx, info[4], info[5]); }
    cardText("ititle", "Fish #" + id);
    cardText("isub", "Generation " + Math.round(info[6]) + " · family #" + Math.round(info[7]) + " · " + Math.round(info[11]) + " genes" +
             (cfgVal("bodyModel") ? " · body " + info[4].toFixed(2) + " × " + info[5].toFixed(2) : ""));
    var reproE = cfgVal("reproEnergy"), maxAge = cfgVal("maxAge"), maxSpd = cfgVal("maxSpeed"), pitchMax = cfgVal("pitchMax");
    cardText("ienergyv", info[2].toFixed(1));
    setBar("ienergy", info[2] / (reproE * 2), 0.5);
    el("ienergy").title = info[2] >= reproE ? "enough energy to breed" : "needs " + reproE + " to breed";
    cardText("iagev", Math.round(info[3]) + " s");
    setBar("iage", info[3] / maxAge);
    cardText("ispeedv", String(Math.round(info[1])));
    setBar("ispeed", info[1] / maxSpd);
    cardText("idepthv", info[8].toFixed(0) + " of " + D.toFixed(0));
    setBar("idepth", info[8] / D, info[9]);
    el("idepth").title = "the tick: its neutral depth";
    cardText("ibuoyv", (info[9] * D).toFixed(0));
    setBar("ibuoy", info[9]);
    var deg = Math.atan(info[10]) * 180 / Math.PI;
    cardText("ipitchv", Math.abs(deg) < 0.5 ? "level" : Math.abs(deg).toFixed(0) + "° " + (deg > 0 ? "down" : "up"));
    setBar("ipitch", 0.5 + 0.5 * Math.max(-1, Math.min(1, info[10] / (pitchMax || 0.5))), 0.5);
    if (cardExpanded) drawBrain(f);
    return s;
  }
  function brainColor(v, a) {
    var m = Math.min(1, Math.abs(v));
    return v >= 0 ? "rgba(" + Math.round(70 + 130 * m) + "," + Math.round(90 + 150 * m) + "," + Math.round(60 + 10 * m) + "," + a + ")"
                  : "rgba(" + Math.round(80 + 175 * m) + "," + Math.round(90 + 20 * m) + "," + Math.round(70 + 10 * m) + "," + a + ")";
  }
  /* senses -> hidden -> actions (renderer.js drawBrain, the Ocean's shape) */
  function drawBrain(f) {
    var box = inetCanvas.parentNode, w = box.clientWidth, h = box.clientHeight;
    if (w < 60 || h < 60) return;
    var sh = f.shape, nS = sh.nS, nH = sh.nH, nA = sh.nA;
    var dpr = window.devicePixelRatio || 1, bw = Math.round(w * dpr), bh = Math.round(h * dpr);
    if (inetCanvas.width !== bw) inetCanvas.width = bw;
    if (inetCanvas.height !== bh) inetCanvas.height = bh;
    var g = inetCtx, i;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    var wide = w > h * 1.15, pos = { sen: [], hid: [], act: [] }, m = 14;
    if (wide) {
      var yIn = 26, yOut = h - 40, yHid = (yIn + yOut) / 2;
      for (i = 0; i < nS; i++) pos.sen.push([m + (w - 2 * m) * i / (nS - 1), yIn]);
      for (i = 0; i < nH; i++) pos.hid.push([m + 18 + (w - 2 * m - 36) * i / (nH - 1), yHid]);
      for (i = 0; i < nA; i++) pos.act.push([m + 18 + (w - 2 * m - 36) * i / (nA - 1), yOut]);
    } else {
      var yTop = 26, yBot = h - 12, xS = 18, xA = w - 100, xH = (xS + xA) / 2;
      for (i = 0; i < nS; i++) pos.sen.push([xS, yTop + (yBot - yTop) * i / (nS - 1)]);
      for (i = 0; i < nH; i++) pos.hid.push([xH, yTop + (yBot - yTop) * i / (nH - 1)]);
      for (i = 0; i < nA; i++) pos.act.push([xA, yTop + (yBot - yTop) * i / (nA - 1)]);
    }
    var hoverKey = netHover >= 0 && netNodes[netHover] ? netNodes[netHover].kind + netNodes[netHover].i : null;
    for (var k = 0; k < f.connCount; k++) {
      var o = k * 5;
      var srcSen = f.conns[o] !== 0, srcNum = f.conns[o + 1] | 0, sinkAct = f.conns[o + 2] !== 0, sinkNum = f.conns[o + 3] | 0, wgt = f.conns[o + 4];
      var a = srcSen ? pos.sen[srcNum] : pos.hid[srcNum], b = sinkAct ? pos.act[sinkNum] : pos.hid[sinkNum];
      if (!a || !b) continue;
      var srcAct = srcSen ? f.sensors[srcNum] : f.hidden[srcNum];
      var mag = Math.min(1, Math.abs(wgt) * Math.abs(srcAct) * 1.5);
      var hl = hoverKey !== null && ((srcSen ? "Sense" : "Hidden neuron") + srcNum === hoverKey ||
                                     (sinkAct ? "Action" : "Hidden neuron") + sinkNum === hoverKey);
      g.strokeStyle = (wgt >= 0 ? "rgba(180,235,80," : "rgba(255,120,80,") + (hl ? 0.9 : (0.10 + 0.7 * mag)).toFixed(3) + ")";
      g.lineWidth = hl ? 2 : 0.8 + 1.6 * mag;
      g.beginPath();
      g.moveTo(a[0], a[1]);
      if (wide) g.bezierCurveTo(a[0], (a[1] + b[1]) / 2, b[0], (a[1] + b[1]) / 2, b[0], b[1]);
      else g.bezierCurveTo((a[0] + b[0]) / 2, a[1], (a[0] + b[0]) / 2, b[1], b[0], b[1]);
      g.stroke();
    }
    netNodes = [];
    function node(p, r, v, kind, idx) {
      g.fillStyle = brainColor(v, 1);
      g.beginPath(); g.arc(p[0], p[1], r, 0, Math.PI * 2); g.fill();
      g.strokeStyle = "rgba(4,14,16,0.85)"; g.lineWidth = 1; g.stroke();
      netNodes.push({ x: p[0], y: p[1], kind: kind, i: idx, v: v });
    }
    var rs = Math.max(2, Math.min(3.2, (wide ? w : h) / nS * 0.36));
    for (i = 0; i < nS; i++) node(pos.sen[i], rs, f.sensors[i], "Sense", i);
    for (i = 0; i < nH; i++) node(pos.hid[i], 5, f.hidden[i], "Hidden neuron", i);
    for (i = 0; i < nA; i++) node(pos.act[i], 5.5, f.actions[i], "Action", i);
    if (netHover >= 0 && netNodes[netHover]) {
      var hn = netNodes[netHover];
      g.strokeStyle = "rgba(230,255,160,0.95)"; g.lineWidth = 2;
      g.beginPath(); g.arc(hn.x, hn.y, 8, 0, Math.PI * 2); g.stroke();
    }
    if (!uiFont) uiFont = getComputedStyle(document.body).fontFamily;
    g.font = "11px " + uiFont;
    function tag(text, x, y) {
      var tw = g.measureText(text).width;
      g.fillStyle = "rgba(15,39,39,0.92)"; g.fillRect(x - 3, y - 10, tw + 6, 14);
      g.fillStyle = "rgba(190,210,205,0.95)"; g.fillText(text, x, y);
    }
    g.fillStyle = "rgba(190,210,205,0.9)";
    if (wide) {
      tag("senses", m, 13);
      tag("hidden", m, (26 + h - 40) / 2 - 9);
      g.fillStyle = "rgba(190,210,205,0.9)";
      g.textAlign = "center";
      for (i = 0; i < nA; i++) g.fillText(ACTION_SHORT[i] || "", pos.act[i][0], h - 40 + (i % 2 ? 30 : 18));
      g.textAlign = "left";
    } else {
      g.fillText("senses", 6, 13);
      g.fillText("hidden", (18 + w - 100) / 2 - 18, 13);
      g.fillText("actions", w - 106, 13);
      for (i = 0; i < nA; i++) g.fillText(ACTION_LABELS[i] || "", pos.act[i][0] + 10, pos.act[i][1] + 4);
    }
    updateSenseLine();
  }
  var DEFAULT_SENSE = "Senses on one side, actions on the other. Lime links excite, orange ones inhibit; brighter = stronger right now. Tap a dot to name it.";
  function updateSenseLine() {
    if (netHover >= 0 && netNodes[netHover]) {
      var n = netNodes[netHover];
      var label = n.kind === "Sense" ? (sensorNames[n.i] || "sense " + (n.i + 1)) : n.kind === "Action" ? (ACTION_LABELS[n.i] || actionNames[n.i]) : "neuron " + (n.i + 1);
      var html = n.kind + ": <b></b> " + (n.v >= 0 ? "+" : "") + n.v.toFixed(2);
      if (cardCache.sense !== html + label) {
        cardCache.sense = html + label;
        el("isense").innerHTML = html;
        el("isense").querySelector("b").textContent = label;
      }
    } else if (cardCache.sense !== "default") {
      cardCache.sense = "default";
      el("isense").textContent = coarseMQ.matches ? DEFAULT_SENSE : DEFAULT_SENSE.replace("Tap", "Point at");
    }
  }
  function nearestNode(e) {
    var r = inetCanvas.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
    var best = -1, bd = e.pointerType === "mouse" ? 100 : 400;
    netNodes.forEach(function (n, i) { var d = (n.x - x) * (n.x - x) + (n.y - y) * (n.y - y); if (d < bd) { bd = d; best = i; } });
    return best;
  }
  inetCanvas.addEventListener("pointermove", function (e) { if (e.pointerType === "mouse") netHover = nearestNode(e); });
  inetCanvas.addEventListener("pointerdown", function (e) { if (e.pointerType !== "mouse") netHover = nearestNode(e); });
  inetCanvas.addEventListener("pointerleave", function (e) { if (e.pointerType === "mouse") netHover = -1; });

  /* ---------------- the dashboards' recorder ---------------- */
  var sampleBucket = -1;
  function dash() { return window.EvoOceanDash || null; }
  /* one sample of this frame's synced world: the row the Turbo worker records
   * too (web/ocean_handoff.js row(): the stats copied before the summary and
   * the family lines allocate) */
  function takeSample(st) {
    if (!dash() || !has("oc_summary")) return;
    var every = dash().every(), b = Math.floor(st[1] / every);
    if (b === sampleBucket) return;
    sampleBucket = b;
    dash().push(HO.row(M, dash().famK, statsLen));
  }
  function wiringNow() {
    if (!M || turboState !== "live" || !has("oc_wiring")) return null;
    var sh = brainShape(), n = M._oc_wiring();   /* both allocate: the view after them */
    var data = new Float32Array(heap(), M._oc_wiring_data(), 2 * sh.nS * sh.nA);
    return { at: performance.now(), n: n, senses: sh.nS, actions: sh.nA,
             path: Array.prototype.slice.call(data, 0, sh.nS * sh.nA), direct: Array.prototype.slice.call(data, sh.nS * sh.nA),
             senseNames: cString(M._oc_sensor_names()).split(","), actionNames: cString(M._oc_action_names()).split(",") };
  }

  /* ---------------- TURBO (web/ocean_worker.js) ----------------
   * live -> entering -> turbo -> exiting -> live. The worker gets a copy of
   * this module's memory and hands its own back on Stop; meanwhile this page
   * keeps drawing the frozen world under the Turbo card, and live settings
   * go to the worker. */
  var turboState = "live", turboWorker = null, turboStats = null;
  function updateLocks() {
    var tb = el("turbobtn"), on = turboState === "turbo" || turboState === "exiting";
    tb.setAttribute("aria-pressed", on ? "true" : "false");
    tb.disabled = turboState === "entering" || turboState === "exiting";
    var locked = turboState !== "live";
    ["restart", "savecp", "loadcp", "pausebtn", "slower", "faster", "randombtn", "randombig", "newworld", "replay"].forEach(function (id) {
      el(id).classList.toggle("locked", locked);
    });
    el("turbo").hidden = turboState === "live";
    el("turboexit").disabled = turboState !== "turbo";
    el("turboexit").textContent = turboState === "exiting" ? "Syncing back…" : turboState === "entering" ? "Starting…" : "Stop Turbo and watch";
    if (turboState !== "live") paintTurbo();
    dirtyHud = true;
  }
  function paintTurbo() {
    var t = turboStats;
    el("ts-rate").textContent = t ? Math.round(t.stepsPerSec).toLocaleString() : "–";
    el("ts-steps").textContent = t ? t.totalSteps.toLocaleString() : "–";
    el("ts-pop").textContent = t ? Math.round(t.pop).toLocaleString() : "–";
    el("ts-food").textContent = t ? Math.round(t.food).toLocaleString() : "–";
  }
  var turboEntryCfg = null;   /* runCfg when Turbo took the world */
  function onTurboMessage(e) {
    var msg = e.data || {};
    if (msg.type === "stats") {
      turboStats = msg;
      if (turboState === "entering") { turboState = "turbo"; updateLocks(); flushHeld(); }
      paintTurbo();
    } else if (msg.type === "rec") {
      if (dash()) dash().ingest(msg.rows);
    } else if (msg.type === "set") {
      for (var i = 0; i < inflight.length; i++) if (inflight[i][0] === msg.key) { inflight.splice(i, 1); break; }
      if (msg.ok) liveApplied(msg.key, valOf(msg.key, msg.value));
      else refuse(msg.key, msg.error, inflight.some(function (f) { return f[0] === msg.key; }));
    } else if (msg.type === "memory") {
      if (turboState !== "exiting") return;
      try { takeMemory(msg.mem); }
      catch (err) {
        /* the worker still has the world: it carries on, nothing is lost */
        turboWorker.postMessage({ cmd: "resume" });
        turboState = "turbo";
        updateLocks();
        showError("Turbo could not hand the world back (" + (err.message || err) + "), so it keeps running. Stop it again to retry.");
        flushHeld();
        return;
      }
      turboBack();
      showNote("Turbo ran " + (msg.totalSteps || 0).toLocaleString() + " steps.");
    } else if (msg.type === "error") {
      turboFailed("Turbo stopped: " + (msg.message || "unknown error"), msg.mem);
    }
  }
  /* the page owns the world again (its memory: the worker's, or its own) */
  function turboBack(quiet) {
    turboState = "live";
    inflight.forEach(function (f) { heldLive[f[0]] = f[1]; });   /* never answered: never applied */
    inflight = [];
    afterNewWorld(true);
    sampleBucket = Math.floor(M._oc_stats ? frameNow() / (dash() ? dash().every() : 60) : -1);
    setPaused(false);
    updateLocks();
    flushHeld(quiet);
  }
  /* The worker stopped on an error, or died. With its memory (the world was
   * whole), the run goes on from where Turbo got it, as after a Stop.
   * Without, from where Turbo took it: this page's own copy, untouched
   * since; Turbo's steps are lost, and the live changes that applied in the
   * worker are applied again here. Either way the worker is done: a fresh
   * one runs the next Turbo. */
  function turboFailed(why, mem) {
    var was = turboState;
    dropWorker();
    if (was === "live") return;
    var back = false;
    if (mem && was !== "entering") {
      try { takeMemory(mem); back = true; } catch (err) { why += "; the world it had could not come back (" + (err.message || err) + ")"; }
    }
    var redo = [];
    if (!back && was !== "entering" && turboEntryCfg) {
      var now = runCfg, before = turboEntryCfg;
      Object.keys(Object.assign({}, now, before)).forEach(function (k) {
        var a = Object.prototype.hasOwnProperty.call(now, k) ? now[k] : KEYS[k] && KEYS[k][0];
        var b = Object.prototype.hasOwnProperty.call(before, k) ? before[k] : KEYS[k] && KEYS[k][0];
        if (LIVE[k] && a !== b && !(k in heldLive)) { heldLive[k] = numOf(k, a); redo.push(LABELS[k] || k); }
      });
      runCfg = Object.assign({}, before);
    }
    turboBack(true);
    if (!back && was !== "entering" && dash() && dash().rewind) dash().rewind(frameNow());
    showError(why + (back ? ". The world is back where Turbo got it." :
              was === "entering" ? "." : ". Its steps are lost: the world is back where Turbo took it" +
              (redo.length ? ", with " + redo.join(", ") + " applied again." : ".")));
  }
  function dropWorker() {
    if (!turboWorker) return;
    turboWorker.onmessage = null;
    turboWorker.onerror = null;
    turboWorker.terminate();
    turboWorker = null;
  }
  function frameNow() { M._oc_sync(); return HO.f64(M, M._oc_stats(), statsLen)[1]; }
  function enterTurbo() {
    if (turboState !== "live" || !M || MOCK) { if (MOCK) showNote("Turbo needs the real engine."); return; }
    turboState = "entering";
    turboStats = null;
    turboEntryCfg = Object.assign({}, runCfg);
    inflight = [];
    select(null);
    unfollow();
    updateLocks();
    if (!turboWorker) {
      turboWorker = new Worker(A.worker);
      turboWorker.onmessage = onTurboMessage;
      turboWorker.onerror = function (ev) {
        if (ev && ev.preventDefault) ev.preventDefault();
        turboFailed("The Turbo worker failed: " + (ev && ev.message || "could not load ocean_worker.js"), null);
      };
    }
    var mem = heap().slice(0);
    turboWorker.postMessage({ cmd: "start", js: moduleUrls.js, wasm: moduleUrls.wasm, handoff: moduleUrls.handoff, mem: mem,
                              recEvery: dash() ? dash().every() : 60, famK: dash() ? dash().famK : 8 }, [mem]);
  }
  function exitTurbo() {
    if (turboState !== "turbo") return;
    turboState = "exiting";
    updateLocks();
    turboWorker.postMessage({ cmd: "stop" });
  }

  /* ---------------- Hold 60 fps ----------------
   * The step budget (stepsThisFrame) first; then, frames still slow at one
   * step, the tank's detail, a level at a time. Never the simulation. */
  var slowSince = 0, detail0 = 0;
  function govern(now) {
    if (!governorOn || paused || turboState !== "live" || !scene) { slowSince = 0; return; }
    if (fps && fps < 48 && lastSteps <= 1) {
      if (!slowSince) slowSince = now;
      else if (now - slowSince > 3000) {
        slowSince = 0;
        if (scene.degrade()) toast("Drawing the tank with less detail, to hold 60 fps.");
      }
    } else slowSince = 0;
  }
  function restoreDetail() { if (scene && detail0) scene.setQuality(detail0); }

  /* ---------------- the loop ---------------- */
  var splashDone = false;
  function frame(now) {
    requestAnimationFrame(frame);
    var dt = lastNow ? Math.min(0.1, (now - lastNow) / 1000) : 0;
    lastNow = now;
    var steps = 0;
    if (turboState === "live" && (!paused || stepOnce)) {
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
    if (turboState === "live") { takeSample(s.stats); s = fresh(s); }
    if (!HAS_VZ) s.vz = vzEstimate(s, steps > 0);
    var ax = { f: 0, r: 0, u: 0 };
    for (var key in heldMoveKeys) ax[heldMoveKeys[key][0]] += heldMoveKeys[key][1];
    scene.setMove(ax.f, ax.r, ax.u, shiftHeld);
    /* the selected fish, by id; a followed fish that died hands over to another */
    var idx = turboState === "live" ? indexOfId(s, selectedId) : -1;
    if (idx < 0 && selectedId !== null && turboState === "live") {
      if (wantFollow && s.n) { idx = randomFish(s, null); select(s.ids[idx]); }
      else select(null);
    }
    s.selIdx = idx;
    lastS = s;
    if (!(dash() && dash().isOpen())) scene.render(s, { animT: animT, dtSim: steps * DT, selIdx: idx });
    s = updateCard(s);
    fpsFrames++;
    if (now - fpsT >= 1000) { fps = Math.round(fpsFrames * 1000 / (now - fpsT)); fpsFrames = 0; fpsT = now; dirtyHud = true; govern(now); }
    updateHud(s, now);
    if (!splashDone) { splashDone = true; el("splash").classList.add("done"); }
  }

  /* ---------------- input ---------------- */
  /* The camera's movement keys, held (scene.setMove): W A S D by their place
   * on the keyboard (e.code: the same four keys on any layout), Q / E and
   * PageDown / PageUp down / up, Shift faster. [axis, sign]: f forward, r
   * right, u up. Never while typing in a field. */
  var MOVE_CODES = { KeyW: ["f", 1], KeyS: ["f", -1], KeyD: ["r", 1], KeyA: ["r", -1], KeyE: ["u", 1], KeyQ: ["u", -1] };
  var MOVE_KEYS = { pageup: ["u", 1], pagedown: ["u", -1] };
  var heldMoveKeys = {}, shiftHeld = false;
  function releaseMoveKeys() { heldMoveKeys = {}; shiftHeld = false; }
  /* a focused field takes the keys: the text inputs, selects, text areas and
   * editable text (the settings' number fields among them) */
  function typingIn(t) {
    if (!t || !t.tagName) return false;
    return t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.tagName === "SELECT" || !!t.isContentEditable;
  }
  var escHandledElsewhere = false;
  window.addEventListener("keydown", function (e) {
    if (e.key === "Escape") escHandledElsewhere = !!(dash() && dash().isOpen()) || !!document.getElementById("guide");
  }, true);
  function wireInput() {
    document.addEventListener("keyup", function (e) {
      delete heldMoveKeys["c:" + e.code];
      delete heldMoveKeys["k:" + String(e.key).toLowerCase()];
      if (e.key === "Shift") shiftHeld = false;
    });
    window.addEventListener("blur", releaseMoveKeys);
    document.addEventListener("visibilitychange", function () { if (document.hidden) releaseMoveKeys(); });
    document.addEventListener("focusin", function (e) { if (typingIn(e.target)) releaseMoveKeys(); });
    window.addEventListener("keydown", function (e) {
      if (e.key === "Escape") {
        if (drawerEl.classList.contains("open")) setDrawer(false);
        else if (statOpen) setStatPanel(false);
        else if (!escHandledElsewhere) { if (scene.following()) unfollow(); else select(null); }
        if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
        return;
      }
      var t = e.target;
      if (typingIn(t)) return;
      if (e.key === "Shift") { shiftHeld = true; return; }
      if (e.ctrlKey || e.metaKey || e.altKey) return;
      var k = e.key, lk = String(k).toLowerCase();
      /* the camera's movement: also with a button focused (letters don't press it) */
      var mv = MOVE_CODES[e.code], mk = mv ? "c:" + e.code : null;
      if (!mv && MOVE_KEYS[lk]) { mv = MOVE_KEYS[lk]; mk = "k:" + lk; }
      if (mv) {
        heldMoveKeys[mk] = mv; shiftHeld = e.shiftKey;
        e.preventDefault();
        return;
      }
      if (k === "?") { setKeysShown(!keysShown); return; }
      if (t && t.tagName === "BUTTON") return;   /* Space / Enter belong to the button */
      /* the view: always */
      if (lk === "c") { wantFollow = false; scene.resetView(true); return; }
      if (lk === "l") { toggleLight(); return; }
      if (lk === "f") { follow(); return; }
      /* the world: not while Turbo owns it */
      if (turboState !== "live") {
        if (k === " " || k === "ArrowUp" || k === "ArrowDown" || k === "+" || k === "=" || k === "-" || lk === "r" || k === ".") {
          showNote("Stop Turbo first."); e.preventDefault();
        }
        return;
      }
      if (k === " " || k === "Spacebar") { setPaused(!paused); e.preventDefault(); }
      else if (k === "+" || k === "=" || k === "ArrowUp") { changeSpeed(1); e.preventDefault(); }
      else if (k === "-" || k === "_" || k === "ArrowDown") { changeSpeed(-1); e.preventDefault(); }
      else if (lk === "r") { if (e.shiftKey) replay(); else newWorld(); }
      else if (k === "." && paused) stepOnce = true;
    });
    /* a short tap or click selects the fish nearest it on screen; empty water
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
      var s = fresh(lastS);
      if (moved > 8 || held > 600 || !s || turboState !== "live") return;
      var best = scene.pick(s, e.clientX, e.clientY, e.pointerType === "mouse" ? 18 : 30);
      if (best < 0 && scene.following()) return;   /* a stray tap doesn't drop a follow */
      if (best >= 0 && scene.following() && s.ids[best] !== selectedId) unfollow();
      select(best >= 0 ? s.ids[best] : null);
    });
    var fitTimer = 0;
    window.addEventListener("resize", function () {
      scene.resize(); updateInsets();
      clearTimeout(fitTimer);
      fitTimer = setTimeout(function () { updateFitButton(); updateInsets(); }, 250);
    });
    if (reducedMQ.addEventListener) reducedMQ.addEventListener("change", function () { scene.setReducedMotion(reducedMQ.matches); });
  }
  /* The part of the screen the tank is framed in (renderer.js updateInsets):
   * below the top bar and above the dock on a desktop, the whole screen on
   * compact ones, where the controls float over it; less the open settings. */
  function updateInsets() {
    if (!scene) return;
    var vh = window.innerHeight, open = drawerEl.classList.contains("open");
    var top = 0, bottom = 0, right = 0;
    if (!compactMQ.matches) {
      top = el("topbar").getBoundingClientRect().bottom + 6;
      bottom = vh - el("dock").getBoundingClientRect().top + 6;
    }
    if (open && wideMQ.matches) right = drawerEl.getBoundingClientRect().width;
    else if (open) {
      var sheetTop = drawerEl.getBoundingClientRect().top;
      bottom = sheetTop - top >= vh * 0.3 ? vh - sheetTop : bottom;
    }
    scene.setInsets({ top: Math.round(top), right: Math.round(right), bottom: Math.round(Math.max(0, bottom)), left: 0 });
  }

  /* ---- settings: a bottom sheet on narrow screens, a side panel on wide ones ---- */
  var drawerEl = el("drawer"), menuBtn = el("menubtn");
  function setDrawer(open) {
    if (open && dash()) dash().close();
    drawerEl.classList.toggle("open", open);
    document.body.classList.toggle("sheet-open", open);
    menuBtn.setAttribute("aria-expanded", open ? "true" : "false");
    if (!open) hideTip();
    if (open) setStatPanel(false);
    if (scene) { updateInsets(); setTimeout(updateInsets, 320); }
  }
  menuBtn.addEventListener("click", function () { setDrawer(!drawerEl.classList.contains("open")); this.blur(); });
  el("drawerclose").addEventListener("click", function () { setDrawer(false); this.blur(); });
  (function () {   /* drag the sheet's grip (or header) down to close it */
    var start = null, grip = el("grip"), head = drawerEl.querySelector(".sheet-head");
    function down(e) {
      if (wideMQ.matches || e.target.closest("button")) return;
      start = { y: e.clientY, id: e.pointerId, dy: 0 };
      drawerEl.classList.add("dragging");
      e.currentTarget.setPointerCapture(e.pointerId);
    }
    function move(e) {
      if (!start || e.pointerId !== start.id) return;
      start.dy = Math.max(0, e.clientY - start.y);
      drawerEl.style.transform = "translateY(" + start.dy + "px)";
    }
    function up(e) {
      if (!start || e.pointerId !== start.id) return;
      drawerEl.classList.remove("dragging");
      drawerEl.style.transform = "";
      if (start.dy > Math.min(120, drawerEl.offsetHeight * 0.25)) setDrawer(false);
      start = null;
    }
    [grip, head].forEach(function (n) {
      n.addEventListener("pointerdown", down); n.addEventListener("pointermove", move);
      n.addEventListener("pointerup", up); n.addEventListener("pointercancel", up);
    });
  })();
  el("readout").addEventListener("click", function () { setStatPanel(!statOpen); this.blur(); });
  document.addEventListener("pointerdown", function (e) {
    if (statOpen && !e.target.closest("#statpanel, #readout")) setStatPanel(false);
  });

  /* ---- the shared hover tooltip (renderer.js's) ---- */
  var tipEl = document.createElement("div");
  tipEl.id = "tooltip";
  var tipDesc = document.createElement("div"); tipDesc.className = "tipd";
  var tipTry = document.createElement("div"); tipTry.className = "tipt";
  tipEl.appendChild(tipDesc); tipEl.appendChild(tipTry);
  document.body.appendChild(tipEl);
  var tipTimer = 0, tipFor = null;
  function hideTip() { clearTimeout(tipTimer); tipEl.style.display = "none"; tipFor = null; }
  function showTip(rowEl, tip) {
    tipDesc.textContent = tip.d; tipTry.textContent = tip.t;
    tipEl.style.display = "block";
    tipFor = rowEl;
    var dr = drawerEl.getBoundingClientRect(), lr = rowEl.getBoundingClientRect(), left, top;
    if (wideMQ.matches && dr.left >= tipEl.offsetWidth + 16) {
      left = Math.max(6, dr.left - 10 - tipEl.offsetWidth);
      top = lr.top + lr.height / 2 - tipEl.offsetHeight / 2;
    } else {
      left = Math.max(8, Math.min(lr.left, window.innerWidth - tipEl.offsetWidth - 8));
      top = lr.bottom + 4;
      if (top + tipEl.offsetHeight > window.innerHeight - 6) top = lr.top - 4 - tipEl.offsetHeight;
    }
    top = Math.max(6, Math.min(top, window.innerHeight - tipEl.offsetHeight - 6));
    tipEl.style.left = left + "px"; tipEl.style.top = top + "px";
  }
  function attachTip(rowEl, key) {
    var tip = CFG.TIPS[key];
    if (!tip || !rowEl) return;
    rowEl.addEventListener("mouseenter", function () {
      if (lastPointer !== "mouse") return;
      clearTimeout(tipTimer);
      tipTimer = setTimeout(function () { showTip(rowEl, tip); }, 200);
    });
    rowEl.addEventListener("mouseleave", function () { if (lastPointer === "mouse") hideTip(); });
    var name = rowEl.querySelector(".lt");
    if (name) name.addEventListener("click", function (e) {
      if (lastPointer === "mouse") return;
      e.preventDefault();
      if (tipFor === rowEl) hideTip(); else showTip(rowEl, tip);
    });
  }
  panelsEl.addEventListener("scroll", hideTip);
  document.addEventListener("pointerdown", function (e) {
    if (tipFor && e.pointerType !== "mouse" && !(e.target.closest && e.target.closest(".lt"))) hideTip();
  });

  /* ---- buttons ---- */
  function btn(id, fn) { el(id).addEventListener("click", function () { fn(); this.blur(); }); }
  buildSettings();
  panelsEl.addEventListener("input", onEdit);
  panelsEl.addEventListener("change", onEdit);
  btn("restart", restart);
  btn("pausebtn", function () { if (guardTurbo()) return; setPaused(!paused); });
  btn("slower", function () { if (guardTurbo()) return; changeSpeed(-1); });
  btn("faster", function () { if (guardTurbo()) return; changeSpeed(1); });
  btn("turbobtn", function () { if (turboState === "live") enterTurbo(); else if (turboState === "turbo") exitTurbo(); });
  btn("turboexit", exitTurbo);
  btn("randombtn", randomize);
  btn("randombig", randomize);
  btn("newworld", newWorld);
  btn("replay", replay);
  btn("lightbtn", toggleLight);
  btn("viewbtn", function () { if (scene) { wantFollow = false; scene.resetView(true); } });
  btn("fitbtn", fitToScreen);
  btn("followbtn", function () {
    if (!scene) return;
    if (scene.following()) unfollow();
    else if (selectedId !== null) { wantFollow = true; scene.setFollow(true); }
  });
  btn("iclose", function () { unfollow(); select(null); });
  btn("iexpand", function () {
    cardExpanded = !cardExpanded;
    cardEl.classList.toggle("expanded", cardExpanded);
    el("iexpand").setAttribute("aria-expanded", cardExpanded ? "true" : "false");
    el("iexpand").title = cardExpanded ? "Hide its brain" : "Show its brain";
    cardCache.sense = null;
  });
  btn("advreset", function () {
    resetForm();
    setPending(true); updateNeeds(); updateTabDots();
    showNote("Every setting is back to its default. Restart to apply.");
  });
  btn("guidebtn", function () { if (window.EvoGuide) EvoGuide.start(); });
  btn("savecp", function () { if (guardTurbo()) return; saveSetup(); });
  btn("loadcp", function () { if (guardTurbo()) return; el("cpfile").click(); });
  el("cpfile").addEventListener("change", function () {
    var file = this.files && this.files[0];
    this.value = "";
    if (!file || !M || guardTurbo()) return;
    file.text().then(loadSetup, function (err) { showError("Couldn't read that file: " + err); });
  });
  el("governor").addEventListener("change", function () {
    governorOn = this.checked;
    if (!governorOn) restoreDetail();
    this.blur();
  });
  el("followLight").addEventListener("change", function () {
    followLight = this.checked;
    store("evo_boids.ocean.followLight", followLight ? "1" : "0");
    updateLight();
    this.blur();
  });
  el("followLight").checked = followLight;
  /* Fish colour: by family line (the default), by brain similarity, or the
   * fish's own appearance genome (skin pattern and body shape; the Ocean
   * only). Family / brain is shared with Classic (the same key and ?colour=);
   * Classic reads "appearance" as family. Display only. */
  (function () {
    var want = P.get("colour"), saved = store("evo_boids.colourBy");
    if (want !== "brain" && want !== "family" && want !== "appearance")
      want = saved === "brain" || saved === "appearance" ? saved : "family";
    colourMode = want;
    colourBrain = want === "brain";
    el("colourMode").value = colourMode;
  })();
  el("colourMode").addEventListener("change", function () {
    colourMode = this.value === "brain" || this.value === "appearance" ? this.value : "family";
    colourBrain = colourMode === "brain";
    store("evo_boids.colourBy", colourMode);
    if (M) applyDisplaySwitches();
    updateModeline();
    this.blur();
  });
  function updateModeline() {
    el("modeline").textContent = "Running the single-threaded Ocean module, reproducible from the seed" +
      (HAS_EYE ? " (the eye build)" : "") + ". Drawing: the 3D tank" + (scene ? ", detail " + scene.quality() + " of 3" : "") +
      (colourMode === "appearance" ? (HAS_APP ? ", each fish drawn from its appearance genes" : ", coloured by family (this module has no appearance genes)")
       : colourBrain ? ", coloured by brain" : ", coloured by family") + ".";
  }
  setPaused(false);
  changeSpeed(0);

  /* ---------------- the tour (web/guide.js reads this) ---------------- */
  var guideFish = null;
  window.EvoGuideConfig = {
    key: "evo_boids.ocean.guideSeen.v1", introKey: "evo_boids.ocean.turboIntroSeen.v1",
    steps: function (h) {
      var tap = function () { return h.touch() ? "Tap" : "Click"; };
      return [
        { welcome: true, drawer: false, title: "Welcome to evo_boids",
          body: function () {
            return "Each fish has a tiny neural network wired by its genes. Fish that eat enough have children with " +
                   "slightly mutated genes. This is the 3-D Ocean, a sea with real depth: they sense up and down and " +
                   "steer in it, and each inherits the depth it floats at. Nobody programs what they do: their " +
                   "behaviour evolves.<br>Take the one-minute tour?";
          } },
        { drawer: false, title: "The tank",
          target: function () { return h.rectOf("#tank"); },
          body: function () {
            return "Glowing specks are food, each family has its own colour, and the dark fish with glowing eyes are " +
                   "predators. Light fades and turns blue with depth. " +
                   (h.touch() ? "Drag to move, pinch to zoom, twist with two fingers to turn (below the horizon too)."
                              : "Drag to move, scroll to zoom, right-drag to turn (below the horizon too). Or fly: " +
                                "<kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd> move where you look, <kbd>Q</kbd> " +
                                "<kbd>E</kbd> go down and up, <kbd>Shift</kbd> faster.");
          } },
        { drawer: false, title: "A fish's brain", fish: true,
          enter: function () {
            if (h.app().inspectSomeFish) h.app().inspectSomeFish();
            var b = h.q("#iexpand");
            if (b && b.getAttribute("aria-expanded") === "false") b.click();
          },
          target: function () { return h.app().inspectorRect ? h.app().inspectorRect() : null; },
          body: function () {
            return tap() + " any fish to see its energy, age, speed, depth, the depth it floats at and its pitch, and " +
                   "its brain, live: senses on one side, actions on the other (steer z is the depth one). " +
                   "<b>Follow</b> (F) keeps the camera on it, with a flashlight that shows its true colours" +
                   (h.touch() ? "." : "; then W and S come closer or back off, A and D circle it, Q and E lower or " +
                                      "raise the camera, and the follow stays on.");
          } },
        { drawer: false, title: "Live numbers",
          target: function () { return h.rectOf("#readout"); },
          body: function () {
            return "Fish, generations, their average depth and the frame rate. " + tap() + " it for the rest: food, " +
                   "births and deaths, family lines, and where in the depth the fish are.";
          } },
        { drawer: false, title: "Two worlds",
          target: function () { return h.rectOf("#worlds"); },
          body: function () {
            return "You are in the <b>3-D Ocean</b>. <b>2-D</b> opens Classic, the original flat world that the " +
                   "native app runs, which you can watch as a tank or flat. Each world runs on its own, so " +
                   "switching starts the other one fresh.";
          } },
        { desktopOnly: true, drawer: false, title: "Keyboard",
          body: function () {
            return "<kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd> fly · <kbd>Q</kbd> <kbd>E</kbd> down and up · " +
                   "<kbd>Space</kbd> pause · <kbd>↑</kbd> <kbd>↓</kbd> speed · <kbd>F</kbd> follow · <kbd>L</kbd> " +
                   "flashlight · <kbd>C</kbd> fit the view · <kbd>R</kbd> a new world (<kbd>Shift</kbd> <kbd>R</kbd> " +
                   "replays this one) · <kbd>?</kbd> the keys beside the dock. Try one now; the tour waits. " +
                   "(<kbd>Esc</kbd> closes things, and this tour.)";
          } },
        { drawer: false, title: "Playback",
          target: function () { return h.rectOf("#dock"); },
          body: function () {
            return "Pause and change speed. <b>Turbo</b> runs the world headless in a background thread, many times " +
                   "faster, and hands it back exactly. The dice draws a new world; the lamp is the flashlight; the " +
                   "corners fit the whole tank back on screen.";
          } },
        { drawer: false, title: "Dashboards",
          target: function () { return h.rectOf("#dashbtn"); },
          body: function () {
            return "Charts of this run as it happens: population, where in the depth the fish live and the depth they " +
                   "float at, family lines, bodies, and which senses their brains use. Recorded on your device only.";
          } },
        { drawer: false, title: "Settings",
          target: function () { return h.rectOf("#menubtn"); },
          body: function () { return "Everything else lives behind this button."; } },
        { drawer: true, tab: "run", scroll: ".run-hero", place: h.drawerSides, title: "Randomize",
          target: function () { return h.rectOf(".run-hero"); },
          body: function () {
            return "The quickest way to explore. It draws a setup from ranges that worked in experiments (predators, " +
                   "currents, the E1 physics), restarts, and lists what it picked.";
          } },
        { drawer: true, tab: "depth", scroll: "#f_kBuoy", place: h.drawerSides, title: "Your own settings",
          target: function () { return h.rectOf("#f_kBuoy"); },
          body: function () {
            return "Change anything by hand: the depth physics here, waves, light, currents and the tuned E1 preset " +
                   "under Water (E1). " +
                   (h.touch() ? "Tap" : "Hover over") + " a setting's name to see what it does. Changes apply on " +
                   "<b>Restart</b>; settings marked <i>live</i> apply at once.";
          } },
        { last: true, drawer: false, title: "You're set",
          body: function () { return "Replay this tour any time with <b>Tour</b> in the settings. Enjoy!"; } }
      ];
    },
    intro: function (h) {
      return { intro: true, drawer: false, title: "Turbo: evolution, fast-forwarded",
        target: function () { return h.rectOf("#turbobtn"); },
        place: ["above", "below", "left", "right"],
        body: function () {
          return "Behaviour evolves over many generations, which is slow to watch. <b>Turbo</b> runs the world " +
                 "headless, in a background thread, many times faster, and keeps going in a background tab. Stop it " +
                 "whenever you like: the world comes back exactly where Turbo got it.";
        } };
    }
  };
  window.EvoApp = {
    ready: false,
    touch: function () { return coarseMQ.matches; },
    compact: function () { return true; },
    has3D: function () { return true; },
    colourByBrain: function () { return colourBrain; },
    colourMode: function () { return colourMode; },
    startTurbo: function () { if (M && turboState === "live") enterTurbo(); },
    setDrawer: setDrawer,
    setSheetTab: setSheetTab,
    inspectSomeFish: function () {
      var s = fresh(lastS);
      if (!M || turboState !== "live" || !s || !s.n) return false;
      if (selectedId !== null) return true;
      var best = scene.pick(s, innerWidth * 0.4, innerHeight * 0.45, 4000);
      if (best < 0) best = 0;
      select(guideFish = s.ids[best]);
      return true;
    },
    releaseFish: function () {
      if (guideFish !== null && selectedId === guideFish) select(null);
      guideFish = null;
    },
    inspectorRect: function () { return selectedId === null || cardEl.hidden ? null : cardEl.getBoundingClientRect(); }
  };

  /* ---------------- start ---------------- */
  buildHist();
  var linkRefused = false;   /* oc_init refused the link's ?cfg= / ?e1=1 */
  splashSay(MOCK ? "Starting the ocean (mock engine)…" : "Starting the ocean…");
  var sceneReady = window.EvoOceanScene ? EvoOceanScene.load() : Promise.reject(new Error("ocean_scene.js missing"));
  Promise.all([loadModule().catch(function (e) {
    var why = String(e && e.message || e);
    if (/could not load/.test(why) && !MOCK) why += "\nThe Ocean module is web/evo_ocean.js + .wasm, built by wasm/build_ocean.sh.";
    throw new Error(why);
  }), sceneReady]).then(function (r) {
    M = r[0];
    var missing = REQUIRED.filter(function (f) { return typeof M["_" + f] !== "function"; });
    if (missing.length) throw new Error("the module lacks exports: " + missing.join(", "));
    if (!M.wasmMemory && !M.HEAPU8) throw new Error("the module exposes neither wasmMemory nor HEAPU8");
    if (!HO) throw new Error("ocean_handoff.js did not load");
    if (!M._malloc) throw new Error("the module does not export _malloc/_free");
    HAS_VZ = has("oc_creature_vz");
    /* the appearance genome: the exports, a layout and laws this page renders (web/ocean_appearance.js) */
    HAS_APP = ["oc_creature_appearance", "oc_appearance_count", "oc_appearance_abi", "oc_appearance_stride",
               "oc_pattern_law", "oc_morphology_law"].every(has) && !!window.EvoAppearance &&
              M._oc_appearance_abi() === EvoAppearance.LAWS.abi && M._oc_appearance_stride() === EvoAppearance.STRIDE &&
              M._oc_pattern_law() === EvoAppearance.LAWS.patternLaw && M._oc_morphology_law() === EvoAppearance.LAWS.morphLaw;
    HAS_EYE = has("oc_has_eye") && !!M._oc_has_eye();
    if (!HAS_EYE) ["eye", "eyeOnly", "eyeCode"].forEach(function (k) {
      var i = el("o_" + k); if (i) i.disabled = true;
      var row = el("f_" + k); if (row) row.querySelectorAll(".stepper button").forEach(function (b) { b.disabled = true; });
    });
    /* the URL's world: ?seed / ?pop / ?depth, ?cfg (JSON; one that is not
     * goes to oc_init as it is, so it says why), ?e1=1 under it. The setup
     * goes to oc_init as it is (importWorld), and into the sheet only once
     * accepted: a value the sheet would round or drop is refused with the
     * engine's reason, not quietly changed. */
    var rawCfg = P.get("cfg") || "", userCfg = {}, badCfg = false;
    if (rawCfg) { try { userCfg = JSON.parse(rawCfg); if (!userCfg || typeof userCfg !== "object" || Array.isArray(userCfg)) badCfg = true; } catch (e) { badCfg = true; } }
    if (E1 && !has("oc_e1_preset")) throw new Error("?e1=1 needs oc_e1_preset: rebuild web/evo_ocean (wasm/build_ocean.sh)");
    var cfg0 = badCfg ? {} : Object.assign({}, E1 ? e1Preset() : {}, userCfg);
    var num = function (key, def, lo, hi) { var v = parseFloat(P.get(key)); return isFinite(v) ? Math.max(lo, Math.min(hi, v)) : def; };
    var refused = importWorld(num("seed", KEYS.seed[0], 0, 4294967295) >>> 0, Math.round(num("pop", KEYS.population[0], 0, 100000)),
                              num("depth", KEYS.depth[0], 10, 100000), cfg0, badCfg ? rawCfg : undefined);
    if (refused) {
      linkRefused = !!(rawCfg || E1);
      throw new Error("This link's settings were refused: " + refused);
    }
    scene = EvoOceanScene.create(canvas, {
      tankDepth: D,
      coarse: coarseMQ.matches,
      reducedMotion: reducedMQ.matches,
      bodyScale: bodyScale, onFollowChange: onFollowChange
    });
    if (!scene) throw new Error("WebGL2 is unavailable, and the 3-D Ocean has only the 3D tank");
    detail0 = scene.quality();
    scene.setWorld(W, H, D);
    scene.setLight(LIGHT);
    if (scene.setAppearance) scene.setAppearance(colourMode === "appearance");
    labelHist();
    wireInput();
    updateInsets();
    updateLight();
    updateModeline();
    if (dash()) dash().attach({
      depth: function () { return D; },
      familyColor: function (id) { return has("oc_family_color") ? M._oc_family_color(id) : 0x3b5957; },
      wiring: wiringNow,
      setDrawer: setDrawer,
      setRecEvery: function (every) {
        if (turboWorker && (turboState === "entering" || turboState === "turbo")) turboWorker.postMessage({ cmd: "rec", every: every });
      }
    });
    window.EvoOcean = {
      module: M, scene: scene,
      steepest: function (sinking) {
        var s = fresh(lastS), best = -1, bv = 0;
        if (!s || !s.vz) return null;
        for (var i = 0; i < s.n; i++) { var v = sinking ? s.vz[i] : Math.abs(s.vz[i]); if (v > bv) { bv = v; best = i; } }
        return best < 0 ? null : { idx: best, id: s.ids[best], vz: s.vz[best], x: s.segs[12 * best], y: s.segs[12 * best + 1],
                                   x1: s.segs[12 * best + 2], y1: s.segs[12 * best + 3], depth: s.depth[best] };
      },
      set: function (name, v) {
        if (!has("oc_set_param")) return false;
        var ok = withCString(String(name), function (p) { return M._oc_set_param(p, +v); });
        if (!ok) console.warn("oc_set_param(" + name + "): " + lastError());
        readLight();
        return !!ok;
      },
      select: function (idx) { var s = fresh(lastS); if (s && idx >= 0 && idx < s.n) select(s.ids[idx]); },
      /* index of the deepest fish right now (-1 with none) */
      deepest: function () {
        var s = fresh(lastS), best = -1;
        if (s) for (var i = 0; i < s.n; i++) if (best < 0 || s.depth[i] > s.depth[best]) best = i;
        return best;
      },
      follow: follow,
      turbo: function () { return turboState; },
      state: function () {
        var s = fresh(lastS);
        if (!s) return null;
        var lo = Infinity, hi = -Infinity;
        for (var i = 0; i < s.n; i++) { lo = Math.min(lo, s.depth[i]); hi = Math.max(hi, s.depth[i]); }
        return { n: s.n, fc: s.fc, pc: s.pc, W: W, H: H, D: D, paused: paused, speed: speed(), steps: lastSteps,
                 hasVz: HAS_VZ, view: scene.view(), e1: E1, cfg: initArgs ? initArgs.cfg : null, seed: initArgs ? initArgs.seed : null,
                 runCfg: Object.assign({}, runCfg), light: LIGHT, flashlight: scene.flashlight(), flashlightLevel: scene.flashlightLevel(),
                 stepMs: stepMs, fps: fps, following: scene.following(), selectedId: selectedId, selIdx: s.selIdx,
                 turbo: turboState, colourBrain: colourBrain, frame: s.stats[1],
                 held: Object.assign({}, heldLive), fixedSize: [fixedW, fixedH],
                 stats: Array.prototype.slice.call(s.stats), depthMin: s.n ? lo : 0, depthMax: s.n ? hi : 0,
                 dashRows: dash() ? dash()._rows().length : 0 };
      }
    };
    requestAnimationFrame(frame);
    window.EvoApp.ready = true;
    window.dispatchEvent(new Event("evo:ready"));
    setTimeout(function () {
      if (!everSelected && selectedId === null && turboState === "live" && !document.getElementById("guide"))
        toast((coarseMQ.matches ? "Tap" : "Click") + " any fish to see its brain; F follows one with a flashlight.");
    }, 9000);
  }).catch(function (e) {
    var why = String(e && e.message || e);
    splashSay("The 3-D Ocean could not start.\n" + why, true);
    if (linkRefused) {   /* the same link without its settings */
      var u = new URL(location.href), a = document.createElement("a");
      u.searchParams.delete("cfg"); u.searchParams.delete("e1");
      a.href = u.href; a.id = "splashplain"; a.className = "btn";
      a.textContent = "Open it with the default settings";
      el("splashstate").parentNode.appendChild(a);
    }
    console.error("ocean: " + why);
  });
})();
