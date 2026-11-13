# build_scene.py - offline preview render of CW_VargaClinic01 from clinicgen placement data.
# Target: Blender 4.5.x (the rig's pinned 4.5.15), headless:
#   blender --background --factory-startup --python-exit-code 1 --python build_scene.py -- --kit <dir> --out <dir>
# Clay + real light placement from the esp (no FO4 textures): layout/lighting preview, not final art.
# 1 Blender unit = 1 game unit (~1/70 m); cameras/lights use game-scale numbers throughout.
import json, math, os, sys

import bpy

assert bpy.app.version[:2] == (4, 5), bpy.app.version_string

argv = sys.argv[sys.argv.index('--') + 1:]
KIT = argv[argv.index('--kit') + 1]
OUT = argv[argv.index('--out') + 1]
ONLY = argv[argv.index('--shots') + 1].split(',') if '--shots' in argv else None
SAMPLES = int(argv[argv.index('--samples') + 1]) if '--samples' in argv else 160
os.makedirs(OUT, exist_ok=True)

refs = json.load(open(os.path.join(KIT, 'refs.json')))
lights = json.load(open(os.path.join(KIT, 'lights.json')))

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
scene = bpy.context.scene

# ---------------- materials (name-keyed clay)
def mat(name, base, rough=0.6, metal=0.0, emit=None, emit_str=0.0):
    m = bpy.data.materials.get(name)
    if m: return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes['Principled BSDF']
    b.inputs['Base Color'].default_value = (*base, 1)
    b.inputs['Roughness'].default_value = rough
    b.inputs['Metallic'].default_value = metal
    if emit:
        b.inputs['Emission Color'].default_value = (*emit, 1)
        b.inputs['Emission Strength'].default_value = emit_str
    return m

M_CONCRETE = mat('concrete', (0.30, 0.29, 0.27), 0.9)
M_METAL    = mat('metal', (0.45, 0.46, 0.48), 0.35, 0.9)
M_PROP     = mat('prop', (0.5, 0.47, 0.42), 0.6)
M_CLOTH    = mat('cloth', (0.75, 0.73, 0.68), 0.9)
M_GLASS    = mat('glass', (0.8, 0.85, 0.85), 0.05)
M_LAMP     = mat('lamp', (1, 1, 1), 0.4, 0.0, (1.0, 0.95, 0.85), 5.0)
M_EMERG    = mat('emerg', (0.6, 0.1, 0.1), 0.4, 0.0, (1.0, 0.1, 0.05), 6.0)
M_SCREEN   = mat('screen', (0.1, 0.3, 0.3), 0.3, 0.0, (0.2, 0.9, 0.8), 3.0)

def pick_mat(name):
    n = name.lower()
    if any(k in n for k in ('concrete', 'bldgshell', 'bldconc', 'cave', 'rubble')): return M_CONCRETE
    if '_screen' in n or 'jar' in n and False: pass
    if 'screen' in n: return M_SCREEN
    if any(k in n for k in ('fluorescent', 'ceilinglamp', 'desklamp', 'oillamp', 'hospitallights01', 'hospitallights02', 'hospitallights05')): return M_LAMP
    if 'emergency' in n: return M_EMERG
    if any(k in n for k in ('beaker', 'bottle', 'jar', 'bloodpack')): return M_GLASS
    if any(k in n for k in ('towel', 'rug', 'curtain', 'coat', 'mattress', 'cot')): return M_CLOTH
    if any(k in n for k in ('metal', 'shelf', 'gurney', 'pipe', 'locker', 'cabinet', 'safe', 'table', 'trashcan',
                            'cooler', 'toolbox', 'bucket', 'stool', 'mirror', 'divider', 'door', 'crate', 'fileснc')): return M_METAL
    return M_PROP

# ---------------- import each unique model once, then instance
FX_SKIP = ('mist', 'lightbeam', 'fxsmoke', 'fxdrip', 'fxwater')   # atmosphere meshes: lights, not geometry
if '--cutaway' in argv:   # diagnostics: hide ceiling tiles so top-down views show the interior
    refs = [r for r in refs if not (r['model'] and 'floorconcrete01' in r['model'] and r['pos'][2] > 700)]
refs = [r for r in refs if not (r['model'] and any(k in r['model'] for k in FX_SKIP))]
unique = {}
for r in refs:
    if r['model']: unique.setdefault(r['model'], None)
coll = bpy.data.collections.new('clinic')
scene.collection.children.link(coll)

for model in unique:
    objname = model.replace('\\', '_')[:-4] + '.obj'
    path = os.path.join(KIT, 'objs', objname)
    if not os.path.exists(path):
        # ba2 names keep original case; fall back to case-insensitive match
        low = objname.lower()
        cands = [f for f in os.listdir(os.path.join(KIT, 'objs')) if f.lower() == low]
        if not cands: print('MISSING OBJ', objname); continue
        path = os.path.join(KIT, 'objs', cands[0])
    before = set(bpy.data.objects)
    bpy.ops.wm.obj_import(filepath=path, forward_axis='Y', up_axis='Z')
    new = [o for o in bpy.data.objects if o not in before]
    # join multi-object imports into one mesh object
    base = new[0]
    if len(new) > 1:
        with bpy.context.temp_override(active_object=base, selected_editable_objects=new):
            bpy.ops.object.join()
    base.data.materials.clear()
    base.data.materials.append(pick_mat(model))
    base.name = 'SRC_' + os.path.basename(model)
    for c in list(base.users_collection): c.objects.unlink(base)
    unique[model] = base

placed = 0
for r in refs:
    src = unique.get(r['model'])
    if src is None: continue
    inst = bpy.data.objects.new(r['edid'] or os.path.basename(r['model']), src.data)
    inst.location = r['pos']
    rx, ry, rz = r['rot']
    inst.rotation_mode = 'XYZ'
    inst.rotation_euler = (-rx, -ry, -rz)      # Bethesda angles are clockwise-positive
    s = r.get('scale', 1.0)
    inst.scale = (s, s, s)
    coll.objects.link(inst)
    placed += 1
print('placed', placed, 'instances of', len([v for v in unique.values() if v]), 'meshes')

# ---------------- lights from the esp
def add_light(name, kind, loc, color, energy, radius=6.0, shadow=True, spot_down=False):
    ld = bpy.data.lights.new(name, kind)
    ld.color = color
    ld.energy = energy
    ld.shadow_soft_size = radius
    if kind == 'SPOT':
        ld.spot_size = math.radians(70)
        ld.spot_blend = 0.4
    ld.use_shadow = shadow
    lo = bpy.data.objects.new(name, ld)
    lo.location = loc
    if spot_down: lo.rotation_euler = (0, 0, 0)   # Blender spot points -Z = straight down
    coll.objects.link(lo)
    return lo

# game-unit energies: watts scale ~ (unit/m)^2, so numbers are large on purpose
for L in lights:
    e = L['edid'].lower(); p = L['pos']
    shadow = 'ns' not in e.replace('nonspec', '')
    if 'spot' in e:
        add_light(L['edid'], 'SPOT', p, (1.0, 0.98, 0.92), 2.0e6, 6, True, spot_down=True)
    elif 'fire' in e:
        add_light(L['edid'], 'POINT', p, (1.0, 0.55, 0.25), 3.5e5, 10, shadow)
    elif 'red' in e:
        add_light(L['edid'], 'POINT', p, (1.0, 0.12, 0.08), 1.8e5, 8, shadow)
    elif 'fluor' in e or 'white' in e:
        add_light(L['edid'], 'POINT', p, (0.85, 0.95, 1.0), 4.5e5, 14, shadow)
    else:
        add_light(L['edid'], 'POINT', p, (1.0, 0.85, 0.65), 4.0e5, 12, shadow)
print('lights', len(lights))

# the operating station focal point: one strong warm key over the gurney (matches CeilingLamp01On)
add_light('StationKey', 'SPOT', (1835, 2065, 748), (1.0, 0.96, 0.88), 4.0e6, 10, True, spot_down=True)

world = bpy.data.worlds.new('w'); scene.world = world
world.use_nodes = True
world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.0

# ---------------- cameras
def cam(name, loc, at, lens=24):
    c = bpy.data.cameras.new(name); c.lens = lens; c.clip_end = 20000
    o = bpy.data.objects.new(name, c); o.location = loc
    coll.objects.link(o)
    t = bpy.data.objects.new(name + '_t', None); t.location = at
    coll.objects.link(t)
    con = o.constraints.new('TRACK_TO'); con.target = t
    con.track_axis = 'TRACK_NEGATIVE_Z'; con.up_axis = 'UP_Y'
    return o

shots = [
    ('01_from_door',   (1932, 1762, 634), (1838, 2068, 590), 19),
    ('02_station',     (2120, 2000, 650), (1830, 2080, 600), 24),
    ('03_wide',        (2230, 2280, 648), (1795, 2060, 585), 17),
    ('04_night_corner',(2080, 2058, 640), (1900, 2432, 588), 22),
]


# ---------------- diagnostic shots (--shots 00_plan / 00_axon): sun + ortho top-down
if ONLY and any(n.startswith('00_') for n in ONLY):
    sun = bpy.data.lights.new('diag_sun', 'SUN'); sun.energy = 4.0
    so = bpy.data.objects.new('diag_sun', sun); so.rotation_euler = (0.6, 0.2, 0.0)
    coll.objects.link(so)
    c = bpy.data.cameras.new('00_plan'); c.type = 'ORTHO'; c.ortho_scale = 1500; c.clip_end = 20000
    o = bpy.data.objects.new('00_plan', c); o.location = (2048, 2048, 4000); o.rotation_euler = (0, 0, 0)
    coll.objects.link(o)
    scene.render.engine = 'CYCLES'; scene.cycles.device = 'CPU'; scene.cycles.samples = 32
    scene.cycles.use_denoising = True
    scene.render.resolution_x = 1400; scene.render.resolution_y = 1400
    scene.view_settings.view_transform = 'AgX'
    scene.render.image_settings.file_format = 'PNG'
    if '00_plan' in ONLY:
        scene.camera = o
        scene.render.filepath = os.path.join(OUT, '00_plan.png')
        bpy.ops.render.render(write_still=True)
        print('rendered', scene.render.filepath)
    if '00_axon' in ONLY:
        ax = cam('00_axon', (3400, 600, 2600), (2050, 2050, 560), 35)
        scene.camera = ax
        scene.render.resolution_x = 1600; scene.render.resolution_y = 1200
        scene.render.filepath = os.path.join(OUT, '00_axon.png')
        bpy.ops.render.render(write_still=True)
        print('rendered', scene.render.filepath)
    print('DONE'); import sys; sys.exit(0)

scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = SAMPLES
scene.cycles.use_denoising = True
scene.render.resolution_x = 1600
scene.render.resolution_y = 900
scene.view_settings.view_transform = 'AgX'
scene.view_settings.look = 'AgX - Base Contrast'
scene.view_settings.exposure = 0.0
scene.render.image_settings.file_format = 'PNG'

for name, loc, at, lens in shots:
    if ONLY and name not in ONLY: continue
    scene.camera = cam(name, loc, at, lens)
    bpy.context.view_layer.update()
    scene.render.filepath = os.path.join(OUT, name + '.png')
    bpy.ops.render.render(write_still=True)
    print('rendered', scene.render.filepath)
print('DONE')
# APIs used that changed in 4.0+: bpy.ops.wm.obj_import (4.0 replaces import_scene.obj),
# Principled 'Emission Color'/'Emission Strength' input names (4.0), AgX view transform (4.0).
