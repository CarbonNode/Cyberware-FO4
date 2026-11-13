#!/usr/bin/env python3
# signs_nif.py - the clinic's own sign meshes and materials: flat boards, A-frames and plates.
#   py -3 signs_nif.py --pyn <dir with pyn\> --bgsm-dir <dir with bgsm.py> --out <stage root>
# Run on the build machine after texconv (the DDS names are fixed). Both faces carry the art, the back with mirrored U,
# so a sign reads the right way round from either side. Nothing from a game is read; pynifly writes BSTriShape NIFs.
import argparse, math, os, sys

SIZE = 2048.0
PANELS = {'cash': (0, 0, 512, 768), 'knock': (512, 0, 512, 768), 'jars': (1024, 0, 512, 768), 'logo': (1536, 0, 512, 512),
          'still': (0, 768, 1024, 512), 'sharps': (1024, 768, 1024, 512), 'edge': (0, 1280, 512, 256)}
MAT, MAT_GLOW = 'CWC_Signs', 'CWC_SignsGlow'
TEX = 'Cyberware/Clinic/Signs/CWC_Signs'
MAT_PATH = 'Materials\\Cyberware\\Clinic\\Signs\\%s.bgsm'

def uv(name):
    x, y, w, h = PANELS[name]
    return x / SIZE, y / SIZE, (x + w) / SIZE, (y + h) / SIZE

def add_quad(mesh, a, b, c, d, n, uvs):
    i = len(mesh['v'])
    mesh['v'] += [a, b, c, d]; mesh['n'] += [n] * 4; mesh['t'] += uvs
    mesh['f'] += [(i, i + 1, i + 2), (i, i + 2, i + 3)]

def board(mesh, name, w, h, t, xf):
    """A w x h x t board centred on x, base at z=0, face normal +-Y, through transform xf(point)."""
    u0, v0, u1, v1 = uv(name)
    eu0, ev0, eu1, ev1 = uv('edge')
    hw, ht = w / 2, t / 2
    P = lambda x, y, z: xf((x, y, z))
    def N(dx, dy, dz):
        o = xf((0, 0, 0)); p = xf((dx, dy, dz))
        return (p[0] - o[0], p[1] - o[1], p[2] - o[2])
    # front (-Y) and back (+Y, U mirrored so the art still reads)
    add_quad(mesh, P(-hw, -ht, 0), P(hw, -ht, 0), P(hw, -ht, h), P(-hw, -ht, h), N(0, -1, 0), [(u0, v1), (u1, v1), (u1, v0), (u0, v0)])
    add_quad(mesh, P(hw, ht, 0), P(-hw, ht, 0), P(-hw, ht, h), P(hw, ht, h), N(0, 1, 0), [(u0, v1), (u1, v1), (u1, v0), (u0, v0)])
    e = [(eu0, ev1), (eu1, ev1), (eu1, ev0), (eu0, ev0)]
    add_quad(mesh, P(-hw, ht, 0), P(-hw, -ht, 0), P(-hw, -ht, h), P(-hw, ht, h), N(-1, 0, 0), e)   # sides
    add_quad(mesh, P(hw, -ht, 0), P(hw, ht, 0), P(hw, ht, h), P(hw, -ht, h), N(1, 0, 0), e)
    add_quad(mesh, P(-hw, -ht, h), P(hw, -ht, h), P(hw, ht, h), P(-hw, ht, h), N(0, 0, 1), e)      # top
    add_quad(mesh, P(-hw, ht, 0), P(hw, ht, 0), P(hw, -ht, 0), P(-hw, -ht, 0), N(0, 0, -1), e)     # bottom

def new_mesh():
    return {'v': [], 'n': [], 't': [], 'f': []}

def aframe(name, w=34.0, h=52.0, t=1.4, spread=11.0):
    m = new_mesh()
    for side in (-1, 1):
        ang = math.atan2(spread, h) * side
        c, s = math.cos(ang), math.sin(ang)
        # base of each board is out at y = side * spread, the tops meet at y = 0 and z = h
        def xf(p):
            x, y, z = p
            return (x, side * spread + y * c - z * s, y * s + z * c)
        board(m, name, w, math.hypot(h, spread), t, xf)
    def bar(p):
        x, y, z = p
        return (x, y * 1.0, z + 12.0)
    board(m, 'edge', w * .92, 1.4, spread * 2 * .55, bar)      # the spreader bar
    return m

def plate(name, w, h, t=0.8):
    # hung by its own centre; z runs -h/2..h/2 so a placement height is the plate's middle
    m = new_mesh()
    board(m, name, w, h, t, lambda p: (p[0], p[1], p[2] - h / 2.0))
    return m

SHAPES = {
    'CWC_Sign_AFrameCash':  (aframe('cash'), MAT),
    'CWC_Sign_AFrameKnock': (aframe('knock'), MAT),
    'CWC_Sign_AFrameJars':  (aframe('jars'), MAT),
    'CWC_Sign_PlateStill':  (plate('still', 42.0, 21.0), MAT),
    'CWC_Sign_PlateSharps': (plate('sharps', 32.0, 16.0), MAT),
    'CWC_Sign_PlateLogo':   (plate('logo', 26.0, 26.0), MAT_GLOW),
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pyn', required=True); ap.add_argument('--bgsm-dir', required=True); ap.add_argument('--out', required=True)
    a = ap.parse_args()
    sys.path.insert(0, a.pyn); sys.path.insert(0, a.bgsm_dir)
    from pyn.pynifly import NifFile
    from pyn.nifdefs import NiShapeBuf, PynBufferTypes
    import bgsm
    mdir = os.path.join(a.out, 'Meshes', 'Cyberware', 'Clinic', 'Signs')
    bdir = os.path.join(a.out, 'Materials', 'Cyberware', 'Clinic', 'Signs')
    os.makedirs(mdir, exist_ok=True); os.makedirs(bdir, exist_ok=True)
    plain = {'DiffuseTexture': TEX + '_d.dds', 'NormalTexture': TEX + '_n.dds', 'SmoothSpecTexture': TEX + '_s.dds',
             'environmentMapping': 0, 'specularEnabled': 1, 'specularColor': (1.0, 1.0, 1.0), 'specularMult': 0.5, 'smoothness': 0.45}
    data = bgsm.write(dict(plain)); open(os.path.join(bdir, MAT + '.bgsm'), 'wb').write(data)
    # environmentMapping stays 0: FO4 ignores a glow map when the env map is on (LESSONS)
    glow = dict(plain, GlowTexture=TEX + '_g.dds', emitEnabled=1, emittanceColor=(0.24, 0.92, 0.84), emittanceMult=2.2, glowmap=1)
    gd = bgsm.write(glow); open(os.path.join(bdir, MAT_GLOW + '.bgsm'), 'wb').write(gd)
    assert bgsm.read(gd)['glowmap'] and bgsm.read(gd)['emitEnabled']
    for name, (mesh, mat) in SHAPES.items():
        path = os.path.join(mdir, name + '.nif')
        nif = NifFile(); nif.initialize('FO4', path, root_type='BSFadeNode', root_name=name)
        props = NiShapeBuf(); props.bufType = PynBufferTypes.BSTriShapeBufType
        shp = nif.createShapeFromData(name, mesh['v'], mesh['f'], mesh['t'], mesh['n'], props=props, parent=nif.root)
        shp.shader.name = MAT_PATH % mat
        shp.save_shader_attributes()
        nif.save()
        back = NifFile(path); s0 = back.shapes[0]
        print('NIF', name, len(s0.verts), 'verts', 'shader', s0.shader.name)
    print('SIGNS_NIF OK')

if __name__ == '__main__':
    main()
