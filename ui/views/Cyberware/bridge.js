/* bridge.js — THE ONLY file that knows how the page talks to Cyberware.dll.
 *
 * Contract: fallout/cyberware/docs/UI-API.md (apiVersion 1), FRAMEWORK stream.
 * Real engine responses it was built against: docs/ui-api-samples/*.json
 *
 *   page -> DLL :  window.cwRequest(JSON.stringify({id, op, args?, dryRun?}))
 *                  bound with PrismaUI BindUIEvent, so it runs on the game thread.
 *   DLL -> page :  window.cwOnResponse(res)   one per request, same id
 *                  window.cwOnEvent(ev)       pushes: open | close | state
 *                  Both receive an ALREADY-PARSED object, not a string.
 *
 * Both handlers are installed synchronously at script load, before anything
 * awaits, because UI-API.md §2 says a call made before they exist is dropped.
 *
 * When window.cwRequest is absent (the headless harness, or a browser) the
 * bridge routes the identical request objects into CW.MockEngine instead, so
 * nothing above this file can tell the difference.
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};

  var API_VERSION = 1;
  var REQUEST_TIMEOUT_MS = 10000;

  var seq = 1;
  var pending = {};            // id -> {resolve, reject, timer, op}
  var listeners = {};          // 'open' | 'close' | 'state' -> [fn]
  var transport = 'mock';      // 'native' | 'mock'

  /* PrismaUI F4 runs the page in Ultralight. The DLL binds window.cwRequest at
     the view's DOM ready, from the game thread, which is usually AFTER this
     page's own DOMContentLoaded. So the transport is never decided once at
     load: inside Ultralight the page waits for cwRequest (it always comes) and
     never falls back to the mock (which needs fetch, which Ultralight lacks);
     only a plain browser / the headless harness gets the mock. First in-game
     run, 2026-11-08: deciding at load picked the mock and the menu booted into
     "fetch is not a function". */
  var IN_PRISMA = /Ultralight/i.test((global.navigator && global.navigator.userAgent) || '');

  function detect() {
    transport = (typeof global.cwRequest === 'function') ? 'native' : 'mock';
    return transport;
  }
  function isNative() {
    if (transport !== 'native' && typeof global.cwRequest === 'function') { transport = 'native'; }
    return transport === 'native';
  }

  /* cb('native') once the DLL is attached; cb('mock') in a browser after a
     short wait; cb('none') once as a warning if, inside the game, it is late,
     then cb('native') whenever it does attach. */
  function whenReady(cb) {
    var t0 = Date.now(), warned = false;
    (function poll() {
      if (detect() === 'native') { cb('native'); return; }
      var waited = Date.now() - t0;
      if (!IN_PRISMA && waited >= 300) { cb('mock'); return; }
      /* warn, but keep polling: a long save load fires the view's DOM ready
         after this timeout (26 s in-game, 2026-11-08), and the DLL binds
         cwRequest only then. */
      if (IN_PRISMA && !warned && waited >= 20000) { warned = true; cb('none'); }
      setTimeout(poll, 50);
    })();
  }
  /* the DLL may call this right after binding cwRequest */
  global.cwNativeReady = function () { detect(); };

  /* ---- DLL -> page ------------------------------------------------------ */

  global.cwOnResponse = function (res) {
    if (!res || typeof res !== 'object') { return; }
    var p = pending[res.id];
    if (!p) { return; }                       // late or unknown id: ignore
    delete pending[res.id];
    if (p.timer) { clearTimeout(p.timer); }
    if (res.ok) {
      p.resolve(res);
    } else {
      var err = res.error || {};
      var e = new Error(err.message || ('request "' + p.op + '" failed'));
      e.code = err.code || 'E_INTERNAL';
      e.op = p.op;
      p.reject(e);
    }
  };

  global.cwOnEvent = function (ev) {
    if (!ev || typeof ev !== 'object') { return; }
    emit(ev.type, ev);
  };

  function emit(type, payload) {
    var fns = listeners[type];
    if (!fns) { return; }
    for (var i = 0; i < fns.length; i++) {
      try { fns[i](payload); }
      catch (e) { CW.Bridge.onError('bridge: "' + type + '" listener threw: ' + e.message); }
    }
  }

  /* ---- page -> DLL ------------------------------------------------------ */

  /* The read-only "your setup" view (app.js isSetup): nothing that changes a piece is even sent. Cyberware.dll
     refuses the same ops (E_READONLY); this keeps the harness honest and the page from trying. */
  var WRITE_OPS = { buy: 1, buyMod: 1, sell: 1, sellMod: 1, install: 1, remove: 1, upgrade: 1, attachMod: 1, detachMod: 1,
                    setVariant: 1, buyCyberPerk: 1, refundCyberPerk: 1, reroll: 1, rerollResolve: 1, useCapacityItem: 1,
                    buyCapacityItem: 1, debugGrant: 1, debugReset: 1 };
  function request(op, args, dryRun) {
    if (CW.Bridge && CW.Bridge.readOnly && WRITE_OPS[op]) {
      var ro = new Error('Your setup is read only. A ripperdoc changes your chrome.');
      ro.code = 'E_READONLY';
      return Promise.reject(ro);
    }
    var id = seq++;
    var msg = { id: id, op: op };
    if (args) { msg.args = args; }
    if (dryRun) { msg.dryRun = true; }

    return new Promise(function (resolve, reject) {
      pending[id] = {
        resolve: resolve, reject: reject, op: op,
        timer: setTimeout(function () {
          delete pending[id];
          var e = new Error('"' + op + '" timed out after ' + REQUEST_TIMEOUT_MS + 'ms');
          e.code = 'E_TIMEOUT';
          reject(e);
        }, REQUEST_TIMEOUT_MS)
      };

      if (isNative()) {
        try {
          global.cwRequest(JSON.stringify(msg));
        } catch (e) {
          clearTimeout(pending[id].timer); delete pending[id];
          var err = new Error('bridge: could not send "' + op + '": ' + e.message);
          err.code = 'E_INTERNAL';
          reject(err);
        }
        return;
      }

      // harness: the mock engine answers through the same cwOnResponse path
      if (IN_PRISMA || !CW.MockEngine) {   // never the mock inside the game
        clearTimeout(pending[id].timer); delete pending[id];
        var e2 = new Error(IN_PRISMA ? 'Cyberware.dll is not connected to this view yet' : 'no plugin and no mock engine loaded');
        e2.code = 'E_INTERNAL';
        reject(e2);
        return;
      }
      CW.MockEngine.handle(msg, function (res) { global.cwOnResponse(res); },
                                function (ev) { global.cwOnEvent(ev); });
    });
  }

  /* Fire-and-forget UI sound (sound.js). No id, no response, no timer: the
     DLL plays a vanilla SNDR by editor id through RE::UIUtils::PlayMenuSound
     and answers nothing, so a hover tick never costs a round trip. The op is
     BETA QA's (plugin/cw_settings_plugin.cpp, cwset::MaybeUiSound), gated on
     state.settings.menuSounds. A DLL without it answers E_UNKNOWN_OP with
     id null, which cwOnResponse ignores. In a browser: no-op. */
  function sound(name, fallback) {
    if (!isNative()) { return false; }
    try { global.cwRequest(JSON.stringify({ op: 'uiSound', args: { sound: String(name), fallback: String(fallback || '') } })); return true; }
    catch (e) { return false; }
  }

  CW.Bridge = {
    API_VERSION: API_VERSION,
    sound: sound,
    detect: detect,
    isNative: isNative,
    whenReady: whenReady,
    inGame: function () { return IN_PRISMA; },
    transport: function () { return transport; },
    on: function (type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
    request: request,

    /* ---- the 18 ops of UI-API.md §4, in its order --------------------- */
    hello:           function () { return request('hello'); },
    getCatalog:      function () { return request('getCatalog'); },
    getState:        function () { return request('getState'); },
    getShop:         function () { return request('getShop'); },
    buy:             function (a, dry) { return request('buy', a, dry); },
    buyMod:          function (a, dry) { return request('buyMod', a, dry); },
    sell:            function (uid, dry) { return request('sell', { uid: uid }, dry); },
    sellMod:         function (uid, dry) { return request('sellMod', { uid: uid }, dry); },
    install:         function (a, dry) { return request('install', a, dry); },
    remove:          function (uid, dry) { return request('remove', { uid: uid }, dry); },
    upgrade:         function (uid, dry) { return request('upgrade', { uid: uid }, dry); },
    attachMod:       function (a, dry) { return request('attachMod', a, dry); },
    detachMod:       function (a, dry) { return request('detachMod', a, dry); },
    setVariant:      function (a, dry) { return request('setVariant', a, dry); },
    buyCyberPerk:    function (p, dry) { return request('buyCyberPerk', { perk: p }, dry); },
    refundCyberPerk: function (p, dry) { return request('refundCyberPerk', { perk: p }, dry); },
    buyCapacityItem: function (id, dry) { return request('buyCapacityItem', { id: id }, dry); },   // BETA QA
    close:           function () { return request('close'); },
    hudEdit:         function () { return request('hudEdit'); },   // MENU POLISH: the HUD layout editor (closes the menu)
    debugGrant:      function (a) { return request('debugGrant', a); },
    debugReset:      function () { return request('debugReset'); },

    /* ---- BETA QA additions (engine 2026-11-08) ------------------------ */
    setSettings:     function (a, dry) { return request('setSettings', a, dry); },
    /* ABILITIES D1: the player's quickhack ranking and how many slots to fill ({hacks, use}). Free, and allowed in
       the field setup view (it is not in WRITE_OPS): a loadout change installs nothing. */
    setLoadout:      function (a, dry) { return request('setLoadout', a, dry); },
    /* sound is fire-and-forget: the DLL plays the SNDR and sends NO response
       (plugin cwset::MaybeUiSound), so it must not enter the pending table.
       Same name and semantics as MENU POLISH's cw-menu-polish bridge.js: at a
       merge, either copy is correct. */
    sound:           function (sound, fallback) {
      if (transport === 'native' && global.cwRequest) {
        try { global.cwRequest(JSON.stringify({ id: null, op: 'uiSound', args: { sound: String(sound), fallback: String(fallback || '') } })); }
        catch (e) { CW.Bridge.onError('bridge: sound threw: ' + e.message); }
      }
    },

    onError: function (text) {
      if (global.console && global.console.error) { global.console.error(text); }
    }
  };

  detect();
})(this);
