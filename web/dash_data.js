"use strict";
/* evo_boids dashboards: the data side. No page access, so Node can load it
 * (require it, then use globalThis.EvoDashData) and the tests in
 * ~/wasm_parity/dash can drive it with the wasm recorder's real output.
 *
 *   History    the recorder's sample rows (wasm/evo_wasm.cpp, "Dashboard
 *              recorder"), kept at bounded size: when full, every other row
 *              goes and the sampling interval doubles, so a session of any
 *              length fits in a few MB with even coverage of the whole run.
 *   Family     the tree of the live fish through each one's first source (the
 *              parent whose family line, lineageId, it carries), pruned as fish
 *              die so it stays at most about twice the live set.
 *   Lines      stable colours for the family lines worth naming.
 *   analysis   UPGMA, classical MDS, histograms: the similarity tab's maths. */
(function (root) {
  var DT = 0.016;            /* simulated seconds per step (evo_step) */
  var Q = 5, NSENS = 41, TOP = 24;
  /* column names, in RecCol order (wasm/evo_wasm.cpp; change both) */
  var BASE = ["frame", "pop", "food", "pred", "births", "deaths", "kills", "starved", "aged",
              "avgAge", "maxAge", "avgEnergy", "avgSpeed", "avgGlen", "glenStd", "diversity",
              "flowAlign", "predAlign", "neighAlign", "neighCount",
              "predBirths", "predStarved", "predAged", "predEnergy",
              "components", "genMean", "genMax", "lines", "bodySize", "bodyElong", "current", "culled"];
  var COL = {};
  BASE.forEach(function (name, i) { COL[name] = i; });
  ["speed", "glen", "body", "age"].forEach(function (t, k) {
    COL["q_" + t] = BASE.length + k * Q;   /* Q quantiles: 10, 25, 50, 75, 90 % */
  });
  COL.sensors = BASE.length + 4 * Q;
  COL.top = COL.sensors + NSENS;
  var ROW = COL.top + 2 * TOP;

  /* ---- History ---- */
  var CAP = 4096;            /* rows kept; halved when full */
  function History(baseEvery) {
    this.base = baseEvery || 60;   /* the recorder's interval at stride 1 */
    this.clear();
  }
  History.prototype.clear = function () {
    this.data = new Float64Array(CAP * ROW);
    this.n = 0;
    this.stride = 1;
    this.version = 0;          /* bumps on every change, so views redraw only then */
    this.epoch = (this.epoch || 0) + 1;   /* bumps when rows are dropped or renumbered */
  };
  History.prototype.every = function () { return this.base * this.stride; };
  History.prototype.frame = function (i) { return this.data[i * ROW]; };
  History.prototype.get = function (i, c) { return this.data[i * ROW + c]; };
  /* rows: a flat array of whole rows. Returns true when the interval doubled
   * (the caller passes every() on to the recorder). */
  History.prototype.push = function (rows) {
    var doubled = false;
    for (var o = 0; o + ROW <= rows.length; o += ROW) {
      var f = rows[o];
      var last = this.n ? this.frame(this.n - 1) : -1;
      if (f <= last) continue;                     /* a repeat (hand-off) */
      /* rows the recorder took before it heard of a doubling; the first row
       * of a run is kept whatever its frame */
      if (this.n && f % this.every() !== 0) continue;
      if (this.n === CAP) { this.halve(); doubled = true; if (f % this.every() !== 0) continue; }
      this.data.set(rows.subarray ? rows.subarray(o, o + ROW) : rows.slice(o, o + ROW), this.n * ROW);
      this.n++;
    }
    if (rows.length >= ROW) this.version++;
    return doubled;
  };
  History.prototype.halve = function () {
    this.stride *= 2;
    this.epoch++;
    var every = this.every(), w = 0;
    for (var i = 0; i < this.n; i++) {
      /* keep the run's first row too: it is the chart's left edge */
      if (i === 0 || this.frame(i) % every === 0) {
        if (w !== i) this.data.copyWithin(w * ROW, i * ROW, (i + 1) * ROW);
        w++;
      }
    }
    this.n = w;
  };
  /* one column as a plain array */
  History.prototype.col = function (c) {
    var out = new Array(this.n);
    for (var i = 0; i < this.n; i++) out[i] = this.data[i * ROW + c];
    return out;
  };
  History.prototype.times = function () {
    var out = new Array(this.n);
    for (var i = 0; i < this.n; i++) out[i] = this.data[i * ROW] * DT;
    return out;
  };
  /* per-second rate of a cumulative column between neighbouring rows; the
   * first row repeats the second's rate */
  History.prototype.rate = function (c) {
    var out = new Array(this.n);
    for (var i = 1; i < this.n; i++) {
      var dt = (this.frame(i) - this.frame(i - 1)) * DT;
      out[i] = dt > 0 ? Math.max(0, this.get(i, c) - this.get(i - 1, c)) / dt : 0;
    }
    if (this.n) out[0] = this.n > 1 ? out[1] : 0;
    return out;
  };
  /* the top family lines of row i as [[lineageId, count], ...], largest first */
  History.prototype.top = function (i) {
    var out = [];
    for (var t = 0; t < TOP; t++) {
      var cnt = this.get(i, COL.top + 2 * t + 1);
      if (!(cnt > 0)) break;
      out.push([this.get(i, COL.top + 2 * t), cnt]);
    }
    return out;
  };
  /* every row, for Export: header + lines */
  History.prototype.csv = function (sensorNames) {
    var head = ["time_s"].concat(BASE);
    ["speed", "genome_len", "body_size", "age"].forEach(function (t) {
      [10, 25, 50, 75, 90].forEach(function (p) { head.push(t + "_p" + p); });
    });
    for (var s = 0; s < NSENS; s++) head.push("uses_" + (sensorNames && sensorNames[s] || "sensor" + s));
    for (var t = 0; t < TOP; t++) head.push("line" + (t + 1) + "_id", "line" + (t + 1) + "_count");
    var lines = [head.join(",")];
    for (var i = 0; i < this.n; i++) {
      var row = [+(this.frame(i) * DT).toFixed(3)];
      for (var c = 0; c < ROW; c++) {
        var v = this.get(i, c);
        row.push(Number.isInteger(v) ? v : +v.toPrecision(6));
      }
      lines.push(row.join(","));
    }
    return lines.join("\n") + "\n";
  };

  /* ---- Family: the tree through first sources ----
   * A node is a fish that is alive, or a dead one with at least two branches
   * below it that lead to live fish. A dead fish with one such branch is
   * spliced out (its child takes over its place, from = where it branched),
   * one with none is removed, so the tree is bounded by about twice the live
   * set however long the run. Fed in stream order: each batch's births before
   * its changes (a death is reported at the sample after the step it happened). */
  function Family() { this.clear(); }
  Family.prototype.clear = function () {
    this.nodes = new Map();
    this.alive = 0;
    this.version = 0;
    this.unknownSources = 0;   /* newborns whose sources the recorder could not name */
  };
  function node(id, lin, frame) {
    return { id: id, lin: lin, born: frame, from: frame, died: NaN, alive: true, mom: null, kids: [] };
  }
  Family.prototype.add = function (n) { this.nodes.set(n.id, n); this.alive++; return n; };
  /* births: flat rows (id, first source, second source, lineageId, frame) */
  Family.prototype.births = function (rows) {
    for (var o = 0; o + 5 <= rows.length; o += 5) {
      var id = rows[o], a = rows[o + 1], lin = rows[o + 3], frame = rows[o + 4];
      if (this.nodes.has(id)) continue;
      var mom = a ? this.nodes.get(a) : null;
      if (!a) this.unknownSources++;
      else if (!mom) mom = this.add(node(a, lin, frame));   /* not seen before: a root */
      var kid = this.add(node(id, lin || (mom ? mom.lin : id), frame));
      if (mom) { kid.mom = mom; mom.kids.push(kid); }
    }
    if (rows.length) this.version++;
  };
  /* changes: flat rows (kind, id, lineageId, frame); 1 = left, 2 = joined */
  Family.prototype.changes = function (rows) {
    for (var o = 0; o + 4 <= rows.length; o += 4) {
      var kind = rows[o], id = rows[o + 1];
      if (kind === 2) {
        if (!this.nodes.has(id)) this.add(node(id, rows[o + 2] || id, rows[o + 3]));
      } else {
        var n = this.nodes.get(id);
        if (!n || !n.alive) continue;
        n.alive = false;
        n.died = rows[o + 3];
        this.alive--;
        this.prune(n);
      }
    }
    if (rows.length) this.version++;
  };
  Family.prototype.prune = function (n) {
    while (n && !n.alive) {
      var mom = n.mom;
      if (n.kids.length === 0) {
        this.nodes.delete(n.id);
        if (mom) mom.kids.splice(mom.kids.indexOf(n), 1);
        n = mom;
      } else if (n.kids.length === 1) {
        var k = n.kids[0];
        k.mom = mom;
        k.from = n.from;
        if (mom) mom.kids[mom.kids.indexOf(n)] = k;
        this.nodes.delete(n.id);
        return;
      } else {
        return;
      }
    }
  };
  Family.prototype.roots = function () {
    var out = [];
    this.nodes.forEach(function (n) { if (!n.mom) out.push(n); });
    out.sort(function (a, b) { return a.from - b.from || a.id - b.id; });
    return out;
  };
  /* Layout of at most maxTips live fish, spread evenly over the tree, with
   * the branches joining them. A node's line runs from `from` (where it
   * branched off its parent's line) to now (alive) or its death; children
   * hang below it, latest branch first, so no connector crosses a line.
   * Returns { rows: [{node, y, x0, x1, parentRow, tip}], tips, nowFrame }. */
  Family.prototype.layout = function (maxTips, nowFrame) {
    /* every live fish in display order (depth first, children latest-first) */
    var order = [], stack = this.roots().reverse();
    var kidsOf = function (n) { return n.kids.slice().sort(function (a, b) { return b.from - a.from || b.id - a.id; }); };
    while (stack.length) {
      var n = stack.pop();
      if (n.alive) order.push(n);
      var ks = kidsOf(n);
      for (var i = ks.length - 1; i >= 0; i--) stack.push(ks[i]);
    }
    var keep = new Set();
    var step = Math.max(1, order.length / Math.max(1, maxTips));
    var tips = 0;
    for (var t = 0; t < order.length; t += step) {
      var m = order[Math.floor(t)];
      if (keep.has(m)) continue;
      tips++;
      for (; m && !keep.has(m); m = m.mom) keep.add(m);
    }
    /* rows: kept nodes in the same order, skipping dead ones with a single
     * kept child (that child's line starts where theirs did). An explicit
     * stack, since a long run makes deep trees. */
    var rows = [];
    this.roots().forEach(function (r) {
      if (!keep.has(r)) return;
      var st = [[r, -1, r.from]];
      while (st.length) {
        var e = st.pop(), n = e[0], parentRow = e[1], from = e[2];
        var kept = kidsOf(n).filter(function (k) { return keep.has(k); });
        if (!n.alive && kept.length === 1) { st.push([kept[0], parentRow, from]); continue; }
        rows.push({ node: n, x0: from, x1: n.alive ? nowFrame : n.died, parentRow: parentRow,
                    tip: n.alive, y: rows.length });
        var idx = rows.length - 1;
        for (var i = kept.length - 1; i >= 0; i--) st.push([kept[i], idx, kept[i].from]);
      }
    });
    return { rows: rows, tips: tips, liveTotal: order.length, nowFrame: nowFrame };
  };
  /* The most recent fish every live fish descends from (first-source side),
   * or null when the live fish come from more than one root. */
  Family.prototype.commonAncestor = function () {
    var roots = this.roots().filter(function (r) { return r.alive || r.kids.length; });
    if (roots.length !== 1) return null;
    var n = roots[0];
    /* walk down while exactly one branch leads to live fish and n is dead */
    while (!n.alive && n.kids.length === 1) n = n.kids[0];
    return n;
  };
  /* Newick of a layout (branch lengths in simulated seconds), for tree viewers
   * such as FigTree or iTOL. A node's line becomes a chain of splits, one per
   * child, ending in a tip when the fish is alive. */
  Family.prototype.newick = function (lay) {
    var rows = lay.rows, kids = rows.map(function () { return []; });
    rows.forEach(function (r, i) { if (r.parentRow >= 0) kids[r.parentRow].push(i); });
    function sec(f) { return +(f * DT).toFixed(3); }
    /* the line of row i from time t0, with its children from index j on */
    function line(i, j, t0) {
      var r = rows[i], ks = kids[i];
      /* children are latest-first; walk them oldest-first along the line */
      var asc = ks.slice().reverse();
      if (j >= asc.length) {
        return "fish" + r.node.id + ":" + sec(r.x1 - t0);
      }
      var k = rows[asc[j]];
      if (j === asc.length - 1 && !r.tip) return line(asc[j], 0, t0);  /* the line ends in its last child */
      return "(" + line(asc[j], 0, k.x0) + "," + line(i, j + 1, k.x0) + "):" + sec(k.x0 - t0);
    }
    var tops = [];
    rows.forEach(function (r, i) { if (r.parentRow < 0) tops.push(line(i, 0, r.x0)); });
    return (tops.length === 1 ? tops[0].replace(/:[^:()]*$/, "") : "(" + tops.join(",") + ")") + ";\n";
  };

  /* ---- Lines: colours for the family lines worth naming ----
   * A line gets one of `slots` colours the first time its share reaches
   * `minShare`, keeps it while it lives (colour follows the line, never its
   * rank), and frees it once it has been gone from the top list for good.
   * Lines without a colour are drawn together as "other lines". */
  function Lines(slots, minShare) {
    this.slots = slots;
    this.minShare = minShare;
    this.clear();
  }
  Lines.prototype.clear = function () {
    this.slotOf = new Map();       /* lineageId -> slot */
    this.owner = new Array(this.slots).fill(null);
    this.lastSeen = new Map();     /* lineageId -> last row index it was in the top list */
    this.upTo = 0;                 /* rows of History already scanned */
    this.epoch = -1;
  };
  Lines.prototype.update = function (h) {
    if (this.epoch !== h.epoch) { this.clear(); this.epoch = h.epoch; }   /* cleared or halved */
    for (var i = this.upTo; i < h.n; i++) {
      var pop = h.get(i, COL.pop) || 1, top = h.top(i);
      for (var t = 0; t < top.length; t++) {
        var id = top[t][0];
        this.lastSeen.set(id, i);
        if (this.slotOf.has(id) || top[t][1] / pop < this.minShare) continue;
        var s = this.owner.indexOf(null);
        if (s < 0) {
          /* recycle the slot of a line missing from the top list for 20 rows */
          for (var k = 0; k < this.slots && s < 0; k++) {
            var o = this.owner[k];
            if (i - (this.lastSeen.get(o) || 0) > 20) { this.slotOf.delete(o); s = k; }
          }
        }
        if (s < 0) continue;
        this.owner[s] = id;
        this.slotOf.set(id, s);
      }
    }
    this.upTo = h.n;
  };

  /* ---- analysis for the similarity tab ---- */
  /* UPGMA on a dense m x m distance matrix (Float32Array). Returns the leaf
   * order and merges [{a, b, h, size}] (a, b: cluster indices; leaves are
   * 0..m-1, merge k makes cluster m+k). Each row's nearest neighbour is
   * cached and only rescanned when it was merged away, so a step costs
   * about O(m) instead of a full O(m^2) search. */
  function upgma(dist, m) {
    var d = new Float64Array(m * m);
    for (var i = 0; i < m * m; i++) d[i] = dist[i];
    var id = [], size = [], active = [];
    for (i = 0; i < m; i++) { id.push(i); size.push(1); active.push(true); }
    var merges = [], members = [];
    for (i = 0; i < m; i++) members.push([i]);
    var near = new Int32Array(m), nearD = new Float64Array(m);
    function rescan(r) {
      var bd = Infinity, bk = -1;
      for (var k = 0; k < m; k++) if (k !== r && active[k] && d[r * m + k] < bd) { bd = d[r * m + k]; bk = k; }
      near[r] = bk; nearD[r] = bd;
    }
    for (i = 0; i < m; i++) rescan(i);
    for (var step = 0; step < m - 1; step++) {
      var best = Infinity, bi = -1, bj = -1;
      for (i = 0; i < m; i++) {
        if (active[i] && near[i] >= 0 && nearD[i] < best) { best = nearD[i]; bi = i; bj = near[i]; }
      }
      if (bi < 0) break;
      if (bj < bi) { var tmp = bi; bi = bj; bj = tmp; }
      var si = size[bi], sj = size[bj];
      for (var k = 0; k < m; k++) {
        if (!active[k] || k === bi || k === bj) continue;
        var v = (d[bi * m + k] * si + d[bj * m + k] * sj) / (si + sj);
        d[bi * m + k] = d[k * m + bi] = v;
      }
      merges.push({ a: id[bi], b: id[bj], h: best / 2, size: si + sj });
      members[bi] = members[bi].concat(members[bj]);
      id[bi] = m + step;
      size[bi] = si + sj;
      active[bj] = false;
      rescan(bi);
      for (k = 0; k < m; k++) {
        if (!active[k] || k === bi) continue;
        if (near[k] === bi || near[k] === bj) rescan(k);
        else if (d[k * m + bi] < nearD[k]) { near[k] = bi; nearD[k] = d[k * m + bi]; }
      }
    }
    var order = [];
    for (i = 0; i < m; i++) if (active[i]) order = order.concat(members[i]);
    return { order: order, merges: merges };
  }
  /* Classical MDS to 2-D: double-centre the squared distances, take the top
   * two eigenvectors by power iteration (deterministic start; a picture, so
   * a few dozen iterations are plenty). */
  function mds2(dist, m) {
    var B = new Float64Array(m * m), rowMean = new Float64Array(m), all = 0, i, j;
    for (i = 0; i < m; i++) for (j = 0; j < m; j++) {
      var s = dist[i * m + j] * dist[i * m + j];
      B[i * m + j] = s; rowMean[i] += s / m; all += s / (m * m);
    }
    for (i = 0; i < m; i++) for (j = 0; j < m; j++)
      B[i * m + j] = -0.5 * (B[i * m + j] - rowMean[i] - rowMean[j] + all);
    var vecs = [], vals = [];
    for (var e = 0; e < 2; e++) {
      var v = new Float64Array(m);
      for (i = 0; i < m; i++) v[i] = Math.sin(i * 12.9898 + e * 78.233) + 1.5;
      var lambda = 0;
      for (var it = 0; it < 60; it++) {
        var w = new Float64Array(m);
        for (i = 0; i < m; i++) { var acc = 0; for (j = 0; j < m; j++) acc += B[i * m + j] * v[j]; w[i] = acc; }
        for (var p = 0; p < vecs.length; p++) {   /* deflate the found ones */
          var dot = 0; for (i = 0; i < m; i++) dot += w[i] * vecs[p][i];
          for (i = 0; i < m; i++) w[i] -= dot * vecs[p][i];
        }
        var norm = 0; for (i = 0; i < m; i++) norm += w[i] * w[i];
        norm = Math.sqrt(norm);
        if (!(norm > 1e-12)) break;
        for (i = 0; i < m; i++) w[i] /= norm;
        lambda = norm;
        var diff = 0; for (i = 0; i < m; i++) diff += Math.abs(w[i] - v[i]);
        v = w;
        if (diff < 1e-6 * m) break;
      }
      vecs.push(v); vals.push(lambda);
    }
    var xy = new Float64Array(2 * m);
    for (i = 0; i < m; i++) {
      xy[2 * i] = vecs[0][i] * Math.sqrt(Math.max(0, vals[0]));
      xy[2 * i + 1] = vecs[1][i] * Math.sqrt(Math.max(0, vals[1]));
    }
    return xy;
  }
  function histogram(values, bins, lo, hi) {
    var counts = new Array(bins).fill(0), w = (hi - lo) / bins;
    for (var i = 0; i < values.length; i++) {
      var b = Math.floor((values[i] - lo) / w);
      if (b >= 0 && b < bins) counts[b]++;
      else if (values[i] === hi) counts[bins - 1]++;
    }
    return { lo: lo, hi: hi, width: w, counts: counts };
  }

  root.EvoDashData = {
    DT: DT, COL: COL, ROW: ROW, Q: Q, NSENS: NSENS, TOP: TOP,
    History: History, Family: Family, Lines: Lines,
    upgma: upgma, mds2: mds2, histogram: histogram
  };
})(typeof window !== "undefined" ? window : globalThis);
