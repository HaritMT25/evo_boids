"use strict";
/* evo_boids dashboards: a small hand-rolled canvas chart kit (no third-party
 * code). Every chart is a canvas in a box, drawn in CSS px at
 * devicePixelRatio, with a hover (or tap) readout in one shared tooltip.
 *
 * Look, after the data-viz rules this page follows: 2 px lines, hairline solid
 * grid, one y-axis per chart (two measures of different scale get two
 * charts), a legend whenever there are two or more series, categorical colours
 * in a fixed order validated for colour-blind separation on this page's dark
 * surface (#3987e5 ... #e66767: worst adjacent CVD dE 8.4, normal 19.3, all
 * >= 3:1), one-hue blue ramp for magnitudes, text in ink colours only. */
(function () {
  var T = {
    surface: "#101c17", ink: "#dfe9e3", ink2: "#a9bdb1", muted: "#7f958a",
    grid: "#1d2c25", axis: "#33473d",
    series: ["#3987e5", "#d95926", "#199e70", "#c98500", "#d55181", "#008300", "#9085e9", "#e66767"],
    other: "#3f5048",
    /* sequential, dark mode: near the surface = none, light = most */
    seq: ["#13241c", "#104281", "#1c5cab", "#2a78d6", "#5598e7", "#86b6ef", "#cde2fb"]
  };
  var FONT = "11px system-ui, -apple-system, 'Segoe UI', sans-serif";
  var PAD = { l: 46, r: 14, t: 10, b: 24 };

  /* ---- formatting ---- */
  function fmtNum(v) {
    if (!Number.isFinite(v)) return "–";
    var a = Math.abs(v);
    if (a >= 1e6) return (v / 1e6).toFixed(a >= 1e7 ? 0 : 1) + "M";
    if (a >= 1e4) return Math.round(v / 1e3) + "K";
    if (a >= 1000) return (v / 1e3).toFixed(1) + "K";
    if (a >= 100 || Number.isInteger(v)) return String(Math.round(v));
    if (a >= 10) return v.toFixed(1);
    if (a === 0) return "0";
    return v.toFixed(2);
  }
  function fmtTime(s) {
    s = Math.max(0, Math.round(s));
    if (s < 60) return s + " s";
    var h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, ss = s % 60;
    var mm = h ? String(m).padStart(2, "0") : String(m);
    return (h ? h + ":" : "") + mm + ":" + String(ss).padStart(2, "0");
  }
  function niceStep(span, n) {
    var raw = span / Math.max(1, n), p = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / p;
    return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10) * p;
  }
  function ticks(lo, hi, n) {
    if (!(hi > lo)) return [lo];
    var st = niceStep(hi - lo, n), out = [];
    for (var v = Math.ceil(lo / st - 1e-9) * st; v <= hi + st * 1e-9; v += st) out.push(+v.toPrecision(12));
    return out;
  }
  var TIME_STEPS = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400];
  function timeTicks(lo, hi, n) {
    var st = TIME_STEPS[TIME_STEPS.length - 1];
    for (var i = 0; i < TIME_STEPS.length; i++) if ((hi - lo) / TIME_STEPS[i] <= n) { st = TIME_STEPS[i]; break; }
    var out = [];
    for (var v = Math.ceil(lo / st) * st; v <= hi; v += st) out.push(v);
    return out;
  }
  function hexRgb(hex) {
    var n = parseInt(hex.slice(1), 16);
    return [n >> 16 & 255, n >> 8 & 255, n & 255];
  }
  var SEQ = T.seq.map(hexRgb);
  function seqRgb(v, out) {   /* 0..1 -> [r, g, b] on the blue ramp */
    v = Math.max(0, Math.min(1, v || 0)) * (SEQ.length - 1);
    var i = Math.min(SEQ.length - 2, Math.floor(v)), f = v - i, a = SEQ[i], b = SEQ[i + 1];
    out = out || [0, 0, 0];
    out[0] = Math.round(a[0] + (b[0] - a[0]) * f);
    out[1] = Math.round(a[1] + (b[1] - a[1]) * f);
    out[2] = Math.round(a[2] + (b[2] - a[2]) * f);
    return out;
  }
  function seq(v) { var c = seqRgb(v); return "rgb(" + c[0] + "," + c[1] + "," + c[2] + ")"; }
  /* a rows x cols matrix as an image, one pixel per cell, for drawing scaled
   * (thousands of cells: far cheaper than a rectangle each) */
  function matrixImage(rows, cols, v) {
    var cv = document.createElement("canvas");
    cv.width = cols; cv.height = rows;
    var cx = cv.getContext("2d"), img = cx.createImageData(cols, rows), px = img.data, c = [0, 0, 0];
    for (var r = 0; r < rows; r++) {
      for (var k = 0; k < cols; k++) {
        seqRgb(v(r, k), c);
        var o = (r * cols + k) * 4;
        px[o] = c[0]; px[o + 1] = c[1]; px[o + 2] = c[2]; px[o + 3] = 255;
      }
    }
    cx.putImageData(img, 0, 0);
    return cv;
  }
  function drawMatrix(g, img, x, y, w, h) {
    g.imageSmoothingEnabled = false;
    g.drawImage(img, x, y, w, h);
    g.imageSmoothingEnabled = true;
  }
  function alpha(hex, a) {
    var c = hexRgb(hex);
    return "rgba(" + c[0] + "," + c[1] + "," + c[2] + "," + a + ")";
  }

  /* ---- the shared tooltip ---- */
  var tipEl = null;
  function tip(html, cx, cy) {
    if (!tipEl) {
      tipEl = document.createElement("div");
      tipEl.className = "charttip";
      document.body.appendChild(tipEl);
    }
    if (!html) { tipEl.style.display = "none"; return; }
    tipEl.innerHTML = html;
    tipEl.style.display = "block";
    var w = tipEl.offsetWidth, h = tipEl.offsetHeight;
    var x = cx + 14, y = cy + 14;
    if (x + w > innerWidth - 6) x = cx - 14 - w;
    if (y + h > innerHeight - 6) y = cy - 14 - h;
    tipEl.style.left = Math.max(6, x) + "px";
    tipEl.style.top = Math.max(6, y) + "px";
  }
  function esc(s) {
    return String(s).replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; });
  }
  /* one tooltip line: a colour key, then text in ink */
  function tipRow(color, label, value, square) {
    return "<div class='tr'><i style='background:" + color + (square ? "" : ";height:3px") + "'></i>" +
           "<span>" + esc(label) + "</span><b>" + esc(value) + "</b></div>";
  }

  /* ---- a chart: canvas in a box; render(g, w, h, data, hover) -> tooltip html ---- */
  function Chart(box, render) {
    this.box = box;
    this.render = render;
    this.data = null;
    this.hover = null;
    var c = this.canvas = document.createElement("canvas");
    box.appendChild(c);
    this.g = c.getContext("2d");
    var self = this;
    function at(e) {
      var r = c.getBoundingClientRect();
      self.hover = { x: e.clientX - r.left, y: e.clientY - r.top, cx: e.clientX, cy: e.clientY };
      self.draw();
    }
    c.addEventListener("pointermove", at);
    c.addEventListener("pointerdown", at);
    c.addEventListener("pointerleave", function (e) {
      if (e.pointerType !== "mouse") return;   /* a tap keeps its readout until the next tap */
      self.hover = null; tip(null); self.draw();
    });
  }
  Chart.prototype.draw = function (data) {
    if (data !== undefined) this.data = data;
    var w = this.box.clientWidth, h = this.box.clientHeight;
    if (w < 20 || h < 20) return;
    var dpr = window.devicePixelRatio || 1, c = this.canvas;
    var bw = Math.round(w * dpr), bh = Math.round(h * dpr);
    if (c.width !== bw) c.width = bw;
    if (c.height !== bh) c.height = bh;
    var g = this.g;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    g.font = FONT;
    if (!this.data) return;
    var html = this.render(g, w, h, this.data, this.hover);
    if (this.hover) tip(html || null, this.hover.cx, this.hover.cy);
  };
  Chart.prototype.clearHover = function () { this.hover = null; };

  /* ---- axes ---- */
  /* frame: grid, y ticks and labels, x time ticks. Returns scale functions. */
  function frame(g, w, h, x0, x1, y0, y1, o) {
    o = o || {};
    var pl = o.padL || PAD.l, L = pl, R = w - PAD.r, Tp = PAD.t, B = h - PAD.b;
    if (!(x1 > x0)) x1 = x0 + 1;
    if (!(y1 > y0)) y1 = y0 + 1;
    var sx = function (v) { return L + (v - x0) / (x1 - x0) * (R - L); };
    var sy = function (v) { return B - (v - y0) / (y1 - y0) * (B - Tp); };
    g.lineWidth = 1;
    g.fillStyle = T.muted;
    g.textAlign = "right";
    g.textBaseline = "middle";
    var yt = o.yTicks || ticks(y0, y1, Math.max(2, Math.floor((B - Tp) / 34)));
    yt.forEach(function (v) {
      var y = Math.round(sy(v)) + 0.5;
      g.strokeStyle = T.grid;
      g.beginPath(); g.moveTo(L, y); g.lineTo(R, y); g.stroke();
      g.fillText((o.yFmt || fmtNum)(v), L - 6, y);
    });
    g.strokeStyle = T.axis;
    g.beginPath(); g.moveTo(L, Math.round(B) + 0.5); g.lineTo(R, Math.round(B) + 0.5); g.stroke();
    g.textAlign = "center";
    g.textBaseline = "top";
    var xt = o.xTicks || (o.xFmt ? ticks(x0, x1, Math.max(2, Math.floor((R - L) / 70)))
                                  : timeTicks(x0, x1, Math.max(2, Math.floor((R - L) / 70))));
    xt.forEach(function (v) {
      var x = sx(v);
      if (x < L - 1 || x > R + 1) return;
      g.fillText((o.xFmt || fmtTime)(v), Math.max(L + 14, Math.min(R - 14, x)), B + 6);
    });
    return { sx: sx, sy: sy, L: L, R: R, T: Tp, B: B, x0: x0, x1: x1, y0: y0, y1: y1 };
  }
  function nearestIndex(t, x) {   /* t ascending */
    var lo = 0, hi = t.length - 1;
    if (hi < 0) return -1;
    while (hi - lo > 1) { var mid = (lo + hi) >> 1; if (t[mid] < x) lo = mid; else hi = mid; }
    return Math.abs(t[lo] - x) <= Math.abs(t[hi] - x) ? lo : hi;
  }
  function range(arrs, zero) {
    var lo = zero ? 0 : Infinity, hi = zero ? 0 : -Infinity;
    arrs.forEach(function (a) {
      for (var i = 0; i < a.length; i++) if (Number.isFinite(a[i])) { if (a[i] < lo) lo = a[i]; if (a[i] > hi) hi = a[i]; }
    });
    if (!Number.isFinite(lo)) { lo = 0; hi = 1; }
    if (hi === lo) { hi = lo + (Math.abs(lo) || 1) * 0.5; if (!zero) lo -= (Math.abs(lo) || 1) * 0.5; }
    else if (!zero) { var pad = (hi - lo) * 0.06; lo -= pad; hi += pad; }
    else hi += (hi - lo) * 0.06;
    return [lo, hi];
  }
  function dot(g, x, y, color, r) {
    g.beginPath(); g.arc(x, y, (r || 4) + 2, 0, 6.2832); g.fillStyle = T.surface; g.fill();
    g.beginPath(); g.arc(x, y, r || 4, 0, 6.2832); g.fillStyle = color; g.fill();
  }

  /* ---- line chart over time ----
   * d: { t: [s], series: [{name, y, color}], bands: [{lo, hi, color}],
   *      yMin, yMax, zero (axis from 0), yFmt, valFmt } */
  function lines(g, w, h, d, hover) {
    if (d.t.length < 1) return empty(g, w, h, d.empty);
    var arrs = d.series.map(function (s) { return s.y; });
    (d.bands || []).forEach(function (b) { arrs.push(b.lo, b.hi); });
    var yr = range(arrs, d.zero !== false);
    if (d.yMin !== undefined) yr[0] = d.yMin;
    if (d.yMax !== undefined) yr[1] = d.yMax;
    var f = frame(g, w, h, d.t[0], d.t[d.t.length - 1], yr[0], yr[1], { yFmt: d.yFmt });
    (d.bands || []).forEach(function (b) {
      g.beginPath();
      for (var i = 0; i < d.t.length; i++) g.lineTo(f.sx(d.t[i]), f.sy(b.hi[i]));
      for (i = d.t.length - 1; i >= 0; i--) g.lineTo(f.sx(d.t[i]), f.sy(b.lo[i]));
      g.closePath();
      g.fillStyle = alpha(b.color, b.alpha || 0.18);
      g.fill();
    });
    g.lineJoin = "round"; g.lineCap = "round";
    d.series.forEach(function (s) {
      g.strokeStyle = s.color; g.lineWidth = s.width || 2;
      g.beginPath();
      var on = false;
      for (var i = 0; i < d.t.length; i++) {
        var v = s.y[i];
        if (!Number.isFinite(v)) { on = false; continue; }
        if (on) g.lineTo(f.sx(d.t[i]), f.sy(v)); else { g.moveTo(f.sx(d.t[i]), f.sy(v)); on = true; }
      }
      g.stroke();
    });
    if (d.t.length === 1) d.series.forEach(function (s) { dot(g, f.sx(d.t[0]), f.sy(s.y[0]), s.color); });
    if (!hover || hover.x < f.L - 4 || hover.x > f.R + 4) return null;
    var k = nearestIndex(d.t, d.t[0] + (hover.x - f.L) / (f.R - f.L) * (d.t[d.t.length - 1] - d.t[0]));
    var x = f.sx(d.t[k]);
    g.strokeStyle = T.axis; g.lineWidth = 1;
    g.beginPath(); g.moveTo(Math.round(x) + 0.5, f.T); g.lineTo(Math.round(x) + 0.5, f.B); g.stroke();
    var html = "<div class='th'>" + fmtTime(d.t[k]) + "</div>";
    (d.bands || []).forEach(function (b) {
      if (b.label) html += tipRow(alpha(b.color, 0.5), b.label, (d.valFmt || fmtNum)(b.lo[k]) + " – " + (d.valFmt || fmtNum)(b.hi[k]), true);
    });
    d.series.forEach(function (s) {
      if (!Number.isFinite(s.y[k])) return;
      dot(g, x, f.sy(s.y[k]), s.color);
      html += tipRow(s.color, s.name, (d.valFmt || fmtNum)(s.y[k]));
    });
    return html;
  }

  /* ---- stacked areas over time ----
   * d: { t, layers: [{name, y, color}] bottom to top; shares 0..1 (Muller
   *      style), or abs: true for plain amounts stacked } */
  function stack(g, w, h, d, hover) {
    if (d.t.length < 2) return empty(g, w, h, d.empty);
    var n = d.t.length, f, cap = 1;
    if (d.abs) {
      var tot = 0;
      for (var j = 0; j < n; j++) {
        var s = 0;
        d.layers.forEach(function (ly) { s += ly.y[j] || 0; });
        if (s > tot) tot = s;
      }
      cap = Infinity;
      f = frame(g, w, h, d.t[0], d.t[n - 1], 0, (tot || 1) * 1.06);
    } else {
      f = frame(g, w, h, d.t[0], d.t[n - 1], 0, 1,
                { yTicks: [0, 0.25, 0.5, 0.75, 1], yFmt: function (v) { return Math.round(v * 100) + "%"; } });
    }
    var base = new Float64Array(n);
    d.layers.forEach(function (ly) {
      var top = new Float64Array(n);
      for (var i = 0; i < n; i++) top[i] = Math.min(cap, base[i] + (ly.y[i] || 0));
      g.beginPath();
      for (i = 0; i < n; i++) g.lineTo(f.sx(d.t[i]), f.sy(top[i]));
      for (i = n - 1; i >= 0; i--) g.lineTo(f.sx(d.t[i]), f.sy(base[i]));
      g.closePath();
      g.fillStyle = ly.color; g.fill();
      /* the surface gap between neighbouring bands */
      g.strokeStyle = T.surface; g.lineWidth = 1;
      g.beginPath();
      for (i = 0; i < n; i++) g.lineTo(f.sx(d.t[i]), f.sy(top[i]));
      g.stroke();
      base = top;
    });
    if (!hover || hover.x < f.L - 4 || hover.x > f.R + 4) return null;
    var k = nearestIndex(d.t, d.t[0] + (hover.x - f.L) / (f.R - f.L) * (d.t[n - 1] - d.t[0]));
    var x = f.sx(d.t[k]);
    g.strokeStyle = T.ink; g.globalAlpha = 0.6; g.lineWidth = 1;
    g.beginPath(); g.moveTo(Math.round(x) + 0.5, f.T); g.lineTo(Math.round(x) + 0.5, f.B); g.stroke();
    g.globalAlpha = 1;
    var html = "<div class='th'>" + fmtTime(d.t[k]) + "</div>";
    d.layers.slice().reverse().forEach(function (ly) {
      var v = ly.y[k] || 0;
      if (d.abs) html += tipRow(ly.color, ly.name, fmtNum(v), true);
      else if (v > 0.0005) html += tipRow(ly.color, ly.name, (v * 100).toFixed(1) + "%", true);
    });
    return html;
  }

  /* ---- a path through two quantities (phase plot) ----
   * d: { x, y, t, xName, yName, color }: older steps fade, the latest is a dot */
  function phase(g, w, h, d, hover) {
    if (d.x.length < 2) return empty(g, w, h, d.empty);
    var xr = range([d.x], true), yr = range([d.y], true);
    var f = frame(g, w, h, xr[0], xr[1], yr[0], yr[1], { xFmt: fmtNum });
    var n = d.x.length;
    g.lineWidth = 2; g.lineCap = "round";
    for (var i = 1; i < n; i++) {
      g.strokeStyle = alpha(d.color, (0.12 + 0.88 * Math.pow(i / (n - 1), 2)).toFixed(3));
      g.beginPath(); g.moveTo(f.sx(d.x[i - 1]), f.sy(d.y[i - 1])); g.lineTo(f.sx(d.x[i]), f.sy(d.y[i])); g.stroke();
    }
    dot(g, f.sx(d.x[n - 1]), f.sy(d.y[n - 1]), d.color);
    /* axis names in the top-right corner, clear of the path's usual start */
    g.fillStyle = T.muted; g.textAlign = "right"; g.textBaseline = "top";
    g.fillText("↑ " + d.yName + "   " + d.xName + " →", f.R, f.T + 2);
    if (!hover) return null;
    var best = -1, bd = 24 * 24;
    for (i = 0; i < n; i++) {
      var dx = f.sx(d.x[i]) - hover.x, dy = f.sy(d.y[i]) - hover.y, dd = dx * dx + dy * dy;
      if (dd <= bd) { bd = dd; best = i; }
    }
    if (best < 0) return null;
    dot(g, f.sx(d.x[best]), f.sy(d.y[best]), T.ink);
    return "<div class='th'>" + fmtTime(d.t[best]) + "</div>" +
           tipRow(d.color, d.xName, fmtNum(d.x[best])) + tipRow(d.color, d.yName, fmtNum(d.y[best]));
  }

  /* ---- heatmap ----
   * d: { rows, cols, v(r, c) 0..1, rowLabel(r), colLabel(c), labelW, labelH,
   *      bottomH (room under it), cellTip(r, c) } */
  function heat(g, w, h, d, hover) {
    var L = d.labelW || 0, Tp = d.labelH || 0, R = w - 6, B = h - (d.bottomH || 4);
    var cw = (R - L) / d.cols, ch = (B - Tp) / d.rows, r, c;
    if (!d._img) d._img = matrixImage(d.rows, d.cols, d.v);   /* kept for hover redraws */
    drawMatrix(g, d._img, L, Tp, R - L, B - Tp);
    g.fillStyle = T.ink2;
    if (d.rowLabel && ch >= 7) {
      g.textAlign = "right"; g.textBaseline = "middle";
      g.font = (ch >= 11 ? 11 : Math.max(8, Math.floor(ch))) + "px system-ui, sans-serif";
      for (r = 0; r < d.rows; r++) g.fillText(d.rowLabel(r), L - 5, Tp + (r + 0.5) * ch);
    }
    if (d.colLabel && Tp) {
      g.font = FONT;
      g.textAlign = "left"; g.textBaseline = "middle";
      for (c = 0; c < d.cols; c++) {
        g.save();
        g.translate(L + (c + 0.5) * cw, Tp - 5);
        g.rotate(-Math.PI / 3);
        g.fillText(d.colLabel(c), 0, 0);
        g.restore();
      }
    }
    g.font = FONT;
    if (!hover) return null;
    c = Math.floor((hover.x - L) / cw); r = Math.floor((hover.y - Tp) / ch);
    if (c < 0 || r < 0 || c >= d.cols || r >= d.rows) return null;
    g.strokeStyle = T.ink; g.lineWidth = 1.5;
    g.strokeRect(L + c * cw + 0.75, Tp + r * ch + 0.75, Math.max(1, cw - 1.5), Math.max(1, ch - 1.5));
    return d.cellTip(r, c);
  }

  /* ---- histogram ----
   * d: { lo, width, counts, color, xName, marker: {x, label} } */
  function hist(g, w, h, d, hover) {
    var n = d.counts.length, max = Math.max.apply(null, d.counts.concat([1]));
    var f = frame(g, w, h, d.lo, d.lo + n * d.width, 0, max * 1.08, { xFmt: fmtNum });
    var bw = (f.R - f.L) / n;
    for (var i = 0; i < n; i++) {
      if (!d.counts[i]) continue;
      var x = f.sx(d.lo + i * d.width) + 1, y = f.sy(d.counts[i]), wd = Math.min(24, Math.max(1, bw - 2));
      x += (bw - 2 - wd) / 2;
      roundTop(g, x, y, wd, f.B - y, Math.min(4, wd / 2, f.B - y));
      g.fillStyle = d.color; g.fill();
    }
    if (d.marker && d.marker.x >= f.x0 && d.marker.x <= f.x1) {
      var mx = Math.round(f.sx(d.marker.x)) + 0.5;
      g.strokeStyle = T.ink; g.lineWidth = 1.5;
      g.beginPath(); g.moveTo(mx, f.T); g.lineTo(mx, f.B); g.stroke();
      g.fillStyle = T.ink2; g.textAlign = mx > (f.L + f.R) / 2 ? "right" : "left"; g.textBaseline = "top";
      g.fillText(d.marker.label, mx + (g.textAlign === "right" ? -5 : 5), f.T + 2);
    }
    g.fillStyle = T.muted; g.textAlign = "right"; g.textBaseline = "bottom";
    g.fillText(d.xName + " →", f.R, f.T + 12);
    if (!hover) return null;
    i = Math.floor((hover.x - f.L) / bw);
    if (i < 0 || i >= n) return null;
    return "<div class='th'>" + esc(d.xName) + " " + fmtNum(d.lo + i * d.width) + "–" + fmtNum(d.lo + (i + 1) * d.width) + "</div>" +
           tipRow(d.color, d.countName || "pairs", fmtNum(d.counts[i]), true);
  }
  function roundTop(g, x, y, w, h, r) {
    g.beginPath();
    g.moveTo(x, y + h); g.lineTo(x, y + r);
    g.arcTo(x, y, x + r, y, r); g.lineTo(x + w - r, y);
    g.arcTo(x + w, y, x + w, y + r, r); g.lineTo(x + w, y + h);
    g.closePath();
  }

  /* ---- scatter ----
   * d: { x, y, color(i) css, tip(i) html } (axes hidden: a map, not a measure) */
  function scatter(g, w, h, d, hover) {
    var n = d.x.length;
    if (!n) return empty(g, w, h, d.empty);
    var xr = range([d.x], false), yr = range([d.y], false);
    /* same scale on both axes, so distances read true */
    var L = 10, R = w - 10, Tp = 10, B = h - 10;
    var k = Math.min((R - L) / (xr[1] - xr[0]), (B - Tp) / (yr[1] - yr[0]));
    var cx = (L + R) / 2, cy = (Tp + B) / 2, mx = (xr[0] + xr[1]) / 2, my = (yr[0] + yr[1]) / 2;
    var sx = function (v) { return cx + (v - mx) * k; }, sy = function (v) { return cy - (v - my) * k; };
    g.strokeStyle = T.grid; g.lineWidth = 1;
    g.strokeRect(0.5, 0.5, w - 1, h - 1);
    for (var i = 0; i < n; i++) dot(g, sx(d.x[i]), sy(d.y[i]), d.color(i), 3.5);
    if (!hover) return null;
    var best = -1, bd = 24 * 24;
    for (i = 0; i < n; i++) {
      var dx = sx(d.x[i]) - hover.x, dy = sy(d.y[i]) - hover.y, dd = dx * dx + dy * dy;
      if (dd <= bd) { bd = dd; best = i; }
    }
    if (best < 0) return null;
    g.strokeStyle = T.ink; g.lineWidth = 1.5;
    g.beginPath(); g.arc(sx(d.x[best]), sy(d.y[best]), 7, 0, 6.2832); g.stroke();
    return d.tip(best);
  }

  function empty(g, w, h, msg) {
    g.fillStyle = T.muted; g.textAlign = "center"; g.textBaseline = "middle";
    g.fillText(msg || "collecting data…", w / 2, h / 2);
    return null;
  }

  /* a legend: [{name, color, square, value}] into a DOM element */
  function legend(el, items) {
    var html = items.map(function (it) {
      return "<span class='li'><i style='background:" + it.color + (it.square ? "" : ";height:3px") + "'></i>" +
             esc(it.name) + (it.value !== undefined ? " <b>" + esc(it.value) + "</b>" : "") + "</span>";
    }).join("");
    if (el.__html !== html) { el.innerHTML = html; el.__html = html; }
  }

  window.EvoCharts = {
    T: T, Chart: Chart, frame: frame, lines: lines, stack: stack, phase: phase, heat: heat, hist: hist,
    scatter: scatter, empty: empty, legend: legend, tip: tip, tipRow: tipRow, esc: esc, dot: dot,
    fmtNum: fmtNum, fmtTime: fmtTime, seq: seq, alpha: alpha, ticks: ticks, timeTicks: timeTicks,
    nearestIndex: nearestIndex, matrixImage: matrixImage, drawMatrix: drawMatrix
  };
})();
