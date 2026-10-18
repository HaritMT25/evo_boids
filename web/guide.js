"use strict";
/* evo_boids first-visit guide: a short spotlight tour of the page.
 *
 * It offers itself on a first visit once the world is running (renderer.js
 * fires "evo:ready"); "? Guide" in the menu replays it. ?guide=0 keeps it from
 * starting on its own (tests), ?guide=1 always starts it. "Seen" lives in
 * localStorage; where that is unavailable the guide simply offers itself again
 * next visit. The shade takes no clicks, so the simulation keeps running and
 * every control still works mid-tour (the keyboard step invites trying them).
 * Page hooks it uses: window.EvoApp (renderer.js). */
(function () {
  var KEY = "evo_boids.guideSeen.v1";
  function app() { return window.EvoApp || {}; }
  function q(sel) { return document.querySelector(sel); }
  function rectOf(sel) { var e = q(sel); return e ? e.getBoundingClientRect() : null; }
  function touch() { return !!(app().touch && app().touch()); }
  function seen() { try { return localStorage.getItem(KEY) === "1"; } catch (e) { return false; } }
  function markSeen() { try { localStorage.setItem(KEY, "1"); } catch (e) { /* private mode */ } }

  /* One step: title, body() (trusted HTML), target() -> box or null (centred
   * card), drawer (open/close the menu on entry), scroll (bring into view in
   * the menu), place (card sides to try, in order), enter() (side effects). */
  var DRAWER_SIDES = ["left", "below", "above"];
  var STEPS = [
    { welcome: true, drawer: false, title: "Welcome to evo_boids",
      body: function () {
        return "Each fish has a tiny neural network wired by its genes. Fish that eat enough " +
               "have children with slightly mutated genes; nobody programs what they do, " +
               "their behaviour evolves.<br>Take the one-minute tour?";
      } },
    { drawer: false, title: "The ocean",
      target: function () { return rectOf("#canvas"); },
      body: function () {
        return "Green dots are food. Fish that find enough of it have children. Colour follows " +
               "the genes, so look-alikes are relatives.";
      } },
    { drawer: false, title: "A fish's brain", fish: true,
      enter: function () { if (app().inspectSomeFish) app().inspectSomeFish(); },
      target: function () { return app().inspectorRect ? app().inspectorRect() : null; },
      body: function () {
        return (touch() ? "Tap" : "Click") + " any fish to see its brain, live: senses on the " +
               "left, hidden neurons in the middle, actions on the right. Green links excite, " +
               "red ones inhibit; brighter means stronger right now. " +
               (touch() ? "Tap ✕" : "Click empty water") + " to close it.";
      } },
    { drawer: false, title: "Live numbers",
      target: function () { return rectOf("#status"); },
      body: function () {
        return "Frame rate, number of fish, and generations: <b>gen</b> is the longest family " +
               "line alive, <b>avg</b> the typical one. Watch avg climb as evolution runs.";
      } },
    { desktopOnly: true, drawer: false, title: "Keyboard",
      body: function () {
        return "<kbd>Space</kbd> pause · <kbd>↑</kbd> <kbd>↓</kbd> speed · <kbd>R</kbd> a fresh " +
               "world. Try one now; the tour waits. (<kbd>Esc</kbd> closes the menu, and this tour.)";
      } },
    { drawer: false, title: "Settings",
      target: function () { return rectOf("#menubtn"); },
      body: function () { return "Everything else lives behind this button."; } },
    { drawer: true, scroll: "#quick", place: DRAWER_SIDES, title: "🎲 Randomize",
      target: function () { return rectOf("#randombtn"); },
      body: function () {
        return "The quickest way to explore. It draws a setup from ranges that worked in our " +
               "experiments (predators, ocean currents, schooling rules), restarts, and lists " +
               "what it picked.";
      } },
    { drawer: true, scroll: "#bar fieldset", place: DRAWER_SIDES, title: "Playback",
      target: function () { return rectOf("#bar fieldset"); },
      body: function () {
        return "Pause and change speed. <b>Turbo</b> runs the simulation headless (no drawing, " +
               "in a background thread), many times faster; switch it off to see how far " +
               "evolution got. <b>Hold 60 fps</b> removes fish at random when your device " +
               "can't keep up.";
      } },
    { drawer: true, scroll: "#controls fieldset", place: DRAWER_SIDES, title: "Your own settings",
      target: function () { return rectOf("#controls fieldset"); },
      body: function () {
        return "Change anything by hand. " + (touch() ? "Tap" : "Hover over") + " a setting's " +
               "name to see what it does and good values to try. Changes apply on " +
               "<b>Restart</b>; settings marked <i>live</i> apply at once.";
      } },
    { drawer: true, scroll: "#bar fieldset:nth-of-type(2)", place: DRAWER_SIDES, title: "Save & load",
      target: function () { return rectOf("#bar fieldset:nth-of-type(2)"); },
      body: function () {
        return "Save the whole world to a file and load it later; it carries on exactly where " +
               "it stopped.";
      } },
    { last: true, drawer: false, title: "You're set",
      body: function () { return "Replay this tour any time with <b>? Guide</b> in the menu. Enjoy!"; } }
  ];

  var root = null, spot = null, card = null;
  var steps = [], idx = -1, timer = 0, lastKey = "";
  function drawerOpen() { var d = q("#drawer"); return !!d && d.classList.contains("open"); }
  /* the fish the brain step selected goes back once that step is over */
  function releaseFish() { if (app().releaseFish) app().releaseFish(); }

  function build() {
    root = document.createElement("div");
    root.id = "guide";
    spot = document.createElement("div");
    spot.className = "spot none";
    card = document.createElement("div");
    card.className = "card";
    card.setAttribute("role", "dialog");
    card.setAttribute("aria-live", "polite");
    root.appendChild(spot);
    root.appendChild(card);
  }
  function button(text, cls, fn) {
    var b = document.createElement("button");
    b.className = cls;
    b.textContent = text;
    /* blur: Space must keep pausing the simulation, not press this again */
    b.addEventListener("click", function () { fn(); b.blur(); });
    return b;
  }

  function show(i) {
    idx = i;
    var s = steps[i];
    if (!s.fish) releaseFish();
    if (s.drawer !== undefined && app().setDrawer) app().setDrawer(s.drawer);
    if (s.enter) s.enter();
    if (s.scroll) { var t = q(s.scroll); if (t) t.scrollIntoView({ block: "nearest" }); }
    card.innerHTML = "<h2></h2><p></p><div class='nav'></div>";
    card.querySelector("h2").textContent = s.title;
    card.querySelector("p").innerHTML = s.body();
    var nav = card.querySelector(".nav");
    if (s.welcome) {
      var none = document.createElement("span");
      none.className = "count";
      nav.appendChild(none);
      nav.appendChild(button("Not now", "skip", finish));
      nav.appendChild(button("Show me around", "next", function () { show(i + 1); }));
    } else {
      var count = document.createElement("span");
      count.className = "count";
      count.textContent = i + " / " + (steps.length - 1);
      nav.appendChild(count);
      if (!s.last) nav.appendChild(button("Skip tour", "skip", finish));
      if (i > 1) nav.appendChild(button("Back", "back", function () { show(i - 1); }));
      nav.appendChild(button(s.last ? "Done" : "Next", "next", s.last ? finish : function () { show(i + 1); }));
    }
    lastKey = "";
    tick();
  }

  /* Re-measured ten times a second: targets move (the menu slides, the
   * inspector follows its fish, the status line steps aside). A timer, not
   * requestAnimationFrame, so the reads land on a settled layout instead of
   * right after the simulation's per-frame DOM writes. */
  function tick() {
    var s = steps[idx];
    /* a menu step whose menu got closed: nothing to point at until it reopens */
    var r = s.target && !(s.drawer && !drawerOpen()) ? s.target() : null;
    if (r && !(r.width > 0 && r.height > 0)) r = null;
    var key = idx + "|" + innerWidth + "x" + innerHeight + "|" +
              (r ? [r.left, r.top, r.width, r.height].map(Math.round).join() : "none") +
              "|" + card.offsetWidth + "x" + card.offsetHeight;
    if (key === lastKey) return;
    lastKey = key;
    place(r, s.place || ["below", "above", "right", "left"]);
  }
  function place(r, sides) {
    var pad = 6, gap = pad + 10, m = 12;
    var vw = innerWidth, vh = innerHeight, cw = card.offsetWidth, ch = card.offsetHeight;
    var x = (vw - cw) / 2, y = (vh - ch) / 2;
    if (r) {
      spot.className = "spot";
      spot.style.left = (r.left - pad) + "px";
      spot.style.top = (r.top - pad) + "px";
      spot.style.width = (r.width + 2 * pad) + "px";
      spot.style.height = (r.height + 2 * pad) + "px";
      var bottom = r.top + r.height, right = r.left + r.width;
      var cx = Math.max(m, Math.min(vw - cw - m, r.left + r.width / 2 - cw / 2));
      var cy = Math.max(m, Math.min(vh - ch - m, r.top + r.height / 2 - ch / 2));
      var at = {
        below: [cx, bottom + gap, bottom + gap + ch <= vh - m],
        above: [cx, r.top - gap - ch, r.top - gap - ch >= m],
        right: [right + gap, cy, right + gap + cw <= vw - m],
        left: [r.left - gap - cw, cy, r.left - gap - cw >= m]
      };
      for (var k = 0; k < sides.length; k++) {
        var a = at[sides[k]];
        if (a[2]) { x = a[0]; y = a[1]; break; }
      }
      /* nowhere beside it (a big target, such as the whole ocean): centred */
    } else {
      spot.className = "spot none";
      spot.style.left = vw / 2 + "px";
      spot.style.top = vh / 2 + "px";
      spot.style.width = "0px";
      spot.style.height = "0px";
    }
    card.style.left = Math.round(x) + "px";
    card.style.top = Math.round(y) + "px";
  }

  function finish() {
    markSeen();
    clearInterval(timer);
    releaseFish();
    if (root && root.parentNode) root.parentNode.removeChild(root);
    idx = -1;
    if (app().setDrawer) app().setDrawer(false);
  }
  /* replay: straight to the first real step; first visit: the welcome card */
  function start(replay) {
    if (idx >= 0) return;
    if (!root) build();
    document.body.appendChild(root);
    steps = STEPS.filter(function (s) { return !(s.desktopOnly && touch()); });
    show(replay ? 1 : 0);
    timer = setInterval(tick, 100);
  }
  window.addEventListener("keydown", function (e) {
    if (idx < 0) return;
    var t = e.target && e.target.tagName;
    if (t === "INPUT" || t === "SELECT" || t === "TEXTAREA") return;
    if (e.key === "Escape") finish();
    else if (e.key === "ArrowRight" && !steps[idx].welcome && !steps[idx].last) show(idx + 1);
    else if (e.key === "ArrowLeft" && idx > 1) show(idx - 1);
  });

  window.EvoGuide = { start: function () { start(true); } };
  /* renderer.js never fires evo:ready under ?bench, so no tour there */
  function auto() {
    var g = new URLSearchParams(location.search).get("guide");
    if (g === "0") return;
    if (g === "1" || !seen()) start(false);
  }
  if (app().ready) auto();
  else window.addEventListener("evo:ready", auto, { once: true });
})();
