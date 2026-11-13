/* scene.js — the ripperdoc install scene, as the VIEW's half of it (INSTALL SCENE, 2026-11-10).
 *
 * The contract is docs/INSTALL-SCENE.md. THE DLL SEQUENCES, THIS FILE PAINTS: on an accepted install or removal
 * the page sends {op:"scene"}, cw_scene.cpp answers with the mode it chose and then pushes
 *   cwOnEvent {type:"scene", phase:"bars|dip|up|step|line|work|flare|reveal|back|end|abort", ms, ...}
 * and each phase switches a class on #scene. Three modes, all drawn from the same parts:
 *   "chair"  the full scene: bars close, the dip, the subtitle in the lower bar, the flare, the lower-third
 *   "short"  a second commit in the same visit: the flare and the lower-third in place, 2.0 s
 *   "fade"   the 2D fallback: identical to "short" except a faint veil stands in for the camera work, so the two
 *            can never look like different features
 *
 * WHY THE OLD VERSION WAS WRONG, so nobody rebuilds it: it faded the whole screen to black for 3.4 s and drew an
 * EKG and a centred "CHROME INSTALLED" box. The world went away, which is the one thing docs/INSTALL-SCENE.md says
 * makes CP's chair sequence work. SCN-1 now fails any black over 0.5 s, and there is no centre box at all.
 *
 * NO DLL, NO PROBLEM. The harness and any build whose Cyberware.dll predates the scene op get playLocal(): the same
 * 2.0 s flare + lower-third with the world visible. That is also the reduced-motion path. The page therefore never
 * depends on the DLL answering, and ui/harness/check.sh can still exercise it.
 *
 * ULTRALIGHT (PrismaUI): no CSS grid, no `inset` shorthand, flat fills on full-screen layers, every sized <span> is
 * display:block, and a key is read ONLY through CW.keyOf(e) — Esc arrives as key "Unidentified" (keyCode 27).
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};

  /* These mirror cw_scene.cpp's beat table. Changing one here changes nothing in game: change it there too. */
  var DIP_MS = 450;            /* the only full-screen black, and it must stay under 500 (SCN-1) */
  var FADEUP_MS = 600;
  var LOCAL_FLARE_MS = 800;    /* playLocal: the short beat / fallback look */
  var LOCAL_THIRD_MS = 1200;
  var SKIP_ARM_MS = 1000;      /* SCN-10: a skip only counts after 1.0 s */
  var SUB_FALLBACK_MS = 4500;  /* how long a text-only line (no clip) stays readable */
  var DRIVEN_TIMEOUT_MS = 1200; /* no answer from the DLL in this long: draw it ourselves rather than nothing */

  var st = {
    playing: false, driven: false, mode: '', kind: '', name: '',
    startedAt: 0, timers: [], subTimer: 0, skipHandler: null, answered: false
  };

  function $(id) { return document.getElementById(id); }
  function later(ms, fn) { st.timers.push(global.setTimeout(fn, ms)); }
  function clearTimers() {
    for (var i = 0; i < st.timers.length; i++) { global.clearTimeout(st.timers[i]); }
    st.timers = [];
  }
  function cls() { return $('scene'); }
  function on(c) { var e = cls(); if (e) { e.classList.add(c); } }
  function off(c) { var e = cls(); if (e) { e.classList.remove(c); } }

  function sfx(name, fallback) {
    if (CW.Settings && CW.Settings.get && !CW.Settings.get().menuSounds) { return; }
    if (CW.Bridge && CW.Bridge.sound) { CW.Bridge.sound(name, fallback); }
  }
  function reducedMotion() {
    return !!(global.matchMedia && global.matchMedia('(prefers-reduced-motion: reduce)').matches);
  }
  function enabled() {
    var s = CW.Settings && CW.Settings.get ? CW.Settings.get() : null;
    return !s || s.installScene !== false;
  }

  /* ---- the layer ---------------------------------------------------------- */

  function reset() {
    var e = cls();
    if (e) { e.hidden = true; e.className = 'scene'; }
    var line = $('scene-line');
    if (line) { line.hidden = true; }
    var done = $('scene-done');
    if (done) { done.hidden = true; }
  }

  function finish() {
    clearTimers();
    if (st.subTimer) { global.clearTimeout(st.subTimer); st.subTimer = 0; }
    if (st.skipHandler) {
      document.removeEventListener('keydown', st.skipHandler, true);
      document.removeEventListener('mousedown', st.skipHandler, true);
      st.skipHandler = null;
    }
    reset();
    st.playing = false;
    st.driven = false;
    st.mode = '';
  }

  /* The lower-third. Never centred, never a box in the middle of the frame (SCN-7's "no centre box"). */
  function third(kind, name) {
    var done = $('scene-done');
    if (!done) { return; }
    $('scene-done-kind').textContent = kind === 'remove' ? 'CHROME REMOVED' : 'CHROME INSTALLED';
    $('scene-done-name').textContent = name || '';
    done.hidden = false;
    on('is-third');
    sfx(kind === 'remove' ? 'CW_SND_UI_Back' : 'CW_SND_UI_Select', kind === 'remove' ? 'UIModsPartUnselect' : 'UIPerkMenuChoosePerk');
  }

  /* Her subtitle sits in the LOWER BAR and outlives the scene when her clip is longer than it (SCN-6: her install
     line runs to 8.71 s from beat 4, past the 8.5 s end, and no beat may cut her off). */
  function subtitle(text, voiceMs) {
    var line = $('scene-line');
    if (!line) { return; }
    if (text) { line.textContent = text; }
    var has = !!(line.textContent || '').length;
    line.hidden = !has;
    if (!has) { return; }
    on('is-line');
    if (st.subTimer) { global.clearTimeout(st.subTimer); }
    var hold = voiceMs > 0 ? voiceMs + 250 : SUB_FALLBACK_MS;
    st.subTimer = global.setTimeout(function () {
      st.subTimer = 0;
      var l = $('scene-line');
      if (l) { l.hidden = true; }
      off('is-line');
    }, hold);
  }

  /* ---- the DLL's phases --------------------------------------------------- */

  function onPhase(ev) {
    if (!ev || ev.type !== 'scene') { return; }
    var ph = ev.phase || '';
    var ms = typeof ev.ms === 'number' ? ev.ms : 0;
    if (!st.playing && ph !== 'end' && ph !== 'abort') { return; }   /* a phase with no run: ignore, never half-draw */
    st.driven = true;
    switch (ph) {
      case 'bars':
        on('is-bars');
        break;
      case 'dip':
        on('is-dip');
        later(ms || DIP_MS, function () { off('is-dip'); });   /* the net: `up` normally clears it */
        break;
      case 'up':
        off('is-dip');
        on('is-up');
        break;
      case 'step':
      case 'work':
        break;                                                  /* world beats: nothing to paint */
      case 'line':
        subtitle(ev.text, typeof ev.voiceMs === 'number' ? ev.voiceMs : 0);
        break;
      case 'flare':
        on('is-flare');
        later(ms || LOCAL_FLARE_MS, function () { off('is-flare'); });
        if (st.mode !== 'chair') { subtitle(ev.text, typeof ev.voiceMs === 'number' ? ev.voiceMs : 0); }
        break;
      case 'reveal':
        off('is-flare');
        third(st.kind, st.name);
        break;
      case 'back':
        off('is-bars');
        on('is-back');
        break;
      case 'abort':
        /* Everything off at once: no bars, no veil, no flare left behind (SCN-10's "a skip leaves nothing running"
           applies to an abort too). The subtitle goes with it, because she was hushed. */
        finish();
        break;
      case 'end':
        /* The bars open and the menu comes back, but her line may still be playing: keep the subtitle timer alive. */
        var keep = st.subTimer;
        st.subTimer = 0;
        finish();
        if (keep) {
          st.subTimer = keep;
          var lay = cls();
          if (lay) { lay.hidden = false; lay.className = 'scene is-subonly'; }
          var l = $('scene-line');
          if (l) { l.hidden = false; }
        }
        break;
      default:
        break;
    }
  }

  /* ---- the DLL-less path: the harness, reduced motion, an older DLL ------- */

  function playLocal(kind, name, quiet) {
    on('is-local');
    on('is-flare');
    sfx('CWC_SND_InstallPart', kind === 'remove' ? 'UIModsBreakDown' : 'UIModsBuild');
    var flareMs = quiet ? 220 : LOCAL_FLARE_MS;
    later(flareMs, function () {
      off('is-flare');
      third(kind, name);
      later(LOCAL_THIRD_MS, finish);
    });
  }

  /* ---- start -------------------------------------------------------------- */

  function play(kind, name, uid) {
    if (st.playing || !enabled()) { return; }
    var layer = cls();
    if (!layer) { return; }
    st.playing = true;
    st.driven = false;
    st.answered = false;
    st.mode = '';
    st.kind = kind;
    st.name = name || '';
    st.startedAt = Date.now();
    layer.hidden = false;
    layer.className = 'scene';
    var done = $('scene-done');
    if (done) { done.hidden = true; }

    /* RIPPERDOC LORE's seam (bd6d0b05): talk.js listens for this and fills #scene-line SYNCHRONOUSLY, so the text
       is readable on the next line and its voice key can go to the DLL with the request. */
    document.dispatchEvent(new CustomEvent('cw:scene', { detail: { kind: kind, name: st.name } }));
    var lineEl = $('scene-line');
    var text = lineEl ? (lineEl.textContent || '') : '';
    var key = (text && CW.Talk && CW.Talk.voiceKey) ? CW.Talk.voiceKey(text) : '';
    if (lineEl) { lineEl.hidden = true; }           /* the DLL's `line` beat decides when she is heard */

    /* Skip fires on ANY keydown on purpose — never test e.key here: in Ultralight Esc arrives as
       key="Unidentified" (keyCode 27, empty e.code). While the scene is up this capture-phase handler also eats
       Esc before app.js's close: the first press skips, the next closes the menu. */
    st.skipHandler = function (e) {
      if (Date.now() - st.startedAt < SKIP_ARM_MS) { return; }   /* SCN-10: armed after 1.0 s */
      if (e) { e.preventDefault(); e.stopPropagation(); }
      skip();
    };
    document.addEventListener('keydown', st.skipHandler, true);
    document.addEventListener('mousedown', st.skipHandler, true);

    /* Reduced motion never asks for the chair scene: it gets the shortened fallback (docs/INSTALL-SCENE.md). */
    if (reducedMotion()) { st.mode = 'fade'; on('is-reduced'); playLocal(kind, st.name, true); return; }

    var req = CW.Bridge && CW.Bridge.request;
    if (typeof req !== 'function') { st.mode = 'fade'; playLocal(kind, st.name, false); return; }

    req('scene', { kind: kind, name: st.name, uid: uid || '', key: key, text: text }, false).then(function (res) {
      st.answered = true;
      var d = (res && res.data) || {};
      st.mode = d.mode || 'fade';
      on('is-' + st.mode);
      /* The DLL drives every beat from here; nothing else to do but wait for its phases. */
    }, function () {
      st.answered = true;
      if (!st.driven && st.playing) { st.mode = 'fade'; playLocal(kind, st.name, false); }
    });
    /* If the DLL neither answers nor pushes a phase, draw it ourselves rather than leaving the menu hidden
       behind nothing. A scene that silently did nothing cost cycle 3 a whole boot. */
    later(DRIVEN_TIMEOUT_MS, function () {
      if (st.playing && !st.driven && !st.answered) { st.mode = 'fade'; playLocal(kind, st.name, false); }
    });
  }

  function skip() {
    if (!st.playing) { return; }
    if (st.driven && CW.Bridge && CW.Bridge.request) {
      try { CW.Bridge.request('sceneSkip', {}, false); } catch (e) { /* the DLL will end it, or the timers will */ }
      return;                                /* the DLL jumps to the reveal and pushes the rest: one owner */
    }
    clearTimers();
    off('is-flare');
    third(st.kind, st.name);
    later(LOCAL_THIRD_MS, finish);
  }

  /* ---- triggers -----------------------------------------------------------
   * app.js commits every mutation through Bridge.request(op, args, false) AND dispatches 'cw:committed' after a
   * successful commit, so wrapping the request property and listening to that event both work; lastFire plus the
   * playing guard keep it to exactly one scene per commit. Zero app.js edits, by design. */
  var lastFire = 0;

  function instName(res, uid) {
    try { var inst = res.state.instances[uid]; return inst ? inst.name : ''; } catch (e) { return ''; }
  }

  function maybePlay(op, args, res) {
    var isInstall = op === 'install' || (op === 'buy' && args && args.install);
    var isRemove = op === 'remove';
    if (!isInstall && !isRemove) { return; }
    if (!res || !res.ok) { return; }                /* a refused commit gets the menu's own error, never a scene */
    var now = Date.now();
    if (now - lastFire < 500) { return; }
    lastFire = now;
    var uid = op === 'buy' ? (res.data && res.data.uid) : (args && args.uid);
    play(isRemove ? 'remove' : 'install', instName(res, uid), uid);
  }

  function wrap() {
    var B = CW.Bridge;
    if (!B || B.__sceneWrapped) { return; }
    B.__sceneWrapped = true;
    var orig = B.request;
    if (typeof orig === 'function') {
      B.request = function (op, args, dry) {
        var p = orig(op, args, dry);
        if (!dry && p && p.then) { p.then(function (res) { maybePlay(op, args, res); }, function () {}); }
        return p;
      };
    }
    document.addEventListener('cw:committed', function (e) {
      var d = e && e.detail;
      if (d) { maybePlay(d.op, d.args, d.res); }
    });
    if (typeof B.on === 'function') { B.on('scene', onPhase); }   /* the DLL's beats */
  }

  if (document.readyState === 'loading') { document.addEventListener('DOMContentLoaded', wrap); }
  else { wrap(); }

  CW.Scene = { play: play, skip: skip, wrap: wrap, onPhase: onPhase, finish: finish };
})(this);
