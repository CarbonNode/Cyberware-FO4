/* armory.js — the Armory tab (ARMORY stream, 2026-11-09; docs/ARMORY.md §5, UI-API.md buyGear).
 *
 * Renders into <main id="view-armory">. Data: shop.gear (ShopView, UI-API v1): plain inventory items from the
 * separate plugin "Cyberware - Armory.esp" that THIS doc sells. The tab exists only while shop.gear is non-empty
 * (Varga's shop, Armory plugin loaded), so no other doc and no player without the plugin ever sees it.
 *
 * app.js integration is one line in renderAllInner; index.html adds the tab, the <main>, the css and this file.
 * Keys come through app.js onKey (document capture): the search box exposes __cwKey; rows and buttons are
 * buttons, so arrows / Enter work like every other view.
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};

  /* The text for one gear row comes from the row itself (data/armory/armory.json -> armory-formmap.json's stock ->
     the DLL -> shop.gear): `description` is what it does, `lore` what it is (the lead's ruling 2026-11-10: one key name,
     `lore`, everywhere). There is no copy of the text in this file. A row with neither shows its name, kind and price
     and nothing invented. `desc` is ARMORY's older single mixed line, read as the description until the rows are split. */
  function copyOf(g) {
    return { what: String(g.description || g.desc || '').trim(), lore: String(g.lore || '').trim() };
  }
  var KINDS = [
    { id: 'all', label: 'All' },
    { id: 'weapon', label: 'Weapons' },
    { id: 'armor', label: 'Armour' },
    { id: 'consumable', label: 'Supplies' }
  ];
  var KIND_WORD = { weapon: 'Weapon', armor: 'Armour', consumable: 'Food & meds' };

  var ui = { q: '', kind: 'all', sel: null, qty: 1, busy: false, ledger: null, err: null, lastFor: null };

  function $(id) { return document.getElementById(id); }
  function esc(s) { return String(s).replace(/[&<>"]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]; }); }
  function caps(n) { return String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ','); }
  function gear() { var s = CW.Data && CW.Data.shop && CW.Data.shop(); return (s && s.gear) || []; }
  function have() { var st = CW.Data && CW.Data.state && CW.Data.state(); return (st && st.currency && st.currency.amount) || 0; }
  function sound(n) { if (CW.Bridge && CW.Bridge.sound) { CW.Bridge.sound(n); } }

  function visible() {
    var q = ui.q.toLowerCase(), out = [], g = gear();
    for (var i = 0; i < g.length; i++) {
      if (ui.kind !== 'all' && g[i].kind !== ui.kind) { continue; }
      if (q && (g[i].name + ' ' + (KIND_WORD[g[i].kind] || '') + ' ' + copyOf(g[i]).what + ' ' + copyOf(g[i]).lore).toLowerCase().indexOf(q) === -1) { continue; }
      out.push(g[i]);
    }
    return out;
  }
  function find(id) { var g = gear(); for (var i = 0; i < g.length; i++) { if (g[i].gear === id) { return g[i]; } } return null; }
  function kindsPresent() {
    var g = gear(), seen = {}, out = [];
    for (var i = 0; i < g.length; i++) { seen[g[i].kind] = true; }
    for (var k = 0; k < KINDS.length; k++) { if (KINDS[k].id === 'all' || seen[KINDS[k].id]) { out.push(KINDS[k]); } }
    return out;
  }

  /* The header tab: shown only while this doc sells gear; the section control widens to fit it. */
  function syncTab() {
    var tab = document.querySelector('#view-tabs [data-v="armory"]');
    if (!tab) { return; }
    var on = gear().length > 0;
    tab.hidden = !on;
    /* a doc with no gear (or the plugin unloaded) while the Armory was open: back to Chrome */
    if (!on && tab.classList.contains('is-on')) { var chrome = document.querySelector('#view-tabs [data-v="chrome"]'); if (chrome) { chrome.click(); } }
    var bar = $('view-tabs'), n = 0, cells = bar.querySelectorAll('.seg__c');
    for (var i = 0; i < cells.length; i++) { if (!cells[i].hidden) { n++; } }
    bar.dataset.cells = String(n);   // armory.css lays out 5 cells (one row on desktop, 3 + 2 on a phone)
  }

  /* dryRun the purchase so the ledger shows the real engine numbers (UI-API §5) */
  function preview() {
    var g = ui.sel && find(ui.sel);
    if (!g || !CW.Bridge) { ui.ledger = null; return; }
    var key = g.gear + 'x' + ui.qty;
    if (ui.lastFor === key) { return; }
    ui.lastFor = key;
    CW.Bridge.request('buyGear', { gear: g.gear, count: ui.qty }, true).then(function (res) {
      if (ui.lastFor !== key) { return; }
      var d = (res && res.data && res.data.currencyDelta) || 0;
      ui.ledger = { from: have(), to: have() + d, cost: -d };
      ui.err = null;
      paintDetail();
    }, function (e) {
      if (ui.lastFor !== key) { return; }
      ui.ledger = null;
      ui.err = (e && e.message) || 'Not available';
      paintDetail();
    });
  }

  function buy() {
    var g = ui.sel && find(ui.sel);
    if (!g || ui.busy || !CW.Bridge) { return; }
    ui.busy = true; paintDetail();
    CW.Bridge.request('buyGear', { gear: g.gear, count: ui.qty }).then(function (res) {
      if (CW.Data && CW.Data.absorb) { CW.Data.absorb(res); }
      sound('UIMenuOK');
      ui.busy = false; ui.lastFor = null;
      ui.flash = (ui.qty > 1 ? ui.qty + ' x ' : '') + g.name + ' is in your pack.';
      return CW.Bridge.getShop().then(function (r) {   // prices / affordability after the caps moved
        if (r && r.data && CW.Data.absorb) { CW.Data.absorb({ shop: r.data, state: r.state }); }
      }).then(function () { render(); var bb = $('arm-buy'); if (bb) { bb.focus(); } });
    }, function (e) {
      ui.busy = false;
      ui.err = (e && e.message) || 'The purchase failed';
      sound('UIMenuCancel');
      paintDetail();
    });
  }

  function rowHtml(g, i) {
    var on = g.gear === ui.sel;
    return '<li class="arm-row-wrap" style="--i:' + i + '"><button type="button" class="arm-row' + (on ? ' is-on' : '') +
           (g.available ? '' : ' is-short') + '" data-gear="' + esc(g.gear) + '" aria-pressed="' + (on ? 'true' : 'false') + '">' +
           '<span class="arm-row__name">' + esc(g.name) + '</span>' +
           '<span class="arm-row__kind">' + esc(KIND_WORD[g.kind] || g.kind) + '</span>' +
           '<span class="arm-row__price">' + caps(g.price) + '<span class="arm-row__cur"> caps</span></span>' +
           '</button></li>';
  }

  function paintList() {
    var list = $('arm-list'), none = $('arm-none');
    if (!list) { return; }
    var rows = visible(), html = [];
    for (var i = 0; i < rows.length; i++) { html.push(rowHtml(rows[i], i)); }
    list.innerHTML = html.join('');
    none.hidden = rows.length > 0;
    none.textContent = ui.q ? 'Nothing here matches "' + ui.q + '".' : 'Nothing of this kind on the shelf today.';
    var b = list.querySelectorAll('.arm-row');
    for (var j = 0; j < b.length; j++) {
      (function (btn) {
        btn.addEventListener('click', function () { select(btn.dataset.gear); });
        btn.addEventListener('focus', function () { if (ui.sel !== btn.dataset.gear) { select(btn.dataset.gear, true); } });
      })(b[j]);
    }
  }

  function paintDetail() {
    var box = $('arm-detail');
    if (!box) { return; }
    var g = ui.sel && find(ui.sel);
    if (!g) {
      box.innerHTML = '<div class="arm-detail__empty"><p class="arm-detail__hint">Pick something on the left.</p>' +
                      '<p class="arm-detail__sub">Everything here is paid for in caps and goes straight into your pack.</p></div>';
      return;
    }
    var multi = g.kind === 'consumable';
    var total = g.price * ui.qty;
    var short = have() < total;
    var html = ['<h3 class="arm-detail__name">' + esc(g.name) + '</h3>',
                '<span class="arm-detail__kind">' + esc(KIND_WORD[g.kind] || g.kind) + '</span>'];
    var words = copyOf(g);
    if (words.what) { html.push('<p class="arm-detail__copy">' + esc(words.what) + '</p>'); }
    if (words.lore) { html.push('<p class="arm-detail__copy arm-detail__lore">' + esc(words.lore) + '</p>'); }
    if (multi) {
      html.push('<div class="arm-qty" role="group" aria-label="Quantity">' +
                '<button type="button" class="arm-qty__b" data-q="-1" aria-label="One fewer"' + (ui.qty <= 1 ? ' disabled' : '') + '>&#8722;</button>' +
                '<span class="arm-qty__v" aria-live="polite">' + ui.qty + '</span>' +
                '<button type="button" class="arm-qty__b" data-q="1" aria-label="One more"' + (ui.qty >= 20 ? ' disabled' : '') + '>+</button></div>');
    }
    var led = ui.ledger;
    html.push('<dl class="arm-ledger">' +
              '<div class="arm-ledger__r"><dt>Price</dt><dd>' + caps(total) + ' caps</dd></div>' +
              '<div class="arm-ledger__r"><dt>Your caps</dt><dd>' + caps(have()) + ' &#8594; <b class="' + (short ? 'is-bad' : 'is-good') + '">' +
              caps(led ? led.to : have() - total) + '</b></dd></div></dl>');
    if (ui.err) { html.push('<p class="arm-msg is-bad" role="alert">' + esc(ui.err) + '</p>'); }
    else if (ui.flash) { html.push('<p class="arm-msg is-good" role="status">' + esc(ui.flash) + '</p>'); }
    html.push('<button type="button" class="arm-buy" id="arm-buy"' + (short || ui.busy ? ' disabled' : '') + '>' +
              (ui.busy ? 'Paying&#8230;' : (short ? 'Not enough caps' : 'Buy for ' + caps(total) + ' caps')) + '</button>');
    box.innerHTML = html.join('');
    var bb = $('arm-buy');
    if (bb) { bb.addEventListener('click', buy); }
    var qb = box.querySelectorAll('.arm-qty__b');
    for (var i = 0; i < qb.length; i++) {
      (function (btn) {
        btn.addEventListener('click', function () {
          ui.qty = Math.max(1, Math.min(20, ui.qty + parseInt(btn.dataset.q, 10)));
          ui.flash = null; ui.err = null; sound('UIMenuPrevNext');
          paintDetail(); preview();
          var again = box.querySelector('.arm-qty__b[data-q="' + btn.dataset.q + '"]');
          if (again && !again.disabled) { again.focus(); } else { var bz = $('arm-buy'); if (bz) { bz.focus(); } }
        });
      })(qb[i]);
    }
  }

  function select(id, keepFocus) {
    if (ui.sel !== id) { ui.qty = 1; ui.flash = null; }
    ui.sel = id; ui.err = null; ui.ledger = null; ui.lastFor = null;
    var rows = document.querySelectorAll('#arm-list .arm-row');
    for (var i = 0; i < rows.length; i++) {
      var on = rows[i].dataset.gear === id;
      rows[i].classList.toggle('is-on', on);
      rows[i].setAttribute('aria-pressed', on ? 'true' : 'false');
    }
    if (!keepFocus) { sound('UIMenuFocus'); }
    paintDetail(); preview();
  }

  function paintKinds() {
    var seg = $('arm-kinds');
    if (!seg) { return; }
    var ks = kindsPresent();
    if (ks.length <= 2) { seg.hidden = true; ui.kind = 'all'; return; }   // one kind on the shelf: nothing to filter by
    seg.hidden = false;
    seg.style.gridTemplateColumns = 'repeat(' + ks.length + ', minmax(0, 1fr))';
    var html = [];
    for (var i = 0; i < ks.length; i++) {
      var on = ks[i].id === ui.kind;
      html.push('<button type="button" class="seg__c' + (on ? ' is-on' : '') + '" role="tab" aria-selected="' + (on ? 'true' : 'false') +
                '" data-kind="' + ks[i].id + '">' + ks[i].label + '</button>');
    }
    seg.innerHTML = html.join('');
    var b = seg.querySelectorAll('.seg__c');
    for (var j = 0; j < b.length; j++) {
      (function (btn) {
        btn.addEventListener('click', function () { ui.kind = btn.dataset.kind; sound('UIMenuPrevNext'); paintKinds(); paintList(); });
      })(b[j]);
    }
  }

  function build(root) {
    root.innerHTML =
      '<div class="armwrap">' +
        '<header class="arm-head">' +
          '<h2 class="pane__h">Armory</h2>' +
          '<p class="arm-head__lede">Kept behind the door, off the books. To anyone without a Smart Link half of this is dead iron.</p>' +
        '</header>' +
        '<div class="arm-tools">' +
          '<label class="srch srch--arm">' +
            '<span class="srch__glyph" aria-hidden="true"><svg viewBox="0 0 24 24" focusable="false" aria-hidden="true"><circle cx="10.5" cy="10.5" r="6.5"/><path d="M15.5 15.5 L21 21"/></svg></span>' +
            '<input type="search" id="find-armory" class="srch__input" placeholder="Search the armory…" autocomplete="off" spellcheck="false" aria-label="Search the armory">' +
            '<button type="button" class="srch__clear" id="find-armory-clear" aria-label="Clear search" hidden>&#215;</button>' +
          '</label>' +
          '<div class="seg arm-kinds" role="tablist" aria-label="Kind" id="arm-kinds"></div>' +
        '</div>' +
        '<div class="arm-body">' +
          '<section class="arm-listpane" aria-label="For sale"><ul class="arm-list" id="arm-list"></ul><p class="rows__none" id="arm-none" hidden></p></section>' +
          '<section class="arm-detail" id="arm-detail" aria-live="polite"></section>' +
        '</div>' +
      '</div>';
    var input = $('find-armory'), clr = $('find-armory-clear');
    input.addEventListener('input', function () { ui.q = input.value.trim(); clr.hidden = !input.value; paintList(); });
    /* app.js onKey hands keys to a focused input through __cwKey */
    input.__cwKey = function (k) {
      if (k === 'Enter' || k === 'ArrowDown') {
        var first = document.querySelector('#arm-list .arm-row');
        if (!first) { return true; }
        if (k === 'Enter') { select(first.dataset.gear, true); }
        first.focus();
        return true;
      }
      if (k === 'Escape' && input.value) { input.value = ''; ui.q = ''; clr.hidden = true; paintList(); return true; }
      return false;
    };
    clr.addEventListener('click', function () { input.value = ''; ui.q = ''; clr.hidden = true; paintList(); input.focus(); });
    root.dataset.built = '1';
  }

  function render() {
    var root = $('view-armory');
    if (!root) { return; }
    if (!root.dataset.built) { build(root); }
    if (ui.sel && !find(ui.sel)) { ui.sel = null; }
    if (!ui.sel) { var v = visible(); if (v.length) { ui.sel = v[0].gear; } }
    paintKinds(); paintList(); paintDetail(); preview();
  }

  CW.Armory = { render: render, syncTab: syncTab };

  if (global.document) {
    document.addEventListener('DOMContentLoaded', function () {
      if (CW.Data && CW.Data.subscribe) { CW.Data.subscribe(syncTab); }
      syncTab();
    });
  }
})(this);
