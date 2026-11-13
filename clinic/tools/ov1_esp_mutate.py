# ov1_esp_mutate.py - synthetic negative controls for verify_clinic OV-1 (2026-11-10, batch 7): rewrite a plugin with every GRUP size recomputed.
#   achr-xscl <in> <out> <fid-hex>        append XSCL 1.5 to that ACHR (an un-allow-listed drift only OV-1 sees)
#   stray-glob <in> <out> <fid-hex>       add a top-level GLOB group holding an override of <fid> (master-indexed),
#                                         a record the winner does not carry
import struct, sys, zlib
def parse(d, off, end):
    out = []
    while off < end:
        typ = d[off:off+4]; sz = struct.unpack_from('<I', d, off+4)[0]
        if typ == b'GRUP':
            out.append(['G', d[off+8:off+24], parse(d, off+24, off+sz)]); off += sz
        else:
            out.append(['R', typ, d[off+8:off+24], d[off+24:off+24+sz]]); off += 24+sz
    return out
def ser(items):
    b = bytearray()
    for it in items:
        if it[0] == 'G':
            body = ser(it[2]); b += b'GRUP' + struct.pack('<I', 24+len(body)) + it[1] + body
        else:
            b += it[1] + struct.pack('<I', len(it[3])) + it[2] + it[3]
    return bytes(b)
def find(items, fid):
    for it in items:
        if it[0] == 'G':
            r = find(it[2], fid)
            if r: return r
        elif struct.unpack_from('<I', it[2], 4)[0] == fid: return it
    return None
mode, src, dst, fid = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16)
d = open(src, 'rb').read(); hs = struct.unpack_from('<I', d, 4)[0]
head = bytearray(d[:24+hs]); tree = parse(d, 24+hs, len(d))
if mode == 'achr-xscl':
    r = find(tree, fid); assert r and r[1] == b'ACHR', 'no ACHR ' + hex(fid)
    flags = struct.unpack_from('<I', r[2], 0)[0]
    body = zlib.decompress(r[3][4:]) if flags & 0x40000 else r[3]
    body += b'XSCL' + struct.pack('<H', 4) + struct.pack('<f', 1.5)
    if flags & 0x40000: body = struct.pack('<I', len(body)) + zlib.compress(body)
    r[3] = body
elif mode == 'stray-glob':
    assert find(tree, fid) is None
    ed = b'CW_TestStrayOverride\0'
    body = b'EDID' + struct.pack('<H', len(ed)) + ed + b'FNAM' + struct.pack('<H', 1) + b'f' + b'FLTV' + struct.pack('<H', 4) + struct.pack('<f', 1.0)
    rec = ['R', b'GLOB', struct.pack('<IIIHH', 0, fid, 0, 131, 0), body]
    tree.append(['G', b'GLOB' + struct.pack('<IHHHH', 0, 0, 0, 131, 0), [rec]])
    i = head.find(b'HEDR')     # numRecords at HEDR+6+4
    n = struct.unpack_from('<I', head, i+10)[0]; struct.pack_into('<I', head, i+10, n+1)
else: sys.exit('mode?')
open(dst, 'wb').write(bytes(head) + ser(tree))
print(mode, src, '->', dst)
