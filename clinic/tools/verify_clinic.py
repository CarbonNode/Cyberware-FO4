# verify_clinic.py - offline gate for "Cyberware - Clinic.esp" (no game, no third-party modules).
# Asserts the invariants the CLINIC stream promises in its READY message:
#   1. header: ESL flag set, masters == [Fallout4.esm]
#   2. every new record id is in the ESL range 0x800-0xFFF
#   3. the new cell exists, holds the expected ref counts and exactly one NAVM
#   4. that NAVM's NVNM parent is the new cell, with donor's vertex/triangle counts intact
#   5. the DmndSurgeryBasement01 override's CELL body is byte-identical to the winner's copy
#      (previs/precombine stay untouched) and adds exactly ONE child: the outer door
#   6. the two door REFRs teleport-link to EACH OTHER, landings where the spec says
#
# usage: python -I verify_clinic.py <Cyberware - Clinic.esp> <winner Cyberware.esp> <clinic.json> [--phase2] [--verdict <json>]
#   --verdict writes the result, pass OR fail, as JSON bound to the esp's sha256. build-rig.ps1 stages a
#   clinic esp only when the stage folder holds an OK verdict for exactly that esp that ran the checks it requires
#   (framework/tools/clinic-stage-gate.ps1): the door does not fetch clinic/, so this file is how this gate reaches a rig build.
#   --phase2 additionally asserts: the CW_Ripperdoc_diamondcity NPC_ override is byte-identical to the
#   master's record EXCEPT the appended PKID list (LORE's no-silent-revert rule), Varga's ACHR override
#   lives in the new cell at the spec position, the menu chair + bed refs exist, and both PACKs exist.
import json, struct, sys, os, hashlib, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cellsurvey import Walk, subs   # noqa: E402

DC_CELL = 0x198F1
fails = []
checks = []
def check(ok, what):
    print(('ok   ' if ok else 'FAIL ') + what)
    checks.append({'ok': bool(ok), 'text': what})
    if not ok: fails.append(what)

def _sha(path, lf=False):
    b = open(path, 'rb').read()
    if lf: b = b.replace(b'\r\n', b'\n')   # text inputs hash over LF, as clinicgen hashes the spec (a CRLF checkout agrees)
    return hashlib.sha256(b).hexdigest()

def write_verdict(ok):
    if '--verdict' not in sys.argv: return
    i = sys.argv.index('--verdict')
    if i + 1 >= len(sys.argv): print('VERDICT NOT WRITTEN: --verdict needs a path'); sys.exit(2)
    out = sys.argv[i + 1]
    v = {
        'schema': 1, 'tool': 'verify_clinic',
        'verdict': 'VERIFY OK' if ok else 'VERIFY FAILED', 'fails': len(fails), 'checks_run': len(checks),
        'phase2': PHASE2,
        'esp': os.path.basename(esp_path), 'esp_sha256': _sha(esp_path),
        'winner_sha256': _sha(winner_path), 'spec_sha256_lf': _sha(spec_path, True),
        'verify_sha256_lf': _sha(os.path.abspath(__file__), True),
        'written': time.strftime('%Y-%m-%dT%H:%M:%S'),
        'checks': checks,
    }
    with open(out, 'w', encoding='ascii') as f: json.dump(v, f, indent=1, ensure_ascii=True)
    print(f"VERDICT WRITTEN {out}: {v['verdict']} for esp {v['esp_sha256'][:8]} ({len(checks)} checks)")

esp_path, winner_path, spec_path = sys.argv[1], sys.argv[2], sys.argv[3]
spec = json.load(open(spec_path))
w = Walk(esp_path)

# 1. header
d = w.data
hflags = struct.unpack_from('<I', d, 8)[0]
check(hflags & 0x200 != 0, 'ESL header flag set')
PHASE2 = '--phase2' in sys.argv
want_masters = ['Fallout4.esm', 'Cyberware.esp'] if PHASE2 else ['Fallout4.esm']
check(w.masters == want_masters, f'masters == {want_masters} (got {w.masters})')

# 2. id range (own records only: index == number of masters)
own = [fid for fid in w.recs if fid >> 24 == len(w.masters)]
bad = [fid for fid in own if not (0x800 <= (fid & 0xFFFFFF) <= 0xFFF)]
check(not bad, f'{len(own)} own records all in ESL id range 0x800-0xFFF')

# 3. the new cell
cells = {fid: w.edid(fid) for fid, r in w.recs.items() if r[0] == 'CELL'}
new_cell = next((fid for fid, e in cells.items() if e == spec['cell']['edid']), None)
check(new_cell is not None, f"new cell {spec['cell']['edid']} present")
kids = w.cell_children.get(new_cell, [])
refs = [k for k in kids if w.recs[k][0] == 'REFR']
navm = [k for k in kids if w.recs[k][0] == 'NAVM']
expect_refs = (160 - len(spec['strip'])) + len(spec['dress']) + len(spec.get('cpProps', [])) + sum(1 for d in spec.get('sounds', []) if 'pos' in d) + 1   # donor clones + dressing + cp props + ambience markers + inner door
if PHASE2:
    expect_refs += 3                          # menu chair + bed frame + bed furniture (the ACHR is counted separately)
    _p2 = spec['phase2']
    expect_refs += len(_p2.get('clutter', [])) + len(_p2.get('statics', [])) - len(_p2.get('swapOut', []))
    expect_refs += len(_p2.get('lights', [])) - len(_p2.get('stripDonor', []))
    expect_refs += sum(1 for k in ('patientSeat', 'patientSeatB', 'surgeonMark', 'sandboxAnchor') if k in _p2)   # INSTALL SCENE seat (+ the B probe seat while it exists) + her mark + the on-mesh sandbox anchor
check(len(refs) == expect_refs, f'new cell holds {len(refs)} REFRs (expected {expect_refs})')
check(len(navm) == 1, 'new cell holds exactly one NAVM')

# 4. navmesh parent + geometry counts
for st, v in subs(w.body(navm[0])):
    if st == 'NVNM':
        p1, p2 = struct.unpack_from('<II', v, 8)
        check(p1 == 0 and p2 == new_cell, 'NVNM parent is the new cell (interior form)')
        nv = struct.unpack_from('<I', v, 16)[0]
        nt = struct.unpack_from('<I', v, 16 + 4 + nv * 12)[0]
        check(nv == 72 and nt == 54, f'navmesh geometry intact ({nv} verts, {nt} tris; donor had 72/54)')

# 5. DC override: CELL body identical to the winner's, exactly one added child
wn = Walk(winner_path)
def cell_body_subs(walk, fid):
    return [(t, bytes(v)) for t, v in subs(walk.body(fid))]
ours, theirs = cell_body_subs(w, DC_CELL), cell_body_subs(wn, DC_CELL)
check(ours == theirs, 'DmndSurgeryBasement01 CELL body byte-identical to the winner copy')
dc_kids = [k for k in w.cell_children.get(DC_CELL, [])]
check(len(dc_kids) == 1 and w.edid(dc_kids[0]) == 'CW_ClinicDoorOuter',
      f'DC override adds exactly one child, the outer door (got {[w.edid(k) for k in dc_kids]})')

# 6. the teleport pair
doors = {w.edid(fid): fid for fid, r in w.recs.items() if r[0] == 'REFR' and w.edid(fid) in ('CW_ClinicDoorInner', 'CW_ClinicDoorOuter')}
check(len(doors) == 2, 'both door refs present')
def xtel(fid):
    for t, v in subs(w.body(fid)):
        if t == 'XTEL':
            dest = struct.unpack_from('<I', v)[0]
            x, y, z, rx, ry, rz = struct.unpack_from('<6f', v, 4)
            return dest, (x, y, z), rz
    return None
inner, outer = xtel(doors['CW_ClinicDoorInner']), xtel(doors['CW_ClinicDoorOuter'])
check(inner is not None and inner[0] == doors['CW_ClinicDoorOuter'], 'inner door teleports to outer door')
check(outer is not None and outer[0] == doors['CW_ClinicDoorInner'], 'outer door teleports to inner door')
def near(a, b): return all(abs(x - y) < 0.1 for x, y in zip(a, b))
check(near(inner[1], spec['doors']['dc']['landing'][:3]), f"inner XTEL lands at spec dc landing {inner[1]}")
check(near(outer[1], spec['doors']['clinic']['landing'][:3]), f"outer XTEL lands at spec clinic landing {outer[1]}")

# persistent flags on the doors
for name, fid in doors.items():
    check(w.recs[fid][1] & 0x400 != 0, f'{name} is persistent')

if PHASE2:
    p2 = spec['phase2']
    # NPC_ override == master record except PKID (and record header size field)
    npc_m = next((f for f, r in wn.recs.items() if r[0] == 'NPC_' and wn.edid(f) == p2['vargaNpcEdid']), None)
    npc_o = next((f for f, r in w.recs.items() if r[0] == 'NPC_' and w.edid(f) == p2['vargaNpcEdid']), None)
    check(npc_m is not None and npc_o is not None, 'phase2: NPC_ record in master and override')
    ours = [(t, bytes(v)) for t, v in subs(w.body(npc_o))]
    theirs = [(t, bytes(v)) for t, v in subs(wn.body(npc_m))]
    ours_nopkid = [x for x in ours if x[0] != 'PKID']
    theirs_nopkid = [x for x in theirs if x[0] != 'PKID']
    check(ours_nopkid == theirs_nopkid, 'phase2: NPC_ override identical to master except PKID')
    added = len([x for x in ours if x[0] == 'PKID']) - len([x for x in theirs if x[0] == 'PKID'])
    check(added == len(p2['packages']), f'phase2: exactly {len(p2["packages"])} packages appended (got {added})')
    # ACHR moved into the new cell at the spec position, keeping Cyberware.esp's formid
    achr = next((k for k in w.cell_children.get(new_cell, []) if w.recs[k][0] == 'ACHR'), None)
    check(achr is not None, 'phase2: an ACHR override lives in the new cell')
    if achr is not None:
        pos = None
        for t, v in subs(w.body(achr)):
            if t == 'DATA' and len(v) >= 24:
                pos = struct.unpack_from('<3f', v)
        check(pos is not None and near(pos, p2['varga']['pos']), f'phase2: Varga at spec position {pos}')
        owner = w.masters[achr >> 24] if (achr >> 24) < len(w.masters) else 'SELF'
        check(owner == 'Cyberware.esp', f"phase2: the ACHR keeps Cyberware.esp's formid (owner {owner})")
        # the sit package finds her seat through this link (keyword LinkCustom01, target the persistent chair)
        seat_fid = next((f for f, r in w.recs.items() if w.edid(f) == p2['officeSeatEdid']), None)
        links = [struct.unpack_from('<II', v) for t, v in subs(w.body(achr)) if t == 'XLKR' and len(v) >= 8]
        kw_ok = [l for l in links if (l[0] & 0xFFFFFF) == 0x05D5E6 and l[1] == seat_fid]
        check(seat_fid is not None and len(kw_ok) == 1, f'phase2: LinkCustom01 points at {p2["officeSeatEdid"]} (links {len(links)})')
    # the clutter pass stays original: vanilla bases plus our own Signs meshes, never a Cyberpunk-derived model
    for d in p2.get('statics', []):
        m = d['model'].lower().replace('/', '\\')
        ok = m.startswith('cyberware\\clinic\\signs\\') or m.startswith('setdressing\\') or m.startswith('furniture\\')
        check(ok, f"phase2: own static {d['model']} is a Signs mesh or a vanilla path")
    cp_stems = {os.path.splitext(d['model'])[0].lower() for d in spec.get('cpProps', [])}
    check(not any(os.path.splitext(os.path.basename(d['model'].replace('\\', '/')))[0].lower() in cp_stems for d in p2.get('statics', [])), 'phase2: no own static reuses a CP prop mesh')
    # station lighting: the stack stays separated, and the key spot really aims at the chair
    import math
    CHAIR = (1835.0, 2065.0)
    gone_sw = {(m['base'], round(m['from'][0]), round(m['from'][1])) for m in p2.get('swapOut', [])}
    moved = {(m['base'], round(m['from'][0]), round(m['from'][1])) for m in p2.get('reposition', []) if 'pos' in m}
    stacked = [d['base'] for d in spec['dress'] if (d['base'], round(d['pos'][0]), round(d['pos'][1])) not in gone_sw
               and (d['base'], round(d['pos'][0]), round(d['pos'][1])) not in moved and d['pos'][2] > 660
               and math.dist(d['pos'][:2], CHAIR) < 12]
    stacked += [l['edid'] for l in p2.get('lights', []) if math.dist(l['pos'][:2], CHAIR) < 12]
    check(not stacked, f'phase2: nothing hangs on the chair spot (got {stacked})')
    for l in p2.get('lights', []):
        check(w_has_edid := any(w.edid(f) == l['edid'] and r[0] == 'LIGH' for f, r in w.recs.items()), f"phase2: {l['edid']} is a LIGH record")
        # the record carries the spec's numbers (2026-11-10: the face fill and the teal raise are judged as built, not as written).
        # LIGH DATA: int32 time, uint32 radius @4, RGBA @8; FNAM = fade (float). The placed ref <edid>_Ref: DATA = pos/rot.
        lf = next((f for f, r in w.recs.items() if r[0] == 'LIGH' and w.edid(f) == l['edid']), None)
        if lf is not None:
            ls = dict((t, v) for t, v in subs(w.body(lf)))
            dat, fnam = ls.get('DATA', b''), ls.get('FNAM', b'')
            okl = len(dat) >= 12
            if okl and 'radius' in l: okl = struct.unpack_from('<I', dat, 4)[0] == l['radius']
            if okl and 'color' in l: okl = list(dat[8:11]) == list(l['color'])
            if okl and 'fade' in l: okl = len(fnam) >= 4 and abs(struct.unpack_from('<f', fnam, 0)[0] - l['fade']) < 1e-4
            check(okl, f"phase2: {l['edid']} has the spec's radius/colour/fade")
        rf = next((f for f, r in w.recs.items() if r[0] == 'REFR' and w.edid(f) == l['edid'] + '_Ref'), None)
        rd = dict((t, v) for t, v in subs(w.body(rf))).get('DATA', b'') if rf is not None else b''
        okr = len(rd) >= 12 and max(abs(a - b) for a, b in zip(struct.unpack_from('<3f', rd, 0), l['pos'])) < 0.5
        check(okr, f"phase2: {l['edid']}_Ref stands at {l['pos']}")
        if 'aim' in l:
            a, b, c = l['rot']                                      # forward +X, rotations clockwise (vanilla lamp spots)
            dx, dy, dz = math.cos(c) * math.cos(b), -math.sin(c), math.cos(c) * math.sin(b)
            want = [q - p for p, q in zip(l['pos'], l['aim'])]
            n = math.sqrt(sum(x * x for x in want))
            cos = (dx * want[0] + dy * want[1] + dz * want[2]) / n
            check(cos > 0.995, f"phase2: {l['edid']} aims at {l['aim']} (cos {cos:.4f})")
    # chair + bed + packs
    names = {w.edid(f): r[0] for f, r in w.recs.items() if w.edid(f)}
    expected = [('CW_ClinicChair', 'REFR'), ('CW_ClinicBedFrame', 'REFR'), ('CW_ClinicBed', 'REFR')]
    expected += [(pkg['edid'], 'PACK') for pkg in p2['packages']]
    expected += [(p2['officeSeatEdid'], 'REFR')]
    # INSTALL SCENE: the patient seat (own FURN, persistent ref on the CP chair, no script) and her mark (persistent XMarkerHeading)
    for key, kind in (('patientSeat', 'FURN'), ('patientSeatB', 'FURN'), ('surgeonMark', None), ('sandboxAnchor', None)):
        if key not in p2: continue
        item = p2[key]
        if kind: expected += [(item['baseEdid'], kind)]
        expected += [(item['refEdid'], 'REFR')]
        rfid = next((f for f in w.recs if w.edid(f) == item['refEdid']), None)
        if rfid is None: continue
        flags = w.recs[rfid][1]
        check(flags & 0x400, f"phase2: {item['refEdid']} is persistent (the DLL looks it up by id)")
        data = dict((t, v) for t, v in subs(w.body(rfid)))
        okp = 'DATA' in data and len(data['DATA']) >= 24
        if okp:
            px, py, pz, rx, ry, rz = struct.unpack_from('<6f', data['DATA'], 0)
            want = item['pos']
            okp = max(abs(px - want[0]), abs(py - want[1]), abs(pz - want[2])) < 0.5 and abs(rz - item['rotZ']) < 0.001
        check(okp, f"phase2: {item['refEdid']} stands at the spec pos/rot")
        if kind:
            bfid = next((f for f in w.recs if w.edid(f) == item['baseEdid']), None)
            bsubs = dict((t, v) for t, v in subs(w.body(bfid))) if bfid is not None else {}
            check(bfid is not None and 'VMAD' not in bsubs, f"phase2: {item['baseEdid']} carries no script (VMAD)")
            check(bsubs.get('MODL', b'').rstrip(b'\0').decode('latin1').lower() == item['model'].lower(), f"phase2: {item['baseEdid']} uses model {item['model']}")
            # XMRK. An invisible MODL carries no markers and no behaviour graph: that was SCN-T0's cause
            # (docs/integration/SEAT-XMRK-CAUSE.md), so a seat with such a model must name a marker model, and the esp
            # must carry the one the spec names. Refused here rather than discovered on a boot.
            got_xmrk = bsubs.get('XMRK', b'').rstrip(b'\0').decode('latin1')
            want_xmrk = item.get('markerModel', '')
            if want_xmrk:
                check(got_xmrk.lower() == want_xmrk.lower(), f"phase2: {item['baseEdid']} carries marker model (XMRK) {want_xmrk} (got {got_xmrk!r})")
            invisible = os.path.basename(item['model'].replace('\\', '/')).lower().startswith('invisible')
            check(not invisible or got_xmrk != '', f"phase2: {item['baseEdid']} uses an invisible model, so it must carry a marker model (XMRK); it carries {got_xmrk!r}")
            # A PROBE seat must stay out of Varga's reach, or it can pull her off her station and void the clinic capture
            # rows (QD-B, 2026-11-10). Her only packages that can run in the cell are the sandboxes that name a nearRef and
            # a radius; anything with a bigger radius is conditioned GetInCell clinic == 0. Asserted so a later pos edit
            # cannot quietly move the probe inside one.
            if key != 'patientSeat':
                for pkg in p2.get('packages', []):
                    if 'nearRef' not in pkg or 'radius' not in pkg: continue
                    nfid = next((f for f in w.recs if w.edid(f) == pkg['nearRef']), None)
                    ndata = dict((t, v) for t, v in subs(w.body(nfid))) if nfid is not None else {}
                    if 'DATA' not in ndata or len(ndata['DATA']) < 24: continue
                    nx, ny = struct.unpack_from('<2f', ndata['DATA'], 0)
                    d = ((item['pos'][0] - nx) ** 2 + (item['pos'][1] - ny) ** 2) ** 0.5
                    check(d > pkg['radius'] + 100, f"phase2: probe seat {item['refEdid']} is {d:.0f} u from {pkg['nearRef']}, outside {pkg['edid']}'s {pkg['radius']} u sandbox +100 u margin")
            # its name is the spec's, never the donor's localized one (which resolved only where Strings sit beside the esm)
            want_name = item.get('name', '')
            got_name = bsubs['FULL'].rstrip(b'\0').decode('latin1') if 'FULL' in bsubs else ''
            check(got_name == want_name and (want_name or 'FULL' not in bsubs), f"phase2: {item['baseEdid']} is named {want_name!r} by the spec (no FULL when empty; got {got_name!r})")
            check(data.get('NAME', b'\0\0\0\0')[:4] == struct.pack('<I', bfid if bfid is not None else 0), f"phase2: {item['refEdid']} places {item['baseEdid']}")
    for e, t in expected:
        check(names.get(e) == t, f'phase2: {e} is a {t}')
    # package schedules and the in-clinic condition. PKDT is not parsed here; PSDT holds the schedule.
    pack_by_edid = {w.edid(f): f for f, r in w.recs.items() if r[0] == 'PACK'}
    names_fid = {w.edid(f): f for f in w.recs if w.edid(f)}
    for pkg in p2['packages']:
        fid = pack_by_edid.get(pkg['edid'])
        if fid is None: continue
        sub = dict()
        ctda = 0
        pldt = []
        for t, v in subs(w.body(fid)):
            if t == 'PSDT': sub['PSDT'] = v
            if t == 'CTDA': ctda += 1
            if t == 'PLDT': pldt.append(v)
        if 'nearRef' in pkg:
            # PLDT: int32 type (0 = near reference), uint32 target formid, int32 radius
            ref_fid = names_fid.get(pkg['nearRef'])
            okl = len(pldt) == 1 and len(pldt[0]) >= 12 and ref_fid is not None
            if okl:
                ltype, ltgt, lrad = struct.unpack_from('<iIi', pldt[0], 0)
                okl = ltype == 0 and ltgt == ref_fid and lrad == pkg.get('radius', lrad)
            check(okl, f"phase2: {pkg['edid']} is leashed near {pkg['nearRef']}")
        psdt = sub.get('PSDT')
        ok = psdt is not None and len(psdt) >= 12
        if ok and 'scheduleHour' in pkg:
            hour = struct.unpack_from('<b', psdt, 3)[0]
            dur = struct.unpack_from('<i', psdt, 8)[0]
            ok = hour == pkg['scheduleHour'] and dur == pkg['scheduleDurationMinutes']
        check(ok, f"phase2: {pkg['edid']} schedule matches the spec")
        want = 1 if pkg.get('condition') in ('inClinic', 'notInClinic') else 0
        check(ctda >= want, f"phase2: {pkg['edid']} has its condition ({ctda} CTDA)")
        if want:
            # CTDA: u8 op (high 3 bits = compare, 0 = ==), float value @4, u16 function @8, param1 @12. GetInCell = 0x43.
            cell_fid = names_fid.get(spec['cell']['edid'])
            wantv = 1.0 if pkg['condition'] == 'inClinic' else 0.0
            okc = any(len(v) >= 16 and (v[0] >> 5) == 0 and struct.unpack_from('<H', v, 8)[0] == 0x43
                      and struct.unpack_from('<I', v, 12)[0] == cell_fid and struct.unpack_from('<f', v, 4)[0] == wantv
                      for t, v in subs(w.body(fid)) if t == 'CTDA')
            check(okc, f"phase2: {pkg['edid']} runs only {'in' if wantv else 'outside'} the clinic (GetInCell == {wantv:.0f})")
        if 'hours' in pkg:
            # P16: the window is GameHour conditions (GetGlobalValue = 0x4A on Fallout4.esm GLOB 000038), op >= from and op < to
            # (op high 3 bits: 3 = >=, 4 = <), ANDed (op bit 0 clear), and the PSDT says "any" (hour -1, duration 0).
            lo, hi = pkg['hours']
            conds = [(v[0] >> 5, v[0] & 1, struct.unpack_from('<f', v, 4)[0]) for t, v in subs(w.body(fid))
                     if t == 'CTDA' and len(v) >= 16 and struct.unpack_from('<H', v, 8)[0] == 0x4A
                     and struct.unpack_from('<I', v, 12)[0] == 0x38]
            wantc = ([(3, 0, float(lo))] if lo is not None else []) + ([(4, 0, float(hi))] if hi is not None else [])
            check(sorted(conds) == sorted(wantc), f"phase2: {pkg['edid']} runs only {lo if lo is not None else 0}-{hi if hi is not None else 24} by GameHour ({conds})")
            anyp = psdt is not None and len(psdt) >= 12 and struct.unpack_from('<b', psdt, 3)[0] == -1 and struct.unpack_from('<i', psdt, 8)[0] == 0
            check(anyp, f"phase2: {pkg['edid']} has no PSDT schedule (its window is the condition)")
    # INSTALL SCENE: the globals the DLL writes (short GLOB, start value) and the packages they gate (GetGlobalValue == v, ANDed)
    for g in p2.get('globals', []):
        gfid = names_fid.get(g['edid'])
        check(names.get(g['edid']) == 'GLOB', f"phase2: {g['edid']} is a GLOB")
        if gfid is None: continue
        gs = dict((t, v) for t, v in subs(w.body(gfid)))
        okg = gs.get('FNAM', b'')[:1] == b's' and len(gs.get('FLTV', b'')) >= 4 and struct.unpack_from('<f', gs['FLTV'], 0)[0] == float(g.get('value', 0))
        check(okg, f"phase2: {g['edid']} is a short global starting at {g.get('value', 0)}")
    for pkg in p2['packages']:
        if 'globalIs' not in pkg: continue
        fid, gfid = pack_by_edid.get(pkg['edid']), names_fid.get(pkg['globalIs'][0])
        okc = fid is not None and gfid is not None and any(
            len(v) >= 16 and (v[0] >> 5) == 0 and (v[0] & 1) == 0 and struct.unpack_from('<H', v, 8)[0] == 0x4A
            and struct.unpack_from('<I', v, 12)[0] == gfid and struct.unpack_from('<f', v, 4)[0] == float(pkg['globalIs'][1])
            for t, v in subs(w.body(fid)) if t == 'CTDA')
        check(okc, f"phase2: {pkg['edid']} runs only while {pkg['globalIs'][0]} == {pkg['globalIs'][1]} (ANDed)")
    # her package list (NPC_ PKID, first match wins) is the spec's order, so a global-gated package stays on top
    npc_fid = next((f for f, r in w.recs.items() if r[0] == 'NPC_' and w.edid(f) == p2['vargaNpcEdid']), None)
    pkids = [struct.unpack_from('<I', v, 0)[0] for t, v in subs(w.body(npc_fid)) if t == 'PKID'] if npc_fid is not None else []
    wantids = [pack_by_edid.get(pkg['edid']) for pkg in p2['packages']]
    check(pkids[-len(wantids):] == wantids and all(wantids), f"phase2: her package list is the spec's order ({len(pkids)} PKID)")
    # the DLL (cw_clinic.cpp kPkg* and kStallTargets) hardcodes these local ids; the lock must keep them
    dll_ids = {'CW_ClinicSleep0to6': 0x96E, 'CW_ClinicOffice6to10': 0x973, 'CW_ClinicWalk10to14': 0x974, 'CW_ClinicShop14to24': 0x975,
               'CW_ClinicOutside': 0xA09, 'CW_ClinicIndoorHold': 0xA4C, 'CW_ClinicStepIn': 0xA51, 'CW_ClinicSceneActive': 0xA50,
               'CW_ClinicChair': 0x970, 'CW_ClinicBed': 0x972, p2['officeSeatEdid']: 0x89A}
    for e, lid in dll_ids.items():
        got = names_fid.get(e)
        check(got is not None and (got & 0xFFFFFF) == lid, f"phase2: {e} has the local id the DLL compiles in ({lid:03X}, got {got if got is None else format(got & 0xFFFFFF, '03X')})")
    # every in-clinic hour has exactly one clinic package (else a still-valid package can hold her past her next stop)
    inside = [(pkg['hours'][0] or 0, pkg['hours'][1] if pkg['hours'][1] is not None else 24, pkg['edid'])
              for pkg in p2['packages'] if pkg.get('condition') == 'inClinic' and 'hours' in pkg]
    # a global-gated package (the scene step-in) is not part of the clock: it passes only while the DLL holds the global at 1
    unwindowed = [pkg['edid'] for pkg in p2['packages'] if pkg.get('condition') == 'inClinic' and 'hours' not in pkg and 'globalIs' not in pkg]
    check(not unwindowed, f"phase2: every in-clinic package has an hour window ({unwindowed or 'all do'})")
    cover = [sum(1 for a, b, _ in inside if a <= h + 0.5 < b) for h in range(24)]
    check(all(c == 1 for c in cover), f"phase2: in the clinic exactly one package passes at each hour ({cover})")

    # CL-4, measured: no loose prop may be left over air or in the floor, because havok moves it after
    # the cell loads and the room is then graded on where it ended up (QUALITY FAIL CLNCL1/CLNCL4,
    # 2026-11-10). prop_support.py reads OBND from data/vanilla_bounds.json, so this needs no game.
    import prop_support                                           # noqa: E402
    _bounds = json.load(open(os.path.join(os.path.dirname(os.path.abspath(spec_path)), 'vanilla_bounds.json')))
    _placed = prop_support.from_esp(esp_path, spec, _bounds)
    _psfails, _ = prop_support.check(_placed, _bounds, p2.get('propRest', {}).get('exempt', []))
    for _f in sorted(_psfails):
        print('     ' + _f)
    check(not _psfails, f'phase2: every loose prop rests on a surface (prop_support: {len(_psfails)} would move)')
    # CLNCL4 (2026-11-10): resting on a surface is NOT enough. The west-counter trays each sat on a
    # MetalTable02 top at exactly their own base height - the check above passed them - and two were
    # still graded lying on the floor, because loose props were authored INSIDE EACH OTHER and the
    # engine separates them when the cell loads. On a counter top that push ends on the floor. The
    # fix is the vanilla one: the REFR "Don't Havok Settle" flag, so the engine leaves the ref where
    # the spec put it. This refuses a build where a conflicting pair is left unflagged.
    _cfails = prop_support.conflicts(_placed, _bounds, p2.get('propRest', {}).get('exempt', []))
    for _f in sorted(_cfails):
        print('     ' + _f)
    check(not _cfails, f'phase2: no loose prop is spawned inside another unpinned one '
                       f'(prop_support: {len(_cfails)} conflicting pair(s))')

# OV-1, the override rule (the lead, 2026-11-10): every record the clinic esp SHARES with the winner Cyberware.esp
# (a Cyberware.esp record it overrides, or a master record both override) must equal the winner's subrecords, except the
# fields on OVERRIDE_ALLOW, each with its reason. Why: the clinic loads after Cyberware.esp, so ITS copy wins in game. The
# stale 76b4e763 carried Varga's NPC_ with the OLD VTCK 00013ADD and masked the voice-type fix (01000F87) for every boot
# that loaded it, the same way it masked the patient seat's XMRK. A copy that drifted from its source is now a build failure.
OVERRIDE_ALLOW = {
    'CW_Ripperdoc_diamondcity': {'PKID': 'the clinic appends its own packages (the PKID list; the NPC_ check above holds the rest byte-identical)'},
    'CW_RipperdocRef_diamondcity': {'DATA': 'her placed ref is moved into the clinic (position and rotation)',
                                    'XLKR': 'the clinic links her ref to its own marks'},
}
# OV-1b (composer intake of 55b672b9, 2026-11-10): a clinic override of an id the winner does NOT carry used to be
# skipped silently. That shape means the clinic was built against a different Cyberware.esp (or carries a master record
# the winner never touched), so it FAILS unless it is listed here with its reason. Keyed by EDID, else by 8-hex form id.
OVERRIDE_UNSHARED_ALLOW = {}
def _subs_of(walk, fid):
    return [(t, bytes(v)) for t, v in subs(walk.body(fid))]
_ov_owner = 'Cyberware.esp'
_ov_ok = _ov_owner not in wn.masters
check(_ov_ok, f'override: the winner is {_ov_owner} itself (its masters {wn.masters} do not include it), so shared ids can be mapped')
_ov_shared, _ov_bad, _ov_master, _ov_unshared = 0, [], 0, []
if _ov_ok:
    for fid, rec in sorted(w.recs.items()):
        idx = fid >> 24
        if idx >= len(w.masters):
            continue                      # the clinic's own record, nothing to compare
        owner = w.masters[idx]
        low = fid & 0xFFFFFF
        _ov_master += 1
        ed = w.edid(fid) or ''
        if owner == _ov_owner:
            nf = (len(wn.masters) << 24) | low
        elif owner in wn.masters:
            nf = (wn.masters.index(owner) << 24) | low
        else:
            nf = None                     # a master the winner does not even list
        if nf is None or nf not in wn.recs:
            key = ed or format(fid, '08X')
            why = OVERRIDE_UNSHARED_ALLOW.get(key)
            check(why is not None, f"override: {fid:08X} {rec[0]} '{ed}' (owned by {owner}) is NOT carried by the winner "
                                   f"({'allowed: ' + why if why else 'not on the allow-list: the clinic was built against another Cyberware.esp'})")
            if why is None:
                _ov_unshared.append(f'{key}({rec[0]}, {owner})')
            continue
        _ov_shared += 1
        a, b = _subs_of(w, fid), _subs_of(wn, nf)
        types = sorted({t for t, _ in a} | {t for t, _ in b})
        diff = [t for t in types if [x for x in a if x[0] == t] != [x for x in b if x[0] == t]]
        allowed = OVERRIDE_ALLOW.get(ed, {})
        bad = [t for t in diff if t not in allowed]
        check(not bad, f"override: {fid:08X} {rec[0]} '{ed}' (owned by {owner}) equals the winner's copy except the allow-list "
                       f"(differs in {diff or 'nothing'}; allowed {sorted(allowed) or 'none'}; NOT allowed {bad or 'none'})")
        if bad:
            _ov_bad.append(f'{ed or format(fid, "08X")}:{"+".join(bad)}')
check(_ov_ok and _ov_master > 0 and not _ov_unshared,
      f'override: every clinic override (a record owned by a master) is carried by the winner, except the allow-list '
      f'({_ov_master} master-indexed, {_ov_shared} carried; NOT carried: {", ".join(_ov_unshared) or "none"})')
check(_ov_ok and not _ov_bad and not _ov_unshared and (not PHASE2 or _ov_shared >= 2),
      f'override: every record shared with the winner Cyberware.esp equals it except the allow-list '
      f'({_ov_shared} shared; drifted: {", ".join(_ov_bad) or "none"}; not carried: {len(_ov_unshared)}' + ('; phase2 needs at least her NPC_ and ACHR' if PHASE2 else '') + ')')

print()
if fails:
    print(f'VERIFY FAILED: {len(fails)} check(s)')
    write_verdict(False)
    sys.exit(1)
print('VERIFY OK: all checks passed')
write_verdict(True)
