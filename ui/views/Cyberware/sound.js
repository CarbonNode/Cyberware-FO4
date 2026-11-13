/* sound.js — the menu's sound palette.
 *
 * NAMES are the Cyberpunk-sourced palette (SNDRs CW_SND_UI_* in Cyberware -
 * Sound.esp). FALLBACK holds the vanilla Fallout 4 UI sound for each kind
 * (SNDRs in Fallout4.esm, checked on the test rig 2026-11-08). The DLL plays
 * the palette only when [Sound] bCues is on and the record resolves; otherwise
 * it plays the vanilla name, so the menu always sounds. Both go through the
 * game's own mixer. The page never plays audio itself: Ultralight's <audio>
 * support is not something PrismaUI promises, and it would bypass the mixer.
 *
 * Transport: CW.Bridge.sound(name), a fire-and-forget `uiSound` request (no
 * id, no response). In a browser / the harness nothing is played; every
 * accepted sound is still logged to window.__cwSounds so the probe can check
 * which ones fire.
 *
 * Restraint rules (the author: "tasteful, not spammy"):
 *   - hover / focus ticks only when the pointer or focus reaches a DIFFERENT
 *     control, at most one per 70 ms, and never right after an action sound
 *   - typing ticks at most one per 45 ms
 *   - any one kind plays at most once per 40 ms
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};

  var NAMES = {
    focus:     'CW_SND_UI_Focus',      // hover / focus moves to another control
    select:    'CW_SND_UI_Select',     // pick a slot, a row, a mod bay
    tab:       'CW_SND_UI_Tab',        // Chrome/Perks, detail tabs, list filters
    back:      'CW_SND_UI_Back',       // close a popout, step back, cancel
    open:      'CW_SND_UI_Open',       // a confirm popout or picker opens
    type:      'CW_SND_UI_Type',       // a key typed into a search box
    clear:     'CW_SND_UI_Back',       // a search box cleared
    error:     'CW_SND_UI_Error',      // refused: cannot afford, level, capacity
    buy:       'CW_SND_UI_Buy',        // bought a piece or a mod
    install:   'CW_SND_UI_Install',    // fitted a piece or attached a mod
    remove:    'CW_SND_UI_Remove',     // removed a piece, detached a mod, refunded a perk
    sell:      'CW_SND_UI_Sell',       // sold a piece or a mod
    upgrade:   'CW_SND_UI_Buy',        // tier upgrade
    look:      'UIModsPartSwitch',     // appearance variant (no palette slot; vanilla)
    perk:      'CW_SND_UI_Perk',       // bought a cyber perk rank
    menuOpen:  'CW_SND_UI_MenuOpen',   // the ripperdoc menu opened
    menuClose: 'CW_SND_UI_MenuClose'   // the ripperdoc menu is closing
  };

  /* The vanilla sound for each kind: what plays when bCues is off or the SNDR is missing. */
  var FALLBACK = {
    focus: 'UIMenuFocus', select: 'UIMenuOK', tab: 'UIMenuPrevNext', back: 'UIMenuCancel', open: 'UIMenuPopUpGeneric',
    type: 'UITerminalCharSingle', clear: 'UIMenuCancel', error: 'UIVATSInsufficientAP', buy: 'UIModsBuild',
    install: 'UIModsPartSelect', remove: 'UIModsPartUnselect', sell: 'UIModsBreakDown', upgrade: 'UIModsBuild',
    look: 'UIModsPartSwitch', perk: 'UIPerkMenuChoosePerk', menuOpen: 'UIModsMenuEnter', menuClose: 'UIModsMenuExit'
  };

  /* the op that succeeded -> what it sounds like */
  var OP_KIND = {
    buy: 'buy', buyMod: 'buy', install: 'install', attachMod: 'install',
    remove: 'remove', detachMod: 'remove', sell: 'sell', sellMod: 'sell',
    upgrade: 'upgrade', setVariant: 'look', buyCyberPerk: 'perk', refundCyberPerk: 'remove',
    buyCapacityItem: 'buy'
  };

  var GAP = { focus: 70, type: 45 };   // per-kind minimum spacing, ms
  var DEFAULT_GAP = 40;
  var QUIET_AFTER_ACTION = 160;        // no hover tick right after an action sound

  var enabled = true;
  try { enabled = global.localStorage && global.localStorage.getItem('cw.sound') === '0' ? false : true; }
  catch (e) { enabled = true; }

  var last = {};                       // kind -> time it last played
  var lastAction = 0;                  // time the last non-focus sound played
  var log = global.__cwSounds = [];    // harness: what was accepted, in order

  function now() { return (global.performance && global.performance.now) ? global.performance.now() : Date.now(); }

  function play(kind) {
    var name = NAMES[kind];
    if (!enabled || !name) { return false; }
    var t = now();
    var gap = GAP[kind] || DEFAULT_GAP;
    if (last[kind] && t - last[kind] < gap) { return false; }
    if (kind === 'focus' && t - lastAction < QUIET_AFTER_ACTION) { return false; }
    last[kind] = t;
    if (kind !== 'focus' && kind !== 'type') { lastAction = t; }
    log.push(kind);
    if (log.length > 400) { log.splice(0, 200); }
    if (CW.Bridge && CW.Bridge.sound) { CW.Bridge.sound(name, FALLBACK[kind] || name); }
    return true;
  }

  /* Hover and focus: one tick when a DIFFERENT live control is reached. */
  var hot = null;
  var SEL = 'button, input, [role="tab"], [role="option"]';
  function reach(target) {
    var c = target && target.closest ? target.closest(SEL) : null;
    if (!c || c === hot) { return; }
    hot = c;
    if (c.disabled) { return; }
    play('focus');
  }

  function install(doc) {
    doc.addEventListener('mouseover', function (e) { reach(e.target); });
    doc.addEventListener('focusin', function (e) { reach(e.target); });
  }

  CW.Sound = {
    NAMES: NAMES,
    FALLBACK: FALLBACK,
    play: play,
    forOp: function (op) { return OP_KIND[op] || 'select'; },
    install: install,
    enabled: function () { return enabled; },
    /* BETA QA's Settings tab exposes this as "Menu sounds" */
    setEnabled: function (on) {
      enabled = !!on;
      try { if (global.localStorage) { global.localStorage.setItem('cw.sound', enabled ? '1' : '0'); } } catch (e) { /* no storage in this view */ }
    }
  };
})(this);
