#!/usr/bin/env python3
# p2_dress.py - authoring tool for the Phase 2 crate swap (the lead's clutter order, 2026-11-09).
# Writes phase2.swapOut / clutter / statics into data/clinic.json. Vanilla Fallout4.esm bases and our own painted
# signs only: nothing from the Cyberpunk extraction. Heights come from each base's OBND, so a prop rests on its surface.
#
#   python3 -I p2_dress.py <cat.json: EDID + OBND dump of Fallout4.esm> <clinic.json>
# Then rebuild, and run lane_check.py and verify_clinic.py --phase2.
import json, math, sys

FLOOR = 567.5
TABLE = FLOOR + 56.0            # MetalTable01/02, MetalTableRound01 top

cat = {r[1]: r for r in json.load(open(sys.argv[1]))}
spec_path = sys.argv[2]
spec = json.load(open(spec_path))
dress = spec['dress']

# crates leave in groups, with whatever stood on them, so nothing is left floating (lane_check enforces it)
SWAP_IDX = [22, 23, 24, 25, 26,        # the light stack on the chair's spot: fill, spot, beam, mist, stand fill
            81, 82, 83, 89,            # mid-room stack + hot plate
            129, 130, 133,             # entry nook stack + folder
            149, 150,                  # south-east crate + toolbox
            152, 153, 155,             # by the wheeled cart, and the west crate that crowded the parts cabinet
            158, 159,                  # west of the bed route
            173, 177, 178,             # east wall crate + teapot + typewriter
            180, 181, 182, 185, 186,   # west wall stack
            183, 184, 189,             # west wall, behind the divider
            194, 195, 196, 200, 201, 202, 203,   # north band
            206, 207, 212,             # north-east column
            248, 249]                  # south-middle crate + box
swap = [{'base': dress[i]['base'], 'from': dress[i]['pos'][:2]} for i in SWAP_IDX]

clutter, statics = [], []

def bounds(base):
    o = cat[base][3]
    return o

def radius(base, scale=1.0):
    o = bounds(base)
    return round(math.hypot(max(abs(o[0]), abs(o[3])), max(abs(o[1]), abs(o[4]))) * scale, 1)

def put(base, x, y, surface=FLOOR, rot=0.0, scale=None, big=False):
    """Rest base on a surface. big: a floor piece that needs its footprint checked against her lanes."""
    z1 = bounds(base)[2]
    e = {'base': base, 'pos': [round(x, 1), round(y, 1), round(surface - z1 * (scale or 1.0), 1)], 'rotZ': round(rot, 3)}
    if scale: e['scale'] = scale
    if big: e['r'] = radius(base, scale or 1.0)
    clutter.append(e)

TERMINAL_ON = 'SetDressing\\Office\\TerminalOn.nif'   # vanilla lit screen, no plain STAT exists; its mesh sits at desk height
def terminal(x, y, rot=0.0):
    statics.append({'model': TERMINAL_ON, 'pos': [x, y, FLOOR], 'rotZ': rot, 'r': 0.0})

def sign(name, x, y, z, rot=0.0, scale=None, r=0.0):
    e = {'model': 'Cyberware\\Clinic\\Signs\\%s.nif' % name, 'pos': [x, y, z], 'rotZ': rot, 'r': r}
    if scale: e['scale'] = scale
    statics.append(e)

# --- mid-room: a salvage bench where the crate stack was; parts laid out for fitting. The low floor clutter stays under it.
BX, BY = 2035, 1915
put('MetalTable01', BX, BY, big=True)
put('SurgicalTray01', BX - 13, BY - 2, TABLE)
for b, dx, dy in (('CircuitBoardAssaultron', -8, -4), ('BoSR02_Sensor', 6, 6), ('PowerRelayCoil', -4, 9), ('Fuse', 9, -6), ('Fuse', 11, -3)):
    put(b, BX - 13 + dx, BY - 2 + dy, TABLE + 3.5, rot=(dx * 0.31) % 6.28)
put('CircuitBoardMilitary', BX - 40, BY + 6, TABLE, rot=0.5)
put('BoSR02_Capacitor', BX - 43, BY - 12, TABLE, rot=2.2)
put('Institute_Driver01', BX + 8, BY - 9, TABLE, rot=1.3)
put('Institute_Spanner01', BX + 15, BY - 4, TABLE, rot=1.7)
put('Institute_TriTool01', BX + 22, BY - 8, TABLE, rot=1.1)
put('SurgicalCutter', BX + 11, BY + 8, TABLE, rot=0.2)
put('VacuumTube', BX + 27, BY + 12, TABLE)
put('VacuumTube', BX + 32, BY + 9, TABLE)
put('HotPlate', BX + 39, BY - 12, TABLE, rot=0.4)
put('MS11PowerCables', BX - 26, BY + 15, TABLE, rot=1.0)
put('BotModelEyebot', BX + 37, BY + 14, TABLE, rot=3.9)
terminal(BX + 31, BY - 6, 0.0)

# --- west wall by the station: a parts cabinet with a plate on the face the room sees
put('ConsoleCabSingle02', 1748, 1830, big=True)
sign('CWC_Sign_PlateStill', 1769.2, 1822, FLOOR + 92, 1.571)

# --- west of the bed route: the scanner she tests parts on, and a half-finished body beside it
put('ConScanner01', 1765, 2180, big=True)
put('SynthGen2Partial', 1750, 2212, big=True)

# --- by the head of her bed: a drip and its bag (centre only: the bed box allows a stand beside it)
put('IVstand01', 1810, 2455)
put('IVBag', 1826, 2438, FLOOR + 0.5, rot=0.7)

# --- east wall: terminal corner where the crate and typewriter were
put('MetalTable02', 2322, 2140, rot=1.571, big=True)
terminal(2322, 2152, 4.712)
put('Typewriter', 2318, 2124, TABLE, rot=4.5)
put('Teapot', 2336, 2122, TABLE, rot=0.8)
put('MakeshiftBattery01', 2306, 2150, TABLE, rot=1.9)

# --- north band: the lit monitor bank, facing the room
put('InstituteMonitorBank02Anim', 2210, 2378, FLOOR, big=True)
put('RRCourserChipScanner', 2112, 2378, big=True)
put('Loot_Prewar_ToolBox', 2290, 2350, rot=6.19, big=True)
put('MetalBucket_PreWar', 2160, 2352, rot=0.4)
put('MedicalLiquidNitrogenGun', 2138, 2352, FLOOR + 0.5, rot=1.2)

# --- north-east column: cabinet carrying her mark
put('ConsoleCabSingle02', 2325, 2290, big=True)
sign('CWC_Sign_PlateLogo', 2303.8, 2282, FLOOR + 92, 1.571)

# --- south-middle: a tool table the door sees first
put('MetalTableRound01', 1940, 1860, big=True)
for b, dx, dy, rot in (('Blowtorch', -14, 8, 0.6), ('Wrench02', 8, -10, 1.2), ('ScrewDriver01', 16, 4, 2.0), ('Hammer01', -6, -14, 0.3), ('SmithingToolSaw02', 2, 14, 3.1), ('Institute_Examiner', -20, -6, 1.0)):
    put(b, 1940 + dx, 1860 + dy, TABLE, rot=rot)

# --- entry nook: small cabinet and sharps plate where the stacked crates were
put('ConsoleCabSingle01', 1925, 1545, rot=3.14, big=True)
sign('CWC_Sign_PlateSharps', 1929, 1580.2, FLOOR + 50, 0.0)

# --- hand-lettered boards where the door's sightline finds them
sign('CWC_Sign_AFrameCash', 2238, 1640, FLOOR, 5.8, r=24.0)
sign('CWC_Sign_AFrameKnock', 2080, 1783, FLOOR, 0.3, r=24.0)

# --- a tent card on the jar table
sign('CWC_Sign_AFrameJars', 2126, 2158, TABLE, 6.0, scale=0.35)

# --- station light: one hot key spot from the moved lamp, a teal rim off the tall monitor, a dim warm at the lit stand.
CHAIR_AIM = (1835.0, 2065.0, 625.0)
def aim_rot(pos, target):
    dx, dy, dz = (t - p for p, t in zip(pos, target))
    n = math.sqrt(dx * dx + dy * dy + dz * dz); dx, dy, dz = dx / n, dy / n, dz / n
    c = math.asin(-dy); b = math.atan2(dz, dx)
    return [0.0, round(b, 4), round(c, 4)]             # forward +X; vanilla's lamp spots show rotations act clockwise
KEY = [1885.0, 2130.0, 722.0]
lights = [
    {'edid': 'CW_ClinicKeySpot', 'copyOf': 'Fallout4.esm|054467', 'pos': KEY, 'rot': aim_rot(KEY, CHAIR_AIM), 'aim': list(CHAIR_AIM),
     'radius': 520, 'fov': 100.0, 'nearClip': 36.0, 'fade': 2.6, 'color': [255, 226, 190]},
    {'edid': 'CW_ClinicAccentTeal', 'copyOf': 'Fallout4.esm|0560D0', 'pos': [1765.0, 2150.0, 660.0], 'rot': [0.0, 0.0, 0.0],
     'radius': 300, 'fade': 1.4, 'color': [60, 210, 220]},
    {'edid': 'CW_ClinicStandWarm', 'copyOf': 'Fallout4.esm|0560DC', 'pos': [1750.0, 2120.0, 700.0], 'rot': [0.0, 0.0, 0.0],
     'radius': 130, 'fade': 0.35},
]
spec['phase2']['lights'] = lights
# donor fills next to the station and every mist: strips the wash that lifted the blacks
spec['phase2']['stripDonor'] = ['15BF30', '15B6CF', '15B6C8', '15BF31', '15B70F', '15BF2E', '15B6AC', '15B6CD']
spec['phase2']['cellLighting'] = {'fogNear': 350.0, 'fogFar': 2600.0, 'fogMax': 0.5, 'fogNearColor': [9, 12, 12], 'fogFarColor': [10, 13, 12], 'directionalColor': [18, 20, 20]}
if not any(m['base'] == 'CeilingLamp01On' for m in spec['phase2']['reposition']):
    spec['phase2']['reposition'].append({'base': 'CeilingLamp01On', 'from': [1835.0, 2065.0], 'pos': [1885.0, 2130.0, 742.0]})

spec['phase2']['swapOut'] = swap
spec['phase2']['clutter'] = clutter
spec['phase2']['statics'] = statics
json.dump(spec, open(spec_path, 'w'), indent=2)
open(spec_path, 'a').write('\n')
print(f'swapOut {len(swap)}, clutter {len(clutter)}, statics {len(statics)}; crates swapped: ' + str(sum(1 for i in SWAP_IDX if dress[i]['base'].startswith('WoodCrate'))))
