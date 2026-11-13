#!/usr/bin/env python3
"""check_ripperdoc_dialogue.py <Cyberware.esp> [--expect-absent] - the ripperdoc talk's records, read back. ASCII only.

VARGA TALK 2026-11-12 (docs/VARGA-TALK.md). This is not a text match on the generator: it opens the WRITTEN plugin and
judges the records the engine will read. Run it on any Cyberware.esp, the rig's own included - it needs no .NET.

What it holds the records to, and why (every number measured on the rig's Fallout4.esm and FalloutNV34.esm, 2026-11-12):

  1 QUST CW_RipperdocDialogue exists, is start-game enabled and has priority 90, above the 50 of
    F34_DialogueShared, whose top-level branch wins the conversation today.
  2 Its DLBR has the Top-Level bit (DNAM 0x01). 129 of Fallout4.esm's 132 DLBR are 0x01 and all 5 of
    FalloutNV34.esm's are; the single vanilla Blocking-without-Top-Level branch is a package forcegreet. A branch
    without that bit is not in the pool a player-started conversation picks from, which is defect D5.
  3 Its DLBR's SNAM names the greeting topic.
  4 EVERY topic of that quest carries SNAM 'CUST' with DATA 00000000 - the custom-topic shape every one of
    Fallout4.esm's 35,443 DIAL and every F34 FNV_GREETING topic uses. Four NUL bytes is no subtype at all.
  5 The greeting topic carries at least two INFOs (one per ripperdoc, plus the faction-wide fallback), each with a
    response text that is not empty and NO player prompt: a greeting is played, not chosen.
  6 There are at least two player topics, each with an INFO carrying a non-empty prompt (RNAM). One of them must be
    the shop line, CW_RipperdocInfoShop, which is what the DLL opens the view from.
  7 Every INFO of the talk has ENAM with Force Subtitle (0x0200) set, so the line is readable with the player's
    dialogue subtitles off - the lines are text only in this update - and so the DLL can see it in the
    SubtitleManager.
  8 Every response's TRDA emotion is FFFFFFFF, vanilla's "no emotion" (the same rule as check_info_forms.py).

    python -I check_ripperdoc_dialogue.py "Cyberware.esp"
        last line RDTALK_OK <n> topic(s), <m> info(s) (exit 0), or RDTALK_FAIL <n> problem(s) (exit 1).
    python -I check_ripperdoc_dialogue.py "Cyberware.esp" --expect-absent
        for a PLAY build, which is generated without --ripperdoc-dialogue-guard: PASSES only if the plugin holds no
        ripperdoc dialogue record at all, and FAILS if a half-built talk is in it.

INFOs are read as the children that follow their DIAL, which is how a plugin's Topic Children group is laid out.
"""
import struct
import sys
import zlib

WANT = (b'QUST', b'DLBR', b'DIAL', b'INFO')
FORCE_SUBTITLE = 0x0200
TOP_LEVEL = 0x01
EXPECT_PRIORITY = 90
SHOP_INFO_EDID = 'CW_RipperdocInfoShop'
QUEST_EDID = 'CW_RipperdocDialogue'


def subrecords(data):
    p, big, out = 0, 0, []
    while p + 6 <= len(data):
        st = data[p:p + 4]
        sl = struct.unpack_from('<H', data, p + 4)[0]
        if st == b'XXXX':
            big = struct.unpack_from('<I', data, p + 6)[0]
            p += 6 + sl
            continue
        if big:
            sl, big = big, 0
        out.append((st, data[p + 6:p + 6 + sl]))
        p += 6 + sl
    return out


def records(buf):
    off, end = 0, len(buf)
    while off + 24 <= end:
        typ = buf[off:off + 4]
        if typ == b'GRUP':
            off += 24
            continue
        size, flags, fid = struct.unpack_from('<III', buf, off + 4)
        data = buf[off + 24:off + 24 + size]
        if flags & 0x00040000:
            try:
                data = zlib.decompress(data[4:])
            except zlib.error:
                data = None
        if typ in WANT:
            yield typ, fid, data
        off += 24 + size


def zstr(b):
    return b.split(b'\x00')[0].decode('ascii', 'replace')


def u32(b):
    return struct.unpack_from('<I', b, 0)[0] if len(b) >= 4 else None


def read(path):
    """-> (quests, branches, topics) where a topic is a dict with its infos attached."""
    quests, branches, topics = {}, {}, []
    cur = None
    with open(path, 'rb') as f:
        buf = f.read()
    for typ, fid, data in records(buf):
        if data is None:
            continue
        sr = {}
        for st, sd in subrecords(data):
            sr.setdefault(st, []).append(sd)
        edid = zstr(sr[b'EDID'][0]) if b'EDID' in sr else ''
        if typ == b'QUST':
            dn = sr.get(b'DNAM', [b''])[0]
            quests[edid] = {
                'fid': fid,
                'flags': struct.unpack_from('<H', dn, 0)[0] if len(dn) >= 2 else -1,
                'priority': dn[2] if len(dn) >= 3 else -1,
            }
        elif typ == b'DLBR':
            dn = sr.get(b'DNAM', [b''])[0]
            branches[fid] = {
                'edid': edid,
                'flags': u32(dn) if len(dn) >= 4 else (dn[0] if dn else -1),
                'quest': u32(sr.get(b'QNAM', [b''])[0]) if b'QNAM' in sr else None,
                'start': u32(sr.get(b'SNAM', [b''])[0]) if b'SNAM' in sr else None,
                'subs': [s.decode('ascii', 'replace') for s, _ in subrecords(data)],
            }
        elif typ == b'DIAL':
            cur = {
                'fid': fid, 'edid': edid,
                'quest': u32(sr.get(b'QNAM', [b''])[0]) if b'QNAM' in sr else None,
                'branch': u32(sr.get(b'BNAM', [b''])[0]) if b'BNAM' in sr else None,
                'snam': sr.get(b'SNAM', [b''])[0],
                'data': sr.get(b'DATA', [b''])[0],
                'infos': [],
            }
            topics.append(cur)
        elif typ == b'INFO':
            if cur is None:
                continue
            enam = sr.get(b'ENAM', [b''])[0]
            cur['infos'].append({
                'fid': fid, 'edid': edid,
                'enam': struct.unpack_from('<H', enam, 0)[0] if len(enam) >= 2 else None,
                'prompt': zstr(sr[b'RNAM'][0]) if b'RNAM' in sr else '',
                'texts': [zstr(x) for x in sr.get(b'NAM1', [])],
                'emotions': [u32(x) for x in sr.get(b'TRDA', [])],
                'conds': len(sr.get(b'CTDA', [])),
            })
    return quests, branches, topics


def main():
    args = sys.argv[1:]
    absent = '--expect-absent' in args
    args = [a for a in args if a != '--expect-absent']
    if not args:
        print(__doc__.strip())
        return 2
    path = args[0]
    quests, branches, topics = read(path)
    bad = []

    q = quests.get(QUEST_EDID)
    ours = [t for t in topics if q and t['quest'] == q['fid']]
    ourbr = {f: b for f, b in branches.items() if q and b['quest'] == q['fid']}

    if absent:
        n = (1 if q else 0) + len(ourbr) + len(ours)
        if n:
            bad.append('--expect-absent: the plugin holds %d ripperdoc dialogue record(s) (quest %s, %d branch(es), %d topic(s))'
                       % (n, 'yes' if q else 'no', len(ourbr), len(ours)))
        print(('RDTALK_FAIL %d problem(s)' % len(bad)) if bad else 'RDTALK_OK no ripperdoc dialogue, as a play build should have')
        for b in bad:
            print('  ' + b)
        return 1 if bad else 0

    # 1 the quest
    if not q:
        bad.append('no QUST %s: the talk is not in this plugin' % QUEST_EDID)
    else:
        if not q['flags'] & 0x0001:
            bad.append('QUST %s DNAM flags 0x%04X: start-game-enabled (0x0001) is not set' % (QUEST_EDID, q['flags']))
        if q['priority'] != EXPECT_PRIORITY:
            bad.append('QUST %s priority %d, expected %d (above F34_DialogueShared\'s 50)' % (QUEST_EDID, q['priority'], EXPECT_PRIORITY))

    # 2, 3 the branch
    if len(ourbr) != 1:
        bad.append('%d DLBR on quest %s, expected exactly 1' % (len(ourbr), QUEST_EDID))
    for f, b in ourbr.items():
        if not (b['flags'] or 0) & TOP_LEVEL:
            bad.append('DLBR %s (%08X) DNAM 0x%08X: the Top-Level bit 0x01 is NOT set, so a player-started conversation '
                       'never considers this branch (defect D5). 129 of 132 vanilla DLBR are 0x01'
                       % (b['edid'], f, b['flags'] or 0))
        if not b['start']:
            bad.append('DLBR %s has no SNAM: no starting topic' % b['edid'])
        elif not any(t['fid'] == b['start'] for t in ours):
            bad.append('DLBR %s SNAM %08X is not a topic of this quest' % (b['edid'], b['start']))

    if not ours:
        bad.append('no DIAL on quest %s' % QUEST_EDID)

    # 4 the topic shape
    for t in ours:
        if t['snam'] != b'CUST':
            bad.append('DIAL %s (%08X) SNAM %r: every one of Fallout4.esm\'s 35,443 DIAL carries a real 4-character '
                       'subtype name, and a custom topic uses \'CUST\'' % (t['edid'] or '-', t['fid'], t['snam']))
        if t['data'] not in (b'\x00\x00\x00\x00',):
            bad.append('DIAL %s (%08X) DATA %s, expected 00000000 (category 0, the CUST shape)'
                       % (t['edid'] or '-', t['fid'], t['data'].hex()))

    greet = [t for t in ours for b in ourbr.values() if t['fid'] == b['start']]
    players = [t for t in ours if t not in greet]

    # 5 the greeting
    for t in greet:
        if len(t['infos']) < 2:
            bad.append('greeting topic %s has %d INFO(s): expected one per ripperdoc plus the faction-wide fallback'
                       % (t['edid'], len(t['infos'])))
        for i in t['infos']:
            if i['prompt']:
                bad.append('greeting INFO %s (%08X) carries a prompt %r: a greeting is played, not chosen'
                           % (i['edid'] or '-', i['fid'], i['prompt']))
            if not any(x.strip() for x in i['texts']):
                bad.append('greeting INFO %s (%08X) has no response text: the player would read nothing'
                           % (i['edid'] or '-', i['fid']))
            if i['conds'] < 1:
                bad.append('greeting INFO %s (%08X) has no condition: it would be offered on every actor in the game'
                           % (i['edid'] or '-', i['fid']))

    # 6 the player choices
    prompted = [i for t in players for i in t['infos'] if i['prompt']]
    if len(prompted) < 2:
        bad.append('%d player INFO(s) with a prompt, expected at least 2 ("Let me see your cyberware." and a way out)'
                   % len(prompted))
    if not any(i['edid'] == SHOP_INFO_EDID for i in prompted):
        bad.append('no INFO %s with a prompt: that is the record the DLL opens the view from' % SHOP_INFO_EDID)
    for i in prompted:
        if not any(x.strip() for x in i['texts']):
            bad.append('player INFO %s (%08X) has no response text: she would answer with nothing'
                       % (i['edid'] or '-', i['fid']))

    # 7, 8 every INFO of the talk
    allinfos = [i for t in ours for i in t['infos']]
    for i in allinfos:
        if i['enam'] is None:
            bad.append('INFO %s (%08X) has no ENAM, so Force Subtitle cannot be set' % (i['edid'] or '-', i['fid']))
        elif not i['enam'] & FORCE_SUBTITLE:
            bad.append('INFO %s (%08X) ENAM 0x%04X: Force Subtitle (0x0200) is not set, so a player with dialogue '
                       'subtitles off reads nothing and the DLL cannot see the line'
                       % (i['edid'] or '-', i['fid'], i['enam']))
        for e in i['emotions']:
            if e == 0:
                bad.append('INFO %s (%08X) has a response with emotion 00000000 (a NULL form); vanilla\'s "no emotion" '
                           'is FFFFFFFF' % (i['edid'] or '-', i['fid']))

    for b in bad:
        print('  ' + b)
    if bad:
        print('RDTALK_FAIL %d problem(s)' % len(bad))
        return 1
    print('RDTALK_OK %d topic(s), %d info(s): quest %s priority %d, branch %s DNAM 0x%02X (top-level), all topics \'CUST\', '
          'force-subtitle on every info'
          % (len(ours), len(allinfos), QUEST_EDID, q['priority'],
             list(ourbr.values())[0]['edid'], list(ourbr.values())[0]['flags']))
    return 0


sys.exit(main())
