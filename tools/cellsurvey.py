# cellsurvey.py - read-only FO4 interior-cell surveyor (no third-party modules).
# Built for the CLINIC stream (donor-cell choice + door-spot survey); same parsing
# style as fallout/tools/f34rec.py.
#
# usage:
#   python -I cellsurvey.py cells <plugin> [substr]
#       every interior CELL whose EDID contains substr: ref/navmesh counts + floor bounds
#   python -I cellsurvey.py cell <plugin> <localFormIDhex>
#       one cell: every placed ref with base form type/EDID, position, rotation, scale
#   python -I cellsurvey.py navm <plugin> <localFormIDhex>
#       navmesh records parented to that cell: vertex/triangle counts, bounds, door triangles
import struct, sys, zlib

def subs(buf):
    i = 0; big = None
    while i + 6 <= len(buf):
        t = buf[i:i+4]; n = struct.unpack_from('<H', buf, i+4)[0]; i += 6
        if t == b'XXXX':
            big = struct.unpack_from('<I', buf, i)[0]; i += n; continue
        if big is not None:
            n = big; big = None
        yield t.decode('latin1'), buf[i:i+n]; i += n

class Walk:
    """Walks the whole plugin once, remembering each record's owning cell."""
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        self.hsize = struct.unpack_from('<I', d, 4)[0]
        self.masters = []
        for t, v in subs(d[24:24 + self.hsize]):
            if t == 'MAST': self.masters.append(v.rstrip(b'\0').decode('latin1'))
        self.recs = {}          # fid -> (type, flags, off, sz)
        self.cell_children = {} # cell fid -> [fid, ...]
        self.cells = []         # interior cell fids in file order
        self._walk(24 + self.hsize, len(d), None)
    def _walk(self, off, end, cell):
        d = self.data
        while off < end:
            typ = d[off:off + 4]; sz = struct.unpack_from('<I', d, off + 4)[0]
            if typ == b'GRUP':
                label, gtype = struct.unpack_from('<Ii', d, off + 8)
                self._walk(off + 24, off + sz, label if gtype in (6, 8, 9) else cell)
                off += sz; continue
            flags, fid = struct.unpack_from('<II', d, off + 8)
            t = typ.decode('latin1')
            self.recs[fid] = (t, flags, off, sz)
            if t == 'CELL':
                self.cells.append(fid)
            elif cell is not None:
                self.cell_children.setdefault(cell, []).append(fid)
            off += 24 + sz
    def body(self, fid):
        t, flags, off, sz = self.recs[fid]
        b = self.data[off + 24:off + 24 + sz]
        if flags & 0x40000:
            b = zlib.decompress(b[4:])
        return b
    def edid(self, fid):
        if fid not in self.recs: return None
        for t, v in subs(self.body(fid)):
            if t == 'EDID': return v.rstrip(b'\0').decode('latin1')
        return ''

def ref_info(w, fid):
    """(baseFid, pos(x,y,z), rotZ, scale, boundRadius-ish) for a REFR/ACHR, or None."""
    base = None; pos = None; rot = None; scale = 1.0
    for t, v in subs(w.body(fid)):
        if t == 'NAME' and len(v) >= 4: base = struct.unpack_from('<I', v)[0]
        elif t == 'DATA' and len(v) >= 24:
            x, y, z, rx, ry, rz = struct.unpack_from('<6f', v)
            pos = (x, y, z); rot = rz
        elif t == 'XSCL' and len(v) >= 4: scale = struct.unpack_from('<f', v)[0]
    if base is None or pos is None: return None
    return base, pos, rot, scale

def cell_flags(w, fid):
    for t, v in subs(w.body(fid)):
        if t == 'DATA' and len(v) >= 1:
            return v[0] | (v[1] << 8 if len(v) > 1 else 0)
    return 0

def full_name_present(w, fid):
    for t, v in subs(w.body(fid)):
        if t == 'FULL': return True
    return False

def cmd_cells(path, sub):
    w = Walk(path)
    print('cell      EDID                                     refs  navm  prevised  floor extent (x*y u)')
    for fid in w.cells:
        if not (cell_flags(w, fid) & 0x1):   # interior flag
            continue
        e = w.edid(fid) or ''
        if sub.lower() not in e.lower(): continue
        kids = w.cell_children.get(fid, [])
        refs = [k for k in kids if w.recs[k][0] in ('REFR', 'ACHR')]
        navm = [k for k in kids if w.recs[k][0] == 'NAVM']
        visi = any(t == 'VISI' for t, _ in subs(w.body(fid)))
        xs = []; ys = []
        for r in refs:
            ri = ref_info(w, r)
            if ri: xs.append(ri[1][0]); ys.append(ri[1][1])
        ext = ('%5.0f*%-5.0f' % (max(xs) - min(xs), max(ys) - min(ys))) if xs else '    ?'
        print('%08X  %-40s %4d  %4d  %-8s  %s' % (fid, e[:40], len(refs), len(navm), 'yes' if visi else 'no', ext))

def cmd_cell(path, want):
    w = Walk(path)
    kids = w.cell_children.get(want, [])
    print('%d children of cell %08X (%s)' % (len(kids), want, w.edid(want)))
    for k in kids:
        t = w.recs[k][0]
        if t not in ('REFR', 'ACHR'):
            print('%08X  %-4s' % (k, t)); continue
        ri = ref_info(w, k)
        if not ri:
            print('%08X  %-4s (no DATA)' % (k, t)); continue
        base, pos, rot, scale = ri
        bt, be = (w.recs[base][0], w.edid(base)) if base in w.recs else ('?', '%08X' % base)
        print('%08X  %-4s base %-4s %-38s pos %8.1f %8.1f %8.1f rotZ %6.3f%s' %
              (k, t, bt, (be or '')[:38], pos[0], pos[1], pos[2], rot or 0,
               ('' if abs(scale - 1.0) < 1e-3 else ' scale %.2f' % scale)))

def cmd_navm(path, want):
    w = Walk(path)
    for fid, (t, flags, off, sz) in sorted(w.recs.items()):
        if t != 'NAVM': continue
        body = w.body(fid)
        for st, v in subs(body):
            if st != 'NVNM': continue
            ver, crc = struct.unpack_from('<II', v, 0)
            # parent: 8 bytes (worldspace fid + cell fid for interiors: first 4 bytes zero)
            p1, p2 = struct.unpack_from('<II', v, 8)
            cell = p2 if p1 == 0 else None
            if cell != want: continue
            o = 16
            nv = struct.unpack_from('<I', v, o)[0]; o += 4
            verts = struct.unpack_from('<%df' % (nv * 3), v, o); o += nv * 12
            nt = struct.unpack_from('<I', v, o)[0]; o += 4
            door_tris = 0
            for i in range(nt):
                tri = v[o + i * 21: o + (i + 1) * 21]
                flags16 = struct.unpack_from('<H', tri, 17)[0]
                if flags16 & 0x0200: door_tris += 1
            xs = verts[0::3]; ys = verts[1::3]; zs = verts[2::3]
            print('NAVM %08X: ver %d crc %08X verts %d tris %d doorTris %d  x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f'
                  % (fid, ver, crc, nv, nt, door_tris, min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))

if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'cells': cmd_cells(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else '')
    elif mode == 'cell': cmd_cell(sys.argv[2], int(sys.argv[3], 16))
    elif mode == 'navm': cmd_navm(sys.argv[2], int(sys.argv[3], 16))
    else: raise SystemExit('unknown mode')
