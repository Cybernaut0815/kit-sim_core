# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""views.exe ray by ray against Radiance rtrace (hard gates), and next to Ladybug's LB View Percent (soft, printed).

  uv run python -I views.py all [--exe PATH]                  # rays, then ladybug; exit 1 on a hard failure
  uv run python -I views.py rays [--exe PATH] [--case NAME ...]
  uv run python -I views.py ladybug [--exe PATH]

rays: each case runs `views --jitter off --out out.npy --dump-rays rays.npy` into _out/views/<case>/. The host rebuilds
every sensor as src/mesh.cpp make_sensors_into does without ladybug rules (face: Newell normal, area-weighted fan
centroid; vertex: the normalised sum of its faces' Newell vectors, (0,0,1) when zero; the eye --offset along the
normal, a back side at minus the offset with the negated normal; rows element by element, front then back) with the
core's float32 recentring on the bounding-box centre and offset_ray's nudge, and every ray of a row as
src/trace_dev.cu __raygen__view does at --jitter off (cell centres: the middle of the azimuth and of the sine of the
elevation; rows top-down from HI, columns clockwise; view mode: forward = the normal, right = normal x z, or, within 1e-3
of vertical, east turned by --north and made square to the normal, up = right x forward; 360 mode: forward = north
turned by --north, up = +z).
The blocking meshes (context, receivers unless --no-self-occlude / --no-occlude) go to Radiance as the core's
triangles at its float32 vertex positions, one plastic modifier per class (c2 obstruction, c3 + g target group g);
`rtrace -ab 0 -oLmN` gives each ray's first hit. There is no plane in the Radiance scene: on the host a downward ray
whose hit lies past where the core's ray ends (1 cm under the virtual ground, tmax = (o.z - plane + 0.01) / -dir.z),
or that hits nothing, is `ground` at its exact plane distance (eye on or below the plane: ground at 0.1 m, untraced),
with --virtual-ground off a downward miss is `ground` at +inf; a miss on or above the horizon (dir.z >= 0) is `sky`
(+inf). Own-surface rays (360 mode, --offset 0, n.d <= 0, the sensor's mesh blocking rays: not --no-self-occlude or
--no-occlude) must be class 254 and are not traced.
Per case, hard: >= 99.9 % of the compared rays in the same class, and >= 99.9 % of those with a finite distance within
|dt| <= 1e-5 t + 1e-4 m. Not compared: grazing hits (|cos| < 0.05 between the ray and the face rtrace hit), hits within
1 mm (in z) of where the core's downward rays end, and rays within 1e-6 of a sign test (n.d for the own surface,
dir.z; exact zeros agree on both sides). Geometry lying on the plane itself is compared: the core's ray goes on 1 cm
past it. The virtual ground is a surface too: a ray meeting it at |cos| = |dir.z| < 0.05 keeps its class check but not
its distance (the core's float32 direction is ~3e-8 off in dir.z, x 1 / |dir.z| in the plane distance: 8 cm at 350 m
for a ray 0.02 deg below the horizon). Also hard: the output columns recomputed from the dump itself (shares, reach
1e-9; log means 1e-6 relative: the core takes float logs; minima exact; the isovist, volume or area through the rays'
ends, 1e-5 relative: the core's float directions). A flat field (`--fov H,0,0`) culls no own surface.
ladybug: Ladybug's LB View Percent types 0 (HorizontalRadial, `--fov 360,-1,1`), 2 (Spherical, `360,-90,90`) and 3
(SkyExposure, `360,0,90`, the receivers block), its rays and weights from ladybug-core's view_sphere at resolutions 1
and 3, traced with rtrace from the eyes of a views run at --jitter off (a ray counts when it hits nothing; jitter
would spread type 0's band over -1..1 deg, into the terrain), next to views' unobstructed share (sky, plus ground for
types 0 and 2: Ladybug counts every ray that hits nothing). Printed, never fails. Type 1 (Horizontal30DegreeOffset) is
left out: its band depends on how the interpreter rounds.
The street scene (_out/views/street/) is generated here: a 300 m terrain quad, two rows of buildings along a 20 m
street, a tower, tree canopies, and a lake and a park lifted 5 cm above the terrain (targets); receivers `facade` (a
south facade, 5 m cells) and `street` (12 x 6 m cells on the street)."""
import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
OUT, FIX = HERE / '_out' / 'views', HERE / '_out' / 'fixtures'
RADIANCE = Path((HERE / '_dl' / 'radiance_path.txt').read_text().strip()) / 'bin'
RTRACE, OCONV = str(RADIANCE / 'rtrace.exe'), str(RADIANCE / 'oconv.exe')
BOX = HERE.parent / 'tests' / 'box.obj'
ROOM, ROOM_GRID = HERE.parent / 'tests' / 'room.obj', HERE.parent / 'tests' / 'room_grid.obj'
# the interior: a measure grid the rays ignore (--no-occlude), all around at eye height, the window glass
ROOM_FLAGS = ['--fov', '360', '--offset', '0.4', '--no-occlude', 'grid', '--target', 'tree=green', '--optics',
              'window=0,0.64,glass', '--reach', '6', '--cell', '3']
STREET = OUT / 'street'
KDEG = 3.14159265358979323846 / 180
MATCH, GRAZING, CUT_M, SIGN, TOL_REL, TOL_ABS = 0.999, 0.05, 1e-3, 1e-6, 1e-5, 1e-4
F32 = np.float32


TARGETS = ['--target', 'lake=water;park=park']
# name -> (receiver files, context files, flags); --jitter off and the dumps are added. At most ~300k rays each.
CASES = {
    'box_view': ([BOX], [], []),                                                        # defaults: 120,-70,50 at 2 deg
    'box_vertices': ([BOX], [], ['--on', 'vertices']),                                  # box corners: 3-face normals
    'box_360_both': ([BOX], [], ['--fov', '360', '--offset', '1.6', '--sides', 'both', '--cell', '3']),
    'box_360_own': ([BOX], [], ['--fov', '360', '--cell', '4']),                        # offset 0: own surface
    'box_360_open': ([BOX], [], ['--fov', '360', '--cell', '4', '--no-occlude', 'box']),   # ... but none on the box
    'fixture_box_north': ([FIX / 'box' / 'sensors.obj'], [FIX / 'box' / 'context.obj'],
                          ['--north', '30', '--cell', '3']),                            # up-facing: east turned by north
    'fixture_tilt30': ([FIX / 'tilt30' / 'sensors.obj'], [], ['--cell', '3']),
    'street_facade': ([STREET / 'facade.obj'], [STREET / 'context.obj'],
                      TARGETS + ['--distance', 'mean,min,group', '--reach', 'en17037']),
    'street_360': ([STREET / 'street.obj'], [STREET / 'context.obj'],
                   TARGETS + ['--fov', '360', '--offset', '1.6', '--north', '20', '--distance', 'mean,min,group',
                              '--reach', '6,20,50,100']),
    'street_vg_off': ([STREET / 'facade.obj'], [STREET / 'context.obj'], TARGETS + ['--virtual-ground', 'off', '--cell', '3']),
    'street_ground_z': ([STREET / 'facade.obj'], [STREET / 'context.obj'], TARGETS + ['--ground-z', '6', '--cell', '3']),
    'room_glass': ([ROOM_GRID], [ROOM], ROOM_FLAGS + ['--transmission', 'on']),   # out through the window
    'room_shut': ([ROOM_GRID], [ROOM], ROOM_FLAGS),                               # the window blocks
    'room_2d': ([ROOM_GRID], [ROOM], ['--fov', '360,0,0'] + ROOM_FLAGS[2:] + ['--transmission', 'on', '--far', '50',
                                                                          '--distance', 'isovist,mean,min']),   # horizontal
}
# LB View Percent type -> (name, views --fov, views' unobstructed classes); sensor sets (receivers, context, --offset)
LB_TYPES = {0: ('HorizontalRadial', '360,-1,1', ('sky', 'ground')), 2: ('Spherical', '360,-90,90', ('sky', 'ground')),
            3: ('SkyExposure', '360,0,90', ('sky',))}
LB_SETS = {'street': ([STREET / 'street.obj'], [STREET / 'context.obj'], '1.6'),
           'facade': ([STREET / 'facade.obj'], [STREET / 'context.obj'], '0.1')}


# ---------------------------------------------------------------- scene and meshes
def grid(o, u, v, nu, nv):
    """main.cpp grid(): nu x nv quads from o along u and v (normal u x v) -> (points, faces)."""
    o, u, v = (np.asarray(a, float) for a in (o, u, v))
    pts = [o + u * i / nu + v * j / nv for j in range(nv + 1) for i in range(nu + 1)]
    return pts, [[j * (nu + 1) + i, j * (nu + 1) + i + 1, (j + 1) * (nu + 1) + i + 1, (j + 1) * (nu + 1) + i]
                 for j in range(nv) for i in range(nu)]


def box(x0, y0, z0, x1, y1, z1, skip=()):
    """Box as outward quads (bottom, top, south, east, north, west), leaving out the faces named in skip."""
    c = [(x1 if k & 1 else x0, y1 if k & 2 else y0, z1 if k & 4 else z0) for k in range(8)]
    quads = {'bottom': [0, 2, 3, 1], 'top': [4, 5, 7, 6], 'south': [0, 1, 5, 4], 'east': [1, 3, 7, 5],
             'north': [3, 2, 6, 7], 'west': [2, 0, 4, 6]}
    return c, [q for k, q in quads.items() if k not in skip]


def join(parts):
    pts, faces = [], []
    for p, f in parts:
        faces += [[k + len(pts) for k in q] for q in f]
        pts += list(p)
    return pts, faces


def write_obj(path, meshes):
    lines, base = [], 1
    for name, (pts, faces) in meshes.items():
        lines.append(f'o {name}')
        lines += ['v %.17g %.17g %.17g' % tuple(p) for p in pts]
        lines += ['f ' + ' '.join(str(k + base) for k in f) for f in faces]
        base += len(pts)
    path.write_text('\n'.join(lines) + '\n')


def street_scene():
    """Deterministic street / lake / park scene -> _out/views/street/{facade,street,context}.obj."""
    north = [(-90, 18), (-40, 30), (60, 24)]          # x0, height: blocks 40 x 30 m north of the street (the park at 10)
    south = [(-100, 21), (-50, 15), (60, 9)]          # ... south of it (the lake reaches the street at -10..50)
    buildings = [box(x, 10, 0, x + 40, 40, h, ('bottom', 'south') if x == -40 else ('bottom',)) for x, h in north]
    buildings += [box(x, -40, 0, x + 40, -10, h, ('bottom',)) for x, h in south]
    buildings.append(box(-120, 70, 0, -90, 100, 60, ('bottom',)))                                       # a tower
    trees = [box(x, y, 3, x + 6, y + 6, 9) for x, y in ((15, 20), (35, 30), (20, 60), (38, 80), (15, 100))]   # park
    trees += [box(x, 6.5, 2.5, x + 3, 9.5, 7) for x in (-55, -25, 75)]                                 # street trees
    STREET.mkdir(parents=True, exist_ok=True)
    write_obj(STREET / 'facade.obj', {'facade': grid((-40, 10, 0), (40, 0, 0), (0, 0, 30), 8, 6)})   # faces south
    write_obj(STREET / 'street.obj', {'street': grid((-60, -6, 0), (120, 0, 0), (0, 12, 0), 10, 2)})
    write_obj(STREET / 'context.obj', {'terrain': grid((-150, -150, 0), (300, 0, 0), (0, 300, 0), 1, 1),
                                       'buildings': join(buildings), 'trees': join(trees),
                                       'lake': grid((-10, -110, 0.05), (60, 0, 0), (0, 98, 0), 1, 1),
                                       'park': grid((10, 12, 0.05), (40, 0, 0), (0, 108, 0), 1, 1)})


def read_obj(path):
    """src/mesh.cpp read_obj: one mesh per o/g group (the file's stem before any), its points the vertices its faces use,
    in file order -> [(name, points [P x 3], faces as local index lists)]."""
    V, meshes, faces, name = [], [], [], Path(path).stem

    def finish():
        if faces:
            used = sorted({k for f in faces for k in f})
            at = {g: i for i, g in enumerate(used)}
            meshes.append((name, np.array([V[g] for g in used]), [[at[k] for k in f] for f in faces]))
    for line in Path(path).read_text().splitlines():
        t = line.split()
        if t and t[0] == 'v':
            V.append([float(x) for x in t[1:4]])
        elif t and t[0] == 'f':
            f = []
            for x in t[1:]:
                if x.startswith('#'):
                    break
                k = int(x.split('/')[0])
                f.append(k - 1 if k > 0 else len(V) + k)
            if f:
                faces.append(f)
        elif t and t[0] in ('o', 'g'):
            finish()
            faces, name = [], t[1] if len(t) > 1 else Path(path).stem
    finish()
    return meshes


def newell(P):
    a, b = P, np.roll(P, -1, axis=0)
    return np.array([((a[:, 1] - b[:, 1]) * (a[:, 2] + b[:, 2])).sum(), ((a[:, 2] - b[:, 2]) * (a[:, 0] + b[:, 0])).sum(),
                     ((a[:, 0] - b[:, 0]) * (a[:, 1] + b[:, 1])).sum()])


def elements(meshes, on):
    """make_sensors_into's (point, unit normal) per element of the receiver meshes, physical rules."""
    out = []
    for _, pts, faces in meshes:
        vn = np.zeros_like(pts)
        for f in faces:
            P = pts[f]
            N = newell(P)
            ln = np.linalg.norm(N)
            Nu = N / ln if ln > 0 else np.zeros(3)
            nrm = Nu if ln / 2 >= 1e-12 else np.zeros(3)
            if on == 'vertices':
                vn[f] += nrm * (ln / 2)
                continue
            cen, s = np.zeros(3), 0.0
            for a, b in zip(P[1:-1], P[2:]):          # signed-area-weighted fan centroid
                w = np.dot(np.cross(a - P[0], b - P[0]), Nu) / 2
                cen, s = cen + (P[0] + a + b) * (w / 3), s + w
            out.append((cen / s if s > 1e-12 else P.mean(axis=0), nrm))
        if on == 'vertices':
            for p, n in zip(pts, vn):
                ln = np.linalg.norm(n)
                out.append((p, n / ln if ln > 0 else np.array([0.0, 0, 1])))
    return out


def offset_ray(p, n):
    """trace_dev.cu offset_ray (Waechter & Binder) on float32 [S x 3] points and normals."""
    oi = (F32(256) * n).astype(np.int32)
    moved = (p.view(np.int32) + np.where(p < 0, -oi, oi)).view(F32)
    return np.where(np.abs(p) < F32(1 / 32), p + F32(1 / 65536) * n, moved).astype(F32)


def flag(flags, name, default=None):
    return flags[flags.index(name) + 1] if name in flags else default


def no_occlude(flags):
    """The --no-occlude mesh names (every such flag, ;-separated)."""
    return set(';'.join(flags[k + 1] for k, f in enumerate(flags) if f == '--no-occlude').split(';'))


def layout(receivers, context, flags):
    """-> (receiver meshes, blocking meshes [(points, faces, class code)], bbox centre C, virtual ground z): the
    receivers are mesh-major in file order; a target mesh gets 3 + its group's first appearance."""
    rec = [m for p in receivers for m in read_obj(p)]
    ctx = [m for p in context for m in read_obj(p)]
    allp = np.vstack([m[1] for m in rec + ctx])
    C = (allp.min(axis=0) + allp.max(axis=0)) / 2
    groups, code = [], {}
    for spec in ';'.join(flags[k + 1] for k, f in enumerate(flags) if f == '--target').split(';'):
        if spec:
            mesh, g = spec.rsplit('=', 1)
            groups += [g] if g not in groups else []
            code[mesh] = 3 + groups.index(g)
    open_ = no_occlude(flags)
    glass = set()   # meshes the rays pass: --transmission on and a whole-mesh --optics row NAME=0,TAU,glass
    if flag(flags, '--transmission') == 'on':
        for spec in ';'.join(flags[k + 1] for k, f in enumerate(flags) if f == '--optics').split(';'):
            mesh, _, v = spec.rpartition('=')
            v = v.split(',')
            if len(v) == 3 and v[2] == 'glass' and float(v[1]) > 0:
                glass.add(mesh)
    blocking = [(p, f, code.get(n, 2)) for n, p, f in ctx if n not in glass]
    if '--no-self-occlude' not in flags:
        blocking += [(p, f, code.get(n, 2)) for n, p, f in rec if n not in open_ and n not in glass]
    panes = [(p, f, 2) for n, p, f in ctx + rec if n in glass]
    gz = float(flag(flags, '--ground-z', allp[:, 2].min()))
    return rec, blocking, C, gz, panes


def radiance(d, blocking, C, name='scene'):
    """The blocking meshes as the core's triangles (quads split 0-1-2, 0-2-3) at its float32 positions -> frozen octree."""
    lines = [f'void plastic c{c} 0 0 5 .5 .5 .5 0 0' for c in sorted({b[2] for b in blocking})]
    k = 0
    for pts, faces, c in blocking:
        P = (np.asarray(pts) - C).astype(F32).astype(np.float64) + C
        for f in faces:
            assert len(f) in (3, 4), 'tri / quad faces only (the core triangulates n-gons by earcut)'
            for t in ([f] if len(f) == 3 else [[f[0], f[1], f[2]], [f[0], f[2], f[3]]]):
                lines.append(f'c{c} polygon t{k} 0 0 9 ' + ' '.join('%.17g' % x for x in P[t].ravel()))
                k += 1
    (d / f'{name}.rad').write_text('\n'.join(lines) + '\n')
    with open(d / f'{name}.oct', 'wb') as f:
        subprocess.run([OCONV, '-f', str(d / f'{name}.rad')], stdout=f, check=True)
    return d / f'{name}.oct'


def rtrace(octree, org, dirs):
    """First hits -> (t, class code, |cos| with the hit face's normal); t = inf, code -1 where nothing is hit."""
    if not len(org):
        return np.zeros(0), np.zeros(0, int), np.zeros(0)
    inp = np.hstack([org, dirs]).astype('<f8')

    def one(rows):
        r = subprocess.run([RTRACE, '-h', '-fda', '-ab', '0', '-oLmN', str(octree)], input=rows.tobytes(),
                           capture_output=True, check=False)
        if r.returncode:
            raise RuntimeError(f'rtrace exit {r.returncode}: {r.stderr.decode(errors="replace")[-300:]}')
        return r.stdout.split()
    with ThreadPoolExecutor(os.cpu_count()) as ex:   # rtrace has no -n on Windows: one process per slice
        tok = [t for part in ex.map(one, np.array_split(inp, min(os.cpu_count(), len(inp) // 5000 + 1))) for t in part]
    assert len(tok) == 5 * len(inp), (len(tok), len(inp))
    t = np.array(tok[0::5]).astype(np.float64)
    names, inv = np.unique(np.array(tok[1::5]), return_inverse=True)
    code = np.array([-1 if n == b'*' else int(n[1:]) for n in names])[inv]
    n = np.stack([np.array(tok[k::5]).astype(np.float64) for k in (2, 3, 4)], axis=1)
    cos = np.abs((n * dirs).sum(axis=1)) / np.maximum(np.linalg.norm(n, axis=1), 1e-300)
    return np.where(code < 0, np.inf, t), code, cos


# ---------------------------------------------------------------- the views run and its rays
def run_views(exe, receivers, context, flags, d):
    d.mkdir(parents=True, exist_ok=True)
    args = [str(p) for p in receivers] + [x for c in context for x in ('--context', str(c))] + flags + ['--out', 'out.npy']
    r = subprocess.run([str(exe)] + args, cwd=d, capture_output=True, text=True, errors='replace', check=False)
    if r.returncode:
        raise RuntimeError(f'views exit {r.returncode}: {r.stderr.strip()[-500:]}')
    return json.loads((d / 'out.npy.json').read_text()), np.load(d / 'out.npy')


def eyes(rec, flags, C):
    """Rows in dump order -> (float32 recentred sensor points after offset_ray, float32 normals, whether each one's mesh
    blocks rays: the views run's own surface is a blocking receiver's)."""
    on, sides, off = flag(flags, '--on', 'faces'), flag(flags, '--sides', 'front'), float(flag(flags, '--offset', 0))
    open_ = no_occlude(flags)
    P, N, B = [], [], []
    for m in rec:
        blocks = '--no-self-occlude' not in flags and m[0] not in open_
        for q, n in elements([m], on):
            if sides != 'back':
                P.append(q + n * off)
                N.append(n)
                B.append(blocks)
            if sides != 'front':
                P.append(q - n * off)
                N.append(n * -1)
                B.append(blocks)
    P32, N32 = (np.array(P) - C).astype(F32), np.array(N).astype(F32)
    return offset_ray(P32, N32), N32, np.array(B, bool)


def local_dirs(view):
    """params.h view_local at --jitter off -> the directions in the field's own frame (x right, y forward, z up) [K x 3]."""
    H, LO, HI = view['fov']
    nEl, nAz = view['grid']
    dEl, k = (HI - LO) / nEl, np.arange(nEl)
    bot, top = (np.sin((HI - j * dEl) * KDEG).astype(F32) for j in (k + 1, k))           # rowSin: float32 sines
    s = (F32(0.5) * (bot + top)).astype(np.float64)
    arg = (F32(0 if H == 360 else -H / 2) + (np.arange(nAz).astype(F32) + F32(0.5)) * F32(H / nAz)) * F32(1 / 180)
    arg = arg.astype(np.float64)
    q = np.round(2 * arg)                       # sincospif is exact at half-integers (cos 90 deg = 0, not 6e-17)
    sa = np.where(q == 2 * arg, np.array([0.0, 1, 0, -1])[q.astype(int) % 4], np.sin(np.pi * arg))
    ca = np.where(q == 2 * arg, np.array([1.0, 0, -1, 0])[q.astype(int) % 4], np.cos(np.pi * arg))
    c = np.sqrt(np.maximum(0, 1 - s * s))
    return np.stack([(c[:, None] * sa[None, :]).ravel(), (c[:, None] * ca[None, :]).ravel(), np.repeat(s, nAz)], axis=1)


def rays(view, N32):
    """__raygen__view at --jitter off -> unit directions [S x K x 3], d = row * nAz + column."""
    H = view['fov'][0]
    loc = local_dirs(view)
    rad = view['north'] * KDEG
    cn, sn = float(F32(np.cos(rad))), float(F32(np.sin(rad)))
    n = N32.astype(np.float64)
    if H == 360:
        f = np.broadcast_to([-sn, cn, 0.0], n.shape)
        r = np.broadcast_to([cn, sn, 0.0], n.shape)
    else:                                       # right = n x z normalised; within 1e-3 of vertical: east turned by
        f, ln = n, np.hypot(n[:, 0], n[:, 1])   # north, made square to the normal
        side = np.stack([n[:, 1], -n[:, 0], np.zeros(len(n))], axis=1) / np.where(ln > 0, ln, 1)[:, None]
        east = np.array([cn, sn, 0.0]) - (n @ np.array([cn, sn, 0.0]))[:, None] * n
        east /= np.maximum(np.linalg.norm(east, axis=1, keepdims=True), 1e-300)   # 0 only where side is taken
        r = np.where((np.sqrt(N32[:, 0] * N32[:, 0] + N32[:, 1] * N32[:, 1]) > F32(1e-3))[:, None], side, east)
    u = np.cross(r, f)
    cs, cc, sd = loc[:, 0], loc[:, 1], loc[:, 2]                                            # per direction d
    D = cs[None, :, None] * r[:, None, :] + cc[None, :, None] * f[:, None, :] + sd[None, :, None] * u[:, None, :]
    return D / np.linalg.norm(D, axis=2, keepdims=True)


def expect(view, o32, N32, blocks, C, gz, dirs, octree, vground, panes=None):
    """The host's class and distance of every ray -> (cls uint8, t float64, excluded, distance not compared, why) [S x K].
    blocks: per sensor, its mesh blocks rays (an own surface). panes: the octree of the glass the rays pass
    (--transmission on): a ray meeting it before its end gets + 128."""
    S, K = dirs.shape[:2]
    cls, t = np.full((S, K), 2, np.uint8), np.full((S, K), np.inf)
    dz, eye = dirs[..., 2], o32.astype(np.float64) + C
    nd = (dirs * N32.astype(np.float64)[:, None, :]).sum(axis=2)
    own = np.zeros((S, K), bool)
    sign = (np.abs(dz) < SIGN) & (dz != 0)      # dir.z decides sky / ground and the virtual ground; exact zeros agree
    if view['fov'][0] == 360 and view['offset'] == 0 and view['fov'][1:] != [0, 0]:   # a flat field culls nothing
        own = (nd <= 0) & blocks[:, None]
        sign |= (np.abs(nd) < SIGN) & (nd != 0) & blocks[:, None]
        cls[own] = 254
    down = dz < 0
    below = vground & down & ~(o32[:, 2] > F32(gz - C[2]))[:, None] & ~own   # the core's float test of its eye
    cls[below], t[below] = 1, float(F32(0.1))
    go = ~own & ~below
    ht, hc, hcos = rtrace(octree, np.broadcast_to(eye[:, None, :], dirs.shape)[go], dirs[go])
    dzg = dz[go]
    tg = (eye[:, 2:3] - gz) / -np.where(down, dz, -1)                     # plane distance of downward rays
    cut = (eye[:, 2:3] - gz + 0.01) / -np.where(down, dz, -1)              # where the core's downward ray ends
    plane, tg, cut = (vground & down)[go], tg[go], cut[go]
    hit = np.isfinite(ht) & ~(plane & (ht > cut))                           # past the end: the ray meets the ground
    with np.errstate(invalid='ignore'):
        near = plane & np.isfinite(ht) & (np.abs(ht - cut) * -dzg < CUT_M)
    graze = hit & (hcos < GRAZING)
    cls[go] = np.where(hit, hc, np.where(dzg >= 0, 0, 1))   # the horizon is sky
    t[go] = np.where(hit, ht, np.where(plane, tg, np.inf))
    out, nodist = sign.copy(), np.zeros((S, K), bool)
    out[go] |= graze | near
    if panes is not None:   # glass before the end (the hit, the ground cut of a downward ray, else none)
        pt, _, pcos = rtrace(panes, np.broadcast_to(eye[:, None, :], dirs.shape)[go], dirs[go])
        end = np.where(hit, ht, np.where(plane, cut, np.inf))
        glazed = np.isfinite(pt) & (pt < end)
        sub = cls[go]
        sub[glazed] |= 128
        cls[go] = sub
        with np.errstate(invalid='ignore'):
            out[go] |= np.isfinite(pt) & ((pcos < GRAZING) | (np.abs(pt - end) < CUT_M))   # grazing a pane, or at the end
    nodist[go] = plane & ~hit & (-dzg < GRAZING)     # the virtual ground met at |cos| < 0.05: class only
    why = {'sign ties': int(sign.sum()), 'grazing': int((graze & ~sign[go]).sum()),
           'at the ground cut': int((near & ~graze & ~sign[go]).sum())}
    return cls, t, out, nodist & ~out, why


def columns(man, vals, cls, dep):
    """The output columns recomputed from the dump -> {share: worst share / reach diff, mean: worst log-mean relative
    diff, min: minima differing, iso: worst volume / area relative diff}, as src/views.cpp store() and gpu.cu k_view
    compute them."""
    v = man['view']
    H, LO, HI = v['fov']
    nEl, nAz = v['grid']
    flat = LO == HI == 0
    far32, near32 = F32(v['far']), F32(0.1)
    k = np.arange(nEl)
    wr = (H / nAz) * KDEG * (np.ones(nEl) if flat else np.sin((HI - k * ((HI - LO) / nEl)) * KDEG) - np.sin((HI - (k + 1) * ((HI - LO) / nEl)) * KDEG))
    w = np.repeat(wr / (wr * nAz).sum(), nAz)
    own = cls == 254
    glazed = ~own & (cls >= 128)
    cls = np.where(own, cls, cls & 127)
    d = np.clip(dep, near32, far32)
    lnd = np.where(d == far32, np.log(v['far']), np.where(d == near32, np.log(0.1), np.log(d.astype(np.float64))))
    total = w.sum()
    groups = v['classes'][3:]
    exp = {name: (w * (cls == c)).sum(axis=1) / total for c, name in enumerate(v['classes'])}
    exp['obstruction'] = exp['obstruction'] + (w * own).sum(axis=1) / total
    exp['obstruction_total'] = (w * (cls != 0)).sum(axis=1) / total   # all that is not sky, the own surface too

    def logmean(m):
        ws = (w * m).sum(axis=1)
        with np.errstate(invalid='ignore', divide='ignore'):
            return np.where(ws > 0, np.exp((w * lnd * m).sum(axis=1) / ws), np.nan)

    def least(m):
        mn = np.where(m, d, np.inf).min(axis=1).astype(np.float64)
        return np.where(np.isinf(mn), np.nan, np.where(mn <= near32, 0.1, np.where(mn >= far32, v['far'], mn)))
    exp['distance'], exp['distance_min'] = logmean(~own), least(~own)
    exp['glazing'] = (w * glazed).sum(axis=1) / total
    for g, name in enumerate(groups):
        exp[f'{name}_distance'], exp[f'{name}_distance_min'] = logmean(cls == 3 + g), least(cls == 3 + g)
    for D in v['reach']:
        exp['reach_%g' % D] = (w * (~own & (dep >= F32(D)))).sum(axis=1) / total
    # the isovist through the rays' ends (min(t, far) along the field-frame directions, own surface at the eye): segments
    # to the next column (flat), tetrahedra eye-A-B-C and eye-B-D-C per quad; 360 wraps the columns
    U = local_dirs(v).reshape(nEl, nAz, 3)
    G = np.where(own, 0, np.minimum(dep.astype(np.float64), v['far'])).reshape(-1, nEl, nAz)
    U1, G1 = np.roll(U, -1, axis=1), np.roll(G, -1, axis=2)
    if flat:
        part = 0.5 * G * G1 * (U[..., 1] * U1[..., 0] - U[..., 0] * U1[..., 1])
    else:
        def det(x, y, z):
            return np.einsum('...i,...i', x, np.cross(y, z))
        part = (G[:, :-1] * G1[:, :-1] * G[:, 1:] * det(U[:-1], U1[:-1], U[1:])
                + G1[:, :-1] * G1[:, 1:] * G[:, 1:] * det(U1[:-1], U1[1:], U[1:])) / 6
    if H != 360:
        part[..., -1] = 0                       # a fan's last column has no next
    exp['area' if flat else 'volume'] = part.sum(axis=(1, 2))
    nS = 2 if any(c.endswith('_back') for c in man['columns']) and any(c.endswith('_front') for c in man['columns']) else 1
    worst = {'share': 0.0, 'mean': 0.0, 'min': 0, 'iso': 0.0}
    for j, col in enumerate(man['columns']):
        name = col.rsplit('_', 1)[0]
        e = exp[name][(j % nS)::nS]
        a = vals[:, j]
        both = np.isnan(a) & np.isnan(e)
        if name in ('volume', 'area'):
            with np.errstate(invalid='ignore', divide='ignore'):
                worst['iso'] = max(worst['iso'], float(np.nanmax(np.where(e == a, 0, np.abs(a / e - 1)), initial=0)))
        elif name.endswith('distance_min'):
            worst['min'] += int((~both & (a != e)).sum())
        elif name.endswith('distance'):
            rel = np.inf if (np.isnan(a) != np.isnan(e)).any() else np.nanmax(np.abs(a / e - 1), initial=0)
            worst['mean'] = max(worst['mean'], float(rel))
        else:
            worst['share'] = max(worst['share'], float(np.abs(a - e).max()))
    return worst


def case_rays(exe, name, receivers, context, flags):
    """One case -> (ok, summary line, detail lines)."""
    d = OUT / name
    flags = flags + ['--jitter', 'off', '--dump-rays', 'rays.npy']
    man, vals = run_views(exe, receivers, context, flags, d)
    gcls, gdep = np.load(d / 'rays.npy'), np.load(d / 'rays.npy.depth.npy')
    rec, blocking, C, gz, panes = layout(receivers, context, flags)
    v = man['view']
    assert abs(v['ground_z'] - gz) <= 1e-9 * max(1, abs(gz)), (v['ground_z'], gz)
    o32, N32, blocks = eyes(rec, flags, C)
    dirs = rays(v, N32)
    assert gcls.shape == dirs.shape[:2], (gcls.shape, dirs.shape)
    cls, t, ex, nodist, why = expect(v, o32, N32, blocks, C, gz, dirs, radiance(d, blocking, C), v['virtual_ground'],
                                     radiance(d, panes, C, 'panes') if panes else None)
    gt = gdep.astype(np.float64)
    same = gcls == cls
    fin = (np.isfinite(t) | np.isfinite(gt)) & ~nodist
    with np.errstate(invalid='ignore'):
        dt = np.where(fin, np.abs(gt - t), 0)
        lim = TOL_REL * np.where(np.isfinite(t), t, 0) + TOL_ABS
    dok = ~fin | (np.isfinite(t) & np.isfinite(gt) & (dt <= lim))
    use = ~ex
    nUse, nSame = int(use.sum()), int((same & use).sum())
    nFin = int((same & use & fin).sum())
    nDist = int((same & use & fin & dok).sum())
    worst = columns(man, vals, gcls, gdep)
    cmatch, dmatch = nSame / max(nUse, 1), nDist / nFin if nFin else 1.0
    colok = worst['share'] <= 1e-9 and worst['mean'] <= 1e-6 and worst['min'] == 0 and worst['iso'] <= 1e-5
    ok = cmatch >= MATCH and dmatch >= MATCH and colok
    rel = np.where(same & use & fin & np.isfinite(dt), dt / lim, 0)
    names = dict(enumerate(v['classes'])) | {254: 'own'}
    seen = ' '.join(f'{names[c & 127] if c != 254 else "own"}{"+glass" if 128 <= c < 254 else ""} {n}'
                    for c, n in zip(*np.unique(gcls, return_counts=True)))
    line = (f'{"ok  " if ok else "FAIL"} {name}: {gcls.size} rays ({seen}), {nUse} compared ({gcls.size - nUse} not: '
            + ', '.join(f'{n} {k}' for k, n in why.items()) + '), '
            f'class {100 * cmatch:.3f} %, distance {100 * dmatch:.3f} % of {nFin} (worst {rel.max():.2g} x tol; '
            f'{int(nodist.sum())} grazing the virtual ground: class only), '
            f'columns: shares {worst["share"]:.1e}, log means {worst["mean"]:.1e}, minima off {worst["min"]}, '
            f'isovist {worst["iso"]:.1e}')
    detail = []
    bad = np.argwhere(use & ~(same & dok))
    nEl, nAz = v['grid']
    for si, di in bad[:8]:
        detail.append(f'       sensor {si} row {di // nAz} column {di % nAz} dir {np.round(dirs[si, di], 6).tolist()}: '
                      f'views {gcls[si, di]} at {gdep[si, di]:.6g}, rtrace {cls[si, di]} at {t[si, di]:.6g}')
    if len(bad) > 8:
        detail.append(f'       ... {len(bad) - 8} more')
    return ok, line, detail


# ---------------------------------------------------------------- Ladybug LB View Percent (soft)
def ladybug(exe):
    """-> info lines: per sensor set and type, views' unobstructed share next to LB View Percent at resolutions 1, 3."""
    try:
        from ladybug.viewsphere import view_sphere
    except ImportError as e:
        return [f'skip ladybug: {e}']

    def lb_rays(typ, res):   # LB View Percent's directions and weights (mean 1)
        if typ == 0:
            v = view_sphere.horizontal_radial_vectors(30 * res)
            return np.array([[p.x, p.y, p.z] for p in v]), np.ones(len(v))
        v, w = (view_sphere.sphere_patches(res)[1], view_sphere.sphere_patch_weights(res)) if typ == 2 else \
            (view_sphere.dome_patches(res)[1], view_sphere.dome_patch_weights(res))
        return np.array([[p.x, p.y, p.z] for p in v]), np.array(w)
    lines = []
    for sname, (receivers, context, off) in LB_SETS.items():
        rec, blocking, C, gz, _ = layout(receivers, context, ['--offset', off])
        d = OUT / 'ladybug' / sname
        d.mkdir(parents=True, exist_ok=True)
        octree = radiance(d, blocking, C)
        eye = eyes(rec, ['--offset', off], C)[0].astype(np.float64) + C
        for typ, (tname, fov, unob) in LB_TYPES.items():
            flags = ['--fov', fov, '--offset', off, '--jitter', 'off', '--distance', 'none']   # 360,-1,1: no isovist
            man, vals = run_views(exe, receivers, context, flags, d / f'type{typ}')
            ours = 100 * sum(vals[:, man['columns'].index(f'{c}_front')] for c in unob)
            line = f'info ladybug type {typ} {tname} {sname} ({len(eye)} sensors): views {" ".join(flags)} {ours.mean():.2f} %'
            for res in (1, 3):
                V, W = lb_rays(typ, res)
                V /= np.linalg.norm(V, axis=1, keepdims=True)
                ht = rtrace(octree, np.repeat(eye, len(V), axis=0), np.tile(V, (len(eye), 1)))[0]
                lb = 100 * (np.isinf(ht).reshape(len(eye), len(V)) * W).sum(axis=1) / len(V)   # Ladybug divides by the count
                diff = ours - lb
                line += (f'; ladybug resolution {res} ({len(V)} rays) {lb.mean():.2f} %, views - ladybug mean '
                         f'{diff.mean():+.2f}, max |{np.abs(diff).max():.2f}| points')
            lines.append(line)
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['all', 'rays', 'ladybug'])
    ap.add_argument('--exe', default=str(HERE.parent / 'build' / 'RelWithDebInfo' / 'views.exe'))
    ap.add_argument('--case', nargs='*', default=list(CASES), choices=list(CASES))
    a = ap.parse_args()
    exe = Path(a.exe).resolve()
    if not exe.exists():
        sys.exit(f'error: --exe {exe} not found: build the CLI first (cmake --build --preset release)')
    if not all((FIX / s / 'sensors.obj').exists() for s in ('box', 'tilt30')):
        subprocess.run([sys.executable, '-I', str(HERE / 'fixtures.py')], check=True)
    street_scene()
    fails = 0
    if a.cmd in ('all', 'rays'):
        for name in a.case:
            try:
                ok, line, detail = case_rays(exe, name, *CASES[name])
            except (RuntimeError, AssertionError, subprocess.CalledProcessError) as e:
                ok, line, detail = False, f'FAIL {name}: {e}', []
            fails += not ok
            print(line, flush=True)
            for l in detail:
                print(l)
    if a.cmd in ('all', 'ladybug'):
        for line in ladybug(exe):
            print(line, flush=True)
    print(f'{fails} hard failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
