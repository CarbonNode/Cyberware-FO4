#!/usr/bin/env python3
# check_evp_gated.py - every function that issues a `<ref>.evp` console command must pass it through
# framework/core/cw_evpgate.h FIRST.
#
# Run: python3 -I fallout/cyberware/tools/check_evp_gated.py [repo_root]
# Self-test (must-pass and every must-FAIL shape): python3 -I fallout/cyberware/tools/test_check_evp_gated.py
#
# THE RULE AND WHY IT IS A COMMIT GATE (2026-11-12, the navmesh-crash attribution).
# `evp` makes the engine re-pick an actor's AI package and therefore BUILD A PATH. For an actor whose 3D is away the
# cell and navmesh are not in memory, so the pathing server walks absent data. That was measured on 2026-11-10
# (framework/core/cw_clinicguard.h: 5 of 6 SanctuaryExt arrivals crashed at a constant 15-16 s inside
# BSPathing::BuildPath) and the law was written down.
#
# It was then enforced at exactly ONE of the FOUR places this plugin issues an evp:
#   framework/plugin/cw_clinic.cpp  Nudge()    - GATED on 2026-11-10
#   framework/plugin/cw_clinic.cpp  the MOVER  - NOT gated, and reached ONLY when she is unloaded (Tick returns at
#                                                `if (ref->Get3D())` above it; that is what "unseen" means in its log)
#   framework/plugin/cw_scene.cpp   :663       - gated, behind Ready()'s "the doctor is not loaded here"
#   framework/plugin/cw_scene.cpp   EndRun / back-beat end - NOT gated, and EndRun runs on an ABORT and on a LOAD,
#                                                which is precisely when her 3D is gone
# Two of the four shipped ungated, in a file whose own header states the law, because nothing checked. A law that
# lives in one file's comments is not a law. This checker is the law.
#
# WHAT IT CHECKS, exactly: for every C++ function under fallout/cyberware/ whose body builds a ".evp" command AND
# calls RE::Console::ExecuteCommand, the body must mention `cw::evpgate::` BEFORE that ExecuteCommand call. Putting
# the gate after the call does not count. The generic console seam (ExecuteCommand on arbitrary harness text) is not
# an evp emitter and is not touched.
import os
import sys

GATE = 'cw::evpgate::'
EXEC = 'ExecuteCommand'
EVP = '.evp'
CPP_EXT = ('.cpp', '.h', '.inl', '.hpp', '.cxx', '.cc')
SCOPE = 'fallout/cyberware/'
# Build output and vendored libraries are not ours to gate.
SKIP_DIRS = ('/framework/lib/', '/framework/build/', '/framework/.xmake/', '/framework/espgen/bin/',
             '/framework/espgen/obj/')


def _depths(src):
    """Brace depth at every offset, ignoring braces inside comments and literals.

    Returns (depths, code) where code is src with comment and literal bodies blanked to spaces, so later
    substring searches cannot match text that only appears in a comment.
    """
    depths = [0] * (len(src) + 1)
    code = list(src)
    i, n, depth = 0, len(src), 0
    while i < n:
        c = src[i]
        two = src[i:i + 2]
        if two == '//':
            while i < n and src[i] != '\n':
                code[i] = ' '
                depths[i] = depth
                i += 1
            continue
        if two == '/*':
            while i < n and src[i:i + 2] != '*/':
                code[i] = ' '
                depths[i] = depth
                i += 1
            for _ in range(min(2, n - i)):
                code[i] = ' '
                depths[i] = depth
                i += 1
            continue
        if c in '"\'':
            quote = c
            depths[i] = depth
            i += 1   # the opening quote stays, so a ".evp" literal is still findable via its body below
            while i < n and src[i] != quote:
                if src[i] == '\\':
                    depths[i] = depth
                    i += 1
                    if i < n:
                        depths[i] = depth
                        i += 1
                    continue
                depths[i] = depth
                i += 1
            if i < n:
                depths[i] = depth
                i += 1
            continue
        if c == '{':
            depth += 1
            depths[i] = depth
        elif c == '}':
            depths[i] = depth
            depth -= 1
        else:
            depths[i] = depth
        i += 1
    depths[n] = depth
    return depths, ''.join(code)


def _enclosing_function(src, depths, off):
    """Start offset of the body of the innermost enclosing block that looks like a function, else None.

    Walks outward from the innermost block. A block is a function body when the text just before its `{` ends in
    `)` or `const`/`noexcept` -- i.e. a signature rather than an `if`/`for`/`try`/initialiser block.
    """
    depth = depths[off]
    while depth > 0:
        # the `{` that opened this depth: nearest '{' before off whose depth == depth
        open_at = None
        j = off
        while j >= 0:
            if src[j] == '{' and depths[j] == depth:
                open_at = j
                break
            j -= 1
        if open_at is None:
            return None
        before = src[:open_at].rstrip()
        # strip a trailing comment-blanked run
        if before.endswith(')') or before.endswith('const') or before.endswith('noexcept'):
            # an `if (...)`/`for (...)`/`while (...)`/`switch (...)`/`catch (...)` also ends in ')'
            head = before[:-1] if before.endswith(')') else before
            # find the matching '(' for the trailing ')'
            k, bal = len(before) - 1, 0
            while k >= 0:
                if before[k] == ')':
                    bal += 1
                elif before[k] == '(':
                    bal -= 1
                    if bal == 0:
                        break
                k -= 1
            name = before[:k].strip().split()[-1] if k > 0 and before[:k].strip() else ''
            if name.split('(')[0] not in ('if', 'for', 'while', 'switch', 'catch'):
                return open_at + 1
        depth -= 1
        off = open_at - 1 if open_at > 0 else 0
        if off <= 0:
            return None
    return None


def scan_source(rel, src):
    """(findings, emitters) for one file. findings is a list of (rel, line, why)."""
    bad, emitters = [], 0
    depths, code = _depths(src)
    start = 0
    while True:
        at = code.find(EXEC, start)
        if at < 0:
            break
        start = at + len(EXEC)
        body = _enclosing_function(code, depths, at)
        if body is None:
            continue
        func = code[body:at]
        if EVP not in func:
            continue   # not an evp emitter: the generic console seam and friends are none of this rule's business
        emitters += 1
        if GATE not in func:
            line = code[:at].count('\n') + 1
            bad.append((rel, line, 'builds a "%s" command and calls %s with no %s gate before it'
                        % (EVP, EXEC, GATE)))
    return bad, emitters


def iter_sources(root):
    base = os.path.join(root, SCOPE.replace('/', os.sep))
    for dirpath, dirnames, filenames in os.walk(base):
        for fn in sorted(filenames):
            if not fn.lower().endswith(CPP_EXT):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace(os.sep, '/')
            if any(s in '/' + rel for s in SKIP_DIRS):
                continue
            yield rel, full


def check(root):
    """(findings, emitters_seen). A non-empty findings list means REFUSE."""
    bad, emitters = [], 0
    for rel, full in iter_sources(root):
        try:
            with open(full, 'r', encoding='utf-8', errors='replace') as f:
                src = f.read()
        except OSError as e:
            bad.append((rel, 0, 'could not be read: %s' % e))
            continue
        if EVP not in src:
            continue
        found, n = scan_source(rel, src)
        bad.extend(found)
        emitters += n
    return bad, emitters


def main(argv):
    root = argv[1] if len(argv) > 1 else os.path.abspath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
    bad, emitters = check(root)
    if not os.path.isdir(os.path.join(root, SCOPE.replace('/', os.sep))):
        print('check_evp_gated: %s is not in %s - nothing checked, and nothing claims to be'
              % (SCOPE, root), file=sys.stderr)
        return 1
    if bad:
        print('check_evp_gated: REFUSING - a `<ref>.evp` is issued without the %s gate:' % GATE, file=sys.stderr)
        for rel, line, why in bad:
            print('  %s:%d  %s' % (rel, line, why), file=sys.stderr)
        print('\n`evp` makes the engine re-pick a package and so BUILD A PATH. An actor whose 3D is away has no cell\n'
              'and no navmesh in memory, and the pathing server walks absent data (measured 2026-11-10: 5 of 6\n'
              'SanctuaryExt arrivals crashed at a constant 15-16 s inside BSPathing::BuildPath). Route the call\n'
              'through framework/core/cw_evpgate.h: Check(ref != nullptr, ref && ref->Get3D() != nullptr), refuse\n'
              'unless Allowed(), and log Why(). Skipping is safe - a stale package on an unloaded actor is invisible,\n'
              'and the clinic nudge re-arms on her unloaded -> loaded edge.', file=sys.stderr)
        return 1
    # A PROOF THAT CANNOT FAIL PROVES NOTHING. If a rename or a refactor leaves no emitter for this checker to
    # recognise, it must say so rather than print OK over an unchecked tree -- which is how the 2026-11-10 law came
    # to be enforced at one site out of four.
    if emitters == 0:
        print('check_evp_gated: REFUSING - found no `.evp` emitter to check in %s. Either the evp call sites were\n'
              'renamed/refactored out of this checker\'s recognition (it looks for a function whose body builds a\n'
              '".evp" command and calls RE::Console::ExecuteCommand), or the tree is incomplete. Update the checker\n'
              'in the same commit as the refactor; do not let it pass vacuously.' % SCOPE, file=sys.stderr)
        return 1
    print('check_evp_gated: OK - %d .evp emitter(s), every one gated by %s' % (emitters, GATE))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
