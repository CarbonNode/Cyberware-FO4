# vanilla_barks.py - read-only: which vanilla generic dialogue can an NPC reach BECAUSE of her voice type,
# and where a given subtitle line comes from. No third-party modules. ASCII only.
#
#   py -3 -I vanilla_barks.py <Fallout4.esm> <strings dir> <voice type hex> [phrase ...]
#
# e.g. on the build machine (the pinned master, no rig turn - it is a read):
#   py -3 -I vanilla_barks.py "<masters>\Fallout4.esm" "<masters>\strings" 13ADD "huh?"
#
# Why it exists: FO4 gates every generic dialogue set at the QUEST level with one GetIsVoiceType
# (condition function 426) against a voice type or a FormList of them. An NPC reaches a set only if she
# passes that one condition, so her voice type alone decides which vanilla lines the engine may put in
# her mouth. See docs/VARGA-VOICETYPE.md.
import struct, zlib, sys, os, io, collections
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
esm, sdir, vthex = sys.argv[1], sys.argv[2], sys.argv[3]
TARGET = int(vthex, 16) & 0xFFFFFF
phrases = [p.strip('"').lower() for p in sys.argv[4:]]
GETISVOICETYPE = 426

KEEP = {b'DIAL', b'INFO', b'QUST', b'VTYP', b'FLST', b'FACT', b'NPC_'}
b = open(esm, 'rb').read()
recs = []
def walk(i, end, parent):
    while i < end:
        t = b[i:i+4]
        if t == b'GRUP':
            size, label, gt = struct.unpack('<IIi', b[i+4:i+16])
            if size < 24: return
            walk(i+24, i+size, label if gt == 7 else parent); i += size; continue
        size, flags, fid = struct.unpack('<III', b[i+4:i+16])
        if t in KEEP:
            d = b[i+24:i+24+size]
            if flags & 0x40000:
                try: d = zlib.decompress(d[4:])
                except Exception: d = b''
            recs.append((t, fid, d, parent))
        i += 24 + size
walk(24 + struct.unpack('<I', b[4:8])[0], len(b), None)

def subs(data):
    i, out, big = 0, [], None
    while i + 6 <= len(data):
        t = data[i:i+4]; n = struct.unpack('<H', data[i+4:i+6])[0]; i += 6
        if t == b'XXXX': big = struct.unpack('<I', data[i:i+4])[0]; i += n; continue
        if big is not None: n, big = big, None
        out.append((t, data[i:i+n])); i += n
    return out
def edid(d):
    for t, s in subs(d):
        if t == b'EDID': return s.split(b'\0')[0].decode('latin1')
    return ''

vtyp, vflag, flst, qust, dial, infos, names = {}, {}, {}, {}, {}, [], {}
for t, fid, d, p in recs:
    if t == b'VTYP':
        vtyp[fid] = edid(d)
        for tt, s in subs(d):
            if tt == b'DNAM' and s: vflag[fid] = s[0]
    elif t == b'FLST': flst[fid] = (edid(d), [struct.unpack('<I', s[:4])[0] for tt, s in subs(d) if tt == b'LNAM' and len(s) >= 4])
    elif t == b'QUST': qust[fid] = d
    elif t == b'DIAL': dial[fid] = d
    elif t == b'INFO': infos.append((fid, d, p))
    if t in (b'FACT', b'QUST', b'FLST', b'VTYP'): names[fid] = t.decode() + ' ' + edid(d)
print('%s: %d VTYP, %d FLST, %d QUST, %d DIAL, %d INFO' % (os.path.basename(esm), len(vtyp), len(flst), len(qust), len(dial), len(infos)))
print('CONTROL target %08X -> %r  DNAM=0x%02X (bit0 Allow Default Dialogue = %d)'
      % (TARGET, vtyp.get(TARGET), vflag.get(TARGET, 0), vflag.get(TARGET, 0) & 1))

def conds(d):
    out = []
    for t, s in subs(d):
        if t == b'CTDA' and len(s) >= 28:
            out.append({'orflag': s[0] & 1, 'op': s[0] >> 5, 'comp': struct.unpack('<f', s[4:8])[0],
                        'fn': struct.unpack('<H', s[8:10])[0], 'p1': struct.unpack('<I', s[12:16])[0],
                        'p2': struct.unpack('<I', s[16:20])[0]})
    return out

sets = {f for f, (n, m) in flst.items() if TARGET in m} | {TARGET}
print('\n=== FormLists holding it: %d ===' % (len(sets) - 1))
for f in sorted(sets - {TARGET}, key=lambda x: flst[x][0]):
    print('   %08X %-34s %d members' % (f, flst[f][0], len(flst[f][1])))

ninfo = collections.Counter()
for fid, d, p in infos: ninfo[p] += 1
dtop = collections.defaultdict(list)
for fid, d in dial.items():
    q, sn = None, ''
    for t, s in subs(d):
        if t == b'QNAM' and len(s) >= 4: q = struct.unpack('<I', s[:4])[0]
        elif t == b'SNAM': sn = s[:4].decode('latin1')
    if q is not None: dtop[q].append((fid, sn, ninfo.get(fid, 0)))

print('\n=== Quests selecting on it (quest CTDA fn=%d). "other gates" are ANDed conditions she must ALSO pass ===' % GETISVOICETYPE)
rows = []
for qf, qd in qust.items():
    cs = conds(qd)
    if not any(c['fn'] == GETISVOICETYPE and c['p1'] in sets for c in cs): continue
    other = ['fn=%d p1=%08X %s' % (c['fn'], c['p1'], names.get(c['p1'], '')) for c in cs
             if c['fn'] != GETISVOICETYPE and not c['orflag']]
    sub = collections.Counter()
    for df, sn, n in dtop.get(qf, []): sub[sn] += n
    rows.append((edid(qd), qf, sum(sub.values()), dict(sub), other))
for nm, qf, tot, sub, other in sorted(rows, key=lambda x: -x[2]):
    print('  %-42s %08X  INFOs %5d%s' % (nm, qf, tot, '   OTHER GATES: ' + '; '.join(other) if other else ''))
    print('        %s' % sub)
print('\nTOTAL %d quests, %d INFOs select on %s' % (len(rows), sum(r[2] for r in rows), vtyp.get(TARGET)))

if phrases:
    def load_strings(path, lenpref):
        out = {}
        if not os.path.exists(path): return out
        d = open(path, 'rb').read()
        cnt = struct.unpack('<I', d[:4])[0]; dstart = 8 + cnt * 8
        for k in range(cnt):
            sid, off = struct.unpack('<II', d[8+k*8:16+k*8]); o = dstart + off
            if lenpref:
                n = struct.unpack('<I', d[o:o+4])[0]; out[sid] = d[o+4:o+4+n].split(b'\0')[0].decode('utf-8', 'replace')
            else:
                out[sid] = d[o:d.index(b'\0', o)].decode('utf-8', 'replace')
        return out
    S = {}
    for fn, lp in (('Fallout4_en.STRINGS', False), ('Fallout4_en.DLSTRINGS', True), ('Fallout4_en.ILSTRINGS', True)):
        for k, v in load_strings(os.path.join(sdir, fn), lp).items(): S.setdefault(k, v)
    want = {sid for sid, v in S.items() if v.strip().lower() in phrases}
    print('\n=== INFOs whose response text is one of %r (%d string ids) ===' % (phrases, len(want)))
    for fid, d, p in infos:
        if not any(t == b'NAM1' and len(s) >= 4 and struct.unpack('<I', s[:4])[0] in want for t, s in subs(d)): continue
        dd = dial.get(p or 0, b''); sn = ''; qn = 0
        for t, s in subs(dd):
            if t == b'SNAM': sn = s[:4].decode('latin1')
            elif t == b'QNAM' and len(s) >= 4: qn = struct.unpack('<I', s[:4])[0]
        reach = 'REACHABLE' if any(r[1] == qn for r in rows) else 'not via this voice type'
        print('INFO %08X | DIAL %08X | %s | quest %08X %s | %s'
              % (fid, p or 0, sn, qn, edid(qust.get(qn, b'')), reach))
        for c in conds(d):
            print('    cond fn=%-5d op=%d comp=%g p1=%08X %s' % (c['fn'], c['op'], c['comp'], c['p1'], names.get(c['p1'], '')))
