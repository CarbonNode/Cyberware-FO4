# navwin_scan.py - which plugin WINS the navmesh (and the CELL record) of given cells in THIS
# load order (read-only). ASCII only. Written for the ripperdoc chair approach check: the vanilla
# NAVM said the chair entries are open, but with 1300+ plugins the winning navmesh may not be
# vanilla (PRP, BTInteriors, Goodneighbor/Institute overhauls all rebuild interiors).
#
#   py -3 -I navwin_scan.py --profile "<MO2>\profiles\<profile>"
#       --mods <MO2>\mods --data "<game>\Data"
#       --cells 022884,153795 [--navms 106E6D,1537A5]
#
# Prints, in load order, every enabled plugin that (a) overrides one of the named vanilla NAVM
# form ids, (b) carries ANY NAVM whose NVNM parent is one of the named Fallout4.esm cells
# (a replacement navmesh under a new form id), or (c) overrides the CELL record itself.
# The LAST line per cell is the winner the game uses.
import argparse, io, os, struct, sys, zlib
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

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


def read_lines(p):
    return [l.rstrip('\r\n') for l in open(p, encoding='utf-8', errors='replace')] if os.path.exists(p) else []


def scan_plugin(path, name, cells, navm_ids, refs=frozenset()):
    """cells/navm_ids/refs: sets of (master_lower, local_id). Returns list of hit strings."""
    d = open(path, 'rb').read()
    hsize = struct.unpack_from('<I', d, 4)[0]
    masters = [v.rstrip(b'\0').decode('latin1') for t, v in subs(d[24:24 + hsize]) if t == 'MAST']
    me = name

    def gkey(fid):
        idx = fid >> 24
        owner = masters[idx] if idx < len(masters) else me
        return (owner.lower(), fid & 0xFFFFFF)

    hits = []
    has_navm = b'NAVM' in d       # cheap triage for the NVNM walk
    # cheap triage for the whole walk: a CELL override must carry the raw local id bytes
    # somewhere in the file (false positives only cost a walk)
    if not has_navm and not any(struct.pack('<I', c[1])[:3] in d for c in (list(cells) + list(refs))):
        return hits

    def walk(off, end):
        while off < end:
            typ = d[off:off + 4]; sz = struct.unpack_from('<I', d, off + 4)[0]
            if typ == b'GRUP':
                walk(off + 24, off + sz); off += sz; continue
            flags, fid = struct.unpack_from('<II', d, off + 8)
            if typ == b'CELL' and gkey(fid) in cells:
                hits.append('CELL override %s|%06X' % gkey(fid))
            elif typ in (b'REFR', b'ACHR') and gkey(fid) in refs:
                body = d[off + 24:off + 24 + sz]
                if flags & 0x40000:
                    try:
                        body = zlib.decompress(body[4:])
                    except Exception:
                        body = b''
                xloc = ''
                for st, v in subs(body):
                    if st == 'XLOC':
                        xloc = ' XLOC ' + v.hex()
                hits.append('%s override %s|%06X flags %08X%s' % (typ.decode(), gkey(fid)[0], gkey(fid)[1], flags, xloc or ' (no XLOC)'))
            elif typ == b'NAVM' and has_navm:
                k = gkey(fid)
                body = d[off + 24:off + 24 + sz]
                if flags & 0x40000:
                    try:
                        body = zlib.decompress(body[4:])
                    except Exception:
                        body = b''
                parent = None
                for st, v in subs(body):
                    if st == 'NVNM' and len(v) >= 16:
                        ws, cellfid = struct.unpack_from('<II', v, 8)
                        if ws == 0:
                            parent = gkey(cellfid)
                        break
                if k in navm_ids:
                    hits.append('NAVM override %s|%06X (parent %s)' % (k[0], k[1], parent))
                elif parent in cells:
                    hits.append('NEW NAVM %06X parented to %s|%06X' % (fid & 0xFFFFFF, parent[0], parent[1]))
            off += 24 + sz

    walk(24 + hsize, len(d))
    return hits


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--profile', required=True); a.add_argument('--mods', required=True); a.add_argument('--data', required=True)
    a.add_argument('--cells', required=True); a.add_argument('--navms', default=''); a.add_argument('--refs', default='')
    o = a.parse_args()
    cells = {('fallout4.esm', int(x, 16)) for x in o.cells.split(',') if x}
    navm_ids = {('fallout4.esm', int(x, 16)) for x in o.navms.split(',') if x}
    refs = {('fallout4.esm', int(x, 16)) for x in o.refs.split(',') if x}

    mods = [l[1:] for l in read_lines(os.path.join(o.profile, 'modlist.txt')) if l.startswith('+')]
    plugins = [l.lstrip('*') for l in read_lines(os.path.join(o.profile, 'plugins.txt')) if l.startswith('*')]
    order = IMPLICIT + [p for p in plugins if p not in IMPLICIT]

    def find_file(rel):
        for m in mods:
            p = os.path.join(o.mods, m, rel)
            if os.path.exists(p):
                return p
        p = os.path.join(o.data, rel)
        return p if os.path.exists(p) else None

    n = 0
    for name in order:
        path = find_file(name)
        if not path:
            continue
        n += 1
        try:
            hits = scan_plugin(path, name, cells, navm_ids, refs)
        except Exception as e:
            print('skip %s: %s' % (name, e)); continue
        for hline in hits:
            print('%4d %-52s %s' % (n, name, hline))
    print('scanned %d plugins; the LAST line per cell/navm wins' % n)


if __name__ == '__main__':
    main()
