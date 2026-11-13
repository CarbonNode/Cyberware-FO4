#!/usr/bin/env python3
"""Build fallout/cyberware/data/cyberware.json (FRAMEWORK catalog schema v1) from tools/catalog_data.py.

    python3 -I fallout/cyberware/tools/build_catalog.py           # build + validate
    python3 -I fallout/cyberware/tools/build_catalog.py --check   # fail if outputs are stale

It also rewrites the generated mapping section of docs/DESIGN.md (between the
BEGIN/END GENERATED markers). The JSON is the contract FRAMEWORK and UI load;
see docs/DESIGN.md section "Catalog format" for the field reference.
"""

import json
import math
import os
import sys

# Never write .pyc for the catalog sources: an edit that keeps a file's size inside the same second leaves a stale
# catalog_data.pyc that Python still trusts, and the catalog is then built from the OLD text (seen 2026-11-10).
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import catalog_data as D  # noqa: E402
import to_schema  # noqa: E402
import player_text  # noqa: E402
import tier_ladder  # noqa: E402
import tier_text  # noqa: E402

JSON_OUT = os.path.join(ROOT, 'data', 'cyberware.json')
HANDLERS_OUT = os.path.join(ROOT, 'data', 'native-handlers.json')
PLACEMENT_OUT = os.path.join(ROOT, 'data', 'ripperdoc-placement.json')
DESIGN = os.path.join(ROOT, 'docs', 'DESIGN.md')
BEGIN = '<!-- BEGIN GENERATED MAPPING (tools/build_catalog.py) -->'
END = '<!-- END GENERATED MAPPING -->'

SCHEMA_VERSION = 1


def _cp_capacity():
    """item id -> CP2077 2.x capacity (Humanity) cost, exact from TweakDB (BETA QA, lead-approved
    2026-11-08: capacityCost is FLAT across tiers and CP-exact; the wiki-read cp_capacity_t5 stays
    as the min-tier / base-price anchor so tier ladders and prices do not shift).
    Sources: data/qa/cp_dump.json (record per item/tier) + data/qa/cp_humanity.json (.tweak sources)."""
    try:
        dump = json.load(open(os.path.join(ROOT, 'data', 'qa', 'cp_dump.json')))
        hum = json.load(open(os.path.join(ROOT, 'data', 'qa', 'cp_humanity.json')))
    except FileNotFoundError:
        return {}
    out = {}
    for iid, v in dump['items'].items():
        vals = []
        for rec in v['tiers'].values():
            h = (hum.get(rec['record'].split('.', 1)[1]) or {}).get('humanity')
            if h is not None:
                vals.append(h)
        if vals:
            out[iid] = max(vals)  # flat in CP for every item; max() guards a partial parse
    return out


CP_CAPACITY = _cp_capacity()

TIERS = [
    # id, level gate, capacity factor, effect factor, price factor, upgrade components
    ('T1', 1, 0.55, 0.40, 1.0, {}),
    ('T2', 10, 0.65, 0.55, 1.8, {'c_circuitry': 2, 'c_copper': 4}),
    ('T3', 20, 0.75, 0.70, 3.0, {'c_circuitry': 4, 'c_fiber_optics': 2}),
    ('T4', 30, 0.88, 0.85, 4.5, {'c_circuitry': 6, 'c_fiber_optics': 4, 'c_nuclear_material': 2}),
    ('T5', 40, 1.00, 1.00, 6.5, {'c_fiber_optics': 6, 'c_nuclear_material': 4, 'c_crystal': 3}),
    ('T5+', 45, 1.00, 1.10, 8.0, {'c_nuclear_material': 6, 'c_crystal': 5, 'cw_military_chip': 1}),
    ('T5++', 50, 1.00, 1.20, 10.0, {'c_nuclear_material': 8, 'c_crystal': 8, 'cw_military_chip': 2}),
]
TIER_IDS = [t[0] for t in TIERS]
assert [t[3] for t in TIERS] == tier_ladder.TIER_EFFECT, 'tier_ladder.TIER_EFFECT must equal the effect factors in TIERS'
assert {i + 1: t[3] for i, t in enumerate(TIERS)} == D.ARM_TIER_FACTOR, \
    'catalog_data.ARM_TIER_FACTOR must equal the effect factors in TIERS (arm weapon damage by tier)'
KINDS = {'passive', 'trigger', 'active', 'weapon'}
FEAS = {'vanilla-data', 'vanilla-script', 'needs-f4se', 'needs-custom-anims', 'not-feasible'}
CONF = {'wiki', 'wiki-partial', 'recall', 'legacy-1.x'}
ATTUNE = {'body', 'reflexes', 'technical', 'intelligence', 'cool', 'random'}
ATTUNE_SPECIAL = {'body': 'Strength', 'reflexes': 'Agility', 'technical': 'Intelligence',
                  'intelligence': 'Perception', 'cool': 'Luck', 'random': None}


def round_to(x, step):
    return int(step * round(float(x) / step))


def min_tier(item):
    if item.get('iconic'):
        return 'T5'
    if item['kind'] == 'weapon':
        return 'T1'
    if item.get('os_type') == 'cyberdeck':
        return {2: 'T1', 3: 'T2', 4: 'T3', 5: 'T4'}.get(item['params']['qh_slots'], 'T5')
    cap = item['cp_capacity_t5']
    if cap >= 35:
        return 'T4'
    if cap >= 20:
        return 'T3'
    if cap >= 10:
        return 'T2'
    return 'T1'


def base_price(item):
    if item['kind'] == 'weapon':
        p = 1500 if not item.get('variant_of') else 2000
    elif item.get('os_type') == 'cyberdeck':
        p = 600 + 400 * item['params']['qh_slots']
    elif item.get('os_type') in ('sandevistan', 'berserk'):
        p = 1500 + 60 * item['cp_capacity_t5']
    else:
        p = 150 + 40 * max(item['cp_capacity_t5'], 2)
    if item.get('iconic'):
        p *= 2
    return round_to(p, 50)


def build_item(raw):
    it = dict(raw)
    sysid = it.pop('system')
    first = min_tier(it)
    tiers = TIER_IDS[TIER_IDS.index(first):]
    bp = base_price(it)
    cap5 = it.pop('cp_capacity_t5')
    by_tier = {}
    for tid, lvl, capf, efff, pricef, comps in TIERS:
        if tid not in tiers:
            continue
        cp_cap = CP_CAPACITY.get(raw['id'])
        by_tier[tid] = {
            'level': lvl,
            # CP 2.x capacity cost is flat across tiers (TweakDB, all 115 items); use the exact
            # value. Items without a CP record (FO4 originals) are flat too, at their stated cost.
            'capacity': cp_cap if cp_cap is not None else cap5,
            'effect_scale': efff,
            'price_caps': round_to(bp * pricef, 50),
            'upgrade_components': comps if tid != first else {},
        }
    conf = it.pop('confidence')
    out = {
        'id': it.pop('id'),
        'name': it.pop('name'),
        'system': sysid,
        'kind': it.pop('kind'),
        'activation': {'passive': 'always', 'trigger': 'condition', 'active': 'hotkey',
                       'weapon': 'equip'}[raw['kind']],
        'tiers': tiers,
        'iconic': bool(it.pop('iconic', False)),
        'optional': bool(it.pop('optional', False)),
        'cp2077': {
            'effect_t5': it.pop('cp_effect'),
            'capacity_t5': cap5,
            'source': {'url': it.pop('source_url', D.SRC.get(sysid if sysid != 'arms' else 'arms')),
                       'confidence': conf},
        },
        'fo4': {
            'mechanism': it.pop('fo4_mechanism'),
            'effect_t5': it.pop('fo4_effect'),
            'params_t5': it.pop('params'),
            'feasibility': it.pop('feasibility'),
        },
        'attunement': it.pop('attunement'),
        'by_tier': by_tier,
        'capacity': by_tier['T5']['capacity'] if 'T5' in by_tier else None,
        'price_caps': by_tier[first]['price_caps'],
        'level_gate': by_tier[first]['level'],
        'asset_hints': {
            'cp_search': it.pop('asset_hint', None) or ('icon only (passive, no mesh): UI icon from the '
                                                        'cyberware inkatlas for "%s"' % raw['name']),
            'visible_mesh': raw['kind'] == 'weapon' or sysid in ('face',) and 'kiroshi' in raw['id'],
        },
    }
    pbt = it.pop('params_by_tier', None)
    if pbt:
        out['fo4']['params_by_tier'] = pbt
        if 5 in pbt: out['fo4']['params_t5'] = pbt[5]   # what the mapping tables and descriptions show
    for key in ('substitute', 'notes', 'os_type', 'anims', 'mod_slots', 'variant_of', 'weapon_record', 'lore'):
        val = it.pop(key, None)
        if val is not None:
            (out['fo4'] if key in ('substitute', 'notes') else out)[key] = val
    if it:
        raise SystemExit('unknown fields on %s: %s' % (out['id'], sorted(it)))
    return out


def build():
    systems = [{'id': s, 'name': n, 'slots': sl,
                'extra_slots': ({'source': ex[0], 'count': ex[1]} if ex else None),
                'fo4_anchor': anc, 'notes': no} for s, n, sl, ex, anc, no in D.SYSTEMS]
    items = [build_item(r) for r in D.ITEMS]
    mods = [{'id': i, 'name': n, 'applies_to': a, 'slot': s, 'effect': e, 'rarity': r,
             'confidence': c, 'fo4': ('OMOD on the arm weapon' if a in ('mantis', 'gorilla', 'monowire', 'pls')
                                      else 'misc item slotted at the ripperdoc; applied by script'),
             'price_caps': {'rare': 600, 'epic': 1500, 'legendary': 4000}[r],
             **({'lore': D.MOD_LORE[i].strip()} if (D.MOD_LORE.get(i) or '').strip() else {})}
            for i, n, a, s, e, r, c in D.MODS]
    perks = [{'id': i, 'name': n, 'cp_tree': t, 'effect': e, 'prereq': p, 'level': lv,
              'price_caps': pc, 'confidence': c} for i, n, t, e, p, lv, pc, c in D.PERKS]
    qhs = [{'id': i, 'name': n, 'ram': r, 'kind': k, 'effect': e, 'mechanism': m, 'feasibility': f, 'upload_s': D.QH_UPLOAD_S[i]}
           for i, n, r, k, e, m, f in D.QUICKHACKS if i in D.QH_UPLOAD_S]
    for q in qhs:   # a sentence that depends on a gated key says what the SHIPPED ini applies (catalog_data.QH_GATED_TEXT)
        g = D.QH_GATED_TEXT.get(q['id'])
        if not g:
            continue
        sec, key = g['key']
        off, on = q['effect'], g['on']   # the QUICKHACKS row is the OFF sentence
        said = [w for w in g['promises'] if w.lower() in off.lower()]
        if said:
            raise SystemExit('ERROR quickhack %s: its OFF sentence %r promises %s, which the build only does with [%s] %s=1. '
                             'Player text states what Cyberware.ini\'s default applies.' % (q['id'], off, ', '.join(said), sec, key))
        if not any(w.lower() in on.lower() for w in g['promises']):
            raise SystemExit('ERROR quickhack %s: its ON sentence %r states none of %s' % (q['id'], on, ', '.join(g['promises'])))
        shipped = shipped_ini_int(sec, key)
        q['effect'] = on if shipped else off
        q['effectWhen'] = {'key': '[%s] %s' % (sec, key), 'shipped': shipped, 'off': off, 'on': on}
    pools = {k: [{'stat': s, 'min': lo, 'max': hi} for s, lo, hi in v] for k, v in D.STAT_POOLS.items()}
    doc = {
        'schema_version': SCHEMA_VERSION,
        'title': 'Fallout 4 Cyberware catalog (personal port of Cyberpunk 2077 2.x cyberware)',
        'private': 'Carries data derived from Cyberpunk 2077. Read docs/DESIGN.md (Licensing) before distributing this file or the mod.',
        'generated_by': 'fallout/cyberware/tools/build_catalog.py from tools/catalog_data.py',
        'cp_sources_note': ('CP2077 numbers come from cyberpunk2077.wiki.fextralife.com Tier 5 pages, read '
                            '2026-11-08 (fandom returned HTTP 402). cp2077.source.confidence says how sure each row is.'),
        'tiers': [{'id': t, 'level': l, 'capacity_factor': c, 'effect_scale': e, 'price_factor': p,
                   'upgrade_components': comp} for t, l, c, e, p, comp in TIERS],
        'systems': systems,
        'attunement_special': ATTUNE_SPECIAL,
        'stat_rolls': {
            'rolls_by_tier': {'T1': 1, 'T2': 2, 'T3': 2, 'T4': 2, 'T5': 2, 'T5+': 2, 'T5++': 2},
            'extra_roll_perk': 'perk_driver_update',
            'reroll_perk': 'perk_chipware_connoisseur',
            'reroll_price_factor': 0.25,
            'scale': 'roll magnitude x tier effect_scale x (1 + 0.02 x attuned SPECIAL)',
            'pools': pools,
        },
        'capacity': {
            'base': 60, 'per_level': 3, 'per_level_cap_level': 80, 'per_endurance': 5,
            'shard_item': 'cw_neural_matrix', 'shard_value': 6, 'shard_max': 10,
            'overflow_perk': 'perk_edgerunner', 'overflow_max_pct': 50, 'overflow_hp_pct_per_point': 0.5,
        },
        'custom_actor_values': [
            'CW_Capacity', 'CW_CapacityMax', 'CW_RAM', 'CW_RAMMax', 'CW_RAMRegen', 'CW_QuickhackDamageMult',
            'CW_QuickhackUploadMult', 'CW_CooldownMult', 'CW_MitigationChance', 'CW_MitigationStrength',
            'CW_CritChance', 'CW_CritDamage', 'CW_Overflow', 'CW_Humanity'],
        'items': items,
        'mods': mods,
        'perks': perks,
        'quickhacks': qhs,
        'finishes': [
            {'id': 'fin_chrome', 'name': 'Bare Chrome', 'applies_to': 'visible'},
            {'id': 'fin_matte_black', 'name': 'Matte Black', 'applies_to': 'visible'},
            {'id': 'fin_militech', 'name': 'Militech Olive', 'applies_to': 'visible'},
            {'id': 'fin_arasaka', 'name': 'Arasaka Red/Black', 'applies_to': 'visible'},
            {'id': 'fin_kang_tao', 'name': 'Kang Tao Gold', 'applies_to': 'visible'},
            {'id': 'fin_vault_tec', 'name': 'Vault-Tec Blue/Yellow (FO4 flavour)', 'applies_to': 'visible'},
            {'id': 'fin_realskinn', 'name': 'Realskinn cover (skin-tone sleeve)', 'applies_to': 'arms'},
            {'id': 'fin_hidden', 'name': 'Hidden (no mesh)', 'applies_to': 'visible'},
        ],
        'eye_glow_colors': ['none', 'kiroshi_yellow', 'cyan', 'red', 'green', 'violet', 'white'],
    }
    return doc


def shipped_ini_int(sec, key, default=0):
    """The value a PLAYER's build reads: framework/plugin/Cyberware.ini, or `default` when the file does not name the key."""
    cur = None
    with open(os.path.join(ROOT, 'framework', 'plugin', 'Cyberware.ini'), encoding='utf-8', errors='replace') as f:
        for line in f:
            s = line.strip()
            if s.startswith('[') and s.endswith(']'):
                cur = s[1:-1]
            elif cur == sec and not s.startswith(';') and '=' in s and s.split('=', 1)[0].strip() == key:
                try:
                    return int(s.split('=', 1)[1].split(';')[0].strip(), 0)
                except ValueError:
                    return default
    return default


def validate(doc):
    errs = []
    sys_ids = {s['id'] for s in doc['systems']}
    ids = [i['id'] for i in doc['items']]
    dup = {i for i in ids if ids.count(i) > 1}
    if dup:
        errs.append('duplicate item ids: %s' % sorted(dup))
    idset = set(ids)
    for it in doc['items']:
        w = it['id']
        if it['system'] not in sys_ids:
            errs.append('%s: unknown system %s' % (w, it['system']))
        if it['kind'] not in KINDS:
            errs.append('%s: bad kind' % w)
        if it['fo4']['feasibility'] not in FEAS:
            errs.append('%s: bad feasibility' % w)
        if it['fo4']['feasibility'] == 'not-feasible' and not it['fo4'].get('substitute'):
            errs.append('%s: not-feasible without substitute' % w)
        if it['cp2077']['source']['confidence'] not in CONF:
            errs.append('%s: bad confidence' % w)
        if it['attunement'] not in ATTUNE:
            errs.append('%s: bad attunement' % w)
        if not it['cp2077']['source']['url']:
            errs.append('%s: no source url' % w)
        if it['kind'] in ('active', 'weapon') and not it.get('anims'):
            errs.append('%s: active/weapon item without an anims list' % w)
        if it.get('variant_of') and it['variant_of'] not in idset:
            errs.append('%s: variant_of %s missing' % (w, it['variant_of']))
        req = it['fo4']['params_t5'].get('requires')
        if req and req not in idset:
            errs.append('%s: requires %s missing' % (w, req))
        if not it['tiers'] or any(t not in TIER_IDS for t in it['tiers']):
            errs.append('%s: bad tiers' % w)
    for sid in sys_ids:
        if not any(i['system'] == sid for i in doc['items']):
            errs.append('system %s has no items' % sid)
    targets = {'mantis', 'gorilla', 'monowire', 'pls', 'optics', 'sandevistan', 'berserk'}
    mids = [m['id'] for m in doc['mods']]
    if len(mids) != len(set(mids)):
        errs.append('duplicate mod ids')
    for m in doc['mods']:
        if m['applies_to'] not in targets:
            errs.append('%s: bad applies_to' % m['id'])
    pids = {p['id'] for p in doc['perks']}
    for p in doc['perks']:
        if p['prereq'] and p['prereq'] not in pids:
            errs.append('%s: prereq missing' % p['id'])
    for s in doc['systems']:
        ex = s['extra_slots']
        if ex and ex['source'].startswith('perk_') and ex['source'] not in pids:
            errs.append('system %s: extra-slot perk %s missing' % (s['id'], ex['source']))
    for k in doc['stat_rolls']['pools']:
        if k not in ATTUNE:
            errs.append('pool %s unknown' % k)
    return errs


def mapping_markdown(doc):
    sysname = {s['id']: s['name'] for s in doc['systems']}
    lines = [BEGIN, '',
             '_Generated from `data/cyberware.json`; edit `tools/catalog_data.py` and rebuild. '
             'Effects are the Tier 5 values; lower tiers scale by `effect_scale` (T1 0.40 ... T5++ 1.20). '
             '"CP src" = how sure the CP2077 column is (wiki / wiki-partial / recall / legacy-1.x)._', '']
    for s in doc['systems']:
        rows = [i for i in doc['items'] if i['system'] == s['id']]
        lines += ['#### %s (%d slot%s%s)' % (
            sysname[s['id']], s['slots'], '' if s['slots'] == 1 else 's',
            ', +%d from `%s`' % (s['extra_slots']['count'], s['extra_slots']['source']) if s['extra_slots'] else ''),
            '',
            '| id | Name | Kind | CP2077 effect (T5) | FO4 mechanism | FO4 effect (T5) | Feasibility | Cap T5 | Caps (from) | Min tier / lvl | CP src |',
            '|---|---|---|---|---|---|---|---|---|---|---|']
        for i in rows:
            fe = i['fo4']['feasibility']
            if i['fo4'].get('substitute'):
                fe += ' (sub: %s)' % i['fo4']['substitute']
            lines.append('| `%s` | %s%s | %s | %s | %s | %s | %s | %s | %s | %s / %d | %s |' % (
                i['id'], i['name'], ' (iconic)' if i['iconic'] else '', i['kind'],
                i['cp2077']['effect_t5'].replace('|', '/'), ', '.join(i['fo4']['mechanism']),
                i['fo4']['effect_t5'].replace('|', '/'), fe,
                i['capacity'] if i['capacity'] is not None else '-', i['price_caps'],
                i['tiers'][0], i['level_gate'], i['cp2077']['source']['confidence']))
        lines.append('')
    lines += ['#### Animations each active piece needs (for ARMS / FRAMEWORK)', '',
              '| id | Name | Animations | Note |', '|---|---|---|---|']
    for i in doc['items']:
        if i.get('anims') and not i.get('variant_of'):
            lines.append('| `%s` | %s | %s | %s |' % (
                i['id'], i['name'], '; '.join(i['anims']),
                (i['fo4'].get('notes') or '').replace('|', '/') if i['kind'] == 'weapon' else
                (i['fo4'].get('substitute') or '')))
    lines += ['', '_Electrifying / Thermal / Toxic are damage-type OMODs on the same weapon (one WEAP per arm), '
              'so they share its animation set; only the material swap, enchantment and hit FX differ._', '']
    lines.append(END)
    return '\n'.join(lines)


def counts(doc):
    c = {}
    for i in doc['items']:
        c[i['fo4']['feasibility']] = c.get(i['fo4']['feasibility'], 0) + 1
    return c


def main():
    check = '--check' in sys.argv
    doc = build()
    errs = validate(doc)
    if errs:
        print('\n'.join('ERROR ' + e for e in errs))
        return 1
    raw = to_schema.to_schema(doc, D)
    before = tier_ladder.snapshot(raw, tier_text.tier_values)   # the catalog as it was: the engine's old tier/5 rule
    perk = json.load(open(os.path.join(ROOT, 'data', 'perk-keys.json'), encoding='utf-8'))   # cwtest --perk-keys: what a baked perk carries
    bands = tier_ladder.load_bands(os.path.join(ROOT, 'framework', 'espgen', 'Program.cs'),   # the generator's own Chrome Crit bands
                                   os.path.join(ROOT, 'data', 'chrome-crit-bands.json'))
    ladder = tier_ladder.apply(raw, perk, bands)                       # explicit numbers for every sold tier, each strictly better; shown = applied
    schema = player_text.apply(raw)
    tt = tier_text.apply(schema)                                # every description states the shown tier's own numbers
    if tt:
        print('\n'.join('ERROR tier text (tools/tier_text.py): ' + e for e in tt))
        return 1
    lad = tier_ladder.problems(schema)
    if lad:
        print('\n'.join('ERROR tier ladder (tools/tier_ladder.py): ' + e for e in lad))
        return 1
    if '--ladder-report' in sys.argv:
        print('tier ladder: before %s' % before)
        print('tier ladder: after  %s' % tier_ladder.snapshot(schema))
        for k in ('filled', 'stepped', 'clamped', 'banded', 'dropped', 'dropped_for_good', 'dropped_until_perk'):
            for line in ladder[k]:
                print('  %s %s' % (k, line))
        for o in ladder['perk_owed']:
            print('  perk_owed %s tier %d: %s applied %s, intended %s' % (o['ability'], o['tier'], o['param'], o['applied'], o['intended']))
    jargon = player_text.lint(schema)
    for w in player_text.WARNINGS:   # known build notes, BETA QA 2's to fix; loud on every build, fatal once fixed and still listed
        print('WARNING player text carries a build note (known, owner BETA QA 2; tools/player_text.py KNOWN_BUILD_NOTES): ' + w)
    if jargon:
        print('\n'.join('ERROR player text reads like a design note (tools/player_text.py): ' + e for e in jargon))
        return 1
    text = json.dumps(schema, indent=1, ensure_ascii=False) + '\n'
    json.loads(text)
    handlers = {}
    for a in schema['abilities']:
        n = a['spec']['native']
        handlers.setdefault(n.get('activation'), []).append({'handler': n['handler'], 'ability': a['id']})
    htext = json.dumps({'_comment': 'Every distinct spec.native.handler in cyberware.json, grouped by activation '
                        '(always | condition | hotkey | equip). Generated; FRAMEWORK stubs each one.',
                        'count': sum(len(v) for v in handlers.values()),
                        'byActivation': {k: handlers[k] for k in sorted(handlers)}}, indent=1) + '\n'
    ptext = json.dumps({'_comment': 'Ripperdoc NPC + chair placement for espgen (FRAMEWORK). Read from Fallout4.esm on '
                        'the test rig 2026-11-08; points checked against each cell navmesh. Rotations in radians '
                        '(REFR DATA convention). chairExisting = reuse that vanilla ref, place no new chair. '
                        'Generated from tools/catalog_data.py RIPPERDOCS; see docs/RIPPERDOCS.md.',
                        'ripperdocs': [dict(id=r['id'], name=r['name'], race=r['race'], sex=r['sex'], voice=r['voice'],
                                            outfit=r['outfit'], factions=r['factions'], **r['placement'],
                                            **r.get('look', {}))
                                       for r in D.RIPPERDOCS]}, indent=1) + '\n'
    with open(DESIGN, encoding='utf-8') as fh:
        design = fh.read()
    if BEGIN not in design or END not in design:
        print('ERROR DESIGN.md is missing the generated-mapping markers')
        return 1
    new_design = design[:design.index(BEGIN)] + mapping_markdown(doc) + design[design.index(END) + len(END):]
    if check:
        stale = []
        if not os.path.exists(JSON_OUT) or open(JSON_OUT, encoding='utf-8').read() != text:
            stale.append(JSON_OUT)
        if not os.path.exists(HANDLERS_OUT) or open(HANDLERS_OUT, encoding='utf-8').read() != htext:
            stale.append(HANDLERS_OUT)
        if not os.path.exists(PLACEMENT_OUT) or open(PLACEMENT_OUT, encoding='utf-8').read() != ptext:
            stale.append(PLACEMENT_OUT)
        if new_design != design:
            stale.append(DESIGN)
        if stale:
            print('STALE: ' + ', '.join(stale))
            return 1
    else:
        with open(JSON_OUT, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
        with open(HANDLERS_OUT, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(htext)
        with open(PLACEMENT_OUT, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(ptext)
        with open(DESIGN, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(new_design)
    conf = {}
    for i in doc['items']:
        k = i['cp2077']['source']['confidence']
        conf[k] = conf.get(k, 0) + 1
    print('items=%d mods=%d perks=%d quickhacks=%d feasibility=%s cp_confidence=%s' % (
        len(doc['items']), len(doc['mods']), len(doc['perks']), len(doc['quickhacks']),
        json.dumps(counts(doc), sort_keys=True), json.dumps(conf, sort_keys=True)))
    print('schema: stats=%d abilities=%d items=%d mods=%d cyberPerks=%d ripperdocs=%d records~%s' % (
        len(schema['stats']), len(schema['abilities']), len(schema['items']), len(schema['mods']),
        len(schema['cyberPerks']), len(schema['ripperdocs']), json.dumps(to_schema.record_estimate(schema))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
