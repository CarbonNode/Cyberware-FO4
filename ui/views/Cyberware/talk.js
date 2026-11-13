/* talk.js: what a ripperdoc says, given who is in the chair (RIPPERDOC LORE, 2026-11-09).
 *
 * Pure selection logic over the catalog's ripperdocs[].talk (tools/ripperdoc_talk.py) and the live StateView; app.js
 * draws the conversation. Kept out of app.js so it can be exercised without a browser (harness/talk-test.mjs).
 *
 * The surgery scene (scene.js, BETA QA) stays untouched: if it fires 'cw:scene' {kind, name}, the doc's remark goes
 * into #scene-line. Without that event this does nothing.
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};

  function installed(state) {
    var out = [], all = (state && state.instances) || {};
    Object.keys(all).forEach(function (uid) { if (all[uid].installed) { out.push(all[uid]); } });
    return out;
  }

  function ownsNothing(state) {
    return Object.keys((state && state.instances) || {}).length === 0;
  }

  function hasItem(list, ids) {
    ids = typeof ids === 'string' ? [ids] : ids;
    return list.some(function (x) { return ids.indexOf(x.item) !== -1; });
  }

  /* every key in `when` must hold; an unknown key never matches, so a typo mutes the line instead of always showing it */
  function matches(when, state, player) {
    if (!when) { return true; }
    var cap = (state && state.capacity) || { used: 0, max: 0 };
    var inst = installed(state);
    var factions = (player && player.factions) || [];
    return Object.keys(when).every(function (k) {
      var v = when[k];
      switch (k) {
        case 'new': return ownsNothing(state) === v;
        case 'over': return (cap.used > cap.max) === v;
        case 'capAtLeast': return cap.max > 0 && (cap.used / cap.max) * 100 >= v;
        case 'item': return hasItem(inst, v);
        case 'itemPrefix': return inst.some(function (x) { return x.item.indexOf(v) === 0; });
        case 'slot': return inst.some(function (x) { return x.installed.slot === v; });
        case 'iconic': return inst.some(function (x) { return !!x.iconic; }) === v;
        case 'capsBelow': return !!state && state.currency.amount < v;
        case 'level': return !!state && (v[0] == null || state.level >= v[0]) && (v[1] == null || state.level <= v[1]);
        case 'faction': return factions.indexOf(v) !== -1;
        default: return false;
      }
    });
  }

  function textOf(g) { return typeof g === 'string' ? g : g.text; }
  function whenOf(g) { return typeof g === 'string' ? null : g.when; }

  /* The doc's first line: the first opener that applies, else any greeting that applies. `rnd` is injectable for tests. */
  function opener(talk, state, player, rnd) {
    var list = (talk && talk.openers) || [];
    for (var i = 0; i < list.length; i++) {
      if (matches(list[i].when, state, player)) { return list[i].text; }
    }
    var pool = ((talk && talk.greetings) || []).filter(function (g) { return matches(whenOf(g), state, player); });
    if (!pool.length) { return null; }
    return textOf(pool[Math.floor((rnd || Math.random)() * pool.length)]);
  }

  function topics(talk, state, player) {
    return ((talk && talk.topics) || []).filter(function (t) { return matches(t.when, state, player); });
  }

  /* install scene: the current doc's remark, spoken once per surgery */
  function sceneLine(talk, kind, rnd) {
    var lines = talk && talk.scene && talk.scene[kind === 'remove' ? 'remove' : 'install'];
    if (!lines || !lines.length) { return null; }
    return lines[Math.floor((rnd || Math.random)() * lines.length)];
  }

  function wireScene() {
    if (typeof document === 'undefined') { return; }
    document.addEventListener('cw:scene', function (e) {
      var line = document.getElementById('scene-line');
      if (!line) { return; }
      var Data = CW.Data;
      var sd = Data && Data.shop && Data.shop() && Data.shop().ripperdoc;
      var def = sd && Data.ripperdoc && Data.ripperdoc(sd.id);
      var text = sceneLine(def && def.talk, e.detail && e.detail.kind);
      line.textContent = text || '';
      line.hidden = !text;
    });
  }

  /* The voice clip key: FNV-1a 32 over the UTF-8 line text, 8 lower-case hex (docs/RIPPERDOC-VOICE.md). Exact catalog string. */
  function voiceKey(t) {
    var b = unescape(encodeURIComponent(t)), h = 0x811c9dc5;
    for (var i = 0; i < b.length; i++) { h ^= b.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; }
    return ('0000000' + h.toString(16)).slice(-8);
  }

  CW.Talk = { matches: matches, opener: opener, topics: topics, sceneLine: sceneLine, voiceKey: voiceKey };
  wireScene();
})(this);
