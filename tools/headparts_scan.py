# headparts_scan.py - which head parts would crash a runtime-built head in THIS load order (read-only). ASCII only.
#
# Why: our ripperdocs have no FaceGen, so the engine builds their heads at runtime from head parts. A head part whose
# winning NIF carries cloth data (BSClothExtraData) crashed the game in hclClothInstance::setTransformSet when Dr.
# Varga's cell loaded (crash-2026-11-09-00-07-47.log, HairFemale01). This lists, for HumanRace (and GhoulRace) and for
# every female/male HDPT of the asked types: the winning HDPT record (last plugin in the load order), its model, the
# winning NIF (loose file in the highest-priority enabled MO2 mod, else the BA2s in load order, else vanilla BA2s) and
# whether that NIF holds cloth data. Extra parts (HNAM) are followed.
#
#   py -3 -I headparts_scan.py --profile "<MO2>\profiles\<profile>" --mods <MO2>\mods
#       --data "<game>\Data" [--types hair,eyebrows,eyes,face,facialhair,teeth,headrear,misc]
#       [--json out.json]
import argparse, io, json, os, struct, sys, zlib
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

PNAM = {0: 'misc', 1: 'face', 2: 'eyes', 3: 'hair', 4: 'facialhair', 5: 'scar', 6: 'eyebrows', 7: 'meatcaps', 8: 'teeth', 9: 'headrear'}
IMPLICIT = ['Fallout4.esm', 'DLCRobot.esm', 'DLCworkshop01.esm', 'DLCCoast.esm', 'DLCworkshop02.esm', 'DLCworkshop03.esm', 'DLCNukaWorld.esm']


def subs(buf):
    i = 0; big = None
    while i + 6 <= len(buf):
        t = buf[i:i + 4]; n = struct.unpack_from('<H', buf, i + 4)[0]; i += 6
        if t == b'XXXX':
            big = struct.unpack_from('<I', buf, i)[0]; i += n; continue
        if big is not None:
            n = big; big = None
        yield t.decode('latin1'), buf[i:i + n]; i += n


class Plugin:
    def __init__(self, path, keep):
        self.name = os.path.basename(path)
        d = open(path, 'rb').read()
        hsize = struct.unpack_from('<I', d, 4)[0]
        self.masters = [v.rstrip(b'\0').decode('latin1') for t, v in subs(d[24:24 + hsize]) if t == 'MAST']
        self.recs = {}
        self._walk(d, 24 + hsize, len(d), keep)

    def _walk(self, d, off, end, keep):
        while off < end:
            typ = d[off:off + 4]; sz = struct.unpack_from('<I', d, off + 4)[0]
            if typ == b'GRUP':
                gtype = struct.unpack_from('<I', d, off + 12)[0]
                label = d[off + 8:off + 12]
                if gtype == 0 and label.decode('latin1') not in keep:
                    off += sz; continue
                self._walk(d, off + 24, off + sz, keep); off += sz; continue
            flags, fid = struct.unpack_from('<II', d, off + 8)
            t = typ.decode('latin1')
            if t in keep:
                b = d[off + 24:off + 24 + sz]
                if flags & 0x40000:
                    b = zlib.decompress(b[4:])
                self.recs[fid] = (t, b)
            off += 24 + sz

    def gkey(self, fid):
        """global key (owning plugin name, local id) of a form id as written in this plugin"""
        idx = fid >> 24
        owner = self.masters[idx] if idx < len(self.masters) else self.name
        return (owner.lower(), fid & 0xFFFFFF)


def read_lines(p):
    return [l.rstrip('\r\n') for l in open(p, encoding='utf-8', errors='replace')] if os.path.exists(p) else []


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--profile', required=True); a.add_argument('--mods', required=True); a.add_argument('--data', required=True)
    a.add_argument('--types', default='hair,eyebrows,eyes,face,facialhair,teeth,headrear,misc,meatcaps,scar')
    a.add_argument('--json', default='')
    o = a.parse_args()
    want_types = set(o.types.split(','))

    # MO2: modlist.txt lists the HIGHEST priority first; '+' = enabled
    mods = [l[1:] for l in read_lines(os.path.join(o.profile, 'modlist.txt')) if l.startswith('+')]
    plugins = [l.lstrip('*') for l in read_lines(os.path.join(o.profile, 'plugins.txt')) if l.startswith('*')]
    order = IMPLICIT + [p for p in plugins if p not in IMPLICIT]

    def find_file(rel):
        for m in mods:
            p = os.path.join(o.mods, m, rel)
            if os.path.exists(p): return p, m
        p = os.path.join(o.data, rel)
        return (p, 'Data') if os.path.exists(p) else (None, None)

    hdpt, race = {}, {}
    for name in order:
        path, _ = find_file(name)
        if not path: continue
        try:
            pl = Plugin(path, {'HDPT', 'RACE'})
        except Exception as e:
            print('skip', name, e, file=sys.stderr); continue
        for fid, (t, b) in pl.recs.items():
            k = pl.gkey(fid)
            (hdpt if t == 'HDPT' else race)[k] = (name, b, pl)

    def hd_info(k):
        name, b, pl = hdpt[k]
        info = {'plugin': name, 'edid': '', 'model': '', 'type': None, 'female': False, 'male': False, 'extra': []}
        for t, v in subs(b):
            if t == 'EDID': info['edid'] = v.rstrip(b'\0').decode('latin1')
            elif t == 'MODL': info['model'] = v.rstrip(b'\0').decode('latin1')
            elif t == 'PNAM' and len(v) == 4: info['type'] = PNAM.get(struct.unpack('<I', v)[0], str(struct.unpack('<I', v)[0]))
            elif t == 'DATA' and len(v) >= 1:
                info['male'] = bool(v[0] & 0x02); info['female'] = bool(v[0] & 0x04)
            elif t == 'HNAM' and len(v) == 4: info['extra'].append(pl.gkey(struct.unpack('<I', v)[0]))
        return info

    # every BA2 the game would load, plugin-named ones in load order, then the vanilla ones; last loaded wins
    ba2s = []
    # every .ba2 on disk, indexed ONCE: lower-case name -> path (highest-priority mod wins, the stock Data folder last)
    ba2_on_disk = {}
    for f in (os.listdir(o.data) if os.path.isdir(o.data) else []):
        if f.lower().endswith('.ba2'): ba2_on_disk[f.lower()] = os.path.join(o.data, f)
    for m in reversed(mods):
        d = os.path.join(o.mods, m)
        try:
            for f in os.listdir(d):
                if f.lower().endswith('.ba2'): ba2_on_disk[f.lower()] = os.path.join(d, f)
        except OSError:
            pass
    for name in order:
        stem = os.path.splitext(name)[0].lower() + ' - '
        ba2s += [p for f, p in sorted(ba2_on_disk.items()) if f.startswith(stem)]
    ba2_index = {}
    for p in ba2s:
        try:
            f = open(p, 'rb')
            magic, ver, typ, n, nto = struct.unpack('<4sI4sIQ', f.read(24))
            if magic != b'BTDX' or typ != b'GNRL': continue
            recs = [struct.unpack('<I4sIIQIII', f.read(36)) for _ in range(n)]
            f.seek(nto)
            for r in recs:
                l = struct.unpack('<H', f.read(2))[0]
                nm = f.read(l).decode('latin1').lower().replace('/', '\\')
                ba2_index[nm] = (p, r)            # later archive overrides earlier
        except Exception as e:
            print('ba2 skip', p, e, file=sys.stderr)

    def nif_bytes(model):
        rel = os.path.join('Meshes', model) if not model.lower().startswith('meshes') else model
        p, m = find_file(rel)
        if p and m != 'Data':
            return open(p, 'rb').read(), 'loose:' + m
        key = rel.lower().replace('/', '\\')
        if key in ba2_index:
            ap, (nh, ext, dh, fl, off, psz, usz, al) = ba2_index[key]
            f = open(ap, 'rb'); f.seek(off)
            b = f.read(psz) if psz else f.read(usz)
            return (zlib.decompress(b) if psz else b), 'ba2:' + os.path.basename(ap)
        if p:
            return open(p, 'rb').read(), 'loose:Data'
        return None, 'MISSING'

    def verdict(k, seen=None):
        seen = seen or set()
        if k in seen or k not in hdpt: return []
        seen.add(k)
        i = hd_info(k)
        b, src = nif_bytes(i['model']) if i['model'] else (b'', 'no model')
        cloth = bool(b) and b'BSClothExtraData' in b
        rows = [{'key': '%s|%06X' % k, **{x: i[x] for x in ('edid', 'type', 'female', 'male', 'plugin', 'model')}, 'nif': src, 'cloth': cloth}]
        for e in i['extra']:
            rows += verdict(e, seen)
        return rows

    out = {'races': {}, 'parts': []}
    for rk, label in ((('fallout4.esm', 0x013746), 'HumanRace'), (('fallout4.esm', 0x0EAFB6), 'GhoulRace')):
        if rk not in race: continue
        name, b, pl = race[rk]
        sex = None; heads = {'male': [], 'female': []}
        for t, v in subs(b):
            if t == 'MNAM' and len(v) == 0: sex = 'male'
            elif t == 'FNAM' and len(v) == 0: sex = 'female'
            elif t == 'HEAD' and len(v) == 4 and sex: heads[sex].append(pl.gkey(struct.unpack('<I', v)[0]))
        out['races'][label] = {'plugin': name, 'female': [r for k in heads['female'] for r in verdict(k)],
                               'male': [r for k in heads['male'] for r in verdict(k)]}
    for k in sorted(hdpt):
        i = hd_info(k)
        if i['type'] in want_types:
            out['parts'] += verdict(k)
    if o.json:
        json.dump(out, open(o.json, 'w'), indent=1)
    for label, r in out['races'].items():
        for sex in ('female', 'male'):
            print('%s %s defaults (from %s):' % (label, sex, r['plugin']))
            for x in r[sex]:
                print('   %-5s %-10s %-34s %-28s %s' % ('CLOTH' if x['cloth'] else 'ok', x['type'], x['edid'], x['nif'][:28], x['model']))
    cl = [x for x in out['parts'] if x['cloth']]
    print('parts scanned %d, with cloth %d' % (len(out['parts']), len(cl)))
    for x in cl:
        print('   CLOTH %-10s %-34s %s %s' % (x['type'], x['edid'], x['nif'], x['model']))


if __name__ == '__main__':
    main()
