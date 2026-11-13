/* layout.js -- the player's HUD layout: every element can be dragged and resized.
 *
 * The author, 2026-11-08: "Make sure any HUD stuff is resizable and draggable around the screen."
 *
 * TWO JOBS
 *   1. Apply the saved layout. The plugin sends it in the first snapshot (cfg.layout); in a browser it comes from
 *      localStorage. An element with no entry stays exactly where styles.css puts it (its corner rail), so the
 *      default HUD is unchanged.
 *   2. The editor. Cyberware.dll opens it (window.cwHudEdit({on:true, layout})) from the menu's Settings tab, L in
 *      the ripperdoc menu, or [HUD] iEditKey, with the HUD view focused and the game paused. Every element shows
 *      with sample data, a frame and its name:
 *        mouse     drag to move, drag the corner handle to resize, wheel to resize, click to select
 *        keys      Tab / Shift+Tab select, arrows nudge (Shift: further), + / - resize, R reset element,
 *                  Shift+R reset all, G snap on/off, H hide / show the panel, Enter save, Esc cancel
 *      Moves snap to the screen centre lines, the stock margins and the other elements' edges, with guide lines.
 *      Save sends {"op":"saveLayout","layout":...} then {"op":"editDone"} through window.cwHudRequest.
 *
 * SAVED LAYOUT (cw_hudlayout.cpp keeps exactly this shape; anything else is dropped there)
 *   { v:1,
 *     el:   { <id>: { ax:"l"|"r", ay:"t"|"b", x, y, s } },   x / y: the gap from the anchored screen edge as a
 *                                                             fraction of the screen; s: scale 0.6..2
 *     ring: { l, r, t, b, s } }                               the threat ring's insets in % of the screen, and the
 *                                                             chevrons' scale
 *   An element is anchored to its nearest screen corner and scales from it, so a layout made at 2560x1440 lands in
 *   the same place at 1920x1080 or after any resolution change; anything that would end up off screen is pulled in.
 *   The note (the one-shot "WEAK POINT" line) stores its CENTRE as x.
 *
 * Ultralight rules kept: mouse events only (no pointer events), keys read through keyOf (Esc arrives as
 * "Unidentified" + keyCode 27) from ONE capture-phase listener, nothing loops, ASCII only.
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};
  var doc = global.document;

  var ITEMS = [
    { id: 'optic',   name: 'Kiroshi optics' },
    { id: 'target',  name: 'Target analysis' },
    { id: 'ml',      name: 'Machine Learning' },
    { id: 'timers',  name: 'Cyberware timers' },
    { id: 'deck',    name: 'RAM and quickhacks' },
    { id: 'os',      name: 'OS meter' },
    { id: 'pls',     name: 'Launcher' },
    { id: 'note',    name: 'Notes' },
    { id: 'threats', name: 'Threat ring' }
  ];
  var DEF_RING = { l: 7.5, r: 7.5, t: 8, b: 18, s: 1 };
  var S_MIN = 0.6, S_MAX = 2.0, S_STEP = 0.05;
  var SNAP_PX = 12;
  var LS_KEY = 'cw.hudLayout';

  var layout = { v: 1, el: {} };
  var version = 0;
  var editing = false, before = null, selected = 'target', snapOn = true;
  var ui = null;            /* the editor's DOM, while editing */

  function clone(o) { return JSON.parse(JSON.stringify(o)); }
  function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
  function vw() { return global.innerWidth || doc.documentElement.clientWidth || 1; }
  function vh() { return global.innerHeight || doc.documentElement.clientHeight || 1; }
  function $(id) { return doc.getElementById(id); }
  function itemName(id) { for (var i = 0; i < ITEMS.length; i++) { if (ITEMS[i].id === id) { return ITEMS[i].name; } } return id; }

  /* ---- keys, the Ultralight way (same table as the menu's CW.keyOf) ---- */
  var KEYCODE = { 27: 'Escape', 13: 'Enter', 9: 'Tab', 8: 'Backspace', 46: 'Delete', 37: 'ArrowLeft', 38: 'ArrowUp',
                  39: 'ArrowRight', 40: 'ArrowDown', 107: '+', 187: '+', 109: '-', 189: '-', 16: 'Shift' };
  function keyOf(e) {
    if (CW.keyOf) { var k0 = CW.keyOf(e); if (k0 === '=') { return '+'; } if (k0 === '_') { return '-'; } if (k0) { return k0; } }
    var k = e && e.key;
    if (k && k !== 'Unidentified') { return k === '=' ? '+' : (k === '_' ? '-' : k); }
    var c = e ? (e.keyCode || e.which || 0) : 0;
    if (KEYCODE[c]) { return KEYCODE[c]; }
    if (c >= 65 && c <= 90) { return String.fromCharCode(c + 32); }
    return k || '';
  }

  /* ================================================================ apply */

  function ring() {
    var r = layout.ring;
    return r ? { l: r.l, r: r.r, t: r.t, b: r.b, s: r.s || 1 } : { l: DEF_RING.l, r: DEF_RING.r, t: DEF_RING.t, b: DEF_RING.b, s: 1 };
  }

  /* the live node an item stands for (the note only exists as a sample while editing) */
  function node(id) {
    if (id === 'note') { return ui ? ui.note : null; }
    if (id === 'threats') { return ui ? ui.ringBox : null; }
    return $(id);
  }

  function applyOne(id) {
    var e = layout.el[id];
    if (id === 'threats') { applyRing(); return; }
    if (id === 'note') { applyNote(); return; }
    var n = $(id);
    if (!n) { return; }
    var st = n.style;
    if (!e) {
      st.position = st.left = st.right = st.top = st.bottom = st.transform = st.transformOrigin = '';
      n.removeAttribute('data-moved');
      return;
    }
    st.position = 'fixed';
    st.left = e.ax === 'l' ? (e.x * 100).toFixed(3) + 'vw' : 'auto';
    st.right = e.ax === 'r' ? (e.x * 100).toFixed(3) + 'vw' : 'auto';
    st.top = e.ay === 't' ? (e.y * 100).toFixed(3) + 'vh' : 'auto';
    st.bottom = e.ay === 'b' ? (e.y * 100).toFixed(3) + 'vh' : 'auto';
    st.transformOrigin = (e.ax === 'l' ? 'left' : 'right') + ' ' + (e.ay === 't' ? 'top' : 'bottom');
    st.transform = e.s && e.s !== 1 ? 'scale(' + e.s.toFixed(3) + ')' : '';
    n.setAttribute('data-moved', 'true');
  }

  function applyNote() {
    var hud = $('hud');
    if (!hud) { return; }
    var e = layout.el.note;
    if (!e) {
      hud.style.removeProperty('--note-x'); hud.style.removeProperty('--note-y'); hud.style.removeProperty('--note-s');
      return;
    }
    hud.style.setProperty('--note-x', (e.x * 100).toFixed(3) + 'vw');
    hud.style.setProperty('--note-y', (e.y * 100).toFixed(3) + 'vh');
    hud.style.setProperty('--note-s', String(e.s || 1));
  }

  function applyRing() {
    var t = $('threats');
    if (t) { if (layout.ring && layout.ring.s && layout.ring.s !== 1) { t.style.setProperty('--ts', String(layout.ring.s)); } else { t.style.removeProperty('--ts'); } }
    if (ui) { placeRingBox(); }
  }

  function applyAll() {
    for (var i = 0; i < ITEMS.length; i++) { applyOne(ITEMS[i].id); }
    version++;
    clampAll();
  }

  /* Pull an element that ends up off screen (a smaller resolution, a hand-edited file) back on. */
  function clampOne(id) {
    var e = layout.el[id];
    if (!e || id === 'threats' || id === 'note') { return false; }
    var n = $(id);
    if (!n || n.hidden) { return false; }
    var r = n.getBoundingClientRect();
    if (r.width <= 0) { return false; }
    var W = vw(), H = vh(), changed = false;
    if (r.width > W || r.height > H) {
      e.s = clamp((e.s || 1) * Math.min(W / r.width, H / r.height) * 0.98, S_MIN, S_MAX);
      applyOne(id); r = n.getBoundingClientRect(); changed = true;
    }
    var dx = r.left < 0 ? -r.left : (r.right > W ? W - r.right : 0);
    var dy = r.top < 0 ? -r.top : (r.bottom > H ? H - r.bottom : 0);
    if (dx || dy) {
      e.x = clamp(e.x + (e.ax === 'l' ? dx : -dx) / W, 0, 1);
      e.y = clamp(e.y + (e.ay === 't' ? dy : -dy) / H, 0, 1);
      applyOne(id); changed = true;
    }
    return changed;
  }
  function clampAll() {
    for (var i = 0; i < ITEMS.length; i++) { clampOne(ITEMS[i].id); }
    if (layout.ring) {
      var g = layout.ring;
      g.l = clamp(g.l, 0, 45); g.r = clamp(g.r, 0, 45); g.t = clamp(g.t, 0, 45); g.b = clamp(g.b, 0, 45);
    }
    var ne = layout.el.note;
    if (ne) { ne.x = clamp(ne.x, 0.05, 0.95); ne.y = clamp(ne.y, 0, 0.92); applyNote(); }
  }

  function setLayout(l) {
    var out = { v: 1, el: {} };
    if (l && typeof l === 'object') {
      if (l.el && typeof l.el === 'object') {
        for (var k in l.el) {
          if (!l.el.hasOwnProperty(k)) { continue; }
          var e = l.el[k];
          if (!e || typeof e !== 'object' || typeof e.x !== 'number' || typeof e.y !== 'number') { continue; }
          out.el[k] = { ax: e.ax === 'r' ? 'r' : 'l', ay: e.ay === 'b' ? 'b' : 't',
                        x: clamp(e.x, 0, 1), y: clamp(e.y, 0, 1), s: clamp(typeof e.s === 'number' ? e.s : 1, S_MIN, S_MAX) };
        }
      }
      if (l.ring && typeof l.ring === 'object') {
        var g = l.ring;
        out.ring = { l: num(g.l, DEF_RING.l), r: num(g.r, DEF_RING.r), t: num(g.t, DEF_RING.t), b: num(g.b, DEF_RING.b),
                     s: clamp(num(g.s, 1), S_MIN, S_MAX) };
      }
    }
    layout = out;
    applyAll();
  }
  function num(v, d) { return (typeof v === 'number' && isFinite(v)) ? v : d; }

  /* ================================================================ the editor */

  function el(tag, cls, text) {
    var n = doc.createElement(tag);
    if (cls) { n.className = cls; }
    if (text !== undefined) { n.textContent = text; }
    return n;
  }

  function demoSnapshot() {
    var s = CW.HudMock && CW.HudMock.scenes && CW.HudMock.scenes.max ? CW.HudMock.scenes.max(0) : { v: 1 };
    s.visible = true;
    return s;
  }

  function build() {
    var root = el('div', 'hle');
    root.id = 'hle';
    var guides = el('div', 'hle__guides');
    var frames = el('div', 'hle__frames');
    var ringBox = el('div', 'hle__ring');
    var note = el('div', 'note is-sample', 'WEAK POINT  RIGHT ARM');
    note.setAttribute('data-tone', 'info');

    var bar = el('div', 'hle__bar');
    bar.setAttribute('role', 'dialog');
    bar.setAttribute('aria-label', 'HUD layout');
    var head = el('div', 'hle__head');
    head.appendChild(el('span', 'hle__title', 'HUD LAYOUT'));
    var sel = el('span', 'hle__sel');
    head.appendChild(sel);
    bar.appendChild(head);
    var row = el('div', 'hle__acts');
    function btn(label, key, cls, fn) {
      var b = el('button', 'hle__btn' + (cls ? ' ' + cls : ''));
      b.type = 'button';
      b.appendChild(el('span', 'hle__bl', label));
      if (key) { b.appendChild(el('span', 'hle__bk', key)); }
      b.addEventListener('click', function (e) { e.preventDefault(); fn(); });
      row.appendChild(b);
      return b;
    }
    var snapBtn = btn('Snap on', 'G', '', function () { setSnap(!snapOn); });
    btn('Reset element', 'R', '', function () { resetOne(selected); });
    btn('Reset all', 'Shift+R', '', resetAll);
    btn('Cancel', 'Esc', '', function () { exit(false); });
    btn('Save', 'Enter', 'hle__btn--go', function () { exit(true); });
    bar.appendChild(row);
    bar.appendChild(el('p', 'hle__hint',
      'Drag to move. Drag the corner, use the wheel or + / - to resize. Arrows nudge, Shift moves further. Tab picks the next element. H hides this panel.'));
    var status = el('p', 'hle__status');
    status.hidden = true;
    bar.appendChild(status);

    root.appendChild(guides);
    root.appendChild(frames);
    root.appendChild(bar);
    var hud = $('hud');
    hud.appendChild(ringBox);
    hud.appendChild(note);
    doc.body.appendChild(root);

    ui = { root: root, guides: guides, frames: frames, bar: bar, sel: sel, snapBtn: snapBtn, status: status,
           ringBox: ringBox, note: note, frame: {} };

    /* the threat ring's frame spans most of the screen: it goes in FIRST so every other frame sits above it */
    var order = [ITEMS[ITEMS.length - 1]].concat(ITEMS.slice(0, ITEMS.length - 1));
    for (var i = 0; i < order.length; i++) {
      (function (it) {
        var f = el('div', 'hle__f');
        f.setAttribute('data-id', it.id);
        var lab = el('span', 'hle__lab');
        lab.appendChild(el('span', 'hle__ln', it.name));
        lab.appendChild(el('span', 'hle__ls', '100%'));
        f.appendChild(lab);
        var h = el('span', 'hle__h');
        h.setAttribute('aria-hidden', 'true');
        f.appendChild(h);
        f.addEventListener('mousedown', function (e) { startDrag(e, it.id, e.target === h ? 'size' : 'move'); });
        frames.appendChild(f);
        ui.frame[it.id] = { f: f, lab: lab, pct: lab.lastChild, h: h };
      })(order[i]);
    }
    root.addEventListener('wheel', onWheel, { passive: false });
    root.addEventListener('mousewheel', onWheel, { passive: false });
  }

  function placeRingBox() {
    if (!ui) { return; }
    var g = ring();
    var s = ui.ringBox.style;
    s.left = g.l + 'vw'; s.right = g.r + 'vw'; s.top = g.t + 'vh'; s.bottom = g.b + 'vh';
  }

  function rectOf(id) {
    var n = node(id);
    if (!n || n.hidden) { return null; }
    var r = n.getBoundingClientRect();
    return r.width > 0 ? r : null;
  }

  /* the corner an element scales from (the one nearest the screen corner it lives in) */
  function anchorOf(id, r) {
    var e = layout.el[id];
    if (e && id !== 'note') { return { ax: e.ax, ay: e.ay }; }
    if (id === 'note') { return { ax: 'c', ay: 't' }; }
    return { ax: (r.left + r.right) / 2 < vw() / 2 ? 'l' : 'r', ay: (r.top + r.bottom) / 2 < vh() / 2 ? 't' : 'b' };
  }

  function scaleOf(id) {
    if (id === 'threats') { return ring().s; }
    var e = layout.el[id];
    return e ? (e.s || 1) : 1;
  }

  function overlaps(a, b) { return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top; }

  function drawFrames() {
    if (!ui) { return; }
    var rects = {};
    for (var q = 0; q < ITEMS.length; q++) { rects[ITEMS[q].id] = rectOf(ITEMS[q].id); }
    for (var i = 0; i < ITEMS.length; i++) {
      var id = ITEMS[i].id, fr = ui.frame[id], r = rects[id];
      if (!r) { fr.f.hidden = true; continue; }
      fr.f.hidden = false;
      var s = fr.f.style;
      s.left = r.left + 'px'; s.top = r.top + 'px'; s.width = r.width + 'px'; s.height = r.height + 'px';
      var a = anchorOf(id, r);
      fr.f.setAttribute('data-h', (a.ay === 'b' ? 't' : 'b') + (a.ax === 'r' ? 'l' : 'r'));   /* handle: the corner opposite the anchor */
      if (id !== 'threats') { fr.f.setAttribute('data-lab', labelSpot(id, r, fr.lab, rects)); }
      fr.f.classList.toggle('is-on', id === selected);
      fr.f.classList.toggle('is-moved', !!(layout.el[id] || (id === 'threats' && layout.ring)));
      var p = Math.round(scaleOf(id) * 100) + '%';
      if (fr.pct.textContent !== p) { fr.pct.textContent = p; }
    }
    ui.sel.textContent = itemName(selected) + '  ' + Math.round(scaleOf(selected) * 100) + '%';
    /* the panel sits in the clear centre of the screen (the HUD keeps it empty for the crosshair); while the element
       being placed is there it moves to the top, or to the bottom if that is taken too */
    /* Of the three spots, the one covering the least of the HUD wins, the element being placed counting four times
       (on a 1440 screen the stock plates reach the centre, so no spot is always free). Ties keep the centre. */
    var rb = ui.bar.getBoundingClientRect(), H = vh();
    var spots = [['mid', H * 0.36], ['top', H * 0.03], ['bottom', H * 0.97 - rb.height]], best = null;
    for (var z = 0; z < spots.length; z++) {
      var zr = { left: rb.left, right: rb.right, top: spots[z][1], bottom: spots[z][1] + rb.height }, cost = 0;
      for (var o in rects) {
        var rr = rects[o];
        if (!rr || o === 'threats') { continue; }
        var ox = Math.min(zr.right, rr.right) - Math.max(zr.left, rr.left), oy = Math.min(zr.bottom, rr.bottom) - Math.max(zr.top, rr.top);
        if (ox > 0 && oy > 0) { cost += ox * oy * (o === selected ? 4 : 1); }
      }
      if (!best || cost < best.cost - 1) { best = { k: spots[z][0], cost: cost }; }
    }
    if (best && ui.bar.getAttribute('data-pos') !== best.k) { ui.bar.setAttribute('data-pos', best.k); }
  }

  /* An element's name goes above its box; if that is off screen or on another element, below it; else inside. */
  function labelSpot(id, r, lab, rects) {
    var lr = lab.getBoundingClientRect(), h = lr.height || 30, w = lr.width || 200;
    var spots = [{ k: 'out', top: r.top - h }, { k: 'below', top: r.bottom }];
    for (var i = 0; i < spots.length; i++) {
      var b = { left: r.left, right: r.left + w, top: spots[i].top, bottom: spots[i].top + h };
      if (b.top < 0 || b.bottom > vh()) { continue; }
      var clear = true;
      for (var o in rects) {
        if (o === id || o === 'threats' || !rects[o]) { continue; }
        if (overlaps(b, rects[o])) { clear = false; break; }
      }
      if (clear) { return spots[i].k; }
    }
    return 'in';
  }

  function status(text, bad) {
    if (!ui) { return; }
    ui.status.hidden = !text;
    ui.status.textContent = text || '';
    ui.status.setAttribute('data-tone', bad ? 'bad' : 'info');
  }

  function setSnap(on) {
    snapOn = !!on;
    if (ui) {
      ui.snapBtn.firstChild.textContent = snapOn ? 'Snap on' : 'Snap off';
      ui.snapBtn.classList.toggle('is-off', !snapOn);
    }
  }

  function select(id) { selected = id; drawFrames(); }

  /* Store where an element is now (rect r, after any scale) as a saved entry. */
  function commitRect(id, r, s) {
    var W = vw(), H = vh();
    if (id === 'threats') {
      var g = ring();
      layout.ring = { l: clamp(r.left / W * 100, 0, 45), r: clamp((W - r.right) / W * 100, 0, 45),
                      t: clamp(r.top / H * 100, 0, 45), b: clamp((H - r.bottom) / H * 100, 0, 45), s: s || g.s };
      applyRing();
      version++;
      return;
    }
    if (id === 'note') {
      layout.el.note = { ax: 'l', ay: 't', x: clamp((r.left + r.right) / 2 / W, 0.05, 0.95), y: clamp(r.top / H, 0, 0.92), s: s || scaleOf('note') };
      applyNote();
      return;
    }
    var cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    var ax = cx < W / 2 ? 'l' : 'r', ay = cy < H / 2 ? 't' : 'b';
    layout.el[id] = { ax: ax, ay: ay,
                      x: clamp(ax === 'l' ? r.left / W : (W - r.right) / W, 0, 1),
                      y: clamp(ay === 't' ? r.top / H : (H - r.bottom) / H, 0, 1),
                      s: clamp(s || scaleOf(id), S_MIN, S_MAX) };
    applyOne(id);
    clampOne(id);
    version++;
  }

  function moveBy(id, dx, dy) {
    var r = rectOf(id);
    if (!r) { return; }
    var W = vw(), H = vh();
    dx = clamp(dx, -r.left, W - r.right); dy = clamp(dy, -r.top, H - r.bottom);
    commitRect(id, { left: r.left + dx, right: r.right + dx, top: r.top + dy, bottom: r.bottom + dy }, scaleOf(id));
    drawFrames();
  }

  function scaleBy(id, f) {
    if (id === 'threats') {
      var g = ring();
      layout.ring = { l: g.l, r: g.r, t: g.t, b: g.b, s: clamp(g.s * f, S_MIN, S_MAX) };
      applyRing(); version++; drawFrames();
      return;
    }
    var r = rectOf(id);
    if (!r) { return; }
    var s0 = scaleOf(id), s1 = clamp(Math.round(s0 * f * 100) / 100, S_MIN, S_MAX);
    if (s1 === s0) { return; }
    /* grow from the anchor corner: that corner stays where it is */
    var a = anchorOf(id, r), k = s1 / s0, w = r.width * k, h = r.height * k, nr;
    var left = a.ax === 'r' ? r.right - w : (a.ax === 'c' ? (r.left + r.right) / 2 - w / 2 : r.left);
    var top = a.ay === 'b' ? r.bottom - h : r.top;
    nr = { left: left, right: left + w, top: top, bottom: top + h };
    if (!layout.el[id] && id !== 'note') { layout.el[id] = { ax: a.ax === 'r' ? 'r' : 'l', ay: a.ay, x: 0, y: 0, s: s0 }; }
    commitRect(id, nr, s1);
    drawFrames();
  }

  function resetOne(id) {
    if (id === 'threats') { delete layout.ring; applyRing(); }
    else { delete layout.el[id]; applyOne(id); }
    version++;
    drawFrames();
    status(itemName(id) + ' is back where it started.');
  }
  function resetAll() {
    layout = { v: 1, el: {} };
    applyAll();
    drawFrames();
    status('Every element is back where it started.');
  }

  /* ---- dragging ---- */
  var drag = null;

  function snapTargets(id) {
    var W = vw(), H = vh(), xs = [W / 2, W * 0.026, W - W * 0.026], ys = [H / 2, H * 0.042, H - H * 0.14, H - H * 0.30];
    for (var i = 0; i < ITEMS.length; i++) {
      var o = ITEMS[i].id;
      if (o === id || o === 'threats') { continue; }
      var r = rectOf(o);
      if (!r) { continue; }
      xs.push(r.left, r.right, (r.left + r.right) / 2);
      ys.push(r.top, r.bottom, (r.top + r.bottom) / 2);
    }
    return { xs: xs, ys: ys };
  }
  function snapAxis(edges, targets) {
    var best = null;
    for (var i = 0; i < edges.length; i++) {
      for (var j = 0; j < targets.length; j++) {
        var d = targets[j] - edges[i];
        if (Math.abs(d) <= SNAP_PX && (!best || Math.abs(d) < Math.abs(best.d))) { best = { d: d, at: targets[j] }; }
      }
    }
    return best;
  }
  function showGuides(gx, gy) {
    var g = ui.guides;
    while (g.firstChild) { g.removeChild(g.firstChild); }
    if (gx !== null) { var a = el('span', 'hle__gx'); a.style.left = gx + 'px'; g.appendChild(a); }
    if (gy !== null) { var b = el('span', 'hle__gy'); b.style.top = gy + 'px'; g.appendChild(b); }
  }

  function startDrag(e, id, mode) {
    if (e.button !== undefined && e.button !== 0) { return; }
    e.preventDefault();
    e.stopPropagation();
    select(id);
    var r = rectOf(id);
    if (!r) { return; }
    var a = anchorOf(id, r);
    drag = { id: id, mode: mode, x0: e.clientX, y0: e.clientY, r: { left: r.left, right: r.right, top: r.top, bottom: r.bottom },
             s0: scaleOf(id), a: a, snaps: snapOn ? snapTargets(id) : null };
    ui.root.classList.add('is-dragging');
  }

  function onMove(e) {
    if (!drag) { return; }
    var dx = e.clientX - drag.x0, dy = e.clientY - drag.y0, r0 = drag.r, W = vw(), H = vh();
    if (drag.mode === 'move') {
      var nl = clamp(r0.left + dx, 0, W - (r0.right - r0.left)), nt = clamp(r0.top + dy, 0, H - (r0.bottom - r0.top));
      var w = r0.right - r0.left, h = r0.bottom - r0.top, gx = null, gy = null;
      if (drag.snaps) {
        var sx = snapAxis([nl, nl + w, nl + w / 2], drag.snaps.xs);
        var sy = snapAxis([nt, nt + h, nt + h / 2], drag.snaps.ys);
        if (sx) { nl = clamp(nl + sx.d, 0, W - w); gx = sx.at; }
        if (sy) { nt = clamp(nt + sy.d, 0, H - h); gy = sy.at; }
      }
      showGuides(gx, gy);
      commitRect(drag.id, { left: nl, right: nl + w, top: nt, bottom: nt + h }, drag.s0);
    } else if (drag.id === 'threats') {
      /* the ring's handle drags its far corner: the rectangle itself changes size */
      var corner = { left: r0.left, top: r0.top, right: r0.right, bottom: r0.bottom };
      var hdl = ui.frame.threats.f.getAttribute('data-h');
      if (hdl.charAt(1) === 'r') { corner.right = clamp(r0.right + dx, r0.left + W * 0.2, W); } else { corner.left = clamp(r0.left + dx, 0, r0.right - W * 0.2); }
      if (hdl.charAt(0) === 'b') { corner.bottom = clamp(r0.bottom + dy, r0.top + H * 0.2, H); } else { corner.top = clamp(r0.top + dy, 0, r0.bottom - H * 0.2); }
      commitRect('threats', corner, ring().s);
    } else {
      /* scale from the anchor corner by how far the handle moved away from it */
      var ax = drag.a.ax === 'r' ? r0.right : (drag.a.ax === 'c' ? (r0.left + r0.right) / 2 : r0.left);
      var ay = drag.a.ay === 'b' ? r0.bottom : r0.top;
      var hx = drag.a.ax === 'r' ? r0.left : r0.right, hy = drag.a.ay === 'b' ? r0.top : r0.bottom;
      var d0 = Math.sqrt((hx - ax) * (hx - ax) + (hy - ay) * (hy - ay)) || 1;
      var d1 = Math.sqrt((hx + dx - ax) * (hx + dx - ax) + (hy + dy - ay) * (hy + dy - ay));
      var s1 = clamp(Math.round(drag.s0 * d1 / d0 * 100) / 100, S_MIN, S_MAX), k = s1 / drag.s0;
      var w2 = (r0.right - r0.left) * k, h2 = (r0.bottom - r0.top) * k;
      var l2 = drag.a.ax === 'r' ? r0.right - w2 : (drag.a.ax === 'c' ? ax - w2 / 2 : r0.left);
      var t2 = drag.a.ay === 'b' ? r0.bottom - h2 : r0.top;
      if (!layout.el[drag.id] && drag.id !== 'note') { layout.el[drag.id] = { ax: drag.a.ax === 'r' ? 'r' : 'l', ay: drag.a.ay, x: 0, y: 0, s: drag.s0 }; }
      commitRect(drag.id, { left: l2, right: l2 + w2, top: t2, bottom: t2 + h2 }, s1);
    }
    drawFrames();
  }

  function onUp() {
    if (!drag) { return; }
    drag = null;
    ui.root.classList.remove('is-dragging');
    showGuides(null, null);
    drawFrames();
  }

  function onWheel(e) {
    if (!editing) { return; }
    e.preventDefault();
    var dy = e.deltaY !== undefined ? e.deltaY : -(e.wheelDelta || 0);
    var hov = e.target && e.target.closest ? e.target.closest('.hle__f') : null;
    var id = hov ? hov.getAttribute('data-id') : selected;
    if (id !== selected) { select(id); }
    scaleBy(id, dy < 0 ? 1 + S_STEP : 1 / (1 + S_STEP));
  }

  function onKey(e) {
    if (!editing) { return; }
    var k = keyOf(e), big = !!e.shiftKey;
    var step = big ? 0.02 : 0.0025, handled = true;
    switch (k) {
      case 'Escape': exit(false); break;
      case 'Enter': exit(true); break;
      case 'Tab': {
        var i = 0;
        for (var j = 0; j < ITEMS.length; j++) { if (ITEMS[j].id === selected) { i = j; } }
        for (var n = 1; n <= ITEMS.length; n++) {
          var c = ITEMS[(i + (big ? -n : n) + ITEMS.length * 2) % ITEMS.length].id;
          if (rectOf(c)) { select(c); break; }
        }
        break;
      }
      case 'ArrowLeft': moveBy(selected, -vw() * step, 0); break;
      case 'ArrowRight': moveBy(selected, vw() * step, 0); break;
      case 'ArrowUp': moveBy(selected, 0, -vh() * step); break;
      case 'ArrowDown': moveBy(selected, 0, vh() * step); break;
      case '+': scaleBy(selected, 1 + S_STEP); break;
      case '-': scaleBy(selected, 1 / (1 + S_STEP)); break;
      case 'r': case 'R': if (big) { resetAll(); } else { resetOne(selected); } break;
      case 'g': case 'G': setSnap(!snapOn); break;
      case 'h': case 'H': if (ui) { ui.bar.hidden = !ui.bar.hidden; } break;   /* on a small screen the panel can hide what you place */
      default: handled = false;
    }
    if (handled) { e.preventDefault(); e.stopPropagation(); }
  }

  /* ---- in / out ---- */
  function send(obj) {
    var s = JSON.stringify(obj);
    if (typeof global.cwHudRequest === 'function') { global.cwHudRequest(s); return true; }
    return false;
  }

  function enter(saved) {
    if (editing) { return; }
    if (saved) { setLayout(saved); }
    editing = true;
    if (CW.HudBridge && CW.HudBridge.hold) { CW.HudBridge.hold(true); }
    var hud = $('hud');
    hud.setAttribute('data-edit', 'true');
    if (CW.Hud) { CW.Hud.render(demoSnapshot()); }
    build();
    applyAll();
    /* Cancel goes back to this: the layout as it stands on THIS screen (applyAll has just pulled anything that was
       off screen back on, which is not an edit to undo) */
    before = clone(layout);
    setSnap(snapOn);
    placeRingBox();
    drawFrames();
    status('');
  }

  function exit(save) {
    if (!editing) { return; }
    if (save) {
      if (!send({ op: 'saveLayout', layout: layout })) {
        try { global.localStorage.setItem(LS_KEY, JSON.stringify(layout)); } catch (e) { /* no storage here */ }
      }
    } else {
      setLayout(before);
    }
    editing = false;
    if (ui) {
      if (ui.root.parentNode) { ui.root.parentNode.removeChild(ui.root); }
      if (ui.ringBox.parentNode) { ui.ringBox.parentNode.removeChild(ui.ringBox); }
      if (ui.note.parentNode) { ui.note.parentNode.removeChild(ui.note); }
      ui = null;
    }
    $('hud').removeAttribute('data-edit');
    version++;
    send({ op: 'editDone' });
    if (CW.HudBridge && CW.HudBridge.hold) { CW.HudBridge.hold(false); }
  }

  /* ---- wiring ---- */
  doc.addEventListener('keydown', onKey, true);
  doc.addEventListener('mousemove', onMove, true);
  doc.addEventListener('mouseup', onUp, true);
  global.addEventListener('resize', function () {
    applyAll();
    if (ui) { placeRingBox(); drawFrames(); }
  });

  /* DLL -> page */
  global.cwHudEdit = function (m) {
    if (typeof m === 'string') { try { m = JSON.parse(m); } catch (e) { return; } }
    if (!m) { return; }
    if (m.on) { enter(m.layout); } else { exit(!!m.save); }
  };
  global.cwHudSaved = function (m) {
    if (typeof m === 'string') { try { m = JSON.parse(m); } catch (e) { return; } }
    if (m && m.ok === false && CW.Hud) { CW.Hud.event({ type: 'note', text: 'HUD LAYOUT NOT SAVED: ' + String(m.error || ''), tone: 'bad' }); }
  };

  CW.HudLayout = {
    ITEMS: ITEMS,
    /* the snapshot's cfg.layout (bridge.js); ignored while the editor is open, which owns the layout then */
    set: function (l) { if (!editing) { setLayout(l); } },
    get: function () { return clone(layout); },
    ring: ring,
    version: function () { return version; },
    editing: function () { return editing; },
    clampOne: function (id) { if (layout.el[id]) { clampOne(id); } },
    edit: function (on, save) { if (on) { enter(null); } else { exit(!!save); } }
  };

  /* A browser / the harness: the last saved layout from localStorage; ?edit=1 opens the editor. In game the plugin
     sends the layout instead (cfg.layout) and opens the editor itself. */
  var inGame = /Ultralight/i.test((global.navigator && global.navigator.userAgent) || '') || !!global.prisma;
  if (!inGame) {
    try { var ls = global.localStorage.getItem(LS_KEY); if (ls) { setLayout(JSON.parse(ls)); } } catch (e) { /* none */ }
    if (/[?&]edit=1/.test(String(global.location && global.location.search || ''))) {
      global.addEventListener('load', function () { enter(null); });   /* after hud.js and mock.js are in */
    }
  }
})(this);
