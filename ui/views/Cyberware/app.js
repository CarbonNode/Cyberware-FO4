/* app.js — the ripperdoc menu's screens, input and flows.
 *
 * Reads only through CW.Data, writes only through CW.Bridge. Every mutation
 * is previewed with dryRun first (UI-API.md §5), so the confirm popout shows
 * the real before -> after the engine will produce, not an estimate.
 */
(function (global) {
  'use strict';

  var CW = global.CW;
  var Bridge = CW.Bridge, Data = CW.Data, Snd = CW.Sound, Fx = CW.Fx;

  var $ = function (id) { return document.getElementById(id); };
  /* every key check goes through CW.keyOf (fx.js): in game Esc arrives as
     key "Unidentified" + keyCode 27 */
  var keyOf = CW.keyOf;

  /* ── where each slot sits on the body scan ──────────────────────────
     Coordinates are percentages of the FIGURE. FIG is the figure's own
     bounding box inside icons/body-scan.png (404x834, measured by
     harness/crop-body.md's procedure). Converting here means the art can
     be re-cropped without touching a single slot coordinate. */
  var FIG = { l: 5.45, t: 3.72, w: 89.11, h: 92.57 };

  var SLOT_ART = {
    frontalCortex:       { icon: 'slot-frontal-cortex.png',  x: 50, y: 3 },
    ocularSystem:        { icon: 'slot-ocular.png',          x: 50, y: 8 },
    face:                { icon: 'slot-ocular.png',          x: 50, y: 8 },  /* SPEC calls it "Face (Ocular)" */
    operatingSystem:     { icon: 'slot-operating-system.png', x: 60, y: 13 },
    nervousSystem:       { icon: 'slot-nervous.png',         x: 50, y: 21 },
    circulatorySystem:   { icon: 'slot-circulatory.png',     x: 44, y: 27 },
    immuneSystem:        { icon: 'slot-immune.png',          x: 56, y: 33 },
    integumentarySystem: { icon: 'slot-integumentary.png',   x: 34, y: 30 },
    skeleton:            { icon: 'slot-skeleton.png',        x: 50, y: 45 },
    arms:                { icon: 'slot-arms.png',            x: 19, y: 41 },
    hands:               { icon: 'slot-hands.png',           x: 10, y: 53 },
    legs:                { icon: 'slot-legs.png',            x: 41, y: 74 }
  };
  var FALLBACK_ART = { icon: 'slot-operating-system.png', x: 50, y: 50 };

  var SCAN_W = 404, SCAN_H = 834;      /* icons/body-scan.png, natural size */

  function art(slotId) { return SLOT_ART[slotId] || FALLBACK_ART; }
  function pinPos(a) { return { x: FIG.l + a.x / 100 * FIG.w, y: FIG.t + a.y / 100 * FIG.h }; }

  /* The scan is drawn with object-fit: contain, so it rarely fills its box.
     Size the pin layer to the rect the image ACTUALLY occupies, and pin
     percentages then refer to the image rather than to the container. */
  function layoutPins() {
    var fig = $('bodymap') && document.querySelector('.bodymap__fig');
    var pins = $('body-pins');
    if (!fig || !pins) { return; }
    var bw = fig.clientWidth, bh = fig.clientHeight;
    if (!bw || !bh) { return; }
    var scale = Math.min(bw / SCAN_W, bh / SCAN_H);
    var w = SCAN_W * scale, h = SCAN_H * scale;
    pins.style.left = ((bw - w) / 2) + 'px';
    pins.style.top = ((bh - h) / 2) + 'px';
    pins.style.width = w + 'px';
    pins.style.height = h + 'px';
  }

  /* ── view state (UI only; the game state lives in Data) ───────────── */
  var ui = {
    view: 'chrome',          // 'chrome' | 'perks'
    pane: 'body',            // phone screen: 'body' | 'list' | 'detail'
    slot: null,              // selected slot id
    itemId: null,            // selected catalog item id
    uid: null,               // selected owned instance, when there is one
    tab: 'stats',
    filter: 'all',
    query: '',               // slot filter box
    globalQuery: '',         // header search box
    perkQuery: '',
    rowIds: [],              // current row order, for arrow keys
    setup: false,            // read-only "your setup" view (the hotkey anywhere); the shop is only at a ripperdoc
    busy: false
  };

  /* ── small helpers ────────────────────────────────────────────────── */
  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) { n.className = cls; }
    if (text !== undefined && text !== null) { n.textContent = String(text); }
    return n;
  }
  function clear(node) { while (node.firstChild) { node.removeChild(node.firstChild); } }

  function iconFor(itemDef) {
    // Data.iconPath applies the SPEC-to-ASSETS name alias; anything still
    // missing falls back to this slot's own glyph, so a row is never empty
    return { src: Data.iconPath(itemDef), fallback: 'icons/' + art(itemDef.slot).icon };
  }
  function imgWithFallback(cls, src, fallback, alt) {
    var im = el('img', cls);
    im.alt = alt || '';
    im.width = 128; im.height = 128;
    im.onerror = function () { im.onerror = null; im.src = fallback; };
    im.src = src || fallback;
    return im;
  }

  var toastTimer = null;
  /* Engine errors arrive as fragments ("every Face (Ocular) position is taken");
     the toast shows a sentence, without the API hint meant for callers. */
  function sentence(m) {
    m = String(m || '').replace(/\s*\(pass [^)]*\)/g, '').trim();
    if (!m) { return m; }
    m = m.charAt(0).toUpperCase() + m.slice(1);
    return /[.!?]$/.test(m) ? m : m + '.';
  }
  function toast(text, ok) {
    var t = $('toast');
    $('toast-text').textContent = sentence(text);
    t.className = 'toast' + (ok ? ' is-ok' : '');
    t.hidden = false;
    Fx.replay(t, 'fx-toast', 400);
    if (!ok) { Snd.play('error'); }
    if (toastTimer) { clearTimeout(toastTimer); }
    toastTimer = setTimeout(function () { t.hidden = true; }, ok ? 2600 : 5200);
  }

  function matches(hay, q) {
    if (!q) { return true; }
    return String(hay).toLowerCase().indexOf(q.toLowerCase()) !== -1;
  }

  /* ── search: every word must match somewhere; name hits rank first ──
     "mantis" finds Mantis Blades by name; "armor arms" finds arm pieces that
     give armor (slot name + stat label); "kiroshi" finds by maker. */
  function tokens(q) {
    return String(q || '').toLowerCase().split(/\s+/).filter(function (t) { return t; });
  }
  var hayCache = {};
  function itemHay(it) {
    if (hayCache[it.id]) { return hayCache[it.id]; }
    var bits = [it.name, it.manufacturer, it.description, it.iconic ? 'iconic' : ''];
    var sd = Data.slotDef && Data.slotDef(it.slot);
    if (sd) { bits.push(sd.name); }
    var seen = {};
    (it.tiers || []).forEach(function (t) {
      Object.keys(t.stats || {}).forEach(function (k) {
        if (!seen[k]) { seen[k] = 1; bits.push(Data.statLabel(k)); }
      });
    });
    return (hayCache[it.id] = bits.filter(Boolean).join('   ').toLowerCase());
  }
  /* -1 = no match; lower = better. 0 name starts with the query, 1 a name
     word starts with a term, 2 every term is in the name, 3 found elsewhere. */
  function rank(name, hay, toks) {
    if (!toks.length) { return 0; }
    for (var i = 0; i < toks.length; i++) { if (hay.indexOf(toks[i]) === -1) { return -1; } }
    var n = String(name).toLowerCase();
    if (n.indexOf(toks.join(' ')) === 0) { return 0; }
    var words = n.split(/[^a-z0-9]+/);
    var allInName = true, wordStart = false;
    for (var j = 0; j < toks.length; j++) {
      if (n.indexOf(toks[j]) === -1) { allInName = false; }
      for (var w = 0; w < words.length; w++) { if (words[w].indexOf(toks[j]) === 0) { wordStart = true; } }
    }
    return allInName ? (wordStart ? 1 : 2) : 3;
  }
  /* the name with each matched term marked, for the result rows */
  function hl(text, toks) {
    var frag = document.createDocumentFragment();
    text = String(text || '');
    if (!toks || !toks.length) { frag.appendChild(document.createTextNode(text)); return frag; }
    var low = text.toLowerCase(), marks = [];
    toks.forEach(function (t) {
      var at = 0, k;
      while ((k = low.indexOf(t, at)) !== -1) { marks.push([k, k + t.length]); at = k + t.length; }
    });
    marks.sort(function (a, b) { return a[0] - b[0]; });
    var pos = 0;
    marks.forEach(function (m) {
      if (m[0] < pos) { if (m[1] > pos) { m[0] = pos; } else { return; } }
      if (m[0] > pos) { frag.appendChild(document.createTextNode(text.slice(pos, m[0]))); }
      frag.appendChild(el('mark', 'hl', text.slice(m[0], m[1])));
      pos = m[1];
    });
    if (pos < text.length) { frag.appendChild(document.createTextNode(text.slice(pos))); }
    return frag;
  }

  /* Mod slot types are catalog ids ("mantis_edge", "pls_rounds"); the player
     reads "Mantis Edge", "PLS Rounds". */
  var TYPE_WORD = { pls: 'PLS', frag: 'Fragment' };
  function typeName(t) {
    return String(t || '').split('_').map(function (w) {
      return TYPE_WORD[w] || (w.charAt(0).toUpperCase() + w.slice(1));
    }).join(' ');
  }

  /* An upgrade's component bill (UI-API: instances[uid].upgrade.components,
     BETA QA): one row per component, have / need, short ones in red. Empty or
     absent (Settings "Upgrades need components" off) draws nothing. */
  function billNode(components, heading) {
    if (!components || !components.length) { return null; }
    var box = el('div', 'bill');
    if (heading) { box.appendChild(el('div', 'bill__h', heading)); }
    components.forEach(function (c) {
      var short = (c.have || 0) < c.need;
      var row = el('div', 'bill__r' + (short ? ' is-short' : ''));
      row.appendChild(el('span', 'bill__n', c.name || c.form));
      var q = el('span', 'bill__q');
      q.appendChild(el('b', null, c.have || 0));
      q.appendChild(document.createTextNode(' / ' + c.need));
      row.appendChild(q);
      row.setAttribute('aria-label', (c.name || c.form) + ': you have ' + (c.have || 0) + ', it needs ' + c.need);
      box.appendChild(row);
    });
    return box;
  }

  /* An empty state is never a dead end: say what happened and offer the way out. */
  function emptyState(node, text, actions) {
    clear(node);
    node.appendChild(el('span', 'empty__t', text));
    if (actions && actions.length) {
      var row = el('span', 'empty__acts');
      actions.forEach(function (a) {
        var b = el('button', 'btn btn--sm', a.label);
        b.type = 'button';
        b.onclick = a.run;
        row.appendChild(b);
      });
      node.appendChild(row);
    }
  }

  /* ── the read-only "your setup" view ─────────────────────────────
     The author, 2026-11-08: "no hotkey, only openable by the doctor. Maybe config or hotkey anywhere to see your setup."
     Buying, fitting, removing, upgrading, selling, mods, looks and perks happen only at a ripperdoc. The hotkey
     anywhere opens this view: installed chrome, stats, capacity, Customize and Settings, nothing that changes a
     piece. With no ripperdoc in the shop it is ALWAYS this view, whatever opened the menu. Cyberware.dll refuses
     every chrome-changing op here too (E_READONLY), and bridge.js refuses them before they are sent. */
  function isSetup() { var sh = Data.shop(); return ui.setup || !(sh && sh.ripperdoc); }
  /* The ripperdocs come from the catalog, so a doc added or removed there (the author removed the Institute's) is right
     here with no code change. A doc with "main": true (the main shop, Varga) leads; its "place" names where it is,
     with this map as the fallback for a catalog that does not carry one yet. */
  var DOC_PLACE = { goodneighbor: 'Goodneighbor', diamondcity: 'Diamond City', institute: 'the Institute' };
  function docList() {
    var docs = (Data.ripperdocs ? Data.ripperdocs() : []).filter(function (d) { return !d.disabled; })   /* a doc taken out ("disabled": true) is not a place to go */
      .map(function (d, i) { return { d: d, i: i }; });
    docs.sort(function (a, b) { return (b.d.main ? 1 : 0) - (a.d.main ? 1 : 0) || a.i - b.i; });
    return docs.map(function (x) {
      var place = x.d.place || DOC_PLACE[x.d.id];
      return x.d.name + (place ? ' in ' + place : '');
    });
  }
  function joinAnd(a) { return a.length < 2 ? (a[0] || '') : a.slice(0, -1).join(', ') + ' or ' + a[a.length - 1]; }
  function setupNote() {
    var docs = docList();
    /* The lead's ruling 2026-11-10: with a quickhack loadout the field view is not "read only" any more. It says
       what is true: nothing is bought, fitted or sold here, and the loadout is the one thing that can change. */
    return (hasLoadout() ? 'No buying, fitting or selling in the field: your quickhack loadout is the one thing you can change here (Q). '
                         : 'Read only. ') +
           'To buy, fit, upgrade or sell, see a ripperdoc' + (docs.length ? ': ' + joinAnd(docs) + '.' : '.');
  }
  function ownsAny() {
    var st = Data.state();
    for (var u in st.instances) { if (st.instances.hasOwnProperty(u)) { return true; } }
    return false;
  }

  /* The list footer: the piece count, then as many key hints as fit, most useful first. */
  var fitRetry = false;
  function fitKeys() {
    var foot = $('list-keys') && $('list-keys').parentNode;
    if (!foot) { return; }
    /* the pane is not on screen yet (a phone-width search shows it a moment later): measure once it is */
    if (!foot.clientWidth) {
      if (!fitRetry) { fitRetry = true; global.setTimeout(function () { fitRetry = false; if (foot.clientWidth) { fitKeys(); } }, 0); }
      return;
    }
    var gs = Array.prototype.slice.call($('list-keys').querySelectorAll('.kg'));
    var qk = $('kg-qh');
    if (qk) { qk.hidden = !hasLoadoutScreen(); gs = gs.filter(function (g) { return g !== qk || hasLoadoutScreen(); }); }
    gs.forEach(function (g) { g.hidden = false; });
    gs.sort(function (a, b) { return (+b.getAttribute('data-p')) - (+a.getAttribute('data-p')); });
    for (var i = 0; i < gs.length && foot.scrollWidth > foot.clientWidth + 1; i++) { gs[i].hidden = true; }
  }
  /* The list box takes whatever height is left, which cut the last visible row
     in half and left a strip of nothing under it (VISUAL-GATE M1, seen again in
     cycle 4). Snap its height down to a whole number of rows; the few leftover
     pixels go to the watermark, which is built to take space nobody wants, so
     the pane still ends flush. Only when the list actually scrolls: a short
     list keeps its natural height. */
  var snapAgain = false;
  function snapRows(settled) {
    var rows = $('rows');
    if (!rows) { return; }
    /* always measure unconstrained: a cap left over from a render when the pane
       was still growing would otherwise be measured as the real height and kept */
    rows.style.maxHeight = '';
    var first = rows.querySelector('li');
    if (!first) { return; }
    var avail = rows.clientHeight;
    var second = first.nextElementSibling;
    /* pitch = one row plus its gap, measured from two rows when there are two
       (offsetHeight alone misses the margin, and the margin is a rem value) */
    var pitch = second ? (second.offsetTop - first.offsetTop) : first.offsetHeight;
    /* the LAST row's bottom margin never has to be on screen, so it is not
       content that makes a list scroll: without this a list that fits exactly
       overflowed by those few pixels and lost a whole row to the snap */
    var gap = Math.max(0, pitch - first.offsetHeight);
    if (avail && pitch > 8 && rows.scrollHeight - gap > avail + 1) {
      var whole = Math.floor((avail + gap) / pitch) * pitch - gap;
      if (whole >= first.offsetHeight && whole < avail) { rows.style.maxHeight = whole + 'px'; }
    }
    /* The pane is still settling on the first render of a slot: the positions
       block and the footer claim their height after this pass, and a list that
       fits can measure as one that scrolls. Measure once more next tick, the
       way fitKeys does, and let that result stand. */
    if (!settled && !snapAgain) {
      snapAgain = true;
      global.setTimeout(function () { snapAgain = false; snapRows(true); }, 0);
    }
  }

  global.addEventListener('resize', function () { fitKeys(); snapRows(); });

  /* ═══════════════════ render: header ═══════════════════ */
  function renderHeader() {
    var st = Data.state(), shop = Data.shop();
    var ro = isSetup();
    $('doc-name').textContent = ro ? 'Your chrome' : ((shop && shop.ripperdoc && shop.ripperdoc.name) || 'No ripperdoc');
    $('doc-kind').textContent = ro ? (hasLoadout() ? 'In the field' : 'Read only') : 'Ripperdoc';
    $('cw').dataset.mode = ro ? 'setup' : 'shop';
    if (CW.Bridge) { CW.Bridge.readOnly = ro; }
    $('cur-name').textContent = st.currency.name;
    Fx.countTo($('w-caps'), st.currency.amount, Data.money);
    Fx.countTo($('w-level'), st.level);
    Fx.countTo($('w-perk'), st.perkPoints.available);

    Fx.countTo($('cap-used'), st.capacity.used);
    Fx.countTo($('cap-max'), st.capacity.max);
    var pct = st.capacity.max ? Math.min(100, st.capacity.used / st.capacity.max * 100) : 0;
    $('cap-fill').style.width = pct + '%';
    $('cap-track').className = 'cap__track' + (st.capacity.used > st.capacity.max ? ' is-over' : '');
    var bonus = $('cap-bonus');
    if (st.capacity.bonus) {
      bonus.hidden = false;
      bonus.textContent = '+' + st.capacity.bonus + ' from perks';
    } else { bonus.hidden = true; }
    clearPreview();
  }

  /* The striped overlay on the capacity bar: what a pending action would cost. */
  function showPreview(newUsed) {
    var st = Data.state();
    var pre = $('cap-pre');
    var a = Math.min(st.capacity.used, newUsed) / st.capacity.max * 100;
    var b = Math.max(st.capacity.used, newUsed) / st.capacity.max * 100;
    pre.style.left = Math.max(0, a) + '%';
    pre.style.width = Math.max(0, Math.min(100, b) - Math.max(0, a)) + '%';
  }
  function clearPreview() { $('cap-pre').style.width = '0%'; }

  /* ═══════════════════ render: body map ═══════════════════ */
  function renderBody() {
    var st = Data.state();
    var slots = Data.slots();
    var railL = $('rail-l'), railR = $('rail-r'), pins = $('body-pins');
    clear(railL); clear(railR); clear(pins);

    var half = Math.ceil(slots.length / 2);

    slots.forEach(function (slot, i) {
      var a = art(slot.id);
      var filled = 0;
      for (var k = 0; k < slot.installed.length; k++) { if (slot.installed[k]) { filled++; } }

      var lit = ui.slot === slot.id && !ui.globalQuery;   // a search lists every slot: no slot is the current one
      var b = el('button', 'node' + (lit ? ' is-on' : ''));
      b.type = 'button';
      b.setAttribute('aria-pressed', ui.slot === slot.id ? 'true' : 'false');
      var label = slot.name + ', ' + filled + ' of ' + slot.count + ' filled';
      b.title = label;
      b.setAttribute('aria-label', label);

      var im = el('img', 'node__icon');
      im.src = 'icons/' + a.icon; im.alt = ''; im.width = 64; im.height = 64;
      b.appendChild(im);
      /* The rail is an index, not a heading: five of the eleven slots end in
         "System", which costs the width that makes the other words truncate.
         The full name is in the title and on the slot browser's heading. */
      var tx = el('span', 'node__txt');
      tx.appendChild(el('span', 'node__name', slot.name.replace(/\s+System$/i, '')));
      var f = el('span', 'node__fill' + (filled === slot.count ? ' is-full' : ''));
      var bb = el('b', null, filled);
      f.appendChild(bb);
      f.appendChild(document.createTextNode(' / ' + slot.count + (slot.count === 1 ? ' position' : ' positions')));
      tx.appendChild(f);
      b.appendChild(tx);

      b.dataset.slot = slot.id;
      b.onclick = function () { Snd.play('select'); selectSlot(slot.id); };
      (i < half ? railL : railR).appendChild(b);

      var p = pinPos(a);
      var pin = el('span', 'pin' + (filled ? ' is-filled' : '') + (lit ? ' is-on' : ''));
      pin.style.left = p.x + '%';
      pin.style.top = p.y + '%';
      pin.dataset.slot = slot.id;
      pins.appendChild(pin);

      /* the rail is an index into the scan: pointing at a slot lights its pin */
      var hot = function () { pin.classList.add('is-hot'); };
      var cold = function () { pin.classList.remove('is-hot'); };
      b.addEventListener('mouseenter', hot); b.addEventListener('focus', hot);
      b.addEventListener('mouseleave', cold); b.addEventListener('blur', cold);
    });

    // the rails must not end on an empty cell: the shorter one carries capacity.
    // When this doc sells capacity shards (shop.capacityItems), the tile is the
    // way in to buying one.
    var shards = (Data.shop() && Data.shop().capacityItems) || [];
    var capTile = el(shards.length ? 'button' : 'div', 'node node--cap' + (shards.length ? ' node--capbuy' : ''));
    capTile.appendChild(el('span', 'node__icon'));
    var ctx = el('span', 'node__txt');
    ctx.appendChild(el('span', 'node__name', 'Capacity'));
    var cf = el('span', 'node__fill');
    cf.appendChild(el('b', null, st.capacity.used));
    cf.appendChild(document.createTextNode(' / ' + st.capacity.max + ' used'));
    ctx.appendChild(cf);
    if (shards.length) {
      capTile.type = 'button';
      capTile.id = 'cap-shop';
      ctx.appendChild(el('span', 'node__cta', 'Raise it \u2192'));
      capTile.setAttribute('aria-label', 'Capacity, ' + st.capacity.used + ' of ' + st.capacity.max +
        ' used. Buy capacity from this ripperdoc.');
      capTile.onclick = function () { Snd.play('open'); openCapacityShop(); };
    }
    capTile.appendChild(ctx);
    (railL.childNodes.length <= railR.childNodes.length ? railL : railR).appendChild(capTile);

    var installed = 0;
    for (var u in st.instances) {
      if (st.instances.hasOwnProperty(u) && st.instances[u].installed) { installed++; }
    }
    $('body-sub').textContent = installed + ' installed · ' + (st.stash || []).length + ' in stash';

    layoutPins();
    /* Fonts load with font-display: block and the scan decodes asynchronously,
       so the pane's height can still change after this pass. Re-measure on the
       next frame, or the pins sit where the layout used to be. */
    if (global.requestAnimationFrame) { global.requestAnimationFrame(layoutPins); }
    renderTotals();
  }

  function renderTotals() {
    var st = Data.state();
    var list = $('totals-list');
    clear(list);
    var keys = Object.keys(st.totals || {}).sort(function (a, b) {
      return Data.statLabel(a).localeCompare(Data.statLabel(b));
    });
    if (!keys.length) {
      var none = el('li', 'tot');
      none.className = 'tot tot--none';
      none.appendChild(el('span', 'tot__k', 'Nothing installed yet. Every stat your chrome gives adds up here.'));
      list.appendChild(none);
    }
    keys.forEach(function (k) {
      var li = el('li', 'tot');
      li.appendChild(el('span', 'tot__k', Data.statLabel(k)));
      li.appendChild(el('span', 'tot__v', Data.formatStat(k, st.totals[k])));
      li.title = Data.statLabel(k) + ': ' + Data.formatStat(k, st.totals[k]) +
                 ' (the game receives ' + Data.formatStat(k, (st.applied || {})[k] || 0) + ')';
      list.appendChild(li);
    });

    /* With no chrome yet the totals block is one sentence, and the column ended
       on a band of nothing. The body scan IS the slot view (the CP2077 habit of
       a big body on the left), so it takes that space back until there are
       totals to show, and gives it up again the moment there are. */
    var pane = $('pane-body');
    if (pane) { pane.dataset.lean = keys.length ? '' : 'yes'; }

    var ab = $('abil-list');
    clear(ab);
    (st.abilities || []).forEach(function (id) {
      var li = el('li', 'abil__i', Data.abilityName(id));
      li.title = Data.abilityDesc(id);
      ab.appendChild(li);
    });
  }

  /* ═══════════════════ render: slot browser ═══════════════════ */
  var listKey = null;
  function renderList() {
    var title = $('list-title'), sub = $('list-sub');
    var rows = $('rows'), none = $('rows-none');
    clear(rows);
    ui.rowIds = [];

    var source, heading, slot = null;
    /* during a catalog-wide search the slot filter has nothing to filter */
    $('find-slot').closest('.srch').hidden = !!ui.globalQuery;
    if (ui.globalQuery) {
      source = Data.everyItemRow();
      heading = 'All Cyberware';
      sub.textContent = 'matching \u201c' + ui.globalQuery + '\u201d';
    } else if (ui.slot) {
      source = Data.catalogForSlot(ui.slot);
      slot = Data.slotState(ui.slot);
      heading = slot ? slot.name : ui.slot;
      var filled = 0;
      for (var k = 0; slot && k < slot.installed.length; k++) { if (slot.installed[k]) { filled++; } }
      sub.textContent = slot ? filled + ' of ' + slot.count + ' positions filled' : '';
    } else {
      title.textContent = 'Pick a slot';
      sub.textContent = '';
      none.hidden = false;
      emptyState(none, 'Choose a slot on the chrome map to see what fits it.');
      $('rows-count').textContent = '';
      return;
    }
    title.textContent = heading;

    var q = ui.globalQuery || ui.query;
    var toks = tokens(q);
    var shown = [];
    source.forEach(function (r) {
      var rk = rank(r.item.name, itemHay(r.item), toks);
      if (rk < 0) { return; }
      if (ui.filter === 'installed' && !r.installed) { return; }
      if (ui.filter === 'stash' && !r.owned) { return; }
      if (ui.filter === 'buy' && (r.installed || r.owned || !r.affordable)) { return; }
      if (isSetup() && !r.installed && !r.owned) { return; }   /* your setup: only what you own */
      r.rank = rk;
      shown.push(r);
    });

    /* searching: best match first. browsing: installed, owned, affordable, rest */
    function status(r) { return r.installed ? 0 : r.owned ? 1 : r.affordable ? 2 : 3; }
    shown.sort(function (a, b) {
      if (toks.length && a.rank !== b.rank) { return a.rank - b.rank; }
      if (status(a) !== status(b)) { return status(a) - status(b); }
      return a.item.name.localeCompare(b.item.name);
    });

    /* ONE insertion, not one per row: a keystroke in the filter rebuilds the
       whole list, and at 2560x1440 the catalog list is 122 rows of ~10 nodes
       each. Ultralight has no fast path for 122 separate appends.
       Rows are re-used WHENEVER the piece is one this list has built before,
       including across a change of list. The first keystroke into the global
       search is exactly such a change - a slot's rows become the whole
       catalog - and it is the keystroke the player feels, because it is the
       one that builds the longest list. Measured over six interleaved pairs:
       tying the cache to the list instead left that keystroke at 13.8-18.8 ms
       while every later one was 7-8 ms.
       `sameList` therefore decides only the arrival STAGGER, which still plays
       on a change of list and never on a keystroke, exactly as before. */
    var listKeyNow = (ui.globalQuery ? 'g' : 's:' + ui.slot) + '|' + ui.filter;
    var sameList = listKeyNow === listKey;
    var frag = document.createDocumentFragment();
    shown.forEach(function (r) {
      frag.appendChild(rowNode(r, toks));
      ui.rowIds.push(r.item.id);
    });
    rows.appendChild(frag);

    /* global search also looks through the cyber perks: offer the jump */
    if (ui.globalQuery) {
      var perkHits = (Data.state().cyberPerks || []).filter(function (p) {
        var d = Data.perk(p.id) || {};
        return rank(p.name, (p.name + ' ' + (d.description || '')).toLowerCase(), toks) >= 0;
      }).length;
      if (perkHits) {
        var li = el('li', 'rows__more');
        li.setAttribute('role', 'presentation');
        var go = el('button', 'morelink');
        go.type = 'button';
        go.appendChild(el('span', 'morelink__t', perkHits + ' cyber perk' + (perkHits === 1 ? '' : 's') +
                                                  ' match \u201c' + ui.globalQuery + '\u201d'));
        go.appendChild(el('span', 'morelink__a', 'Show in Perks \u2192'));
        go.onclick = function () { showPerksFor(ui.globalQuery); };
        li.appendChild(go);
        rows.appendChild(li);
      }
    }

    none.hidden = shown.length > 0;
    if (!shown.length) {
      var acts = [];
      if (q) {
        acts.push({ label: ui.globalQuery ? 'Clear search' : 'Clear filter', run: function () {
          Snd.play('clear');
          if (ui.globalQuery) { setSearch('find-all', ''); } else { setSearch('find-slot', ''); }
        } });
        if (!ui.globalQuery && Data.everyItemRow().some(function (r) { return rank(r.item.name, itemHay(r.item), toks) >= 0; })) {
          acts.push({ label: 'Search every slot', run: function () {
            var v = ui.query; setSearch('find-slot', ''); setSearch('find-all', v); $('find-all').focus();
          } });
        }
      }
      if (ui.filter !== 'all') {
        acts.push({ label: 'Show all', run: function () { setFilter('all'); } });
      }
      var why = (isSetup() && !q)
        ? (ownsAny() ? 'You have no ' + heading + ' chrome yet.' : 'You have no chrome yet.')
        : q
        ? 'Nothing ' + (ui.globalQuery ? (isSetup() ? 'you own' : 'in the catalog') : 'in ' + heading) + ' matches \u201c' + q + '\u201d' +
          (ui.filter !== 'all' ? ' under \u201c' + FILTER_NAMES[ui.filter] + '\u201d.' : '.')
        : ({ installed: 'Nothing is installed in ' + heading + ' yet.',
             stash: 'Your stash has nothing for ' + heading + '.',
             buy: 'Nothing for ' + heading + ' that you can afford and do not already own.' })[ui.filter] ||
          'This ripperdoc has nothing for ' + heading + '.';
      emptyState(none, why, acts);
    }
    $('rows-count').textContent = isSetup() ? shown.length + ' owned'
      : shown.length + (shown.length === 1 ? ' piece' : ' pieces') + (shown.length !== source.length ? ' of ' + source.length : '');
    fitKeys();

    /* the list arrives with a short stagger when its CONTENT changes (another
       slot, filter or search mode), never on a keystroke or a selection */
    if (!sameList) {
      listKey = listKeyNow;
      /* ONE reflow for the whole list (not one per row): updateRow has just
         taken `fx-in` off the recycled nodes, and without a style flush between
         the removal and Fx.enter's re-add the engine sees no change and the
         entrance never replays. This costs a single forced layout, and only on
         a render that changes which list is shown - never while typing. */
      void rows.offsetWidth;
      Fx.enter(rows.querySelectorAll('.row, .morelink'), 'fx-in', 16, 12);
    }

    renderPositions();

    // the watermark belongs to whatever slot is on screen
    /* The watermark is this pane's art, and hiding it during a global search is
       what left a 1000px slab of bare background under a one-hit list (probe 74,
       MENU-AAA A4). A search still has a subject: the slot of whatever the
       detail is showing, and the body scan when nothing is picked yet. */
    renderListMark();

    /* last, once the positions block and the footer have claimed their height:
       end the list on a whole row instead of slicing one (VISUAL-GATE M1) */
    snapRows();
  }

  /* The watermark belongs to whatever slot is on screen; see renderList. */
  function renderListMark() {
    var mi = $('list-mark-img');
    var markSlot = ui.slot;
    if (ui.globalQuery) {
      var sel = ui.itemId && Data.item(ui.itemId);
      markSlot = sel ? sel.slot : null;
    }
    if (markSlot) { mi.src = 'icons/' + art(markSlot).icon; mi.style.display = ''; }
    else { mi.src = 'icons/body-scan.png'; mi.style.display = ''; }
  }

  /* Move the selection highlight WITHOUT rebuilding the list. Typing in the
     global search retargets the detail at the new top hit on every keystroke,
     and that used to call renderList() a SECOND time for the same keystroke
     purely to move one class - 122 rows of DOM thrown away and rebuilt to
     light a different border. */
  function markSelectedRow() {
    var nodes = $('rows').querySelectorAll('[data-item]');
    for (var i = 0; i < nodes.length; i++) {
      nodes[i].classList.toggle('is-on', nodes[i].dataset.item === ui.itemId);
    }
    renderListMark();
  }

  var FILTER_NAMES = { all: 'All', installed: 'Installed', stash: 'Stash', buy: 'Can Buy' };

  /* A slot holds `count` pieces at numbered positions, and install takes an
     index. Showing the positions is the only way the player can see which one
     is free, and it is where "install into THIS one" lives. */
  function renderPositions() {
    var box = $('positions'), cells = $('pos-cells');
    clear(cells);
    var slot = ui.slot && !ui.globalQuery ? Data.slotState(ui.slot) : null;
    if (!slot) { box.hidden = true; return; }
    box.hidden = false;

    var st = Data.state();
    var sel = currentInstance();
    var canPlace = !isSetup() && sel && !sel.installed && Data.item(sel.item).slot === slot.id;
    var allFull = true;
    for (var f = 0; f < slot.count; f++) { if (!slot.installed[f]) { allFull = false; } }
    var canSwap = canPlace && allFull;
    $('pos-h').textContent = slot.name + ' · ' + slot.count +
      (slot.count === 1 ? ' position' : ' positions');

    for (var i = 0; i < slot.count; i++) {
      var uid = slot.installed[i];
      var inst = uid ? st.instances[uid] : null;
      var b = el('button', 'poscell' + (inst ? ' is-full' : '') +
                           (sel && inst && inst.uid === sel.uid ? ' is-on' : ''));
      b.type = 'button';
      b.appendChild(el('span', 'poscell__n', i + 1));
      var mid = el('span', 'poscell__mid');
      mid.appendChild(el('span', 'poscell__name' + (inst ? '' : ' is-empty'),
                         inst ? inst.name : 'Empty'));
      mid.appendChild(el('span', 'poscell__sub', inst
        ? (canSwap ? 'Swap for ' + sel.name : Data.tierName(inst.tier) + ' · ' + inst.capacityCost + ' cap')
        : (canPlace ? 'Install ' + sel.name + ' here' : 'Nothing fitted')));
      b.appendChild(mid);

      if (inst && canSwap) {
        b.classList.add('is-swap');
        (function (idx, piece, out) {
          b.onclick = function () { confirmSwap(piece, idx, out, slot); };
        })(i, sel, inst);
        b.title = 'Replace ' + inst.name + ' with ' + sel.name;
      } else if (inst) {
        (function (it) { b.onclick = function () { Snd.play('select'); selectItem(it.item, it.uid); }; })(inst);
        b.title = 'Show ' + inst.name;
      } else if (canPlace) {
        b.classList.add('is-open');
        (function (idx, piece) {
          b.onclick = function () {
            preview('Install ' + piece.name,
              'Into ' + slot.name + ' position ' + (idx + 1) + '. Takes ' +
              piece.capacityCost + ' of your cyberware capacity.',
              'install', { uid: piece.uid, index: idx }, 'Install');
          };
        })(i, sel);
      } else {
        b.disabled = true;
        b.title = 'Pick a piece you own to fit it here.';
      }
      cells.appendChild(b);
    }
  }

  /* ── what a row SAYS, in one place ────────────────────────────────────
     Both the fresh path (buildRow) and the recycled path (updateRow) read
     these, so a row that is reused can never drift from a row that is built. */
  function rowMeta(r) {
    // ONE truncating metadata line, never a wrapping pill row
    var it = r.item;
    var bits = [it.manufacturer, Data.tierName(r.displayTier)];
    if (it.iconic) { bits.push('Iconic'); }
    if (r.installed) { bits.push('Installed'); }
    else if (r.owned) { bits.push('In stash'); }
    if (it.modSlotTypes && it.modSlotTypes.length) { bits.push(it.modSlotTypes.length + ' mod slots'); }
    /* SPEC's catalog leaves manufacturer off most pieces, so drop empties
       rather than render a leading separator */
    return bits.filter(function (b) { return b; }).join('  \u00b7  ');
  }
  function rowPrice(price, r) {
    price.title = '';
    if (r.installed) { price.textContent = 'Installed'; price.className = 'row__price is-have'; }
    else if (r.owned) { price.textContent = 'Owned'; price.className = 'row__price is-have'; }
    else if (r.cheapest) {
      price.textContent = Data.money(r.cheapest.price);
      price.className = 'row__price' + (r.affordable ? '' : ' is-poor');
    } else if (r.locked) {
      price.textContent = 'Level ' + r.locked.requiresLevel; price.className = 'row__price is-locked';
      price.title = 'Needs level ' + r.locked.requiresLevel;
    } else {
      /* Never a bare dash: a row you cannot buy says why on the row itself.
         The full sentence (who does sell it, and where) is on the panel. */
      var elsewhere = Data.stockedElsewhere(r.item.id);
      price.textContent = elsewhere.length ? 'Not sold here' : 'Not sold';
      price.className = 'row__price is-why';
      price.title = elsewhere.length ? Data.stockedElsewhereLine(r.item.id)
                                     : 'No ripperdoc stocks this piece.';
    }
  }
  /* the row's own text runs name, metadata and price together; spell the
     important part out for assistive tech and controller focus */
  function rowAria(r) {
    return r.item.name + ', ' + Data.tierName(r.displayTier) +
      (r.installed ? ', installed' : r.owned ? ', in your stash' :
        (r.cheapest ? ', ' + Data.money(r.cheapest.price) + ' ' + Data.state().currency.name
                    : r.locked ? ', needs level ' + r.locked.requiresLevel : ', not stocked')) +
      ', ' + r.capacityCost + ' capacity';
  }
  function rowClass(r) {
    return 'row' + (ui.itemId === r.item.id ? ' is-on' : '') +
           (!r.installed && !r.owned && !r.affordable ? ' is-dim' : '');
  }

  /* ── row recycling (E, beta update 2) ─────────────────────────────────
     Every keystroke in the filter used to throw away the whole list and build
     it again: 100+ rows of a dozen nodes each, an <img> with its fallback
     wiring per row. Measured at 30.1 ms for the worst keystroke on build E,
     against a 16.7 ms frame.
     A keystroke does not change what a row IS, only which rows are listed, in
     what order, and where the search marks fall. So the row elements are kept
     and re-used: `updateRow` rewrites every field that can possibly differ -
     the class, aria-selected, the marked-up name, the metadata line, the price
     and its class and title, the capacity and the aria-label - which is the
     whole of what buildRow derives from `r` and from state. Only the icon is
     left alone, because it comes from the catalog entry and cannot change.
     The cache is dropped whenever the list is a DIFFERENT list (another slot,
     filter or search mode), which is exactly when the stagger animation plays:
     those renders build fresh nodes, so `Fx.enter` still animates an arrival
     that has never been seen. Within one list - which is what typing is - the
     nodes are re-used and nothing animates, which is also what you want while
     somebody is typing. */
  var rowCache = {};
  function updateRow(li, r, toks) {
    var b = li.firstChild;
    if (!b || !b.dataset || b.dataset.item !== r.item.id) { return null; }   /* never trust a stale node */
    b.className = rowClass(r);
    b.setAttribute('aria-selected', ui.itemId === r.item.id ? 'true' : 'false');
    var nm = b.querySelector('.row__name');
    clear(nm);
    nm.appendChild(hl(r.item.name, toks));
    b.querySelector('.row__meta').textContent = rowMeta(r);
    rowPrice(b.querySelector('.row__price'), r);
    b.querySelector('.row__cap').textContent = r.capacityCost + ' cap';
    b.setAttribute('aria-label', rowAria(r));
    /* a recycled node must not carry the entrance it played in an earlier list;
       renderList forces ONE reflow before Fx.enter so re-adding it restarts */
    b.classList.remove('fx-in');
    b.style.animationDelay = '';
    return li;
  }
  function rowNode(r, toks) {
    var id = r.item.id;
    var li = rowCache[id] && updateRow(rowCache[id], r, toks);
    if (!li) { li = buildRow(r, toks); rowCache[id] = li; }
    return li;
  }

  function buildRow(r, toks) {
    var it = r.item;
    var li = el('li');
    li.setAttribute('role', 'presentation');

    var b = el('button', 'row' + (ui.itemId === it.id ? ' is-on' : '') +
                         (!r.installed && !r.owned && !r.affordable ? ' is-dim' : ''));
    b.type = 'button';
    b.setAttribute('role', 'option');
    b.setAttribute('aria-selected', ui.itemId === it.id ? 'true' : 'false');
    b.dataset.item = it.id;

    var wrap = el('span', 'row__iconwrap');
    var ic = iconFor(it);
    wrap.appendChild(imgWithFallback('row__icon', ic.src, ic.fallback, ''));
    b.appendChild(wrap);

    var mid = el('span', 'row__mid');
    var nm = el('span', 'row__name');
    nm.appendChild(hl(it.name, toks));
    mid.appendChild(nm);

    mid.appendChild(el('span', 'row__meta', rowMeta(r)));
    b.appendChild(mid);

    var right = el('span', 'row__right');
    var price = el('span', 'row__price');
    rowPrice(price, r);
    right.appendChild(price);
    right.appendChild(el('span', 'row__cap', r.capacityCost + ' cap'));
    b.appendChild(right);

    /* the row's own text runs name, metadata and price together; spell the
       important part out for assistive tech and controller focus */
    b.setAttribute('aria-label', rowAria(r));
    b.onclick = function () { Snd.play('select'); selectItem(it.id); };
    li.appendChild(b);
    return li;
  }

  /* ═══════════════════ render: detail ═══════════════════ */
  function currentInstance() {
    var st = Data.state();
    if (ui.uid && st.instances[ui.uid]) { return st.instances[ui.uid]; }
    if (!ui.itemId) { return null; }
    var installed = null, owned = null;
    for (var u in st.instances) {
      if (!st.instances.hasOwnProperty(u)) { continue; }
      if (st.instances[u].item !== ui.itemId) { continue; }
      if (st.instances[u].installed) { installed = st.instances[u]; } else if (!owned) { owned = st.instances[u]; }
    }
    return installed || owned;
  }

  var detailKey = null, tabKey = null;
  var TAB_ORDER = ['stats', 'tiers', 'mods', 'rolls', 'looks'];
  function renderDetail() {
    var blank = $('detail-blank'), dt = $('detail');
    var it = ui.itemId ? Data.item(ui.itemId) : null;

    /* An id with no catalog entry is a real case, not a harness typo: UI-API
       §3 keeps "orphans" -- saved pieces whose catalog entry is gone. Say so
       instead of throwing halfway through a render and leaving a stale panel. */
    if (ui.itemId && !it) {
      blank.hidden = false; dt.hidden = true;
      $('detail-blank').querySelector('.blank__t').textContent =
        'This piece is not in the catalog any more. Its id is "' + ui.itemId +
        '". It is kept in your save, but it cannot be shown.';
      return;
    }
    if (!it) {
      blank.hidden = false; dt.hidden = true;
      var bt = $('detail-blank').querySelector('.blank__t');
      blank.classList.toggle('blank--none', isSetup() && !ownsAny());
      if (isSetup() && !ownsAny()) {
        /* no chrome at all: the screen says so on purpose, and where to get some */
        clear(bt);
        bt.appendChild(el('span', 'blank__h', 'No chrome yet'));
        bt.appendChild(el('span', 'blank__s', 'Your installed cyberware, its stats and your capacity show here once a ripperdoc fits some.'));
        var docs = el('span', 'blank__docs');
        docList().forEach(function (d) { docs.appendChild(el('span', 'blank__doc', d)); });
        bt.appendChild(docs);
      } else if (ui.slot && !ui.globalQuery) {
        var ss = Data.slotState(ui.slot);
        bt.textContent = 'Pick a piece from ' + (ss ? ss.name : 'the list') + ' to see it here.';
      } else {
        bt.innerHTML = 'Pick a slot on the chrome map,<br>then a piece of cyberware.';
      }
      detailKey = null;
      return;
    }
    blank.hidden = true; dt.hidden = false;

    var inst = currentInstance();
    var r = Data.rowFor(it);

    /* a different piece slides in; a re-render of the same one stays still */
    var dkey = it.id + '|' + (inst ? inst.uid : '');
    if (dkey !== detailKey) { detailKey = dkey; tabKey = null; Fx.replay(dt, 'fx-detail', 420); }

    var ic = iconFor(it);
    var img = $('d-icon');
    img.onerror = function () { img.onerror = null; img.src = ic.fallback; };
    img.src = ic.src || ic.fallback;
    img.alt = it.name;

    $('d-maker').textContent = it.manufacturer || '';
    $('d-name').textContent = it.name;
    /* The tier ON SCREEN, never the T5 reference sentence: the instance's own
       tier when the player owns it, else the tier this row displays. */
    var shownTier = inst ? inst.tier : r.displayTier;
    $('d-desc').textContent = Data.tierDesc(it, shownTier);

    var chips = $('d-chips');
    clear(chips);
    var tc = el('span', 'chip chip--tier');
    tc.style.color = Data.tierColor(shownTier);
    tc.textContent = Data.tierName(shownTier);
    chips.appendChild(tc);
    if (it.iconic) { chips.appendChild(el('span', 'chip chip--iconic', 'Iconic')); }
    if (inst && inst.installed) {
      var slot = Data.slotState(inst.installed.slot);
      chips.appendChild(el('span', 'chip chip--in',
        'Installed · ' + (slot ? slot.name : inst.installed.slot) +
        (slot && slot.count > 1 ? ' ' + (inst.installed.index + 1) : '')));
    } else if (inst) {
      chips.appendChild(el('span', 'chip chip--stash', 'In stash'));
    }
    var capChip = el('span', 'chip chip--tier',
      (inst ? inst.capacityCost : r.capacityCost) + ' capacity');
    capChip.style.color = 'var(--c)';
    chips.appendChild(capChip);

    var dm = $('dt-mark-img');
    dm.onerror = function () { dm.onerror = null; dm.src = ic.fallback; };
    dm.src = ic.src || ic.fallback;

    renderTab();
    renderFit();
    renderActions();
  }

  function renderTab() {
    var panel = $('dt-panel');
    clear(panel);
    /* tab change: the panel comes in from the side you moved towards */
    if (tabKey && tabKey !== ui.tab) {
      Fx.replay(panel, TAB_ORDER.indexOf(ui.tab) > TAB_ORDER.indexOf(tabKey) ? 'fx-tab-r' : 'fx-tab-l', 360);
    }
    tabKey = ui.tab;
    var it = Data.item(ui.itemId);
    var inst = currentInstance();
    ({
      stats: tabStats, tiers: tabTiers, mods: tabMods,
      rolls: tabRolls, looks: tabLooks
    })[ui.tab](panel, it, inst);
    /* `description` (the boxed line above the tabs) says what the piece does; `lore` (optional,
       docs/CATALOG-SCHEMA.md) says what it is. It closes the Stats page, inside the panel, because the
       panel is the one part of the card that scrolls: at 1080 a long lore under the description squeezed
       the stat table to three rows (probe 75b). No lore: no block and no gap. */
    var lore = String(it.lore || '').trim();
    if (ui.tab === 'stats' && lore) {
      var lp = el('p', 'dt__lore', lore);
      lp.id = 'd-lore';
      panel.appendChild(lp);
    }
  }

  /* ── Stats: what this piece gives, and what changes if you fit it ── */
  function tabStats(panel, it, inst) {
    var st = Data.state();
    var tierNo = inst ? inst.tier : Data.rowFor(it).displayTier;
    var tdef = Data.tierDef(it, tierNo);
    var source = inst ? inst.stats : (tdef ? tdef.stats : {});

    /* A fact strip so the tab reads as a complete page even for a piece with
       one stat: what it is, what it costs to carry, what it can take. */
    var facts = el('div', 'facts');
    function fact(k, v, tone) {
      var d = el('div', 'fact');
      d.appendChild(el('span', 'fact__k', k));
      var val = el('span', 'fact__v' + (tone ? ' ' + tone : ''), v);
      d.appendChild(val);
      facts.appendChild(d);
    }
    fact('Tier', Data.tierName(tierNo));
    fact('Capacity', String(inst ? inst.capacityCost : (tdef ? tdef.capacityCost : '—')), 'is-cyan');
    fact('Mod slots', inst
      ? inst.mods.filter(Boolean).length + ' / ' + inst.modSlotTypes.length
      : String((tdef && tdef.modSlots) || 0));
    fact('Rolled stats', inst ? String((inst.rolls || []).length)
      : String(((tdef && tdef.rolls) || []).reduce(function (n, r) { return n + (r.count || 1); }, 0)));
    if (inst && inst.installed) {
      var sl = Data.slotState(inst.installed.slot);
      fact('Fitted', (sl ? sl.name : inst.installed.slot) +
        (sl && sl.count > 1 ? ' ' + (inst.installed.index + 1) : ''), 'is-good');
    } else if (inst) {
      fact('Where', 'In your stash', 'is-cyan');
    } else {
      var rowInfo = Data.rowFor(it), row = rowInfo.cheapest;
      fact('Price', row ? Data.money(row.price) : rowInfo.locked ? 'Needs level ' + rowInfo.locked.requiresLevel : 'Not stocked', 'is-y');
    }
    if (it.iconic) { fact('Class', 'Iconic', 'is-y'); }
    else {
      /* no dash: either what it sells back for, or why there is no figure */
      fact('Resale', !inst ? 'Once you own it'
        : inst.sellPrice === null ? 'Cannot be sold' : Data.money(inst.sellPrice));
    }
    panel.appendChild(facts);

    var table = el('div', 'stats');
    var head = el('div', 'st st--head');
    head.appendChild(el('span', 'st__k', inst ? 'This piece gives' : 'At ' + Data.tierName(tierNo)));
    head.appendChild(el('span', 'st__v', 'Value'));
    head.appendChild(el('span', 'st__d', inst && inst.installed ? 'Of your total' : ''));
    table.appendChild(head);

    var keys = Object.keys(source).sort(function (a, b) {
      return Data.statLabel(a).localeCompare(Data.statLabel(b));
    });
    if (!keys.length) {
      var r0 = el('div', 'st st--note');
      r0.appendChild(el('span', 'st__k', 'No direct stats \u2014 this piece works through its effect.'));
      table.appendChild(r0);
    }
    keys.forEach(function (k) {
      var row = el('div', 'st');
      row.appendChild(el('span', 'st__k', Data.statLabel(k)));
      row.appendChild(el('span', 'st__v', Data.formatStat(k, source[k])));
      var share = '';
      if (inst && inst.installed && st.totals[k]) {
        share = Math.round(source[k] / st.totals[k] * 100) + '%';
      }
      row.appendChild(el('span', 'st__d is-same', share));
      table.appendChild(row);
    });
    panel.appendChild(table);

    /* The piece's own ability usually carries the same text as the description
       at the top of the panel; repeating it is noise. Only abilities that say
       something new are listed. */
    var ownDesc = Data.tierDesc(it, tierNo);
    var extra = ((tdef && tdef.abilities) || []).filter(function (a) {
      var d = Data.abilityDescAt(a, tierNo);
      return d && d !== ownDesc;
    });
    if (extra.length) {
      var ab = el('div', 'note');
      ab.textContent = 'Grants: ' + extra.map(function (a) {
        return Data.abilityName(a) + ' — ' + Data.abilityDescAt(a, tierNo);
      }).join('  ·  ');
      panel.appendChild(ab);
    }
    if (inst && inst.mods && inst.mods.length) {
      var fitted = inst.mods.filter(function (m) { return !!m; }).length;
      if (fitted) {
        var n = el('div', 'note');
        n.textContent = 'Includes ' + fitted + ' fitted mod' + (fitted === 1 ? '' : 's') +
                        '. Open the Mods tab to see or change them.';
        panel.appendChild(n);
      }
    }
  }

  /* ── Tiers: the ladder, including iconic tracks, with upgrade costs ── */
  function tabTiers(panel, it, inst) {
    var st = Data.state();
    var rows = Data.shopRowsFor(it.id);
    var ladder = el('div', 'ladder');

    it.tiers.forEach(function (t) {
      var shopRow = null;
      for (var i = 0; i < rows.length; i++) { if (rows[i].tier === t.tier) { shopRow = rows[i]; } }
      var isCur = inst && inst.tier === t.tier;
      var isNext = inst && inst.upgrade && inst.upgrade.tier === t.tier;

      var b = el('button', 'rung' + (isCur ? ' is-cur' : ''));
      b.type = 'button';
      b.style.setProperty('--t', Data.tierColor(t.tier));

      b.appendChild(el('span', 'rung__t', Data.tierName(t.tier)));

      var bits = [];
      var sk = Object.keys(t.stats || {});
      sk.forEach(function (k) { bits.push(Data.statLabel(k) + ' ' + Data.formatStat(k, t.stats[k])); });
      bits.push(t.capacityCost + ' cap');
      if (t.modSlots) { bits.push(t.modSlots + ' mod slot' + (t.modSlots === 1 ? '' : 's')); }
      if (t.requires && t.requires.level) { bits.push('Lv ' + t.requires.level); }
      /* The rung is the tier the press LEADS to, so it carries that tier's own
         sentence under its numbers -- but only where the catalog has one for
         this tier: without the per-tier field every rung would repeat the one
         reference sentence, so an older catalog shows none at all. */
      var mid = el('span', 'rung__m');
      mid.appendChild(el('span', 'rung__s', bits.join('  ·  ')));
      if (t.description) { mid.appendChild(el('span', 'rung__d', t.description)); }
      b.appendChild(mid);

      var p = el('span', 'rung__p');
      if (isCur) {
        p.textContent = 'Current';
        p.style.color = 'var(--y)';
      } else if (isNext) {
        p.textContent = Data.money(inst.upgrade.price);
        var s = el('small', null, 'upgrade');
        p.appendChild(s);
      } else if (!inst && shopRow) {
        p.textContent = Data.money(shopRow.price);
        p.appendChild(el('small', null, 'buy'));
      } else {
        /* a rung with no price says what it is, not a dash: "Not sold" here,
           a tier below the one you own, or one you cannot reach yet */
        p.textContent = !inst ? 'Not sold' : (t.tier < inst.tier ? 'Below yours' : 'Later');
        p.style.color = 'var(--ink2)';
      }
      b.appendChild(p);

      var blocked = null;
      if (isCur) { blocked = 'This is the tier you have.'; }
      else if (inst && !isNext) {
        blocked = inst.tier > t.tier ? 'Below your current tier.'
                                     : 'Upgrade one tier at a time.';
      } else if (isNext && !inst.upgrade.available) {
        blocked = inst.upgrade.reason === 'E_LEVEL'
          ? 'Needs level ' + inst.upgrade.requiresLevel + ' (you are ' + st.level + ').'
          : inst.upgrade.reason === 'E_COMPONENTS' ? 'Missing components: the bill is under the ladder.'
          : 'You cannot afford this yet.';
      } else if (!inst && (!shopRow || !shopRow.available)) {
        blocked = !shopRow ? 'This ripperdoc does not stock that tier.'
          : (shopRow.reason === 'E_LEVEL' ? 'Needs level ' + shopRow.requiresLevel + '.'
             : shopRow.reason === 'E_NO_FUNDS' ? 'You cannot afford this yet.' : 'Not available here.');
      }
      if (blocked) { b.disabled = true; b.title = blocked; }

      b.onclick = function () {
        if (isNext) { confirmUpgrade(inst); }
        else if (!inst && shopRow) { confirmBuy(it, shopRow); }
      };
      ladder.appendChild(b);
    });
    panel.appendChild(ladder);

    var tierBill = inst && inst.upgrade && billNode(inst.upgrade.components,
      'Upgrading to ' + Data.tierName(inst.upgrade.tier) + ' uses');
    if (tierBill) { panel.appendChild(tierBill); }

    if (inst && inst.upgrade) {
      panel.appendChild(el('div', 'note',
        'Upgrading re-rolls this piece’s own rolled stats for the new tier, and its mod ' +
        'slots grow or shrink with the tier. Anything pushed out goes back to your mod stash.'));
    } else if (inst && !inst.upgrade) {
      panel.appendChild(el('div', 'note', 'This piece is at its highest tier.'));
    }
    if (it.iconic) {
      panel.appendChild(el('div', 'note',
        'Iconic. Only one can be owned, it cannot be sold, and it carries abilities ' +
        'ordinary cyberware does not.'));
    }

    /* MENU-AAA A14: open on the rung that matters. A piece owned at T5 of 7 used
       to open this tab on T1 and make the player scroll to find themselves, and
       the sentence each rung now carries makes a rung about twice as tall, so
       fewer fit. The rung they are ON goes to the top of the panel; the one they
       can upgrade to is the next one down. Only the panel's own scroll is
       touched - never scrollIntoView, which can scroll an ancestor or the
       document - and only when the panel actually scrolls. */
    var mine = ladder.querySelector('.rung.is-cur') || ladder.querySelector('.rung:not([disabled])');
    if (mine) {
      /* WHICH element scrolls is not the same at every width: on the three-pane
         layout it is #dt-panel, and below 1500 the panel grows to its content
         and #pane-detail scrolls instead. Walk up to whichever one actually
         scrolls, and move it only as far as it takes to bring the rung inside
         the visible box - never to the top, which would push the card and the
         tabs off screen on the one-pane layout and lose the player their place. */
      var sc = mine.parentElement;
      while (sc && sc !== document.body &&
             !(sc.scrollHeight > sc.clientHeight + 1 && /auto|scroll/.test(getComputedStyle(sc).overflowY))) {
        sc = sc.parentElement;
      }
      if (sc && sc !== document.body) {
        var r = mine.getBoundingClientRect(), box = sc.getBoundingClientRect(), pad = 8;
        if (r.bottom > box.bottom - pad) { sc.scrollTop += (r.bottom - box.bottom) + pad; }
        else if (r.top < box.top + pad) { sc.scrollTop -= (box.top - r.top) + pad; }
      }
    }
  }

  /* ── Mods: one bay per ACTIVE mod slot at this tier ── */
  function tabMods(panel, it, inst) {
    if (!it.modSlotTypes || !it.modSlotTypes.length) {
      panel.appendChild(el('div', 'note', 'This piece takes no mods.'));
      return;
    }
    if (!inst) {
      panel.appendChild(el('div', 'note',
        'Mods are fitted to a piece you own. Buy this one first — it opens ' +
        it.modSlotTypes.length + ' mod slot' + (it.modSlotTypes.length === 1 ? '' : 's') +
        ' as its tier rises.'));
      return;
    }
    var st = Data.state();
    var bays = el('div', 'bays');

    inst.modSlotTypes.forEach(function (type, i) {
      var uid = inst.mods[i];
      var mod = uid ? st.mods[uid] : null;
      var b = el('button', 'bay' + (mod ? ' is-full' : ''));
      b.type = 'button';
      b.appendChild(el('span', 'bay__n', i + 1));
      /* a fitted mod shows its own art, like everywhere else it appears */
      if (mod) {
        var mi = modIcon(mod.mod);
        var mw = el('span', 'bay__iconwrap');
        mw.appendChild(imgWithFallback('bay__icon', mi.src, 'icons/' + art(it.slot).icon, ''));
        b.appendChild(mw);
      }

      var mid = el('span', 'bay__mid');
      mid.appendChild(el('span', 'bay__name' + (mod ? '' : ' is-empty'),
        mod ? mod.name : 'Empty ' + typeName(type) + ' slot'));
      var sub = [];
      if (mod) {
        sub.push(Data.tierName(mod.tier));
        Object.keys(mod.stats || {}).forEach(function (k) {
          sub.push(Data.statLabel(k) + ' ' + Data.formatStat(k, mod.stats[k]));
        });
      } else {
        sub.push('Takes any "' + typeName(type) + '" mod');
      }
      mid.appendChild(el('span', 'bay__sub', sub.join('  ·  ')));
      b.appendChild(mid);
      if (isSetup()) {
        b.disabled = true;
        b.title = 'Mods are fitted at a ripperdoc.';
      } else {
        b.appendChild(el('span', 'bay__act', mod ? 'Change' : 'Fit'));
        b.onclick = function () { Snd.play('open'); openModPicker(inst, i, type); };
      }
      bays.appendChild(b);
    });
    panel.appendChild(bays);

    var total = (it.modSlotTypes || []).length;
    if (inst.modSlotTypes.length < total) {
      panel.appendChild(el('div', 'note',
        'This piece has ' + total + ' mod slots in all; ' + inst.modSlotTypes.length +
        ' are open at ' + Data.tierName(inst.tier) + '. Higher tiers open the rest.'));
    }
  }

  /* ── Rolls: this piece's own rolled stats, against their window ── */
  function tabRolls(panel, it, inst) {
    if (!inst) {
      var any = it.tiers.some(function (t) { return t.rolls && t.rolls.length; });
      panel.appendChild(el('div', 'note', any
        ? 'This piece rolls its own extra stats when you buy it, and re-rolls them on every upgrade. Buy it to see what you get.'
        : 'This piece has no rolled stats — what you see is what it gives.'));
      return;
    }
    if (!inst.rolls || !inst.rolls.length) {
      panel.appendChild(el('div', 'note', 'This piece rolled no extra stats at its current tier.'));
      return;
    }
    var tdef = Data.tierDef(it, inst.tier);
    var windows = {};
    ((tdef && tdef.rolls) || []).forEach(function (r) {
      (r.pool || []).forEach(function (p) { windows[p.stat] = { min: p.min, max: p.max }; });
    });

    var wrap = el('div', 'rolls');
    inst.rolls.forEach(function (r) {
      var w = windows[r.stat] || { min: r.value, max: r.value };
      var box = el('div', 'roll');
      var top = el('div', 'roll__top');
      top.appendChild(el('span', 'roll__k', Data.statLabel(r.stat)));
      top.appendChild(el('span', 'roll__v', Data.formatStat(r.stat, r.value)));
      box.appendChild(top);

      var bar = el('div', 'roll__bar');
      var at = el('span', 'roll__at');
      var span = (w.max - w.min) || 1;
      var pct = Math.max(0, Math.min(100, (r.value - w.min) / span * 100));
      at.style.left = 'calc(' + pct + '% - 2px)';
      at.title = 'Rolled ' + r.value + ' in a window of ' + w.min + ' to ' + w.max;
      bar.appendChild(at);
      box.appendChild(bar);

      var ends = el('div', 'roll__ends');
      ends.appendChild(el('span', null, Data.formatStat(r.stat, w.min)));
      ends.appendChild(el('span', null, Data.formatStat(r.stat, w.max)));
      box.appendChild(ends);
      wrap.appendChild(box);
    });
    panel.appendChild(wrap);
    panel.appendChild(el('div', 'note',
      'Rolls are fixed until you upgrade. Upgrading to the next tier re-rolls them, ' +
      'so a good roll is a reason to think twice about an upgrade.'));
  }

  /* ── Looks: appearance variants ── */
  function tabLooks(panel, it, inst) {
    var variants = it.variants || [];
    if (!variants.length) {
      panel.appendChild(el('div', 'note', 'This piece has one appearance.'));
      return;
    }
    var grid = el('div', 'looks');
    variants.forEach(function (v) {
      var on = inst && inst.variant === v.id;
      var b = el('button', 'look' + (on ? ' is-on' : ''));
      b.type = 'button';
      var sw = el('span', 'look__sw');
      sw.style.background = swatchFor(v.id);
      b.appendChild(sw);
      b.appendChild(el('span', 'look__n', v.name));
      var p = el('span', 'look__p' + (v.price ? '' : ' is-free'),
                 on ? 'Fitted' : (v.price ? Data.money(v.price) : 'No charge'));
      b.appendChild(p);
      if (!inst) { b.disabled = true; b.title = 'Buy this piece to change how it looks.'; }
      else if (on) { b.disabled = true; }
      else if (isSetup()) { b.disabled = true; b.title = 'Looks are changed at a ripperdoc.'; }
      else { b.onclick = function () { confirmVariant(inst, v); }; }
      grid.appendChild(b);
    });
    panel.appendChild(grid);
    if (!inst) {
      panel.appendChild(el('div', 'note', 'Appearance can be changed once you own the piece.'));
    } else {
      panel.appendChild(el('div', 'note',
        'Appearance only — variants never change stats. A priced variant is paid every time you switch to it.'));
    }
  }

  /* A readable swatch per variant id. Catalog variants carry no colour, so the
     view owns this mapping; an unknown id gets a neutral chrome. */
  var SWATCH = {
    'default':  'linear-gradient(135deg,#9fb0bd,#55656f)',
    'chrome':   'linear-gradient(135deg,#d6e2ea,#71818d)',
    'red':      'linear-gradient(135deg,#ff5a4e,#7d1f18)',
    'gold':     'linear-gradient(135deg,#f0c550,#7d5f14)',
    'neon':     'linear-gradient(135deg,#00f0ff,#065f68)',
    'black':    'linear-gradient(135deg,#3a444d,#11171c)',
    'matte':    'linear-gradient(135deg,#444c54,#1a1f24)',
    'gunmetal': 'linear-gradient(135deg,#79868f,#333c44)',
    'blood':    'linear-gradient(135deg,#c23b32,#4a1310)'
  };
  function swatchFor(id) { return SWATCH[id] || SWATCH['default']; }

  /* ── "If you fit this" ─────────────────────────────────────────────────
     CP2077's ripperdoc screen never shows a piece on its own: it shows what the
     piece would do to YOU — the capacity it eats, the position it takes, the
     piece it pushes out, your totals afterwards. Ours showed a table and then a
     tall band of empty background under it (MENU-AAA A4/A5). This fills that
     band with the consequences, and shrinks to nothing when the tab above it is
     already long. */
  function renderFit() {
    var box = $('dt-fit');
    if (!box) { return; }
    clear(box); box.hidden = true;
    var it = Data.item(ui.itemId);
    if (!it) { return; }
    var st = Data.state();
    var inst = currentInstance();
    var r = Data.rowFor(it);
    var cost = inst ? inst.capacityCost : r.capacityCost;
    var cap = st.capacity || { used: 0, max: 0 };
    var fitted = !!(inst && inst.installed);
    var tdef = Data.tierDef(it, inst ? inst.tier : r.displayTier);
    var gives = inst ? inst.stats : ((tdef && tdef.stats) || {});

    box.hidden = false;
    box.appendChild(el('h3', 'fit__h', fitted ? 'What it is doing for you' : 'If you fit this'));
    var grid = el('div', 'fit__grid');

    /* capacity: what you carry now, and what you would carry */
    var after = fitted ? cap.used : cap.used + cost;
    var over = after - cap.max;
    var box1 = el('div', 'fit__b');
    box1.appendChild(el('span', 'fit__k', 'Capacity'));
    var line = el('span', 'fit__v');
    if (fitted) {
      line.appendChild(el('span', 'fit__now', cap.used + ' of ' + cap.max + ' used'));
      line.appendChild(el('span', 'fit__sub', 'this piece carries ' + cost));
    } else {
      line.appendChild(el('span', 'fit__now', cap.used + ' of ' + cap.max));
      line.appendChild(el('span', 'fit__arrow' + (over > 0 ? ' is-bad' : ''), '→'));
      line.appendChild(el('span', 'fit__next' + (over > 0 ? ' is-bad' : ''), after + ' of ' + cap.max));
    }
    box1.appendChild(line);
    var bar = el('span', 'fit__bar');
    var pct = function (n) { return cap.max > 0 ? Math.max(0, Math.min(100, n / cap.max * 100)) : 0; };
    var used = el('span', 'fit__fill');
    used.style.width = pct(fitted ? cap.used : Math.min(cap.used, cap.max)) + '%';
    bar.appendChild(used);
    if (!fitted && cost > 0) {
      var add = el('span', 'fit__add' + (over > 0 ? ' is-bad' : ''));
      add.style.width = pct(Math.max(0, Math.min(after, cap.max) - cap.used)) + '%';
      bar.appendChild(add);
    }
    box1.appendChild(bar);
    if (!fitted && over > 0) {
      box1.appendChild(el('span', 'fit__warn', 'Over capacity by ' + over +
        (cap.overCapacityPenalty ? '. Going over costs you ' +
          Data.statLabel(cap.overCapacityPenalty.stat) + ' ' + cap.overCapacityPenalty.value + '.'
         : '. Raise capacity, or take something out first.')));
    }
    grid.appendChild(box1);

    /* where it goes, and what it pushes out */
    var box2 = el('div', 'fit__b');
    box2.appendChild(el('span', 'fit__k', fitted ? 'Fitted' : 'Where it goes'));
    var plan = fitPlan(it.id);
    var sl = plan && plan.slot;
    var whereV = el('span', 'fit__v');
    if (fitted) {
      var isl = Data.slotState(inst.installed.slot);
      whereV.appendChild(el('span', 'fit__now', (isl ? isl.name : inst.installed.slot) +
        (isl && isl.count > 1 ? ' · position ' + (inst.installed.index + 1) : '')));
    } else if (!sl) {
      whereV.appendChild(el('span', 'fit__now', (Data.slotDef(it.slot) || {}).name || it.slot));
    } else if (!plan.full) {
      /* the slot name on its own line: "Integumentary System · position 3 of 3"
         runs past this box at 1440 and reads as clipped */
      whereV.appendChild(el('span', 'fit__now', sl.name));
      whereV.appendChild(el('span', 'fit__sub', sl.count > 1
        ? 'position ' + (plan.free + 1) + ' of ' + sl.count + ', empty'
        : 'that position is empty'));
    } else if (plan.holder) {
      whereV.appendChild(el('span', 'fit__now', sl.name));
      whereV.appendChild(el('span', 'fit__sub is-warn', 'replaces ' + plan.holder.name +
        ' — it goes back to your stash'));
    } else {
      whereV.appendChild(el('span', 'fit__now', sl.name + ' · every position full'));
      whereV.appendChild(el('span', 'fit__sub is-warn', 'you pick which one it swaps out'));
    }
    box2.appendChild(whereV);
    grid.appendChild(box2);
    box.appendChild(grid);

    /* your totals, now and after — only the stats this piece touches */
    var keys = Object.keys(gives || {}).sort(function (a, b) {
      return Data.statLabel(a).localeCompare(Data.statLabel(b));
    });
    if (keys.length && !fitted) {
      var tot = el('div', 'fit__tot');
      tot.appendChild(el('span', 'fit__k', 'Your totals after'));
      var tl = el('div', 'fit__rows');
      keys.slice(0, 6).forEach(function (k) {
        var now = (st.totals && st.totals[k]) || 0;
        var row = el('div', 'fit__r');
        row.appendChild(el('span', 'fit__rk', Data.statLabel(k)));
        row.appendChild(el('span', 'fit__rn', Data.formatStat(k, now)));
        row.appendChild(el('span', 'fit__ra', '→'));
        row.appendChild(el('span', 'fit__rv', Data.formatStat(k, now + gives[k])));
        tl.appendChild(row);
      });
      tot.appendChild(tl);
      box.appendChild(tot);
    } else if (!keys.length && !fitted) {
      box.appendChild(el('p', 'fit__note',
        'No stat lines change: this piece works through its effect, not through numbers.'));
    }
  }

  /* ── the action bar under the detail ── */
  function renderActions() {
    var acts = $('dt-acts');
    clear(acts);
    var it = Data.item(ui.itemId);
    var inst = currentInstance();
    var st = Data.state();
    var r = Data.rowFor(it);

    /* A disabled action says why IN the panel, not only in a tooltip: a couch
       player on a pad never hovers. Labels that already say it ("Not stocked",
       "Max tier") pass quiet=true. */
    var why = $('dt-why'), reasons = [];
    why.hidden = true;
    /* F: the note is rebuilt by every render, so a fitted piece can never keep
       a stale "not fitted yet" line from the piece shown before it. */
    var next = $('dt-next');
    next.hidden = true; next.textContent = '';
    function mk(label, sub, cls, onclick, disabledWhy, quiet) {
      var b = el('button', 'btn' + (cls ? ' ' + cls : ''));
      b.type = 'button';
      b.appendChild(el('span', 'btn__t', label));
      if (sub) { b.appendChild(el('span', 'btn__sub', sub)); }
      if (disabledWhy) {
        b.disabled = true;
        if (quiet) { b.title = disabledWhy; }
        else {
          /* the reason line below says it; a tooltip on top of it would only cover it */
          b.setAttribute('aria-describedby', 'dt-why');
          if (reasons.indexOf(disabledWhy) === -1) { reasons.push(disabledWhy); }
        }
      } else { b.onclick = onclick; }
      acts.appendChild(b);
      if (reasons.length) { why.hidden = false; why.textContent = reasons.join('  '); }
      return b;
    }
    var bill = $('dt-bill');
    clear(bill); bill.hidden = true;
    function upgradeWhy(u) {
      if (u.reason === 'E_LEVEL') { return 'Upgrade needs level ' + u.requiresLevel + ' (you are ' + st.level + ').'; }
      if (u.reason === 'E_COMPONENTS') {
        var b = billNode(u.components, 'Components for ' + Data.tierName(u.tier));
        if (b) { bill.appendChild(b); bill.hidden = false; }
        var short = (u.components || []).filter(function (c) { return (c.have || 0) < c.need; })
          .map(function (c) { return (c.need - (c.have || 0)) + ' more ' + (c.name || c.form); });
        return 'Upgrade needs ' + (short.length ? short.join(' and ') : 'components you do not have yet') + '.';
      }
      if (u.reason === 'E_NO_FUNDS' || st.currency.amount < u.price) {
        return 'Upgrade needs ' + Data.money(Math.max(0, u.price - st.currency.amount)) + ' more ' + st.currency.name + '.';
      }
      return 'Upgrade is not available here.';
    }

    /* your setup: no actions at all, one line that says where they are */
    if (isSetup()) {
      var ro = el('div', 'dt__ro');
      ro.setAttribute('role', 'note');
      var where = !inst ? 'You do not own this piece.'
        : inst.installed ? 'Fitted' + (inst.installed.slot ? ' in ' + ((Data.slotState(inst.installed.slot) || {}).name || 'its slot') : '') + '.'
        : 'In your stash.';
      ro.appendChild(el('span', 'dt__rot', where));
      ro.appendChild(el('span', 'dt__ros', setupNote()));
      acts.appendChild(ro);
      /* the one thing the field allows on a piece: the fitted cyberdeck's quickhack loadout */
      if (inst && inst.installed && hasLoadout() && qh().deck === it.id) {
        mk('Quickhack loadout', loadoutLine(qh()), 'btn--go', function () { Snd.play('open'); openLoadout(); });
      }
      return;
    }

    if (!inst) {
      var row = r.cheapest || r.locked || r.shopRows[0];
      if (!row) {
        /* Not a tooltip: a player on a pad never hovers, and "not stocked" on
           its own is a dead end. Say who does sell it, and where. */
        var docName = (Data.shop() && Data.shop().ripperdoc && Data.shop().ripperdoc.name) || 'This ripperdoc';
        var elsewhere = Data.stockedElsewhereLine(it.id);
        mk('Not stocked', 'this ripperdoc', '', null,
           docName + ' does not stock this. ' + (elsewhere || 'No ripperdoc stocks it yet.'));
      } else {
        var no = null;
        if (row.reason === 'E_NO_FUNDS' || (row.available && st.currency.amount < row.price)) {
          no = 'You need ' + Data.money(row.price - st.currency.amount) + ' more ' + st.currency.name + ' to buy this.';
        } else if (!row.available) {
          no = row.reason === 'E_LEVEL' ? 'Needs level ' + row.requiresLevel + ' (you are ' + st.level + ').' : 'Not available at this ripperdoc.';
        }
        mk('Buy', Data.money(row.price) + ' · ' + Data.tierName(row.tier), 'btn--go',
           function () { confirmBuy(it, row); }, no);
        var bp = fitPlan(it.id);
        var bpNo = no;
        if (!bpNo && bp && bp.full && !bp.holder) {
          bpNo = 'Every ' + bp.slot.name + ' position is full. Buy it, then swap it in from your stash.';
        }
        mk('Buy & install', bp && bp.holder ? 'swaps out ' + bp.holder.name : 'fits it straight in', '',
           function () { confirmBuy(it, row, true); }, bpNo);
      }
      return;
    }

    if (!inst.installed) {
      /* F: say it in words, above the button that does it. The author owned the
         Mantis arms and asked how to use them: the Install button alone was
         not enough to tell him a bought piece is not a fitted piece. */
      var step = fitNextStep(inst.item);
      if (step) { next.textContent = step; next.hidden = false; }
      var ip = fitPlan(inst.item);
      if (ip && ip.full) {
        mk('Swap', ip.holder ? 'replaces ' + ip.holder.name : 'pick a position', 'btn--go',
           function () { confirmInstall(inst); });
      } else {
        mk('Install', inst.capacityCost + ' capacity', 'btn--go', function () { confirmInstall(inst); });
      }
      if (inst.upgrade) {
        mk('Upgrade', inst.upgrade.available
            ? Data.money(inst.upgrade.price) + ' · ' + Data.tierName(inst.upgrade.tier)
            : (inst.upgrade.reason === 'E_COMPONENTS' ? 'needs parts' : 'locked'),
           '', function () { confirmUpgrade(inst); },
           inst.upgrade.available ? null : upgradeWhy(inst.upgrade));
      }
      mk('Sell', inst.sellPrice === null ? 'iconic' : Data.money(inst.sellPrice), 'btn--bad',
         function () { confirmSell(inst); },
         inst.sellPrice === null ? 'Iconic cyberware cannot be sold.' : null, true);
      return;
    }

    mk('Remove', 'back to the stash', 'btn--bad', function () { confirmRemove(inst); });
    if (inst.upgrade) {
      mk('Upgrade', inst.upgrade.available
          ? Data.money(inst.upgrade.price) + ' · ' + Data.tierName(inst.upgrade.tier)
          : (inst.upgrade.reason === 'E_COMPONENTS' ? 'needs parts' : 'locked'),
         'btn--go', function () { confirmUpgrade(inst); },
         inst.upgrade.available ? null : upgradeWhy(inst.upgrade));
    } else {
      mk('Max tier', 'nothing above this', '', null, 'This piece is already at its highest tier.', true);
    }
    if (inst.modSlotTypes && inst.modSlotTypes.length) {
      mk('Mods', inst.mods.filter(Boolean).length + ' of ' + inst.modSlotTypes.length + ' fitted', '',
         function () { setTab('mods'); });
    }
    /* the fitted cyberdeck: which quickhacks fill its slots, and in what order */
    if (hasLoadout() && qh().deck === it.id) {
      mk('Quickhack loadout', loadoutLine(qh()), '', function () { Snd.play('open'); openLoadout(); });
    }
  }

  /* ═══════════════════ flows (every one previews with dryRun) ═══════════════════ */

  /* Run the op as a dry run, build a before -> after ledger, and only send it
     for real when the player confirms. opts: { icon, name } for the popout's
     header and the success band. */
  function preview(title, lede, op, args, confirmLabel, danger, opts) {
    if (ui.busy) { return; }
    if (isSetup()) { toast(setupNote(), true); return; }
    ui.busy = true;
    opts = opts || {};
    Bridge.request(op, args, true).then(function (res) {
      ui.busy = false;
      Snd.play('open');
      openConfirm(title, lede, res, confirmLabel, danger, function () {
        commit(op, args, title, opts);
      }, opts);
    }, function (e) {
      ui.busy = false;
      clearPreview();
      toast(e.message || 'That is not possible right now.');
    });
  }

  var DONE_WORD = {
    buy: 'Purchased', buyMod: 'Mod purchased', install: 'Installed', attachMod: 'Mod fitted',
    remove: 'Removed', detachMod: 'Mod removed', sell: 'Sold', sellMod: 'Mod sold',
    upgrade: 'Upgraded', setVariant: 'New look', buyCyberPerk: 'Rank gained', refundCyberPerk: 'Refunded',
    buyCapacityItem: 'Capacity raised'
  };

  function commit(op, args, what, opts) {
    if (ui.busy) { return; }
    ui.busy = true;
    opts = opts || {};
    closePop(true);
    Bridge.request(op, args, false).then(function (res) {
      ui.busy = false;
      Data.absorb(res);
      if (res.data && res.data.uid) { ui.uid = res.data.uid; }
      var kind = (op === 'buy' && args.install) ? 'install' : Snd.forOp(op);
      Snd.play(kind);
      renderAll();
      flourish(op, args, opts.name || what, res);
      /* the seam for other streams (BETA QA's install scene): every committed
         mutation, after the view has re-rendered. app.js sends ops through
         Bridge.request(op, ...), so wrapping Bridge.install & co. sees nothing. */
      try {
        document.dispatchEvent(new CustomEvent('cw:committed', { detail: { op: op, args: args, res: res, name: opts.name || what } }));
      } catch (e) { /* a listener's error must not break the menu */ }
    }, function (e) {
      ui.busy = false;
      toast(e.message || (what + ' failed.'));
      renderAll();
    });
  }

  /* The confirmation flourish: a band names what happened over the pane it
     happened in, the slot it touched pings on the map, the capacity bar
     flashes, and the header numbers count to their new values (renderHeader). */
  function flourish(op, args, name, res) {
    var word = (op === 'buy' && args.install) ? 'Bought & installed' : (DONE_WORD[op] || 'Done');
    var host = null;
    if (op === 'buyCyberPerk' || op === 'refundCyberPerk') {
      host = document.querySelector('.perk[data-perk="' + args.perk + '"]');
      if (host) { Fx.replay(host, 'fx-pulse', 900); }
    }
    if (!host) {
      var panes = [$('pane-detail'), $('pane-list'), $('pane-body')];
      for (var i = 0; i < panes.length && !host; i++) { if (panes[i].offsetParent !== null) { host = panes[i]; } }
    }
    /* in the detail the band sits just above the action bar, where the player
       just pressed, and never over the piece's name (cycle 1 shot) */
    if (host && host.id === 'pane-detail' && $('dt-acts').offsetParent !== null) { host = $('dt-acts'); }
    /* a cyberdeck just went in: say where its quickhacks are set (the action is on the card under this band) */
    var fitted = (op === 'install' || (op === 'buy' && args.install)) && hasLoadout() && res && res.state &&
                 res.state.quickhacks && (function () {
                   var u = (res.data && res.data.uid) || args.uid, i = u && res.state.instances[u];
                   return !!(i && i.item === res.state.quickhacks.deck);
                 })();
    Fx.banner(host || $('cw'), word, name);
    /* A deck just went in. The install scene (scene.js) fades the menu to black on this same commit, so a band
       shown now would play under it and be gone when the player can see again (QD on MN-30). Wait for the scene
       to end, then say where the deck's quickhacks are set, over the card that now carries the action, for long
       enough to read. With the scene switched off it follows the "Installed" band. */
    if (fitted) {
      var tries = 0;
      var say = function () {
        var sc = $('scene');
        if (sc && !sc.hidden && tries++ < 150) { global.setTimeout(say, 200); return; }
        var at = $('dt-acts').offsetParent !== null ? $('dt-acts') : (host || $('cw'));
        Fx.banner(at, 'Deck fitted', 'Set your quickhack loadout: Q', 5000);
      };
      global.setTimeout(say, 1800);
    }
    if (!Fx.reduced() && host && host.id === 'pane-detail') { Fx.replay($('detail'), 'fx-scan', 900); }

    /* ── F: the commit ends on the NEXT step, not on "Purchased" alone ──
       A buy that did not fit the piece leaves the sentence in the detail's own
       note (#dt-next, renderActions) and flashes it once, so the eye goes to
       the words sitting right above the Install button. It is deliberately NOT
       a band over the action bar: `.dt__acts .fxband` covers the buttons it
       sits on and takes pointer events, so a six-second band there would hide
       the very control the words tell him to press. The hint checks' H4 row
       caught exactly that in the first attempt.
       An arm that just went in has no "next step" left to live in, so that one
       IS a band - hosted on the detail pane, where a band is top-anchored and
       `pointer-events: none`, never on the action bar. */
    var paneHost = ($('pane-detail') && $('pane-detail').offsetParent !== null) ? $('pane-detail') : (host || $('cw'));
    var sayLater = function (word, sub, ms) {
      /* scene.js fades the menu to black on this same commit, so a band shown
         now would play under it and be gone by the time he can see again (QD
         on MN-30). Wait for the scene to end, as the deck line above does. */
      var tries = 0;
      var say = function () {
        var sc = $('scene');
        if (sc && !sc.hidden && tries++ < 150) { global.setTimeout(say, 200); return; }
        Fx.banner(paneHost, word, sub, ms);
      };
      global.setTimeout(say, 1800);
    };
    if (op === 'buy' && !args.install) {
      /* `hidden` is not the question - whether he can SEE it is. On the phone
         layout one pane is on screen at a time, so the detail's note can be
         un-hidden and still off screen, and a pulse there tells him nothing.
         The hint checks' H2 row at 390x844 caught exactly that: the buy ended
         on "Purchased" and no next step. offsetParent is null for a node whose
         pane is display:none, which is how that layout hides a pane. */
      var nx = $('dt-next');
      if (nx && !nx.hidden && nx.offsetParent !== null) { Fx.replay(nx, 'fx-pulse', 1200); }
      else {
        var bought = fitNextStep(args.item);
        if (bought) { sayLater('Bought, not fitted', bought, 6000); }
      }
    } else if (op === 'install' || (op === 'buy' && args.install)) {
      var fitU = (res && res.data && res.data.uid) || args.uid;
      var fitI = fitU && Data.state().instances[fitU];
      var armLine = armDrawLine(args.item || (fitI && fitI.item));
      if (armLine) { sayLater('Arm fitted', armLine, 5500); }
    }

    var st = Data.state(), slot = null;
    var uid = (res && res.data && res.data.uid) || args.uid;
    if (uid && st.instances[uid]) {
      var inst = st.instances[uid];
      slot = inst.installed ? inst.installed.slot : (Data.item(inst.item) || {}).slot;
    } else if (args.item) { slot = (Data.item(args.item) || {}).slot; }
    if (slot) {
      Fx.replay(document.querySelector('.node[data-slot="' + slot + '"]'), 'fx-pulse', 900);
      Fx.replay(document.querySelector('.pin[data-slot="' + slot + '"]'), 'fx-ping', 900);
    }
    if (res && res.state && /install|remove|upgrade|buy|attachMod|detachMod|CyberPerk/.test(op)) {
      Fx.replay($('cap-track'), 'fx-flash', 700);
    }
  }

  function defIcon(itemId) {
    var it = Data.item(itemId);
    return it ? iconFor(it) : null;
  }

  /* ═══════════════ F: what is left to do with a piece you own ═══════════
     The author, 2026-11-12, having bought the Mantis arms on the installed beta:
     "i own them now but how to use them??" and "not in my inventory?". A piece
     bought here goes into the ripperdoc's stash and does nothing at all until
     it is FITTED, and the menu never said so: the buy ended on a "Purchased"
     band and the only thing that would have told him was an Install button he
     had no reason to look at. It is not in the Pip-Boy either, which is what
     sent him looking there.
     fitNextStep() is the one sentence that says what is left, for the state
     the piece is actually in. It returns '' when there is nothing to do, so
     nothing is ever shown for a fitted piece. The slot, the position and the
     numbers come from state - never a generic "equip it from your inventory",
     which is the wrong instruction here. */
  function fitNextStep(itemId) {
    var def = Data.item(itemId);
    if (!def) { return ''; }
    var st = Data.state();
    var plan = fitPlan(itemId);
    if (!plan) { return ''; }
    var slotName = (plan.slot && plan.slot.name) || 'its slot';
    var r = Data.rowFor(def);        /* the OBJECT: rowFor reads it.tiers, and a bare id throws */
    var cost = r && r.capacityCost;
    /* a gate the player cannot pass yet is the next step, and it comes first */
    var locked = r && !r.cheapest && r.locked;
    if (locked && locked.reason === 'E_LEVEL' && locked.requiresLevel) {
      return 'It stays in your stash until level ' + locked.requiresLevel + ' (you are ' + st.level + ').';
    }
    if (cost && st.capacity && (st.capacity.used + cost) > st.capacity.max) {
      return 'It needs ' + cost + ' capacity and you have ' +
             Math.max(0, st.capacity.max - st.capacity.used) + ' of ' + st.capacity.max +
             ' free: take something out before it will fit.';
    }
    if (plan.full && plan.holder) {
      return 'Every ' + slotName + ' position is taken. Swap puts it in and ' + plan.holder.name +
             ' goes back to your stash.';
    }
    if (plan.full) {
      return 'Every ' + slotName + ' position is taken: pick the one to swap out.';
    }
    return 'It is in your stash, not fitted yet. Install puts it in ' + slotName +
           (plan.slot && plan.slot.count > 1 ? ' position ' + (plan.free + 1) : '') + '.';
  }

  /* The line after an ARM is fitted. On this build the install equips and
     draws the arm itself - no input from the player - so the hint says it is
     already out and names the vanilla route for a LATER draw. There is NO
     plugin keybind for drawing an arm and this line must never invent one.
     Source: the ARMS builder (2026-11-12) reading the author's
     own beta log (Cyberware.log, evidence set betastuck-20261112):
       arms: ar_mantis holds CW_MantisBlades
       arms: ar_mantis equipped on install
       arms: weaponDraw -> visual on          (343 ms later, no input between)
     docs/USE-CASES.md row 1 says the opposite ("no auto-equip, no key, no
     hint"); that row was measured 2026-11-08 on an older build and the same
     builder calls it STALE for E. The second sentence (the manual route) is
     NOT proven in game - it is test E2, and no weaponSheathe event appears in
     any of the five older logs - so it is worded as what to do, not as a
     promise about what will happen. */
  function armDrawLine(itemId) {
    var def = Data.item(itemId);
    if (!def || def.slot !== 'arms') { return ''; }
    return 'It is equipped and drawn. Later: equip it from your Pip-Boy or a favourite, then ready your weapon as normal.';
  }

  function confirmBuy(it, shopRow, alsoInstall) {
    var args = { item: it.id, tier: shopRow.tier };
    var swapNote = '';
    if (alsoInstall) {
      args.install = true;
      var plan = fitPlan(it.id);
      if (plan && plan.holder) {
        args.replace = true; args.index = 0;
        swapNote = ' It replaces ' + plan.holder.name + ', which goes back to your stash.';
      }
    }
    /* the sentence for the tier being bought, not the reference one */
    var buyDesc = Data.tierDesc(it, shopRow.tier);
    /* F: a plain Buy does NOT fit the piece, and the confirmation is the first
       place that can say so - before the money is spent, not after. */
    var nextStep = alsoInstall ? '' : fitNextStep(it.id);
    preview(alsoInstall ? 'Buy and install' : 'Buy ' + it.name,
      it.name + ' at ' + Data.tierName(shopRow.tier) + ', from ' +
      ((Data.shop().ripperdoc && Data.shop().ripperdoc.name) || 'this ripperdoc') + '.' +
      (buyDesc ? ' ' + buyDesc : '') + swapNote +
      (nextStep ? ' ' + nextStep : ''),
      'buy', args, alsoInstall ? 'Buy & install' : 'Buy', false, { icon: iconFor(it), name: it.name });
  }

  /* Where a piece would go: the first free position of its slot. A full
     single-position slot names the piece a swap would replace; a full slot
     with several positions has no default -- the positions row is where the
     player picks (cw-int-2: INSTALL on a stash piece into a full slot ended in
     the engine's raw "every Face (Ocular) position is taken"). */
  function fitPlan(itemId) {
    var def = Data.item(itemId);
    var slot = def ? Data.slotState(def.slot) : null;
    if (!slot) { return null; }
    var free = -1;
    for (var i = 0; i < slot.count; i++) { if (!slot.installed[i]) { free = i; break; } }
    var holder = (free < 0 && slot.count === 1 && slot.installed[0]) ? Data.state().instances[slot.installed[0]] : null;
    return { slot: slot, free: free, holder: holder || null, full: free < 0 };
  }

  function capDelta(n) {
    return n > 0 ? 'Uses ' + n + ' more capacity.' : n < 0 ? 'Frees ' + (-n) + ' capacity.' : 'Same capacity.';
  }

  function confirmSwap(piece, idx, out, slot) {
    preview('Swap in ' + piece.name,
      'Replaces ' + out.name + (slot.count > 1 ? ' in ' + slot.name + ' position ' + (idx + 1) : '') +
      '. It goes back to your stash with its mods and rolls. ' +
      capDelta(piece.capacityCost - out.capacityCost),
      'install', { uid: piece.uid, index: idx, replace: true }, 'Swap', false,
      { icon: defIcon(piece.item), name: piece.name });
  }

  /* A full slot with several positions: show its positions with the swap
     offers lit and put focus on the first one. */
  function pickPosition(inst, plan) {
    if (ui.globalQuery) { setSearch('find-all', ''); }
    ui.slot = plan.slot.id; ui.itemId = inst.item; ui.uid = inst.uid;
    if (isNarrow()) { ui.pane = 'list'; }
    renderAll();
    var cell = $('pos-cells').querySelector('.poscell.is-swap');
    if (cell) { focusSoon(cell); if (cell.scrollIntoView) { cell.scrollIntoView({ block: 'nearest' }); } }
    toast('Every ' + plan.slot.name + ' position is full. Pick the one to swap out.', true);
  }

  function confirmInstall(inst) {
    var plan = fitPlan(inst.item);
    if (plan && plan.full) {
      if (plan.holder) { confirmSwap(inst, 0, plan.holder, plan.slot); } else { pickPosition(inst, plan); }
      return;
    }
    preview('Install ' + inst.name,
      'Takes ' + inst.capacityCost + ' of your cyberware capacity.',
      'install', { uid: inst.uid }, 'Install', false, { icon: defIcon(inst.item), name: inst.name });
  }

  function confirmRemove(inst) {
    preview('Remove ' + inst.name,
      'It goes back to your stash and keeps its mods and its rolls. Nothing is lost.',
      'remove', { uid: inst.uid }, 'Remove', true, { icon: defIcon(inst.item), name: inst.name });
  }

  function confirmUpgrade(inst) {
    /* What the player is buying is the NEXT tier's sentence, so the confirm
       states it, and names the one being left behind when the two differ. */
    var udef = Data.item(inst.item);
    var uNow = udef ? Data.tierDesc(udef, inst.tier) : '';
    var uNext = udef ? Data.tierDesc(udef, inst.upgrade.tier) : '';
    var uSays = !uNext ? ''
      : (uNow && uNow !== uNext) ? ' Now: ' + uNow + ' After: ' + uNext
      : ' ' + uNext;
    preview('Upgrade ' + inst.name,
      'To ' + Data.tierName(inst.upgrade.tier) + '.' + uSays + ' This RE-ROLLS the piece\u2019s own rolled ' +
      'stats, and its mod slots change with the tier.',
      'upgrade', { uid: inst.uid }, 'Upgrade', false,
      { icon: defIcon(inst.item), name: inst.name, bill: inst.upgrade.components });
  }

  function confirmSell(inst) {
    preview('Sell ' + inst.name,
      'You get ' + Data.money(inst.sellPrice) + '. Its rolled stats are gone for good; any mods fitted ' +
      'to it drop into your mod stash.',
      'sell', { uid: inst.uid }, 'Sell', true, { icon: defIcon(inst.item), name: inst.name });
  }

  function confirmVariant(inst, v) {
    preview('Fit ' + v.name,
      v.price ? 'Appearance only. ' + Data.money(v.price) + ', charged every time you switch to it.'
              : 'Appearance only \u2014 no charge, no stat change.',
      'setVariant', { uid: inst.uid, variant: v.id }, 'Apply', false, { icon: defIcon(inst.item), name: inst.name + ' \u00b7 ' + v.name });
  }

  function modIcon(modId, type) {
    var m = Data.mod(modId);
    return { src: (m && Data.iconPath(m)) || '', fallback: 'icons/' + art(null).icon, type: type };
  }

  function confirmAttach(inst, modSlot, modUid, modName) {
    var m = Data.state().mods[modUid];
    preview('Fit ' + modName,
      'Into mod slot ' + (modSlot + 1) + ' of ' + inst.name + '.',
      'attachMod', { uid: inst.uid, modSlot: modSlot, modUid: modUid, replace: true }, 'Fit', false,
      { icon: m ? modIcon(m.mod) : null, name: modName });
  }

  function confirmBuyMod(inst, modSlot, modDef, shopRow) {
    preview('Buy ' + modDef.name,
      Data.tierName(shopRow.tier) + ', fitted straight into slot ' + (modSlot + 1) + '.',
      'buyMod', { mod: modDef.id, tier: shopRow.tier, attachTo: inst.uid, modSlot: modSlot }, 'Buy & fit', false,
      { icon: modIcon(modDef.id), name: modDef.name });
  }

  function confirmDetach(inst, modSlot, modName) {
    var uid = inst.mods[modSlot], m = uid && Data.state().mods[uid];
    preview('Remove ' + modName,
      'It goes to your mod stash and can be fitted again later.',
      'detachMod', { uid: inst.uid, modSlot: modSlot }, 'Remove', true,
      { icon: m ? modIcon(m.mod) : null, name: modName });
  }

  function confirmPerk(perk, buy) {
    preview((buy ? 'Buy ' : 'Refund ') + perk.name,
      buy ? 'Spends ' + perk.cost + ' perk point' + (perk.cost === 1 ? '' : 's') + '.'
          : 'Returns ' + perk.cost + ' perk point' + (perk.cost === 1 ? '' : 's') +
            '. Refused if it would leave you over capacity or unseat a fitted piece.',
      buy ? 'buyCyberPerk' : 'refundCyberPerk', { perk: perk.id },
      buy ? 'Buy rank' : 'Refund rank', !buy, { name: perk.name });
  }

  /* ═══════════════════ popout ═══════════════════ */
  var popOnConfirm = null;
  var popOnEscape = null;       // a popout whose Esc does more than close it (the conversation: Esc walks away)
  var popRestoreFocus = null;

  function openPop(title) {
    if ($('scrim').hidden) { popRestoreFocus = document.activeElement; }
    $('pop').classList.remove('pop--greet', 'pop--pick', 'pop--talk', 'pop--loadout');
    $('scrim').classList.remove('is-talk');
    document.documentElement.classList.remove('cw-talking');
    Fx.hideTip();
    $('pop-title').textContent = title;
    $('scrim').hidden = false;
    Fx.replay($('scrim'), 'fx-scrim', 300);
    Fx.replay($('pop'), 'fx-pop', 320);
    clear($('pop-body'));
    clear($('pop-foot'));
  }

  /* silent: the popout closes because its action was confirmed, and that
     action's own sound is about to play */
  function closePop(silent) {
    if ($('scrim').hidden) { return; }
    if (silent !== true) { Snd.play('back'); }
    Fx.hideTip();
    $('scrim').hidden = true;
    $('scrim').classList.remove('is-talk');
    document.documentElement.classList.remove('cw-talking');
    $('pop').classList.remove('pop--greet', 'pop--pick', 'pop--talk', 'pop--loadout');
    popOnConfirm = null;
    popOnEscape = null;
    clearPreview();
    if (popRestoreFocus && popRestoreFocus.focus) {
      try { popRestoreFocus.focus(); } catch (e) { /* gone from the DOM */ }
    }
    popRestoreFocus = null;
  }

  /* The conversation: talking to a placed ripperdoc opens the menu with via:"talk". The doc speaks first
     (CW.Talk picks the line from who is in the chair), the player can ask about things, then either
     "Let's see your chrome" (drops into the menu) or walks away. Written dialogue only: see docs/RIPPERDOC-LORE.md. */
  function openGreeting(ev) {
    var sd = Data.shop() && Data.shop().ripperdoc;
    var def = sd && Data.ripperdoc(sd.id);
    var talk = def && def.talk;
    var player = (ev && ev.player) || {};
    var st = Data.state();
    var first = talk && CW.Talk.opener(talk, st, player);
    if (!first) { return false; }
    openPop((def && def.name) || sd.name);
    $('pop').classList.add('pop--greet');
    /* talk mode: the game keeps running behind a conversation at the bottom of the screen ([Menu] bTalkUnpaused, DLL open event) */
    var talkMode = !!(ev && ev.talkMode);
    $('pop').classList.toggle('pop--talk', talkMode);
    $('scrim').classList.toggle('is-talk', talkMode);
    document.documentElement.classList.toggle('cw-talking', talkMode);   // the menu hides, the game shows through
    var body = $('pop-body');
    if (talk.where) { body.appendChild(el('div', 'greet__where', 'Ripperdoc  \u00b7  ' + talk.where)); }
    var line = el('p', 'greet__line', first);
    line.setAttribute('aria-live', 'polite');
    body.appendChild(line);

    /* voice: only in talk mode (a paused game can't play it); the line simply stays text if there is no clip */
    function say(text) {
      if (!talkMode || !CW.Bridge || !CW.Bridge.request) { return; }
      try { CW.Bridge.request('hush', {}, false); CW.Bridge.request('say', { doc: sd.id, key: CW.Talk.voiceKey(text) }, false); } catch (e) { /* text only */ }
    }
    function hush() { if (talkMode && CW.Bridge && CW.Bridge.request) { try { CW.Bridge.request('hush', {}, false); } catch (e) { /* nothing playing */ } } }
    say(first);

    var go = function () { hush(); closePop(true); Snd.play('select'); };
    var leave = function () { hush(); closePop(true); closeMenu(); };
    var all = CW.Talk.topics(talk, st, player);
    var said = {};            // per topic: how many of its lines the doc has used in this conversation
    var input = null;
    if (all.length) {
      var search = el('label', 'srch srch--talk');
      var g = el('span', 'srch__glyph');
      g.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke-width="2" aria-hidden="true"><circle cx="11" cy="11" r="7"></circle><path d="m20 20-3.5-3.5"></path></svg>';
      search.appendChild(g);
      input = el('input', 'srch__input');
      input.type = 'search';
      input.placeholder = 'Ask about\u2026';
      input.setAttribute('aria-label', 'Filter what you can ask');
      input.autocomplete = 'off';
      input.spellcheck = false;
      search.appendChild(input);
      var x = el('button', 'srch__clear', '\u00d7');
      x.type = 'button';
      x.setAttribute('aria-label', 'Clear filter');
      x.hidden = true;
      search.appendChild(x);
      /* how many things there are to ask, inside the box so it costs no height:
         the ask list is capped in vh and scrolls, and a list that scrolls has to
         say there is more (MENU-AAA A11) */
      var count = el('span', 'srch__count');
      count.setAttribute('aria-live', 'polite');
      search.appendChild(count);
      body.appendChild(search);
      var list = el('div', 'pick talk__list');
      list.setAttribute('role', 'listbox');
      list.setAttribute('aria-label', 'What you can ask');
      body.appendChild(list);

      var ask = function (t) {
        var n = said[t.id] || 0;
        line.textContent = t.lines[n % t.lines.length];
        say(line.textContent);
        if (talkMode && CW.Bridge && CW.Bridge.request) { try { CW.Bridge.request('talkGesture', {}, false); } catch (e) { /* she just stands */ } }
        said[t.id] = n + 1;
        Fx.replay(line, 'fx-talk', 320);
        Snd.play('select');
        draw();
      };
      /* MENU-AAA A11, from the Quality Director's DC3a / DG4 on boot 1: the ask
         list is capped in vh, so it ended mid-row and left a 19px stub of an
         empty third line between the last option and the footer - A1 failing in
         the talk panel. End it on whole lines, exactly as snapRows() does for the
         slot list. In talk mode the rows wrap two to a line and in the paused
         panel they stack, so the pitch is measured from the first row that starts
         a NEW line, never from the next sibling. */
      var snapTalkList = function () {
        list.style.maxHeight = '';
        var rows = list.querySelectorAll('.talk__row');
        if (!rows.length) { return; }
        var avail = list.clientHeight;
        if (!avail) { return; }
        /* The lines, with the DEEPEST bottom on each. A pitch would do for the
           paused panel, where the rows stack and are the same height, but in
           talk mode they wrap two to a line and a line whose text wraps is
           taller than its neighbour, so the cap has to be a real bottom edge
           and never a multiple of one row's pitch. */
        var base = rows[0].offsetTop, lines = [], i, j, k;
        for (i = 0; i < rows.length; i++) {
          var top = rows[i].offsetTop - base, bottom = top + rows[i].offsetHeight;
          k = -1;
          for (j = 0; j < lines.length; j++) { if (Math.abs(lines[j].top - top) < 2) { k = j; break; } }
          if (k < 0) { lines.push({ top: top, bottom: bottom }); }
          else if (bottom > lines[k].bottom) { lines[k].bottom = bottom; }
        }
        lines.sort(function (x, y) { return x.top - y.top; });
        /* the last line that fits whole - and never less than ONE whole line,
           so a short viewport shows one entire option and scrolls rather than
           slicing the first one (the 390x844 case) */
        var cap = lines[0].bottom;
        for (i = 0; i < lines.length; i++) { if (lines[i].bottom <= avail + 0.5) { cap = lines[i].bottom; } }
        if (cap > 0 && Math.abs(cap - avail) > 0.5) { list.style.maxHeight = cap + 'px'; }
      };
      var draw = function () {
        clear(list);
        var q = input.value.trim(), toks = tokens(q), n = 0;
        all.forEach(function (t) {
          var hay = (t.ask + ' ' + t.lines.join(' ')).toLowerCase();
          if (!toks.every(function (w) { return hay.indexOf(w) !== -1; })) { return; }
          n++;
          var b = el('button', 'pickrow talk__row' + (said[t.id] ? ' is-asked' : ''));
          b.type = 'button';
          b.setAttribute('role', 'option');
          var label = el('span', 'talk__ask');
          label.appendChild(hl(t.ask, toks));
          b.appendChild(label);
          b.onclick = function () { ask(t); };
          list.appendChild(b);
        });
        if (!n) { list.appendChild(el('div', 'rows__none', 'Nothing about that. Try another word.')); }
        count.textContent = q ? n + ' of ' + all.length : all.length + ' to ask';
        snapTalkList();
      };
      var setQ = function (v) { input.value = v; x.hidden = !v; draw(); };
      var lastLen = 0;
      input.oninput = function () {
        if (input.value.length > lastLen) { Snd.play('type'); }
        lastLen = input.value.length;
        x.hidden = !input.value;
        draw();
      };
      x.onclick = function () { Snd.play('clear'); setQ(''); input.focus(); };
      /* keys come through onKey: with text in the box Enter takes the top hit and Esc clears it; with an empty box
         Enter is "Let's see your chrome" and Esc walks away, the same as the two buttons say */
      input.__cwKey = function (k) {
        if (k === 'Enter') {
          if (!input.value) { go(); return true; }
          var top = list.querySelector('.talk__row');
          if (top) { top.click(); input.select(); } else { Snd.play('error'); }
          return true;
        }
        if (k === 'Escape' && input.value) { Snd.play('clear'); setQ(''); return true; }
        return false;
      };
      draw();
    }

    var foot = $('pop-foot');
    var no = el('button', 'btn');
    no.type = 'button';
    no.appendChild(el('span', null, talk.no || 'Just looking.'));
    no.appendChild(el('span', 'btn__sub', 'Esc'));
    no.onclick = leave;
    var yes = el('button', 'btn btn--go');
    yes.type = 'button';
    yes.appendChild(el('span', null, talk.yes || "Let's see your chrome."));
    yes.appendChild(el('span', 'btn__sub', 'Enter'));
    yes.onclick = go;
    foot.appendChild(no);
    foot.appendChild(yes);
    popOnConfirm = go;
    popOnEscape = leave;
    focusSoon(input || yes);
    return true;
  }

  /* VARGA'S PROMPT (Builder 3, 2026-11-13; docs/VARGA-TALK.md "What ships tonight"). The esp has no greeting topic yet, so
     activating a ripperdoc starts no conversation; the DLL shows this instead (cwOnEvent {type:"talkPrompt", doc, state,
     player}). Her greeting comes from the catalog's ripperdocs[].talk for THAT doc (CW.Talk.opener), then two answers.
     The menu stays hidden behind it (cw-talking): nothing of the shop is drawn while the prompt is up.
     Keys are the popout idiom (onKey): Up/Down (and Left/Right) move between the two, Enter presses the focused one,
     Esc is "Not right now.". Focus is seated on the first answer, so the arrows work from the first frame.
     After an answer the prompt stays drawn until the DLL acts (hides the view, or opens the shop, whose 'open' closes it),
     so the shop never shows through for a frame. */
  function openPrompt(ev) {
    Data.absorb(ev);
    var doc = ev && ev.doc;
    var def = doc ? Data.ripperdoc(doc) : null;
    var talk = def && def.talk;
    var first = talk ? CW.Talk.opener(talk, Data.state(), (ev && ev.player) || {}) : null;
    openPop((def && def.name) || 'Ripperdoc');
    $('pop').classList.add('pop--greet', 'pop--talk', 'pop--prompt');
    $('scrim').classList.add('is-talk');
    document.documentElement.classList.add('cw-talking');
    var body = $('pop-body');
    if (talk && talk.where) { body.appendChild(el('div', 'greet__where', 'Ripperdoc  ·  ' + talk.where)); }
    if (first) {
      var line = el('p', 'greet__line', first);
      line.setAttribute('aria-live', 'polite');
      body.appendChild(line);
      Fx.replay(line, 'fx-talk', 320);
    }
    var answered = false;
    var yesBtn = null;
    var foot = $('pop-foot');
    /* DECLINE IS ALWAYS SENDABLE, SHOP IS ONCE (the softlock of 2026-11-13). The old latch disabled BOTH answers
       after the first one and refused to send again - so when the DLL ignored that answer and left this panel up
       with no state behind it, the player's Esc and his mouse were both dead and only the test seam freed him.
       The DLL's decline is idempotent (it hides the panel whatever its state says), so a second Esc is always the
       way out; only 'shop' stays once, so a double Enter can never open two menus. */
    function answer(choice) {
      if (answered && choice !== 'decline') { return; }
      answered = true;
      Snd.play(choice === 'shop' ? 'select' : 'back');
      if (yesBtn) { yesBtn.disabled = true; }
      var p = Bridge.request('talkPrompt', { choice: choice }, false);
      if (p && p.then) { p.then(null, function () { /* the DLL acts on the answer; a late reply changes nothing */ }); }
    }
    function choice(label, hint, cls, pick) {
      var b = el('button', 'btn prompt__choice' + (cls ? ' ' + cls : ''));
      b.type = 'button';
      b.appendChild(el('span', 'prompt__label', label));
      b.appendChild(el('span', 'btn__sub', hint));
      b.onclick = function () { answer(pick); };
      foot.appendChild(b);
      return b;
    }
    var yes = choice('Let me see your cyberware.', 'Enter', 'is-first', 'shop');
    yesBtn = yes;                 /* answer() disables only this one; the decline stays live as the way out */
    choice('Not right now.', 'Esc', '', 'decline');
    popOnConfirm = function () { answer('shop'); };
    popOnEscape = function () { answer('decline'); };
    focusSoon(yes);
    return true;
  }

  /* The ledger: compare the dry-run state with the one we are showing now. */
  function openConfirm(title, lede, res, confirmLabel, danger, onYes, opts) {
    var now = Data.state();
    var next = res.state || now;
    opts = opts || {};
    openPop(title);
    var body = $('pop-body');
    if (opts.icon) {
      var hero = el('div', 'pop__hero');
      var iw = el('span', 'pop__heroicon');
      iw.appendChild(imgWithFallback('pop__heroimg', opts.icon.src, opts.icon.fallback, ''));
      hero.appendChild(iw);
      hero.appendChild(el('p', 'pop__lede', lede));
      body.appendChild(hero);
    } else {
      body.appendChild(el('p', 'pop__lede', lede));
    }

    var rows = [];
    if (next.currency.amount !== now.currency.amount) {
      rows.push([now.currency.name, Data.money(now.currency.amount), Data.money(next.currency.amount),
                 next.currency.amount > now.currency.amount ? 'is-up' : 'is-down']);
    }
    if (next.capacity.used !== now.capacity.used || next.capacity.max !== now.capacity.max) {
      rows.push(['Capacity used',
                 now.capacity.used + ' / ' + now.capacity.max,
                 next.capacity.used + ' / ' + next.capacity.max,
                 next.capacity.used > next.capacity.max ? 'is-down'
                   : (next.capacity.used < now.capacity.used ? 'is-up' : '')]);
      showPreview(next.capacity.used);
    }
    if (next.perkPoints.available !== now.perkPoints.available) {
      rows.push(['Perk points', now.perkPoints.available, next.perkPoints.available,
                 next.perkPoints.available > now.perkPoints.available ? 'is-up' : 'is-down']);
    }

    var keys = {};
    Object.keys(now.totals || {}).forEach(function (k) { keys[k] = 1; });
    Object.keys(next.totals || {}).forEach(function (k) { keys[k] = 1; });
    Object.keys(keys).sort(function (a, b) {
      return Data.statLabel(a).localeCompare(Data.statLabel(b));
    }).forEach(function (k) {
      var a = (now.totals || {})[k] || 0, b = (next.totals || {})[k] || 0;
      if (Math.abs(a - b) < 0.005) { return; }
      rows.push([Data.statLabel(k), Data.formatStat(k, a), Data.formatStat(k, b), b > a ? 'is-up' : 'is-down']);
    });

    if (rows.length) {
      var ledger = el('div', 'ledger');
      var head = el('div', 'led');
      head.appendChild(el('span', 'led__k', 'What changes'));
      head.appendChild(el('span', 'led__a', 'Now'));
      head.appendChild(el('span', 'led__arrow', ''));
      head.appendChild(el('span', 'led__b', 'After'));
      head.style.background = '#0a1017';
      ledger.appendChild(head);
      rows.forEach(function (r) {
        var d = el('div', 'led');
        d.appendChild(el('span', 'led__k', r[0]));
        d.appendChild(el('span', 'led__a', r[1]));
        d.appendChild(el('span', 'led__arrow', '→'));
        d.appendChild(el('span', 'led__b ' + (r[3] || ''), r[2]));
        ledger.appendChild(d);
      });
      body.appendChild(ledger);
    } else {
      body.appendChild(el('div', 'note', 'Nothing else about your build changes.'));
    }

    if (next.capacity.used > next.capacity.max) {
      body.appendChild(el('div', 'note note--bad',
        'This would put you over your cyberware capacity. Remove something first' +
        /* the over-capacity perk, by the catalog's own name for it (id `edgerunner`), never a name kept here */
        (Data.perk('edgerunner') ? ', or take the ' + Data.perk('edgerunner').name + ' perk.' : '.')));
    }
    /* an upgrade that eats components lists them, with what you hold */
    var cb = billNode(opts.bill, 'Uses from your inventory');
    if (cb) { body.appendChild(cb); }

    /* an expensive step says so before the player commits to it */
    var spend = now.currency.amount - next.currency.amount;
    if (spend > 0 && spend >= now.currency.amount * 0.5) {
      body.appendChild(el('div', 'note note--warn',
        'This spends ' + Math.round(spend / now.currency.amount * 100) + '% of your ' + now.currency.name +
        ' \u2014 ' + Data.money(next.currency.amount) + ' left afterwards.'));
    }

    var foot = $('pop-foot');
    var yes = el('button', 'btn ' + (danger ? 'btn--bad' : 'btn--go'));
    yes.type = 'button';
    yes.appendChild(el('span', null, confirmLabel));
    yes.onclick = onYes;
    var no = el('button', 'btn');
    no.type = 'button';
    no.appendChild(el('span', null, 'Cancel'));
    no.appendChild(el('span', 'btn__sub', 'Esc'));
    no.onclick = closePop;
    foot.appendChild(no);
    foot.appendChild(yes);
    popOnConfirm = onYes;
    /* destructive: the safe answer has focus, so a stray Enter / A press
       cancels instead of selling or removing */
    focusSoon(danger ? no : yes);
  }

  /* ── mod picker: a searchable list of what you own, then what is for sale ──
     Type to filter, \u2193 into the list, \u2191 \u2193 move, Enter takes the top
     hit, Esc clears the box and then closes. */
  function openModPicker(inst, modSlot, type) {
    openPop('Mod slot ' + (modSlot + 1) + ' \u00b7 ' + typeName(type));
    $('pop').classList.add('pop--pick');
    var body = $('pop-body');
    var fitted = inst.mods[modSlot];
    var st = Data.state();

    body.appendChild(el('p', 'pop__lede',
      'This slot takes any \u201c' + typeName(type) + '\u201d mod. Mods you own are listed first; ' +
      'below them is what this ripperdoc sells.'));

    var search = el('label', 'srch');
    var g = el('span', 'srch__glyph');
    g.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="10.5" cy="10.5" r="6.5"/><path d="M15.5 15.5 L21 21"/></svg>';
    search.appendChild(g);
    var input = el('input', 'srch__input');
    input.type = 'search';
    input.placeholder = 'Filter ' + typeName(type) + ' mods\u2026';
    input.setAttribute('aria-label', 'Filter mods');
    input.autocomplete = 'off';
    input.spellcheck = false;
    search.appendChild(input);
    var x = el('button', 'srch__clear', '\u00d7');
    x.type = 'button';
    x.setAttribute('aria-label', 'Clear filter');
    x.hidden = true;
    search.appendChild(x);
    body.appendChild(search);

    var count = el('p', 'pick__count');
    body.appendChild(count);
    var list = el('div', 'pick');
    list.setAttribute('role', 'listbox');
    list.setAttribute('aria-label', type + ' mods');
    body.appendChild(list);

    function modRow(iconDef, name, toks, subText, right, onclick, why, lore) {
      var b = el('button', 'pickrow');
      b.type = 'button';
      b.setAttribute('role', 'option');
      var w = el('span', 'pickrow__iconwrap');
      if (iconDef) { w.appendChild(imgWithFallback('pickrow__icon', iconDef.src, iconDef.fallback, '')); }
      else { w.appendChild(el('span', 'pickrow__x', '\u00d7')); }
      b.appendChild(w);
      var mid = el('span', 'bay__mid');
      var nm = el('span', 'bay__name');
      nm.appendChild(hl(name, toks));
      mid.appendChild(nm);
      mid.appendChild(el('span', 'bay__sub', subText));
      /* a mod's lore (catalog `lore`, optional, often absent): whole, under what it does */
      lore = String(lore || '').trim();
      if (lore) { mid.appendChild(el('span', 'pickrow__lore', lore)); }
      b.appendChild(mid);
      b.appendChild(right);
      if (why) { b.disabled = true; b.title = why; }
      else { b.onclick = onclick; }
      list.appendChild(b);
      return b;
    }

    var total = 0;
    function draw() {
      clear(list);
      var q = input.value.trim(), toks = tokens(q);
      var n = 0;
      total = 0;

      if (fitted && st.mods[fitted]) {
        var cur = st.mods[fitted];
        total++;
        if (rank(cur.name, cur.name.toLowerCase(), toks) >= 0) {
          modRow(null, 'Remove ' + cur.name, toks, 'Goes to your mod stash',
            el('span', 'bay__act is-bad', 'Remove'), function () { confirmDetach(inst, modSlot, cur.name); });
          n++;
        }
      }

      Data.ownedModsOfType(type).forEach(function (mod) {
        if (mod.uid === fitted) { return; }
        total++;
        var def = Data.mod(mod.mod) || {};
        var ownSays = Data.tierDesc(def, mod.tier);
        if (rank(mod.name, (mod.name + ' ' + (def.description || '') + ' ' + ownSays).toLowerCase(), toks) < 0) { return; }
        var where = mod.attachedTo
          ? 'Fitted to ' + (st.instances[mod.attachedTo.uid] || {}).name
          : 'In your mod stash';
        /* a mod the player owns reads at the tier they own, not the catalog's T5 */
        var sub = [Data.tierName(mod.tier), where];
        if (ownSays) { sub.push(ownSays); }
        Object.keys(mod.stats || {}).forEach(function (k) {
          sub.push(Data.statLabel(k) + ' ' + Data.formatStat(k, mod.stats[k]));
        });
        modRow(modIcon(mod.mod), mod.name, toks, sub.join('  \u00b7  '), el('span', 'bay__act', 'Fit'),
          function () { confirmAttach(inst, modSlot, mod.uid, mod.name); }, null, def.lore);
        n++;
      });

      Data.catalogModsOfType(type).forEach(function (c) {
        if (!c.cheapest) { return; }
        total++;
        var buySays = Data.tierDesc(c.mod, c.cheapest.tier);
        if (rank(c.mod.name, (c.mod.name + ' ' + (c.mod.description || '') + ' ' + buySays).toLowerCase(), toks) < 0) { return; }
        var p = el('span', 'row__price', Data.money(c.cheapest.price));
        var poor = st.currency.amount < c.cheapest.price;
        if (poor) { p.className = 'row__price is-poor'; }
        modRow(modIcon(c.mod.id), c.mod.name, toks,
          [Data.tierName(c.cheapest.tier), buySays].filter(Boolean).join('  \u00b7  '), p,
          function () { confirmBuyMod(inst, modSlot, c.mod, c.cheapest); },
          poor ? 'You need ' + Data.money(c.cheapest.price - st.currency.amount) + ' more ' + st.currency.name + '.' : null,
          c.mod.lore);
        n++;
      });

      count.textContent = q ? n + ' of ' + total + ' match' : total + ' option' + (total === 1 ? '' : 's');
      if (!n) {
        var none = el('div', 'rows__none');
        emptyState(none, q ? 'No \u201c' + typeName(type) + '\u201d mod matches \u201c' + q + '\u201d.'
                           : 'Nothing fits this slot: you own no \u201c' + typeName(type) + '\u201d mod and this ripperdoc sells none.',
          q ? [{ label: 'Clear filter', run: function () { setQ(''); input.focus(); } }] : []);
        list.appendChild(none);
      }
    }
    function setQ(v) { input.value = v; x.hidden = !v; draw(); }

    var lastLen = 0;
    input.oninput = function () {
      if (input.value.length > lastLen) { Snd.play('type'); }
      lastLen = input.value.length;
      x.hidden = !input.value;
      draw();
    };
    x.onclick = function () { Snd.play('clear'); setQ(''); input.focus(); };
    /* keys come through onKey: Enter takes the top hit, \u2193 enters the list
       (\u2191 on its first row comes back), Esc clears the box first */
    input.__cwKey = function (k) {
      if (k === 'Enter') {
        var first = list.querySelector('.pickrow:not([disabled])');
        if (first) { Snd.play('select'); first.click(); } else { Snd.play('error'); }
        return true;
      }
      if (k === 'Escape' && input.value) { Snd.play('clear'); setQ(''); return true; }
      return false;
    };
    draw();

    var foot = $('pop-foot');
    var no = el('button', 'btn');
    no.type = 'button';
    no.appendChild(el('span', null, 'Close'));
    no.appendChild(el('span', 'btn__sub', 'Esc'));
    no.onclick = function () { closePop(); };
    foot.appendChild(no);
    focusSoon(input);
  }

  /* ── capacity shards: what this doc sells to raise your capacity ──
     shop.capacityItems (BETA QA): {id, name, price, add, used, max, available,
     reason}. One row per shard, each with its own reason when it cannot be
     bought; the buy previews with dryRun like every other mutation. */
  function shardWhy(c, st) {
    if (c.reason === 'E_MAX_USES' || c.used >= c.max) { return 'All ' + c.max + ' used: your body takes no more ' + c.name + '.'; }
    if (c.reason === 'E_NO_FUNDS' || st.currency.amount < c.price) {
      return 'You need ' + Data.money(c.price - st.currency.amount) + ' more ' + st.currency.name + '.';
    }
    if (c.reason === 'E_NOT_IN_STOCK') { return 'This ripperdoc is out of ' + c.name + '.'; }
    if (c.reason === 'E_LEVEL') { return 'Needs a higher level.'; }
    return c.available === false ? 'Not available right now.' : null;
  }
  function openCapacityShop() {
    var st = Data.state();
    var items = (Data.shop() && Data.shop().capacityItems) || [];
    openPop('Raise capacity');
    $('pop').classList.add('pop--pick');
    var body = $('pop-body');
    body.appendChild(el('p', 'pop__lede',
      'You carry ' + st.capacity.used + ' of ' + st.capacity.max + ' capacity. Each shard adds to it ' +
      'at once and for good, up to the number your body can take.'));
    var list = el('div', 'pick');
    items.forEach(function (c) {
      var why = shardWhy(c, st);
      var b = el('button', 'pickrow pickrow--shard');
      b.type = 'button';
      var badge = el('span', 'pickrow__iconwrap shard__add');
      badge.appendChild(el('span', null, '+' + c.add));
      b.appendChild(badge);
      var mid = el('span', 'bay__mid');
      mid.appendChild(el('span', 'bay__name', c.name));
      mid.appendChild(el('span', 'bay__sub', '+' + c.add + ' capacity  \u00b7  ' + c.used + ' of ' + c.max + ' used'));
      b.appendChild(mid);
      var p = el('span', 'row__price' + (why ? ' is-poor' : ''), Data.money(c.price));
      b.appendChild(p);
      if (why) { b.disabled = true; b.title = why; }
      else {
        b.onclick = function () {
          preview('Buy ' + c.name,
            '+' + c.add + ' capacity, applied at once. ' + (c.used + 1) + ' of ' + c.max + ' used afterwards.',
            'buyCapacityItem', { id: c.id }, 'Buy', false, { name: c.name + '  +' + c.add + ' capacity' });
        };
      }
      list.appendChild(b);
      if (why) { list.appendChild(el('p', 'pick__why', why)); }
    });
    if (!items.length) {
      var none = el('div', 'rows__none');
      emptyState(none, 'This ripperdoc sells no capacity.');
      list.appendChild(none);
    }
    body.appendChild(list);
    var foot = $('pop-foot');
    var no = el('button', 'btn');
    no.type = 'button';
    no.appendChild(el('span', null, 'Close'));
    no.appendChild(el('span', 'btn__sub', 'Esc'));
    no.onclick = function () { closePop(); };
    foot.appendChild(no);
    var first = list.querySelector('.pickrow:not([disabled])') || no;
    focusSoon(first);
  }

  /* ═══════════════════ the quickhack loadout ═══════════════════
     ABILITIES D1 owns the deck logic and its state; this is the screen. The contract is
     docs/QUICKHACK-LOADOUT.md section 6: state.quickhacks (deck, slots, ramMax, program, equipped, order, custom,
     use, benched, selected, all[]) and one op, setLoadout { hacks, use }. The page never works a rule out for
     itself: which hacks are slotted, benched or unfireable is read from the block, and every change is sent whole
     (the complete order) and redrawn from what comes back.

     One ranked list. The first `use` entries of the order fill the deck's slots (after a fixed program, which is
     pinned and is not a choice); the rest are benched. "Bench" takes a hack out and leaves its slot EMPTY
     (the lead's ruling: the player decides how many presses the cycle key takes in a fight); "Slot" fills an empty
     slot, or with none free replaces the last slotted hack and says which. */
  var QH_KIND = { covert: 'Covert', combat: 'Combat', control: 'Control', ultimate: 'Ultimate' };
  function qh() { var st = Data.state(); return (st && st.quickhacks) || null; }
  /* the screen exists (the engine sends the block): Q opens it even with no deck, where it says what is missing */
  function hasLoadoutScreen() { return !!(qh() && Bridge.setLoadout); }
  /* ...and there is a deck fitted, so there is something to change */
  function hasLoadout() { var q = qh(); return !!(q && q.deck && Bridge.setLoadout); }
  function qhDef(id) {
    var a = (Data.catalog() && Data.catalog().quickhacks) || [];
    for (var i = 0; i < a.length; i++) { if (a[i].id === id) { return a[i]; } }
    return null;
  }
  function loadoutLine(q) {
    var n = q.benched.length;
    return q.equipped.length + ' of ' + q.slots + ' slots filled' + (n ? '  ·  ' + n + ' benched' : '');
  }
  /* the deck that carries a hack as its fixed program, by the catalog's own name for it */
  function programDeckName(hackId) {
    var items = (Data.catalog() && Data.catalog().items) || [];
    for (var i = 0; i < items.length; i++) {
      var f = (items[i].spec && items[i].spec.fo4 && items[i].spec.fo4.params_t5) || {};
      if (f.program === hackId) { return items[i].name; }
    }
    return null;
  }

  function openLoadout(focusHack) {
    if (!qh()) { return; }
    openPop('Quickhack loadout');
    $('pop').classList.add('pop--loadout');
    var body = $('pop-body');
    var busy = false;

    var wrap = el('div', 'lo');
    var main = el('div', 'lo__main'), side = el('aside', 'lo__side');
    side.setAttribute('aria-label', 'Your deck');
    wrap.appendChild(main); wrap.appendChild(side);
    body.appendChild(wrap);

    var search = el('label', 'srch');
    var g = el('span', 'srch__glyph');
    g.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="10.5" cy="10.5" r="6.5"/><path d="M15.5 15.5 L21 21"/></svg>';
    search.appendChild(g);
    var input = el('input', 'srch__input');
    input.type = 'search';
    input.id = 'lo-find';
    input.placeholder = 'Filter quickhacks…';
    input.setAttribute('aria-label', 'Filter quickhacks');
    input.autocomplete = 'off';
    input.spellcheck = false;
    search.appendChild(input);
    var x = el('button', 'srch__clear', '×');
    x.type = 'button';
    x.setAttribute('aria-label', 'Clear filter');
    x.hidden = true;
    search.appendChild(x);
    main.appendChild(search);

    /* the engine's own refusal, word for word (in combat, during an upload), never a tooltip */
    var why = el('p', 'lo__why');
    why.id = 'lo-why';
    why.setAttribute('role', 'alert');
    why.hidden = true;
    main.appendChild(why);

    var count = el('p', 'pick__count');
    main.appendChild(count);
    var list = el('div', 'lo__list');
    list.setAttribute('role', 'list');
    list.setAttribute('aria-label', 'Quickhacks, in your order');
    main.appendChild(list);

    /* what Reset replaced, so one press cannot throw an order away: Undo puts it back until the next change */
    var undo = null;
    function send(args, sound, focus, keepUndo) {
      if (busy) { return; }
      busy = true;
      why.hidden = true;
      Bridge.setLoadout(args).then(function (res) {
        busy = false;
        undo = keepUndo || null;
        Snd.play(sound || 'select');
        Data.absorb(res);
        draw(focus);
      }, function (e) {
        busy = false;
        Snd.play('error');
        /* the refusal goes where the player is looking: under the row they pressed. It stays until the next change
           or keystroke in the filter. */
        var at = focus && focus.hack ? list.querySelector('.lo__row[data-hack="' + focus.hack + '"]') : null;
        if (at) { list.insertBefore(why, at.nextSibling); } else { main.insertBefore(why, count); }
        why.textContent = (e && e.message) || 'The loadout could not be changed.';
        why.hidden = false;
      });
    }
    function setQ(v) { input.value = v; x.hidden = !v; draw(); }

    function draw(focus) {
      var q = qh();
      if (!q) { closePop(true); return; }
      if (why.parentNode !== main) { main.insertBefore(why, count); }
      why.hidden = true;
      clear(list);
      clear(side);
      /* The engine says BEFORE any press that nothing can change right now: quickhacks.locked (true while setLoadout
         would answer E_BUSY) and lockedReason ("in combat" / "a quickhack is running"), D1's words. Then every
         control is drawn unavailable and the sentence is on screen, instead of a button that refuses when pressed. */
      var lock = q.locked ? 'Your loadout cannot be changed right now: ' + (q.lockedReason || 'your deck is busy') + '.' : '';
      var byId = {};
      q.all.forEach(function (a) { byId[a.id] = a; });
      var room = Math.max(0, q.slots - (q.program ? 1 : 0));
      var slotted = q.order.filter(function (id) { return byId[id] && byId[id].equipped; });
      var fill = slotted.length;
      var text = input.value.trim(), toks = tokens(text), shown = 0;
      var ro = !q.deck;

      if (lock) {
        var lk = el('p', 'lo__why lo__lock', lock);
        lk.id = 'lo-lock';
        lk.setAttribute('role', 'status');
        list.appendChild(lk);
      }
      function matches(a) {
        var d = qhDef(a.id) || {};
        return rank(a.name, (a.name + ' ' + (d.effect || '') + ' ' + (QH_KIND[a.kind] || a.kind)).toLowerCase(), toks) >= 0;
      }
      function head(t, sub) {
        var h = el('div', 'lo__h');
        h.appendChild(el('span', 'lo__ht', t));
        if (sub) { h.appendChild(el('span', 'lo__hs', sub)); }
        list.appendChild(h);
      }
      function button(row, act, label, sub, cls, onclick, aria) {
        var b = el('button', 'btn btn--sm lo__b' + (cls ? ' ' + cls : ''));
        b.type = 'button';
        b.dataset.act = act;
        b.appendChild(el('span', 'btn__t', label));
        if (sub) { b.appendChild(el('span', 'btn__sub', sub)); }
        if (aria) { b.setAttribute('aria-label', aria); }
        if (onclick && !lock) { b.onclick = onclick; } else { b.disabled = true; }
        row.appendChild(b);
        return b;
      }
      /* the complete order with `id` moved to position `to` (the page always sends every non-program hack) */
      function moved(id, to) {
        var o = q.order.filter(function (h) { return h !== id; });
        o.splice(Math.max(0, Math.min(o.length, to)), 0, id);
        return o;
      }
      /* how many slots the player wants filled: null once every slot is, which is the engine's default */
      function useOf(n) { return n >= room ? null : n; }

      function row(a, kind) {
        if (toks.length && !matches(a)) { return; }
        shown++;
        var d = qhDef(a.id) || {};
        var dead = !!q.deck && a.fireable === false && kind !== 'other';   // with no deck nothing fires, and the screen says that once
        /* A hack this deck cannot pay for CAN be slotted (D1's spec; the lead's ruling on MN-28): the row says it
           cannot fire and why, with both numbers. The sentence is the ENGINE's (all[].unfireableReason, the same
           function the HUD's refusal uses), printed as it is so the two cannot drift; the page only builds the
           same form itself for an engine that sends none. */
        var cantFire = dead ? 'cannot fire: ' + (a.unfireableReason || ('needs ' + a.ram + ' RAM, this deck holds ' + q.ramMax)) : '';
        var r = el('div', 'lo__row lo__row--' + kind + (dead ? ' is-dead' : '') + (q.selected === a.id ? ' is-next' : ''));
        r.setAttribute('role', 'listitem');
        r.dataset.hack = a.id;
        var badge = el('span', 'lo__slot', a.slot ? String(a.slot) : '–');
        badge.setAttribute('aria-label', a.slot ? 'Slot ' + a.slot : 'Not in the deck');
        r.appendChild(badge);

        var mid = el('div', 'lo__mid');
        var top = el('span', 'lo__top');
        var nm = el('span', 'lo__name');
        nm.appendChild(hl(a.name, toks));
        top.appendChild(nm);
        /* where the hack stands, in WORDS on every row: colour alone never carries it */
        top.appendChild(el('span', 'lo__tag lo__tag--' + kind,
          kind === 'program' ? 'Slot ' + a.slot + '  \u00b7  built into this deck'
          : kind === 'slot' ? 'Slot ' + a.slot + (dead ? '  \u00b7  ' + cantFire : '')
          : kind === 'other' ? 'Locked'
          : ro ? 'No deck'
          : !room ? 'Benched  \u00b7  this deck has no free slot'
          : fill >= room ? 'Benched  \u00b7  all ' + q.slots + ' slots full'
          : 'Benched  \u00b7  slot ' + (q.equipped.length + 1) + ' empty'));
        if (q.selected === a.id) { top.appendChild(el('span', 'lo__tag lo__tag--next', 'Next upload')); }
        top.appendChild(el('span', 'lo__meta',
          [QH_KIND[a.kind] || a.kind, a.upload_s ? a.upload_s + ' s to upload' : 'Uploads at once'].join('  ·  ')));
        mid.appendChild(top);
        /* a locked hack says why it is locked first, then what it does; what it would cost here is beside the point */
        if (kind === 'other') {
          var deckName = programDeckName(a.id);
          /* the engine's reason ("Militech Canto only"); the catalog's deck name only for an engine that sends none */
          mid.appendChild(el('span', 'lo__note', 'Locked: ' + (a.lockedReason || ((deckName || 'another deck') + ' only')) +
            '. It comes built into that deck and cannot be slotted in this one.'));
        }
        if (d.effect) { mid.appendChild(el('span', 'lo__fx', d.effect)); }
        if (dead && kind !== 'slot') {
          /* benched: its tag already says why it is benched, so the cannot-fire sentence gets its own line */
          mid.appendChild(el('span', 'lo__note is-bad', cantFire.charAt(0).toUpperCase() + cantFire.slice(1) + '.'));
        }
        r.appendChild(mid);

        var cost = el('span', 'lo__ram');
        if (a.ramBase !== undefined && a.ramBase !== a.ram) {
          var was = el('s', 'lo__was', String(a.ramBase));
          was.setAttribute('aria-label', 'normally ' + a.ramBase);
          cost.appendChild(was);
        }
        cost.appendChild(el('span', 'lo__now', String(a.ram)));
        cost.appendChild(el('span', 'lo__unit', 'RAM'));
        r.appendChild(cost);

        var acts = el('div', 'lo__acts');
        if (!ro && kind === 'slot') {
          var i = slotted.indexOf(a.id);
          button(acts, 'up', '↑', null, 'lo__b--ico', i > 0 ? function () {
            send({ hacks: moved(a.id, i - 1), use: q.use }, 'tab', { hack: a.id, act: 'up' });
          } : null, 'Move ' + a.name + ' up');
          button(acts, 'down', '↓', null, 'lo__b--ico', i < fill - 1 ? function () {
            send({ hacks: moved(a.id, i + 1), use: q.use }, 'tab', { hack: a.id, act: 'down' });
          } : null, 'Move ' + a.name + ' down');
          button(acts, 'bench', 'Bench', 'frees slot ' + q.equipped.length, '', function () {
            send({ hacks: moved(a.id, fill - 1), use: useOf(fill - 1) }, 'remove', { hack: a.id, act: 'slot' });
          });
        } else if (!ro && kind === 'bench') {
          if (!room) {
            button(acts, 'slot', 'Slot', 'no free slot', '', null);
          } else if (fill < room) {
            button(acts, 'slot', 'Slot', 'into slot ' + (q.equipped.length + 1), 'btn--go', function () {
              send({ hacks: moved(a.id, fill), use: useOf(fill + 1) }, 'install', { hack: a.id, act: 'bench' });
            });
          } else {
            var last = byId[slotted[fill - 1]];
            button(acts, 'slot', 'Slot', 'replaces ' + last.name, 'btn--go', function () {
              send({ hacks: moved(a.id, fill - 1), use: q.use }, 'install', { hack: a.id, act: 'bench' });
            });
          }
        }
        r.appendChild(acts);
        if (!acts.querySelector('.lo__b:not([disabled])')) { r.tabIndex = 0; }
        list.appendChild(r);
      }

      /* ── in the deck ── */
      if (q.deck) {
        head('In your deck', q.equipped.length + ' of ' + q.slots + ' slots' +
          (q.benched.length ? '  \u00b7  ' + q.benched.length + ' benched below' : ''));
        if (q.program && byId[q.program]) { row(byId[q.program], 'program'); }
        slotted.forEach(function (id) { row(byId[id], 'slot'); });
        if (!toks.length) {
          for (var s = q.equipped.length + 1; s <= q.slots; s++) {
            var e = el('div', 'lo__row lo__row--empty');
            e.setAttribute('role', 'listitem');
            e.appendChild(el('span', 'lo__slot', String(s)));
            var em = el('div', 'lo__mid');
            var et = el('span', 'lo__top');
            et.appendChild(el('span', 'lo__name is-empty', 'Empty'));
            et.appendChild(el('span', 'lo__tag lo__tag--empty', 'Slot ' + s + '  \u00b7  left empty'));
            em.appendChild(et);
            em.appendChild(el('span', 'lo__meta', 'The cycle key skips it. Slot a benched quickhack to fill it.'));
            e.appendChild(em);
            list.appendChild(e);
          }
        }
        var benched = q.benched.filter(function (id) { return byId[id]; });
        if (benched.length) {
          head('Benched', 'kept in your order, not in the deck');
          benched.forEach(function (id) { row(byId[id], 'bench'); });
        }
      } else {
        head('Your order', 'no cyberdeck fitted');
        q.order.forEach(function (id) { if (byId[id]) { row(byId[id], 'bench'); } });
      }
      /* locked: the engine says so (all[].locked); not being in the order is the same fact for an engine that does not */
      var others = q.all.filter(function (a) { return a.locked || (a.id !== q.program && q.order.indexOf(a.id) === -1); });
      if (others.length) {
        head('Built into another deck');
        others.forEach(function (a) { row(a, 'other'); });
      }

      /* the count is the filter's answer; with nothing typed the headings already count, and the line is height the list needs */
      count.textContent = text ? shown + ' of ' + q.all.length + ' match' : '';
      count.hidden = !text;
      if (!shown) {
        clear(list);
        var none = el('div', 'rows__none');
        emptyState(none, 'No quickhack matches “' + text + '”.',
          [{ label: 'Clear filter', run: function () { Snd.play('clear'); setQ(''); input.focus(); } }]);
        list.appendChild(none);
      }

      /* ── the deck, beside the list ── */
      var deck = q.deck ? Data.item(q.deck) : null;
      var card = el('div', 'lo__deck');
      if (deck) {
        var ic = iconFor(deck);
        var iw = el('span', 'lo__deckicon');
        iw.appendChild(imgWithFallback('lo__deckimg', ic.src, ic.fallback, ''));
        card.appendChild(iw);
      }
      var dn = el('div', 'lo__deckt');
      dn.appendChild(el('span', 'lo__deckk', deck ? 'Your cyberdeck' : 'No cyberdeck'));
      dn.appendChild(el('span', 'lo__deckn', deck ? deck.name
        : 'Quickhacks need a cyberdeck in your Operating System slot. ' +
          (docList().length ? joinAnd(docList()) + ' can fit one. ' : '') + 'Your order is kept for when you do.'));
      card.appendChild(dn);
      side.appendChild(card);

      if (q.deck) {
        var pips = el('div', 'lo__pips');
        pips.setAttribute('role', 'img');
        pips.setAttribute('aria-label', q.equipped.length + ' of ' + q.slots + ' slots filled');
        for (var p = 1; p <= q.slots; p++) {
          pips.appendChild(el('span', 'lo__pip' + (p <= q.equipped.length ? ' is-on' : '') + (q.program && p === 1 ? ' is-fixed' : '')));
        }
        side.appendChild(pips);
        var facts = el('div', 'lo__facts');
        var fact = function (k, v) {
          var f = el('div', 'lo__fact');
          f.appendChild(el('span', 'lo__factk', k));
          f.appendChild(el('span', 'lo__factv', v));
          facts.appendChild(f);
        };
        fact('Slots filled', q.equipped.length + ' of ' + q.slots);
        fact('Benched', q.benched.length ? String(q.benched.length) : 'None');
        fact('Deck RAM', String(q.ramMax));
        fact('Next upload', q.selected && byId[q.selected] ? byId[q.selected].name : 'Nothing slotted');
        side.appendChild(facts);
      }

      var isDefault = !q.custom && q.use === null;
      var state = el('div', 'lo__state');
      state.appendChild(el('span', 'lo__statek', isDefault ? 'Default order' : 'Your order'));
      state.appendChild(el('span', 'lo__statev', isDefault
        ? 'Nothing changed yet: every slot is filled, in the order the deck came with.'
        : 'Kept when you save, and when you swap decks: a smaller deck benches the last ones, a bigger one brings them back.'));
      side.appendChild(state);
      if (q.deck) {
        var reset = el('button', 'btn btn--sm lo__reset');
        reset.type = 'button';
        reset.id = 'lo-reset';
        reset.appendChild(el('span', 'btn__t', 'Reset to default'));
        reset.appendChild(el('span', 'btn__sub', isDefault ? 'already the default' : 'default order, every slot filled'));
        if (isDefault || lock) { reset.disabled = true; }
        else {
          reset.onclick = function () {
            send({ hacks: [], use: null }, 'back', { reset: true }, { hacks: q.order.slice(), use: q.use });
          };
        }
        side.appendChild(reset);
        if (undo && !lock) {
          var un = el('button', 'btn btn--sm btn--go lo__reset');
          un.type = 'button';
          un.id = 'lo-undo';
          un.appendChild(el('span', 'btn__t', 'Undo reset'));
          un.appendChild(el('span', 'btn__sub', 'back to the order you had'));
          var back = undo;
          un.onclick = function () { send(back, 'install', { undo: true }); };
          side.appendChild(un);
        }
      }
      side.appendChild(el('p', 'lo__hint',
        'The cycle key steps through your slots in this order. Fewer slots filled means fewer presses in a fight.' +
        (isSetup() ? ' You can change this anywhere; buying or fitting a deck needs a ripperdoc.' : '')));

      /* focus follows the hack the player just moved, wherever its row went */
      if (focus) {
        var target = null;
        if (focus.reset) { target = $('lo-undo') || list.querySelector('.lo__b:not([disabled])'); }
        else if (focus.undo) { target = $('lo-reset'); }
        else {
          var fr = list.querySelector('.lo__row[data-hack="' + focus.hack + '"]');
          if (fr) { target = fr.querySelector('.lo__b[data-act="' + focus.act + '"]:not([disabled])') || fr.querySelector('.lo__b:not([disabled])'); }
          if (fr && !focus.quiet) { Fx.replay(fr, 'fx-pulse', 900); }   /* the row that moved lights up where it landed */
        }
        (target || input).focus();
      }
    }

    var lastLen = 0;
    input.oninput = function () {
      if (input.value.length > lastLen) { Snd.play('type'); }
      lastLen = input.value.length;
      x.hidden = !input.value;
      draw();
    };
    x.onclick = function () { Snd.play('clear'); setQ(''); input.focus(); };
    /* Enter takes the top hit: focus lands on that hack's Slot or Bench (the action that puts it in or takes it out
       of the deck, never Up or Down), so a second Enter does exactly what the button says. A hit with no action
       (the fixed program, a locked hack) takes the focus itself and a second Enter does nothing. Down does the same
       without the sound of a choice. Esc clears the box first; a second Esc closes the screen. */
    input.__cwKey = function (k) {
      if (k === 'Enter' || k === 'ArrowDown') {
        var hit = list.querySelector('.lo__row[data-hack]');
        var first = hit && (hit.querySelector('.lo__b[data-act="slot"]:not([disabled]), .lo__b[data-act="bench"]:not([disabled])') || hit);
        if (first) { Snd.play(k === 'Enter' ? 'select' : 'focus'); first.focus(); } else { Snd.play('error'); }
        return true;
      }
      if (k === 'Escape' && input.value) { Snd.play('clear'); setQ(''); return true; }
      return false;
    };
    draw(focusHack ? { hack: focusHack, act: 'slot' } : null);

    var foot = $('pop-foot');
    var no = el('button', 'btn');
    no.type = 'button';
    no.appendChild(el('span', null, 'Done'));
    no.appendChild(el('span', 'btn__sub', 'Esc'));
    no.onclick = function () { closePop(); };
    foot.appendChild(no);
    if (!focusHack) { focusSoon(list.querySelector('.lo__b:not([disabled])') || input); }
  }

  /* ═══════════════════ perks view ═══════════════════ */
  function renderPerks() {
    var st = Data.state();
    Fx.countTo($('pp-available'), st.perkPoints.available);
    $('pp-sub').textContent = st.perkPoints.spent + ' of ' + st.perkPoints.total + ' spent · level ' + st.level;

    var list = $('perks');
    clear(list);
    var q = ui.perkQuery, toks = tokens(q);
    var shown = 0;
    var hits = [];
    (st.cyberPerks || []).forEach(function (p) {
      var def = Data.perk(p.id) || {};
      var rk = rank(p.name, (p.name + ' ' + (def.description || '')).toLowerCase(), toks);
      if (rk >= 0) { hits.push({ p: p, def: def, rk: rk }); }
    });
    /* filtering: best match first; otherwise the catalog's own order */
    if (toks.length) { hits.sort(function (a, b) { return a.rk - b.rk; }); }

    hits.forEach(function (h) {
      var p = h.p, def = h.def;
      shown++;

      var li = el('li', 'perk' + (p.rank ? ' is-owned' : '') + (p.auto ? ' is-auto' : ''));
      li.dataset.perk = p.id;
      var top = el('div', 'perk__top');
      var pn = el('span', 'perk__n');
      pn.appendChild(hl(p.name, toks));
      top.appendChild(pn);
      top.appendChild(el('span', 'perk__r', 'Rank ' + p.rank + ' / ' + p.maxRank));
      li.appendChild(top);

      var ranks = el('div', 'perk__ranks');
      for (var i = 0; i < p.maxRank; i++) {
        ranks.appendChild(el('span', 'perk__pip' + (i < p.rank ? ' is-on' : '')));
      }
      li.appendChild(ranks);

      li.appendChild(el('p', 'perk__d', def.description || ''));

      if (p.auto) {
        li.appendChild(el('div', 'perk__auto',
          'Follows a vanilla Fallout 4 perk. Its rank tracks yours; it cannot be bought or refunded here.'));
      } else {
        var acts = el('div', 'perk__acts');
        var buy = el('button', 'btn btn--go');
        buy.type = 'button';
        var maxed = p.rank >= p.maxRank;
        buy.appendChild(el('span', null, maxed ? 'Max rank' : p.rank ? 'Next rank' : 'Buy'));
        buy.appendChild(el('span', 'btn__sub', maxed ? 'all ' + p.maxRank + ' owned' : p.cost + ' pt' + (p.cost === 1 ? '' : 's')));
        var perkWhy = null;
        if (isSetup() && !maxed) {
          perkWhy = 'Perks are bought at a ripperdoc.';
          buy.disabled = true; buy.title = perkWhy;
        } else if (maxed) {
          buy.disabled = true; buy.title = 'Already at max rank.';
        } else if (!p.canBuy || st.perkPoints.available < p.cost) {
          perkWhy = perkReason(p, st);
          buy.disabled = true; buy.title = perkWhy;
        } else {
          buy.onclick = function () { confirmPerk(p, true); };
        }
        acts.appendChild(buy);

        var ref = el('button', 'btn btn--bad');
        ref.type = 'button';
        ref.appendChild(el('span', null, 'Refund'));
        if (isSetup()) { ref.disabled = true; ref.title = 'Perks are refunded at a ripperdoc.'; }
        else if (!p.canRefund) { ref.disabled = true; ref.title = p.rank ? 'This rank cannot be refunded right now.' : 'No rank to refund yet.'; }
        else { ref.onclick = function () { confirmPerk(p, false); }; }
        acts.appendChild(ref);
        li.appendChild(acts);
        /* the reason is on the card, not only in a tooltip */
        if (perkWhy) { li.appendChild(el('p', 'perk__why', perkWhy)); }
      }
      list.appendChild(li);
    });

    var none = $('perks-none');
    none.hidden = shown > 0;
    if (!shown) {
      emptyState(none, 'No cyber perk matches \u201c' + q + '\u201d.', [
        { label: 'Clear filter', run: function () { Snd.play('clear'); setSearch('find-perk', ''); $('find-perk').focus(); } }
      ]);
    }
    if (perksEnter) { perksEnter = false; Fx.enter(list.querySelectorAll('.perk'), 'fx-in', 22, 12); }
  }
  var perksEnter = false;

  function perkReason(p, st) {
    if (p.reason === 'E_LEVEL' || (p.requiresLevel && st.level < p.requiresLevel)) {
      return 'Needs level ' + p.requiresLevel + ' (you are ' + st.level + ').';
    }
    if (p.reason === 'E_PREREQ') {
      var names = (p.prereq || []).map(function (id) { var d = Data.perk(id); return d ? d.name : id; });
      return 'Needs ' + (names.length ? names.join(' and ') : 'another perk') + ' first.';
    }
    if (p.reason === 'E_PERK_POINTS' || st.perkPoints.available < p.cost) {
      var need = p.cost - st.perkPoints.available;
      return 'Needs ' + need + ' more perk point' + (need === 1 ? '' : 's') + '.';
    }
    if (p.reason === 'E_AUTO_PERK') { return 'Follows a vanilla perk; it cannot be bought here.'; }
    return 'Not available right now.';
  }

  /* ═══════════════════ navigation ═══════════════════ */
  function selectSlot(slotId) {
    ui.slot = slotId;
    ui.globalQuery = '';
    $('find-all').value = '';
    $('find-all-clear').hidden = true;
    ui.query = '';
    $('find-slot').value = '';
    $('find-slot-clear').hidden = true;
    ui.itemId = null; ui.uid = null;
    ui.pane = 'list';
    renderAll();
    /* desktop shows the detail beside the list: open the top piece rather
       than leave a third of the screen asking for a pick */
    if (ui.rowIds.length && !isNarrow()) {
      ui.itemId = ui.rowIds[0]; ui.tab = 'stats'; syncTabs();
      ui.pane = 'list';
      renderAll();
    }
    $('find-slot').focus();
  }

  function selectItem(itemId, uid) {
    ui.itemId = itemId;
    ui.uid = uid || null;      // a specific owned copy (a positions cell), else the best one
    ui.tab = 'stats';
    ui.pane = 'detail';
    var it = Data.item(itemId);
    /* the list beside the detail always shows the piece's own slot (cw-int-2:
       Mantis Blades in the detail beside the Frontal Cortex list) */
    if (it && !ui.globalQuery && ui.slot !== it.slot) {
      ui.slot = it.slot;
      ui.query = ''; $('find-slot').value = ''; $('find-slot-clear').hidden = true;
    }
    syncTabs();
    renderAll();
  }

  /* Desktop shows list and detail side by side. When the list changes under
     the detail (a search cleared after picking a hit, the menu reopened), the
     list moves to the detail's slot; only a detail that still is not in the
     list (a filter hides it, no piece selected) moves to the list's top row. */
  function syncDetailToList() {
    /* your setup with nothing to show: the detail says why instead of describing a piece you do not own */
    if (isSetup() && !ui.rowIds.length && ui.itemId) { ui.itemId = null; ui.uid = null; renderAll(); return; }
    if (isNarrow() || ui.globalQuery || ui.rowIds.indexOf(ui.itemId) !== -1) { return; }
    var it = ui.itemId ? Data.item(ui.itemId) : null;
    if (it && it.slot !== ui.slot) {
      ui.slot = it.slot;
      ui.query = ''; $('find-slot').value = ''; $('find-slot-clear').hidden = true;
      renderAll();
      if (ui.rowIds.indexOf(ui.itemId) !== -1) { return; }
    }
    if (!ui.rowIds.length) { return; }
    ui.itemId = ui.rowIds[0]; ui.uid = null; ui.tab = 'stats'; syncTabs();
    renderList(); renderDetail();
  }

  /* The section tabs (Chrome / Perks, and any a later stream adds) are read
     from the header, so a new <main id="view-X"> + tab button just works. */
  function setView(v) {
    /* the header search is the Chrome page's: it does not ride along to Perks
       or Settings still saying "kiroshi" (visual gate, cw-int-1 T7) */
    if (v !== 'chrome' && ui.globalQuery) { setSearch('find-all', ''); }
    var prev = ui.view;
    ui.view = v;
    $('cw').dataset.view = v;
    var tabs = $('view-tabs').querySelectorAll('.seg__c');
    for (var i = 0; i < tabs.length; i++) {
      var on = tabs[i].dataset.v === v;
      tabs[i].classList.toggle('is-on', on);
      tabs[i].setAttribute('aria-selected', on ? 'true' : 'false');
      var main = $('view-' + tabs[i].dataset.v);
      if (main) { main.hidden = !on; }
    }
    if (v === 'perks' && prev !== 'perks') { perksEnter = true; }
    renderAll();
    if (prev && prev !== v) { Fx.replay($('view-' + v), 'fx-view', 380); }
  }
  function viewOrder() {
    var tabs = $('view-tabs').querySelectorAll('.seg__c'), out = [];
    for (var i = 0; i < tabs.length; i++) { if (!tabs[i].hidden && !tabs[i].disabled) { out.push(tabs[i].dataset.v); } }
    return out;
  }
  function stepView(delta) {
    var order = viewOrder(), i = order.indexOf(ui.view);
    if (i === -1 || order.length < 2) { return; }
    Snd.play('tab');
    setView(order[(i + delta + order.length) % order.length]);
  }

  function setTab(t) {
    if (t === ui.tab || TAB_ORDER.indexOf(t) === -1) { return; }
    Snd.play('tab');
    ui.tab = t; syncTabs(); renderTab();
  }
  function setFilter(f) {
    if (f === ui.filter) { return; }
    Snd.play('tab');
    ui.filter = f; syncTabs(); renderList();
  }
  /* set a search box from code: same path as typing into it */
  function setSearch(inputId, v) {
    var input = $(inputId);
    input.value = v;
    input.dispatchEvent(new Event('input'));
  }
  function showPerksFor(q) {
    var v = q;
    setSearch('find-all', '');
    setView('perks');
    setSearch('find-perk', v);
    $('find-perk').focus();
  }

  function syncTabs() {
    var tabs = $('dt-tabs').querySelectorAll('.seg__c');
    for (var i = 0; i < tabs.length; i++) {
      var on = tabs[i].dataset.t === ui.tab;
      tabs[i].classList.toggle('is-on', on);
      tabs[i].setAttribute('aria-selected', on ? 'true' : 'false');
    }
    var fs = $('list-filter').querySelectorAll('.seg__c');
    for (var j = 0; j < fs.length; j++) {
      var onf = fs[j].dataset.f === ui.filter;
      fs[j].classList.toggle('is-on', onf);
      fs[j].setAttribute('aria-selected', onf ? 'true' : 'false');
    }
  }

  /* renderAll rebuilds rows, nodes and buttons, which drops keyboard / d-pad
     focus on <body>. Remember which control had it, by what it IS (an item
     row, a slot node, a tab, an id), and put focus back after the rebuild. */
  function focusKey() {
    var a = document.activeElement;
    if (!a || a === document.body) { return null; }
    if (a.dataset && a.dataset.item && a.closest('#rows')) { return '#rows .row[data-item="' + a.dataset.item + '"]'; }
    if (a.dataset && a.dataset.slot && a.classList.contains('node')) { return '.node[data-slot="' + a.dataset.slot + '"]'; }
    if (a.dataset && a.dataset.t) { return '#dt-tabs [data-t="' + a.dataset.t + '"]'; }
    if (a.dataset && a.dataset.perk) { return '[data-perk="' + a.dataset.perk + '"]'; }
    if (a.id) { return '#' + a.id; }
    return null;
  }
  function restoreFocus(key) {
    if (!key) { return; }
    var a = document.activeElement;
    if (a && a !== document.body && document.body.contains(a)) { return; }
    var n = document.querySelector(key);
    if (n && n.offsetParent !== null) { try { n.focus({ preventScroll: true }); } catch (e) { n.focus(); } }
  }

  var lastPane = null;
  function renderAll() {
    var fk = focusKey();
    renderAllInner();
    restoreFocus(fk);
  }
  function renderAllInner() {
    $('cw').dataset.pane = ui.pane;
    /* phone width: one pane at a time; it slides in from the side you went */
    if (lastPane && lastPane !== ui.pane && isNarrow()) {
      var order = ['body', 'list', 'detail'];
      Fx.replay($('pane-' + ui.pane), order.indexOf(ui.pane) > order.indexOf(lastPane) ? 'fx-tab-r' : 'fx-tab-l', 360);
    }
    lastPane = ui.pane;
    renderHeader();
    if (ui.view === 'settings') { if (CW.Settings) { CW.Settings.render(); } return; }   /* BETA QA */
    if (ui.view === 'perks') { renderPerks(); return; }
    if (ui.view === 'customize') { CW.Customize.render(); return; }
    if (ui.view === 'armory') { if (CW.Armory) { CW.Armory.render(); } return; }   /* ARMORY */
    if (ui.view !== 'chrome') { return; }   // another stream's view renders itself
    renderBody();
    renderList();
    renderDetail();
  }
  function isNarrow() { return window.matchMedia('(max-width: 1499px)').matches; }

  /* ═══════════════════ input ═══════════════════ */
  function moveRow(delta, keepFocusOnRow) {
    if (!ui.rowIds.length) { return; }
    var i = ui.rowIds.indexOf(ui.itemId);
    i = (i === -1) ? (delta > 0 ? 0 : ui.rowIds.length - 1) : i + delta;
    if (i < 0) { i = ui.rowIds.length - 1; }
    if (i >= ui.rowIds.length) { i = 0; }
    Snd.play('focus');
    selectItem(ui.rowIds[i]);
    var node = $('rows').querySelector('[data-item="' + ui.rowIds[i] + '"]');
    if (node && node.scrollIntoView) { node.scrollIntoView({ block: 'nearest' }); }
    if (node && keepFocusOnRow) { node.focus(); }
  }

  function back() {
    if (ui.pane === 'detail' && isNarrow()) { Snd.play('back'); ui.pane = 'list'; renderAll(); return true; }
    if (ui.pane === 'list' && isNarrow()) { Snd.play('back'); ui.pane = 'body'; renderAll(); return true; }
    return false;
  }
  function closeMenu() {
    Snd.play('menuClose');
    Bridge.close();
  }

  var ARROWS = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };

  /* Put focus on a control in a popout that has just opened, and make it
     stick. In game the click that opened the popout (the DLL answers inside
     the click) gave focus back to the button behind the scrim afterwards
     (cycle 1: Enter in the buy confirm re-ran the dry run), so re-assert it
     after the engine is done with the click. */
  function focusSoon(target) {
    if (!target) { return; }
    var again = function () {
      if ($('scrim').hidden || !document.body.contains(target)) { return; }
      if (!$('pop').contains(document.activeElement)) { target.focus(); }
    };
    target.focus();
    setTimeout(again, 0);
    setTimeout(again, 120);
  }
  function popPrimary() {
    return $('pop-foot').querySelector('.btn--go') || $('pop').querySelector('.srch__input') ||
           $('pop').querySelector('button:not([disabled])');
  }

  /* ═══ the ONE keyboard handler ═══
     PrismaUI's Ultralight does not reliably deliver a key to the focused
     element (cycle 1: Esc typed into a search box closed the whole menu;
     Left / Enter in a popout went to the button behind it). So every key is
     decided here, at document CAPTURE, from document.activeElement; a handled
     key is stopped, so no element listener sees it twice. Buttons are
     activated with click(), never left to the engine's default action. An
     input that wants keys exposes el.__cwKey(key) -> true when it handled it. */
  function onKey(e) {
    var k = keyOf(e);
    /* the scroll experiment's live switch; first, so no screen can swallow it */
    if (e.ctrlKey && e.shiftKey && (k === 'S' || k === 's')) {
      e.preventDefault(); e.stopPropagation();
      toggleScrollPE();
      return;
    }
    var a = document.activeElement;
    if (!a || a === document.body || a === document.documentElement) { a = null; }
    var popOpen = !$('scrim').hidden;
    var inPop = !!(a && $('pop').contains(a));
    if (popOpen && a && !inPop) { a = null; }          // focus left behind the popout counts as none
    var isInput = !!(a && /^(INPUT|TEXTAREA)$/.test(a.tagName));
    var isButton = !!(a && a.tagName === 'BUTTON');
    function eat() { e.preventDefault(); e.stopPropagation(); }

    /* 1. an input that handles this key itself: search boxes, the picker filter */
    if (isInput && a.__cwKey && a.__cwKey(k)) { eat(); return; }

    /* 2. Escape: popout, then a step back (phone), then the menu. UI-API.md §1:
          a focused PrismaUI view eats the key, so the page must send close. */
    if (k === 'Escape') {
      eat();
      if (popOpen) { if (popOnEscape) { popOnEscape(); } else { closePop(); } return; }
      if (back()) { return; }
      closeMenu();
      return;
    }
    /* PgUp / PgDn (a pad's bumpers map here) cycle the top-level sections */
    if (k === 'PageUp' || k === 'PageDown') {
      if (!popOpen) { eat(); stepView(k === 'PageDown' ? 1 : -1); }
      return;
    }
    /* 3. Enter / Space press the focused button; in a popout with nothing
          focused, Enter is the popout's confirm */
    if (k === 'Enter' || k === ' ') {
      if (isInput) { return; }
      if (isButton) { eat(); if (!a.disabled) { a.click(); } return; }
      if (k === 'Enter' && popOpen && popOnConfirm) { eat(); popOnConfirm(); }
      return;
    }

    if (popOpen) {
      if (ARROWS[k] && !(isInput && (k === 'ArrowLeft' || k === 'ArrowRight'))) {
        eat();
        if (!a) { focusSoon(popPrimary()); return; }
        if (isInput && k === 'ArrowDown') {
          var r0 = $('pop').querySelector('.pickrow:not([disabled])');
          if (r0) { r0.focus(); }
          return;
        }
        /* \u2191 on the first picker row goes back to its filter box */
        if (k === 'ArrowUp' && a.classList.contains('pickrow')) {
          var first = $('pop').querySelector('.pickrow:not([disabled])');
          var box = $('pop').querySelector('.srch__input');
          if (first === a && box) { box.focus(); return; }
        }
        Fx.move(ARROWS[k], a, $('pop'));
      }
      return;
    }
    if (isInput) { return; }                            // typing: characters and the caret keys

    /* A scene is a TAKEOVER: while one plays every key belongs to it, because scene.js's skip handler is a
       capture-phase keydown listener that eats ANY key on purpose ("Skip fires on ANY keydown"). Acting on a
       view hotkey here opens a screen OVER a running scene, and every keystroke the player then aims at that
       screen is swallowed by the scene instead - after an install, Q opened the quickhack loadout and its
       filter took NOTHING at all, not one character, until the scene ended. Fall through WITHOUT eating, so
       the scene still receives the key: once it is armed (SKIP_ARM_MS, 1 s in) that key skips the scene, which
       is what pressing one during a scene should do, and inside the arming second it is ignored rather than
       opening anything - measured, both.
       The "Deck fitted / Set your quickhack loadout: Q" banner already waits on this exact test, so the hint
       waited for the scene while the key it names did not. */
    var scn = $('scene');
    if (scn && !scn.hidden) { return; }

    if (k === '/') {
      eat();
      var sbox = ui.view === 'perks' ? $('find-perk') : (ui.slot ? $('find-slot') : $('find-all'));
      if (sbox && sbox.offsetParent !== null) { sbox.focus(); sbox.select(); }
      return;
    }
    if (k === 'Backspace') { eat(); back(); return; }
    /* L: the HUD layout editor (drag and resize every HUD element). The plugin closes this menu and hands the HUD
       the focus; BETA QA's Settings tab has the same action as a button (CW.Bridge.hudEdit). */
    if (k === 'l' || k === 'L') { eat(); Snd.play('open'); Bridge.hudEdit(); return; }
    /* Q: the quickhack loadout, whenever a cyberdeck is fitted (at a ripperdoc and in the field) */
    if ((k === 'q' || k === 'Q') && hasLoadoutScreen()) { eat(); Snd.play('open'); openLoadout(); return; }

    if (ui.view === 'chrome') {
      /* 1-5 jump straight to a detail tab */
      if (/^[1-5]$/.test(k) && ui.itemId) { eat(); setTab(TAB_ORDER[+k - 1]); return; }

      var inRows = !!(a && a.closest('#rows'));
      var inTabs = !!(a && a.closest('#dt-tabs'));
      var free = !a || (!isButton && !a.closest('[tabindex="0"]'));
      if ((k === 'ArrowDown' || k === 'ArrowUp') && (free || (inRows && a.classList.contains('row')))) {
        eat();
        /* \u2191 on the first row goes back up to the filter box */
        if (inRows && k === 'ArrowUp' && ui.rowIds.indexOf(ui.itemId) === 0) {
          (ui.globalQuery ? $('find-all') : $('find-slot')).focus(); return;
        }
        moveRow(k === 'ArrowDown' ? 1 : -1, inRows);
        return;
      }
      if ((k === 'ArrowLeft' || k === 'ArrowRight') && ui.itemId && (free || inTabs)) {
        eat();
        var i = TAB_ORDER.indexOf(ui.tab) + (k === 'ArrowRight' ? 1 : -1);
        if (i >= 0 && i < TAB_ORDER.length) {
          setTab(TAB_ORDER[i]);
          if (inTabs) { $('dt-tabs').querySelector('.is-on').focus(); }
        }
        return;
      }
    }
    /* The stat panel is the one part of the card that scrolls (a long stat table, the lore at 1080). With
       focus on it the d-pad scrolls it first and only moves on once it has reached that end, so a pad can
       read to the last line and the panel is never a trap. */
    if ((k === 'ArrowDown' || k === 'ArrowUp') && a && a.id === 'dt-panel') {
      var room = k === 'ArrowDown' ? a.scrollHeight - a.clientHeight - a.scrollTop : a.scrollTop;
      if (room > 1) {
        eat();
        a.scrollTop += (k === 'ArrowDown' ? 1 : -1) * Math.max(48, Math.round(a.clientHeight * 0.4));
        return;
      }
    }
    /* everywhere else the arrows are a d-pad: nearest control that way */
    if (ARROWS[k] && a) {
      if (Fx.move(ARROWS[k], a, ui.view === 'chrome' ? $('cw') : document)) { eat(); }
    }
  }

  /* ── a list that is MOVING does no hover work ──────────────────────────
     E (beta update 2). The author, 2026-11-12: "scrolling filter is laggy and
     stuttery". When the wheel turns, the rows move under a cursor that stays
     put, so the engine re-hovers a different row every frame and starts that
     row's transitions - the background and border fade, the name's colour and
     the icon's 1.07 scale. Measured by ui/harness/perf-probe.sh (2560x1440,
     the real 122-row catalog list, 60 wheel ticks, cursor resting on the
     list): 236 style recalculations / 50 ms and 112 LAYOUTS / 34 ms, against
     60 recalculations / 3 ms and NO layout for the identical scroll with the
     pointer off the list.
     So while a scroller is actually moving it wears `is-scrolling`, and
     styles.css holds its rows at their resting look with no transitions. The
     class goes on at the first scroll event and comes off about 140 ms after
     the last one, so a list that has stopped hovers exactly as it did before
     and nothing changes for a still list.
     Deliberately NOT done: `pointer-events: none` on the rows during the
     scroll, which would also stop the hover selector matching at all (another
     ~58 recalculations). In Chromium the wheel then falls through to the
     scroller, but Ultralight 1.4 is a different engine and this cannot be
     tested off the rig - a list that stops scrolling mid-flick in game is a
     far worse bug than the one being fixed. */
  /* ── the P4 experiment: rows out of hit-testing while the list moves ──
     OFF by default. Ctrl+Shift+S flips it live, so the ONE in-game run can
     capture the same scroll both ways without a rebuild or a relaunch and the
     grader picks. The rule itself is in styles.css under data-scrollpe; the
     reason it is not simply on is that Ultralight 1.4's wheel handling with
     pointer-events: none on the rows cannot be tested off the rig, and a list
     that stops scrolling mid-flick in game is worse than the stutter.
     A harness flips it without the keyboard:
        document.getElementById('cw').dataset.scrollpe = 'off'   (and delete to undo) */
  function toggleScrollPE() {
    var root = $('cw');
    var turningOn = root.dataset.scrollpe !== 'off';
    if (turningOn) { root.dataset.scrollpe = 'off'; } else { delete root.dataset.scrollpe; }
    Snd.play('tab');
    toast(turningOn
      ? 'Scroll test ON: rows ignore the pointer while the list moves.'
      : 'Scroll test OFF: normal hover while the list moves.', turningOn);
  }

  function wireScrollState(el, idleMs) {
    if (!el || el.__cwScroll) { return; }
    el.__cwScroll = true;
    var t = null;
    el.addEventListener('scroll', function () {
      if (t) { global.clearTimeout(t); }
      else { el.classList.add('is-scrolling'); }
      t = global.setTimeout(function () {
        t = null;
        el.classList.remove('is-scrolling');
      }, idleMs || 140);
    });
  }
  /* every scroller whose rows have a hover state */
  function wireScrollers() {
    var ids = ['rows', 'dt-panel', 'pop-body'];
    for (var i = 0; i < ids.length; i++) { wireScrollState($(ids[i])); }
    var more = document.querySelectorAll('.perkview, .totals__list');
    for (var j = 0; j < more.length; j++) { wireScrollState(more[j]); }
  }

  function wireInput() {
    document.addEventListener('keydown', onKey, true);
    wireScrollers();

    /* ── search boxes: real inputs, filter as you type, Enter takes the top hit ── */
    function wireSearch(inputId, clearId, apply, onEnter) {
      var input = $(inputId), btn = $(clearId), lastLen = 0;
      input.addEventListener('input', function () {
        if (input.value.length > lastLen && document.activeElement === input) { Snd.play('type'); }
        lastLen = input.value.length;
        btn.hidden = !input.value;
        apply(input.value.trim());
      });
      /* keys come through onKey (document capture), not a listener here */
      input.__cwKey = function (k) {
        if (k === 'Enter') { onEnter(); return true; }
        if (k === 'ArrowDown') { onEnter(true); return true; }
        if (k === 'Escape' && input.value) {
          Snd.play('clear');
          input.value = ''; lastLen = 0; btn.hidden = true; apply('');
          return true;
        }
        return false;
      };
      btn.addEventListener('click', function () {
        Snd.play('clear');
        input.value = ''; lastLen = 0; btn.hidden = true; apply(''); input.focus();
      });
    }

    /* Enter: the highlighted row if the player arrowed to one, else the top
       hit; focus lands on it so the d-pad carries on from there. \u2193 moves
       into the list without leaving the box behind. */
    function takeRow(justMove) {
      if (ui.view !== 'chrome') { return; }
      if (!ui.rowIds.length) { if (!justMove) { Snd.play('error'); } return; }
      var id = ui.rowIds.indexOf(ui.itemId) !== -1 ? ui.itemId : ui.rowIds[0];
      if (justMove && ui.rowIds.indexOf(ui.itemId) === -1) { id = ui.rowIds[0]; }
      Snd.play(justMove ? 'focus' : 'select');
      selectItem(id);
      var node = $('rows').querySelector('[data-item="' + id + '"]');
      if (node) { node.focus(); if (node.scrollIntoView) { node.scrollIntoView({ block: 'nearest' }); } }
    }
    function takePerk() {
      var b = $('perks').querySelector('.btn:not([disabled])') || $('perks').querySelector('.btn');
      if (b) { b.focus(); } else { Snd.play('error'); }
    }

    wireSearch('find-all', 'find-all-clear', function (v) {
      var hadQuery = !!ui.globalQuery;
      ui.globalQuery = v;
      if (hadQuery !== !!v) { renderBody(); }   // the map lights no slot while a search lists them all
      if (v && ui.view !== 'chrome') { setView('chrome'); }
      if (v) { ui.pane = 'list'; }
      renderList();
      /* desktop shows the detail beside the results: it follows the top hit
         as the player types (cycle 1: it stayed on an earlier keystroke's hit) */
      if (v && ui.rowIds.length && ui.itemId !== ui.rowIds[0] && !isNarrow()) {
        ui.itemId = ui.rowIds[0]; ui.uid = null; ui.tab = 'stats'; syncTabs();
        markSelectedRow(); renderDetail();
      }
      if (!v) { syncDetailToList(); }
      $('cw').dataset.pane = ui.pane;
    }, takeRow);
    wireSearch('find-slot', 'find-slot-clear', function (v) { ui.query = v; renderList(); }, takeRow);
    wireSearch('find-perk', 'find-perk-clear', function (v) { ui.perkQuery = v; renderPerks(); }, takePerk);

    /* ── segmented controls ── */
    $('view-tabs').addEventListener('click', function (e) {
      var b = e.target.closest('.seg__c');
      if (b && b.dataset.v !== ui.view) { Snd.play('tab'); setView(b.dataset.v); }
    });
    $('dt-tabs').addEventListener('click', function (e) {
      var b = e.target.closest('.seg__c');
      if (b) { setTab(b.dataset.t); }
    });
    $('list-filter').addEventListener('click', function (e) {
      var b = e.target.closest('.seg__c');
      if (b) { setFilter(b.dataset.f); }
    });

    $('back-body').onclick = function () { Snd.play('back'); ui.pane = 'body'; renderAll(); };
    $('back-list').onclick = function () { Snd.play('back'); ui.pane = 'list'; renderAll(); };
    $('btn-close').onclick = closeMenu;
    $('pop-x').onclick = function () { closePop(); };
    $('scrim').addEventListener('mousedown', function (e) {
      if (e.target === $('scrim')) { closePop(); }
    });

    /* keep focus inside the popout while it is open */
    document.addEventListener('focusin', function (e) {
      if ($('scrim').hidden) { return; }
      if (!$('pop').contains(e.target)) { var p = popPrimary(); if (p) { p.focus(); } }
    });
  }

  /* ═══════════════════ harness ═══════════════════
     Query parameters that put the view on a given screen, so the headless
     harness can capture every one and prove it is reachable. In game the
     page is opened with no query string and none of this runs.
       ?view=perks
       ?slot=<slotId>&item=<itemId>&tab=tiers|mods|rolls|looks
       ?pop=upgrade|install|remove|mods|buy|perk|greet
       ?pane=body|list|detail   (phone screens)                           */
  function applyHarnessParams() {
    var q = {};
    String(location.search || '').replace(/^\?/, '').split('&').forEach(function (p) {
      if (!p) { return; }
      var kv = p.split('=');
      q[decodeURIComponent(kv[0])] = decodeURIComponent(kv[1] || '');
    });
    if (!Object.keys(q).length) { return; }

    if (q.setup) { ui.setup = true; }
    if (q.view) { setView(q.view); }
    if (q.slot) { ui.slot = q.slot; ui.itemId = null; }
    if (q.item) { ui.itemId = q.item; ui.uid = null; }
    if (q.tab) { ui.tab = q.tab; }
    if (q.filter) { ui.filter = q.filter; }
    if (q.pane) { ui.pane = q.pane; }
    syncTabs();
    renderAll();
    if (q.find) { setSearch('find-all', q.find); }

    if (!q.pop) { return; }
    var inst = currentInstance();
    var it = ui.itemId && Data.item(ui.itemId);
    if (q.pop === 'mods' && inst && inst.modSlotTypes.length) {
      openModPicker(inst, 0, inst.modSlotTypes[0]);
    } else if (q.pop === 'upgrade' && inst && inst.upgrade) {
      confirmUpgrade(inst);
    } else if (q.pop === 'install' && inst && !inst.installed) {
      confirmInstall(inst);
    } else if (q.pop === 'remove' && inst && inst.installed) {
      confirmRemove(inst);
    } else if (q.pop === 'buy' && it) {
      var row = Data.rowFor(it).cheapest;
      if (row) { confirmBuy(it, row); }
    } else if (q.pop === 'prompt') {
      openPrompt({ doc: q.doc || 'diamondcity' });
    } else if (q.pop === 'greet') {
      openGreeting(q.talkmode ? { talkMode: true } : undefined);
    } else if (q.pop === 'perk') {
      var st = Data.state();
      for (var i = 0; i < st.cyberPerks.length; i++) {
        if (st.cyberPerks[i].canBuy) { confirmPerk(st.cyberPerks[i], true); break; }
      }
    }
  }

  /* ═══════════════════ boot ═══════════════════ */
  function fail(message) {
    $('boot').hidden = false;
    var t = $('boot-text');
    t.className = 'boot__t is-bad';
    t.textContent = message + (Bridge.isNative() ? '  Press Esc to close.' : '');
  }

  /* Before the menu is up, Escape still has to close it: a boot failure used
     to leave a focused view that swallowed every key, with no way out. */
  function bootEscape(e) {
    if (keyOf(e) !== 'Escape' || $('boot').hidden) { return; }
    e.preventDefault();
    if (Bridge.isNative()) { Bridge.close(); }
  }

  function start() {
    Snd.install(document);
    Fx.tipInstall(document);
    document.addEventListener('keydown', bootEscape);
    Bridge.onError = function (m) { toast(m); };
    if (CW.Customize) { CW.Customize.onError = function (m) { toast(m); }; }
    Bridge.whenReady(function (t) {
      if (t === 'none') {
        fail('Cyberware.dll did not connect to this view (window.cwRequest never appeared). See Cyberware.log.');
        return;
      }
      boot();
    });
  }

  function boot() {

    Bridge.on('close', function () { /* the DLL hides the view; nothing to do */ });
    Bridge.on('talkPrompt', openPrompt);   // VARGA'S PROMPT: the DLL's own two-choice talk prompt
    Bridge.on('open', function (ev) {
      Data.absorb(ev);   // this listener runs before data.js's own: take the event's state/shop first (header said "No ripperdoc")
      ui.setup = !!(ev && ev.mode === 'setup');
      if (isSetup() && ui.filter === 'buy') { ui.filter = 'all'; syncTabs(); }
      Snd.play('menuOpen');
      Fx.replay($('cw'), 'fx-open', 500);
      if (!$('scrim').hidden) { closePop(true); }
      ui.pane = 'body'; renderAll();
      syncDetailToList();
      if (ev && ev.via === 'talk' && !isSetup()) { openGreeting(ev); }
    });

    Data.boot().then(function (info) {
      $('boot').hidden = true;
      wireInput();
      Data.subscribe(function () { /* absorbed state re-renders through commit() */ });
      // open on the first slot that has something installed, else the first slot
      var slots = Data.slots();
      var pick = null;
      for (var i = 0; i < slots.length && !pick; i++) {
        for (var k = 0; k < slots[i].installed.length; k++) {
          if (slots[i].installed[k]) { pick = slots[i].id; break; }
        }
      }
      ui.slot = pick || (slots[0] && slots[0].id) || null;
      ui.pane = 'body';
      syncTabs();
      renderAll();
      /* On desktop all three panes are on screen at once, so leaving the
         detail empty would be a third of the screen doing nothing. Open the
         first row of the chosen slot. On a phone only one pane shows, so the
         player still starts on the map. */
      if (ui.rowIds.length && !window.matchMedia('(max-width: 1499px)').matches) {
        ui.itemId = ui.rowIds[0];
        ui.pane = 'body';
        renderAll();
      }
      applyHarnessParams();
      window.addEventListener('resize', layoutPins);
      window.addEventListener('load', layoutPins);
      var scan = document.querySelector('.bodymap__scan');
      if (scan) {
        if (scan.complete) { layoutPins(); } else { scan.addEventListener('load', layoutPins); }
      }
      if (document.fonts && document.fonts.ready && document.fonts.ready.then) {
        document.fonts.ready.then(layoutPins);
      }
      if (!Bridge.isNative()) {
        toast('Harness mode: no plugin attached, the mock engine is answering.', true);
      }
    }, function (e) {
      fail((e && e.message) || 'Could not reach Cyberware.dll.');
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', start);
  } else { start(); }
})(this);
