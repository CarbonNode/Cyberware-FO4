# furnmark_scan.py - the WINNING NIF of given furniture models in THIS load order, and the
# BSFurnitureMarkerNode entries inside it (offset, heading, animation type, entry sides).
# Read-only; ASCII only. This is the ground truth for where an NPC may enter a furniture
# and which way the approach points, for the ripperdoc chair placement check.
#
#   py -3 -I furnmark_scan.py --profile "<MO2>\profiles\<profile>"
#       --mods <MO2>\mods --data "<game>\Data"
#       --models "Furniture\MemoryLounge\MemoryLounger01.nif;Markers\ReclamationChairMarker.nif"
#       [--save <out dir>]
import argparse, io, os, struct, sys, zlib
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

IMPLICIT = ['Fallout4.esm', 'DLCRobot.esm', 'DLCworkshop01.esm', 'DLCCoast.esm', 'DLCworkshop02.esm', 'DLCworkshop03.esm', 'DLCNukaWorld.esm']
ENTRY = {1: 'front', 2: 'behind', 4: 'right', 8: 'left', 16: 'up'}
ANIM = {1: 'sit', 2: 'sleep', 4: 'lean'}


def read_lines(p):
    return [l.rstrip('\r\n') for l in open(p, encoding='utf-8', errors='replace')] if os.path.exists(p) else []


def parse_nif_furniture_markers(b):
    """Return (list of marker lists, note). FO4 NIF: version 20.2.0.7, BS stream 130."""
    nl = b.index(b'\n')
    off = nl + 1
    version, endian, user = struct.unpack_from('<IBI', b, off); off += 9
    nblocks = struct.unpack_from('<I', b, off)[0]; off += 4
    bsver = struct.unpack_from('<I', b, off)[0]; off += 4
    # author, process script, export script, and (BS >= 103) max filepath - all u8-length strings
    for _ in range(4 if bsver >= 103 else 3):
        ln = b[off]; off += 1 + ln
    ntypes = struct.unpack_from('<H', b, off)[0]; off += 2
    types = []
    for _ in range(ntypes):
        ln = struct.unpack_from('<I', b, off)[0]; off += 4
        types.append(b[off:off + ln].decode('latin1')); off += ln
    tindex = list(struct.unpack_from('<%dH' % nblocks, b, off)); off += 2 * nblocks
    sizes = list(struct.unpack_from('<%dI' % nblocks, b, off)); off += 4 * nblocks
    nstr, _maxlen = struct.unpack_from('<II', b, off); off += 8
    for _ in range(nstr):
        ln = struct.unpack_from('<I', b, off)[0]; off += 4 + ln
    ngroups = struct.unpack_from('<I', b, off)[0]; off += 4 + 4 * ngroups
    out = []
    for i in range(nblocks):
        if types[tindex[i] & 0x7FFF] == 'BSFurnitureMarkerNode':
            d = b[off:off + sizes[i]]
            npos = struct.unpack_from('<I', d, 4)[0]
            marks = []
            for j in range(npos):
                ox, oy, oz, heading = struct.unpack_from('<4f', d, 8 + 20 * j)
                at, ep = struct.unpack_from('<HH', d, 8 + 20 * j + 16)
                marks.append((ox, oy, oz, heading, at, ep))
            out.append(marks)
        off += sizes[i]
    return out, '%d blocks, bs %d' % (nblocks, bsver)


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--profile', required=True); a.add_argument('--mods', required=True); a.add_argument('--data', required=True)
    a.add_argument('--models', required=True); a.add_argument('--save', default='')
    a.add_argument('--raw', action='store_true', help='models are full Data-relative paths (any file): find the winning copy, save it, skip the NIF parse')
    o = a.parse_args()

    mods = [l[1:] for l in read_lines(os.path.join(o.profile, 'modlist.txt')) if l.startswith('+')]
    plugins = [l.lstrip('*') for l in read_lines(os.path.join(o.profile, 'plugins.txt')) if l.startswith('*')]
    order = IMPLICIT + [p for p in plugins if p not in IMPLICIT]

    def find_file(rel):
        for m in mods:
            p = os.path.join(o.mods, m, rel)
            if os.path.exists(p):
                return p, m
        p = os.path.join(o.data, rel)
        return (p, 'Data') if os.path.exists(p) else (None, None)

    ba2_on_disk = {}
    for f in (os.listdir(o.data) if os.path.isdir(o.data) else []):
        if f.lower().endswith('.ba2'):
            ba2_on_disk[f.lower()] = os.path.join(o.data, f)
    for m in reversed(mods):
        d = os.path.join(o.mods, m)
        try:
            for f in os.listdir(d):
                if f.lower().endswith('.ba2'):
                    ba2_on_disk[f.lower()] = os.path.join(d, f)
        except OSError:
            pass
    ba2s = []
    for name in order:
        stem = os.path.splitext(name)[0].lower() + ' - '
        ba2s += [p for f, p in sorted(ba2_on_disk.items()) if f.startswith(stem)]
    ba2_index = {}
    for p in ba2s:
        try:
            f = open(p, 'rb')
            magic, ver, typ, n, nto = struct.unpack('<4sI4sIQ', f.read(24))
            if magic != b'BTDX' or typ != b'GNRL':
                continue
            recs = [struct.unpack('<I4sIIQIII', f.read(36)) for _ in range(n)]
            f.seek(nto)
            for r in recs:
                l = struct.unpack('<H', f.read(2))[0]
                nm = f.read(l).decode('latin1').lower().replace('/', '\\')
                ba2_index[nm] = (p, r)
        except Exception as e:
            print('ba2 skip', p, e, file=sys.stderr)

    def nif_bytes(model):
        rel = model if o.raw or model.lower().startswith('meshes') else os.path.join('Meshes', model)
        p, m = find_file(rel)
        if p and m != 'Data':
            return open(p, 'rb').read(), 'loose:' + m
        key = rel.lower().replace('/', '\\')
        if key in ba2_index:
            ap, (nh, ext, dh, fl, offs, psz, usz, al) = ba2_index[key]
            f = open(ap, 'rb'); f.seek(offs)
            b = f.read(psz) if psz else f.read(usz)
            return (zlib.decompress(b) if psz else b), 'ba2:' + os.path.basename(ap)
        if p:
            return open(p, 'rb').read(), 'loose:Data'
        return None, 'MISSING'

    for model in o.models.split(';'):
        model = model.strip()
        if not model:
            continue
        b, src = nif_bytes(model)
        print('%s  <- %s%s' % (model, src, '' if b else '  NOT FOUND'))
        if not b:
            continue
        if o.save:
            os.makedirs(o.save, exist_ok=True)
            fn = os.path.join(o.save, os.path.basename(model))
            open(fn, 'wb').write(b)
            print('   saved %s (%d bytes)' % (fn, len(b)))
        if o.raw:
            continue
        try:
            nodes, note = parse_nif_furniture_markers(b)
        except Exception as e:
            print('   parse FAILED: %r' % e)
            continue
        print('   %s, %d BSFurnitureMarkerNode block(s)' % (note, len(nodes)))
        for marks in nodes:
            for ox, oy, oz, heading, at, ep in marks:
                eps = '+'.join(v for k, v in ENTRY.items() if ep & k) or hex(ep)
                ats = '+'.join(v for k, v in ANIM.items() if at & k) or hex(at)
                print('   marker offset (%7.2f, %7.2f, %7.2f) heading %6.3f rad  anim %-6s entry %s'
                      % (ox, oy, oz, heading, ats, eps))


if __name__ == '__main__':
    main()
