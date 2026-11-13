/* customize.js -- the ripperdoc's Customize page: per-target colour + glow, and the two effect switches.
 *
 * Contract: UI-API.md section 9 (setCustomize / resetCustomize, state.customize). All game state stays in the DLL;
 * this page sends one request per change and re-renders from the state that comes back. The colour change reaches
 * the worn cyberware when the menu closes (the game is paused behind this page), so the preview on this page is the
 * live feedback, and the HUD accent recolours the HUD the same way.
 *
 * Ultralight limits are the menu's own (styles.css header): no filter, no box-shadow, no pointer events assumed.
 */
(function (global) {
  'use strict';
  var CW = global.CW = global.CW || {};

  var TARGETS = [
    { id: 'mantis',   name: 'Mantis Blades',       sub: 'Cutting edge',  item: 'cw_ar_mantis',   icon: 'icons/cw_arms_mantisblades.png',       fb: 'icons/slot-arms.png',    stock: '#8cccff', stage: 'blade' },
    { id: 'monowire', name: 'Monowire',            sub: 'Wire',          item: 'cw_ar_monowire', icon: 'icons/cw_arms_nanowires.png',            fb: 'icons/slot-arms.png',    stock: '#ccb54c', stage: 'wire' },
    { id: 'gorilla',  name: 'Gorilla Arms',        sub: 'Knuckle lights', item: 'cw_ar_gorilla', icon: 'icons/cw_arms_strongarms.png',       fb: 'icons/slot-hands.png',   stock: '#ff7a2f', stage: 'knuckles' },
    { id: 'launcher', name: 'Projectile Launcher', sub: 'Arm lights',    item: 'cw_ar_pls',      icon: 'icons/cw_arms_projectilelauncher.png', fb: 'icons/slot-arms.png',    stock: '#ff7a2f', stage: 'launcher' },
    { id: 'kiroshi',  name: 'Kiroshi Optics',      sub: 'Scan colour', item: 'cw_face_kiroshi_basic', icon: 'icons/cw_eyes_kiroshioptics.png',      fb: 'icons/slot-ocular.png',  stock: '#ff3b47', stage: 'eye', noGlow: true },
    { id: 'sandevistan', name: 'Sandevistan',      sub: 'Trail + slow-time tint', item: 'cw_os_sandy_dynalar', icon: 'icons/cw_system_sandevistanc2.png', fb: 'icons/slot-operating-system.png', stock: '#39ff7a', stage: 'hud', noGlow: true },
    { id: 'hud',      name: 'HUD Accent',          sub: 'Overlay',       icon: 'icons/slot-nervous.png',        fb: 'icons/slot-nervous.png', stock: '#fcee0a', stage: 'hud', noGlow: true, noStock: true }
  ];
  var SWATCHES = [
    { name: 'Teal', hex: '#00e5d0' }, { name: 'Red', hex: '#ff2a3c' }, { name: 'Yellow', hex: '#fcee0a' },
    { name: 'Magenta', hex: '#ff2bd6' }, { name: 'White', hex: '#f4faff' }
  ];
  var GLOWS = [ { label: 'Off', v: 0 }, { label: 'Subtle', v: 25 }, { label: 'Normal', v: 50 }, { label: 'Bright', v: 85 } ];

  var ui = { target: 'mantis', hsv: { h: 180, s: 1, v: 1 }, built: false };
  var root, refs = {};

  function $(id) { return document.getElementById(id); }
  function el(tag, cls, text) { var e = document.createElement(tag); if (cls) { e.className = cls; } if (text != null) { e.textContent = text; } return e; }
  function clamp(x, a, b) { return Math.min(b, Math.max(a, x)); }
  /* In PrismaUI/Ultralight e.key is 'Unidentified' and e.code is empty: fall back to keyCode (MENU POLISH's shared CW.keyOf when present). */
  var KEY_CODES = { 13: 'Enter', 27: 'Escape', 37: 'ArrowLeft', 38: 'ArrowUp', 39: 'ArrowRight', 40: 'ArrowDown' };
  function keyOf(e) {
    if (typeof CW.keyOf === 'function') { return CW.keyOf(e); }
    return (e.key && e.key !== 'Unidentified') ? e.key : (KEY_CODES[e.keyCode] || '');
  }
  function tdef(id) { for (var i = 0; i < TARGETS.length; i++) { if (TARGETS[i].id === id) { return TARGETS[i]; } } return TARGETS[0]; }

  /* ---- colour maths ------------------------------------------------- */
  function hex2rgb(h) { var v = parseInt(h.slice(1), 16); return [(v >> 16) & 255, (v >> 8) & 255, v & 255]; }
  function rgb2hex(r, g, b) { function p(n) { var s = Math.round(n).toString(16); return s.length < 2 ? '0' + s : s; } return '#' + p(r) + p(g) + p(b); }
  function hsv2rgb(h, s, v) {
    var c = v * s, x = c * (1 - Math.abs(((h / 60) % 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    return [(r + m) * 255, (g + m) * 255, (b + m) * 255];
  }
  function rgb2hsv(r, g, b) {
    r /= 255; g /= 255; b /= 255;
    var mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn, h = 0;
    if (d) { if (mx === r) { h = ((g - b) / d) % 6; } else if (mx === g) { h = (b - r) / d + 2; } else { h = (r - g) / d + 4; } h *= 60; if (h < 0) { h += 360; } }
    return { h: h, s: mx ? d / mx : 0, v: mx };
  }
  function rgba(hex, a) { var c = hex2rgb(hex); return 'rgba(' + c[0] + ',' + c[1] + ',' + c[2] + ',' + a + ')'; }

  /* ---- state --------------------------------------------------------- */
  function custom() {
    var s = CW.Data && CW.Data.state && CW.Data.state();
    return (s && s.customize) || { targets: {}, seams: false, pulses: true };
  }
  function cur(id) { var c = custom().targets[id] || {}; return { color: c.color === undefined ? null : c.color, glow: c.glow === undefined ? 50 : c.glow }; }
  function effective(id) { var c = cur(id).color; return c || tdef(id).stock; }

  var pending = null, timer = 0;
  function flush() {
    timer = 0;
    if (!pending) { return; }
    var args = pending; pending = null;
    CW.Bridge.request('setCustomize', args).then(function (res) {
      CW.Data.absorb(res);
      refreshChips();
    }, function (e) { if (CW.Customize.onError) { CW.Customize.onError(e.message || 'Could not save that.'); } });
  }
  function send(args, immediate) {
    args.target = args.target || ui.target;
    pending = merge(pending, args);
    if (immediate) { if (timer) { clearTimeout(timer); } flush(); return; }
    if (!timer) { timer = setTimeout(flush, 120); }
  }
  function merge(a, b) { var o = {}, k; for (k in a) { if (a.hasOwnProperty(k)) { o[k] = a[k]; } } for (k in b) { if (b.hasOwnProperty(k)) { o[k] = b[k]; } } return o; }

  /* ---- preview ---------------------------------------------------------
     Each target gets its own schematic built from clip-path shapes and gradients. Colour and strength are CSS
     variables, so moving a slider restyles it without rebuilding anything. */
  function stageHtml(kind) {
    switch (kind) {
      case 'blade':
        return '<div class="pv pv--blade"><div class="pvb__halo"></div><div class="pvb__body"></div><div class="pvb__spine"></div><div class="pvb__edge"></div></div>';
      case 'wire':
        return '<svg class="pv pv--wire" viewBox="0 0 800 320" aria-hidden="true"><path class="pvw__halo" d="M40 262 C 190 30, 330 320, 520 150 S 720 40, 772 170"/><path class="pvw__mid" d="M40 262 C 190 30, 330 320, 520 150 S 720 40, 772 170"/><path class="pvw__core" d="M40 262 C 190 30, 330 320, 520 150 S 720 40, 772 170"/></svg>';
      case 'knuckles':
        return '<div class="pv pv--knuckles">' + [0, 1, 2, 3].map(function () { return '<div class="pvk"><span class="pvk__halo"></span><span class="pvk__plate"></span><span class="pvk__lamp"></span></div>'; }).join('') + '</div>';
      case 'launcher':
        return '<div class="pv pv--launcher"><div class="pvl__halo"></div><div class="pvl__tube"></div><div class="pvl__ring r1"></div><div class="pvl__ring r2"></div><div class="pvl__ring r3"></div><div class="pvl__bore"></div></div>';
      case 'eye':
        return '<div class="pv pv--eye"><div class="pve__halo"></div><div class="pve__ball"></div><div class="pve__iris"></div><div class="pve__pupil"></div><div class="pve__read"><b>OPTICS</b><span>SCAN&nbsp;&nbsp;48.2 M</span></div></div>';
      default:
        return '<div class="pv pv--hud"><div class="pvh__plate"><span class="pvh__tag">OPERATING SYSTEM</span><div class="pvh__row"><span class="pvh__ring"><b>8.0</b><i>SEC</i></span><span class="pvh__name">Sandevistan</span></div><div class="pvh__bar"><span></span></div></div></div>';
    }
  }

  function applyTint() {
    var id = ui.target, t = tdef(id), c = cur(id);
    var hex = rgb2hex.apply(null, hsv2rgb(ui.hsv.h, ui.hsv.s, ui.hsv.v));
    var g = t.noGlow ? 1 : clamp(c.glow / 70, 0, 1.25);
    root.style.setProperty('--tint', hex);
    root.style.setProperty('--tint-soft', rgba(hex, 0.14 + 0.5 * Math.min(g, 1)));
    root.style.setProperty('--tint-wide', rgba(hex, 0.05 + 0.26 * Math.min(g, 1)));
    root.style.setProperty('--g', String(Math.min(g, 1)));
    root.style.setProperty('--hue', 'hsl(' + ui.hsv.h + ',100%,50%)');
    if (refs.sv) {
      refs.svMark.style.left = (ui.hsv.s * 100) + '%';
      refs.svMark.style.top = ((1 - ui.hsv.v) * 100) + '%';
      refs.hueMark.style.left = (ui.hsv.h / 360 * 100) + '%';
      refs.sv.setAttribute('aria-valuetext', hex);
    }
    if (refs.hex && document.activeElement !== refs.hex) { refs.hex.value = hex.toUpperCase(); }
    if (refs.glowFill) {
      refs.glowFill.style.width = c.glow + '%'; refs.glowKnob.style.left = c.glow + '%';
      refs.glowNum.textContent = c.glow;
      for (var i = 0; i < refs.glowSeg.length; i++) { refs.glowSeg[i].classList.toggle('is-on', Number(refs.glowSeg[i].dataset.v) === c.glow); }
    }
  }

  function setColor(hex, keepHsv) {
    if (!keepHsv) { var c = hex2rgb(hex); var h = rgb2hsv(c[0], c[1], c[2]); if (h.s === 0) { h.h = ui.hsv.h; } ui.hsv = h; }
    applyTint();
    markSwatches();
  }
  function pickHsv() {
    var hex = rgb2hex.apply(null, hsv2rgb(ui.hsv.h, ui.hsv.s, ui.hsv.v));
    applyTint(); markSwatches();
    send({ color: hex });
  }
  function markSwatches() {
    var color = cur(ui.target).color;
    var hex = color ? color.toLowerCase() : null;
    for (var i = 0; i < refs.swatches.length; i++) {
      var b = refs.swatches[i], v = b.dataset.hex;
      b.classList.toggle('is-on', v === 'stock' ? !hex : v === hex);
    }
  }

  /* ---- drag helper: Ultralight gives mouse events, not pointer events --- */
  function drag(target, fn) {
    function pos(e) {
      var r = target.getBoundingClientRect();
      fn(clamp((e.clientX - r.left) / r.width, 0, 1), clamp((e.clientY - r.top) / r.height, 0, 1));
    }
    target.addEventListener('mousedown', function (e) {
      e.preventDefault();
      pos(e);
      function move(ev) { pos(ev); }
      function up() { document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up); send({}, true); }
      document.addEventListener('mousemove', move);
      document.addEventListener('mouseup', up);
    });
  }

  /* ---- build (once) ---------------------------------------------------- */
  function build() {
    root = $('view-customize');
    root.innerHTML = '';

    /* column 1: what to colour */
    var left = el('section', 'cz__col cz__list');
    left.appendChild(el('h2', 'pane__h', 'Customize'));
    var list = el('div', 'cz__targets'); list.setAttribute('role', 'listbox'); list.setAttribute('aria-label', 'What to colour');
    TARGETS.forEach(function (t) {
      var b = el('button', 'cz__t'); b.type = 'button'; b.dataset.id = t.id; b.setAttribute('role', 'option');
      var ic = el('span', 'cz__ticon'); var img = el('img'); img.alt = ''; img.width = 56; img.height = 56;
      var cat = t.item && CW.Data.item && CW.Data.item(t.item);   /* the catalog's own art first, so it cannot drift */
      img.src = (cat && cat.icon) || t.icon;
      img.onerror = function () { if (img.src.indexOf(t.fb) < 0) { img.src = t.fb; } };
      ic.appendChild(img); b.appendChild(ic);
      var tx = el('span', 'cz__tt'); tx.appendChild(el('span', 'cz__tn', t.name)); tx.appendChild(el('span', 'cz__ts', t.sub)); b.appendChild(tx);
      var chip = el('span', 'cz__chip'); chip.dataset.chip = t.id; b.appendChild(chip);
      b.addEventListener('click', function () { selectTarget(t.id); });
      list.appendChild(b);
    });
    left.appendChild(list);
    var fx = el('div', 'cz__fx');
    fx.appendChild(el('h3', 'cz__h', 'Effects'));
    fx.appendChild(toggleRow('seams', 'Idle glow', 'A slow breath on anything that glows'));
    fx.appendChild(toggleRow('pulses', 'Ability pulse', 'A flash on Sandevistan, Berserk or Blood Pump'));
    var test = el('button', 'cz__ghost', 'Test pulse'); test.type = 'button';
    test.addEventListener('click', function () { flashStage(); });
    fx.appendChild(test);
    left.appendChild(fx);
    root.appendChild(left);

    /* column 2: live preview */
    var mid = el('section', 'cz__col cz__prev');
    var phead = el('div', 'cz__ph');
    refs.pName = el('h2', 'pane__h'); phead.appendChild(refs.pName);
    refs.pSub = el('span', 'pane__sub'); phead.appendChild(refs.pSub);
    mid.appendChild(phead);
    refs.stage = el('div', 'cz__stage'); refs.stage.setAttribute('aria-hidden', 'true');
    mid.appendChild(refs.stage);
    refs.note = el('p', 'cz__note');
    mid.appendChild(refs.note);
    root.appendChild(mid);

    /* column 3: picker */
    var right = el('section', 'cz__col cz__pick');
    right.appendChild(el('h3', 'cz__h', 'Colour'));
    var sw = el('div', 'seg seg--6 cz__sw'); refs.swatches = [];
    function addSwatch(hex, label) {
      var b = el('button', 'cz__swatch'); b.type = 'button'; b.dataset.hex = hex; b.title = label;
      b.setAttribute('aria-label', label);
      var fill = el('span', 'cz__swfill'); if (hex === 'stock') { b.classList.add('cz__swatch--stock'); } else { fill.style.background = hex; }
      b.appendChild(fill); b.appendChild(el('span', 'cz__swl', label));
      b.addEventListener('click', function () {
        if (hex === 'stock') { send({ color: null }, true); setTimeout(function () { setColor(tdef(ui.target).stock); }, 0); }
        else { setColor(hex); send({ color: hex }, true); }
      });
      sw.appendChild(b); refs.swatches.push(b);
    }
    addSwatch('stock', 'Stock');
    SWATCHES.forEach(function (s) { addSwatch(s.hex, s.name); });
    right.appendChild(sw);

    refs.sv = el('div', 'cz__sv'); refs.sv.tabIndex = 0; refs.sv.setAttribute('role', 'slider'); refs.sv.setAttribute('aria-label', 'Saturation and brightness');
    refs.svMark = el('span', 'cz__mark'); refs.sv.appendChild(refs.svMark);
    drag(refs.sv, function (x, y) { ui.hsv.s = x; ui.hsv.v = 1 - y; pickHsv(); });
    refs.sv.addEventListener('keydown', function (e) {
      var k = keyOf(e), d = k === 'ArrowLeft' ? [-1, 0] : k === 'ArrowRight' ? [1, 0] : k === 'ArrowUp' ? [0, 1] : k === 'ArrowDown' ? [0, -1] : null;
      if (!d) { return; } e.preventDefault(); e.stopPropagation();
      ui.hsv.s = clamp(ui.hsv.s + d[0] * 0.03, 0, 1); ui.hsv.v = clamp(ui.hsv.v + d[1] * 0.03, 0, 1); pickHsv();
    });
    right.appendChild(refs.sv);

    refs.hue = el('div', 'cz__hue'); refs.hue.tabIndex = 0; refs.hue.setAttribute('role', 'slider'); refs.hue.setAttribute('aria-label', 'Hue');
    refs.hueMark = el('span', 'cz__knob'); refs.hue.appendChild(refs.hueMark);
    drag(refs.hue, function (x) { ui.hsv.h = x * 359.9; pickHsv(); });
    refs.hue.addEventListener('keydown', function (e) {
      var k = keyOf(e), d = k === 'ArrowLeft' ? -6 : k === 'ArrowRight' ? 6 : 0;
      if (!d) { return; } e.preventDefault(); e.stopPropagation();
      ui.hsv.h = (ui.hsv.h + d + 360) % 360; pickHsv();
    });
    right.appendChild(refs.hue);

    var hexRow = el('label', 'cz__hexrow'); hexRow.appendChild(el('span', 'cz__k', 'Hex'));
    refs.hex = el('input', 'cz__hex'); refs.hex.type = 'text'; refs.hex.maxLength = 7; refs.hex.spellcheck = false; refs.hex.autocomplete = 'off';
    refs.hex.setAttribute('aria-label', 'Colour as hex');
    refs.hex.addEventListener('input', function () {
      var v = refs.hex.value.trim(); if (v[0] !== '#') { v = '#' + v; }
      if (/^#[0-9a-fA-F]{6}$/.test(v)) { setColor(v.toLowerCase()); send({ color: v.toLowerCase() }); }
    });
    refs.hex.addEventListener('keydown', function (e) { e.stopPropagation(); var k = keyOf(e); if (k === 'Enter') { refs.hex.blur(); send({}, true); } else if (k === 'Escape') { refs.hex.blur(); } });
    hexRow.appendChild(refs.hex);
    right.appendChild(hexRow);

    refs.glowWrap = el('div', 'cz__glow');
    var gh = el('div', 'cz__gh'); gh.appendChild(el('h3', 'cz__h', 'Glow')); refs.glowNum = el('span', 'cz__num'); gh.appendChild(refs.glowNum); refs.glowWrap.appendChild(gh);
    var seg = el('div', 'seg seg--4'); refs.glowSeg = [];
    GLOWS.forEach(function (g) {
      var b = el('button', 'seg__c', g.label); b.type = 'button'; b.dataset.v = g.v;
      b.addEventListener('click', function () { send({ glow: g.v }, true); localGlow(g.v); });
      seg.appendChild(b); refs.glowSeg.push(b);
    });
    refs.glowWrap.appendChild(seg);
    var slider = el('div', 'cz__slider'); slider.tabIndex = 0; slider.setAttribute('role', 'slider'); slider.setAttribute('aria-label', 'Glow strength'); slider.setAttribute('aria-valuemin', '0'); slider.setAttribute('aria-valuemax', '100');
    refs.glowFill = el('span', 'cz__sfill'); refs.glowKnob = el('span', 'cz__knob'); slider.appendChild(refs.glowFill); slider.appendChild(refs.glowKnob);
    drag(slider, function (x) { var v = Math.round(x * 100); localGlow(v); send({ glow: v }); });
    slider.addEventListener('keydown', function (e) {
      var k = keyOf(e), d = k === 'ArrowLeft' ? -5 : k === 'ArrowRight' ? 5 : 0;
      if (!d) { return; } e.preventDefault(); e.stopPropagation();
      var v = clamp(cur(ui.target).glow + d, 0, 100); localGlow(v); send({ glow: v }, true);
    });
    refs.glowWrap.appendChild(slider);
    right.appendChild(refs.glowWrap);

    var reset = el('button', 'cz__ghost', 'Reset this one'); reset.type = 'button';
    reset.addEventListener('click', function () {
      CW.Bridge.request('resetCustomize', { target: ui.target }).then(function (res) { CW.Data.absorb(res); renderSelection(); refreshChips(); });
    });
    right.appendChild(reset);
    root.appendChild(right);

    ui.built = true;
  }

  function localGlow(v) { var s = CW.Data.state(); if (s && s.customize && s.customize.targets[ui.target]) { s.customize.targets[ui.target].glow = v; } applyTint(); }

  function toggleRow(key, label, sub) {
    var b = el('button', 'cz__toggle'); b.type = 'button'; b.setAttribute('role', 'switch'); b.dataset.key = key;
    var t = el('span', 'cz__tt'); t.appendChild(el('span', 'cz__tn', label)); t.appendChild(el('span', 'cz__ts', sub)); b.appendChild(t);
    b.appendChild(el('span', 'cz__sw2'));
    b.addEventListener('click', function () { var o = {}; o[key] = !custom()[key]; delete o.target; pending = null; CW.Bridge.request('setCustomize', o).then(function (r) { CW.Data.absorb(r); refreshChips(); }); });
    return b;
  }

  function flashStage() {
    refs.stage.classList.remove('is-flash');
    void refs.stage.offsetWidth;   // restart the one-shot
    refs.stage.classList.add('is-flash');
  }

  function selectTarget(id) {
    if (pending) { flush(); }
    ui.target = id;
    renderSelection();
  }

  function renderSelection() {
    var t = tdef(ui.target);
    var rows = root.querySelectorAll('.cz__t');
    for (var i = 0; i < rows.length; i++) { var on = rows[i].dataset.id === ui.target; rows[i].classList.toggle('is-on', on); rows[i].setAttribute('aria-selected', on ? 'true' : 'false'); }
    refs.pName.textContent = t.name; refs.pSub.textContent = t.sub;
    refs.stage.innerHTML = stageHtml(t.stage);
    refs.note.textContent = t.id === 'hud' ? 'Recolours the overlay the moment the menu closes.'
      : t.id === 'kiroshi' ? 'Tints the scan readout on the HUD.'
      : t.id === 'sandevistan' ? 'Colours the time-slow trail and screen tint.'
      : 'The colour lands on your chrome when you leave the clinic.';
    refs.glowWrap.hidden = !!t.noGlow;
    refs.swatches[0].hidden = !!t.noStock;
    setColor(effective(ui.target));
    refreshChips();
  }

  function refreshChips() {
    var chips = root.querySelectorAll('[data-chip]');
    for (var i = 0; i < chips.length; i++) { chips[i].style.background = effective(chips[i].dataset.chip); }
    var togs = root.querySelectorAll('.cz__toggle');
    for (var j = 0; j < togs.length; j++) { var on = !!custom()[togs[j].dataset.key]; togs[j].classList.toggle('is-on', on); togs[j].setAttribute('aria-checked', on ? 'true' : 'false'); }
    markSwatches();
  }

  CW.Customize = {
    onError: null,
    render: function () {
      if (!ui.built) { build(); }
      renderSelection();
    }
  };

  /* Harness only: answer the two ops without the DLL. Never runs inside the game. */
  if (CW.MockEngine && !(CW.Bridge && CW.Bridge.inGame && CW.Bridge.inGame())) {
    var mockStore = { targets: {}, seams: false, pulses: true };
    var resolved = function () {
      var out = { targets: {}, seams: mockStore.seams, pulses: mockStore.pulses };
      TARGETS.forEach(function (t) { var s = mockStore.targets[t.id] || {}; out.targets[t.id] = { color: s.color !== undefined ? s.color : (t.id === 'hud' ? '#fcee0a' : null), glow: s.glow !== undefined ? s.glow : 50 }; });
      return out;
    };
    var wrapped = CW.MockEngine.handle;
    CW.MockEngine.handle = function (msg, respond, emit) {
      if (msg.op === 'setCustomize' || msg.op === 'resetCustomize') {
        var a = msg.args || {};
        if (msg.op === 'resetCustomize') { if (a.target) { delete mockStore.targets[a.target]; } else { mockStore = { targets: {}, seams: false, pulses: true }; } }
        else {
          if (a.seams !== undefined) { mockStore.seams = a.seams; } if (a.pulses !== undefined) { mockStore.pulses = a.pulses; }
          if (a.target) { var e = mockStore.targets[a.target] = mockStore.targets[a.target] || {}; if (a.color !== undefined) { e.color = a.color; } if (a.glow !== undefined) { e.glow = a.glow; } }
        }
        var st = CW.Data.state(); if (st) { st.customize = resolved(); }
        respond({ id: msg.id, op: msg.op, ok: true, data: {}, state: st });
        return;
      }
      wrapped(msg, function (res) { if (res && res.state) { res.state.customize = resolved(); } respond(res); },
              function (ev) { if (ev && ev.state) { ev.state.customize = resolved(); } emit(ev); });
    };
  }

})(this);
