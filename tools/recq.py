# recq.py - small read-only record queries against one FO4 plugin (no third-party modules). ASCII only.
#   py -3 -I recq.py <plugin> furn <substr>            FURN records whose EDID contains substr: EDID, MODL, keywords count
#   py -3 -I recq.py <plugin> refs <cellhex> <x> <y> <r>  placed refs in an interior cell within r units: base type/EDID/MODL
#   py -3 -I recq.py <plugin> otft <hex>               an outfit's INAM items with their type/EDID
#   py -3 -I recq.py <plugin> edid <TYPE> <substr>      any record type: EDID (+MODL) containing substr
#   py -3 -I recq.py <plugin> pack <substr>             PACK records whose EDID contains substr: template (PKCU) and data input types (ANAM)
#   py -3 -I recq.py <plugin> dump <hex>                every subrecord of one record: type, size, first bytes
#   py -3 -I recq.py <plugin> dumpe <EDID>              the same, for the record with that EditorID
import io, math, struct, sys, zlib
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')


def subs(buf):
    i = 0; big = None
    while i + 6 <= len(buf):
        t = buf[i:i + 4]; n = struct.unpack_from('<H', buf, i + 4)[0]; i += 6
        if t == b'XXXX':
            big = struct.unpack_from('<I', buf, i)[0]; i += n; continue
        if big is not None:
            n = big; big = None
        yield t.decode('latin1'), buf[i:i + n]; i += n


class P:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        h = struct.unpack_from('<I', self.d, 4)[0]
        self.recs = {}          # fid -> (type, off, size, flags)
        self.cellkids = {}      # cell fid -> [refr fid]
        self._walk(24 + h, len(self.d), None)

    def _walk(self, off, end, cell):
        d = self.d
        while off < end:
            typ = d[off:off + 4]; sz = struct.unpack_from('<I', d, off + 4)[0]
            if typ == b'GRUP':
                label = struct.unpack_from('<I', d, off + 8)[0]; gt = struct.unpack_from('<I', d, off + 12)[0]
                self._walk(off + 24, off + sz, label if gt in (6, 8, 9, 10) else cell); off += sz; continue
            flags, fid = struct.unpack_from('<II', d, off + 8)
            t = typ.decode('latin1')
            self.recs[fid] = (t, off, sz, flags)
            if cell is not None and t in ('REFR', 'ACHR'):
                self.cellkids.setdefault(cell, []).append(fid)
            off += 24 + sz

    def body(self, fid):
        t, off, sz, fl = self.recs[fid]
        b = self.d[off + 24:off + 24 + sz]
        return zlib.decompress(b[4:]) if fl & 0x40000 else b

    def field(self, fid, name):
        for t, v in subs(self.body(fid)):
            if t == name: return v
        return None

    def edid(self, fid):
        v = self.field(fid, 'EDID') if fid in self.recs else None
        return v.rstrip(b'\0').decode('latin1') if v else '?'

    def modl(self, fid):
        v = self.field(fid, 'MODL') if fid in self.recs else None
        return v.rstrip(b'\0').decode('latin1') if v else ''


def main():
    p = P(sys.argv[1]); cmd = sys.argv[2]
    if cmd in ('furn', 'edid'):
        typ = 'FURN' if cmd == 'furn' else sys.argv[3]
        sub = (sys.argv[3] if cmd == 'furn' else sys.argv[4]).lower()
        for fid, (t, *_r) in sorted(p.recs.items()):
            if t == typ and sub in p.edid(fid).lower():
                print('%08X %-44s %s' % (fid, p.edid(fid), p.modl(fid)))
    elif cmd == 'refs':
        cell = int(sys.argv[3], 16); x, y, r = map(float, sys.argv[4:7])
        for fid in p.cellkids.get(cell, []):
            v = p.field(fid, 'DATA'); base = p.field(fid, 'NAME')
            if not v or not base: continue
            px, py_, pz, rx, ry, rz = struct.unpack('<6f', v[:24])
            dist = math.hypot(px - x, py_ - y)
            if dist <= r:
                b = struct.unpack('<I', base)[0]
                bt = p.recs[b][0] if b in p.recs else '?'
                print('%08X d=%5.0f pos=(%6.0f,%6.0f,%5.0f) rz=%5.2f %s %08X %-36s %s' % (fid, dist, px, py_, pz, rz, bt, b, p.edid(b), p.modl(b)))
    elif cmd == 'pack':
        sub = sys.argv[3].lower()
        for fid, (t, *_r) in sorted(p.recs.items()):
            if t != 'PACK' or sub not in p.edid(fid).lower(): continue
            tpl, ins = 0, []
            for st, v in subs(p.body(fid)):
                if st == 'PKCU' and len(v) >= 8: tpl = struct.unpack_from('<I', v, 4)[0]
                elif st == 'ANAM': ins.append(v.rstrip(b'\0').decode('latin1'))
            print('%08X %-48s tpl %08X %-24s %s' % (fid, p.edid(fid), tpl, p.edid(tpl) if tpl else '-', ','.join(ins)))
    elif cmd in ('dump', 'dumpe'):
        if cmd == 'dumpe':
            fid = next(f for f, (t, *_r) in p.recs.items() if p.edid(f).lower() == sys.argv[3].lower())
            print('%08X' % fid)
        else:
            fid = int(sys.argv[3], 16)
        print(p.recs[fid][0], p.edid(fid))
        for st, v in subs(p.body(fid)):
            extra = ''
            if len(v) == 4: extra = ' u32 %08X %s' % (struct.unpack('<I', v)[0], p.edid(struct.unpack('<I', v)[0]) if struct.unpack('<I', v)[0] in p.recs else '')
            print('  %s %4d %s%s' % (st, len(v), v[:40].hex(), extra))
    elif cmd == 'otft':
        fid = int(sys.argv[3], 16)
        for t, v in subs(p.body(fid)):
            if t == 'INAM':
                for i in range(0, len(v), 4):
                    f = struct.unpack_from('<I', v, i)[0]
                    print('%08X %s %s' % (f, p.recs[f][0] if f in p.recs else '?', p.edid(f)))


if __name__ == '__main__':
    main()
