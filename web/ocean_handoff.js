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
 * run, and copy the numbers out, so a growth can't strand them. */
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

  var api = { PAGE: PAGE, buffer: buffer, growTo: growTo, take: take, cString: cString,
              withCString: withCString, f64: f64, f32: f32, row: row };
  root.EvoOceanHandoff = api;
  if (typeof module === "object" && module && module.exports) module.exports = api;   /* node: the tests */
})(typeof self !== "undefined" ? self : typeof window !== "undefined" ? window : globalThis);
