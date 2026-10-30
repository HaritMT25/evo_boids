"use strict";
/* evo_boids 3-D Ocean: the module-memory helpers the page (web/ocean.js) and
 * the Turbo worker (web/ocean_worker.js) share, so the hand-off has one
 * implementation; tests/ocean_turbo_handoff.js runs them in node against two
 * real module instances.
 *
 * The Turbo hand-off is the module's whole linear memory (web/ocean_worker.js
 * says why a byte copy carries the run on exactly). take() puts an image into
 * a module: it grows the memory to the image's size first, with
 * WebAssembly.Memory.grow (the module exports wasmMemory) by exactly the
 * pages missing, so it never depends on how much free space the receiver's
 * allocator happens to have (growing "through malloc" could be satisfied from
 * free space, or grow too little, and the hand-off then failed), copies the
 * image in and zeroes whatever lies past it, as fresh pages are.
 *
 * A grow from outside leaves the module's own cached views (its HEAPU8 and
 * co., which UTF8ToString / stringToUTF8 read) on the old, detached buffer
 * until its next growth of its own (emscripten_resize_heap renews them). So
 * nothing here, in the page or in the worker reads them: every view is cut
 * from the live buffer (buffer(M)) at the moment it is used, and cString /
 * withCString do their own UTF-8. (The module's other users of those views —
 * the C++ throw path, fatal in this build, and random_get, which only the
 * rng's construction at start-up calls — never run after a hand-off.)
 *
 * The page's controls call exports that allocate (oc_summary, oc_families,
 * oc_inspect, oc_wiring, oc_sensor_names, ...), and any allocation can grow the
 * memory, which detaches every view cut before it. f64 / f32 take the
 * buffer inside, after their arguments (the export's call among them) have
 * run, and copy the numbers out, so a growth can't strand them.
 *
 * Also here, so the tests run the page's own logic: ledger(), the page's live
 * edits around a Turbo session and what it applies when the world comes back
 * (or doesn't); parseFirst() / cfgJson(), the engine's rule for a repeated
 * JSON key on the page's way in (?cfg=, a setup file). */
(function (root) {
  var PAGE = 65536;
  var enc = typeof TextEncoder !== "undefined" ? new TextEncoder() : null;
  var dec = typeof TextDecoder !== "undefined" ? new TextDecoder("utf-8") : null;

  /* the module's memory as it is now (never cache it across a call) */
  function buffer(M) { return M.wasmMemory ? M.wasmMemory.buffer : M.HEAPU8.buffer; }

  /* Grow M's memory to at least `bytes`: by exactly the pages missing, with
   * wasmMemory.grow; without an exported memory, through the allocator (a
   * block of `bytes` ends past `bytes`, so it must grow that far). Returns
   * whether the memory is now that big. */
  function growTo(M, bytes) {
    var have = buffer(M).byteLength;
    if (have >= bytes) return true;
    if (M.wasmMemory && typeof M.wasmMemory.grow === "function") {
      try { M.wasmMemory.grow(Math.ceil((bytes - have) / PAGE)); } catch (e) { /* out of address space */ }
    } else {
      var p = M._malloc(bytes);
      if (p) M._free(p);
    }
    return buffer(M).byteLength >= bytes;
  }

  /* Put a whole memory image (an ArrayBuffer, page-sized) into M. Throws, with
   * M's memory as it was (grown, perhaps, never overwritten), when it can't
   * grow far enough; the caller still holds the image. */
  function take(M, src, who) {
    var need = src.byteLength, have = buffer(M).byteLength;
    if (!need || need % PAGE) throw new Error("not a memory image: " + need + " bytes");
    if (!growTo(M, need))
      throw new Error("could not grow " + (who || "the") + " memory to " + need + " bytes (it has " + buffer(M).byteLength + ")");
    var dst = new Uint8Array(buffer(M));
    dst.set(new Uint8Array(src));
    /* past the image: zero, as fresh pages are (pages this grew already are) */
    if (have > need) dst.fill(0, need, have);
  }

  /* a C string at ptr, read from the live buffer */
  function cString(M, ptr) {
    if (!ptr) return "";
    var h = new Uint8Array(buffer(M)), end = ptr;
    while (end < h.length && h[end]) end++;
    if (dec) return dec.decode(h.subarray(ptr, end));
    var s = "";
    for (var i = ptr; i < end; i++) s += String.fromCharCode(h[i]);
    return s;
  }
  /* fn(ptr) with str as a temporary C string (0 for an empty one) */
  function withCString(M, str, fn) {
    if (str === null || str === undefined || str === "") return fn(0);
    str = String(str);
    var bytes;
    if (enc) bytes = enc.encode(str);
    else { bytes = new Uint8Array(str.length); for (var i = 0; i < str.length; i++) bytes[i] = str.charCodeAt(i) & 255; }
    var ptr = M._malloc(bytes.length + 1);
    if (!ptr) throw new Error("out of module memory");
    var h = new Uint8Array(buffer(M));   /* after the malloc: it may have grown the memory */
    h.set(bytes, ptr);
    h[ptr + bytes.length] = 0;
    try { return fn(ptr); } finally { M._free(ptr); }
  }

  /* len doubles / floats at ptr, copied out. Call it with the export inline,
   * f64(M, M._oc_summary(), M._oc_summary_len()): the buffer is taken here,
   * after the export ran. */
  function f64(M, ptr, len) { return Array.prototype.slice.call(new Float64Array(buffer(M), ptr, len)); }
  function f32(M, ptr, len) { return Array.prototype.slice.call(new Float32Array(buffer(M), ptr, len)); }

  /* One dashboard sample of a synced world (web/ocean_dash.js row()): the
   * stats first (copied before anything allocates), then the summary, then
   * the family lines. statsLen: oc_stats_len(). */
  function row(M, famK, statsLen) {
    var st = f64(M, M._oc_stats(), statsLen);
    var su = f64(M, M._oc_summary(), M._oc_summary_len());
    var k = M._oc_families(famK);
    return { st: st, su: su, fam: f64(M, M._oc_families_data(), 2 * k) };
  }

  /* ---- the page's live edits around a Turbo session (web/ocean.js) ----
   * Every edit gets a sequence number, and is held (neither side owns the
   * world: Turbo is starting or stopping), in flight (sent to the worker, not
   * answered yet) or acknowledged (the worker applied it). back(lost), when
   * the page owns the world again, is what the page must apply, in the order
   * the edits were made: the unanswered and held ones; and, when the
   * worker's world is lost (the page goes on from its own copy, from where
   * Turbo took it), the acknowledged ones first, so that edits which depend
   * on each other (minSpeed 200 only after maxSpeed 300) apply in the order
   * the worker applied them. A newer edit of a key always lands after an
   * older one, never under it. Entries: {key, num, seq, from: "acked" |
   * "sent" | "held"}. */
  function ledger() {
    var seq = 0, held = {}, inflight = [], acked = [];
    function byseq(a, b) { return a.seq - b.seq; }
    function heldList() { return Object.keys(held).map(function (k) { return held[k]; }).sort(byseq); }
    return {
      /* a new Turbo session: nothing in flight, nothing acknowledged yet */
      start: function () { inflight = []; acked = []; },
      /* an edit made while neither side owns the world (the newest per key) */
      hold: function (key, num) { held[key] = { key: key, num: num, seq: ++seq, from: "held" }; },
      /* an edit sent to the worker; seq: a held edit's own, when it is flushed */
      sent: function (key, num, s) { inflight.push({ key: key, num: num, seq: s === undefined ? ++seq : s, from: "sent" }); },
      /* the worker's answer to the oldest in-flight edit of key (it answers in order) */
      answered: function (key, ok) {
        for (var i = 0; i < inflight.length; i++) if (inflight[i].key === key) {
          var e = inflight.splice(i, 1)[0];
          if (ok) acked.push({ key: e.key, num: e.num, seq: e.seq, from: "acked" });
          return e;
        }
        return null;
      },
      /* a newer edit of key is still on its way */
      pending: function (key) { return inflight.some(function (e) { return e.key === key; }); },
      /* the held edits, oldest first, now that one side owns the world */
      takeHeld: function () { var l = heldList(); held = {}; return l; },
      /* the page owns the world again: what it applies, oldest first */
      back: function (lost) {
        var l = (lost ? acked : []).concat(inflight, heldList()).sort(byseq);
        held = {}; inflight = []; acked = [];
        return l;
      },
      held: function () { var o = {}; Object.keys(held).forEach(function (k) { o[k] = held[k].num; }); return o; },
      inflight: function () { return inflight.map(function (e) { return [e.key, e.num]; }); }
    };
  }

  /* ---- JSON from outside the sheet (?cfg=, a setup file) ----
   * The engine reads a repeated key's FIRST occurrence (src/core/json_min.hpp
   * find) and checks every occurrence (ocean_config.hpp readOceanJson);
   * JSON.parse keeps the LAST, so {"toroidal":false,"toroidal":true} ran one
   * way on the page and the other in oc_init, and {"toroidal":1,"toroidal":
   * true} slipped past both checks. parseFirst is JSON.parse (the same syntax
   * check and error, the same values) with the engine's rule: a repeated
   * key's first occurrence is the value, and every later one is listed in
   * repeats [{path, key, value}] (path: the keys / indices down to its
   * object), so the caller can check it and hand it on. cfgJson writes a cfg
   * object for oc_init with those later occurrences after it, so oc_init sees
   * (and checks) every occurrence and takes the same first one. */
  function parseFirst(text) {
    JSON.parse(text);   /* the syntax check, and its message */
    var i = 0, repeats = [];
    function ws() { while (i < text.length && " \t\n\r".indexOf(text.charAt(i)) >= 0) i++; }
    function str() {
      var s = i++;
      while (text.charAt(i) !== '"') i += text.charAt(i) === "\\" ? 2 : 1;
      i++;
      return JSON.parse(text.slice(s, i));
    }
    function val(path) {
      ws();
      var c = text.charAt(i), k, v;
      if (c === "{") {
        var o = {}, seen = Object.create(null);
        i++; ws();
        if (text.charAt(i) === "}") { i++; return o; }
        for (;;) {
          ws(); k = str(); ws(); i++;   /* the ':' */
          v = val(path.concat([k]));
          if (seen[k]) repeats.push({ path: path, key: k, value: v });
          else { seen[k] = true; Object.defineProperty(o, k, { value: v, enumerable: true, writable: true, configurable: true }); }
          ws();
          if (text.charAt(i++) === ",") continue;
          return o;
        }
      }
      if (c === "[") {
        var a = [];
        i++; ws();
        if (text.charAt(i) === "]") { i++; return a; }
        for (;;) {
          a.push(val(path.concat([a.length])));
          ws();
          if (text.charAt(i++) === ",") continue;
          return a;
        }
      }
      if (c === '"') return str();
      var s = i;
      while (i < text.length && ",}] \t\n\r".indexOf(text.charAt(i)) < 0) i++;
      return JSON.parse(text.slice(s, i));
    }
    return { value: val([]), repeats: repeats };
  }
  /* a cfg object as oc_init's JSON ("" for nothing), the later occurrences of
   * its repeated keys ([{key, value}], parseFirst's at its level) after it */
  function cfgJson(cfg, repeats) {
    var keys = Object.keys(cfg || {}), more = repeats || [];
    if (!keys.length && !more.length) return "";
    var s = JSON.stringify(cfg || {});
    more.forEach(function (r) { s = s.slice(0, -1) + (s.length > 2 ? "," : "") + JSON.stringify(r.key) + ":" + JSON.stringify(r.value) + "}"; });
    return s;
  }

  var api = { PAGE: PAGE, buffer: buffer, growTo: growTo, take: take, cString: cString,
              withCString: withCString, f64: f64, f32: f32, row: row,
              ledger: ledger, parseFirst: parseFirst, cfgJson: cfgJson };
  root.EvoOceanHandoff = api;
  if (typeof module === "object" && module && module.exports) module.exports = api;   /* node: the tests */
})(typeof self !== "undefined" ? self : typeof window !== "undefined" ? window : globalThis);
