/* data.js — the page's cache of what the DLL told it, plus lookups.
 *
 * Per UI-API.md the page holds NO game state of its own: it sends a request
 * and the response carries the whole new `state` (and `shop`), which replaces
 * the cache wholesale. Everything here is read-only derivation on top of that.
 * Display names, icons, tier colours and stat formats are NOT in the state --
 * they are looked up in the catalog by id, which is what this file is for.
 */
(function (global) {
  'use strict';

  var CW = global.CW = global.CW || {};
  var Bridge = CW.Bridge;

  var catalog = null, state = null, shop = null;
  var itemById = {}, modById = {}, perkById = {}, statById = {}, tierById = {}, slotById = {}, abilityById = {};
  var subs = [];

  function reindex() {
    itemById = {}; modById = {}; perkById = {}; statById = {}; tierById = {}; slotById = {}; abilityById = {};
    var i;
    for (i = 0; i < (catalog.items || []).length; i++) { itemById[catalog.items[i].id] = catalog.items[i]; }
    for (i = 0; i < (catalog.mods || []).length; i++) { modById[catalog.mods[i].id] = catalog.mods[i]; }
    for (i = 0; i < (catalog.cyberPerks || []).length; i++) { perkById[catalog.cyberPerks[i].id] = catalog.cyberPerks[i]; }
    for (i = 0; i < (catalog.stats || []).length; i++) { statById[catalog.stats[i].id] = catalog.stats[i]; }
    for (i = 0; i < (catalog.tiers || []).length; i++) { tierById[catalog.tiers[i].id] = catalog.tiers[i]; }
    for (i = 0; i < (catalog.slots || []).length; i++) { slotById[catalog.slots[i].id] = catalog.slots[i]; }
    for (i = 0; i < (catalog.abilities || []).length; i++) { abilityById[catalog.abilities[i].id] = catalog.abilities[i]; }
  }

  function absorb(res) {
    var changed = false;
    if (res && res.state) { state = res.state; changed = true; }
    if (res && res.shop) { shop = res.shop; changed = true; }
    if (changed) { notify(); }
    return res;
  }
  function notify() { for (var i = 0; i < subs.length; i++) { subs[i](); } }

  /* The path to a catalog item's or mod's icon. SPEC mapped the delivered
     filenames in by real CP2077 record (6e3b86e8), so this is now the
     catalog's own value; the UI's name-alias stopgap is gone. An entry with no
     icon returns '' and app.js falls back to that slot's glyph, which is why
     no row ever renders as an empty tile. */
  function iconPath(def) { return (def && def.icon) || ''; }

  /* The catalog's ripperdoc entry (name, talk lines), by id. */
  function ripperdoc(id) {
    var r = (catalog && catalog.ripperdocs) || [];
    for (var i = 0; i < r.length; i++) { if (r[i].id === id) { return r[i]; } }
    return null;
  }

  function boot() {
    return Bridge.hello().then(function (res) {
      var v = res.data && res.data.apiVersion;
      if (v !== Bridge.API_VERSION) {
        throw new Error('This menu speaks UI-API v' + Bridge.API_VERSION +
                        ' but Cyberware.dll reports v' + v + '. Update one of them.');
      }
      return Bridge.getCatalog();
    }).then(function (res) {
      catalog = res.data; reindex();
      return Bridge.getShop();      // returns ShopView AND sets state
    }).then(function (res) {
      shop = res.data || res.shop;
      if (res.state) { state = res.state; }
      if (!state) { return Bridge.getState().then(absorb); }
      return null;
    }).then(function () {
      Bridge.on('open', function (ev) { absorb(ev); });
      Bridge.on('state', function (ev) { absorb(ev); });
      notify();
      return { transport: Bridge.transport() };
    });
  }

  /* --------------------------------------------------------- formatting */

  function statLabel(id) { return (statById[id] && statById[id].name) || id; }

  function formatStat(id, value) {
    var def = statById[id];
    var n = Math.round(value * 100) / 100;
    var txt = (n > 0 ? '+' : '') + (n % 1 === 0 ? n : n.toFixed(2).replace(/0$/, ''));
    if (def && def.format) {
      // "+{v}%" -- the catalog owns the sign, so do not double it
      var f = n < 0 ? def.format.replace(/^\+/, '') : def.format;   // "+{v}" with -3 is "-3", never "+-3"
      return f.replace('{v}', String(n % 1 === 0 ? n : n.toFixed(2).replace(/0$/, '')));
    }
    return txt;
  }

  function tierName(t) { return (tierById[t] && tierById[t].name) || ('Tier ' + t); }
  function tierColor(t) { return (tierById[t] && tierById[t].color) || '#b0b0b0'; }

  function money(n) {
    var s = String(Math.abs(Math.round(n)));
    var out = '';
    while (s.length > 3) { out = ',' + s.slice(-3) + out; s = s.slice(0, -3); }
    return (n < 0 ? '-' : '') + s + out;
  }

  /* ---------------------------------------------------------- selectors */

  function slots() { return (state && state.slots) || []; }

  function slotState(slotId) {
    var ss = slots();
    for (var i = 0; i < ss.length; i++) { if (ss[i].id === slotId) { return ss[i]; } }
    return null;
  }

  function installedIn(slotId) {
    var s = slotState(slotId);
    if (!s) { return []; }
    var out = [];
    for (var i = 0; i < s.installed.length; i++) {
      out.push(s.installed[i] ? state.instances[s.installed[i]] : null);
    }
    return out;
  }

  /* Owned-but-not-installed pieces whose item belongs to this slot. */
  function stashFor(slotId) {
    var out = [];
    for (var i = 0; i < (state.stash || []).length; i++) {
      var inst = state.instances[state.stash[i]];
      if (inst && itemById[inst.item] && itemById[inst.item].slot === slotId) { out.push(inst); }
    }
    return out;
  }

  /* Every shop row for one item, cheapest tier first. */
  function shopRowsFor(itemId) {
    var out = [];
    for (var i = 0; i < ((shop && shop.items) || []).length; i++) {
      if (shop.items[i].item === itemId) { out.push(shop.items[i]); }
    }
    out.sort(function (a, b) { return a.tier - b.tier; });
    return out;
  }

  function shopModRowsFor(modId) {
    var out = [];
    for (var i = 0; i < ((shop && shop.mods) || []).length; i++) {
      if (shop.mods[i].mod === modId) { out.push(shop.mods[i]); }
    }
    out.sort(function (a, b) { return a.tier - b.tier; });
    return out;
  }

  /* The catalog items that belong to a slot, each with everything a row needs. */
  function catalogForSlot(slotId) {
    var out = [];
    for (var i = 0; i < catalog.items.length; i++) {
      var it = catalog.items[i];
      if (it.slot !== slotId) { continue; }
      out.push(rowFor(it));
    }
    return out;
  }

  function everyItemRow() {
    var out = [];
    for (var i = 0; i < catalog.items.length; i++) { out.push(rowFor(catalog.items[i])); }
    return out;
  }

  /* One browsable row: the catalog item plus this player's relationship to it. */
  function rowFor(it) {
    var rows = shopRowsFor(it.id);
    var owned = null, installed = null;
    for (var u in state.instances) {
      if (!state.instances.hasOwnProperty(u)) { continue; }
      var inst = state.instances[u];
      if (inst.item !== it.id) { continue; }
      if (inst.installed) { installed = inst; } else if (!owned) { owned = inst; }
    }
    var cheapest = null, best = null, locked = null;
    for (var r = 0; r < rows.length; r++) {
      // on sale = buyable now, or only short of caps (the DLL marks that row unavailable, the mock does not)
      var onSale = rows[r].available || rows[r].reason === 'E_NO_FUNDS';
      if (onSale && (!cheapest || rows[r].price < cheapest.price)) { cheapest = rows[r]; }
      if (rows[r].reason === 'E_LEVEL' && (!locked || rows[r].requiresLevel < locked.requiresLevel)) { locked = rows[r]; }
      if (!best || rows[r].tier > best.tier) { best = rows[r]; }
    }
    var afford = !!(cheapest && state.currency.amount >= cheapest.price);
    return {
      item: it,
      shopRows: rows,
      cheapest: cheapest,
      locked: cheapest ? null : locked,   // nothing on sale yet: the lowest level gate, for "Needs level N"
      topRow: best,
      installed: installed,
      owned: owned,
      affordable: afford,
      // the tier shown on an un-owned row is the cheapest one you could buy
      displayTier: (installed && installed.tier) || (owned && owned.tier) ||
                   (cheapest && cheapest.tier) || (rows[0] && rows[0].tier) || it.tiers[0].tier,
      capacityCost: (installed && installed.capacityCost) ||
                    (cheapest && cheapest.capacityCost) ||
                    (rows[0] && rows[0].capacityCost) || it.tiers[0].capacityCost
    };
  }

  /* Mods the player owns that could go in a given mod slot type. */
  function ownedModsOfType(type) {
    var out = [];
    for (var u in state.mods) {
      if (!state.mods.hasOwnProperty(u)) { continue; }
      if (state.mods[u].type === type) { out.push(state.mods[u]); }
    }
    return out;
  }

  /* Catalog mods of a type, with their shop rows -- the buy side of the picker. */
  function catalogModsOfType(type) {
    var out = [];
    for (var i = 0; i < catalog.mods.length; i++) {
      if (catalog.mods[i].type !== type) { continue; }
      var rows = shopModRowsFor(catalog.mods[i].id);
      var cheapest = null;
      for (var r = 0; r < rows.length; r++) {
        if (rows[r].available && (!cheapest || rows[r].price < cheapest.price)) { cheapest = rows[r]; }
      }
      out.push({ mod: catalog.mods[i], shopRows: rows, cheapest: cheapest });
    }
    return out;
  }

  function tierDef(itemDef, t) {
    for (var i = 0; i < itemDef.tiers.length; i++) {
      if (itemDef.tiers[i].tier === t) { return itemDef.tiers[i]; }
    }
    return null;
  }

  /* THE SENTENCE FOR ONE TIER of a piece or a mod. BETA QA 2's catalog
     (cw-tier-text) gives every tier of all 163 entries its own `description`,
     carrying THAT tier's numbers; `def.description` stays the T5 reference
     sentence and is the search hay. The fallback lives here, in one place, so
     a catalog without the per-tier field renders exactly as it does today --
     the same three-state discipline the Armory text migration needed. */
  function tierDesc(def, t) {
    var td = (def && def.tiers && t != null) ? tierDef(def, t) : null;
    return (td && td.description) || (def && def.description) || '';
  }

  /* AN ABILITY'S OWN SENTENCE AT A TIER. `descriptionByTier` exists only where
     an ability reads differently by tier (98 of 160) and is keyed by the tier
     as a STRING. Anything else keeps its single `description`. */
  function abilityDescAt(id, t) {
    var a = abilityById[id];
    if (!a) { return ''; }
    var by = a.descriptionByTier;
    if (by && t != null && by[String(t)]) { return by[String(t)]; }
    return a.description || '';
  }

  /* Which OTHER ripperdocs stock a piece, and where they are.
     A row the open doc does not sell is a dead end unless the menu says where
     to go instead ("give every unbuyable row its reason"), and the catalog the
     DLL hands us carries every doc's own stock list, so the answer is here. */
  function stockedElsewhere(itemId) {
    var out = [], docs = (catalog && catalog.ripperdocs) || [];
    var here = (state && state.ripperdoc) || (shop && shop.ripperdoc && shop.ripperdoc.id) || null;
    for (var i = 0; i < docs.length; i++) {
      var d = docs[i];
      if (d.disabled || d.id === here) { continue; }
      var rows = d.stock || [], tier = null;
      for (var r = 0; r < rows.length; r++) {
        if (rows[r].item !== itemId) { continue; }
        var ts = rows[r].tiers || [];
        for (var t = 0; t < ts.length; t++) { if (tier === null || ts[t] < tier) { tier = ts[t]; } }
        if (!ts.length) { tier = tier === null ? null : tier; }
      }
      if (tier !== null) { out.push({ id: d.id, name: d.name, place: d.place || null, tier: tier }); }
    }
    return out;
  }

  /* One sentence naming where a piece can be bought, or '' when nowhere does. */
  function stockedElsewhereLine(itemId) {
    var w = stockedElsewhere(itemId);
    if (!w.length) { return ''; }
    var bits = [];
    for (var i = 0; i < w.length; i++) {
      bits.push(w[i].name + (w[i].place ? ' in ' + w[i].place : ''));
    }
    var who = bits.length === 1 ? bits[0] : bits.slice(0, -1).join(', ') + ' and ' + bits[bits.length - 1];
    return who + (bits.length === 1 ? ' sells it.' : ' sell it.');
  }

  function abilityName(id) { return (abilityById[id] && abilityById[id].name) || id; }
  function abilityDesc(id) { return (abilityById[id] && abilityById[id].description) || ''; }

  CW.Data = {
    boot: boot,
    absorb: absorb,
    subscribe: function (fn) { subs.push(fn); },

    catalog: function () { return catalog; },
    state: function () { return state; },
    shop: function () { return shop; },

    item: function (id) { return itemById[id]; },
    mod: function (id) { return modById[id]; },
    perk: function (id) { return perkById[id]; },
    stat: function (id) { return statById[id]; },
    slotDef: function (id) { return slotById[id]; },
    tierDef: tierDef,
    tierDesc: tierDesc,

    iconPath: iconPath,
    ripperdoc: ripperdoc,
    ripperdocs: function () { return ((catalog && catalog.ripperdocs) || []).slice(); },
    stockedElsewhere: stockedElsewhere,
    stockedElsewhereLine: stockedElsewhereLine,

    statLabel: statLabel,
    formatStat: formatStat,
    tierName: tierName,
    tierColor: tierColor,
    money: money,
    abilityName: abilityName,
    abilityDesc: abilityDesc,
    abilityDescAt: abilityDescAt,

    slots: slots,
    slotState: slotState,
    installedIn: installedIn,
    stashFor: stashFor,
    shopRowsFor: shopRowsFor,
    shopModRowsFor: shopModRowsFor,
    catalogForSlot: catalogForSlot,
    everyItemRow: everyItemRow,
    rowFor: rowFor,
    ownedModsOfType: ownedModsOfType,
    catalogModsOfType: catalogModsOfType
  };
})(this);
