/* fx.js — motion, feedback and d-pad helpers for the ripperdoc menu.
 *
 * Ultralight 1.4 rules this file keeps (styles.css header):
 *   - every animation is ONE-SHOT (nothing loops); entrances only move
 *     opacity and transform
 *   - prefers-reduced-motion: count-ups jump to the value, entrances and
 *     banners are skipped (styles.css also strips them)
 *   - no backdrop-filter / filter / box-shadow anywhere
 *
 *   CW.Fx.countTo(el, value, fmt)   tween a number from what it showed to value
 *   CW.Fx.replay(el, cls, ms)       restart a one-shot animation class
 *   CW.Fx.enter(nodes, cls, step)   staggered entrance for a fresh list
 *   CW.Fx.banner(host, text, sub)   the success band after buy/install/...
 *   CW.Fx.move(dir, from, scope)    spatial (d-pad) focus move
 *   CW.Fx.tipInstall(doc)           the in-game tooltip (Ultralight has no native one)
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};

  /* ── CW.keyOf(e): the key, the way Ultralight sends it ─────────────
     PrismaUI F4's Ultralight 1.4.0 delivers Esc as key "Unidentified",
     code "", keyCode 27 (Cyberware.log 18:06:06, VISUAL POLISH's in-game
     pass): every `e.key === 'Escape'` check was dead and the player could not
     close the menu. EVERY keydown handler in every view file goes through
     this. Never rely on e.code (empty in game). */
  var KEYCODE = {
    27: 'Escape', 13: 'Enter', 9: 'Tab', 8: 'Backspace', 46: 'Delete', 32: ' ',
    37: 'ArrowLeft', 38: 'ArrowUp', 39: 'ArrowRight', 40: 'ArrowDown',
    36: 'Home', 35: 'End', 33: 'PageUp', 34: 'PageDown', 191: '/', 16: 'Shift'
  };
  CW.keyOf = function (e) {
    var k = e && e.key;
    if (k && k !== 'Unidentified') { return k; }
    var c = e ? (e.keyCode || e.which || 0) : 0;
    if (KEYCODE[c]) { return KEYCODE[c]; }
    if (c >= 48 && c <= 57) { return String(c - 48); }              // the digit row
    if (c >= 96 && c <= 105) { return String(c - 96); }             // the number pad
    if (c >= 65 && c <= 90) { return String.fromCharCode(c + 32); } // letters, lower case
    return k || '';
  };

  var raf = global.requestAnimationFrame ? function (f) { return global.requestAnimationFrame(f); } : null;
  var caf = global.cancelAnimationFrame ? function (h) { global.cancelAnimationFrame(h); } : function () {};

  function reduced() {
    try { return !!(global.matchMedia && global.matchMedia('(prefers-reduced-motion: reduce)').matches); }
    catch (e) { return false; }
  }

  /* ── numbers that count to their new value ─────────────────────── */
  function countTo(el, value, fmt) {
    if (!el) { return; }
    fmt = fmt || function (v) { return String(v); };
    var prev = el.__cwV;
    el.__cwV = value;
    if (el.__cwRaf) { caf(el.__cwRaf); el.__cwRaf = null; }
    if (typeof prev !== 'number' || typeof value !== 'number' || prev === value || reduced() || !raf) {
      el.textContent = fmt(value);
      return;
    }
    var from = prev, dur = 460, t0 = null;
    el.classList.remove('fx-up', 'fx-down');
    void el.offsetWidth;
    el.classList.add(value > from ? 'fx-up' : 'fx-down');
    function step(t) {
      if (t0 === null) { t0 = t; }
      var p = Math.min(1, (t - t0) / dur);
      var e = 1 - Math.pow(1 - p, 3);
      el.textContent = fmt(p < 1 ? Math.round(from + (value - from) * e) : value);
      if (p < 1) { el.__cwRaf = raf(step); }
      else { el.__cwRaf = null; global.setTimeout(function () { el.classList.remove('fx-up', 'fx-down'); }, 500); }
    }
    el.__cwRaf = raf(step);
  }

  /* ── one-shot animation classes ─────────────────────────────────── */
  function replay(el, cls, ms) {
    if (!el || reduced()) { return; }
    el.classList.remove(cls);
    void el.offsetWidth;            // restart the animation
    el.classList.add(cls);
    global.setTimeout(function () { el.classList.remove(cls); }, ms || 700);
  }

  /* A fresh list: each node arrives a beat after the one above it. Capped so a
     long list is fully in within ~half a second. */
  function enter(nodes, cls, step, max) {
    if (reduced() || !nodes) { return; }
    cls = cls || 'fx-in'; step = step || 18; max = max || 14;
    for (var i = 0; i < nodes.length; i++) {
      nodes[i].style.animationDelay = (Math.min(i, max) * step) + 'ms';
      nodes[i].classList.add(cls);
    }
  }

  /* ── the success band: "INSTALLED · Mantis Blades" over the pane ── */
  function banner(host, text, sub, ms) {   // ms: how long it stays (default: a beat; a message to READ passes more)
    if (!host) { return; }
    var old = host.querySelector('.fxband');
    if (old) { old.parentNode.removeChild(old); }
    var b = document.createElement('div');
    b.className = 'fxband';
    b.setAttribute('role', 'status');
    b.setAttribute('aria-live', 'polite');
    var t = document.createElement('span');
    t.className = 'fxband__t'; t.textContent = text;
    b.appendChild(t);
    if (sub) {
      var s = document.createElement('span');
      s.className = 'fxband__s'; s.textContent = sub;
      b.appendChild(s);
    }
    host.appendChild(b);
    global.setTimeout(function () { if (b.parentNode) { b.parentNode.removeChild(b); } }, ms || (reduced() ? 1800 : 1700));
  }

  /* ── d-pad: move focus to the nearest control in a direction ────── */
  var FOCUSABLE = 'button:not([disabled]), input, [tabindex="0"]';
  function shown(e) {
    for (var p = e; p; p = p.parentElement) { if (p.hidden) { return false; } }
    var r = e.getBoundingClientRect();
    return r.width > 0 && r.height > 0;
  }
  function move(dir, from, scope) {
    scope = scope || document;
    var list = scope.querySelectorAll(FOCUSABLE);
    var fr = from.getBoundingClientRect();
    var fx = fr.left + fr.width / 2, fy = fr.top + fr.height / 2;
    var best = null, bestScore = Infinity;
    for (var i = 0; i < list.length; i++) {
      var c = list[i];
      if (c === from || !shown(c)) { continue; }
      var r = c.getBoundingClientRect();
      var cx = r.left + r.width / 2, cy = r.top + r.height / 2;
      var along, ortho;
      if (dir === 'down')  { along = r.top - fr.bottom + 1; ortho = Math.abs(cx - fx); if (cy <= fy + 2) { continue; } }
      if (dir === 'up')    { along = fr.top - r.bottom + 1; ortho = Math.abs(cx - fx); if (cy >= fy - 2) { continue; } }
      if (dir === 'right') { along = r.left - fr.right + 1; ortho = Math.abs(cy - fy); if (cx <= fx + 2) { continue; } }
      if (dir === 'left')  { along = fr.left - r.right + 1; ortho = Math.abs(cy - fy); if (cx >= fx - 2) { continue; } }
      var s = Math.max(0, along) + ortho * 2.5;
      if (s < bestScore) { bestScore = s; best = c; }
    }
    if (best) {
      best.focus();
      if (best.scrollIntoView) { best.scrollIntoView({ block: 'nearest', inline: 'nearest' }); }
    }
    return best;
  }

  /* ── tooltip ─────────────────────────────────────────────────────
     Ultralight draws no native title tooltip, and a couch player on a pad
     never hovers anyway. Any element with title= (or data-tip=) gets this
     one: on mouse hover after a beat, and on keyboard / d-pad focus at once.
     title is moved to data-tip on first use so a browser never shows both. */
  var tipEl = null, tipFor = null, tipTimer = null, keyMode = false;

  function tipText(el) {
    if (el.hasAttribute('title')) {
      var t = el.getAttribute('title');
      el.removeAttribute('title');
      if (t) { el.setAttribute('data-tip', t); }
    }
    return el.getAttribute('data-tip') || '';
  }
  function hideTip() {
    if (tipTimer) { global.clearTimeout(tipTimer); tipTimer = null; }
    tipFor = null;
    if (tipEl) { tipEl.hidden = true; }
  }
  function placeTip(el) {
    var text = tipText(el);
    if (!text || !document.body.contains(el)) { hideTip(); return; }
    tipEl.textContent = text;
    tipEl.hidden = false;
    var r = el.getBoundingClientRect();
    var tw = tipEl.offsetWidth, th = tipEl.offsetHeight;
    var vw = document.documentElement.clientWidth, vh = document.documentElement.clientHeight;
    var x = Math.max(16, Math.min(vw - tw - 16, r.left + r.width / 2 - tw / 2));
    var y = r.bottom + 10;
    if (y + th > vh - 16) { y = Math.max(16, r.top - th - 10); }
    tipEl.style.left = x + 'px';
    tipEl.style.top = y + 'px';
    replay(tipEl, 'fx-tip', 300);
  }
  function want(el, delay) {
    if (el === tipFor) { return; }
    hideTip();
    if (!el) { return; }
    tipFor = el;
    tipTimer = global.setTimeout(function () { tipTimer = null; if (tipFor === el) { placeTip(el); } }, delay);
  }
  function tipInstall(doc) {
    tipEl = doc.createElement('div');
    tipEl.className = 'tip';
    tipEl.id = 'tip';
    tipEl.setAttribute('role', 'tooltip');
    tipEl.hidden = true;
    doc.body.appendChild(tipEl);

    var pending = false, lx = 0, ly = 0;
    doc.addEventListener('mousemove', function (e) {
      keyMode = false;
      lx = e.clientX; ly = e.clientY;
      if (pending) { return; }
      pending = true;
      (raf || global.setTimeout)(function () {
        pending = false;
        /* elementFromPoint, not e.target: a disabled button gets no mouse
           events, and the disabled ones are exactly where the reason matters */
        var at = doc.elementFromPoint(lx, ly);
        var el = at && at.closest ? at.closest('[title], [data-tip]') : null;
        if (el && el.closest('.tip')) { return; }
        want(el, 420);
      });
    });
    doc.addEventListener('keydown', function (e) {
      keyMode = true;
      var k = CW.keyOf(e);
      if (k !== 'Shift' && k !== 'Tab') { hideTip(); }
    }, true);
    doc.addEventListener('focusin', function (e) {
      if (!keyMode) { return; }
      var el = e.target && e.target.closest ? e.target.closest('[title], [data-tip]') : null;
      want(el, 160);
    });
    doc.addEventListener('mousedown', hideTip, true);
    doc.addEventListener('scroll', hideTip, true);
    doc.addEventListener('mouseleave', hideTip);
  }

  CW.Fx = {
    reduced: reduced,
    countTo: countTo,
    replay: replay,
    enter: enter,
    banner: banner,
    move: move,
    shown: shown,
    tipInstall: tipInstall,
    hideTip: hideTip
  };
})(this);
