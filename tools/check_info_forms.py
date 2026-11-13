#!/usr/bin/env python3
"""check_info_forms.py <plugin> [<plugin> ...] - no response of ours may carry a NULL emotion. ASCII only.

A response's TRDA starts with the emotion form. Fallout4.esm writes "no emotion" as FFFFFFFF; Mutagen writes a null
FormLink as 00000000, which is a NULL form inside TESResponse. Voices.cs patched its own plugin; the dialogue guard's
info in Cyberware.esp was missed until 2026-11-12 (found after boot C crashed in
TESResponse::InitItem on a vanilla quest). The generator now fixes and asserts it (framework/espgen/EmotionFixup.cs);
this is the same rule for any plugin file, usable where no .NET is: the rig, a runner, a READY.

    python -I check_info_forms.py Cyberware.esp "Cyberware - Voices.esp"
        every INFO, every TRDA: emotion 00000000 is a FAIL, a TRDA under 9 bytes is a FAIL, a compressed INFO whose
        payload will not inflate is a FAIL. Last line INFOFORMS_OK <n> response(s) in <m> info(s), <k> plugin(s)
        (exit 0) or INFOFORMS_FAIL <n> problem(s) (exit 1). A plugin with no info at all says so on its own line.
    python -I check_info_forms.py --census Fallout4.esm
        what the master writes in the same fields, as counts. It is how the rule above was derived, not a check.

The SOUND field (bytes 5-8) is not judged: the census shows which value vanilla uses for "none".
"""
import os
import struct
import sys
import zlib


def subrecords(data):
    p, big = 0, 0
    while p + 6 <= len(data):
        st = data[p:p + 4]
        sl = struct.unpack_from('<H', data, p + 4)[0]
        if st == b'XXXX':
            big = struct.unpack_from('<I', data, p + 6)[0]
            p += 6 + sl
            continue
        if big:
            sl, big = big, 0
        yield st, data[p + 6:p + 6 + sl]
        p += 6 + sl


def infos(buf):
    """Yield (formid, payload or None when it will not inflate) for every INFO record, at any group depth."""
    off, end = 0, len(buf)
    while off + 24 <= end:
        typ = buf[off:off + 4]
        if typ == b'GRUP':
            off += 24
            continue
        size, flags, fid = struct.unpack_from('<III', buf, off + 4)
        if typ == b'INFO':
            data = buf[off + 24:off + 24 + size]
            if flags & 0x00040000:
                try:
                    data = zlib.decompress(data[4:])
                except zlib.error:
                    data = None
            yield fid, data
        off += 24 + size


def census(path):
    emo, snd, unk, sizes, n = {}, {}, {}, {}, 0
    for _fid, data in infos(open(path, 'rb').read()):
        if data is None:
            continue
        for st, v in subrecords(data):
            if st != b'TRDA':
                continue
            n += 1
            sizes[len(v)] = sizes.get(len(v), 0) + 1
            if len(v) < 10:
                continue
            e, _num, s, u = struct.unpack_from('<IBIB', v, 0)
            ek = '00000000' if e == 0 else 'FFFFFFFF' if e == 0xFFFFFFFF else 'a form'
            sk = '00000000' if s == 0 else 'FFFFFFFF' if s == 0xFFFFFFFF else 'a form'
            emo[ek] = emo.get(ek, 0) + 1
            snd[sk] = snd.get(sk, 0) + 1
            unk[u] = unk.get(u, 0) + 1
    print('%s: %d TRDA; sizes %s' % (os.path.basename(path), n, sorted(sizes.items())))
    print('  emotion: %s' % sorted(emo.items()))
    print('  sound:   %s' % sorted(snd.items()))
    print('  the byte after the sound form: %s' % sorted(unk.items()))
    return 0


def main(argv):
    if len(argv) >= 2 and argv[0] == '--census':
        return census(argv[1])
    if not argv:
        print(__doc__)
        return 2
    problems, responses, ninfo = 0, 0, 0
    for path in argv:
        name = os.path.basename(path)
        mine_i, mine_r, mine_p = 0, 0, 0
        for fid, data in infos(open(path, 'rb').read()):
            mine_i += 1
            if data is None:
                mine_p += 1
                print('FAIL %s INFO %08X: compressed and will not inflate' % (name, fid))
                continue
            for st, v in subrecords(data):
                if st != b'TRDA':
                    continue
                mine_r += 1
                if len(v) < 9:
                    mine_p += 1
                    print('FAIL %s INFO %08X: TRDA is %d bytes (%s)' % (name, fid, len(v), v.hex()))
                    continue
                e, num, s = struct.unpack_from('<IBI', v, 0)
                if e == 0:
                    mine_p += 1
                    print('FAIL %s INFO %08X response %d: emotion 00000000, a NULL form (vanilla writes FFFFFFFF for none); TRDA %s' % (name, fid, num, v.hex()))
        print('%s: %d info(s), %d response(s), %d problem(s)%s' % (name, mine_i, mine_r, mine_p, '' if mine_i else '  (this plugin holds no info)'))
        problems += mine_p
        responses += mine_r
        ninfo += mine_i
    if problems:
        print('INFOFORMS_FAIL %d problem(s) in %d response(s)' % (problems, responses))
        return 1
    print('INFOFORMS_OK %d response(s) in %d info(s), %d plugin(s)' % (responses, ninfo, len(argv)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
