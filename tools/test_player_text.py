#!/usr/bin/env python3
"""Regression tests for player_text.lint(): the strings that DID ship, and must now fail.

    python3 -I fallout/cyberware/tools/test_player_text.py

Why (the lead, 2026-11-10): the banned-word list was dialogue-only and lint() read only
`description`, so a cyberPerk named "Edgerunner" and nine names ending in " (Relic)" reached the
shipped catalog. Each case below is a string that was really in it.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import player_text  # noqa: E402

PASS, FAIL = [], []


def check(name, ok):
    (PASS if ok else FAIL).append(name)
    print(('PASS ' if ok else 'FAIL ') + name)


def lint_one(coll, rec):
    return player_text.lint({coll: [rec]})


# --- the real strings that shipped -------------------------------------------------------------
hits = lint_one('cyberPerks', {'id': 'edgerunner', 'name': 'Edgerunner', 'description': 'Install up to 15% over max capacity.'})
check('the shipped cyberPerk NAME "Edgerunner" is caught', any('Edgerunner' in h for h in hits))

hits = lint_one('cyberPerks', {'id': 'jailbreak', 'name': 'Jailbreak (Relic)', 'description': 'Unlocks alternate abilities.'})
check('the shipped " (Relic)" name suffix is caught', any('Relic' in h for h in hits))

hits = lint_one('items', {'id': 'cw_os_deck_raven', 'name': 'Raven Microcyber Cyberdeck',
                          'description': '20 RAM, 6 quickhack slots. Enemy netrunners take twice as long to hack you.'})
check('the shipped description word "netrunners" is caught', any('netrunner' in h.lower() for h in hits))

hits = lint_one('quickhacks', {'id': 'qh_blackwall_gateway', 'name': 'Blackwall Gateway (PL)',
                               'kind': 'ultimate', 'effect': 'Kills the target outright.'})
check('the shipped quickhack name "Blackwall Gateway (PL)" is caught', len(hits) >= 1)

hits = player_text.lint_armory({'consumables': [{'edid': 'CW_AR_JohnnySilverhand', 'name': 'Johnny Silverhand',
                                                  'model': 'Cyberware\\Armory\\Consumables\\JohnnySilverhand.nif'}]})
check('the shipped stock NAME "Johnny Silverhand" (Varga\'s tequila) is caught', any('Silverhand' in h for h in hits))

hits = player_text.lint_armory({'consumables': [{'edid': 'CW_AR_JohnnySilverhand', 'name': 'Sierra Madre Reserve',
                                                  'model': 'Cyberware\\Armory\\Consumables\\JohnnySilverhand.nif',
                                                  'description': 'Top-shelf tequila.', 'lore': 'The casino bottled its own.'}]})
check('the renamed tequila passes; its internal edid and model path keep the old name and are not flagged', hits == [])

# --- build-note TEXT, with or without parentheses (the lead, 2026-11-10) ---------------------------------------
# The shipped list is empty since the cw-tier-text merge (all three notes got true text); the allowance mechanism is
# tested on a fixture list so it stays covered while the real list is empty.
_shipped_allowed = player_text.KNOWN_BUILD_NOTES
player_text.KNOWN_BUILD_NOTES = {('items', 'cw_sk_rara_avis')}
hits = lint_one('items', {'id': 'cw_sk_rara_avis', 'name': 'Rara Avis',
                          'description': 'Design placeholder: +10% max Health and +10% AP regen.'})
known_warned = list(player_text.WARNINGS)
check('the shipped "Design placeholder:" description is caught when not on the known list',
      any('build note' in h for h in player_text.lint_build_notes({'items': [{'id': 'cw_new', 'name': 'X', 'description': 'Design placeholder: +1.'}]})))
check('"TODO", "not implemented" and "WIP" without brackets are caught in any field',
      all(player_text.lint_build_notes({'items': [{'id': 'cw_new', 'name': n, 'description': 'ok'}]})
          for n in ('Arm TODO', 'Not implemented yet', 'Blade WIP')))
check('a known build note is a WARNING, not an error, while it is still there', hits == [] and any('cw_sk_rara_avis' in w for w in known_warned))
check('a known entry whose note is GONE fails as a stale allowance',
      any('stale allowance' in h for h in lint_one('items', {'id': 'cw_sk_rara_avis', 'name': 'Rara Avis', 'description': '+10% max Health.'})))
player_text.KNOWN_BUILD_NOTES = _shipped_allowed
_cat = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'cyberware.json'), encoding='utf-8'))
_raw = sorted({(c, i) for c, i, _f, _w, _d in player_text.build_notes(_cat)})
print('  today\'s catalog, raw build-note hits: ' + ', '.join('%s %s' % k for k in _raw))
check('on today\'s catalog the raw check names exactly the three known items and their abilities', set(_raw) == player_text.KNOWN_BUILD_NOTES)
check('a word that merely contains a banned one is not caught ("stubborn", "wipe")',
      player_text.lint_build_notes({'items': [{'id': 'cw_new', 'name': 'Stubborn Wipe', 'description': 'ok'}]}) == [])

# --- it must not fire on what is deliberately allowed ------------------------------------------
hits = lint_one('items', {'id': 'cw_face_kiroshi_basic', 'name': 'Basic Kiroshi Optics',
                          'manufacturer': 'Kiroshi', 'description': 'Turrets and cameras detect you 40% slower.'})
check('a brand name (Kiroshi) is allowed', hits == [])

hits = lint_one('items', {'id': 'cw_os_deck_arasaka', 'name': 'Arasaka Cyberdeck',
                          'description': '20 RAM, 6 quickhack slots. Stealth hacks cost 2 less.'})
check('brand + this mod\'s own vocabulary (quickhack, cyberdeck) is allowed', hits == [])

hits = lint_one('cyberPerks', {'id': 'edgerunner', 'name': 'Red Line', 'description': 'Install up to 15% over max capacity.'})
check('the id "edgerunner" alone is NOT flagged (internal, menu-referenced)', hits == [])

hits = lint_one('items', {'id': 'x', 'name': 'Second Heart', 'description': 'A killing blow brings you back.',
                          'lore': "A VA surgeon's last good idea. It fires once, loudly, and you will know it did."})
check('a clean `lore` line passes', hits == [])

hits = lint_one('items', {'id': 'x', 'name': 'Second Heart', 'description': 'A killing blow brings you back.',
                          'lore': 'Lifted off a corpo in Night City, choom.'})
check('a `lore` line with the other game\'s words is caught', len(hits) >= 1)

# --- engine jargon stays scoped to `description` -----------------------------------------------
hits = lint_one('quickhacks', {'id': 'qh_ping', 'name': 'Ping', 'kind': 'covert',
                               'effect': 'Reveals every device on the network.',
                               'mechanism': 'Engine: TargetAV kill + NearbyHostiles chain'})
check('engine-facing `mechanism` is not treated as player text', hits == [])

print('\n%d of %d checks passed' % (len(PASS), len(PASS) + len(FAIL)))
sys.exit(1 if FAIL else 0)
