"use strict";
/* evo_boids 3-D Ocean: the 3D tank, deep (three.js).
 *
 * A copy of web/scene3d.js (the Classic tank) for the Ocean preview page
 * (web/ocean.html), registered as window.EvoOceanScene; to be folded back into
 * scene3d.js at E6 (PLAN_ocean_preview.md §5). What differs from scene3d.js:
 *   - the world has a real depth: s.depth (per fish), s.foodDepth, s.predDepth
 *     place everything at three Y = -depth (0 = surface .. D); without them
 *     the old hash height / fixed heights are used, as in scene3d.js
 *   - opts.tankDepth = D: the floor sits at -(D + 14), the glass box is a full
 *     box (top + bottom rectangles + verticals), marine snow falls the whole D
 *   - the camera looks more from the side (POLAR 1.0) and frames all eight
 *     corners of the box around y = -D/2; following a fish follows its depth.
 *     It is free: it orbits below the horizontal (to nearly straight up),
 *     moves in depth (moveView, and screen-plane panning from the side), and
 *     clampView keeps it out of the sand and its centre within the water
 *   - light fades and shifts blue with depth (fish, shadows, food); the floor
 *     is sand (grain, ripples) under the caustics; seen from below, the
 *     surface shows Snell's window and a shimmer (SURF_FS). setLight({on, L0,
 *     k}) (E1: the engine's light field) makes the fish / food fall-off the
 *     engine's L(d) = L0·exp(−k·d); off, the preview's linear depth / D
 *   - each body pitches by s.vz (+ = sinking, head down), pivoting at the head
 *   - the flashlight (setFlashlight, viewer only): a cone of clear light from
 *     the camera onto the followed fish or the point it looks at; inside it
 *     the fish, food and sand show their true colours (BEAM_FN)
 *
 * Every fish is a faceted low-poly body bent along the six spine points the
 * core exports; the floor below catches its shadow and slow caustic light.
 * Food is glowing plankton, predators are larger dark hunters whose eyes burn
 * while they hunt, and ocean currents (when the snapshot has them) show as
 * drifting streaks.
 *
 * Cost model: one instanced draw for all fish, one for their shadows, one each
 * for predators, food, currents and the floor. Per frame the CPU only packs
 * the spine points (20 floats per fish) into instance buffers; the bending,
 * shading and tail beat happen in the vertex/fragment shaders.
 *
 * World coordinates: sim (x right, y down, origin top-left) -> three
 * (X = x - W/2, Z = y - H/2, Y up = -depth).
 *
 * EvoOceanScene.load() -> Promise (imports the trimmed three bundle,
 * window.EVO_ASSETS.three); EvoOceanScene.create(canvas, opts) -> scene or null. */
(function () {
  var THREE = null;
  var TANK_D = 240;          /* default tank depth when opts.tankDepth is absent; FLOOR_Y = -(D + 14), per scene */
  var TAU = Math.PI * 2;
  var POLAR = 1.0;           /* default camera tilt from straight down, radians: a deep tank wants a side view */
  var FOV = 38;

  /* sRGB colours, authored for this scene (no colour management in the shaders) */
  var C = {
    fog: [0.027, 0.122, 0.125],
    sand: [0.72, 0.63, 0.48],       /* dry sand, lighter and darker drifts ... */
    sandDark: [0.56, 0.49, 0.38],
    water: [0.52, 0.57, 0.57],      /* ... under the deep water's blue-green */
    surfSky: [0.20, 0.36, 0.38],    /* the surface from below: Snell's window ... */
    surfMirror: [0.035, 0.085, 0.09], /* ... and the mirrored water around it */
    caustic: [0.70, 0.95, 0.84],
    edge: [0.55, 0.85, 0.78],
    foodCore: [0.93, 1.0, 0.62],
    foodGlow: [0.55, 0.86, 0.18],
    current: [0.55, 0.86, 0.98],
    select: [0.78, 0.95, 0.30],
    predator: [0.16, 0.18, 0.22],
    snow: [0.78, 0.90, 0.86]
  };
  var LIGHT = (function () { var v = [-0.38, 1.0, -0.30], l = Math.hypot(v[0], v[1], v[2]); return [v[0] / l, v[1] / l, v[2] / l]; })();

  function load() {
    if (THREE) return Promise.resolve(window.EvoOceanScene);
    var url = window.EVO_ASSETS && window.EVO_ASSETS.three;
    if (!url) return Promise.reject(new Error("EVO_ASSETS.three missing"));
    /* resolved against the page, like the other assets */
    return import(new URL(url, document.baseURI).href).then(function (m) { THREE = m; return window.EvoOceanScene; });
  }

  /* ---------------- shaders ---------------- */
  var FISH_CORE = [
    "attribute float part;",
    "attribute vec4 iA;", "attribute vec4 iB;", "attribute vec4 iC;",
    "attribute vec4 iCol;",   /* rgb, tail-beat phase */
    "attribute vec4 iBody;",  /* radius, height, highlight (1 selected, .5 hover), ember */
    "attribute vec4 iX;",     /* x: pitch as tan(angle) = vz, + = sinking (head down); yzw spare */
    "uniform vec2 uHalf;", "uniform float uTime;", "uniform float uWag;",
    "vec2 seg(int i) {",
    "  if (i <= 0) return iA.xy; if (i == 1) return iA.zw; if (i == 2) return iB.xy;",
    "  if (i == 3) return iB.zw; if (i == 4) return iC.xy; return iC.zw;",
    "}",
    /* Catmull-Rom through the six spine points; straight beyond the ends */
    "vec2 spine(float s) {",
    "  if (s <= 0.0) return seg(0) + (seg(0) - seg(1)) * (-s);",
    "  if (s >= 5.0) return seg(5) + (seg(5) - seg(4)) * (s - 5.0);",
    "  int i = int(floor(s)); float t = s - float(i);",
    "  vec2 p0 = seg(max(i - 1, 0)), p1 = seg(i), p2 = seg(min(i + 1, 5)), p3 = seg(min(i + 2, 5));",
    "  float t2 = t * t, t3 = t2 * t;",
    "  return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2",
    "              + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);",
    "}",
    "vec3 fishWorld() {",
    "  float u = position.x, R = iBody.x;",
    "  float s = clamp(u, -0.5, 1.6) * 5.0;",
    "  vec2 c = spine(s);",
    "  vec2 f = spine(s - 0.15) - spine(s + 0.15);",
    "  float fl = length(f);",
    "  f = fl > 1e-4 ? f / fl : vec2(1.0, 0.0);",
    "  vec2 side = vec2(-f.y, f.x);",
    "  float wag = sin(uTime * 7.5 + iCol.w * 6.2831 - u * 4.5) * uWag * smoothstep(0.1, 1.3, u) * R * 0.3;",
    "  vec2 p = c + side * (position.y * R + wag);",
    /* pitch: the head stays at the fish's depth and every point behind it
     * rises (sinking) or falls (rising) by its distance along the spine times
     * tan(pitch), so trail, rings and picking (all at the head) stay right and
     * the shadow follows the tilted body */
    "  float along = length(c - seg(0)) * (s < 0.0 ? -1.0 : 1.0);",
    "  float lift = along * clamp(iX.x, -1.2, 1.2);",
    "  return vec3(p.x - uHalf.x, iBody.y + position.z * R + lift, p.y - uHalf.y);",
    "}"
  ].join("\n");

  var FISH_VS = FISH_CORE + "\n" + [
    "varying vec3 vWorld; varying vec3 vCol; varying float vPart; varying float vOy;",
    "varying float vSel; varying float vEmber;",
    "void main() {",
    "  vec3 w = fishWorld();",
    "  vWorld = w; vCol = iCol.rgb; vPart = part; vOy = position.z; vSel = iBody.z; vEmber = iBody.w;",
    "  gl_Position = projectionMatrix * viewMatrix * vec4(w, 1.0);",
    "}"
  ].join("\n");

  var FOG_FN = [
    "uniform vec3 uFog; uniform float uFogDensity;",
    "float fogOf(vec3 w) { float d = length(cameraPosition - w) * uFogDensity; return 1.0 - exp(-d * d); }"
  ].join("\n");

  /* The flashlight (setFlashlight; viewer only, the simulation never sees
   * it): a cone of clear light from the camera along uBeamDir, half-angle
   * atan(uBeamTan), soft at its rim, fading out past uBeamReach along the
   * axis. Inside it the fish, the food and the sand show their true colours:
   * no depth fall-off, no water tint, no fog. beamOf() is that share, 0..1
   * (times uBeamOn, the fade); 0 leaves every pixel exactly as without it. */
  var BEAM_FN = [
    "uniform float uBeamOn; uniform vec3 uBeamDir; uniform float uBeamTan; uniform float uBeamReach;",
    "float beamOf(vec3 w) {",
    "  if (uBeamOn <= 0.001) return 0.0;",
    "  vec3 d = w - cameraPosition;",
    "  float along = dot(d, uBeamDir);",
    "  if (along <= 0.0) return 0.0;",
    "  float r = along * uBeamTan;",
    "  float perp = length(d - uBeamDir * along);",
    "  return uBeamOn * (1.0 - smoothstep(0.45 * r, r, perp)) * (1.0 - smoothstep(uBeamReach, uBeamReach * 1.6, along));",
    "}"
  ].join("\n");

  /* Light under water: dimmer and bluer with depth (0 at the surface, 1 at
   * the tank's floor depth D), shared by the fish, their shadows and the food.
   * With the engine's light field on (E1: oc_light_enabled, setLight), the
   * fall-off is the engine's L(d) = L0·exp(−kWater·d) instead: brightness
   * L(d) and a blue shift of 1 − exp(−kWater·d); off, the preview's branch. */
  var DEPTH_FN = [
    "uniform float uDepth;",
    "uniform float uLightOn; uniform float uL0; uniform float uKWater;",
    "float depthOf(float y) { return clamp(-y / uDepth, 0.0, 1.0); }",
    /* how far into the dark a point is: depth / D, or 1 − L(d)/L0 with light on */
    "float shadeOf(float y) {",
    "  if (uLightOn < 0.5) return depthOf(y);",
    "  return clamp(1.0 - exp(-uKWater * max(-y, 0.0)), 0.0, 1.0);",
    "}",
    "vec3 underwater(vec3 col, float dz) {",
    "  col *= mix(vec3(1.0), vec3(0.28, 0.50, 0.90), dz) * (1.0 - 0.42 * dz);",
    "  return col + vec3(0.0, 0.02, 0.055) * dz;",
    "}",
    "vec3 underwaterAt(vec3 col, float y) {",
    "  if (uLightOn < 0.5) return underwater(col, depthOf(y));",
    "  float t = shadeOf(y);",
    "  float L = uL0 * exp(-uKWater * max(-y, 0.0));",          /* the engine's lightAt(d) */
    "  col *= mix(vec3(1.0), vec3(0.28, 0.50, 0.90), t) * clamp(L, 0.05, 1.6);",
    "  return col + vec3(0.0, 0.02, 0.055) * t;",
    "}"
  ].join("\n");

  var FISH_FS = [
    "uniform vec3 uLight; uniform float uTime; uniform vec3 uSelect;",
    FOG_FN,
    DEPTH_FN,
    BEAM_FN,
    "varying vec3 vWorld; varying vec3 vCol; varying float vPart; varying float vOy;",
    "varying float vSel; varying float vEmber;",
    "void main() {",
    /* flat facets: the triangle's own normal, from screen-space derivatives */
    "  vec3 N = normalize(cross(dFdx(vWorld), dFdy(vWorld)));",
    "  vec3 V = normalize(cameraPosition - vWorld);",
    "  if (dot(N, V) < 0.0) N = -N;",
    "  vec3 base = vCol;",
    "  float body = 1.0 - step(0.5, vPart);",
    "  base = mix(base, base * 0.56, smoothstep(0.30, 0.85, vOy) * body);",           /* darker back */
    "  base = mix(base, mix(base, vec3(0.93, 0.97, 0.90), 0.5), 1.0 - smoothstep(-0.7, -0.15, vOy));", /* pale belly */
    "  if (vPart > 0.5 && vPart < 1.5) base = mix(base, vec3(0.92, 0.98, 0.95), 0.22);", /* fins */
    "  float diff = max(dot(N, uLight), 0.0);",
    "  vec3 amb = mix(vec3(0.07, 0.13, 0.14), vec3(0.36, 0.48, 0.46), 0.5 + 0.5 * N.y);",
    "  vec3 col = base * (amb + diff * vec3(1.0, 0.96, 0.86) * 0.92);",
    "  float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);",
    "  col += rim * mix(base, vec3(0.6, 1.0, 0.92), 0.55) * 0.35;",
    "  vec3 clear = col;",                                                             /* in clear light (the flashlight) */
    "  col = underwaterAt(col, vWorld.y);",
    "  vec3 ember = vec3(1.0, 0.38, 0.16) * (0.8 + 0.2 * sin(uTime * 6.0));",
    "  vec3 add = vec3(0.0);",
    "  if (vPart > 1.5) {",                                                            /* eyes */
    "    col = vEmber > 0.5 ? ember * 1.2 : vec3(0.02, 0.03, 0.03);",
    "    col += pow(max(dot(reflect(-uLight, N), V), 0.0), 20.0) * 0.8;",
    "    clear = col;",
    "  } else if (vEmber > 0.5) {",
    "    add += ember * rim * 0.55;",
    "  }",
    "  if (vSel > 0.75) add += uSelect * (0.16 + 0.55 * rim);",
    "  else if (vSel > 0.25) add += uSelect * 0.4 * rim;",
    "  col += add;",
    "  vec3 murky = mix(col, uFog, fogOf(vWorld));",
    "  float bk = beamOf(vWorld);",
    "  if (bk > 0.0) {",
    "    clear = (clear + add) * (1.0 + 0.2 * max(dot(N, -uBeamDir), 0.0));",        /* the beam's own light on the side facing it */
    "    murky = mix(murky, clear, bk);",
    "  }",
    "  gl_FragColor = vec4(murky, 1.0);",
    "}"
  ].join("\n");

  /* a fish high above the floor casts a fainter shadow (vLift: 0 at the floor) */
  var SHADOW_VS = FISH_CORE + "\n" + [
    "uniform vec3 uLight; uniform float uFloorY; uniform float uDepth;",
    "varying vec3 vWorld; varying float vLift;",
    "void main() {",
    "  vec3 w = fishWorld();",
    "  vec3 s = w - uLight * ((w.y - uFloorY) / uLight.y);",
    "  s.y = uFloorY + 0.8;",
    "  vWorld = s;",
    "  vLift = clamp((w.y - uFloorY) / uDepth, 0.0, 1.0);",
    "  gl_Position = projectionMatrix * viewMatrix * vec4(s, 1.0);",
    "}"
  ].join("\n");
  var SHADOW_FS = [
    FOG_FN,
    "uniform float uAlpha;",
    "varying vec3 vWorld; varying float vLift;",
    "void main() { gl_FragColor = vec4(0.008, 0.04, 0.05, uAlpha * (1.0 - 0.7 * vLift) * (1.0 - fogOf(vWorld))); }"
  ].join("\n");

  var FLOOR_VS = [
    "varying vec3 vWorld;",
    "void main() { vec4 w = modelMatrix * vec4(position, 1.0); vWorld = w.xyz;",
    "  gl_Position = projectionMatrix * viewMatrix * w; }"
  ].join("\n");
  /* The seabed: sand under deep water, procedural and non-repeating: every
   * noise is value noise on a sin-free hash, each octave on its own rotated,
   * offset lattice, so nothing tiles or lines up. Three octaves of a
   * four-channel noise (broad L, mid M, small S, with their derivatives) drive:
   *  - tone: broad soft drifts of lighter and darker sand, a faint drift
   *    toward grey, a mid-scale mottle and, up close, a fine one
   *  - ripples: two trains ~50 degrees apart, each domain-warped by all three
   *    octaves, so crests curve, kink and bunch and their spacing and heading
   *    wander; each lies only in large patches (flat sand between, a crossing
   *    where they meet) and is broken into runs; an asymmetric profile with
   *    sharp crests and broad troughs
   *  - grains: up close, two jittered layers of round grains of varied size,
   *    tone and hue, a few dark specks and pale bits
   *  - light: the ripples' and grains' slopes (exact, from the noise
   *    derivatives) tilt the normal, lit from above (uLight), so they read as
   *    relief; troughs a shade darker
   *  - each detail fades out where a pixel spans a large part of its period
   *    (fwidth), so nothing aliases into lines far away or at grazing angles;
   *    far crests soften to bands, and the relief fades with distance (haze)
   * The water above tints it (uWater), then the caustics (two copies of the
   * tileable pattern sliding over each other, one rotated, both bent by the
   * broad noise, so their tiles never line up) and the fog go on top. */
  var FLOOR_FS = [
    FOG_FN,
    BEAM_FN,
    "uniform vec2 uHalf; uniform float uTime; uniform float uCausticAmt;",
    "uniform sampler2D uCaustic; uniform vec3 uSand; uniform vec3 uSandDark; uniform vec3 uWater;",
    "uniform vec3 uCausticCol; uniform vec3 uEdge; uniform vec3 uLight;",
    "varying vec3 vWorld;",
    /* Dave Hoskins' "Hash without Sine": no patterns at large coordinates */
    "vec4 hash42(vec2 p) { vec4 p4 = fract(p.xyxy * vec4(0.1031, 0.1030, 0.0973, 0.1099));",
    "  p4 += dot(p4, p4.wzxy + 33.33); return fract((p4.xxyz + p4.yzzw) * p4.zywx); }",
    "float hash12(vec2 p) { vec3 p3 = fract(p.xyx * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }",
    /* value noise, four independent channels, with each one's derivatives along p.x (gx) and p.y (gy) */
    "vec4 vnoise4(vec2 p, out vec4 gx, out vec4 gy) {",
    "  vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);",
    "  vec4 a = hash42(i), b = hash42(i + vec2(1.0, 0.0)), c = hash42(i + vec2(0.0, 1.0)), d = hash42(i + vec2(1.0, 1.0));",
    "  vec4 k1 = b - a, k2 = c - a, k3 = a - b - c + d;",
    "  gx = du.x * (k1 + k3 * u.y); gy = du.y * (k2 + k3 * u.x);",
    "  return a + k1 * u.x + k2 * u.y + k3 * u.x * u.y;",
    "}",
    "float vnoise1(vec2 p) { vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);",
    "  return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y); }",
    /* one layer of grains, q in grain cells: a round grain in each cell at a
     * random spot (wholly inside it), of random size, tone and hue; returns
     * (tone, hue, its outward direction x coverage) */
    "vec4 grains(vec2 q) {",
    "  vec4 h = hash42(floor(q) + 113.0);",
    "  vec2 v = fract(q) - 0.35 - 0.3 * h.xy;",
    "  float r = 0.2 + 0.15 * h.z, m = 1.0 - smoothstep(0.45 * r, r, length(v));",
    "  float tone = h.w < 0.035 ? -0.42 : (h.w > 0.965 ? 0.3 : (h.w - 0.5) * 0.44);",   /* dark specks, pale bits */
    "  return vec4(tone, h.z - 0.5, v / r) * m;",
    "}",
    "void main() {",
    "  vec2 p = vWorld.xz;",
    "  vec2 q = abs(p) - uHalf;",
    "  float dOut = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);",   /* > 0 outside the tank */
    "  float inside = 1.0 - smoothstep(-40.0, 120.0, dOut);",
    "  float fw = length(fwidth(p));",                                  /* world units per pixel */
    "  const mat2 R1 = mat2(0.80, 0.60, -0.60, 0.80), R2 = mat2(0.28, -0.96, 0.96, 0.28), R3 = mat2(-0.53, 0.848, -0.848, -0.53);",
    "  vec4 lx, ly, mx, my, sx, sy;",
    "  vec4 L = vnoise4(R1 * p * 0.0024 + vec2(17.3, -41.9), lx, ly);",  /* broad, ~420 across */
    "  vec4 M = vnoise4(R2 * p * 0.0075 + vec2(-7.1, 23.4), mx, my);",   /* mid, ~130 */
    "  vec4 S = vnoise4(R3 * p * 0.031 + vec2(5.7, 3.1), sx, sy);",      /* small, ~32 */
    /* the ripple trains' warps (x: the first, y: the second) and their
     * gradients: d/dp n(R p f) = f (grad n) R */
    "  float w1 = 130.0 * L.x + 38.0 * M.x + 7.0 * S.x, w2 = 110.0 * L.y + 32.0 * M.y + 6.0 * S.y;",
    "  vec2 g1 = 0.312 * (vec2(lx.x, ly.x) * R1) + 0.285 * (vec2(mx.x, my.x) * R2) + 0.217 * (vec2(sx.x, sy.x) * R3);",
    "  vec2 g2 = 0.264 * (vec2(lx.y, ly.y) * R1) + 0.24 * (vec2(mx.y, my.y) * R2) + 0.186 * (vec2(sx.y, sy.y) * R3);",
    "  const vec2 D1 = vec2(0.82, 0.57), D2 = vec2(0.11, 0.99);",
    "  const float K1 = 6.2831 / 17.0, K2 = 6.2831 / 12.5;",           /* crests ~17 and ~12.5 apart */
    "  float t1 = K1 * (dot(p, D1) + w1), t2 = K2 * (dot(p, D2) + w2);",
    "  g1 = K1 * (D1 + g1); g2 = K2 * (D2 + g2);",                      /* each phase's gradient */
    /* ripple fields in broad patches (stronger on lighter sand), flat sand
     * between; which train in mid patches (both where they meet); crests
     * broken into runs; gone where a pixel spans much of a period */
    "  float haze = (1.0 - smoothstep(900.0, 2800.0, length(cameraPosition - vWorld))) * mix(0.3, 1.0, inside);",   /* far off, and beyond the glass, the relief softens */
    "  float field = smoothstep(0.42, 0.70, 0.72 * L.w + 0.28 * M.x) * (0.4 + 0.6 * smoothstep(0.2, 0.7, M.z)) * haze;",
    "  float which = smoothstep(0.36, 0.64, M.w);",
    "  float a1 = field * (1.0 - which) * smoothstep(0.25, 0.65, S.z) * (1.0 - smoothstep(0.3, 1.1, fwidth(t1)));",
    "  float a2 = field * 0.75 * which * smoothstep(0.25, 0.65, S.w) * (1.0 - smoothstep(0.3, 1.1, fwidth(t2)));",
    /* the profile in s = t + 0.5 cos t (a long gentle side, a short steep one):
     * height c^2, c = (1 + cos s) / 2: sharp crests, broad troughs; eased to
     * c as a period shrinks below ~50 px (all c by ~15 px), so far crests are
     * soft bands, not thin dark lines */
    "  float s1 = t1 + 0.5 * cos(t1), c1 = 0.5 + 0.5 * cos(s1), e1 = 1.0 - smoothstep(0.12, 0.45, fwidth(t1));",
    "  float s2 = t2 + 0.5 * cos(t2), c2 = 0.5 + 0.5 * cos(s2), e2 = 1.0 - smoothstep(0.12, 0.45, fwidth(t2));",
    "  float hgt = a1 * (mix(c1, c1 * c1, e1) - 0.5 + 0.125 * e1) + a2 * (mix(c2, c2 * c2, e2) - 0.5 + 0.125 * e2);",   /* about its mean */
    "  vec2 slope = 2.0 * (a1 * mix(0.5, c1, e1) * sin(s1) * (0.5 * sin(t1) - 1.0) * g1",
    "                    + a2 * mix(0.5, c2, e2) * sin(s2) * (0.5 * sin(t2) - 1.0) * g2);",
    /* tone: broad drifts, a faint drift toward grey, mottles mid and fine */
    "  float tone = clamp(0.55 + 1.25 * (0.62 * L.z + 0.38 * M.z - 0.5), 0.0, 1.0);",
    "  vec3 base = mix(uSandDark, uSand, tone) * mix(vec3(1.0), vec3(0.94, 0.985, 1.02), smoothstep(0.35, 0.85, M.y));",
    "  float detail = 0.10 * (vnoise1(R2 * p * 0.07 + 31.0) - 0.5) * (1.0 - smoothstep(2.5, 6.0, fw))",
    "               + 0.08 * (vnoise1(R1 * p * 0.33 - 17.0) - 0.5) * (1.0 - smoothstep(0.5, 1.3, fw));",
    /* grains, up close only (none once a grain is under ~2 px) */
    "  float gA = 1.0 - smoothstep(0.08, 0.2, fw);",
    "  vec4 gr = vec4(0.0);",
    "  if (gA > 0.0) {",
    "    vec4 ga = grains(R1 * p * 4.5), gb = grains(R3 * p * 3.1 + 37.0);",
    "    gr = vec4(ga.xy + gb.xy, ga.zw * R1 + gb.zw * R3) * gA;",
    "  }",
    /* light from above on the relief: ripples, grains (domes) */
    "  slope -= 0.45 * gr.zw;",
    "  vec3 N = normalize(vec3(-slope.x, 1.0, -slope.y));",
    "  float lit = max(dot(N, uLight), 0.0) / uLight.y;",               /* 1 on flat sand */
    "  vec3 dry = base * (1.0 + detail + gr.x) * lit * (1.0 + 0.16 * hgt);",   /* the sand in clear light */
    "  dry *= vec3(1.0 + 0.06 * gr.y, 1.0, 1.0 - 0.06 * gr.y);",
    "  vec3 sand = dry * uWater;",
    "  vec3 col = sand * mix(0.36, 1.0, inside);",                      /* beyond the glass it is unlit */
    "  vec2 uv = p / 420.0 + (L.xy - 0.5) * 0.2;",
    "  float cA = texture2D(uCaustic, uv + vec2(uTime * 0.013, uTime * 0.008)).r;",
    "  float cB = texture2D(uCaustic, R1 * uv * 1.31 + vec2(-uTime * 0.010, uTime * 0.012)).r;",
    "  vec3 caus = uCausticCol * min(cA, cB) * uCausticAmt * (0.25 + 0.75 * inside) * (0.55 + 0.45 * lit);",
    "  vec3 edge = uEdge * (1.0 - smoothstep(0.0, 4.0, abs(dOut))) * 0.22;",   /* where the glass meets the floor */
    "  col += caus;",
    "  col += edge;",
    "  col = mix(col, uFog, max(fogOf(vWorld), smoothstep(0.0, 1100.0, dOut)));",
    "  float bk = beamOf(vWorld);",
    "  if (bk > 0.0) col = mix(col, dry * mix(0.36, 1.0, inside) + caus + edge, bk);",
    "  gl_FragColor = vec4(col, 1.0);",
    "}"
  ].join("\n");

  /* The surface, seen from below (from above it draws nothing, so the view
   * into the tank is unchanged). Inside Snell's window — rays within ~49
   * degrees of straight up — the sky's light comes through, brightest
   * overhead, with a bright rim at the window's edge; outside it the surface
   * mirrors the dark water. A shimmer (the caustic pattern, sliding) moves
   * over both. Additive, so it only ever adds light. */
  var SURF_FS = [
    FOG_FN,
    "uniform vec2 uHalf; uniform float uTime; uniform sampler2D uCaustic;",
    "uniform vec3 uSky; uniform vec3 uMirror;",
    "varying vec3 vWorld;",
    "void main() {",
    "  if (cameraPosition.y >= vWorld.y) discard;",
    "  vec3 V = normalize(vWorld - cameraPosition);",                   /* the view ray, looking up */
    "  float win = smoothstep(0.60, 0.70, V.y);",
    "  float rim = 1.0 - smoothstep(0.0, 0.035, abs(V.y - 0.66));",
    "  vec2 uv = vWorld.xz / 150.0;",
    "  float c1 = texture2D(uCaustic, uv + vec2(uTime * 0.021, -uTime * 0.016)).r;",
    "  float c2 = texture2D(uCaustic, uv * 0.73 + vec2(-uTime * 0.017, uTime * 0.019)).r;",
    "  float sh = 0.3 * (c1 + c2) + 0.8 * min(c1, c2);",                /* soft swell plus a bright network */
    "  vec2 q = abs(vWorld.xz) - uHalf;",
    "  float inside = 1.0 - smoothstep(0.0, 300.0, length(max(q, 0.0)));",
    "  vec3 col = uMirror * (0.55 + 0.9 * sh) * (1.0 - win)",
    "           + uSky * (0.40 + 0.8 * smoothstep(0.72, 1.0, V.y) + 0.75 * sh) * win",
    "           + uSky * rim * 0.4;",
    "  col *= (0.45 + 0.55 * inside) * (1.0 - fogOf(vWorld));",
    "  gl_FragColor = vec4(col, 1.0);",
    "}"
  ].join("\n");

  /* plankton glows on its own, so depth only dims and cools it a little */
  var FOOD_VS = [
    "attribute float seed;",
    "uniform float uPx; uniform float uTime; uniform float uSize;",
    DEPTH_FN,
    BEAM_FN,
    "varying float vTw; varying float vDz;",
    "void main() {",
    "  vec4 mv = viewMatrix * vec4(position, 1.0);",
    "  gl_Position = projectionMatrix * mv;",
    "  float bk = beamOf(position);",                                   /* in the flashlight: its true colour */
    "  vDz = shadeOf(position.y) * (1.0 - bk);",
    "  vTw = (0.78 + 0.22 * sin(uTime * 2.3 + seed * 6.2831)) * (1.0 - 0.3 * vDz) * (1.0 + 0.25 * bk);",
    "  gl_PointSize = max(2.0, uSize * uPx / -mv.z);",
    "}"
  ].join("\n");
  var FOOD_FS = [
    "uniform vec3 uCore; uniform vec3 uGlow;",
    "varying float vTw; varying float vDz;",
    "void main() {",
    "  float r = length(gl_PointCoord - 0.5) * 2.0;",
    "  if (r > 1.0) discard;",
    "  vec3 col = uCore * smoothstep(0.34, 0.0, r) + uGlow * pow(1.0 - r, 2.4) * 0.6;",
    "  col = mix(col, col.gbr * vec3(0.55, 0.9, 1.0), 0.45 * vDz);",
    "  gl_FragColor = vec4(col * vTw, 1.0);",
    "}"
  ].join("\n");

  /* a soft ember glow under each predator: readable at any zoom */
  var GLOW_VS = [
    "attribute float amt;",
    "uniform float uPx; uniform float uSize;",
    "varying float vAmt;",
    "void main() {",
    "  vec4 mv = viewMatrix * vec4(position, 1.0);",
    "  gl_Position = projectionMatrix * mv;",
    "  vAmt = amt;",
    "  gl_PointSize = max(6.0, uSize * uPx / -mv.z);",
    "}"
  ].join("\n");
  var GLOW_FS = [
    "uniform vec3 uCol; uniform float uTime;",
    "varying float vAmt;",
    "void main() {",
    "  float r = length(gl_PointCoord - 0.5) * 2.0;",
    "  if (r > 1.0) discard;",
    "  float pulse = 0.85 + 0.15 * sin(uTime * 4.0);",
    "  gl_FragColor = vec4(uCol * pow(1.0 - r, 2.2) * vAmt * pulse * 0.55, 1.0);",
    "}"
  ].join("\n");

  var SNOW_VS = [
    "attribute float seed;",
    "uniform float uPx; uniform float uTime; uniform vec2 uHalf; uniform float uFloorY; uniform float uFall;",
    "varying float vA;",
    "void main() {",
    "  vec3 p = position;",
    /* from just above the surface (y = 10) to just under the floor: the whole depth */
    "  float L = uFall + 40.0;",
    "  float fall = mod(uTime * (2.0 + seed * 3.0) + seed * 300.0 * (L / 170.0), L);",
    "  p.y = uFloorY + uFall + 24.0 - fall;",
    "  p.x += sin(uTime * 0.21 + seed * 40.0) * 14.0;",
    "  p.z += cos(uTime * 0.17 + seed * 23.0) * 14.0;",
    "  vec4 mv = viewMatrix * vec4(p, 1.0);",
    "  gl_Position = projectionMatrix * mv;",
    "  vA = smoothstep(0.0, 20.0, fall) * (1.0 - smoothstep(L - 30.0, L, fall)) * (0.35 + 0.65 * seed);",
    "  gl_PointSize = max(1.0, 3.2 * uPx / -mv.z);",
    "}"
  ].join("\n");
  var SNOW_FS = [
    "uniform vec3 uCol; varying float vA;",
    "void main() { float r = length(gl_PointCoord - 0.5) * 2.0; if (r > 1.0) discard;",
    "  gl_FragColor = vec4(uCol * (1.0 - r) * vA * 0.32, 1.0); }"
  ].join("\n");

  var LINE_VS = [
    "attribute float alpha;", "varying float vA;",
    "void main() { vA = alpha; gl_Position = projectionMatrix * viewMatrix * vec4(position, 1.0); }"
  ].join("\n");
  var LINE_FS = [
    "uniform vec3 uCol; varying float vA;",
    "void main() { gl_FragColor = vec4(uCol * vA, 1.0); }"
  ].join("\n");

  var RING_VS = [
    "varying vec2 vUv;",
    "void main() { vUv = uv; gl_Position = projectionMatrix * viewMatrix * modelMatrix * vec4(position, 1.0); }"
  ].join("\n");
  var RING_FS = [
    "uniform vec3 uCol; uniform float uTime; uniform float uStrength;",
    "varying vec2 vUv;",
    "void main() {",
    "  float r = length(vUv - 0.5) * 2.0;",
    "  float ring = 1.0 - smoothstep(0.0, 0.07, abs(r - 0.72));",
    "  float wave = fract(uTime * 0.6);",
    "  float pulse = (1.0 - smoothstep(0.0, 0.05, abs(r - (0.72 + wave * 0.28)))) * (1.0 - wave);",
    "  float a = (ring + pulse * 0.6) * uStrength;",
    "  if (a < 0.01) discard;",
    "  gl_FragColor = vec4(uCol * a, 1.0);",
    "}"
  ].join("\n");

  /* ---------------- geometry ---------------- */
  /* A fish in body units: x = position along the spine (0 head .. 1 last
   * segment; < 0 snout, > 1 tail fin), y = sideways, z = up, both in body
   * radii. part: 0 body, 1 fin, 2 eye. */
  function fishTemplate(pred) {
    var rings = pred
      ? [[-0.34, 0, 0], [-0.17, 0.40, 0.34], [0.02, 0.80, 0.66], [0.22, 1.0, 0.82],
         [0.46, 0.90, 0.70], [0.68, 0.62, 0.50], [0.86, 0.36, 0.32], [1.0, 0.18, 0.18]]
      : [[-0.20, 0, 0], [-0.10, 0.50, 0.42], [0.04, 0.88, 0.74], [0.20, 1.0, 0.84],
         [0.42, 0.86, 0.72], [0.64, 0.60, 0.52], [0.84, 0.36, 0.33], [1.0, 0.20, 0.20]];
    var SIDES = 6, pos = [], part = [], idx = [];
    function v(u, y, z, p) { pos.push(u, y, z); part.push(p); return part.length - 1; }
    function tri(a, b, c) { idx.push(a, b, c); }
    var tip = v(rings[0][0], 0, 0, 0), prev = null, k, r;
    for (r = 1; r < rings.length; r++) {
      var ring = [];
      for (k = 0; k < SIDES; k++) {
        var a = k / SIDES * TAU;
        ring.push(v(rings[r][0], Math.cos(a) * rings[r][1], Math.sin(a) * rings[r][2], 0));
      }
      for (k = 0; k < SIDES; k++) {
        var k1 = (k + 1) % SIDES;
        if (!prev) tri(tip, ring[k], ring[k1]);
        else { tri(prev[k], ring[k], ring[k1]); tri(prev[k], ring[k1], prev[k1]); }
      }
      prev = ring;
    }
    var cap = v(1.0, 0, 0, 0);
    for (k = 0; k < SIDES; k++) tri(prev[k], cap, prev[(k + 1) % SIDES]);
    /* tail: a horizontal forked fin, read clearly from above */
    var tw = pred ? 1.28 : 1.02, tl = pred ? 1.40 : 1.32, tn = pred ? 1.20 : 1.17;
    var c0 = v(1.0, 0, 0, 1), lb = v(0.99, -0.2, 0, 1), rb = v(0.99, 0.2, 0, 1);
    var lt = v(tl, -tw, 0, 1), rt = v(tl, tw, 0, 1), notch = v(tn, 0, 0, 1);
    tri(c0, lb, lt); tri(c0, lt, notch); tri(c0, notch, rt); tri(c0, rt, rb);
    /* dorsal fin */
    var dh = pred ? 1.95 : 1.45;
    var d0 = v(pred ? 0.18 : 0.26, 0, 0.80, 1), d1 = v(pred ? 0.62 : 0.60, 0, 0.55, 1);
    var d2 = v(pred ? 0.40 : 0.50, 0, dh, 1);
    tri(d0, d1, d2);
    /* pectoral fins */
    var pl = pred ? 2.1 : 1.7;
    [-1, 1].forEach(function (sd) {
      var p0 = v(0.14, sd * 0.84, -0.12, 1), p1 = v(0.34, sd * 0.70, -0.20, 1);
      var p2 = v(0.40, sd * pl, -0.40, 1);
      tri(p0, p1, p2);
      /* eye: a small diamond standing on the flank */
      var u0 = pred ? -0.02 : 0.03, ex = sd * (pred ? 0.70 : 0.78);
      var e0 = v(u0 - 0.055, ex, 0.30, 2), e1 = v(u0, ex * 1.04, 0.44, 2);
      var e2 = v(u0 + 0.055, ex, 0.30, 2), e3 = v(u0, ex * 1.04, 0.16, 2);
      tri(e0, e1, e2); tri(e0, e2, e3);
    });
    return { pos: new Float32Array(pos), part: new Float32Array(part), idx: idx };
  }

  /* A tileable caustic pattern (after Dave Hoskins' "Tileable Water
   * Caustic"), computed once; the floor animates it by sliding two copies
   * across each other and keeping the darker. */
  function causticTexture(size) {
    var data = new Uint8Array(size * size * 4), t = 0.85, inten = 0.005;
    for (var y = 0; y < size; y++) {
      for (var x = 0; x < size; x++) {
        var px = (x / size) * TAU - 250, py = (y / size) * TAU - 250;
        var ix = px, iy = py, c = 1;
        for (var n = 0; n < 5; n++) {
          var tt = t * (1 - 3.5 / (n + 1));
          var nx = px + Math.cos(tt - ix) + Math.sin(tt + iy);
          var ny = py + Math.sin(tt - iy) + Math.cos(tt + ix);
          ix = nx; iy = ny;
          c += 1 / Math.hypot(px / (Math.sin(ix + tt) / inten), py / (Math.cos(iy + tt) / inten));
        }
        c /= 5;
        c = 1.17 - Math.pow(c, 1.4);
        var val = Math.min(1, Math.pow(Math.abs(c), 8));
        var o = (y * size + x) * 4;
        data[o] = data[o + 1] = data[o + 2] = Math.round(val * 255);
        data[o + 3] = 255;
      }
    }
    var tex = new THREE.DataTexture(data, size, size);
    tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
    tex.magFilter = THREE.LinearFilter;
    tex.minFilter = THREE.LinearMipmapLinearFilter;
    tex.generateMipmaps = true;
    tex.needsUpdate = true;
    return tex;
  }

  function hash01(n) { var s = Math.sin(n * 12.9898 + 78.233) * 43758.5453; return s - Math.floor(s); }

  /* ---------------- the scene ---------------- */
  function create(canvas, opts) {
    if (!THREE) return null;
    opts = opts || {};
    var renderer;
    try {
      renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: !opts.coarse, alpha: false,
                                           powerPreference: "high-performance", stencil: false });
    } catch (e) {
      console.warn("evo_boids: WebGL2 unavailable, using the 2D renderer", e);
      return null;
    }
    renderer.toneMapping = THREE.NoToneMapping;
    /* the shaders write sRGB as-is, but three colour-manages the clear colour:
     * hand it over as sRGB ("srgb" = THREE.SRGBColorSpace) so the water above
     * the horizon (seen from a low camera) is the fog colour, not a pale teal */
    renderer.setClearColor(new THREE.Color().setRGB(C.fog[0], C.fog[1], C.fog[2], "srgb"), 1);
    var lost = false;
    canvas.addEventListener("webglcontextlost", function (e) { e.preventDefault(); lost = true; });
    canvas.addEventListener("webglcontextrestored", function () { lost = false; });

    var scene = new THREE.Scene();
    var camera = new THREE.PerspectiveCamera(FOV, 1, 4, 20000);
    var W = 1920, H = 1080, halfW = 960, halfH = 540;
    /* the tank's depth D (sim units, the world's oc_depth_max) and its floor,
     * a little below the deepest a fish can swim */
    var TD = opts.tankDepth > 0 ? +opts.tankDepth : TANK_D;
    var FLOOR_Y = -(TD + 14);
    var reduced = !!opts.reducedMotion;
    var quality = opts.coarse ? 2 : 3;
    var insets = { top: 0, right: 0, bottom: 0, left: 0 };
    var cw = 1, ch = 1;

    function v3(a) { return new THREE.Vector3(a[0], a[1], a[2]); }
    var common = {
      uHalf: { value: new THREE.Vector2(halfW, halfH) },
      uTime: { value: 0 },
      uFog: { value: v3(C.fog) },
      uFogDensity: { value: 0.0003 },
      uLight: { value: v3(LIGHT) },
      uDepth: { value: TD },
      uFloorY: { value: FLOOR_Y },
      /* the engine's light field (setLight); off = the preview's depth shading */
      uLightOn: { value: 0 }, uL0: { value: 1 }, uKWater: { value: 0 },
      /* the flashlight (setFlashlight, BEAM_FN): its fade 0..1, axis, cone, reach */
      uBeamOn: { value: 0 }, uBeamDir: { value: new THREE.Vector3(0, -1, 0) },
      uBeamTan: { value: 0.2 }, uBeamReach: { value: 1000 }
    };
    function beamUniforms(u) {
      u.uBeamOn = common.uBeamOn; u.uBeamDir = common.uBeamDir;
      u.uBeamTan = common.uBeamTan; u.uBeamReach = common.uBeamReach;
      return u;
    }

    /* floor */
    var floorMat = new THREE.ShaderMaterial({
      vertexShader: FLOOR_VS, fragmentShader: FLOOR_FS,
      uniforms: beamUniforms({
        uHalf: common.uHalf, uTime: common.uTime, uFog: common.uFog, uFogDensity: common.uFogDensity,
        uCaustic: { value: (function () { var t = new THREE.DataTexture(new Uint8Array([0, 0, 0, 255]), 1, 1); t.needsUpdate = true; return t; })() },
        uCausticAmt: { value: 0 },
        uSand: { value: v3(C.sand) }, uSandDark: { value: v3(C.sandDark) }, uWater: { value: v3(C.water) },
        uCausticCol: { value: v3(C.caustic) }, uEdge: { value: v3(C.edge) }, uLight: common.uLight
      })
    });
    var floor = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), floorMat);
    floor.rotation.x = -Math.PI / 2;
    floor.position.y = FLOOR_Y;
    floor.renderOrder = 0;
    scene.add(floor);

    /* the surface, lit from above when seen from below (SURF_FS); it shares
     * the floor's caustic texture uniform, so it shimmers once that is built */
    var surfMat = new THREE.ShaderMaterial({
      vertexShader: FLOOR_VS, fragmentShader: SURF_FS,
      uniforms: { uHalf: common.uHalf, uTime: common.uTime, uFog: common.uFog, uFogDensity: common.uFogDensity,
                  uCaustic: floorMat.uniforms.uCaustic, uSky: { value: v3(C.surfSky) }, uMirror: { value: v3(C.surfMirror) } },
      transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, side: THREE.DoubleSide
    });
    var surface = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), surfMat);
    surface.rotation.x = -Math.PI / 2;
    surface.position.y = 0.5;
    surface.renderOrder = 7;
    surface.frustumCulled = false;
    scene.add(surface);

    /* fish + predators: instanced templates sharing one shader */
    var fishUniforms = beamUniforms({
      uHalf: common.uHalf, uTime: common.uTime, uFog: common.uFog, uFogDensity: common.uFogDensity,
      uLight: common.uLight, uWag: { value: reduced ? 0 : 1 }, uSelect: { value: v3(C.select) },
      uDepth: common.uDepth, uLightOn: common.uLightOn, uL0: common.uL0, uKWater: common.uKWater
    });
    var fishMat = new THREE.ShaderMaterial({ vertexShader: FISH_VS, fragmentShader: FISH_FS,
                                             uniforms: fishUniforms, side: THREE.DoubleSide });
    var shadowMat = new THREE.ShaderMaterial({
      vertexShader: SHADOW_VS, fragmentShader: SHADOW_FS,
      uniforms: { uHalf: common.uHalf, uTime: common.uTime, uWag: fishUniforms.uWag, uLight: common.uLight,
                  uFloorY: common.uFloorY, uDepth: common.uDepth, uFog: common.uFog, uFogDensity: common.uFogDensity,
                  uAlpha: { value: 0.40 } },
      transparent: true, depthWrite: true, depthFunc: THREE.LessDepth, side: THREE.DoubleSide
    });

    /* One instanced kind: template + six per-instance attributes (iX: the
     * pitch). The geometry is rebuilt (and the old one disposed) only when it
     * has to grow. */
    function Kind(pred) {
      var tpl = fishTemplate(pred);
      var self = { cap: 0, n: 0, geo: null, mesh: null, shadow: null, attrs: null };
      function build(cap) {
        var g = new THREE.InstancedBufferGeometry();
        g.setAttribute("position", new THREE.BufferAttribute(tpl.pos, 3));
        g.setAttribute("part", new THREE.BufferAttribute(tpl.part, 1));
        g.setIndex(tpl.idx);
        var at = {};
        ["iA", "iB", "iC", "iCol", "iBody", "iX"].forEach(function (name) {
          var a = new THREE.InstancedBufferAttribute(new Float32Array(cap * 4), 4);
          a.setUsage(THREE.DynamicDrawUsage);
          g.setAttribute(name, a);
          at[name] = a;
        });
        g.instanceCount = 0;
        if (self.geo) {
          self.geo.dispose();
          self.mesh.geometry = g;
          self.shadow.geometry = g;
        } else {
          self.mesh = new THREE.Mesh(g, fishMat);
          self.shadow = new THREE.Mesh(g, shadowMat);
          self.mesh.frustumCulled = self.shadow.frustumCulled = false;
          self.shadow.renderOrder = 1;
          self.mesh.renderOrder = 2;
          scene.add(self.shadow);
          scene.add(self.mesh);
        }
        self.geo = g; self.attrs = at; self.cap = cap;
      }
      self.ensure = function (n) {
        if (n <= self.cap) return;
        var cap = Math.max(256, self.cap);
        while (cap < n) cap *= 2;
        build(cap);
      };
      self.commit = function (n) {
        self.n = n;
        self.geo.instanceCount = n;
        Object.keys(self.attrs).forEach(function (name) {
          var a = self.attrs[name];
          a.clearUpdateRanges();
          a.addUpdateRange(0, n * 4);
          a.needsUpdate = true;
        });
        self.mesh.visible = n > 0;
        self.shadow.visible = n > 0 && quality >= 2;
      };
      self.ensure(pred ? 64 : 2048);
      return self;
    }
    var fish = Kind(false), preds = Kind(true);

    /* food: glowing plankton */
    var foodGeo = new THREE.BufferGeometry(), foodCap = 0;
    var foodMat = new THREE.ShaderMaterial({
      vertexShader: FOOD_VS, fragmentShader: FOOD_FS,
      uniforms: beamUniforms({ uPx: { value: 500 }, uTime: common.uTime, uSize: { value: 11 }, uDepth: common.uDepth,
                  uLightOn: common.uLightOn, uL0: common.uL0, uKWater: common.uKWater,
                  uCore: { value: v3(C.foodCore) }, uGlow: { value: v3(C.foodGlow) } }),
      transparent: true, depthWrite: false, blending: THREE.AdditiveBlending
    });
    var food = new THREE.Points(foodGeo, foodMat);
    food.frustumCulled = false;
    food.renderOrder = 3;
    scene.add(food);
    function ensureFood(n) {
      if (n <= foodCap) return;
      var cap = Math.max(1024, foodCap);
      while (cap < n) cap *= 2;
      var p = new THREE.BufferAttribute(new Float32Array(cap * 3), 3);
      var sd = new THREE.BufferAttribute(new Float32Array(cap), 1);
      p.setUsage(THREE.DynamicDrawUsage); sd.setUsage(THREE.DynamicDrawUsage);
      foodGeo.dispose();
      foodGeo.setAttribute("position", p);
      foodGeo.setAttribute("seed", sd);
      foodCap = cap;
    }
    ensureFood(1024);

    /* predator glow */
    var GLOW_MAX = 512;
    var glowGeo = new THREE.BufferGeometry();
    glowGeo.setAttribute("position", new THREE.BufferAttribute(new Float32Array(GLOW_MAX * 3), 3).setUsage(THREE.DynamicDrawUsage));
    glowGeo.setAttribute("amt", new THREE.BufferAttribute(new Float32Array(GLOW_MAX), 1).setUsage(THREE.DynamicDrawUsage));
    glowGeo.setDrawRange(0, 0);
    var glowMat = new THREE.ShaderMaterial({
      vertexShader: GLOW_VS, fragmentShader: GLOW_FS,
      uniforms: { uPx: foodMat.uniforms.uPx, uSize: { value: 70 }, uTime: common.uTime,
                  uCol: { value: v3([1.0, 0.42, 0.18]) } },
      transparent: true, depthWrite: false, blending: THREE.AdditiveBlending
    });
    var glow = new THREE.Points(glowGeo, glowMat);
    glow.frustumCulled = false;
    glow.renderOrder = 1;
    scene.add(glow);

    /* marine snow: a fixed buffer, animated entirely in the shader (more of
     * it than in the shallow tank: it falls through the whole depth) */
    var snowGeo = new THREE.BufferGeometry(), SNOW = 900;
    (function () {
      var p = new Float32Array(SNOW * 3), sd = new Float32Array(SNOW);
      for (var i = 0; i < SNOW; i++) { p[3 * i] = Math.random() - 0.5; p[3 * i + 2] = Math.random() - 0.5; sd[i] = Math.random(); }
      snowGeo.setAttribute("position", new THREE.BufferAttribute(p, 3));
      snowGeo.setAttribute("seed", new THREE.BufferAttribute(sd, 1));
    })();
    var snowUnit = snowGeo.attributes.position.array.slice();
    var snowMat = new THREE.ShaderMaterial({
      vertexShader: SNOW_VS, fragmentShader: SNOW_FS,
      uniforms: { uPx: foodMat.uniforms.uPx, uTime: common.uTime, uHalf: common.uHalf,
                  uFloorY: common.uFloorY, uFall: common.uDepth, uCol: { value: v3(C.snow) } },
      transparent: true, depthWrite: false, blending: THREE.AdditiveBlending
    });
    var snow = new THREE.Points(snowGeo, snowMat);
    snow.frustumCulled = false;
    snow.renderOrder = 6;
    scene.add(snow);
    function placeSnow() {
      var p = snowGeo.attributes.position.array;
      for (var i = 0; i < SNOW; i++) { p[3 * i] = snowUnit[3 * i] * W * 1.1; p[3 * i + 2] = snowUnit[3 * i + 2] * H * 1.1; }
      snowGeo.attributes.position.needsUpdate = true;
    }

    /* line layers: currents, the selected fish's trail, the tank outline */
    function LineLayer(cap, col, order) {
      var g = new THREE.BufferGeometry();
      var p = new THREE.BufferAttribute(new Float32Array(cap * 3), 3);
      var a = new THREE.BufferAttribute(new Float32Array(cap), 1);
      p.setUsage(THREE.DynamicDrawUsage); a.setUsage(THREE.DynamicDrawUsage);
      g.setAttribute("position", p);
      g.setAttribute("alpha", a);
      g.setDrawRange(0, 0);
      var m = new THREE.ShaderMaterial({ vertexShader: LINE_VS, fragmentShader: LINE_FS,
                                         uniforms: { uCol: { value: v3(col) } },
                                         transparent: true, depthWrite: false, blending: THREE.AdditiveBlending });
      var obj = new THREE.LineSegments(g, m);
      obj.frustumCulled = false;
      obj.renderOrder = order;
      scene.add(obj);
      return { obj: obj, p: p.array, a: a.array, geo: g,
               commit: function (verts) {
                 g.setDrawRange(0, verts);
                 g.attributes.position.needsUpdate = true;
                 g.attributes.alpha.needsUpdate = true;
               } };
    }
    var STREAKS = 900;
    var currents = LineLayer(STREAKS * 2, C.current, 4);
    var trail = LineLayer(240 * 2, C.select, 5);
    var outline = LineLayer(24, C.edge, 4);   /* the glass box: top, bottom, four verticals */
    var streak = new Float32Array(STREAKS * 4);   /* x, y, age, life */
    for (var si = 0; si < STREAKS; si++) streak[4 * si + 2] = streak[4 * si + 3] = 1;

    /* selection and hover rings */
    function Ring(strength) {
      var m = new THREE.ShaderMaterial({ vertexShader: RING_VS, fragmentShader: RING_FS,
        uniforms: { uCol: { value: v3(C.select) }, uTime: common.uTime, uStrength: { value: strength } },
        transparent: true, depthWrite: false, blending: THREE.AdditiveBlending,
        side: THREE.DoubleSide });   /* seen from below too */
      var mesh = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), m);
      mesh.rotation.x = -Math.PI / 2;
      mesh.renderOrder = 5;
      mesh.visible = false;
      scene.add(mesh);
      return mesh;
    }
    var selRing = Ring(1), hoverRing = Ring(0.45);

    /* ---------------- camera ---------------- */
    var controls = new THREE.MapControls(camera, canvas);
    controls.enableDamping = true;
    controls.dampingFactor = 0.09;
    controls.screenSpacePanning = false;
    /* A free camera: it may orbit below the horizontal, up to nearly straight
     * up, so from near the floor you can look up at the surface; clampView
     * keeps it out of the sand. */
    controls.maxPolarAngle = Math.PI - 0.08;
    controls.minDistance = 40;
    controls.zoomToCursor = true;
    var MOUSE = { ROTATE: 0, DOLLY: 1, PAN: 2 }, TOUCH = { ROTATE: 0, PAN: 1, DOLLY_ROTATE: 3 };
    var fitDist = 1500, userMoved = false, anim = null;
    var follow = false, onFollow = opts.onFollowChange || function () {};
    var SURF_TOP = 30;   /* the view's centre may rise this far above the surface */
    /* A drag pans over the sea floor when the camera looks down into the tank,
     * and in the screen's plane (so up/down moves the view to another depth)
     * when it looks through the side or up; chosen as each drag starts. */
    controls.addEventListener("start", function () {
      userMoved = true; anim = null;
      controls.screenSpacePanning = controls.getPolarAngle() > 1.2;
    });
    controls.addEventListener("change", clampView);
    /* the view's centre stays near the tank: x/z a little beyond the glass,
     * y between just above the floor and a bit above the surface; the camera
     * never goes under the sand (it slides in along its line of sight) */
    function clampView() {
      var t = controls.target, cp = camera.position, mx = halfW + 80, mz = halfH + 80;
      var x = Math.max(-mx, Math.min(mx, t.x)), z = Math.max(-mz, Math.min(mz, t.z));
      var y = Math.max(FLOOR_Y + 8, Math.min(SURF_TOP, t.y));
      if (x !== t.x || y !== t.y || z !== t.z) {
        cp.x += x - t.x; cp.y += y - t.y; cp.z += z - t.z;
        t.set(x, y, z);
      }
      var minY = FLOOR_Y + 5;
      if (cp.y < minY) {
        var oy = cp.y - t.y, f = oy < -1e-6 ? (minY - t.y) / oy : 1;
        cp.set(t.x + (cp.x - t.x) * f, minY, t.z + (cp.z - t.z) * f);
      }
    }
    /* move the view (target and camera together) up or down, in sim units */
    function moveView(dy) {
      if (follow) { follow = false; setFollowControls(false); onFollow(false); }
      anim = null; userMoved = true;
      controls.target.y += dy; camera.position.y += dy;
      clampView();
    }
    function setFollowControls(on) {
      controls.enablePan = !on;
      controls.mouseButtons.LEFT = on ? MOUSE.ROTATE : MOUSE.PAN;
      controls.touches.ONE = on ? TOUCH.ROTATE : TOUCH.PAN;
    }

    function dir() { return new THREE.Vector3(0, Math.cos(POLAR), Math.sin(POLAR)); }
    function applyViewOffset() {
      var vw = Math.max(1, cw - insets.left - insets.right), vh = Math.max(1, ch - insets.top - insets.bottom);
      var cx = insets.left + vw / 2, cy = insets.top + vh / 2;
      camera.aspect = cw / ch;
      if (Math.abs(cx - cw / 2) < 0.5 && Math.abs(cy - ch / 2) < 0.5) {
        camera.clearViewOffset();
      } else {
        var fw = 2 * Math.max(cx, cw - cx), fh = 2 * Math.max(cy, ch - cy);
        camera.setViewOffset(fw, fh, fw / 2 - cx, fh / 2 - cy, cw, ch);
      }
      camera.updateProjectionMatrix();
      var fullH = camera.view && camera.view.enabled ? camera.view.fullHeight : ch;
      foodMat.uniforms.uPx.value = fullH * renderer.getPixelRatio() / (2 * Math.tan(FOV * Math.PI / 360));
    }
    /* Distance at which the whole tank fits the visible part of the canvas
     * (the canvas minus the panels' insets), found by projecting the eight
     * corners of its box (surface and floor), aimed at its middle depth. */
    function computeFit() {
      var target = new THREE.Vector3(0, -TD / 2, 0), d = 1500, D = dir();
      var vw = Math.max(1, cw - insets.left - insets.right), vh = Math.max(1, ch - insets.top - insets.bottom);
      var bx = insets.left + vw / 2, by = insets.top + vh / 2;
      var corners = [[-halfW, 0, -halfH], [halfW, 0, -halfH], [halfW, 0, halfH], [-halfW, 0, halfH],
                     [-halfW, FLOOR_Y, -halfH], [halfW, FLOOR_Y, -halfH], [halfW, FLOOR_Y, halfH], [-halfW, FLOOR_Y, halfH]];
      var p = new THREE.Vector3();
      var keepPos = camera.position.clone(), keepRot = camera.quaternion.clone();
      for (var it = 0; it < 6; it++) {
        camera.position.copy(target).addScaledVector(D, d);
        camera.lookAt(target);
        camera.updateMatrixWorld();
        var m = 0;
        for (var k = 0; k < 8; k++) {
          p.set(corners[k][0], corners[k][1], corners[k][2]).project(camera);
          var sx = (p.x * 0.5 + 0.5) * cw, sy = (0.5 - p.y * 0.5) * ch;
          m = Math.max(m, Math.abs(sx - bx) / (vw / 2), Math.abs(sy - by) / (vh / 2));
        }
        d *= m / 0.92;
      }
      camera.position.copy(keepPos);
      camera.quaternion.copy(keepRot);
      camera.updateMatrixWorld();
      return { target: target, pos: target.clone().addScaledVector(D, d), dist: d };
    }
    function resetView(animated) {
      var f = computeFit();
      fitDist = f.dist;
      controls.maxDistance = fitDist * 1.9;
      common.uFogDensity.value = 0.42 / fitDist;
      if (follow) { follow = false; setFollowControls(false); onFollow(false); }
      userMoved = false;
      if (animated && !reduced) {
        anim = { t: 0, ft: controls.target.clone(), fp: camera.position.clone(), tt: f.target, tp: f.pos };
      } else {
        anim = null;
        controls.target.copy(f.target);
        camera.position.copy(f.pos);
        camera.lookAt(f.target);
        controls.update();
      }
    }
    function stepAnim(dt) {
      if (!anim) return;
      anim.t = Math.min(1, anim.t + dt / 0.65);
      var e = 1 - Math.pow(1 - anim.t, 4);
      controls.target.lerpVectors(anim.ft, anim.tt, e);
      camera.position.lerpVectors(anim.fp, anim.tp, e);
      if (anim.t >= 1) anim = null;
    }

    function resize() {
      var r = canvas.getBoundingClientRect();
      cw = Math.max(1, Math.round(r.width)); ch = Math.max(1, Math.round(r.height));
      var dpr = Math.min(window.devicePixelRatio || 1, [1, 1.5, 2][quality - 1]);
      renderer.setPixelRatio(dpr);
      renderer.setSize(cw, ch, false);
      applyViewOffset();
      if (!userMoved && !follow) resetView(false);
    }

    /* depth (optional): a new tank depth D; the floor, shading and snow follow */
    function setWorld(w, h, depth) {
      W = w; H = h; halfW = w / 2; halfH = h / 2;
      if (depth > 0) {
        TD = +depth; FLOOR_Y = -(TD + 14);
        common.uDepth.value = TD; common.uFloorY.value = FLOOR_Y;
        floor.position.y = FLOOR_Y;
      }
      common.uHalf.value.set(halfW, halfH);
      var span = Math.max(w, h) * 6;
      floor.scale.set(span, span, 1);
      surface.scale.set(span, span, 1);
      placeSnow();
      /* the glass box: the surface rectangle, the one on the floor, and the
       * four vertical edges between them (the verticals a little fainter) */
      var o = outline, k = 0;
      var cs = [[-halfW, -halfH], [halfW, -halfH], [halfW, halfH], [-halfW, halfH]];
      function seg(ax, ay, az, bx, by, bz, al) {
        o.p[k * 3] = ax; o.p[k * 3 + 1] = ay; o.p[k * 3 + 2] = az; o.a[k++] = al;
        o.p[k * 3] = bx; o.p[k * 3 + 1] = by; o.p[k * 3 + 2] = bz; o.a[k++] = al;
      }
      for (var i = 0; i < 4; i++) {
        var a = cs[i], b = cs[(i + 1) % 4];
        seg(a[0], -1, a[1], b[0], -1, b[1], 0.16);
        seg(a[0], FLOOR_Y + 0.6, a[1], b[0], FLOOR_Y + 0.6, b[1], 0.16);
        seg(a[0], -1, a[1], a[0], FLOOR_Y + 0.6, a[1], 0.12);
      }
      o.commit(k);
      for (var s = 0; s < STREAKS; s++) streak[4 * s + 2] = streak[4 * s + 3] = 1;   /* respawn */
      trailLen = 0;
      resetView(false);
    }

    /* ---------------- per-frame packing ---------------- */
    var bodyScale = opts.bodyScale || function () { return 1; };
    /* per-instance swim height: -depth when the snapshot has depths (s.depth),
     * else keyed off the fish id as in the shallow tank */
    var heights = null;
    var SX = new Float64Array(6), SY = new Float64Array(6);
    function fillFish(s, selIdx, hovIdx) {
      var n = s.n;
      fish.ensure(n);
      var A = fish.attrs.iA.array, B = fish.attrs.iB.array, Cc = fish.attrs.iC.array;
      var COL = fish.attrs.iCol.array, BODY = fish.attrs.iBody.array, X = fish.attrs.iX.array;
      var segs = s.segs, cols = s.colors, body = s.body, ids = s.ids, depth = s.depth, vz = s.vz;
      if (!heights || heights.length < n) heights = new Float32Array(Math.max(n, 2048));
      var xs = SX, ys = SY;
      for (var c = 0; c < n; c++) {
        var b = 12 * c, px = segs[b], py = segs[b + 1];
        var o = 4 * c;
        /* unwrap a body that straddles the world's seam */
        xs[0] = px; ys[0] = py;
        for (var k = 1; k < 6; k++) {
          var x = segs[b + 2 * k], y = segs[b + 2 * k + 1];
          if (x - px > halfW) x -= W; else if (px - x > halfW) x += W;
          if (y - py > halfH) y -= H; else if (py - y > halfH) y += H;
          xs[k] = x; ys[k] = y; px = x; py = y;
        }
        A[o] = xs[0]; A[o + 1] = ys[0]; A[o + 2] = xs[1]; A[o + 3] = ys[1];
        B[o] = xs[2]; B[o + 1] = ys[2]; B[o + 2] = xs[3]; B[o + 3] = ys[3];
        Cc[o] = xs[4]; Cc[o + 1] = ys[4]; Cc[o + 2] = xs[5]; Cc[o + 3] = ys[5];
        var id = ids ? ids[c] : c;
        COL[o] = cols[3 * c] / 255; COL[o + 1] = cols[3 * c + 1] / 255; COL[o + 2] = cols[3 * c + 2] / 255;
        COL[o + 3] = hash01(id);
        var hgt = depth ? -depth[c] : (hash01(id * 1.7 + 3.1) - 0.5) * 8;
        heights[c] = hgt;
        BODY[o] = 4.6 * bodyScale(body ? body[2 * c] : 1);
        BODY[o + 1] = hgt;
        BODY[o + 2] = c === selIdx ? 1 : (c === hovIdx ? 0.5 : 0);
        BODY[o + 3] = 0;
        /* s.vz: + = sinking, as tan(pitch); absent (or NaN) = level */
        var t = vz ? vz[c] : 0;
        X[o] = t === t ? t : 0; X[o + 1] = 0; X[o + 2] = 0; X[o + 3] = 0;
      }
      fish.commit(n);
    }

    /* Predators carry only a position: a short history of where each one has
     * been becomes its spine, so it bends through its turns like the fish. */
    var predTrails = [];
    function fillPreds(s) {
      var n = s.pc;
      preds.ensure(n);
      var A = preds.attrs.iA.array, B = preds.attrs.iB.array, Cc = preds.attrs.iC.array;
      var COL = preds.attrs.iCol.array, BODY = preds.attrs.iBody.array, X = preds.attrs.iX.array;
      predTrails.length = Math.min(predTrails.length, n);
      var SP = 7;
      for (var i = 0; i < n; i++) {
        var x = s.preds[3 * i], y = s.preds[3 * i + 1], hunting = !(s.preds[3 * i + 2] > 0);
        var pdep = s.predDepth ? s.predDepth[i] : 0;
        var tr = predTrails[i];
        if (!tr || Math.hypot(tr[0] - x, tr[1] - y) > 60) { tr = predTrails[i] = [x, y, x - 1, y]; tr.d = pdep; tr.vz = 0; }
        else {
          var mv = Math.hypot(tr[0] - x, tr[1] - y);
          if (mv > 1.2) {
            /* its pitch, as for the fish: depth change over distance swum, smoothed */
            var inst = Math.max(-1.2, Math.min(1.2, (pdep - tr.d) / mv));
            tr.vz += (inst - tr.vz) * 0.25; tr.d = pdep;
            tr.unshift(x, y);
            if (tr.length > 40) tr.length = 40;
          }
        }
        X[4 * i] = tr.vz; X[4 * i + 1] = X[4 * i + 2] = X[4 * i + 3] = 0;
        /* points every SP px along the history; straight on past its end */
        var pts = [x, y], acc = 0, want = SP, j = 0;
        var lx = x, ly = y;
        while (pts.length < 12 && j + 3 < tr.length) {
          var ax = tr[j], ay = tr[j + 1], bx = tr[j + 2], by = tr[j + 3];
          var d = Math.hypot(bx - ax, by - ay);
          if (acc + d >= want && d > 0) {
            var f = (want - acc) / d;
            lx = ax + (bx - ax) * f; ly = ay + (by - ay) * f;
            pts.push(lx, ly);
            want += SP;
          } else { acc += d; j += 2; }
        }
        var dx = tr.length >= 4 ? tr[0] - tr[2] : 1, dy = tr.length >= 4 ? tr[1] - tr[3] : 0;
        var dl = Math.hypot(dx, dy) || 1;
        if (pts.length >= 4) { dx = pts[pts.length - 4] - pts[pts.length - 2]; dy = pts[pts.length - 3] - pts[pts.length - 1]; dl = Math.hypot(dx, dy) || 1; }
        while (pts.length < 12) {
          lx = pts[pts.length - 2] - dx / dl * SP; ly = pts[pts.length - 1] - dy / dl * SP;
          pts.push(lx, ly);
        }
        var o = 4 * i;
        A[o] = pts[0]; A[o + 1] = pts[1]; A[o + 2] = pts[2]; A[o + 3] = pts[3];
        B[o] = pts[4]; B[o + 1] = pts[5]; B[o + 2] = pts[6]; B[o + 3] = pts[7];
        Cc[o] = pts[8]; Cc[o + 1] = pts[9]; Cc[o + 2] = pts[10]; Cc[o + 3] = pts[11];
        COL[o] = C.predator[0]; COL[o + 1] = C.predator[1]; COL[o + 2] = C.predator[2]; COL[o + 3] = hash01(i + 0.5);
        var py = s.predDepth ? -s.predDepth[i] : 3;
        BODY[o] = hunting ? 7.2 : 8.2;   /* a fed predator looks fuller */
        BODY[o + 1] = py;
        BODY[o + 2] = 0;
        BODY[o + 3] = hunting ? 1 : 0;
        if (i < GLOW_MAX) {
          var gp = glowGeo.attributes.position.array, ga = glowGeo.attributes.amt.array;
          gp[3 * i] = x - halfW; gp[3 * i + 1] = py - 2; gp[3 * i + 2] = y - halfH;
          ga[i] = hunting ? 1 : 0.35;
        }
      }
      preds.commit(n);
      glowGeo.setDrawRange(0, Math.min(n, GLOW_MAX));
      glowGeo.attributes.position.needsUpdate = true;
      glowGeo.attributes.amt.needsUpdate = true;
    }

    function fillFood(s) {
      var n = s.fc;
      ensureFood(n);
      var p = foodGeo.attributes.position.array, sd = foodGeo.attributes.seed.array, fd = s.foodDepth;
      for (var i = 0; i < n; i++) {
        var x = s.food[2 * i], y = s.food[2 * i + 1];
        p[3 * i] = x - halfW; p[3 * i + 1] = fd ? -fd[i] : -3; p[3 * i + 2] = y - halfH;
        sd[i] = hash01(Math.floor(x / 9) * 131 + Math.floor(y / 9));
      }
      foodGeo.setDrawRange(0, n);
      foodGeo.attributes.position.clearUpdateRanges();
      foodGeo.attributes.position.addUpdateRange(0, n * 3);
      foodGeo.attributes.position.needsUpdate = true;
      foodGeo.attributes.seed.clearUpdateRanges();
      foodGeo.attributes.seed.addUpdateRange(0, n);
      foodGeo.attributes.seed.needsUpdate = true;
    }

    /* currents: streaks advected through the flow grid */
    function sampleCurrent(s, x, y, out) {
      var gx = (x - 32) / 64, gy = (y - 32) / 64;
      var i0 = Math.max(0, Math.min(s.curCols - 1, Math.floor(gx))), j0 = Math.max(0, Math.min(s.curRows - 1, Math.floor(gy)));
      var i1 = Math.min(s.curCols - 1, i0 + 1), j1 = Math.min(s.curRows - 1, j0 + 1);
      var fx = Math.max(0, Math.min(1, gx - i0)), fy = Math.max(0, Math.min(1, gy - j0));
      var g = s.curGrid, q00 = (j0 * s.curCols + i0) * 2, q10 = (j0 * s.curCols + i1) * 2;
      var q01 = (j1 * s.curCols + i0) * 2, q11 = (j1 * s.curCols + i1) * 2;
      for (var c = 0; c < 2; c++) {
        var a = g[q00 + c] + (g[q10 + c] - g[q00 + c]) * fx;
        var b = g[q01 + c] + (g[q11 + c] - g[q01 + c]) * fx;
        out[c] = a + (b - a) * fy;
      }
    }
    var vel = [0, 0];
    function fillCurrents(s, dtSim) {
      if (!s.curActive || !s.curGrid) { currents.commit(0); return; }
      var k = 0, P = currents.p, Aa = currents.a;
      for (var i = 0; i < STREAKS; i++) {
        var o = 4 * i;
        if (streak[o + 2] >= streak[o + 3]) {
          streak[o] = Math.random() * W; streak[o + 1] = Math.random() * H;
          streak[o + 2] = 0; streak[o + 3] = 1.5 + Math.random() * 3;
        }
        sampleCurrent(s, streak[o], streak[o + 1], vel);
        if (dtSim > 0) {
          streak[o] += vel[0] * dtSim; streak[o + 1] += vel[1] * dtSim;
          streak[o + 2] += dtSim;
          if (streak[o] < 0 || streak[o] > W || streak[o + 1] < 0 || streak[o + 1] > H) streak[o + 2] = streak[o + 3];
        }
        var sp = Math.hypot(vel[0], vel[1]);
        if (sp < 2) continue;
        var life = streak[o + 2] / streak[o + 3];
        var a = Math.sin(Math.PI * Math.min(1, life)) * Math.min(1, sp / 70) * 0.85;
        var x = streak[o] - halfW, z = streak[o + 1] - halfH;
        P[3 * k] = x; P[3 * k + 1] = 6; P[3 * k + 2] = z; Aa[k++] = a;
        P[3 * k] = x - vel[0] * 0.28; P[3 * k + 1] = 6; P[3 * k + 2] = z - vel[1] * 0.28; Aa[k++] = 0;
      }
      currents.commit(k);
    }

    /* the selected fish's recent path */
    var trailLen = 0, trailId = null, TRAIL_MAX = 240;
    var trailPts = new Float32Array(TRAIL_MAX * 3);
    function fillTrail(s, selIdx, stepped) {
      var id = selIdx >= 0 && s.ids ? s.ids[selIdx] : null;
      if (id === null) { trailLen = 0; trailId = null; trail.commit(0); selRing.visible = false; return; }
      if (id !== trailId) { trailLen = 0; trailId = id; }
      var x = s.segs[12 * selIdx] - halfW, z = s.segs[12 * selIdx + 1] - halfH, y = fishY(s, selIdx);
      if (trailLen > 0) {
        var lx = trailPts[0], lz = trailPts[2];
        if (Math.abs(x - lx) > halfW || Math.abs(z - lz) > halfH) trailLen = 0;   /* wrapped: start over */
      }
      if (stepped && (trailLen === 0 || Math.hypot(x - trailPts[0], y - trailPts[1], z - trailPts[2]) > 3)) {
        trailPts.copyWithin(3, 0, (TRAIL_MAX - 1) * 3);
        trailPts[0] = x; trailPts[1] = y; trailPts[2] = z;
        trailLen = Math.min(TRAIL_MAX, trailLen + 1);
      }
      var P = trail.p, Aa = trail.a, k = 0;
      for (var i = 0; i + 1 < trailLen; i++) {
        var f = 1 - i / TRAIL_MAX;
        P[3 * k] = trailPts[3 * i]; P[3 * k + 1] = trailPts[3 * i + 1]; P[3 * k + 2] = trailPts[3 * i + 2]; Aa[k++] = 0.55 * f * f;
        P[3 * k] = trailPts[3 * i + 3]; P[3 * k + 1] = trailPts[3 * i + 4]; P[3 * k + 2] = trailPts[3 * i + 5]; Aa[k++] = 0.55 * f * f;
      }
      trail.commit(k);
      selRing.visible = true;
      selRing.position.set(x, y - 1.5, z);
      selRing.scale.set(44, 44, 1);
    }

    /* ---------------- picking ---------------- */
    var vpm = new Float32Array(16);
    function updateVP() {
      camera.updateMatrixWorld();
      var a = camera.projectionMatrix.elements, b = camera.matrixWorldInverse.elements;
      for (var i = 0; i < 4; i++) for (var j = 0; j < 4; j++) {
        vpm[j * 4 + i] = a[i] * b[j * 4] + a[4 + i] * b[j * 4 + 1] + a[8 + i] * b[j * 4 + 2] + a[12 + i] * b[j * 4 + 3];
      }
    }
    function toScreen(x, y, z, out) {
      var m = vpm;
      var cx = m[0] * x + m[4] * y + m[8] * z + m[12];
      var cy = m[1] * x + m[5] * y + m[9] * z + m[13];
      var cwv = m[3] * x + m[7] * y + m[11] * z + m[15];
      if (cwv <= 0) return false;
      out[0] = (cx / cwv * 0.5 + 0.5) * cw;
      out[1] = (0.5 - cy / cwv * 0.5) * ch;
      return true;
    }
    var scr = [0, 0];
    /* a fish's swim height (three Y): from this snapshot's depths when it has
     * them, so picking and the rings never lag a frame behind fillFish */
    function fishY(s, i) {
      if (s.depth) return -s.depth[i];
      return heights && i < heights.length ? heights[i] : 0;
    }
    /* index of the fish whose head is nearest a canvas point (CSS px), or -1 */
    function pickAt(s, px, py, reach) {
      if (!s || !s.n) return -1;
      var best = -1, bestD2 = reach * reach;
      for (var i = 0; i < s.n; i++) {
        if (!toScreen(s.segs[12 * i] - halfW, fishY(s, i), s.segs[12 * i + 1] - halfH, scr)) continue;
        var dx = scr[0] - px, dy = scr[1] - py, d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best = i; }
      }
      return best;
    }

    var hoverPt = null, hoverIdx = -1, lastS = null, lastT = 0;
    canvas.addEventListener("pointermove", function (e) {
      if (e.pointerType !== "mouse") { hoverPt = null; return; }
      var r = canvas.getBoundingClientRect();
      hoverPt = [e.clientX - r.left, e.clientY - r.top];
    });
    canvas.addEventListener("pointerleave", function () { hoverPt = null; });

    function render(s, info) {
      if (lost) return;
      var now = performance.now();
      var dt = lastT ? Math.min(0.1, (now - lastT) / 1000) : 0.016;
      lastT = now;
      lastS = s;
      common.uTime.value = info.animT;
      stepAnim(dt);
      var sel = info.selIdx;
      if (follow) {
        if (sel < 0) { follow = false; setFollowControls(false); onFollow(false); }
        else {
          /* the target follows the fish's depth too */
          var tx = s.segs[12 * sel] - halfW, tz = s.segs[12 * sel + 1] - halfH, ty = fishY(s, sel);
          var t = controls.target, jump = Math.abs(tx - t.x) > halfW || Math.abs(tz - t.z) > halfH;
          var kk = jump ? 1 : 1 - Math.exp(-dt * 5);
          var dx = (tx - t.x) * kk, dz = (tz - t.z) * kk, dy = (ty - t.y) * (1 - Math.exp(-dt * 5));
          t.x += dx; t.z += dz; t.y += dy;
          camera.position.x += dx; camera.position.z += dz; camera.position.y += dy;
          /* ease in to a close-up on the first frames of a follow */
          var off = camera.position.clone().sub(t), dist = off.length(), want = Math.min(fitDist * 0.32, 420);
          if (dist > want * 1.02 && followDive) camera.position.copy(t).addScaledVector(off, (dist + (want - dist) * (1 - Math.exp(-dt * 3))) / dist);
          else followDive = false;
        }
      }
      controls.update(dt);
      clampView();   /* after follow, damping and animation alike */
      /* near/far follow the zoom, so a close-up keeps its depth precision and
       * a far view never clips the tank */
      var cd = camera.position.distanceTo(controls.target);
      var nearW = Math.max(1, cd * 0.02), farW = cd * 4 + Math.max(W, H) * 4;
      if (Math.abs(camera.near - nearW) > nearW * 0.2 || Math.abs(camera.far - farW) > farW * 0.2) {
        camera.near = nearW; camera.far = farW;
        camera.updateProjectionMatrix();
      }
      updateVP();
      hoverIdx = hoverPt ? pickAt(s, hoverPt[0], hoverPt[1], 16) : -1;
      if (hoverIdx === sel) hoverIdx = -1;
      canvas.style.cursor = hoverIdx >= 0 ? "pointer" : "";
      fillFish(s, sel, hoverIdx);
      fillPreds(s);
      fillFood(s);
      fillCurrents(s, info.dtSim);
      fillTrail(s, sel, info.dtSim > 0);
      if (hoverIdx >= 0) {
        hoverRing.visible = true;
        hoverRing.position.set(s.segs[12 * hoverIdx] - halfW, fishY(s, hoverIdx) - 1.5, s.segs[12 * hoverIdx + 1] - halfH);
        hoverRing.scale.set(34, 34, 1);
      } else hoverRing.visible = false;
      snow.visible = quality >= 3 && !reduced;
      if (causticFade < 1 && causticReady) {
        causticFade = Math.min(1, causticFade + dt / 1.4);
        floorMat.uniforms.uCausticAmt.value = 0.5 * (1 - Math.pow(1 - causticFade, 3));   /* strong enough to read on the sand */
      }
      aimBeam(s, sel, dt);
      renderer.render(scene, camera);
    }
    var followDive = false;

    /* The flashlight (viewer only; BEAM_FN). On, a cone of clear light from
     * the camera: at the followed fish while following (a circle about 70
     * units across it, more from far away), else at the point the camera
     * looks at (the orbit centre; a ~11 degree cone). It reaches past its
     * target by the tank's depth, so the sand behind a fish near the floor
     * lights up too. Fades in and out over ~0.3 s. */
    var beamWant = false, beamAmt = 0;
    var beamTgt = new THREE.Vector3(), beamDir = new THREE.Vector3();
    function aimBeam(s, sel, dt) {
      var goal = beamWant ? 1 : 0;
      beamAmt = reduced ? goal : beamAmt + (goal - beamAmt) * (1 - Math.exp(-dt * 10));
      if (Math.abs(beamAmt - goal) < 0.002) beamAmt = goal;
      common.uBeamOn.value = beamAmt;
      if (beamAmt <= 0) return;
      var onFish = follow && sel >= 0 && s && sel < s.n;
      if (onFish) beamTgt.set(s.segs[12 * sel] - halfW, fishY(s, sel), s.segs[12 * sel + 1] - halfH);
      else beamTgt.copy(controls.target);
      beamDir.copy(beamTgt).sub(camera.position);
      var dist = beamDir.length();
      if (!(dist > 1e-3)) return;
      beamDir.multiplyScalar(1 / dist);
      common.uBeamDir.value.copy(beamDir);
      common.uBeamTan.value = Math.max(70, dist * (onFish ? 0.12 : 0.2)) / dist;
      common.uBeamReach.value = dist + TD * 1.2;
    }
    /* The caustic pattern costs ~40 ms of JS on a laptop (more on a phone), so
     * it is built after the first frames, in idle time, and the light fades in. */
    var causticReady = false, causticFade = 0;
    (window.requestIdleCallback || function (f) { return setTimeout(f, 300); })(function () {
      floorMat.uniforms.uCaustic.value.dispose();
      floorMat.uniforms.uCaustic.value = causticTexture(256);
      causticReady = true;
    }, { timeout: 1200 });

    function setQuality(q) {
      q = Math.max(1, Math.min(3, q | 0));
      if (q === quality) return false;
      quality = q;
      resize();
      return true;
    }

    resize();
    setWorld(W, H);

    /* the engine's light field {on, L0, k} (oc_light_*): with on, the fish /
     * food depth shading follows L(d) = L0·exp(−k·d); off = the preview's */
    function setLight(l) {
      var on = !!(l && l.on);
      common.uLightOn.value = on ? 1 : 0;
      common.uL0.value = on && isFinite(l.L0) ? +l.L0 : 1;
      common.uKWater.value = on && isFinite(l.k) ? +l.k : 0;
    }

    return {
      renderer: "three.js (WebGL2)",
      setWorld: setWorld,
      setLight: setLight,
      light: function () { return { on: common.uLightOn.value > 0.5, L0: common.uL0.value, k: common.uKWater.value }; },
      resize: resize,
      render: render,
      resetView: function (animated) { resetView(animated); },
      setInsets: function (ins) {
        var changed = ["top", "right", "bottom", "left"].some(function (k) { return (ins[k] || 0) !== insets[k]; });
        if (!changed) return;
        insets = { top: ins.top || 0, right: ins.right || 0, bottom: ins.bottom || 0, left: ins.left || 0 };
        applyViewOffset();
        if (!userMoved && !follow) resetView(true);
      },
      /* index of the fish nearest a viewport point (client px), or -1 */
      pick: function (s, clientX, clientY, reach) {
        var r = canvas.getBoundingClientRect();
        updateVP();
        return pickAt(s || lastS, clientX - r.left, clientY - r.top, reach);
      },
      /* client px of a fish's head, or null when it is behind the camera */
      project: function (s, idx) {
        if (!s || idx < 0 || idx >= s.n) return null;
        var r = canvas.getBoundingClientRect();
        if (!toScreen(s.segs[12 * idx] - halfW, fishY(s, idx), s.segs[12 * idx + 1] - halfH, scr)) return null;
        return { x: r.left + scr[0], y: r.top + scr[1] };
      },
      setFollow: function (on) {
        on = !!on;
        if (on === follow) return;
        follow = on;
        followDive = on;
        anim = null;
        userMoved = true;
        setFollowControls(on);
        onFollow(on);
      },
      following: function () { return follow; },
      depth: function () { return TD; },
      /* the view up (+) or down (-) by dy sim units, kept between just above
       * the floor and a bit above the surface; stops a follow */
      moveView: moveView,
      /* put the camera at pos looking at target (three coords, [x, y, z]) */
      lookFrom: function (pos, target) {
        if (follow) { follow = false; setFollowControls(false); onFollow(false); }
        anim = null; userMoved = true;
        controls.target.set(target[0], target[1], target[2]);
        camera.position.set(pos[0], pos[1], pos[2]);
        clampView();
        camera.lookAt(controls.target);
        controls.update();
      },
      /* where the camera is: {pos, target, polar}, three coords */
      view: function () {
        return { pos: camera.position.toArray(), target: controls.target.toArray(), polar: controls.getPolarAngle() };
      },
      quality: function () { return quality; },
      setQuality: setQuality,
      /* one step down: fewer pixels, then no shadows. false at the floor */
      degrade: function () { return setQuality(quality - 1); },
      setReducedMotion: function (on) { reduced = !!on; fishUniforms.uWag.value = reduced ? 0 : 1; },
      /* the flashlight: clear light on the followed fish, or where the camera
       * looks (aimBeam). Viewer only: nothing here reaches the simulation. */
      setFlashlight: function (on) { beamWant = !!on; },
      flashlight: function () { return beamWant; },
      flashlightLevel: function () { return beamAmt; },
      zoomBy: function (f) {
        var t = controls.target, off = camera.position.clone().sub(t);
        var d = Math.max(controls.minDistance, Math.min(controls.maxDistance, off.length() * f));
        camera.position.copy(t).addScaledVector(off.normalize(), d);
        userMoved = true;
      }
    };
  }

  window.EvoOceanScene = { load: load, create: create };
})();
