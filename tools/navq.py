# navq.py - read-only FO4 NAVM (NVNM v15) queries against one plugin. No third-party modules. ASCII only.
#   py -3 -I navq.py <plugin> mesh <cellhex>                      every NAVM in the cell: verts, tris, bbox
#   py -3 -I navq.py <plugin> point <cellhex> <x> <y> <z>         containing triangle + edge clearance, or nearest triangle
#   py -3 -I navq.py <plugin> entry <cellhex> <x> <y> <z> <rz> [fwd]
#       furniture approach check: the point fwd units (default 110) along facing (sin rz, cos rz)
#       from (x,y,z), plus the spot itself; prints each with its `point` verdict
#   py -3 -I navq.py <plugin> reach <cellhex> <x1> <y1> <z1> <x2> <y2> <z2>
#       is there a walkable triangle path between the two points (BFS over triangle adjacency,
#       within one NAVM); prints the bottleneck edges the path squeezes through
#
# NVNM v15 layout (derived from Fallout4.esm cell 022884, probe 2026-11-09): u32 version(15),
# u32 magic, u32 parent worldspace (0 for interiors), u32 parent cell, u32 vertCount,
# vertCount * 3 float32, u32 triCount, triCount * 21 bytes: 3x u16 vertex index, 3x u16
# neighbor triangle index (0xFFFF = none; edge i joins verts i and i+1), f32 (height-like),
# flag bytes. After the triangles: u32 edge-link count + 10-byte entries, u32 door-triangle
# count + 10-byte entries (u16 triangle, u32 crc-like, u32 door/mesh form id).
import math, struct, sys, zlib


def subs(buf):
    i = 0; big = None
    while i + 6 <= len(buf):
        t = buf[i:i + 4]; n = struct.unpack_from('<H', buf, i + 4)[0]; i += 6
        if t == b'XXXX':
            big = struct.unpack_from('<I', buf, i)[0]; i += n; continue
        if big is not None:
            n = big; big = None
        yield t.decode('latin1'), buf[i:i + n]; i += n


def navms_of_cell(path, cell_id):
    d = open(path, 'rb').read()
    h = struct.unpack_from('<I', d, 4)[0]
    found = []

    def walk(off, end, cell):
        while off < end:
            typ = d[off:off + 4]; sz = struct.unpack_from('<I', d, off + 4)[0]
            if typ == b'GRUP':
                label = struct.unpack_from('<I', d, off + 8)[0]
                gt = struct.unpack_from('<I', d, off + 12)[0]
                walk(off + 24, off + sz, label if gt in (6, 8, 9, 10) else cell)
                off += sz; continue
            flags, fid = struct.unpack_from('<II', d, off + 8)
            if typ == b'NAVM' and cell == cell_id:
                body = d[off + 24:off + 24 + sz]
                if flags & 0x40000:
                    body = zlib.decompress(body[4:])
                found.append((fid, body))
            off += 24 + sz

    walk(24 + h, len(d), None)
    meshes = []
    for fid, body in found:
        for st, v in subs(body):
            if st != 'NVNM':
                continue
            ver = struct.unpack_from('<I', v, 0)[0]
            if ver != 15:
                raise SystemExit('NAVM %08X: NVNM version %d, only 15 is supported' % (fid, ver))
            nv = struct.unpack_from('<I', v, 16)[0]
            verts = [struct.unpack_from('<3f', v, 20 + 12 * i) for i in range(nv)]
            to = 20 + 12 * nv
            nt = struct.unpack_from('<I', v, to)[0]
            tris = [struct.unpack_from('<3H', v, to + 4 + 21 * i) for i in range(nt)]
            edges = [struct.unpack_from('<3H', v, to + 4 + 21 * i + 6) for i in range(nt)]
            meshes.append((fid, verts, tris, edges))
    return meshes


def tri_xy(verts, tri):
    return [(verts[i][0], verts[i][1]) for i in tri]


def in_tri_2d(p, a, b, c):
    def cross(o, u, w):
        return (u[0] - o[0]) * (w[1] - o[1]) - (u[1] - o[1]) * (w[0] - o[0])
    d1, d2, d3 = cross(a, b, p), cross(b, c, p), cross(c, a, p)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)


def seg_dist_2d(p, a, b):
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L2))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def classify(meshes, x, y, z, ztol=120.0):
    # (verdict, detail). Containment is 2D; a triangle counts when its mean vertex z is within
    # ztol of z (keeps the upstairs floor from answering for the basement).
    best_out = None
    for fid, verts, tris, _edges in meshes:
        # boundary edges: vertex pairs used by exactly one triangle
        use = {}
        for t in tris:
            for e in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
                use[frozenset(e)] = use.get(frozenset(e), 0) + 1
        for ti, t in enumerate(tris):
            zm = sum(verts[i][2] for i in t) / 3.0
            if abs(zm - z) > ztol:
                continue
            a, b, c = tri_xy(verts, t)
            if in_tri_2d((x, y), a, b, c):
                clear = None
                for e in use:
                    if use[e] != 1:
                        continue
                    i, j = tuple(e)
                    if abs((verts[i][2] + verts[j][2]) / 2.0 - z) > ztol:
                        continue
                    dd = seg_dist_2d((x, y), (verts[i][0], verts[i][1]), (verts[j][0], verts[j][1]))
                    clear = dd if clear is None else min(clear, dd)
                return ('ON', 'NAVM %08X tri %d z %.0f, boundary clearance %.0f u'
                        % (fid, ti, zm, -1 if clear is None else clear))
            dd = min(seg_dist_2d((x, y), a, b), seg_dist_2d((x, y), b, c), seg_dist_2d((x, y), c, a))
            if best_out is None or dd < best_out[0]:
                best_out = (dd, fid, ti, zm)
    if best_out is None:
        return ('OFF', 'no triangle within z tolerance')
    return ('OFF', 'nearest tri %d of NAVM %08X at %.0f u (z %.0f)'
            % (best_out[2], best_out[1], best_out[0], best_out[3]))


def main():
    path, cmd, cellhex = sys.argv[1], sys.argv[2], sys.argv[3]
    meshes = navms_of_cell(path, int(cellhex, 16))
    if not meshes:
        raise SystemExit('no NAVM with an NVNM in cell ' + cellhex)
    if cmd == 'mesh':
        for fid, verts, tris, _edges in meshes:
            xs = [v[0] for v in verts]; ys = [v[1] for v in verts]; zs = [v[2] for v in verts]
            print('NAVM %08X: %d verts, %d tris, bbox x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f'
                  % (fid, len(verts), len(tris), min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
    elif cmd == 'point':
        x, y, z = map(float, sys.argv[4:7])
        v, det = classify(meshes, x, y, z)
        print('%s (%.0f, %.0f, %.0f): %s' % (v, x, y, z, det))
    elif cmd == 'entry':
        x, y, z, rz = map(float, sys.argv[4:8])
        fwd = float(sys.argv[8]) if len(sys.argv) > 8 else 110.0
        s, c = math.sin(rz), math.cos(rz)
        pts = [('spot', x, y), ('front entry +%du' % fwd, x + fwd * s, y + fwd * c)]
        for name, px, py in pts:
            v, det = classify(meshes, px, py, z)
            print('%-18s %s (%.0f, %.0f): %s' % (name, v, px, py, det))
    elif cmd == 'reach':
        x1, y1, z1, x2, y2, z2 = map(float, sys.argv[4:10])

        def containing(px, py, pz):
            for mi, (fid, verts, tris, edges) in enumerate(meshes):
                for ti, t in enumerate(tris):
                    zm = sum(verts[i][2] for i in t) / 3.0
                    if abs(zm - pz) > 120.0:
                        continue
                    a, b, c = tri_xy(verts, t)
                    if in_tri_2d((px, py), a, b, c):
                        return mi, ti
            return None, None

        m1, t1 = containing(x1, y1, z1)
        m2, t2 = containing(x2, y2, z2)
        if m1 is None or m2 is None:
            raise SystemExit('a point is not on any triangle (A %s, B %s)' % (t1, t2))
        if m1 != m2:
            print('UNREACHED: the points are on different NAVM records'); return
        fid, verts, tris, edges = meshes[m1]
        prev = {t1: None}
        q = [t1]
        while q and t2 not in prev:
            cur = q.pop(0)
            for nb in edges[cur]:
                if nb != 0xFFFF and nb < len(tris) and nb not in prev:
                    prev[nb] = cur; q.append(nb)
        if t2 not in prev:
            print('UNREACHED: no triangle path from tri %d to tri %d in NAVM %08X' % (t1, t2, fid))
            return
        path = []
        cur = t2
        while cur is not None:
            path.append(cur); cur = prev[cur]
        path.reverse()
        # bottleneck: the narrowest crossed edge (the shared edge between consecutive tris)
        worst = None
        for i in range(len(path) - 1):
            a, b = path[i], path[i + 1]
            shared = set(tris[a]) & set(tris[b])
            if len(shared) == 2:
                i1, i2 = tuple(shared)
                w = math.hypot(verts[i1][0] - verts[i2][0], verts[i1][1] - verts[i2][1])
                mid = ((verts[i1][0] + verts[i2][0]) / 2, (verts[i1][1] + verts[i2][1]) / 2)
                if worst is None or w < worst[0]:
                    worst = (w, mid, a, b)
        print('REACHED: %d triangles, NAVM %08X, tri %d -> tri %d' % (len(path), fid, t1, t2))
        if worst:
            print('narrowest crossing: %.0f u wide at (%.0f, %.0f) between tri %d and %d'
                  % (worst[0], worst[1][0], worst[1][1], worst[2], worst[3]))
    else:
        raise SystemExit('unknown command ' + cmd)


if __name__ == '__main__':
    main()
