/* mock.js -- stands in for Cyberware.dll outside the game.
 *
 * It drives the SAME two window functions the plugin calls, so nothing above
 * bridge.js can tell the harness and the game apart.
 *
 * It never speaks in game. The plugin pushes its first snapshot within a
 * frame or two of the view being created, so with no query string this waits
 * WAIT_MS and starts only if bridge.js still reports transport 'none'. With a
 * query string it starts at once -- that is the harness asking on purpose,
 * and a headless capture cannot wait out a timer (--virtual-time-budget does
 * not advance one).
 *
 *   ?mock=idle|combat|max|zoom|hidden   which scenario (default combat)
 *   ?t=N                                frame N, deterministic. For captures.
 *   ?live=1                             run it at 10 Hz, for poking at in a browser
 *
 * The `max` scenario is the one that matters for review: it carries the
 * LONGEST REAL values -- FO4's "Legendary Assaultron Dominator", CP's
 * "Cyberware Malfunction" and "Militech Falcon Sandevistan" -- because a
 * layout is only proven by the worst string it will ever be handed.
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};
  var WAIT_MS = 1200;
  var TICK_MS = 100;            /* the agreed 10 Hz */

  function qs() {
    var out = {}, s = String(global.location && global.location.search || '');
    if (s.charAt(0) === '?') { s = s.slice(1); }
    if (!s) { return out; }
    var parts = s.split('&');
    for (var i = 0; i < parts.length; i++) {
      var kv = parts[i].split('=');
      out[decodeURIComponent(kv[0])] = decodeURIComponent(kv[1] === undefined ? '' : kv[1]);
    }
    return out;
  }

  /* ---- the scenarios --------------------------------------------------
   * Every one is a pure function of the frame number, so frame N is always
   * the same picture: a screenshot of ?t=12 is reproducible forever.
   */

  function combat(f) {
    var s = f * (TICK_MS / 1000);
    return {
      v: 1, t: f * TICK_MS, visible: true, inCombat: true,
      cfg: { threatRangeM: 50 },
      zoom: { on: false, factor: 3, scan: false },
      threats: [
        { id: '0001A2B3', name: 'Raider',          bearing: -34 + Math.sin(s) * 4,  dist_m: 14.2 },
        { id: '0001A2B4', name: 'Raider Scum',      bearing: 96,                     dist_m: 23.8 },
        { id: '0001A2B5', name: 'Raider Waster',    bearing: 168,                    dist_m: 41.0 },
        { id: '0001A2B6', name: 'Legendary Raider', bearing: -122,                   dist_m: 31.5 }
      ],
      target: {
        id: '0001A2B3', name: 'Raider', hp: 45, hpMax: 120,
        dr: 30, er: 10, dist_m: 14.2, vulnLimb: 'left leg', vulnLimbGroup: 4
      },
      ml: { stacks: 2, max: 5, untilS: 18.4 },
      ram: {
        cur: 14.5, max: 24,
        queue: [ { name: 'Overheat', cost: 9, state: 'active', etaS: 4.2 } ]
      },
      pls: { charges: 1, max: 2, pool: 0.81, slot: 'pls', nextS: 4.7 },
      timers: [
        { ability: 'ab_os_sandevistan_qiant', name: 'QianT Sandevistan', kind: 'os',
          activeS: 6.2, activeMaxS: 10, readyS: 0, readyMaxS: 30 },
        { ability: 'ab_blood_pump', name: 'Blood Pump', kind: 'heal',
          activeS: 0, activeMaxS: null, readyS: 11.5, readyMaxS: 40 }
      ]
    };
  }

  /* Out of combat: no threats, no target, nothing to analyse. What is left
     is the player's own kit, idle. This is the common case on screen and it
     has to look deliberate rather than broken. */
  function idle(f) {
    return {
      v: 1, t: f * TICK_MS, visible: true, inCombat: false,
      cfg: { threatRangeM: 50 },
      zoom: null, threats: [], target: null, ml: null,
      ram: { cur: 24, max: 24, queue: [] },
      pls: { charges: 2, max: 2, pool: 1 },
      timers: [
        { ability: 'ab_os_sandevistan_qiant', name: 'QianT Sandevistan', kind: 'os',
          activeS: 0, activeMaxS: 10, readyS: 0, readyMaxS: 30 }
      ]
    };
  }

  /* Everything at once, with the longest real strings. If this one reads,
     the HUD reads. */
  function max(f) {
    return {
      v: 1, t: f * TICK_MS, visible: true, inCombat: true,
      cfg: { threatRangeM: 50 },
      zoom: { on: true, factor: 3, scan: false },
      threats: [
        { id: 'a1', name: 'Legendary Assaultron Dominator', bearing: -8,   dist_m: 9.4 },
        { id: 'a2', name: 'Legendary Glowing Radscorpion',  bearing: 47,   dist_m: 18.0 },
        { id: 'a3', name: 'Ancient Behemoth',               bearing: 104,  dist_m: 27.3 },
        { id: 'a4', name: 'Mythic Deathclaw',               bearing: 152,  dist_m: 33.9 },
        { id: 'a5', name: 'Synth Eradicator',               bearing: -143, dist_m: 44.6 },
        { id: 'a6', name: 'Gunner Brigadier',               bearing: -76,  dist_m: 12.1 }
      ],
      target: {
        id: 'a1', name: 'Legendary Assaultron Dominator',
        hp: 1870, hpMax: 2400, dr: 285, er: 140, dist_m: 9.4,
        vulnLimb: 'right arm', vulnLimbGroup: 3
      },
      ml: { stacks: 5, max: 5, untilS: 24.9 },
      ram: {
        cur: 7.5, max: 32,
        queue: [
          { name: 'Cyberware Malfunction', cost: 12, state: 'active', etaS: 12.4 },
          { name: 'Synapse Burnout',       cost: 9,  state: 'active', etaS: 6.1 },
          { name: 'Reboot Optics',         cost: 6,  state: 'active', etaS: 2.0 }
        ]
      },
      pls: { charges: 2, max: 4, pool: 0.63 },
      timers: [
        { ability: 'ab_os_sandevistan_militech', name: 'Militech Falcon Sandevistan',
          kind: 'os', activeS: 0, activeMaxS: 8, readyS: 21.7, readyMaxS: 30 },
        { ability: 'ab_second_heart', name: 'Zetatech Second Heart', kind: 'heal',
          activeS: 0, activeMaxS: null, readyS: 96, readyMaxS: 120 },
        { ability: 'ab_bioconductor', name: 'Bioconductor', kind: 'buff',
          activeS: 3.4, activeMaxS: 6, readyS: 0, readyMaxS: 20 }
      ]
    };
  }

  function zoom(f) {
    var s = combat(f);
    s.zoom = { on: true, factor: 3, scan: false };
    s.threats = [];
    return s;
  }

  function hidden(f) {
    var s = combat(f);
    s.visible = false;
    return s;
  }

  var SCENES = { combat: combat, idle: idle, max: max, zoom: zoom, hidden: hidden };

  /* ---- the driver ----------------------------------------------------- */

  var timer = null;

  function push(scene, f) {
    if (global.cwHud) { global.cwHud(scene(f)); }
    if (global.cwHudEvent && scene === SCENES.combat) {   /* two targets: one locked, one still building */
      global.cwHudEvent({ type: 'smartLocks', locks: [
        { id: 'A', progress: 1, locked: true, x: 0.56, y: 0.42, dist: 500, u: 0.0016 },
        { id: 'B', progress: 0.45, locked: false, x: 0.42, y: 0.5, dist: 1100, u: 0.0007 } ] });
    }
  }

  CW.HudMock = {
    scenes: SCENES,

    start: function () {
      var q = qs();
      var scene = SCENES[q.mock] || SCENES.combat;
      var frame = parseInt(q.t, 10);
      if (!isFinite(frame) || frame < 0) { frame = 0; }
      var explicit = !!(q.mock || q.t !== undefined || q.live);

      /* Never in game: a long save load leaves transport 'none' past WAIT_MS, and
         the player then saw fake raiders over the loading screen (in-game 2026-11-08). */
      /* Two independent signals, so neither a user-agent change nor a missing prisma
         object can bring the fake feed back: Ultralight's UA, and PrismaUI's window.prisma
         (injected at "window object ready", before any script on the page runs). */
      if (!explicit && (/Ultralight/i.test((global.navigator && global.navigator.userAgent) || '') || global.prisma)) { return; }

      var begin = function () {
        if (CW.HudBridge.transport() === 'native') { return; }   /* the plugin is here */
        CW.HudBridge.markMock();
        push(scene, frame);
        if (q.live) {
          var n = frame;
          timer = global.setInterval(function () { push(scene, ++n); }, TICK_MS);
        }
      };

      if (explicit) { begin(); }
      else { global.setTimeout(begin, WAIT_MS); }
    },

    stop: function () { if (timer) { global.clearInterval(timer); timer = null; } },

    /* The self-test and the capture script drive frames by hand. */
    frame: function (name, f) { push(SCENES[name] || SCENES.combat, f || 0); }
  };

})(this);
