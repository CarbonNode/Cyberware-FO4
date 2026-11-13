/* bridge.js -- THE ONLY file that knows how the HUD hears from Cyberware.dll.
 *
 * Contract: fallout/cyberware/docs/HUD-API.md (v1), agreed with the FRAMEWORK
 * stream 2026-11-08. Push only: there is no page -> DLL direction in v1.
 *
 *   DLL -> page :  window.cwHud(snapshot)    full state, <=10 Hz, on change
 *                  window.cwHudEvent(event)  one-off moments, for animation
 *
 * Both are installed SYNCHRONOUSLY at script load, before anything awaits,
 * for the same reason the menu's bridge is: a call made before the function
 * exists is dropped on the floor, and the plugin may push the first snapshot
 * while hud.js is still parsing. Anything that arrives before the renderer
 * registers is held here (latest snapshot wins; events queue, capped) and
 * replayed the moment it does.
 *
 * Snapshots are idempotent and `t` is monotonic, so an out-of-order push is
 * dropped rather than rendered: the HUD can never go backwards in time.
 *
 * When window.cwHud is never called by anyone (a browser, the headless
 * harness) CW.HudMock drives the identical functions, so nothing above this
 * file can tell the game and the harness apart.
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};

  var API_VERSION = 1;
  var EVENT_CAP = 8;

  var sink = null;          // the renderer, once hud.js registers it
  var held = null;          // the latest snapshot that arrived before it did
  var heldEvents = [];
  var lastT = -1;           // monotonic guard
  var transport = 'none';   // 'native' | 'mock' | 'none'
  var holding = false;      // the layout editor is open: live snapshots wait (the latest is kept) so its sample stays put

  function fail(what, e) {
    if (global.console && global.console.error) {
      global.console.error('cwhud: ' + what + ': ' + (e && e.message ? e.message : e));
    }
  }

  /* ---- DLL -> page ---------------------------------------------------- */

  /* Tolerate a JSON string as well as a parsed object: PrismaUI's Invoke
     marshals differently depending on how the plugin builds the call, and a
     HUD that silently renders nothing is the worst failure mode here. */
  function coerce(v, what) {
    if (typeof v === 'string') {
      try { return JSON.parse(v); } catch (e) { fail(what + ' was unparseable', e); return null; }
    }
    return (v && typeof v === 'object') ? v : null;
  }

  global.cwHud = function (snap) {
    if (transport === 'none') { transport = 'native'; }
    snap = coerce(snap, 'snapshot');
    if (!snap) { return; }

    var t = (typeof snap.t === 'number') ? snap.t : null;
    if (t !== null) {
      if (t < lastT) { return; }        // out of order: the newer frame stands
      lastT = t;
    }

    if (snap.cfg && snap.cfg.custom && CW.HudAccent) { CW.HudAccent.apply(snap.cfg.custom); }   // accent.js: colours
    if (snap.cfg && snap.cfg.layout && CW.HudLayout) { CW.HudLayout.set(snap.cfg.layout); }    // layout.js: where things go
    if (!sink || holding) { held = snap; return; }
    try { sink.render(snap); } catch (e) { fail('render threw', e); }
  };

  global.cwHudEvent = function (ev) {
    ev = coerce(ev, 'event');
    if (!ev) { return; }
    if (ev.type === 'pulse' && CW.HudAccent) { CW.HudAccent.pulse(ev); return; }   // accent.js: the ability flash
    if (!sink) {
      if (heldEvents.length < EVENT_CAP) { heldEvents.push(ev); }
      return;
    }
    try { sink.event(ev); } catch (e) { fail('event threw', e); }
  };

  /* ---- page internals ------------------------------------------------- */

  CW.HudBridge = {
    API_VERSION: API_VERSION,

    /* hud.js calls this once, at the end of its own load. */
    attach: function (renderer) {
      sink = renderer;
      if (held) {
        var s = held; held = null;
        try { sink.render(s); } catch (e) { fail('render threw', e); }
      }
      if (heldEvents.length) {
        var q = heldEvents; heldEvents = [];
        for (var i = 0; i < q.length; i++) {
          try { sink.event(q[i]); } catch (e) { fail('event threw', e); }
        }
      }
    },

    /* 'native' once the plugin has pushed anything, 'mock' once the harness
       has, 'none' while nothing has spoken yet. */
    transport: function () { return transport; },
    markMock: function () { if (transport === 'none') { transport = 'mock'; } },

    /* The harness and a browser need to start from a clean slate between
       scripted frames; the game never calls this. */
    resetClock: function () { lastT = -1; },

    /* layout.js: hold live snapshots while the editor shows its sample; on release the newest one renders. */
    hold: function (on) {
      holding = !!on;
      if (!holding && sink && held) {
        var s = held; held = null;
        try { sink.render(s); } catch (e) { fail('render threw', e); }
      }
    }
  };

})(this);
