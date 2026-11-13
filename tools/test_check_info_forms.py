#!/usr/bin/env python3
"""Tests for check_info_forms.py: every refusal is planted and seen. ASCII only.

    python3 -I fallout/cyberware/tools/test_check_info_forms.py
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'check_info_forms.py')
results = []


def check(name, ok, out=''):
    results.append(bool(ok))
    print(('PASS ' if ok else 'FAIL ') + name)
    if not ok and out:
        for l in str(out).splitlines()[:8]:
            print('  -- ' + l)


def sub(tag, data):
    return tag + struct.pack('<H', len(data)) + data


def rec(tag, fid, payload, flags=0):
    return tag + struct.pack('<IIIIHH', len(payload), flags, fid, 0, 131, 0) + payload


def grup(label, gtype, body):
    return b'GRUP' + struct.pack('<I4sIII', 24 + len(body), label, gtype, 0, 0) + body


def trda(emotion, sound=0, num=1):
    return sub(b'TRDA', struct.pack('<IBIBHii', emotion, num, sound, 1, 0, -1, -1))


def plugin(info_payloads, compress=False, bad_zlib=False):
    """TES4 + a DIAL top group holding one topic with the given infos, shaped like a real plugin."""
    infos = b''
    for i, p in enumerate(info_payloads):
        if compress:
            z = struct.pack('<I', len(p)) + (b'not zlib at all' if bad_zlib else zlib.compress(p))
            infos += rec(b'INFO', 0x01000FA2 + i, z, flags=0x00040000)
        else:
            infos += rec(b'INFO', 0x01000FA2 + i, p)
    dial = rec(b'DIAL', 0x01000FA1, sub(b'EDID', b'T\0'))
    body = dial + grup(struct.pack('<I', 0x01000FA1), 7, infos) if info_payloads else b''
    tes4 = rec(b'TES4', 0, sub(b'HEDR', struct.pack('<fiI', 1.0, 2, 0x800)))
    return tes4 + (grup(b'DIAL', 0, body) if body else b'')


def run(*files):
    p = subprocess.run([sys.executable, '-I', TOOL] + list(files), capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    d = tempfile.mkdtemp(prefix='infoforms-')

    def w(name, data):
        f = os.path.join(d, name)
        open(f, 'wb').write(data)
        return f

    good = w('good.esp', plugin([trda(0xFFFFFFFF) + sub(b'NAM1', b' \0')]))
    null = w('null.esp', plugin([trda(0) + sub(b'NAM1', b'\0')]))
    form = w('form.esp', plugin([trda(0x000D755D)]))
    two = w('two.esp', plugin([trda(0xFFFFFFFF), trda(0xFFFFFFFF, num=1) + trda(0, num=2)]))
    short = w('short.esp', plugin([sub(b'TRDA', b'\0\0\0\0')]))
    zok = w('zok.esp', plugin([trda(0xFFFFFFFF)], compress=True))
    znull = w('znull.esp', plugin([trda(0)], compress=True))
    zbad = w('zbad.esp', plugin([trda(0xFFFFFFFF)], compress=True, bad_zlib=True))
    none = w('none.esp', plugin([]))
    snd0 = w('snd0.esp', plugin([trda(0xFFFFFFFF, sound=0)]))

    rc, out = run(good)
    check('emotion FFFFFFFF: OK (rc=%d)' % rc, rc == 0 and 'INFOFORMS_OK 1 response(s) in 1 info(s), 1 plugin(s)' in out, out)
    rc, out = run(null)
    check('emotion 00000000: FAIL, and the line names the info (rc=%d)' % rc, rc == 1 and 'INFOFORMS_FAIL 1 problem(s)' in out and 'INFO 01000FA2 response 1: emotion 00000000' in out, out)
    rc, out = run(form)
    check('emotion that is a form id: OK (rc=%d)' % rc, rc == 0 and 'INFOFORMS_OK' in out, out)
    rc, out = run(two)
    check('two infos, the null one is the SECOND response of the second info: FAIL names it (rc=%d)' % rc, rc == 1 and 'INFO 01000FA3 response 2' in out and '3 response(s)' in out, out)
    rc, out = run(short)
    check('a TRDA of 4 bytes: FAIL (rc=%d)' % rc, rc == 1 and 'TRDA is 4 bytes' in out, out)
    rc, out = run(zok)
    check('a compressed info with a good emotion: OK, it is read (rc=%d)' % rc, rc == 0 and '1 response(s)' in out, out)
    rc, out = run(znull)
    check('a compressed info with a null emotion: FAIL, compression does not hide it (rc=%d)' % rc, rc == 1 and 'emotion 00000000' in out, out)
    rc, out = run(zbad)
    check('a compressed info that will not inflate: FAIL, never a silent skip (rc=%d)' % rc, rc == 1 and 'will not inflate' in out, out)
    rc, out = run(none)
    check('a plugin with no info: OK and SAYS it holds none (rc=%d)' % rc, rc == 0 and 'holds no info' in out and 'INFOFORMS_OK 0 response(s)' in out, out)
    rc, out = run(snd0)
    check('sound 00000000 with emotion FFFFFFFF: OK (a zero sound is vanilla\'s "none") (rc=%d)' % rc, rc == 0, out)
    rc, out = run(good, null)
    check('one good plugin and one bad: FAIL overall, and the good one is still counted (rc=%d)' % rc, rc == 1 and 'good.esp: 1 info(s), 1 response(s), 0 problem(s)' in out, out)
    rc, out = run()
    check('no argument is a usage error, not a pass (rc=%d)' % rc, rc == 2 and not any(l.startswith('INFOFORMS_') for l in out.splitlines()), out)

    print('%d of %d checks passed' % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
