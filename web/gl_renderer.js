"use strict";
/* evo_boids WebGL2 scene renderer.
 *
 * Draws the scene layer — background, current arrows, food, creatures,
 * predators — into an offscreen WebGL2 canvas, which renderer.js then copies
 * onto the visible 2D canvas with one drawImage before drawing the overlays
 * (selection ring, HUD, inspector) in 2D. Why: the 2D path issues one fill per
 * body segment (6 per creature), and at ~2000 creatures that alone costs more
 * than a 60 fps frame (README.md, "Benchmarks"). Here every circle of a kind
 * is one instanced draw call.
 *
 * Same picture as the 2D path, same order: circles are drawn creature-major,
 * segment-minor, so later creatures overlap earlier ones exactly as before;
 * radii, alphas and colours come from renderer.js (passed in `look`), so the
 * two paths cannot drift apart.
 *
 * Fish shapes (look.fishShape, off by default): each creature is one smooth
 * body over the same six spine points instead of six circles. Its half-width
 * at each spine point is that point's circle radius, the head is the head
 * circle, and a tail fin is added whose size scales with the body (look.fin),
 * so body size and elongation show exactly as they do with circles. One
 * instanced draw of a template mesh, bent in the vertex shader; still
 * creature-major. WebGL2 only: the 2D path keeps the circles.
 *
 * EvoGL.create(width, height, bgImage, look) -> renderer or null (no WebGL2).
 * renderer.draw(s) -> the canvas to blit; renderer.resize(w, h). */
(function () {
  var CIRCLE_VS = [
    "#version 300 es",
    "layout(location=0) in vec2 corner;",   /* quad corner, -1..1 */
    "layout(location=1) in vec3 inst;",     /* centre x, y, radius (world px) */
    "layout(location=2) in vec4 color;",    /* rgba, 0..1 */
    "uniform vec2 world;",
    "out vec2 vLocal; out float vR; out vec4 vColor;",
    "void main() {",
    "  float r = inst.z + 1.0;",            /* +1 px for the anti-aliased rim */
    "  vec2 p = inst.xy + corner * r;",
    "  vLocal = corner * r; vR = inst.z; vColor = color;",
    "  gl_Position = vec4(p.x / world.x * 2.0 - 1.0, 1.0 - p.y / world.y * 2.0, 0.0, 1.0);",
    "}"
  ].join("\n");
  var CIRCLE_FS = [
    "#version 300 es",
    "precision mediump float;",
    "in vec2 vLocal; in float vR; in vec4 vColor;",
    "out vec4 outColor;",
    "void main() {",
    "  float a = clamp(vR - length(vLocal) + 0.5, 0.0, 1.0);",
    "  if (a <= 0.0) discard;",
    "  outColor = vec4(vColor.rgb, vColor.a * a);",
    "}"
  ].join("\n");
  /* Fish template vertex: (a, b, part). part 0 = body strip, a = position
   * along the spine (0 head .. 5 tail), b = side (-1 / +1); part 1 = head
   * circle, a < 0 for its centre, b = angle from straight ahead; part 2 = tail
   * fin, a = along (0 root .. 1 tip), b = across (+-0.25 root, +-1 tip). The
   * spine is a Catmull-Rom curve through the six points, clamped at the ends. */
  var FISH_VS = [
    "#version 300 es",
    "layout(location=0) in vec3 tpl;",
    "layout(location=1) in vec4 iA;",       /* spine points 0, 1 (world px) */
    "layout(location=2) in vec4 iB;",       /* spine points 2, 3 */
    "layout(location=3) in vec4 iC;",       /* spine points 4, 5 */
    "layout(location=4) in vec4 iCol;",     /* rgb 0..1, body scale */
    "uniform vec2 world; uniform float radii[6]; uniform float minR;",
    "uniform vec3 fin;",                    /* length, spread (world px at scale 1), alpha */
    "out float vDist; out float vW; out vec4 vColor;",
    "vec2 seg(int i) {",
    "  if (i <= 0) return iA.xy; if (i == 1) return iA.zw; if (i == 2) return iB.xy;",
    "  if (i == 3) return iB.zw; if (i == 4) return iC.xy; return iC.zw;",
    "}",
    "vec2 spine(float s) {",
    "  s = clamp(s, 0.0, 5.0);",
    "  int i = min(int(floor(s)), 4); float t = s - float(i);",
    "  vec2 p0 = seg(max(i - 1, 0)), p1 = seg(i), p2 = seg(i + 1), p3 = seg(min(i + 2, 5));",
    "  float t2 = t * t, t3 = t2 * t;",
    "  return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2",
    "              + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);",
    "}",
    "float halfWidth(float s, float bs) {",
    "  s = clamp(s, 0.0, 5.0);",
    "  int i = min(int(floor(s)), 4); float t = s - float(i);",
    "  return mix(max(minR, radii[i] * bs), max(minR, radii[i + 1] * bs), t);",
    "}",
    "vec2 unit(vec2 v) { float l = length(v); return l > 1e-4 ? v / l : vec2(1.0, 0.0); }",
    "void main() {",
    "  float bs = iCol.w, a = tpl.x, b = tpl.y;",
    "  int part = int(tpl.z + 0.5);",
    "  vec2 p; float dist = 0.0, w = 1e4, alpha = 1.0;",
    "  if (part == 0) {",
    "    vec2 f = unit(spine(a - 0.1) - spine(a + 0.1));",   /* towards the head */
    "    w = halfWidth(a, bs);",
    "    p = spine(a) + vec2(-f.y, f.x) * b * (w + 1.0);",   /* +1 px for the anti-aliased rim */
    "    dist = b * (w + 1.0);",
    "  } else if (part == 1) {",
    "    vec2 f = unit(seg(0) - seg(1));",
    "    w = halfWidth(0.0, bs);",
    "    vec2 d = f * cos(b) + vec2(-f.y, f.x) * sin(b);",
    "    p = a < 0.0 ? seg(0) : seg(0) + d * (w + 1.0);",
    "    dist = a < 0.0 ? 0.0 : w + 1.0;",
    "  } else {",
    "    vec2 k = unit(seg(5) - seg(4)), n = vec2(-k.y, k.x);",  /* backwards, and across */
    "    float across = abs(b) < 0.5 ? sign(b) * halfWidth(5.0, bs) : b * fin.y * bs;",
    "    p = seg(5) + k * a * fin.x * bs + n * across;",
    "    alpha = fin.z;",
    "  }",
    "  vDist = dist; vW = w; vColor = vec4(iCol.rgb, alpha);",
    "  gl_Position = vec4(p.x / world.x * 2.0 - 1.0, 1.0 - p.y / world.y * 2.0, 0.0, 1.0);",
    "}"
  ].join("\n");
  var FISH_FS = [
    "#version 300 es",
    "precision mediump float;",
    "in float vDist; in float vW; in vec4 vColor;",
    "out vec4 outColor;",
    "void main() {",
    "  float a = clamp(vW - abs(vDist) + 0.5, 0.0, 1.0);",
    "  if (a <= 0.0) discard;",
    "  outColor = vec4(vColor.rgb, vColor.a * a);",
    "}"
  ].join("\n");
  /* The template: 16 body stations (3 per segment), the head circle as a
   * 16-sided fan (anti-aliased like the circles), a forked fin. Built once. */
  function fishTemplate() {
    var v = [], idx = [], k, j;
    for (k = 0; k <= 15; k++) { v.push(k / 3, -1, 0, k / 3, 1, 0); }
    for (k = 0; k < 15; k++) { var a = 2 * k; idx.push(a, a + 1, a + 2, a + 1, a + 3, a + 2); }
    var hc = v.length / 3;
    v.push(-1, 0, 1);
    for (j = 0; j <= 16; j++) v.push(0, -Math.PI + 2 * Math.PI * j / 16, 1);
    for (j = 0; j < 16; j++) idx.push(hc, hc + 1 + j, hc + 2 + j);
    var f0 = v.length / 3;
    v.push(0, -0.25, 2, 0, 0.25, 2, 1, -1, 2, 1, 1, 2, 0.55, 0, 2);   /* rootL, rootR, tipL, tipR, notch */
    idx.push(f0, f0 + 1, f0 + 4, f0, f0 + 4, f0 + 2, f0 + 1, f0 + 3, f0 + 4);
    return { verts: new Float32Array(v), index: new Uint16Array(idx) };
  }

  var LINE_VS = [
    "#version 300 es",
    "layout(location=0) in vec2 pos;",
    "layout(location=1) in vec4 color;",
    "uniform vec2 world;",
    "out vec4 vColor;",
    "void main() {",
    "  vColor = color;",
    "  gl_Position = vec4(pos.x / world.x * 2.0 - 1.0, 1.0 - pos.y / world.y * 2.0, 0.0, 1.0);",
    "}"
  ].join("\n");
  var LINE_FS = [
    "#version 300 es",
    "precision mediump float;",
    "in vec4 vColor; out vec4 outColor;",
    "void main() { outColor = vColor; }"
  ].join("\n");
  var BG_VS = [
    "#version 300 es",
    "layout(location=0) in vec2 pos;",      /* clip space */
    "layout(location=1) in vec2 uv;",
    "out vec2 vUv;",
    "void main() { vUv = uv; gl_Position = vec4(pos, 0.0, 1.0); }"
  ].join("\n");
  var BG_FS = [
    "#version 300 es",
    "precision mediump float;",
    "in vec2 vUv; uniform sampler2D tex; out vec4 outColor;",
    "void main() { outColor = texture(tex, vUv); }"
  ].join("\n");

  function compile(gl, vs, fs) {
    function shader(type, src) {
      var sh = gl.createShader(type);
      gl.shaderSource(sh, src);
      gl.compileShader(sh);
      if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(sh));
      return sh;
    }
    var p = gl.createProgram();
    gl.attachShader(p, shader(gl.VERTEX_SHADER, vs));
    gl.attachShader(p, shader(gl.FRAGMENT_SHADER, fs));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    return p;
  }

  function create(width, height, bgImage, look) {
    var canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    var gl = canvas.getContext("webgl2", { alpha: false, antialias: false, depth: false,
                                           stencil: false, premultipliedAlpha: false,
                                           preserveDrawingBuffer: false });
    if (!gl) return null;
    var circleProg, lineProg, bgProg, fishProg = null;
    try {
      circleProg = compile(gl, CIRCLE_VS, CIRCLE_FS);
      lineProg = compile(gl, LINE_VS, LINE_FS);
      bgProg = compile(gl, BG_VS, BG_FS);
    } catch (e) {
      console.warn("evo_boids: WebGL2 shaders failed, using the 2D renderer", e);
      return null;
    }
    /* on its own: a failure here turns off fish shapes, not WebGL2 */
    try { fishProg = compile(gl, FISH_VS, FISH_FS); }
    catch (e) { console.warn("evo_boids: the fish-shape shader failed; circles only", e); }
    var uWorldCircle = gl.getUniformLocation(circleProg, "world");
    var uWorldLine = gl.getUniformLocation(lineProg, "world");
    var uFish = fishProg && { world: gl.getUniformLocation(fishProg, "world"), radii: gl.getUniformLocation(fishProg, "radii"),
                              minR: gl.getUniformLocation(fishProg, "minR"), fin: gl.getUniformLocation(fishProg, "fin") };

    /* --- fish shapes: a template mesh + a per-instance buffer --- */
    var FSTRIDE = 16;  /* six spine points, red, green, blue, body scale */
    var tpl = fishTemplate();
    var folds = makeFoldTracker();
    var fishVao = gl.createVertexArray();
    gl.bindVertexArray(fishVao);
    var tplBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, tplBuf);
    gl.bufferData(gl.ARRAY_BUFFER, tpl.verts, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    var idxBuf = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuf);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, tpl.index, gl.STATIC_DRAW);
    var fishBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, fishBuf);
    for (var at = 1; at <= 4; at++) {
      gl.enableVertexAttribArray(at);
      gl.vertexAttribPointer(at, 4, gl.FLOAT, false, FSTRIDE * 4, (at - 1) * 16);
      gl.vertexAttribDivisor(at, 1);
    }
    gl.bindVertexArray(null);
    var fishInst = new Float32Array(FSTRIDE * 2048);
    var fishCap = 0;

    /* --- circles: one shared unit quad + a per-instance buffer --- */
    var STRIDE = 7;  /* x, y, r, red, green, blue, alpha */
    var circleVao = gl.createVertexArray();
    gl.bindVertexArray(circleVao);
    var quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    var instBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, instBuf);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, STRIDE * 4, 0);
    gl.vertexAttribDivisor(1, 1);
    gl.enableVertexAttribArray(2);
    gl.vertexAttribPointer(2, 4, gl.FLOAT, false, STRIDE * 4, 12);
    gl.vertexAttribDivisor(2, 1);
    gl.bindVertexArray(null);
    var inst = new Float32Array(STRIDE * 4096);
    var instCap = 0;   /* bytes allocated on the GPU */

    /* --- current arrows: plain coloured lines --- */
    var lineVao = gl.createVertexArray();
    gl.bindVertexArray(lineVao);
    var lineBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, lineBuf);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 24, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 24, 8);
    gl.bindVertexArray(null);
    var lines = new Float32Array(6 * 2 * 1024);

    /* --- background: a cover-scaled textured quad --- */
    var bgVao = gl.createVertexArray();
    gl.bindVertexArray(bgVao);
    var bgBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, bgBuf);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 16, 8);
    gl.bindVertexArray(null);
    var bgTex = null, bgW = 0, bgH = 0;

    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    function grow(arr, need) {
      if (arr.length >= need) return arr;
      var n = arr.length;
      while (n < need) n *= 2;
      return new Float32Array(n);
    }

    function uploadBackground() {
      if (bgTex || !bgImage || !bgImage.complete || !(bgImage.naturalWidth > 0)) return;
      bgTex = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, bgTex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bgImage);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      bgW = bgImage.naturalWidth;
      bgH = bgImage.naturalHeight;
    }

    function drawBackground(W, H) {
      uploadBackground();
      if (!bgTex) return;
      /* cover: same placement as the 2D path's drawImage */
      var s = Math.max(W / bgW, H / bgH), bw = bgW * s, bh = bgH * s;
      var x0 = (W - bw) / 2, y0 = (H - bh) / 2, x1 = x0 + bw, y1 = y0 + bh;
      function cx(x) { return x / W * 2 - 1; }
      function cy(y) { return 1 - y / H * 2; }
      gl.useProgram(bgProg);
      gl.bindVertexArray(bgVao);
      gl.bindBuffer(gl.ARRAY_BUFFER, bgBuf);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
        cx(x0), cy(y0), 0, 0,  cx(x1), cy(y0), 1, 0,
        cx(x0), cy(y1), 0, 1,  cx(x1), cy(y1), 1, 1]), gl.DYNAMIC_DRAW);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, bgTex);
      gl.disable(gl.BLEND);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      gl.enable(gl.BLEND);
    }

    function drawCurrents(s, W, H) {
      if (!s.curActive) return;
      var need = s.curCols * s.curRows * 12, k = 0;
      lines = grow(lines, need);
      var t = look.curTail, h = look.curHead;
      for (var j = 0; j < s.curRows; j++) {
        for (var i = 0; i < s.curCols; i++) {
          var q = (j * s.curCols + i) * 2;
          var vx = s.curGrid[q], vy = s.curGrid[q + 1];
          if (vx * vx + vy * vy < 1) continue;
          var x0 = 32 + 64 * i, y0 = 32 + 64 * j;
          lines[k++] = x0; lines[k++] = y0; lines[k++] = t[0]; lines[k++] = t[1]; lines[k++] = t[2]; lines[k++] = t[3];
          lines[k++] = x0 + vx * 0.45; lines[k++] = y0 + vy * 0.45;
          lines[k++] = h[0]; lines[k++] = h[1]; lines[k++] = h[2]; lines[k++] = h[3];
        }
      }
      if (!k) return;
      gl.useProgram(lineProg);
      gl.uniform2f(uWorldLine, W, H);
      gl.bindVertexArray(lineVao);
      gl.bindBuffer(gl.ARRAY_BUFFER, lineBuf);
      gl.bufferData(gl.ARRAY_BUFFER, lines.subarray(0, k), gl.DYNAMIC_DRAW);
      gl.drawArrays(gl.LINES, 0, k / 6);
    }

    /* Fill the instance array: food, then creatures (creature-major), then
     * predators — the 2D path's order. Returns the instance count. */
    function fillCircles(s) {
      var count = s.fc + s.n * 6 + s.pc;
      inst = grow(inst, count * STRIDE);
      var k = 0, i, f = look.food;
      for (i = 0; i < s.fc; i++) {
        inst[k++] = s.food[2 * i]; inst[k++] = s.food[2 * i + 1]; inst[k++] = 2.5;
        inst[k++] = f[0]; inst[k++] = f[1]; inst[k++] = f[2]; inst[k++] = 1;
      }
      var radii = look.radii, alpha = look.segAlpha, minR = look.minRadius;
      for (var c = 0; c < s.n; c++) {
        var r = s.colors[3 * c] / 255, g = s.colors[3 * c + 1] / 255, b = s.colors[3 * c + 2] / 255;
        var bs = look.bodyScale(s.body ? s.body[2 * c] : 1);
        for (var seg = 0; seg < 6; seg++) {
          inst[k++] = s.segs[12 * c + 2 * seg];
          inst[k++] = s.segs[12 * c + 2 * seg + 1];
          inst[k++] = Math.max(minR, radii[seg] * bs);
          inst[k++] = r; inst[k++] = g; inst[k++] = b; inst[k++] = alpha[seg];
        }
      }
      for (i = 0; i < s.pc; i++) {
        var pcol = s.preds[3 * i + 2] > 0 ? look.predDigesting : look.predHunting;
        inst[k++] = s.preds[3 * i]; inst[k++] = s.preds[3 * i + 1]; inst[k++] = 6.5;
        inst[k++] = pcol[0]; inst[k++] = pcol[1]; inst[k++] = pcol[2]; inst[k++] = 1;
      }
      return count;
    }

    function drawCircles(s, W, H) {
      var count = fillCircles(s);
      if (!count) return;
      uploadCircles(count);
      gl.useProgram(circleProg);
      gl.uniform2f(uWorldCircle, W, H);
      gl.bindVertexArray(circleVao);
      gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, count);
    }
    function uploadCircles(count) {
      gl.bindBuffer(gl.ARRAY_BUFFER, instBuf);
      var bytes = count * STRIDE * 4;
      if (bytes > instCap) {
        instCap = Math.max(bytes, instCap * 2);
        gl.bufferData(gl.ARRAY_BUFFER, instCap, gl.DYNAMIC_DRAW);
      }
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, inst, 0, count * STRIDE);
    }
    /* circles [first, first + count) of the uploaded buffer */
    function circleRange(first, count, W, H) {
      if (!count) return;
      gl.useProgram(circleProg);
      gl.uniform2f(uWorldCircle, W, H);
      gl.bindVertexArray(circleVao);
      gl.bindBuffer(gl.ARRAY_BUFFER, instBuf);
      gl.vertexAttribPointer(1, 3, gl.FLOAT, false, STRIDE * 4, first * STRIDE * 4);
      gl.vertexAttribPointer(2, 4, gl.FLOAT, false, STRIDE * 4, first * STRIDE * 4 + 12);
      gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, count);
      gl.vertexAttribPointer(1, 3, gl.FLOAT, false, STRIDE * 4, 0);
      gl.vertexAttribPointer(2, 4, gl.FLOAT, false, STRIDE * 4, 12);
    }

    /* Fish shapes, from the six spine points as exported. (Neighbouring points
     * are never more than one segment apart: the core keeps the trail on the
     * head's side of the world's edge.) A fish whose trail is folded ahead of
     * its head goes to the circles instead (makeFoldTracker); returns the
     * shape count, and the folded fish's indices in foldIdx[0 .. foldN). */
    var foldIdx = new Int32Array(1024), foldN = 0;
    function fillFish(s, W, H) {
      var k = 0;
      fishInst = grow(fishInst, s.n * FSTRIDE);
      if (foldIdx.length < s.n) foldIdx = new Int32Array(Math.max(s.n, 2 * foldIdx.length));
      foldN = 0;
      for (var c = 0; c < s.n; c++) {
        var b = 12 * c;
        if (s.ids && folds.folded(s.ids[c], s.segs, b, W, H)) { foldIdx[foldN++] = c; continue; }
        for (var i = 0; i < 12; i++) fishInst[k++] = s.segs[b + i];
        fishInst[k++] = s.colors[3 * c] / 255; fishInst[k++] = s.colors[3 * c + 1] / 255;
        fishInst[k++] = s.colors[3 * c + 2] / 255;
        fishInst[k++] = look.bodyScale(s.body ? s.body[2 * c] : 1);
      }
      return k / FSTRIDE;
    }
    function drawFish(count, W, H) {
      if (!count) return;
      gl.useProgram(fishProg);
      gl.uniform2f(uFish.world, W, H);
      gl.uniform1fv(uFish.radii, look.radii);
      gl.uniform1f(uFish.minR, look.minRadius);
      gl.uniform3f(uFish.fin, look.fin.len, look.fin.spread, look.fin.alpha);
      gl.bindVertexArray(fishVao);
      gl.bindBuffer(gl.ARRAY_BUFFER, fishBuf);
      var bytes = count * FSTRIDE * 4;
      if (bytes > fishCap) {
        fishCap = Math.max(bytes, fishCap * 2);
        gl.bufferData(gl.ARRAY_BUFFER, fishCap, gl.DYNAMIC_DRAW);
      }
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, fishInst, 0, count * FSTRIDE);
      gl.drawElementsInstanced(gl.TRIANGLES, tpl.index.length, gl.UNSIGNED_SHORT, 0, count);
    }
    /* food circles, fish shapes, the folded fish's circles, predator circles */
    function drawShapes(s, W, H) {
      var shapes = fillFish(s, W, H);
      var nf = foldN * 6;
      inst = grow(inst, (s.fc + nf + s.pc) * STRIDE);
      var k = 0, i, f = look.food;
      for (i = 0; i < s.fc; i++) {
        inst[k++] = s.food[2 * i]; inst[k++] = s.food[2 * i + 1]; inst[k++] = 2.5;
        inst[k++] = f[0]; inst[k++] = f[1]; inst[k++] = f[2]; inst[k++] = 1;
      }
      var radii = look.radii, alpha = look.segAlpha, minR = look.minRadius;
      for (var j = 0; j < foldN; j++) {   /* exactly what drawCircles draws for them */
        var c = foldIdx[j], bs = look.bodyScale(s.body ? s.body[2 * c] : 1);
        var r = s.colors[3 * c] / 255, g = s.colors[3 * c + 1] / 255, b = s.colors[3 * c + 2] / 255;
        for (var seg = 0; seg < 6; seg++) {
          inst[k++] = s.segs[12 * c + 2 * seg]; inst[k++] = s.segs[12 * c + 2 * seg + 1];
          inst[k++] = Math.max(minR, radii[seg] * bs);
          inst[k++] = r; inst[k++] = g; inst[k++] = b; inst[k++] = alpha[seg];
        }
      }
      for (i = 0; i < s.pc; i++) {
        var pcol = s.preds[3 * i + 2] > 0 ? look.predDigesting : look.predHunting;
        inst[k++] = s.preds[3 * i]; inst[k++] = s.preds[3 * i + 1]; inst[k++] = 6.5;
        inst[k++] = pcol[0]; inst[k++] = pcol[1]; inst[k++] = pcol[2]; inst[k++] = 1;
      }
      if (k) uploadCircles(k / STRIDE);
      circleRange(0, s.fc, W, H);
      drawFish(shapes, W, H);
      circleRange(s.fc, nf, W, H);
      circleRange(s.fc + nf, s.pc, W, H);
    }

    return {
      canvas: canvas,
      fishShapes: !!fishProg,   /* the fish-shape shader compiled */
      resize: function (w, h) {
        if (canvas.width !== w) canvas.width = w;
        if (canvas.height !== h) canvas.height = h;
      },
      draw: function (s, W, H) {
        if (gl.isContextLost()) return null;
        gl.viewport(0, 0, W, H);
        var c = look.clear;
        gl.clearColor(c[0], c[1], c[2], 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        drawBackground(W, H);
        drawCurrents(s, W, H);
        if (look.fishShape && fishProg) drawShapes(s, W, H);
        else drawCircles(s, W, H);
        gl.bindVertexArray(null);
        return canvas;
      }
    };
  }

  /* Which fish have their trail folded ahead of the head. The core wraps a
   * creature's position into the world and then pulls its trail with the
   * plain, unwrapped distance (World::step, Creature::updateBody), so right
   * after crossing an edge the trail lies ahead of the head, on the new side,
   * until the head swims through it. Circles show that as overlapping dots; a
   * fish shape would swim backwards. So after a crossing, until the fish has
   * travelled FOLD_RANGE px, while any segment of its trail points along its
   * motion, folded() says so and the fish is drawn as its circles.
   * Per id, in a small open-addressed table; a collision only costs that fish
   * its history. Motion and distance only change when the fish moves, so a
   * pause keeps the state. */
  /* Unfolding takes about twice the trail's length of travel (the head swims
   * through the folded points, then pulls them behind it): the longest trail
   * is 5 x 4 x 2 x 2 = 80 px, so 260 px, with a margin. */
  var FOLD_RANGE = 260;
  function makeFoldTracker() {
    var CAP = 1 << 15;
    var tid = new Float64Array(CAP).fill(-1), hx = new Float32Array(CAP), hy = new Float32Array(CAP);
    var mx = new Float32Array(CAP), my = new Float32Array(CAP), since = new Float32Array(CAP);
    return {
      folded: function (id, segs, o, W, H) {
        var slot = id % CAP, x = segs[o], y = segs[o + 1];
        if (tid[slot] !== id) {
          tid[slot] = id; hx[slot] = x; hy[slot] = y; mx[slot] = 0; my[slot] = 0; since[slot] = 1e9;
          return false;
        }
        var dx = x - hx[slot], dy = y - hy[slot];
        hx[slot] = x; hy[slot] = y;
        var crossed = false;
        if (dx > W / 2) { dx -= W; crossed = true; } else if (dx < -W / 2) { dx += W; crossed = true; }
        if (dy > H / 2) { dy -= H; crossed = true; } else if (dy < -H / 2) { dy += H; crossed = true; }
        var d = Math.sqrt(dx * dx + dy * dy);
        if (crossed) since[slot] = 0;
        if (d > 0.05 && d < 400) {   /* up to 50 steps a frame; a load's jump is ignored */
          mx[slot] = dx / d; my[slot] = dy / d; since[slot] += d; }
        if (since[slot] > FOLD_RANGE) return false;
        for (var k = 0; k < 5; k++) {
          var sx = segs[o + 2 * k + 2] - segs[o + 2 * k], sy = segs[o + 2 * k + 3] - segs[o + 2 * k + 1];
          var sl = Math.sqrt(sx * sx + sy * sy);
          if (sl > 1e-3 && sx * mx[slot] + sy * my[slot] > 0.3 * sl) return true;
        }
        return false;
      }
    };
  }

  window.EvoGL = { create: create, _makeFoldTracker: makeFoldTracker };   /* the second for tests */
})();
