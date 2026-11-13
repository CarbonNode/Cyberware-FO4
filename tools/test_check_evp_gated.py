#!/usr/bin/env python3
# test_check_evp_gated.py - check_evp_gated.py against throwaway trees: the shape that must pass and every shape
# that must FAIL. Run: python3 -I fallout/cyberware/tools/test_check_evp_gated.py
#
# The rule this guards (2026-11-12, the navmesh-crash attribution): a `<ref>.evp` console command must pass the
# framework/core/cw_evpgate.h gate first, because evp makes the engine build a path and an unloaded actor has no
# navmesh in memory. The defect that produced this checker is that the law was written down on 2026-11-10 and then
# enforced at ONE of FOUR call sites; so each rule below is shown to fail on its own, and the real tree is checked
# last. A proof that cannot fail proves nothing.
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_evp_gated as ceg   # noqa: E402

passed = failed = 0


def ok(name, cond, detail=''):
    global passed, failed
    if cond:
        passed += 1
        print('PASS ' + name)
    else:
        failed += 1
        print('FAIL ' + name + ((' :: ' + detail) if detail else ''))


def tree(files):
    """A throwaway repo root holding fallout/cyberware/<rel> for each entry."""
    root = tempfile.mkdtemp(prefix='evpgate-')
    for rel, text in files.items():
        p = os.path.join(root, 'fallout', 'cyberware', rel.replace('/', os.sep))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    return root


def run(files):
    """(findings, emitters) for a throwaway tree."""
    root = tree(files)
    try:
        return ceg.check(root)
    finally:
        shutil.rmtree(root, ignore_errors=True)


GATED = '''#include "../core/cw_evpgate.h"
namespace {
    void Evaluate(RE::TESObjectREFR* ref)
    {
        const auto v = cw::evpgate::Check(ref != nullptr, ref && ref->Get3D() != nullptr);
        if (!cw::evpgate::Allowed(v)) { return; }
        char cmd[32];
        std::snprintf(cmd, sizeof(cmd), "%08X.evp", ref->GetFormID());
        RE::Console::ExecuteCommand(cmd);
    }
}
'''

# cw_clinic.cpp and cw_scene.cpp as they actually shipped in beta 1.
UNGATED_CLINIC = '''namespace clinic {
    namespace {
        void Evaluate(RE::TESObjectREFR* ref)
        {
            char cmd[32];
            std::snprintf(cmd, sizeof(cmd), "%08X.evp", ref->GetFormID());
            RE::Console::ExecuteCommand(cmd);
        }
    }
}
'''

UNGATED_SCENE = '''namespace scene {
    namespace {
        // Console `evp`: the engine re-picks her package. The same path CLINIC's nudge uses, never Papyrus.
        void Evaluate(RE::TESObjectREFR* ref)
        {
            if (!ref) return;
            char cmd[32];
            std::snprintf(cmd, sizeof(cmd), "%08X.evp", ref->GetFormID());
            RE::Console::ExecuteCommand(cmd);
        }
    }
}
'''

# --- 1. the shape that must pass -----------------------------------------------------------------------------
bad, n = run({'framework/plugin/cw_scene.cpp': GATED})
ok('a gated emitter passes', not bad and n == 1, '%r emitters=%d' % (bad, n))

# --- 2/3. the two real defects, each on its own ---------------------------------------------------------------
bad, n = run({'framework/plugin/cw_clinic.cpp': UNGATED_CLINIC})
ok("the shipped cw_clinic.cpp mover emitter FAILS", len(bad) == 1 and bad[0][0].endswith('cw_clinic.cpp'),
   repr(bad))

bad, n = run({'framework/plugin/cw_scene.cpp': UNGATED_SCENE})
ok("the shipped cw_scene.cpp EndRun emitter FAILS", len(bad) == 1 and bad[0][0].endswith('cw_scene.cpp'),
   repr(bad))

bad, n = run({'framework/plugin/cw_clinic.cpp': UNGATED_CLINIC,
              'framework/plugin/cw_scene.cpp': UNGATED_SCENE})
ok('both shipped emitters are reported, not just the first', len(bad) == 2, repr(bad))

# --- 4. the evasions ------------------------------------------------------------------------------------------
AFTER = GATED.replace(
    '        const auto v = cw::evpgate::Check(ref != nullptr, ref && ref->Get3D() != nullptr);\n'
    '        if (!cw::evpgate::Allowed(v)) { return; }\n', '').replace(
    '        RE::Console::ExecuteCommand(cmd);\n',
    '        RE::Console::ExecuteCommand(cmd);\n'
    '        const auto v = cw::evpgate::Check(ref != nullptr, true);\n'
    '        (void)cw::evpgate::Allowed(v);\n')
bad, n = run({'framework/plugin/cw_scene.cpp': AFTER})
ok('a gate placed AFTER the ExecuteCommand still FAILS', len(bad) == 1, repr(bad))

COMMENT_ONLY = '''namespace {
    // Gated by cw::evpgate::Check -- or so this comment claims.
    void Evaluate(RE::TESObjectREFR* ref)
    {
        /* cw::evpgate::Allowed would go here */
        char cmd[32];
        std::snprintf(cmd, sizeof(cmd), "%08X.evp", ref->GetFormID());
        RE::Console::ExecuteCommand(cmd);
    }
}
'''
bad, n = run({'framework/plugin/cw_scene.cpp': COMMENT_ONLY})
ok('a gate that exists only in a COMMENT still FAILS', len(bad) == 1, repr(bad))

# --- 5. what the rule must NOT touch ---------------------------------------------------------------------------
GENERIC_SEAM = '''namespace {
    void RunHarnessCommand(const std::string& t)
    {
        if (!t.empty()) RE::Console::ExecuteCommand(t.c_str());
    }
    void AnimFlavor()
    {
        RE::Console::ExecuteCommand("player.ChangeAnimFlavor");
    }
}
'''
bad, n = run({'framework/plugin/main.cpp': GENERIC_SEAM, 'framework/plugin/cw_scene.cpp': GATED})
ok('the generic console seam needs no gate (it is not an evp emitter)', not bad and n == 1, '%r emitters=%d' % (bad, n))

bad, n = run({'framework/lib/vendored.cpp': UNGATED_SCENE, 'framework/plugin/cw_scene.cpp': GATED})
ok('vendored/build trees are skipped', not bad, repr(bad))

# an `if (...) { ... }` inside the emitter must not be mistaken for the enclosing function
NESTED = GATED.replace('        char cmd[32];',
                       '        if (ref->GetFormID() != 0) { /* still the same function */ }\n        char cmd[32];')
bad, n = run({'framework/plugin/cw_scene.cpp': NESTED})
ok('an if-block inside the emitter does not hide the gate', not bad and n == 1, '%r emitters=%d' % (bad, n))

# --- 6. the vacuous pass -------------------------------------------------------------------------------------
root = tree({'framework/plugin/cw_scene.cpp': 'namespace {}\n'})
try:
    rc = ceg.main(['check_evp_gated.py', root])
finally:
    shutil.rmtree(root, ignore_errors=True)
ok('a tree with NO recognisable emitter is refused, not passed', rc == 1, 'rc=%d' % rc)

root = tree({'framework/plugin/cw_scene.cpp': GATED})
try:
    rc_ok = ceg.main(['check_evp_gated.py', root])
finally:
    shutil.rmtree(root, ignore_errors=True)
ok('main() exits 0 on a gated tree', rc_ok == 0, 'rc=%d' % rc_ok)

# --- 7. the real tree ----------------------------------------------------------------------------------------
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
bad, n = ceg.check(REPO)
ok('THE REAL TREE: every .evp emitter is gated', not bad, repr(bad))
ok('THE REAL TREE: the checker still recognises emitters (not a vacuous pass)', n >= 2, 'emitters=%d' % n)

print('\n%d passed, %d failed' % (passed, failed))
sys.exit(1 if failed else 0)
