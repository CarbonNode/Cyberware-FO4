# clinic_inputs.py - the part of a Cyberware.esp that the Phase 2 clinic esp is built from, as one sha256.
# The clinic esp (clinicgen --phase2 --dc-base <Cyberware.esp>) reads exactly four records from Cyberware.esp:
#   - CELL Fallout4.esm|0198F1 (DmndSurgeryBasement01): its override is copied verbatim, the outer door is added
#   - Varga's NPC_ (phase2.vargaNpcEdid): copied verbatim, her clinic packages appended
#   - Varga's ACHR (phase2.vargaRefEdid): copied, moved into the clinic
#   - the menu chair ACTI (phase2.chair.baseEdid): its FormKey is placed
# Two Cyberware.esp files with the same four records give a byte-identical clinic esp, whatever else differs (test vs
# play build, the dialogue guard, sounds, armory). So the clinic pin records THIS hash, not the whole file's: the
# promotion manifest checks the rig's own Cyberware.esp against it (2026-11-10, the lead).
#
# usage: python -I clinic_inputs.py <Cyberware.esp> <clinic.json>
# prints one line: CLINIC_INPUTS sha256=<64 hex> cell=<8> npc=<8> achr=<8> acti=<8>   (exit 0)
#              or: CLINIC_INPUTS_MISSING <what>                                          (exit 1)
# Each record is hashed as: type, form id (as stored), flags without the compression bit, and its decompressed body.
import hashlib, json, os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
from cellsurvey import Walk   # noqa: E402

DC_CELL = 0x000198F1   # Fallout4.esm is master 0 of Cyberware.esp


def rec_hash(w, fid):
    t, flags, off, sz = w.recs[fid]
    h = hashlib.sha256()
    h.update(t.encode('latin1'))
    h.update(struct.pack('<II', fid, flags & ~0x40000))
    h.update(w.body(fid))
    return h.hexdigest()


def main():
    esp, spec_path = sys.argv[1], sys.argv[2]
    p2 = json.load(open(spec_path))['phase2']
    w = Walk(esp)
    want = [('cell', 'CELL', None), ('npc', 'NPC_', p2['vargaNpcEdid']), ('achr', 'ACHR', p2['vargaRefEdid']), ('acti', 'ACTI', p2['chair']['baseEdid'])]
    found = {}
    for key, typ, edid in want:
        if edid is None:
            fid = DC_CELL if DC_CELL in w.recs and w.recs[DC_CELL][0] == 'CELL' else None
        else:
            fid = next((f for f, r in w.recs.items() if r[0] == typ and (w.edid(f) or '').lower() == edid.lower()), None)
        if fid is None:
            print(f"CLINIC_INPUTS_MISSING {typ} {edid or 'Fallout4.esm|0198F1'} in {os.path.basename(esp)}")
            sys.exit(1)
        found[key] = rec_hash(w, fid)
    total = hashlib.sha256(''.join(found[k] for k, _, _ in want).encode('ascii')).hexdigest()
    print(f"CLINIC_INPUTS sha256={total} " + ' '.join(f"{k}={found[k][:8]}" for k, _, _ in want))


if __name__ == '__main__':
    main()
