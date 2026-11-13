/* settings.js — the Settings tab (BETA QA, 2026-11-08).
 *
 * Renders into <main id="view-settings"> and talks to the engine only through
 * CW.Bridge.setSettings (UI-API op added 2026-11-08). The truth is
 * state.settings (cosave; the plugin mirrors it to CyberwareSettings.ini for
 * new games) and every change is applied live by the engine, so the header's
 * capacity bar, the shop prices and the HUD react on the next state push.
 *
 * app.js integration is two lines (setView + renderAll); everything else is
 * here so MENU POLISH's app.js rework merges cleanly.
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};

  /* One row per setting. kind: 'toggle' | 'range'. Copy is written for the
     couch at 2560x1440: short labels, real sentences underneath. */
  var GROUPS = [
    { id: 'limits', h: 'Chrome Limits', sub: 'How much metal your meat can take.', rows: [
      { key: 'capacityEnabled', kind: 'toggle', label: 'Cyberware capacity',
        desc: 'The original limit. Off: install anything, the bar shows ∞ and nothing is blocked.' },
      { key: 'capacityMult', kind: 'step', label: 'Capacity multiplier', min: 0.5, max: 3, step: 0.25, fmt: 'x',
        desc: 'Scales your maximum capacity. 1.0x is original-exact; 2.0x fits roughly twice the chrome.',
        needs: 'capacityEnabled' },
      { key: 'slotLimits', kind: 'toggle', label: 'Per-slot limits',
        desc: 'Each body system holds its original number of pieces (Frontal Cortex 3, Arms 1, OS 1…). Off: 16 everywhere.' },
      /* The perk is named by the CATALOG (cyberPerks id `edgerunner`), never by a string kept here: when LORE
         renamed it the page went on showing the old name (2026-11-10). A function row is resolved at render. */
      { key: 'overCapacity', kind: 'toggle',
        label: function () { var n = perkName('edgerunner'); return n ? n + ' over-capacity' : 'Over-capacity'; },
        desc: function () {
          var n = perkName('edgerunner');
          return 'With ' + (n ? 'the ' + n + ' perk' : 'the over-capacity perk') +
                 ' you can run up to 15% over the limit, paying max health per point — and risking Fury.';
        } }
    ] },
    { id: 'economy', h: 'Ripperdoc Economy', sub: 'What the chrome costs.', rows: [
      { key: 'priceMult', kind: 'step', label: 'Price multiplier', min: 0.25, max: 3, step: 0.25, fmt: 'x',
        desc: 'Scales every buy, upgrade, reroll and sell-back price at the same time.' },
      { key: 'levelPricing', kind: 'toggle', label: 'Level-based pricing',
        desc: 'Prices grow with your level, as in the original: about half price at level 1, full price at 20, 1.75x from 50.' },
      { key: 'upgradeComponents', kind: 'toggle', label: 'Upgrades need components',
        desc: 'Tier upgrades also cost components \u2014 Circuitry, Fiber Optics, Nuclear Material, Crystal, and military-grade boards at the top \u2014 counted from your junk the way crafting counts it.' }
    ] },
    { id: 'controls', h: 'Controls', sub: 'Keys that work anywhere, not just at the ripperdoc.', rows: [
      /* MENU POLISH's setup-view key (cw-menu-setup, the author's F8 decision 2026-11-09). Same
         avail gate as the HUD row: invisible until their branch merges. Current value comes
         from the open event's keys.setup; rebind goes through the setKey op. */
      { key: 'setupKey', kind: 'key', label: 'Setup view key',
        desc: 'Opens the read-only "your setup" view anywhere in the world. The full shop still needs a ripperdoc.',
        avail: function () { return !!(CW.Bridge && CW.Bridge.hudEdit); } }
    ] },
    { id: 'presentation', h: 'Presentation', sub: 'The feel, not the numbers.', rows: [
      { key: 'installScene', kind: 'toggle', label: 'Surgery scene',
        desc: 'Installing or removing chrome fades to black with the ripperdoc’s tools for a few seconds. Any key skips it.' },
      { key: 'hud', kind: 'toggle', label: 'Combat HUD overlay',
        desc: 'The in-world chrome HUD: threats, target analysis, RAM, cooldown timers. Applies immediately.' },
      /* MENU POLISH's HUD layout editor (cw-hud-layout, 2026-11-09). avail-gated: the row only
         exists once CW.Bridge.hudEdit does, so it is invisible until their branch merges. */
      { key: 'hudEdit', kind: 'button', label: 'HUD layout', btn: 'Edit HUD layout',
        desc: 'Move and resize the HUD overlay where you want it. Closes this menu and puts the HUD into edit mode (L does the same).',
        avail: function () { return !!(CW.Bridge && CW.Bridge.hudEdit); } },
      { key: 'menuSounds', kind: 'toggle', label: 'Menu sounds',
        desc: 'Clicks, confirms and the install-scene audio in this menu.' }
    ] }
  ];

  var DEFAULTS = { capacityEnabled: true, capacityMult: 1, priceMult: 1, levelPricing: true,
                   slotLimits: true, overCapacity: true, upgradeComponents: true,
                   installScene: true, hud: true, menuSounds: true };

  function $(id) { return document.getElementById(id); }
  /* a row's label / desc is a string, or a function resolved at render (for a name the catalog owns) */
  function text(v) { return typeof v === 'function' ? v() : v; }
  /* the catalog's current name for a cyber perk, HTML-safe; '' when the catalog has no such perk */
  function perkName(id) {
    var p = CW.Data && CW.Data.perk && CW.Data.perk(id);
    return p && p.name ? String(p.name).replace(/[&<>"]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]; }) : '';
  }
  function current() {
    var st = CW.Data && CW.Data.state && CW.Data.state();
    var s = (st && st.settings) || {};
    var out = {}, k;
    for (k in DEFAULTS) { out[k] = (s[k] === undefined) ? DEFAULTS[k] : s[k]; }
    return out;
  }

  var pending = false;

  /* ---- setup-view key (MENU POLISH seam) ----------------------------------
     The DLL's open event carries keys:{setup:<vk>}; stash it for the row. In game
     keydown arrives with key="Unidentified", so ONLY e.keyCode is trusted here. */
  var gameKeys = {};
  var capturing = false;

  function vkName(vk) {
    if (vk === undefined || vk === null || vk === 0) { return '—'; }
    if (vk >= 112 && vk <= 123) { return 'F' + (vk - 111); }
    if ((vk >= 65 && vk <= 90) || (vk >= 48 && vk <= 57)) { return String.fromCharCode(vk); }
    var m = { 33: 'Page Up', 34: 'Page Down', 35: 'End', 36: 'Home', 45: 'Insert', 46: 'Delete' };
    return m[vk] || ('VK 0x' + vk.toString(16).toUpperCase());
  }

  function keyError(which, text) {
    var el = $('setkey-err-' + which);
    if (el) { el.textContent = text; el.hidden = false; }
  }

  function captureKey() {
    if (capturing || !CW.Bridge || !CW.Bridge.request) { return; }
    capturing = true;
    var btn = document.querySelector('[data-keybtn="setupKey"]');
    if (btn) { btn.textContent = 'Press a key…'; }
    var onKey = function (e) {
      if (e) { e.preventDefault(); e.stopPropagation(); }
      document.removeEventListener('keydown', onKey, true);
      capturing = false;
      var vk = e ? e.keyCode : 0;
      if (!vk || vk === 27) { render(); return; }          // Esc (or nothing) cancels
      CW.Bridge.request('setKey', { key: 'setup', vk: vk }).then(function (res) {
        gameKeys.setup = (res && res.data && res.data.setup !== undefined) ? res.data.setup : vk;
        if (CW.Sound && CW.Sound.play) { CW.Sound.play('select'); }
        render();
      }, function (err) {
        render();
        keyError('setupKey', (err && (err.message || err.code)) || 'That key was refused.');
      });
    };
    document.addEventListener('keydown', onKey, true);
  }

  function send(patch) {
    if (!CW.Bridge || pending) { return; }
    pending = true;
    CW.Bridge.setSettings(patch).then(function (res) {
      pending = false;
      if (CW.Data && CW.Data.absorb) { CW.Data.absorb(res); }   // new state -> header bar, shop, this tab
      afterChange(patch);
      render();
    }, function (e) {
      pending = false;
      if (global.console && console.error) { console.error('setSettings failed: ' + (e && e.message)); }
      render();   // snap the controls back to the engine's truth
    });
  }

  function afterChange(patch) {
    if (patch.menuSounds !== undefined && CW.Sound && CW.Sound.setEnabled) { CW.Sound.setEnabled(!!patch.menuSounds); }
    if (CW.Sound && CW.Sound.play) { CW.Sound.play('select'); }
    else if (CW.Bridge && CW.Bridge.sound) { CW.Bridge.sound('UIMenuOK'); }
  }

  function rangeLabel(row, v) {
    var n = Math.round(v * 100) / 100;
    return row.fmt === 'x' ? (n + 'x') : String(n);
  }

  function render() {
    var root = $('view-settings');
    if (!root) { return; }
    var s = current();
    var html = ['<div class="setwrap">',
                '<div class="setview__head"><h2 class="pane__h">Settings</h2>',
                '<p class="setview__lede">Gameplay knobs, saved with this save and remembered for new games. ',
                'Defaults are original-exact. Everything applies the moment you change it.</p></div>'];
    for (var g = 0; g < GROUPS.length; g++) {
      var grp = GROUPS[g];
      var vis = [];
      for (var v = 0; v < grp.rows.length; v++) {
        if (!grp.rows[v].avail || grp.rows[v].avail()) { vis.push(grp.rows[v]); }
      }
      if (!vis.length) { continue; }   // a group whose rows are all gated away renders nothing
      html.push('<section class="setgrp" aria-labelledby="setgrp-' + grp.id + '">');
      html.push('<header class="setgrp__head"><h3 class="setgrp__h" id="setgrp-' + grp.id + '">' + grp.h + '</h3>');
      html.push('<span class="setgrp__sub">' + grp.sub + '</span></header>');
      for (var r = 0; r < vis.length; r++) {
        var row = vis[r];
        var off = row.needs && !s[row.needs];
        html.push('<div class="setrow' + (off ? ' is-off' : '') + '" data-key="' + row.key + '">');
        html.push('<div class="setrow__text"><span class="setrow__label" id="setlbl-' + row.key + '">' + text(row.label) + '</span>');
        html.push('<span class="setrow__desc">' + text(row.desc) + '</span></div>');
        if (row.kind === 'key') {
          html.push('<div class="setkey" role="group" aria-labelledby="setlbl-' + row.key + '">' +
                    '<span class="setkey__v" id="setkey-v-' + row.key + '">' + vkName(gameKeys.setup) + '</span>' +
                    '<button type="button" class="setact" data-keybtn="' + row.key + '">Change</button>' +
                    '<span class="setkey__err" id="setkey-err-' + row.key + '" hidden></span></div>');
        } else if (row.kind === 'button') {
          html.push('<button type="button" class="setact" data-act="' + row.key +
                    '" aria-labelledby="setlbl-' + row.key + '"' + (off ? ' disabled' : '') + '>' +
                    row.btn + '</button>');
        } else if (row.kind === 'toggle') {
          html.push('<button type="button" role="switch" aria-checked="' + (s[row.key] ? 'true' : 'false') +
                    '" aria-labelledby="setlbl-' + row.key + '" class="tgl' + (s[row.key] ? ' is-on' : '') +
                    '" data-tgl="' + row.key + '"' + (off ? ' disabled' : '') + '>' +
                    '<span class="tgl__track"><span class="tgl__knob"></span></span>' +
                    '<span class="tgl__word">' + (s[row.key] ? 'ON' : 'OFF') + '</span></button>');
        } else {
          /* stepper, not <input type=range>: Ultralight's form-control support is thin, and big
             minus/plus cells read better from the couch anyway */
          html.push('<div class="setstep" role="group" aria-labelledby="setlbl-' + row.key + '">' +
                    '<button type="button" class="setstep__b" data-step="' + row.key + '" data-dir="-1"' +
                    (off || s[row.key] <= row.min ? ' disabled' : '') + ' aria-label="Less">−</button>' +
                    '<span class="setstep__v" id="setout-' + row.key + '">' + rangeLabel(row, s[row.key]) + '</span>' +
                    '<button type="button" class="setstep__b" data-step="' + row.key + '" data-dir="1"' +
                    (off || s[row.key] >= row.max ? ' disabled' : '') + ' aria-label="More">+</button></div>');
        }
        html.push('</div>');
      }
      html.push('</section>');
    }
    html.push('<footer class="setfoot">');
    html.push('<button type="button" class="setreset" id="set-reset">Reset to original defaults</button>');
    html.push('<span class="setfoot__note">Saved in this save’s cosave and as the default for new games.</span>');
    html.push('</footer></div>');
    root.innerHTML = html.join('');
    wire(root);
  }

  function rowByKey(key) {
    for (var g = 0; g < GROUPS.length; g++) {
      for (var r = 0; r < GROUPS[g].rows.length; r++) {
        if (GROUPS[g].rows[r].key === key) { return GROUPS[g].rows[r]; }
      }
    }
    return null;
  }

  function wire(root) {
    var i, els;
    els = root.querySelectorAll('[data-tgl]');
    for (i = 0; i < els.length; i++) {
      (function (btn) {
        btn.addEventListener('click', function () {
          var patch = {};
          patch[btn.dataset.tgl] = !current()[btn.dataset.tgl];
          send(patch);
        });
      })(els[i]);
    }
    els = root.querySelectorAll('[data-step]');
    for (i = 0; i < els.length; i++) {
      (function (btn) {
        btn.addEventListener('click', function () {
          var key = btn.dataset.step, row = rowByKey(key);
          if (!row) { return; }
          var v = current()[key] + row.step * parseInt(btn.dataset.dir, 10);
          v = Math.min(Math.max(Math.round(v * 100) / 100, row.min), row.max);
          var patch = {};
          patch[key] = v;
          send(patch);
        });
      })(els[i]);
    }
    els = root.querySelectorAll('[data-keybtn]');
    for (i = 0; i < els.length; i++) {
      els[i].addEventListener('click', captureKey);
    }
    els = root.querySelectorAll('[data-act]');
    for (i = 0; i < els.length; i++) {
      (function (btn) {
        btn.addEventListener('click', function () {
          if (btn.dataset.act === 'hudEdit' && CW.Bridge && CW.Bridge.hudEdit) {
            if (CW.Sound && CW.Sound.play) { CW.Sound.play('select'); }
            CW.Bridge.hudEdit();   // closes the menu, focuses the HUD view, pauses (MENU POLISH)
          }
        });
      })(els[i]);
    }
    var reset = $('set-reset');
    if (reset) {
      reset.addEventListener('click', function () {
        var patch = {}, k;
        for (k in DEFAULTS) { patch[k] = DEFAULTS[k]; }
        send(patch);
      });
    }
  }

  CW.Settings = {
    render: render,
    get: current,
    defaults: DEFAULTS
  };

  /* Stash the open event's key map for the Controls rows (absent on branches
     without cw-menu-setup; the rows are avail-gated away there anyway). */
  if (CW.Bridge && CW.Bridge.on) {
    CW.Bridge.on('open', function (ev) {
      if (ev && ev.keys) { gameKeys = ev.keys; }
    });
  }

  /* Headless harness only: index.html#settings opens this tab, #scene plays the install scene,
     so serve-and-shoot can photograph them. In game there is never a location hash. */
  if (global.location && /^#(settings|scene)$/.test(global.location.hash || '')) {
    document.addEventListener('DOMContentLoaded', function () {
      global.setTimeout(function () {
        if (global.location.hash === '#scene') {
          if (CW.Scene) { CW.Scene.play('install', 'Kiroshi Optics'); }
          return;
        }
        var b = document.querySelector('#view-tabs [data-v="settings"]');
        if (b) { b.click(); }
      }, 1200);
    });
  }
})(this);
