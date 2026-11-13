/* hud.js -- the renderer. One snapshot in, the whole overlay out.
 *
 * Contract: ../../docs/HUD-API.md v1. Transport lives in bridge.js and
 * nothing here knows about it.
 *
 * Two properties this file is built to keep:
 *
 *   IDEMPOTENT. render(snap) sets every element it owns from that snapshot
 *   alone -- it never reads the DOM back and keeps no state that the next
 *   snapshot cannot overwrite. A dropped push is therefore harmless, and
 *   two identical pushes are indistinguishable from one.
 *
 *   NO CLOCK. The HUD never counts down on its own. Every number on screen
 *   came from a snapshot. If the plugin stops pushing, the HUD freezes on
 *   the last truth rather than inventing a newer one -- which is why the
 *   agreed cadence includes one all-clear push after everything empties.
 *
 * Events only ever ADD a one-shot animation or a note. Losing one costs
 * nothing: the snapshot behind it already rendered.
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};
  var doc = global.document;

  /* ---- the ring the threat chevrons ride ------------------------------
   * An inset rectangle, in percent of the viewport. The insets are not
   * symmetric and the corners are kept clear, because the screen is not
   * empty: the vanilla FO4 HUD owns the bottom (health and AP bottom left,
   * compass bottom centre, ammo bottom right) and this HUD's own plates own
   * the four corners. An arrow that lands in a corner slides along the ring
   * until it is clear -- the chevron's ROTATION carries the bearing, so a
   * few degrees of position error costs nothing, while an arrow buried
   * under the target readout costs the whole indicator.
   */
  var RING = { left: 7.5, right: 7.5, top: 8, bottom: 18 };   /* 7.5: a 12rem label at the 28px HUD scale stays on screen */
  var SIDE_Y = [14, 80];     /* arrows on a side edge stay between these */
  var MID_X  = [22, 78];     /* arrows on the top or bottom edge, between these */

  /* The player can move and resize the ring (layout.js, "Threat ring"). The stock ring above is what an empty
     layout returns, and the free bands keep the same margins from whatever ring is in use. */
  function ringNow() {
    var g = CW.HudLayout ? CW.HudLayout.ring() : null;
    if (!g) { return { R: RING, sy: SIDE_Y, mx: MID_X }; }
    var R = { left: g.l, right: g.r, top: g.t, bottom: g.b };
    return { R: R, sy: [R.top + 6, Math.max(R.top + 7, 100 - R.bottom - 2)], mx: [R.left + 14.5, Math.max(R.left + 15.5, 100 - R.right - 14.5)] };
  }
  function layoutVersion() { return CW.HudLayout ? CW.HudLayout.version() : 0; }
  var MAX_ARROWS = 8;        /* nearest first; more than this is noise */

  var ARROW_SVG =
    '<svg viewBox="0 0 100 100" aria-hidden="true" focusable="false">' +
    '<path d="M50 4 L94 88 L50 66 L6 88 Z"/></svg>';

  /* ---- tiny DOM helpers ----------------------------------------------- */
  var $cache = {};
  function $(id) {
    var n = $cache[id];
    if (n === undefined) { n = $cache[id] = doc.getElementById(id); }
    return n;
  }
  function show(id, on) {
    var n = $(id);
    if (!n) { return; }
    var was = n.hidden;
    n.hidden = !on;
    /* a moved plate appearing after a resolution change: keep it on screen (layout.js; a no-op when not moved) */
    if (on && was && CW.HudLayout) { CW.HudLayout.clampOne(id); }
  }
  function txt(id, s) { var n = $(id); if (n && n.textContent !== s) { n.textContent = s; } }
  function attr(n, k, v) { if (n && n.getAttribute(k) !== v) { n.setAttribute(k, v); } }
  function css(n, k, v) { if (n) { n.style.setProperty(k, v); } }

  function num(v, d) { return (typeof v === 'number' && isFinite(v)) ? v : d; }
  function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
  function pct(v) { return (clamp(v, 0, 1) * 100).toFixed(2) + '%'; }

  /* A duration, the way a HUD should read it: one decimal under ten
     seconds (it is moving fast and the decimal is information), whole
     seconds above (the decimal is just noise at that distance). */
  function dur(s) { return s < 10 ? s.toFixed(1) : String(Math.round(s)); }

  /* ---- threat chevrons ------------------------------------------------ */

  /* Where on the ring a bearing comes out. Bearing is degrees relative to
     the player's facing: 0 ahead, + right, +-180 behind (HUD-API v1). */
  function ringPoint(bearing) {
    var RING = ringNow().R;
    var b = bearing * Math.PI / 180;
    var dx = Math.sin(b), dy = -Math.cos(b);          /* screen space: -y is ahead */

    var hw = (dx >= 0 ? 50 - RING.right : 50 - RING.left);
    var hh = (dy >= 0 ? 50 - RING.bottom : 50 - RING.top);

    var tx = Math.abs(dx) > 1e-6 ? hw / Math.abs(dx) : Infinity;
    var ty = Math.abs(dy) > 1e-6 ? hh / Math.abs(dy) : Infinity;

    var onSide = tx < ty;                              /* which edge it hit first */
    var t = onSide ? tx : ty;
    return { x: 50 + t * dx, y: 50 + t * dy, onSide: onSide };
  }

  /* ---- keeping the chevrons off the plates ----------------------------
   * An arrow buried under the target readout is a dead indicator, so the
   * plates are MEASURED and avoided rather than guessed at. Measuring is
   * what keeps this honest: move a plate in the CSS and the arrows get out
   * of its way on the next frame, with nothing here to update.
   *
   * getBoundingClientRect forces a synchronous layout, so it is done only
   * when the set of visible plates or the viewport actually changes -- not
   * on every one of the 10 pushes a second.
   */
  var boxCache = null, boxSig = '';

  function plateBoxes() {
    var root = $('hud');
    if (!root) { return []; }
    var plates = root.getElementsByClassName('plate');
    var sig = innerWidth + 'x' + innerHeight + '|L' + layoutVersion();   /* a moved plate is a new keep-out */
    var live = [];
    for (var i = 0; i < plates.length; i++) {
      if (!plates[i].hidden) { live.push(plates[i]); sig += '|' + plates[i].id; }
    }
    if (sig === boxSig && boxCache) { return boxCache; }

    var vw = innerWidth || 1, vh = innerHeight || 1;
    var out = [];
    for (var j = 0; j < live.length; j++) {
      var r = live[j].getBoundingClientRect();
      out.push({
        x0: r.left / vw * 100, x1: r.right / vw * 100,
        y0: r.top / vh * 100,  y1: r.bottom / vh * 100
      });
    }
    boxSig = sig; boxCache = out;
    return out;
  }

  /* The arrow's own half-size, measured once off a real node so it tracks
     the viewport type scale instead of a number copied out of the CSS. */
  var half = null, halfV = -1;
  function arrowHalf(node) {
    if (halfV !== layoutVersion()) { half = null; halfV = layoutVersion(); }   /* the ring's scale resizes the arrows */
    if (half) { return half; }
    var r = node.getBoundingClientRect();
    /* The chevron alone is 2.6rem and the label adds a line: anything much
       shorter than that means this ran before the node had its text, and
       caching it would under-size the keep-out for the rest of the session. */
    if (r.width < 40 || r.height < 60) { return { w: 4.5, h: 3.4 }; }
    half = { w: r.width / 2 / (innerWidth || 1) * 100, h: r.height / 2 / (innerHeight || 1) * 100 };
    return half;
  }

  function merge(ivals) {
    if (!ivals.length) { return ivals; }
    ivals.sort(function (a, b) { return a[0] - b[0]; });
    var out = [ivals[0].slice(0)];
    for (var i = 1; i < ivals.length; i++) {
      var last = out[out.length - 1];
      if (ivals[i][0] <= last[1]) { last[1] = Math.max(last[1], ivals[i][1]); }
      else { out.push(ivals[i].slice(0)); }
    }
    return out;
  }

  /* Move v out of the blocked intervals, as little as possible, staying
     inside [lo, hi]. Blocked must be merged, so the two candidate ends are
     themselves free. */
  function nearestFree(v, blocked, lo, hi) {
    v = clamp(v, lo, hi);
    for (var i = 0; i < blocked.length; i++) {
      var a = blocked[i][0], b = blocked[i][1];
      if (v > a && v < b) {
        var below = a - 0.1, above = b + 0.1;
        var okBelow = below >= lo, okAbove = above <= hi;
        if (okBelow && okAbove) { return (v - a <= b - v) ? below : above; }
        if (okBelow) { return below; }
        if (okAbove) { return above; }
        return v;      /* nowhere to go: an overlap beats a wild position */
      }
    }
    return v;
  }

  /* The final screen position for a bearing: on the ring, clear of the
     plates, inside the free band for whichever edge it landed on. */
  /* A little air between a chevron and a plate: touching reads as broken
     even when the rectangles technically clear (ui-guidelines: room to
     breathe). Percent of viewport height / width. */
  var PAD = 0.9;

  function place(bearing, node) {
    var p = ringPoint(bearing), rn = ringNow(), SIDE_Y = rn.sy, MID_X = rn.mx;
    var m = arrowHalf(node);
    var hs = { w: m.w + PAD, h: m.h + PAD };
    var boxes = plateBoxes();
    var blocked = [];

    if (p.onSide) {
      var ax0 = p.x - hs.w, ax1 = p.x + hs.w;
      for (var i = 0; i < boxes.length; i++) {
        var b = boxes[i];
        if (b.x1 > ax0 && b.x0 < ax1) { blocked.push([b.y0 - hs.h, b.y1 + hs.h]); }
      }
      p.y = nearestFree(p.y, merge(blocked), SIDE_Y[0], SIDE_Y[1]);
    } else {
      var ay0 = p.y - hs.h, ay1 = p.y + hs.h;
      for (var j = 0; j < boxes.length; j++) {
        var c = boxes[j];
        if (c.y1 > ay0 && c.y0 < ay1) { blocked.push([c.x0 - hs.w, c.x1 + hs.w]); }
      }
      p.x = nearestFree(p.x, merge(blocked), MID_X[0], MID_X[1]);
    }
    /* whatever ring the player set, the whole chevron and its label stay on screen */
    p.x = clamp(p.x, m.w + 0.2, 100 - m.w - 0.2);
    p.y = clamp(p.y, m.h + 0.2, 100 - m.h - 0.2);
    return p;
  }

  var threatNodes = {};      /* id -> element, reused across snapshots */

  function renderThreats(list) {
    var host = $('threats');
    if (!host) { return; }

    var keep = {};
    if (list && list.length) {
      /* nearest first, then cap: an arrow for every raider in a firefight
         is a screen full of chevrons and no information. */
      var ts = list.slice(0).sort(function (a, b) {
        return num(a.dist_m, 1e9) - num(b.dist_m, 1e9);
      }).slice(0, MAX_ARROWS);

      for (var i = 0; i < ts.length; i++) {
        var th = ts[i];
        var id = String(th.id === undefined || th.id === null ? 'i' + i : th.id);
        keep[id] = true;

        var n = threatNodes[id];
        if (!n) {
          n = doc.createElement('div');
          n.className = 'threat';
          n.innerHTML =
            '<span class="threat__arrow">' + ARROW_SVG + '</span>' +
            '<span class="threat__label"><span class="threat__nm"></span><b></b></span>';
          threatNodes[id] = n;
          host.appendChild(n);
        }

        /* Text FIRST, then position. place() measures a real arrow node to
           learn its half-size, and an empty label measures short -- which
           silently shrinks the keep-out and lets the arrow slide under a
           plate by exactly the height of the missing line of text. */
        var nm = n.firstChild.nextSibling.firstChild;   /* .threat__nm */
        var ds = nm.nextSibling;                        /* <b> */
        var name = th.name ? String(th.name) : 'HOSTILE';
        if (nm.textContent !== name) { nm.textContent = name; }
        var d = Math.round(num(th.dist_m, 0)) + 'm';
        if (ds.textContent !== d) { ds.textContent = d; }

        var p = place(num(th.bearing, 0), n);
        css(n, '--x', p.x.toFixed(2) + '%');
        css(n, '--y', p.y.toFixed(2) + '%');
        css(n, '--b', num(th.bearing, 0).toFixed(1) + 'deg');
      }
    }

    /* Anything not in this snapshot is gone. The snapshot is the truth. */
    for (var k in threatNodes) {
      if (!keep[k]) {
        var old = threatNodes[k];
        if (old && old.parentNode) { old.parentNode.removeChild(old); }
        delete threatNodes[k];
      }
    }
  }

  /* ---- Kiroshi optics ------------------------------------------------- */
  function renderOptic(z) {
    var on = !!(z && (z.on || z.scan));
    show('optic', on);
    if (!on) { return; }
    var zoom = !!z.on;
    attr($('optic'), 'data-mode', zoom ? 'zoom' : 'scan');
    var f = num(z.factor, 0);
    txt('optic-mode', zoom ? ('ZOOM x' + (f > 0 ? f.toFixed(1) : '1.0')) : 'SCAN');
    txt('optic-tag', 'KIROSHI');
  }

  /* ---- Target Analysis ------------------------------------------------ */
  var FIG = null;
  function figParts() {
    if (!FIG) {
      var f = $('fig');
      FIG = f ? f.getElementsByTagName('span') : [];
    }
    return FIG;
  }

  function renderTarget(t) {
    show('target', !!t);
    if (!t) { return; }

    txt('t-name', t.name ? String(t.name) : 'UNKNOWN');
    txt('t-dist', String(Math.round(num(t.dist_m, 0))));

    var hp = num(t.hp, 0), hpMax = num(t.hpMax, 0);
    var frac = hpMax > 0 ? clamp(hp / hpMax, 0, 1) : 0;
    var fill = $('t-hp-fill');
    css(fill, 'width', pct(frac));
    attr(fill, 'data-band', frac > 0.6 ? 'hi' : (frac > 0.25 ? 'mid' : 'lo'));
    txt('t-hp', String(Math.round(hp)));
    txt('t-hpmax', String(Math.round(hpMax)));

    txt('t-dr', String(Math.round(num(t.dr, 0))));
    txt('t-er', String(Math.round(num(t.er, 0))));

    /* The weak point keys off the INT, never the display name: the name is
       there to be read, the group is there to be trusted. */
    var g = num(t.vulnLimbGroup, -1);
    var hasWeak = g >= 0;
    show('weak', hasWeak);
    var parts = figParts();
    for (var i = 0; i < parts.length; i++) {
      var pg = parseInt(parts[i].getAttribute('data-g'), 10);
      var lit = hasWeak && pg === g;
      if (parts[i].classList.contains('is-weak') !== lit) {
        parts[i].classList[lit ? 'add' : 'remove']('is-weak');
      }
    }
    if (hasWeak) { txt('t-limb', t.vulnLimb ? String(t.vulnLimb) : LIMB[g] || 'LIMB'); }
  }

  var NBSP = '\u00a0';   /* kept as an escape: these files stay ASCII */
  var LIMB = ['TORSO', 'HEAD', 'LEFT ARM', 'RIGHT ARM', 'LEFT LEG', 'RIGHT LEG'];

  /* ---- Machine Learning stacks ---------------------------------------- */
  function renderMl(ml) {
    show('ml', !!ml);
    if (!ml) { return; }
    var max = Math.max(1, Math.round(num(ml.max, 5)));
    var st = clamp(Math.round(num(ml.stacks, 0)), 0, max);
    pips('ml-pips', max, st);
    attr($('ml'), 'data-full', st >= max ? 'true' : 'false');
    show('ml-x', st >= max);          /* CP doubles the bonus at full stacks */
    txt('ml-until', dur(Math.max(0, num(ml.untilS, 0))));
  }

  /* One shape, one size, `max` of them always: the row cannot go ragged. */
  function pips(hostId, max, on) {
    var host = $(hostId);
    if (!host) { return; }
    while (host.childNodes.length > max) { host.removeChild(host.lastChild); }
    while (host.childNodes.length < max) {
      var li = doc.createElement('li');
      li.className = 'pip';
      host.appendChild(li);
    }
    for (var i = 0; i < max; i++) {
      var lit = i < on;
      var n = host.childNodes[i];
      if (n.classList.contains('is-on') !== lit) {
        n.classList[lit ? 'add' : 'remove']('is-on');
      }
    }
  }

  /* ---- cyberdeck: RAM + the quickhack queue --------------------------- */
  function renderDeck(ram) {
    show('deck', !!ram);
    if (!ram) { return; }

    var max = num(ram.max, 0), cur = clamp(num(ram.cur, 0), 0, max);
    var cells = Math.max(1, Math.round(max));

    /* RAM reads as whole units but regenerates continuously, so the cell
       that is filling shows its fraction rather than snapping. */
    txt('ram-cur', cur % 1 >= 0.05 ? cur.toFixed(1) : String(Math.round(cur)));
    txt('ram-max', String(Math.round(max)));

    var host = $('ram-cells');
    while (host.childNodes.length > cells) { host.removeChild(host.lastChild); }
    while (host.childNodes.length < cells) {
      var li = doc.createElement('li');
      li.className = 'cell';
      host.appendChild(li);
    }
    var full = Math.floor(cur + 1e-6), frac = cur - full;
    for (var i = 0; i < cells; i++) {
      var n = host.childNodes[i];
      var on = i < full, part = (i === full && frac > 0.02);
      n.className = 'cell' + (on ? ' is-on' : (part ? ' is-part' : ''));
      if (part) { css(n, '--f', pct(frac)); }
    }

    renderQueue(ram.queue);
  }

  /* FRAMEWORK, 2026-11-08: our quickhacks upload instantly and CP 2.x has no
     quickhack cooldown, so in practice only state:"active" ever arrives. The
     other two states stay wired anyway -- the contract allows them and a HUD
     that throws on a legal payload is a worse bug than an unused branch. */
  var QSTATE = { uploading: 1, active: 1, cooldown: 1, selected: 1 };   /* selected: ABILITIES 2026-11-09, the hack the next key press uploads */

  function renderQueue(q) {
    var host = $('queue');
    if (!host) { return; }
    var list = (q && q.length) ? q : [];
    while (host.childNodes.length > list.length) { host.removeChild(host.lastChild); }
    while (host.childNodes.length < list.length) {
      var li = doc.createElement('li');
      li.className = 'queue__r';
      li.innerHTML = '<span class="queue__n"></span><span class="queue__t"></span>';
      host.appendChild(li);
    }
    for (var i = 0; i < list.length; i++) {
      var h = list[i], n = host.childNodes[i];
      var st = QSTATE[h.state] ? h.state : 'active';
      attr(n, 'data-state', st);
      var nm = n.firstChild, tm = nm.nextSibling;
      var label = h.name ? String(h.name) : 'QUICKHACK';
      if (nm.textContent !== label) { nm.textContent = label; }
      var eta = num(h.etaS, -1);
      var right = (st !== 'selected' && eta >= 0) ? (dur(eta) + 's')
                : (num(h.cost, -1) >= 0 ? (Math.round(h.cost) + ' RAM') : '');
      if (tm.textContent !== right) { tm.textContent = right; }
    }
  }

  /* ---- rings: the OS meter and the launcher --------------------------- */
  var CIRC = 2 * Math.PI * 42;      /* r=42 in the 0 0 100 100 viewBox */

  function arc(id, frac) {
    var c = $(id);
    if (!c) { return; }
    /* Inline style, not a presentation attribute: the CSS transition on
       stroke-dashoffset is what makes the ring sweep instead of jump, and a
       transition on a presentation attribute is not a safe bet in
       Ultralight. In game the launcher ring still stayed full for a whole
       recharge (QD block 13), so the attributes are set as well: they are
       ignored wherever the inline style works. */
    var da = String(CIRC.toFixed(2));
    if (c.style.strokeDasharray !== da) { c.style.strokeDasharray = da; }
    if (c.getAttribute('stroke-dasharray') !== da) { c.setAttribute('stroke-dasharray', da); }
    var off = (CIRC * (1 - clamp(frac, 0, 1))).toFixed(2);
    if (c.style.strokeDashoffset !== off) { c.style.strokeDashoffset = off; }
    if (c.getAttribute('stroke-dashoffset') !== off) { c.setAttribute('stroke-dashoffset', off); }
  }

  /* Fill one pip part way, by width: progress that does not depend on the ring's SVG sweep. */
  function pipFill(hostId, idx, frac) {
    var host = $(hostId);
    if (!host) { return; }
    for (var i = 0; i < host.childNodes.length; i++) {
      var n = host.childNodes[i], f = n.firstChild;
      var w = i === idx ? Math.round(clamp(frac, 0, 1) * 100) + '%' : '0%';
      if (!f) {
        if (w === '0%') { continue; }
        f = doc.createElement('span');
        f.className = 'pip__fill';
        n.appendChild(f);
      }
      if (f.style.width !== w) { f.style.width = w; }
    }
  }

  /* Exactly one timer carries kind:"os" (the installed Sandevistan or
     Berserk). Everything else is a pip row below. */
  function renderOs(t) {
    show('os', !!t);
    if (!t) { return; }

    var a = num(t.activeS, 0), aMax = num(t.activeMaxS, 0);
    var r = num(t.readyS, 0), rMax = num(t.readyMaxS, 0);
    var state, frac, v, u;

    if (a > 0) {
      state = 'active';
      frac = aMax > 0 ? a / aMax : 1;       /* maxS null = unknown: full arc */
      v = dur(a); u = 'SEC';
    } else if (r > 0) {
      state = 'cooling';
      frac = rMax > 0 ? 1 - (r / rMax) : 0;
      v = dur(r); u = 'CD';
    } else {
      state = 'ready';
      frac = 1;
      v = 'RDY'; u = '';
    }

    attr($('os'), 'data-state', state);
    attr($('os-ring'), 'data-state', state);
    arc('os-arc', frac);
    txt('os-v', v);
    txt('os-u', u || NBSP);
    txt('os-name', t.name ? String(t.name) : 'OPERATING SYSTEM');
    txt('os-tag', 'OS');
  }

  function renderPls(pls) {
    show('pls', !!pls);
    if (!pls) { return; }
    var max = Math.max(1, Math.round(num(pls.max, 1)));
    var c = clamp(Math.round(num(pls.charges, 0)), 0, max);

    /* pool is the whole 0..1 CP stat pool and one charge is 1/max of it, so
       the ring shows progress toward the NEXT charge, not the pool itself. */
    var pool = clamp(num(pls.pool, 0), 0, 1);
    var toNext = clamp(pool * max - c, 0, 1);
    var full = c >= max;

    /* A charge only fires while the launcher holds the grenade slot. With a frag or mine there, the throw key throws
       that, so "Ready" would promise a shot the key will not give (QD block 12). No slot field: an older plugin. */
    var slotWord = { grenade: 'Grenade in slot', mine: 'Mine in slot', empty: 'Not in slot' }[pls.slot];
    var blocked = c > 0 && !!slotWord;

    attr($('pls-ring'), 'data-state', blocked ? 'cooling' : (full ? 'full' : 'cooling'));
    attr($('pls'), 'data-state', blocked ? 'blocked' : (full ? 'ready' : 'cooling'));
    var left = num(pls.nextS, 0);                 /* seconds to the next charge; absent from an older plugin */
    var charging = left > 0 ? 'Charging ' + Math.ceil(left) + ' s' : 'Charging';
    txt('pls-state', blocked ? slotWord : (full ? 'Ready' : charging));
    arc('pls-arc', full ? 1 : toNext);
    txt('pls-n', String(c));
    pips('pls-pips', max, c);
    pipFill('pls-pips', c, full ? 0 : toNext);
  }

  function renderTimers(list) {
    var host = $('tmrs');
    if (!host) { return; }
    var rows = [];
    if (list && list.length) {
      for (var i = 0; i < list.length; i++) {
        if (list[i] && list[i].kind !== 'os') { rows.push(list[i]); }
      }
    }
    show('timers', rows.length > 0);

    while (host.childNodes.length > rows.length) { host.removeChild(host.lastChild); }
    while (host.childNodes.length < rows.length) {
      var li = doc.createElement('li');
      li.className = 'tmr';
      li.innerHTML = '<span class="tmr__n"></span><span class="tmr__t"></span>' +
                     '<span class="tmr__bar"><i></i></span>';
      host.appendChild(li);
    }
    for (var j = 0; j < rows.length; j++) {
      var t = rows[j], n = host.childNodes[j];
      var a = num(t.activeS, 0), aMax = num(t.activeMaxS, 0);
      var r = num(t.readyS, 0), rMax = num(t.readyMaxS, 0);
      var active = a > 0;
      attr(n, 'data-state', active ? 'active' : (r > 0 ? 'cooling' : 'ready'));
      var nm = n.firstChild, tm = nm.nextSibling, bar = tm.nextSibling.firstChild;
      var label = t.name ? String(t.name) : (t.ability ? String(t.ability) : 'CYBERWARE');
      if (nm.textContent !== label) { nm.textContent = label; }
      var right = active ? (dur(a) + 's') : (r > 0 ? (dur(r) + 's') : 'READY');
      if (tm.textContent !== right) { tm.textContent = right; }
      var frac = active ? (aMax > 0 ? a / aMax : 1)
                        : (r > 0 ? (rMax > 0 ? 1 - r / rMax : 0) : 1);
      css(bar, 'width', pct(frac));
    }
  }

  /* ---- one-shot notes ------------------------------------------------- */
  var noteNode = null;
  var noteTimer = null;
  function note(text, tone, holdMs) {
    var host = $('hud');
    if (!host) { return; }
    if (noteNode && noteNode.parentNode) { noteNode.parentNode.removeChild(noteNode); }
    if (noteTimer) { global.clearTimeout(noteTimer); noteTimer = null; }
    var n = doc.createElement('div');
    n.className = 'note';
    n.setAttribute('data-tone', tone || 'info');
    /* A HELD note (the arm-use hint) is a sentence to READ, not a combat flash: it holds longer, wraps instead of
       running off a 2560-wide screen, and is sized to be read from a couch. styles.css owns the look. */
    if (holdMs) { n.setAttribute('data-hold', 'long'); n.style.setProperty('--note-hold', holdMs + 'ms'); }
    n.textContent = text;
    function drop() {
      if (n.parentNode) { n.parentNode.removeChild(n); }
      if (noteNode === n) { noteNode = null; }
      if (noteTimer) { global.clearTimeout(noteTimer); noteTimer = null; }
    }
    n.addEventListener('animationend', drop);
    /* IT MUST GO AWAY BY ITSELF EVEN WITH NO ANIMATION. Under prefers-reduced-motion styles.css sets
       `animation: none` on .note, so `animationend` NEVER fires and the node stayed on screen for good - a
       pre-existing bug this backstop also fixes for the ordinary notes. */
    noteTimer = global.setTimeout(drop, (holdMs || 2600) + 400);
    noteNode = n;
    host.appendChild(n);
  }

  /* Smart-gun lock brackets. Positions are 0..1 of the view, so they need no layout
     pass; each bracket draws in on its target as the lock builds and snaps shut in the
     danger colour when the lock is made. Nothing is sent while nothing moves. */
  var lockNodes = {};

  function lockNode(id) {
    var n = lockNodes[id];
    if (n) { return n; }
    var host = $('smart-locks');
    n = document.createElement('div');
    n.className = 'lk';
    var corners = '<i class="lk__c lk__c--tl"></i><i class="lk__c lk__c--tr"></i>' +
                  '<i class="lk__c lk__c--bl"></i><i class="lk__c lk__c--br"></i>';
    n.innerHTML = '<div class="lk__box"><div class="lk__f lk__f--bk">' + corners + '</div><div class="lk__f">' + corners + '</div></div>';
    host.appendChild(n);
    lockNodes[id] = n;
    return n;
  }

  /* A bracket is placed with left / top / width / height, NEVER with a transform (QD block 26, 2026-11-10: a red
     half-bracket stayed on screen after its lock was lost, through a cell reload and a holster). The old node was a
     0 x 0 box moved by translate() with a scaled child, so everything it painted lay outside its own layout box; when
     the node was removed, the pixels it had painted were not all repainted. Cause INFERRED from the frames and the
     DOM (the node was gone and the pixels stayed); it cannot be reproduced in the Chromium harness. Two things now
     make a leftover impossible on our side: the painted box IS the layout box, and with no lock the whole layer is
     taken out (styles.css .locks.is-on), which repaints its full rect. */
  function renderLocks(locks) {
    var host = $('smart-locks');
    var seen = {}, count = 0;
    var w = window.innerWidth, h = window.innerHeight;
    (locks || []).forEach(function (l) {
      var id = String(l.id);
      var n = lockNode(id);
      seen[id] = true;
      count += 1;
      /* u = screen heights per game unit at the target, so the box is one torso at any range and under the aiming zoom */
      var size = Math.max(84, Math.min(380, num(l.u, 0.0015) * 62 * 1440)) * (h / 1440);
      var p = Math.max(0, Math.min(1, num(l.progress, 0)));
      var k = 1 + (1 - p) * 0.6;                       /* wide open while the lock builds, shut at 1 */
      var s = Math.round(size * k);
      n.style.width = s + 'px';
      n.style.height = s + 'px';
      n.style.left = Math.round(num(l.x, 0.5) * w - s / 2) + 'px';
      n.style.top = Math.round(num(l.y, 0.5) * h - s / 2) + 'px';
      /* the corner strokes grow with the box, as they did under scale() */
      n.style.setProperty('--lkw', Math.round(8 * k) + 'px');
      n.style.setProperty('--lkbk', Math.round(14 * k) + 'px');
      n.style.setProperty('--lko', (-Math.round(3 * k)) + 'px');
      var state = l.locked ? 'locked' : 'building';
      if (n.getAttribute('data-state') !== state) {
        n.setAttribute('data-state', state);
        if (l.locked) { oneShot(n, 'is-new'); }
      }
    });
    for (var id in lockNodes) {
      if (!seen[id]) {
        if (lockNodes[id].parentNode) { lockNodes[id].parentNode.removeChild(lockNodes[id]); }
        delete lockNodes[id];
      }
    }
    if (host) {
      if (count) { host.classList.add('is-on'); } else { host.classList.remove('is-on'); }
    }
  }

  function oneShot(node, cls) {
    if (!node) { return; }
    node.classList.remove(cls);
    /* Re-adding the class in a later frame restarts the animation. Reading
       offsetWidth would do it too, but that forces a synchronous layout on
       every event -- on a HUD, during combat. The menu's app.js treats
       requestAnimationFrame as possibly absent in Ultralight, so this does
       too and falls back to a timeout. */
    var go = function () {
      node.classList.add(cls);
      var off = function () {
        node.classList.remove(cls);
        node.removeEventListener('animationend', off);
      };
      node.addEventListener('animationend', off);
    };
    if (global.requestAnimationFrame) { global.requestAnimationFrame(go); }
    else { global.setTimeout(go, 16); }
  }

  /* ---- the renderer bridge.js talks to -------------------------------- */

  var cfg = { threatRangeM: 50 };

  var Renderer = {
    render: function (snap) {
      var root = $('hud');
      if (!root) { return; }

      if (snap.cfg && typeof snap.cfg === 'object') {
        if (num(snap.cfg.threatRangeM, 0) > 0) { cfg.threatRangeM = snap.cfg.threatRangeM; }
      }

      /* visible false = a menu, the Pip-Boy or dialogue is up. Everything
         goes, including the arrows: nothing of ours draws over a menu. */
      var vis = snap.visible !== false;
      attr(root, 'data-visible', vis ? 'true' : 'false');
      attr(root, 'data-combat', snap.inCombat ? 'true' : 'false');
      if (!vis) { return; }

      renderOptic(snap.zoom);
      renderTarget(snap.target);
      renderMl(snap.ml);
      renderDeck(snap.ram);
      renderOs(pickOs(snap.timers));
      renderPls(snap.pls);
      renderTimers(snap.timers);
      renderThreats(snap.threats);
    },

    event: function (ev) {
      switch (ev.type) {
        case 'vulnerability':
          note('WEAK POINT  ' + String(ev.limb || '').toUpperCase(), 'info');
          break;
        case 'vulnDestroyed':
          note('WEAK POINT DESTROYED' +
               (num(ev.mlStacks, 0) > 0 ? '   ML x' + Math.round(ev.mlStacks) : ''), 'good');
          break;
        case 'threatNew':
          var n = threatNodes[String(ev.id)];
          if (n) { oneShot(n, 'is-new'); }   /* .threat.is-new .threat__arrow */
          break;
        case 'plsCharge':
          var host = $('pls-pips');
          var i = Math.round(num(ev.charges, 0)) - 1;
          if (host && i >= 0 && host.childNodes[i]) { oneShot(host.childNodes[i], 'is-gain'); }
          break;
        case 'smartLocks':
          renderLocks(ev.locks);
          break;
        case 'note':
          if (ev.text) {
            note(String(ev.text), ev.tone === 'bad' || ev.tone === 'good' ? ev.tone : 'info',
                 typeof ev.hold === 'number' && ev.hold > 0 ? ev.hold : 0);
          }
          break;
        default:
          /* An unknown type is not an error: FRAMEWORK may add one without
             waiting for the view to catch up. */
          break;
      }
    },

    cfg: function () { return cfg; }
  };

  function pickOs(timers) {
    if (!timers || !timers.length) { return null; }
    for (var i = 0; i < timers.length; i++) {
      if (timers[i] && timers[i].kind === 'os') { return timers[i]; }
    }
    return null;
  }

  CW.Hud = Renderer;
  CW.HudBridge.attach(Renderer);

  /* Outside the game nobody will push anything, so the mock drives the same
     two window functions. In game mock.js is shipped but refuses to start
     (Ultralight user agent, no query string). */
  if (CW.HudMock) { CW.HudMock.start(); }

})(this);
