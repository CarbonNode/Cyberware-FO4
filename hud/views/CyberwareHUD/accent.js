/* accent.js -- applies the player's HUD colours (cfg.custom in the snapshot, HUD-API.md section 3) and plays the
 * ability pulse (the "pulse" event). bridge.js hands it both before any renderer runs; it touches no other file's DOM.
 *   CW.HudAccent.apply({accent:"#rrggbb", kiroshi:"#rrggbb"|null, pulses:bool})
 *   CW.HudAccent.pulse({kind:"os"|"heal"})
 */
(function (global) {
  'use strict';
  var CW = global.CW = global.CW || {};
  var last = '', pulses = true, node = null;

  function shade(hex, f) {
    var v = parseInt(hex.slice(1), 16), c = [(v >> 16) & 255, (v >> 8) & 255, v & 255];
    return 'rgb(' + Math.round(c[0] * f) + ',' + Math.round(c[1] * f) + ',' + Math.round(c[2] * f) + ')';
  }
  function rgba(hex, a) {
    var v = parseInt(hex.slice(1), 16);
    return 'rgba(' + ((v >> 16) & 255) + ',' + ((v >> 8) & 255) + ',' + (v & 255) + ',' + a + ')';
  }
  function valid(c) { return typeof c === 'string' && /^#[0-9a-fA-F]{6}$/.test(c); }

  function apply(c) {
    if (!c || typeof c !== 'object') { return; }
    if (c.pulses !== undefined) { pulses = !!c.pulses; }
    var key = (c.accent || '') + '|' + (c.kiroshi || '');
    if (key === last) { return; }
    last = key;
    var r = document.documentElement.style;
    if (valid(c.accent)) {
      r.setProperty('--y', c.accent);
      r.setProperty('--y-dim', shade(c.accent, 0.5));
      r.setProperty('--pulse', rgba(c.accent, 0.7));
    }
    if (valid(c.kiroshi)) { r.setProperty('--kiroshi', c.kiroshi); } else { r.removeProperty('--kiroshi'); }
  }

  function ensureNode() {
    if (node) { return node; }
    node = document.createElement('div');
    node.className = 'cw-pulse';
    node.setAttribute('aria-hidden', 'true');
    node.innerHTML = '<span class="t"></span><span class="b"></span><span class="l"></span><span class="r"></span>';
    document.body.appendChild(node);
    return node;
  }
  function pulse() {
    if (!pulses) { return; }
    var n = ensureNode();
    n.classList.remove('is-on');
    void n.offsetWidth;   // restart the one-shot
    n.classList.add('is-on');
  }

  CW.HudAccent = { apply: apply, pulse: pulse };

  /* harness: ?accent=#ff2bd6&pulse=1 in a browser. Ignored in game (Ultralight, no query). */
  var q = String(global.location && global.location.search || '');
  var m = /[?&]accent=%23([0-9a-fA-F]{6})/.exec(q);
  if (m) { apply({ accent: '#' + m[1], pulses: true }); }
  if (/[?&]pulse=1/.test(q)) { document.addEventListener('DOMContentLoaded', pulse); }
})(this);
