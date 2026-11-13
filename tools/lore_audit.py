#!/usr/bin/env python3
"""lore_audit.py - the skeptic pass's counting half: does a run of lore obey docs/ITEM-LORE.md?

    python3 -I fallout/cyberware/tools/lore_audit.py

Checks the built catalog's `lore` entries against the standard's hard rules:
  length      <= 165 soft cap (reported), <= 320 hard (FAIL)
  initialisms none: "VA" and its kind are unreadable on a card read alone (FAIL)
  origin mix  at most 4 in any 10 CONSECUTIVE entries may be 'procurement' (the reader's guarantee:
              cards are browsed by slot, so adjacency is what they feel), at most 1 in 3 overall
              (the writer's target, which does not depend on catalog ordering), and any 10 must
              show at least 3 different non-procurement origins (FAIL)
  closings    at most 1 in 3 may be 'reversal' (FAIL)
  tags        every entry with lore must be tagged in catalog_data.LORE_TAGS (FAIL)

Origin and closing are read from LORE_TAGS, not guessed from the words: no keyword heuristic can
tell what a sentence is doing, and a wrong automatic verdict is worse than none.
Last line: LORE_OK / LORE_FAIL with counts. Exit 0 or 1.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import catalog_data as D  # noqa: E402

SOFT, HARD = 165, 320
MAX_PROC_PER_10 = 4
MAX_PROC_SHARE = 1.0 / 3.0   # the writer's target; the window rule is the reader's guarantee
MIN_OTHER_PER_10 = 3
MAX_REVERSAL_SHARE = 1.0 / 3.0
INITIALISM = re.compile(r'(?<![A-Za-z])(?!A\b|I\b)[A-Z]{2,4}(?![A-Za-z])')

cat = json.load(open(os.path.join(HERE, '..', 'data', 'cyberware.json'), encoding='utf-8'))
entries = []
for coll in ('items', 'mods'):
    for x in cat.get(coll, []):
        if (x.get('lore') or '').strip():
            entries.append((x['id'], x['lore'].strip()))

bad, notes = [], []
tags = getattr(D, 'LORE_TAGS', {})
for i, t in entries:
    if i not in tags:
        bad.append('%s: no LORE_TAGS entry (origin, closing)' % i)
for i, t in entries:
    if len(t) > HARD:
        bad.append('%s: %d chars, over the %d hard ceiling' % (i, len(t), HARD))
    elif len(t) > SOFT:
        notes.append('%s: %d chars, over the %d soft cap' % (i, len(t), SOFT))
    m = INITIALISM.search(t)
    if m:
        bad.append('%s: initialism "%s" - spell it out' % (i, m.group(0)))

origins = [tags.get(i, ('?', '?'))[0] for i, _ in entries]
closings = [tags.get(i, ('?', '?'))[1] for i, _ in entries]
for start in range(0, max(1, len(entries) - 9)):
    win = origins[start:start + 10]
    if len(win) < 10 and start:
        break
    proc = win.count('procurement')
    other = len({o for o in win if o != 'procurement'})
    if proc > MAX_PROC_PER_10:
        bad.append('entries %d-%d: %d procurement origins, cap is %d' % (start + 1, start + len(win), proc, MAX_PROC_PER_10))
    if len(win) == 10 and other < MIN_OTHER_PER_10:
        bad.append('entries %d-%d: only %d different non-procurement origins, need %d' % (start + 1, start + 10, other, MIN_OTHER_PER_10))

proc_total = origins.count('procurement')
if entries and proc_total > len(entries) * MAX_PROC_SHARE + 1e-9:
    bad.append('%d of %d entries are procurement origins, share cap is 1 in 3' % (proc_total, len(entries)))

rev = closings.count('reversal')
if entries and rev > len(entries) * MAX_REVERSAL_SHARE + 1e-9:
    bad.append('%d of %d entries close on the dry reversal, cap is 1 in 3' % (rev, len(entries)))

for n in notes:
    print('note:  ' + n)
for b in bad:
    print('FAIL:  ' + b)
lens = [len(t) for _, t in entries]
print('lore: %d entries, length min %d / mean %d / max %d; procurement %d, reversal closings %d; origins used: %s'
      % (len(entries), min(lens) if lens else 0, sum(lens) // len(lens) if lens else 0, max(lens) if lens else 0,
         origins.count('procurement'), rev, ', '.join(sorted(set(origins)))))
print('LORE_FAIL %d problem(s)' % len(bad) if bad else 'LORE_OK %d entries' % len(entries))
sys.exit(1 if bad else 0)
