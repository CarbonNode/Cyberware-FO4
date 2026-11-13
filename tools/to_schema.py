"""Turn the SPEC design model (build_catalog.build()) into FRAMEWORK's catalog schema v1.

Contract: fallout/cyberware/docs/CATALOG-SCHEMA.md (owned by FRAMEWORK). Unknown keys are
ignored by the engine, so SPEC keeps its design metadata in a `spec` object on each item,
ability, mod and perk, plus top-level `specMeta` and `quickhacks`.

Rules this module follows (see DESIGN.md section 3.6):
- `stats` only use ActorValueInfo EditorIDs that exist in Fallout4.esm (verified on the test rig
  2026-11-08: 359 AVIF records). Each stat bit costs 2 ESL records, so bits are small.
- An item's always-on, actor-value part goes into tier `stats` (scaled per tier). Everything
  else is ONE ability per item (`ab_<item id>`). Its `spec.native` block holds the real
  behaviour for the DLL/Papyrus handler. When an ability has no honest actor-value part, its
  `effects` holds a single value-0 MARKER effect (`spec.marker: true`), so the generator emits a
  harmless named spell until FRAMEWORK implements the handler.
- Tier numbers: T1..T5 = 1..5, T5+ = 6, T5++ = 7.
"""

import math
import ripperdoc_talk  # noqa: E402

import icon_map

# id, name, av, step, bits, negBits, format
STATS = [
    ('health', 'Health', 'Health', 1, 8, 7, '+{v}'),  # negBits 7: Edgerunner over-capacity penalty
    ('actionPoints', 'Action Points', 'ActionPoints', 1, 7, 4, '+{v}'),
    ('armor', 'Armor (Damage Resist)', 'DamageResist', 1, 8, 5, '+{v}'),
    ('energyResist', 'Energy Resistance', 'EnergyResist', 1, 8, 0, '+{v}'),
    ('poisonResist', 'Poison Resistance', 'PoisonResist', 1, 7, 0, '+{v}'),
    ('radResist', 'Radiation Resistance', 'RadResistExposure', 1, 7, 0, '+{v}'),
    ('carryWeight', 'Carry Weight', 'CarryWeight', 5, 6, 0, '+{v}'),
    ('speed', 'Movement Speed', 'SpeedMult', 1, 6, 4, '+{v}%'),
    ('jumpHeight', 'Jump Height', 'JumpHeightMult', 0.05, 5, 0, '+{v}x'),
    ('unarmedDamage', 'Unarmed Damage', 'UnarmedDamage', 1, 7, 0, '+{v}'),
    ('healRate', 'Health Regen', 'HealRate', 0.1, 5, 0, '+{v}'),
    ('armorPen', 'Armor Penetration', 'ArmorPenetration', 1, 6, 0, '+{v}%'),
    ('aimStability', 'Aim Stability', 'AimStability', 1, 6, 3, '+{v}'),
    ('noise', 'Movement Noise', 'DetectionMovementMod', 0.05, 1, 4, '{v}'),
    ('strength', 'Strength', 'Strength', 1, 3, 2, '+{v}'),
    ('perception', 'Perception', 'Perception', 1, 3, 2, '+{v}'),
    ('endurance', 'Endurance', 'Endurance', 1, 3, 2, '+{v}'),
    ('charisma', 'Charisma', 'Charisma', 1, 3, 2, '+{v}'),
    ('intelligence', 'Intelligence', 'Intelligence', 1, 3, 2, '+{v}'),
    ('agility', 'Agility', 'Agility', 1, 3, 2, '+{v}'),
    ('luck', 'Luck', 'Luck', 1, 3, 2, '+{v}'),
]
# Custom stats: new AVIFs espgen creates in Cyberware.esp ("custom": true; av must start with CW_).
# id, name, av, step, bits, negBits, format
CUSTOM_STATS = [
    ('ram', 'RAM', 'CW_RAMMax', 1, 5, 3, '+{v}'),
    ('ramRegen', 'RAM Regen', 'CW_RAMRegen', 0.05, 4, 0, '+{v}/s'),
    ('cooldown', 'Cyberware Cooldown Reduction', 'CW_CooldownMult', 1, 6, 0, '-{v}%'),
    ('critChance', 'Crit Chance', 'CW_CritChance', 1, 6, 0, '+{v}%'),
    ('critDamage', 'Crit Damage', 'CW_CritDamage', 5, 5, 0, '+{v}%'),
    ('mitigationChance', 'Mitigation Chance', 'CW_MitigationChance', 1, 6, 0, '+{v}%'),
    ('mitigationStrength', 'Mitigation Strength', 'CW_MitigationStrength', 1, 6, 0, '+{v}%'),
    ('qhDamage', 'Quickhack Damage', 'CW_QuickhackDamage', 1, 6, 0, '+{v}%'),
]
STEP = {s[0]: s[3] for s in STATS + CUSTOM_STATS}

SLOT_IDS = {
    'frontal_cortex': 'frontalCortex', 'face': 'face', 'circulatory': 'circulatorySystem',
    'immune': 'immuneSystem', 'nervous': 'nervousSystem', 'integumentary': 'integumentarySystem',
    'skeleton': 'skeleton', 'hands': 'hands', 'arms': 'arms', 'legs': 'legs', 'os': 'operatingSystem',
}
MARKER_AV = {
    'frontal_cortex': 'Intelligence', 'face': 'Perception', 'circulatory': 'HealRate', 'immune': 'PoisonResist',
    'nervous': 'Agility', 'integumentary': 'DamageResist', 'skeleton': 'DamageResist', 'hands': 'AimStability',
    'arms': 'UnarmedDamage', 'legs': 'SpeedMult', 'os': 'ActionPoints',
}
TIER_NUM = {'T1': 1, 'T2': 2, 'T3': 3, 'T4': 4, 'T5': 5, 'T5+': 6, 'T5++': 7}

# Always-on actor-value params -> (stat id, multiplier). Health_pct / DamageResist_pct are
# turned into flat values against a level-40 reference (about 300 HP, 100 DR).
AV_PARAMS = {
    'DamageResist': ('armor', 1), 'EnergyResist': ('energyResist', 1), 'PoisonResist': ('poisonResist', 1),
    'CarryWeight': ('carryWeight', 1), 'Health': ('health', 1), 'Health_pct': ('health', 3),
    'DamageResist_pct': ('armor', 1), 'recoil_pct': ('aimStability', -0.5), 'noise_pct': ('noise', 0.01),
    'ram_max': ('ram', 1), 'ram_regen': ('ramRegen', 1), 'crit_chance_pct': ('critChance', 1),
    'crit_dmg_pct': ('critDamage', 1), 'mitigation_chance_pct': ('mitigationChance', 1),
    'mitigation_strength_pct': ('mitigationStrength', 1),
}
# Upgrade components (the lead, 2026-11-08): vanilla FO4 forms, read out of Fallout4.esm itself
# (BETA QA EDID scan on the rig, 2026-11-09) and VERIFIED AT LOAD by the plugin's
# "forms: N upgrade component form(s), M failed" line — a wrong id fails the load check loudly.
# The five components are the CMPO records (what junk componentData references; each CMPO's
# scrapItem is the raw MISC the player carries — cw_components_plugin maps between them).
# cw_military_chip = the vanilla junk MISC "Military-grade circuit board" (no new ESL record).
COMPONENT_FORMS = {
    'c_circuitry': ('Fallout4.esm|01FA9B', 'Circuitry'),            # CMPO c_Circuitry
    'c_copper': ('Fallout4.esm|01FA9C', 'Copper'),                  # CMPO c_Copper
    'c_crystal': ('Fallout4.esm|01FA9F', 'Crystal'),                # CMPO c_Crystal
    'c_fiber_optics': ('Fallout4.esm|01FAA0', 'Fiber Optics'),      # CMPO c_FiberOptics
    'c_nuclear_material': ('Fallout4.esm|01FAB3', 'Nuclear Material'),  # CMPO c_NuclearMaterial
    'cw_military_chip': ('Fallout4.esm|154AD2', 'Military-Grade Circuit Board'),  # MISC CircuitBoardMilitary
}

# Chrome Compressor: capacityGrant per tier (schema tier field, counts while installed).
# CP-exact (BETA QA 2026-11-08): CapacityBooster grants 70 at EVERY quality
# (amountOfCapacityGivenForRipperdocUI = 70f on CapacityBoosterBase, no quality override).
CAPACITY_GRANT = {1: 70, 2: 70, 3: 70, 4: 70, 5: 70, 6: 70, 7: 70}
# Items whose AV params are conditional (sneaking, no enemy near, while active...).
CONDITIONAL = {'cw_sk_scarab', 'cw_it_rangeguard', 'cw_sk_scar_coalescer', 'cw_sk_universal_booster',
               'cw_ns_stabber', 'cw_ha_handle_wrap', 'cw_sk_kinetic_frame'}

MANUFACTURERS = ['Kiroshi', 'Militech', 'Arasaka', 'Zetatech', 'Dynalar', 'Qiant', 'Biodyne', 'BioDyne',
                 'Moore Tech', 'Fuyutsuki', 'Biotech', 'Seacho', 'Tetratronic', 'Stephenson', 'Raven',
                 'NetWatch']

ROLL_POOLS = {
    # attunement -> [(stat, min, max, weight)] at T5; scaled down per tier
    'body': [('health', 4, 15, 3), ('armor', 2, 8, 3), ('carryWeight', 5, 20, 2), ('unarmedDamage', 1, 5, 1),
             ('strength', 1, 1, 0.3)],
    'reflexes': [('actionPoints', 3, 10, 3), ('speed', 1, 3, 2), ('aimStability', 1, 4, 2),
                 ('mitigationChance', 1, 3, 2), ('agility', 1, 1, 0.3)],
    'technical': [('energyResist', 2, 8, 3), ('armorPen', 1, 4, 2), ('radResist', 3, 10, 2),
                  ('intelligence', 1, 1, 0.3)],
    'intelligence': [('ram', 1, 2, 3), ('ramRegen', 0.05, 0.15, 2), ('qhDamage', 1, 5, 2), ('energyResist', 2, 6, 1),
                     ('perception', 1, 1, 0.3)],
    'cool': [('critChance', 1, 4, 3), ('critDamage', 5, 10, 2), ('armorPen', 1, 4, 2), ('noise', -0.1, -0.05, 1),
             ('luck', 1, 1, 0.3)],
}
ROLL_POOLS['random'] = [r for k in ('body', 'reflexes', 'technical', 'intelligence', 'cool') for r in ROLL_POOLS[k]]

FINISH_VARIANTS = [('default', 'Bare Chrome', 0), ('matte_black', 'Matte Black', 200), ('militech', 'Militech Olive', 300),
                   ('arasaka', 'Arasaka Red/Black', 300), ('kang_tao', 'Kang Tao Gold', 500),
                   ('vault_tec', 'Vault-Tec Blue/Yellow', 300), ('realskinn', 'Realskinn Cover', 400),
                   ('hidden', 'Hidden', 0)]
EYE_VARIANTS = [('default', 'No Glow', 0), ('kiroshi_yellow', 'Kiroshi Yellow Glow', 150), ('cyan', 'Cyan Glow', 150),
                ('red', 'Red Glow', 150), ('green', 'Green Glow', 150), ('violet', 'Violet Glow', 150),
                ('white', 'White Glow', 150)]


def snap(v, step):
    q = round(v / step) * step
    return int(q) if float(step).is_integer() else round(q, 2)


def manufacturer(name):
    for m in MANUFACTURERS:
        if name.startswith(m) or ('"' in name and m in name):
            return 'Biodyne' if m == 'BioDyne' else m
    return None


def item_stats_t5(it):
    out = {}
    if it['id'] in CONDITIONAL or it.get('os_type') in ('berserk', 'sandevistan'):
        return out
    for k, v in it['fo4']['params_t5'].items():
        if k in AV_PARAMS and isinstance(v, (int, float)) and not isinstance(v, bool):
            sid, mult = AV_PARAMS[k]
            out[sid] = out.get(sid, 0) + v * mult
    return out


def rolls_for(att, tnum, scale, driver_update=False):
    count = 1 if tnum == 1 else 2
    pool = []
    for stat, lo, hi, w in ROLL_POOLS[att]:
        step = STEP[stat]
        a, b = snap(lo * scale, step), snap(hi * scale, step)
        if stat in ('strength', 'perception', 'endurance', 'charisma', 'intelligence', 'agility', 'luck'):
            if tnum < 5:
                continue
            a = b = 1
        if a == 0 and b == 0:
            continue
        if abs(a) < step:
            a = step if lo > 0 else -step
        if abs(b) < abs(a):
            b = a
        lo_, hi_ = (a, b) if a <= b else (b, a)
        pool.append({'stat': stat, 'min': lo_, 'max': hi_, 'weight': w})
    count = min(count, len(pool))
    return [{'count': count, 'pool': pool}] if pool else []


def mod_slot_types(it):
    if it['kind'] == 'weapon':
        base = it['id'].split('_')[2]
        return {'mantis': ['mantis_edge', 'mantis_rotor'], 'gorilla': ['gorilla_knuckles', 'gorilla_battery'],
                'monowire': ['monowire_cable', 'monowire_battery'], 'pls': ['pls_rounds', 'pls_capacity']}[base]
    if it['id'] in ('cw_face_kiroshi_mk2', 'cw_face_kiroshi_mk3'):
        return ['optics'] * it['fo4']['params_t5']['mod_slots']
    if it.get('os_type') in ('sandevistan', 'berserk'):
        return [it['os_type'] + '_frag']
    return []


def mod_slots_at(it, types, tnum):
    if not types:
        return 0
    if it['kind'] == 'weapon':
        return 1 if tnum < 3 else 2
    if it.get('os_type') in ('sandevistan', 'berserk'):
        return 1 if tnum >= 4 else 0
    return len(types)


def perk_native_params(pid, D):
    """paramsT5 + CP source for a cyber perk's native ability (catalog_data.PERK_PARAMS; FRAMEWORK, 2026-11-08)."""
    row = getattr(D, 'PERK_PARAMS', {}).get(pid)
    if not row:
        return {}
    params, url, quote, est = row
    out = {'paramsT5': params, 'scaleWithTier': False, 'source': {'url': url, 'cp': quote, 'read': '2026-11-08', 'by': 'FRAMEWORK'}}
    if est:
        out['source']['estimate'] = est
    return out


def mod_native_params(mid, D):
    """paramsT5 + CP source for a mod's native ability (catalog_data.MOD_PARAMS; FRAMEWORK, 2026-11-08)."""
    row = getattr(D, 'MOD_PARAMS', {}).get(mid)
    if not row:
        return {}
    params, url, quote, est = row
    out = {'paramsT5': params, 'scaleWithTier': False,
           'source': {'url': url, 'cp': quote, 'read': '2026-11-08', 'by': 'FRAMEWORK (SPEC closed)'}}
    if est:
        out['source']['estimate'] = est
    return out


def ability_for(it, slot_key, D=None):
    p = it['fo4']['params_t5']
    stats_part = item_stats_t5(it)
    native = {
        'handler': it['id'][3:],
        'activation': it['activation'],
        'paramsT5': p,
        'scaleWithTier': True,
        'mechanism': it['fo4']['mechanism'],
        'feasibility': it['fo4']['feasibility'],
        'statsHandled': sorted(k for k in p if k in AV_PARAMS and stats_part),
    }
    for t, pt in (it['fo4'].get('params_by_tier') or {}).items():   # per-tier params (CP TweakDB qualities): no scaling
        native['paramsT%d' % int(t)] = pt
        native['scaleWithTier'] = False
    if it['id'] == 'cw_ar_pls' and D is not None and getattr(D, 'PLS_CHARGES', None):
        native['source'] = {'tweakdb': D.PLS_CHARGES['source'], 'url': 'https://cyberpunk2077.wiki.fextralife.com/Projectile+Launch+System',
                            'read': '2026-11-08', 'note': [D.PLS_CHARGES['estimate']]}
    # Pure always-on AV items need no ability.
    leftover = {k: v for k, v in p.items() if k not in AV_PARAMS and k != 'capacity'}
    if not leftover and (stats_part or 'capacity' in p) and it['kind'] == 'passive':
        return None
    ab = {
        'id': 'ab_' + it['id'][3:],
        'name': it['name'],
        'description': it['fo4']['effect_t5'],
        'effects': [{'av': MARKER_AV[slot_key], 'value': 0}],
        'spec': {'marker': True, 'native': native},
    }
    return ab


def icon_fields(row):
    """(icon path or None, spec dict) for an ICON_MAP row."""
    if row is None:
        return None, {'iconPending': {'reason': 'no CP2077 item with this name'}}
    name, rec, part = row
    spec = {'cpName': name, 'cpRecord': rec, 'cpIcon': part}
    if part in icon_map.DELIVERED:
        return 'icons/%s.png' % part, spec
    spec['iconPending'] = {'part': part, 'atlas': icon_map.PENDING_ATLAS.get(part, '?'),
                           'reason': 'CP icon exists but ASSETS has not exported it'}
    return None, spec


def to_schema(doc, D):
    missing = [i['id'] for i in doc['items'] if i['id'] not in icon_map.ITEM_ICONS]
    missing += [m['id'] for m in doc['mods'] if m['id'] not in icon_map.MOD_ICONS]
    if missing:
        raise SystemExit('icon_map has no row for: ' + ', '.join(missing))
    tiers_meta = {t['id']: t for t in doc['tiers']}
    out = {
        'schemaVersion': 1,
        '_comment': 'Generated by tools/build_catalog.py from tools/catalog_data.py; do not hand-edit. Carries data '
                    'derived from Cyberpunk 2077: read docs/DESIGN.md section 7 before distributing.',
        'currency': {'form': 'Fallout4.esm|00000F', 'name': 'Caps'},
        # CP-exact curve (the lead, 2026-11-08): 24 + 3 x level, cap 201 (CP's level-60 base), and
        # the Renaissance Punk shape as +4 per SPECIAL at 9+ (perAV 'min' = threshold). The old
        # 60 + 3L / 300 / +5 x Endurance ladder is withdrawn; the Settings capacityMult loosens.
        'capacity': {'base': 24, 'perLevel': 3, 'max': 201,
                     'perAV': [{'av': av, 'add': 4, 'min': 9} for av in
                               ('Strength', 'Perception', 'Endurance', 'Charisma',
                                'Intelligence', 'Agility', 'Luck')]},
        # 13 uses x 6 = 78, ~CP's ~80 findable shard capacity. Some are sold at the docs
        # (capacityItemStock below); world loot placement is a follow-up board item.
        'capacityItems': [{'id': 'neural_matrix', 'name': 'Neural Matrix', 'add': 6, 'max': 13}],
        'rerollMult': 0.25,
        'perkPoints': {'start': 1, 'perLevels': 5},
        'sellMult': 0.4,
        'iconicUpgradeMult': 1.5,
        'perkRefund': True,
        'tiers': [{'id': TIER_NUM[t], 'name': 'Tier ' + t[1:], 'color': c} for t, c in
                  zip(['T1', 'T2', 'T3', 'T4', 'T5', 'T5+', 'T5++'],
                      ['#9d9d9d', '#3fbf3f', '#3f8fff', '#b04fff', '#ff9f1f', '#ffcf3f', '#ff5f5f'])],
        'slots': [{'id': SLOT_IDS[s['id']], 'name': s['name'], 'count': s['slots']} for s in doc['systems']],
        'stats': [dict(id=i, name=n, av=a, step=st, bits=b, negBits=nb, format=f) for i, n, a, st, b, nb, f in STATS]
                 + [dict(id=i, name=n, av=a, avName=n, custom=True, step=st, bits=b, negBits=nb, format=f)
                    for i, n, a, st, b, nb, f in CUSTOM_STATS],
        'abilities': [],
        'items': [],
        'mods': [],
        'cyberPerks': [],
        'ripperdocs': [],
    }
    abilities = out['abilities']
    for it in doc['items']:
        slot = SLOT_IDS[it['system']]
        ab = ability_for(it, it["system"], D)
        if ab:
            abilities.append(ab)
        st5 = item_stats_t5(it)
        types = mod_slot_types(it)
        tiers = []
        for tid in it['tiers']:
            bt = it['by_tier'][tid]
            tn = TIER_NUM[tid]
            scale = tiers_meta[tid]['effect_scale']
            stats = {}
            for sid, v in st5.items():
                q = snap(v * scale, STEP[sid])
                if q:
                    stats[sid] = q
            t = {'tier': tn, 'price': bt['price_caps'], 'capacityCost': bt['capacity'],
                 'modSlots': mod_slots_at(it, types, tn), 'stats': stats,
                 'abilities': [ab['id']] if ab else [],
                 'rolls': rolls_for(it['attunement'], tn, scale), 'requires': {'level': bt['level']}}
            comps = [{'form': COMPONENT_FORMS[c][0], 'name': COMPONENT_FORMS[c][1], 'count': n}
                     for c, n in (bt.get('upgrade_components') or {}).items()]
            if comps:
                t['components'] = comps   # cost to REACH this tier (Settings.upgradeComponents)
            if it.get('os_type') == 'compressor':
                t['capacityGrant'] = CAPACITY_GRANT[tn]
            tiers.append(t)
        variants = None
        if it['kind'] == 'weapon':
            variants = [{'id': v, 'name': n, **({'price': p} if p else {})} for v, n, p in FINISH_VARIANTS]
        elif it['system'] == 'face' and 'kiroshi' in it['id']:
            variants = [{'id': v, 'name': n, **({'price': p} if p else {})} for v, n, p in EYE_VARIANTS]
        item = {
            'id': it['id'], 'name': it['name'], 'slot': slot,
            'manufacturer': manufacturer(it['name']),
            'description': it['fo4']['effect_t5'],
            # optional: what the piece IS, in Varga's world (I(..., lore='...') in catalog_data.py; CATALOG-SCHEMA.md).
            # The item card prints it at the end of the Stats tab. Absent = no key at all, so the catalog is unchanged.
            **({'lore': it['lore'].strip()} if (it.get('lore') or '').strip() else {}),
            'icon': icon_fields(icon_map.ITEM_ICONS[it['id']])[0],
            'iconic': it['iconic'],
            'iconicAbilities': [],
            'allowDuplicate': False,
            'modSlotTypes': types,
            'chipTier': TIER_NUM[it['tiers'][0]],
            'tiers': tiers,
            'spec': {
                'kind': it['kind'], 'activation': it['activation'], 'optional': it['optional'],
                'cp2077': it['cp2077'], 'fo4': it['fo4'], 'attunement': it['attunement'],
                'assetHints': it['asset_hints'],
                **icon_fields(icon_map.ITEM_ICONS[it['id']])[1],
                **{k: it[k] for k in ('os_type', 'anims', 'variant_of') if it.get(k)},
            },
        }
        if variants:
            item['variants'] = variants
        if item['manufacturer'] is None:
            del item['manufacturer']
        out['items'].append(item)

    for m in doc['mods']:
        abid = 'ab_mod_' + m['id'][4:]
        abilities.append({'id': abid, 'name': m['name'], 'description': m['effect'],
                          'effects': [{'av': 'UnarmedDamage' if m['applies_to'] in ('mantis', 'gorilla', 'monowire')
                                       else 'ActionPoints', 'value': 0}],
                          'spec': {'marker': True, 'native': dict({'handler': 'mod_' + m['id'][4:], 'activation': 'equip',
                                                                   'effect': m['effect'],
                                                                   'fo4': m['fo4']}, **mod_native_params(m['id'], D))}})
        mtype = {'mantis': {'a': 'mantis_edge', 'b': 'mantis_rotor'},
                 'gorilla': {'a': 'gorilla_knuckles', 'b': 'gorilla_battery'},
                 'monowire': {'a': 'monowire_cable', 'b': 'monowire_battery'},
                 'pls': {'a': 'pls_rounds', 'b': 'pls_capacity'}}.get(m['applies_to'], {}).get(m['slot'])
        if not mtype:
            mtype = {'optics': 'optics', 'sandevistan': 'sandevistan_frag', 'berserk': 'berserk_frag'}[m['applies_to']]
        micon, mspec = icon_fields(icon_map.MOD_ICONS[m['id']])
        out['mods'].append({'id': m['id'], 'name': m['name'], 'type': mtype, 'description': m['effect'],
                            **({'lore': m['lore']} if m.get('lore') else {}),   # catalog_data.MOD_LORE (optional)
                            'icon': micon,
                            'tiers': [{'tier': 1, 'price': m['price_caps'], 'stats': {}, 'abilities': [abid],
                                       'capacityCost': 0, 'requires': {'level': {'rare': 1, 'epic': 20,
                                                                                  'legendary': 35}[m['rarity']]}}],
                            'spec': dict({'rarity': m['rarity'], 'confidence': m['confidence'], 'fo4': m['fo4'],
                                          **mspec, **({'cpSource': mod_native_params(m['id'], D)['source']}
                                                      if mod_native_params(m['id'], D) else {})},
                                         **({'elementOmod': D.ELEMENT_OMOD[m['id'].rsplit('_', 1)[1]]}
                                            if mtype in ('mantis_edge', 'gorilla_knuckles', 'monowire_cable')
                                            and m['id'].rsplit('_', 1)[1] in D.ELEMENT_OMOD else {}))})

    perk_effects = {
        'perk_all_things_cyber': [{'type': 'stat', 'stat': 'cooldown', 'add': 15}],
        'perk_license_to_chrome': [{'type': 'slotCount', 'slot': 'skeleton', 'add': 1}],
        'perk_ambidextrous': [{'type': 'slotCount', 'slot': 'hands', 'add': 1}],
        'perk_driver_update': [{'type': 'extraRolls', 'add': 1}],
        'perk_chipware_connoisseur': [{'type': 'rerollCostMult', 'mult': -0.5}, {'type': 'rerollKeepOld'}],
        'perk_chrome_constitution': [{'type': 'statPerPiece', 'stat': 'armor', 'add': 1}],
        # CP2077 Edgerunner: about +50 over capacity at -0.5% max HP per point (15% of the 300 cap = 45).
        'perk_edgerunner': [{'type': 'overCapacity', 'maxPct': 15, 'healthPctPerPoint': 0.5, 'stat': 'health'}],
    }
    for p in doc['perks']:
        pid = p['id'][5:]
        eff = perk_effects.get(p['id'])
        if not eff:
            abid = 'ab_perk_' + pid
            abilities.append({'id': abid, 'name': p['name'], 'description': p['effect'],
                              'effects': [{'av': 'ActionPoints', 'value': 0}],
                              'spec': {'marker': True, 'native': dict({'handler': 'perk_' + pid, 'activation': 'always',
                                                                       'effect': p['effect']}, **perk_native_params(p['id'], D))}})
            eff = [{'type': 'ability', 'ability': abid}]
        if p['id'] == 'perk_edgerunner':
            eff = eff + [{'type': 'ability', 'ability': 'ab_perk_edgerunner_fury'}]
            abilities.append({'id': 'ab_perk_edgerunner_fury', 'name': 'Fury',
                              'description': 'While over capacity: 2% chance per kill of Fury for 10 s (+25% damage).',
                              'effects': [{'av': 'ActionPoints', 'value': 0}],
                              'spec': {'marker': True, 'native': {'handler': 'perk_edgerunner_fury',
                                                                  'activation': 'condition',
                                                                  'paramsT5': {'chance_pct': 2, 'duration_s': 10,
                                                                               'dmg_pct': 25}}}})
        out['cyberPerks'].append({
            'id': pid, 'name': p['name'], 'description': p['effect'], 'maxRank': 1,
            'cost': 1 if p['level'] < 30 else 2, 'auto': False, 'effects': eff,
            'prereq': [p['prereq'][5:]] if p['prereq'] else [],
            'requires': {'level': p['level']},
            'spec': {'cpTree': p['cp_tree'], 'confidence': p['confidence']},
        })

    def stock(pred, lo, hi):
        rows = []
        for i in out['items']:
            if i['spec']['optional'] or not pred(i):
                continue
            ts = [t['tier'] for t in i['tiers'] if lo <= t['tier'] <= hi]
            if ts:
                rows.append({'item': i['id'], 'tiers': ts})
        return rows

    all_mods = [{'mod': m['id'], 'tiers': [1]} for m in out['mods'] if m['spec']['confidence'] != 'legacy-1.x']
    stock_rules = {
        # Kessler is the smaller back-alley shop (the author, 2026-11-08): the stock Varga used to carry.
        'goodneighbor': (stock(lambda i: not i['iconic'] and i['slot'] not in ('operatingSystem', 'arms'), 1, 3), []),
        # Varga is the main shop (the author, 2026-11-08): everything, every tier, the iconics, every mod.
        'diamondcity': (stock(lambda i: True, 1, 7), all_mods),
        'institute': ([], []),                          # disabled for now: sells nothing, records kept (see 'disabled' below)
    }
    # CP shards over the counter (the lead, 2026-11-08): Kessler and the Institute sell Neural
    # Matrix shards at 2000 caps base (level pricing and priceMult apply); Varga does not.
    shard_stock = {'goodneighbor': [{'id': 'neural_matrix', 'price': 2000}],
                   'institute': [{'id': 'neural_matrix', 'price': 2000}]}
    for r in D.RIPPERDOCS:
        st, ms = stock_rules[r['id']]
        pl = r['placement']
        out['ripperdocs'].append({
            # disabled (the author, 2026-11-09, Achterberg): not added to the game yet. Records still generated so
            # the FormID lock never drifts, but espgen skips his placement entirely (no NPC, chair or cell override).
            **({'disabled': True} if r.get('disabled') else {}),
            'id': r['id'], 'name': r['name'], 'priceMult': r['priceMult'], 'sellMult': r['sellMult'],
            'stock': st, 'modStock': ms,
            'capacityItemStock': shard_stock.get(r['id'], []),
            'talk': ripperdoc_talk.talk_for(r['id']),   # what the doc says when you talk (tools/ripperdoc_talk.py)
            # disabled: espgen generates the records (form ids stay locked) and places nothing (no 'placement' = not placed)
            'placement': None if r.get('disabled') else {k: pl[k] for k in ('cell', 'npcPos', 'npcRot', 'chairPos', 'chairRot')},
            **({'disabled': True} if r.get('disabled') else {}),
            **({'main': True} if r.get('main') else {}),        # the menu's setup view lists the main doc first
            **({'place': r['place']} if r.get('place') else {}),
            'spec': {'where': r['where'], 'stockRule': r['stock_rule'], 'race': r['race'], 'sex': r['sex'],
                     'voice': r['voice'], 'outfit': r['outfit'], 'factions': r['factions'],
                     'placementExtra': {k: v for k, v in pl.items()
                                        if k not in ('cell', 'npcPos', 'npcRot', 'chairPos', 'chairRot')},
                     'doc': 'docs/RIPPERDOCS.md'},
        })
    out['quickhacks'] = doc['quickhacks']
    out['specMeta'] = {
        'note': 'SPEC design data the engine ignores. The schema extensions agreed with FRAMEWORK are in '
                'docs/DESIGN.md section 3.6.',
        'designCapacity': doc['capacity'],
        'tierEffectScale': {TIER_NUM[t['id']]: t['effect_scale'] for t in doc['tiers']},
        'tierUpgradeComponents': {TIER_NUM[t['id']]: t['upgrade_components'] for t in doc['tiers']},
        'attunementSpecial': doc['attunement_special'],
        'finishes': doc['finishes'],
    }
    return out


def record_estimate(sch):
    stat = sum((s['bits'] + s['negBits']) * 2 for s in sch['stats'])
    abil = sum(1 + len(a.get('effects', [])) for a in sch['abilities'])  # spell + one MGEF per effect
    items = len(sch['items'])  # one loot chip (MISC) per item
    perks = len(sch['cyberPerks'])
    docs = len(sch['ripperdocs']) * 3  # NPC + chair + vendor container (estimate)
    return {'stat_perks_and_spells': stat, 'abilities_spell_plus_mgef': abil, 'item_chips': items,
            'cyber_perks': perks, 'ripperdocs': docs, 'total': stat + abil + items + perks + docs, 'budget': 2048}
