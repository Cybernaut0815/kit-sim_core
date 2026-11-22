# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""python -m daylight.check: `to_dmesh` round trip and hole / winding / degenerate-face masking on an in-memory
stage (no exe needed), then convert validate/_out/fixtures/usd_xform/usd_xform.usda (fixtures.py) and compare
counts, winding and world points with values derived from the fixture generator (scene_box authored on a Y-up cm
stage: nested rotations, leftHanded sensors, mirrored box_b, guide / inactive / invisible lids), then `run_meshes`
end to end when the exes exist (daylight: sun hours of one quad with / without a slab above it; views: named meshes
and --target), then the colormap stops and `write_results` (hole -> nan / grey, vertex mode, mismatch, skipped
instance proxy / instancer name), then `native` in-process when daylight_c.dll exists (tests/c_abi.c's cases)."""
import json
import locale
import os
import pickle
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from unittest import mock

import numpy as np
from pxr import Sdf, Usd, UsdGeom, Vt

from . import _native
from .core import REPO, VIRIDIS, colormap, run_meshes, to_dmesh
from .native import Control, DaylightError, SimulationMesh, _run, run, usage
from .usd import meshes, to_obj, write_results

FIX = REPO / 'validate' / '_out' / 'fixtures' / 'usd_xform' / 'usd_xform.usda'
TESTS, BUILD = REPO / 'tests', REPO / 'build'                       # BUILD: ctest's smoke.npy, views_smoke.npy
QUAD = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], float)
FACADE = np.array([[-1, 0, 0], [1, 0, 0], [1, 0, 2], [-1, 0, 2]], float)   # 2 x 2 m facing -y, its foot at z 0
SUN = ['--lat', '41.98', '--lon', '-87.92', '--tz', '-6', '--quantity', 'sunhours', '--time', '06-21T12:00']
OPEN = ['--no-self-occlude', '--jitter', 'off']


def check_dmesh():
    P = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0], [2, 0, 1]], float)
    counts, idx = [4, 3], [0, 1, 2, 3, 1, 4, 2]
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / 'm.dmesh'
        to_dmesh(P, counts, idx, f)
        b = f.read_bytes()
        assert struct.unpack_from('<4sIQQQ', b) == (b'DLM1', 0, 5, 2, 7) and len(b) == 32 + 24 * 5 + 4 * 2 + 4 * 7, len(b)
        assert np.array_equal(np.frombuffer(b, '<f8', 15, 32).reshape(-1, 3), P)
        assert np.frombuffer(b, '<i4', 2, 152).tolist() == counts and np.frombuffer(b, '<i4', 7, 160).tolist() == idx
        for bad in (counts, idx[:-1]), ([2, 2, 3], idx), (counts, idx[:-1] + [5]):    # sum, count < 3, index range
            try:
                to_dmesh(P, *bad, f)
                raise AssertionError(bad)
            except ValueError:
                pass
        to_obj([('e', P[:2], [], [])], f)
        assert f.read_text() == 'o e\nv 0 0 0\nv 1 0 0\n', f.read_text()       # 0 faces: no `f` line


def check_masking():
    """counts [4,3,2,3], face 1 a hole, face 2 degenerate, leftHanded on a Z-up metre stage -> 2 faces, reversed."""
    st = Usd.Stage.CreateInMemory()
    UsdGeom.SetStageUpAxis(st, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(st, 1.0)
    m = UsdGeom.Mesh.Define(st, '/m')
    m.CreatePointsAttr(Vt.Vec3fArray.FromNumpy(np.arange(21, dtype=np.float32).reshape(7, 3)))
    m.CreateFaceVertexCountsAttr(Vt.IntArray([4, 3, 2, 3]))
    m.CreateFaceVertexIndicesAttr(Vt.IntArray([0, 1, 2, 3, 1, 4, 2, 5, 6, 2, 4, 6]))
    m.CreateHoleIndicesAttr(Vt.IntArray([1, 9]))
    m.CreateOrientationAttr(UsdGeom.Tokens.leftHanded)
    (name, P, C, I), = meshes(st)
    assert name == '/m' and P.dtype == np.float64 and np.array_equal(P, np.arange(21).reshape(7, 3)), (name, P)
    assert C.tolist() == [4, 3] and I.tolist() == [3, 2, 1, 0, 6, 4, 2], (C, I)
    assert C.dtype == I.dtype == np.int32
    m.GetFaceVertexCountsAttr().Set(Vt.IntArray([4, -1, 6, 3]))     # negative count: ValueError, not a numpy crash
    try:
        meshes(st)
        raise AssertionError('negative count accepted')
    except ValueError as e:
        assert '/m' in str(e), e


def check_exe(name, fn, tries=5):
    """fn() runs an exe: retried while it is missing, old or rebuilding, then skipped."""
    for k in range(tries):
        try:
            fn()
            return print(f'{name} OK')
        except (OSError, subprocess.CalledProcessError, ValueError) as e:     # missing / old / rebuilding exe
            print(f'exe not usable ({e}); retry {k + 1}/{tries} in 60 s', file=sys.stderr)
            time.sleep(60)
    print(f'skip: {name} (no usable exe)')


def check_run():
    args = ['--quantity', 'sunhours', '--time', '06-21T12:00', '--lat', '42', '--lon', '-71', '--tz', '-5']
    slab = (QUAD * 20 + [-10, -10, 2], [4], [0, 1, 2, 3])           # 20 x 20 m, 2 m above the sensor quad
    v, m = run_meshes([(QUAD, [4], [0, 1, 2, 3])], [slab], args=args)
    assert v.shape == (1, 1) and v[0, 0] == 0 and [x['name'] for x in m['meshes']] == ['sensor_0'], (v, m)
    v, _ = run_meshes((QUAD, [4], [0, 1, 2, 3]), args=args)
    assert v[0, 0] == 1, v


def check_views_run():
    """views.exe on (name, points, counts, indices) meshes as usd.meshes() gives them: their file stems in --target."""
    lake = QUAD * 20 + [-10, -21, 0.05]                              # 20 x 20 m of water in front of the facade
    v, m = run_meshes([('/site/facade', FACADE, [4], [0, 1, 2, 3])], [('/site/lake', lake, [4], [0, 1, 2, 3])],
                      args=['--target', 'site_lake=water', '--jitter', 'off'], tool='views')
    assert m['columns'][:5] == ['sky_front', 'ground_front', 'obstruction_front', 'obstruction_total_front', 'water_front'], m['columns']
    assert [x['name'] for x in m['meshes']] == ['site_facade'], m['meshes']
    assert m['targets'] == [{'mesh': 'site_lake', 'group': 'water'}], m['targets']
    assert v.shape == (1, 7) and v[0, 4] > 0 and v[0, 2] == 0 and abs(v[0, [0, 1, 2, 4]].sum() - 1) < 1e-9 and abs(v[0, 3] - (1 - v[0, 0])) < 1e-9, v   # column 3: all but the sky


def fails(status, msg, fn, *args):
    """fn(*args) raises DaylightError with this status and `msg` in its message."""
    try:
        fn(*args)
    except DaylightError as e:
        assert e.status == status and msg in str(e), (e.status, str(e), status, msg)
        return
    raise AssertionError(f'no DaylightError ({msg})')


def same_as_cli(values, manifest, name):
    """values and manifest text equal ctest's build/<name> (the .npy data after its header) and <name>.json (read as
    text: CRLF -> LF) byte for byte; skipped when ctest has not written them."""
    npy, man = BUILD / name, BUILD / f'{name}.json'
    if not (npy.exists() and man.exists()):
        return print(f'skip: {npy} (+ .json) missing, ctest writes it')
    b = npy.read_bytes()
    assert np.load(npy).shape == values.shape and values.tobytes() == b[10 + int.from_bytes(b[8:10], 'little'):], name
    assert manifest == man.read_text(encoding='utf-8'), name


def check_native():
    """daylight_c.dll in-process, tests/c_abi.c's cases: version, usage, errors (status + message), both tools on
    tests/box.obj against the CLI's smokes, the resident scene, a closed handle's scene, the views facade, cancel."""
    with mock.patch.dict(os.environ, DAYLIGHT_DLL=str(TESTS / 'box.obj')):     # not a DLL: an OSError naming it
        try:
            _native.lib()
            raise AssertionError('box.obj loaded')
        except OSError as e:
            assert type(e) is OSError and 'box.obj' in str(e), e
    e = pickle.loads(pickle.dumps(DaylightError(2, 'cancelled')))     # crosses process pools with status and message
    assert e.status == 2 and str(e) == 'cancelled', e
    try:
        dll = _native.lib()
    except FileNotFoundError as e:
        return print(f'skip: native ({e})')
    assert dll.dl_version() == _native.ABI_VERSION == 1
    assert usage('daylight').startswith('usage: daylight') and usage('views').startswith('usage: views')
    try:
        usage('nosuch')
        raise AssertionError('unknown tool accepted')
    except ValueError:
        pass

    facade = SimulationMesh('facade', FACADE, [4], [0, 1, 2, 3])
    assert facade.names == ('facade',) and SimulationMesh('empty', np.empty((0, 3)), [], []).names == ()
    fails(1, 'bad: vertex index out of range', SimulationMesh, 'bad', FACADE, [4], [0, 1, 2, 4])
    fails(1, 'fewer than 3 vertices', SimulationMesh, 'two', FACADE, [2], [0, 1])
    fails(2, 'cannot open', SimulationMesh.load, TESTS / 'nosuch.obj')
    fails(2, 'unknown extension', SimulationMesh.load, 'x.usd')
    try:
        SimulationMesh('slots', FACADE, [4], [0, 1, 2, 3], slots=[0, 0])
        raise AssertionError('2 slots for 1 face accepted')
    except ValueError:
        pass
    fails(1, 'no mesh given', run, 'views', [], [])
    for tool, status, msg, args in (
            ('daylight', 1, 'unknown flag --bogus', ['--bogus']), ('views', 1, 'dl_mesh_load', ['x.obj']),
            ('views', 1, '--context', ['--context', 'c.obj']), ('daylight', 1, '--out', ['--out', 'v.npy']),
            ('views', 1, '--bench', ['--bench']), ('daylight', 1, 'dl_usage', ['-h']),
            ('daylight', 1, 'nosuch', ['--optics', 'nosuch=0.5']), ('views', 1, 'nosuch', ['--target', 'nosuch=g']),
            ('daylight', 2, 'nosuch.epw', ['--epw', TESTS / 'nosuch.epw'])):
        fails(status, msg, run, tool, args, facade)
    plain = _run('views', OPEN + ['--offset', '1.6'], facade, (), None)
    locale.setlocale(locale.LC_NUMERIC, 'German_Germany.1252')        # a ',' would make the core read --offset 1.6 as 1
    try:                                                              # the DLL runs in the "C" locale: the same bytes
        comma = _run('views', OPEN + ['--offset', '1.6'], facade, (), None)
        assert locale.localeconv()['decimal_point'] == ','           # and the host's locale is back
    finally:
        locale.setlocale(locale.LC_NUMERIC, 'C')
    assert np.array_equal(plain[0], comma[0]) and plain[1] == comma[1], 'a decimal-comma locale changed the run'

    # the GPU: tests/box.obj (a ground of 4 quads, a floating box) as ctest's cli_smoke_npy / views_smoke_npy run it
    box, ctl = SimulationMesh.load(TESTS / 'box.obj'), Control()
    assert box.names == ('ground', 'box'), box.names
    try:
        v1, m1, s1 = _run('daylight', SUN, box, (), ctl)
    except DaylightError as e:
        if any(k in str(e) for k in ('CUDA', 'OptiX', 'optix')):
            return print(f'skip: native runs (no usable GPU: {e})')
        raise
    same_as_cli(v1, m1, 'smoke.npy')
    assert not s1['scene_reused'] and ctl.progress == 1.0, (s1, ctl.progress)
    r2 = run('daylight', SUN, box)                                    # the same handle: the resident scene serves it
    assert r2.stats['scene_reused'] and np.array_equal(r2.values, v1) and r2.manifest == json.loads(m1), r2.stats
    v3, m3, s3 = _run('views', ['--fov', '360', '--offset', '1.6', '--sides', 'both'], box, (), None)
    assert s3['scene_reused'], s3                                     # an opaque scene is views' too
    same_as_cli(v3, m3, 'views_smoke.npy')
    box.close()
    with SimulationMesh.load(TESTS / 'box.obj') as box2:                        # a closed handle's scene is never reused
        r4 = run('daylight', SUN, [box2])
        assert not r4.stats['scene_reused'] and np.array_equal(r4.values, v1), r4.stats

    # the views facade (check_views case 1): sky above the field's horizon, the virtual ground below, no obstruction
    v, m = run('views', OPEN, facade)[:2]
    assert m['columns'] == ['sky_front', 'ground_front', 'obstruction_front', 'obstruction_total_front', 'distance_front', 'distance_min_front'], m
    assert v.shape == (1, 6) and abs(v[0, 0] - 0.4490987851112869) < 1e-9 and abs(v[0, 1] - 0.5509012148887131) < 1e-9, v
    assert v[0, 2] == 0 and abs(v[0, 3] - v[0, 1]) < 1e-12, v   # obstruction_total: the ground alone here

    # a Path flag goes in the narrow (ANSI) encoding the core opens files with, as the CLI's argv; echoed back decoded
    if '\u00fc'.encode(_native.NARROW, 'replace') == b'?':
        print('skip: a non-ASCII path flag (no u-umlaut in the ANSI code page)')
    else:
        with tempfile.TemporaryDirectory() as t:
            d = Path(t) / 'J\u00fcrgen'
            d.mkdir()
            run('daylight', SUN + ['--dump-sun', d / 'sun.txt'], facade)
            assert (d / 'sun.txt').exists(), list(Path(t).iterdir())
            fails(2, f'{d}', run, 'daylight', SUN + ['--dump-sun', d / 'no' / 'sun.txt'], facade)

    # cancel: a cancelled control fails the run as it starts; the next run works
    with Control() as stop:
        stop.cancel()
        fails(2, 'cancelled', run, 'views', OPEN, facade, (), stop)
    assert run('views', OPEN, facade).values.shape == (1, 6)
    facade.close()
    fails(1, 'NULL mesh handle', run, 'views', OPEN, facade)
    ctl.close()
    print('native OK')


def check_colormap():
    rgb = colormap([0, 0.3, 1, np.nan])                                 # default range = finite min/max = 0..1
    assert np.allclose(rgb[[0, 2]], VIRIDIS[[0, 8]]) and (rgb[3] == 0.5).all(), rgb
    assert np.allclose(rgb[1], [0.206931, 0.372933, 0.550578], atol=1e-5), rgb[1]
    assert np.array_equal(colormap([2.0, 2.0]), VIRIDIS[[0, 0]]) and colormap([]).shape == (0, 3)
    assert np.allclose(colormap([5], vmin=0, vmax=10), colormap([0, 0.5, 1])[1])


def check_write_results():
    with tempfile.TemporaryDirectory() as d:
        st = Usd.Stage.CreateNew(f'{d}/src.usda')
        m = UsdGeom.Mesh.Define(st, '/m')                                 # 4 points, 3 faces, face 1 a hole
        m.CreatePointsAttr(Vt.Vec3fArray.FromNumpy(np.arange(12, dtype=np.float32).reshape(4, 3)))
        m.CreateFaceVertexCountsAttr(Vt.IntArray([3, 3, 3]))
        m.CreateFaceVertexIndicesAttr(Vt.IntArray([0, 1, 2, 1, 2, 3, 0, 2, 3]))
        m.CreateHoleIndicesAttr(Vt.IntArray([1]))
        UsdGeom.Mesh.Define(st, '/proto/mesh')
        inst = st.DefinePrim('/inst')
        inst.GetReferences().AddInternalReference('/proto')
        inst.SetInstanceable(True)
        st.Save()
        out, man = f'{d}/sub/daylight.usda', {'columns': ['a', 'b'], 'meshes': [{'first': 0, 'count': 2}]}
        assert write_results(st, [[1, 10], [3, 30]], man, ['/m'], out) == ['/m']
        so = Usd.Stage.Open(out)
        assert Sdf.Layer.Find(out).subLayerPaths == ['../src.usda'] and len(UsdGeom.Mesh(so.GetPrimAtPath('/m')).GetPointsAttr().Get()) == 4
        pv = UsdGeom.PrimvarsAPI(so.GetPrimAtPath('/m'))
        a, b, col = (np.array(pv.GetPrimvar(k).Get()) for k in ('daylight_a', 'daylight_b', 'displayColor'))
        assert a.shape == b.shape == (3,) and a[[0, 2]].tolist() == [1, 3] and b[2] == 30 and np.isnan(a[1]) and np.isnan(b[1]), (a, b)
        assert pv.GetPrimvar('daylight_a').GetInterpolation() == 'uniform' and pv.GetPrimvar('displayColor').GetInterpolation() == 'uniform'
        assert col.shape == (3, 3) and np.allclose(col[1], 0.5) and np.allclose(col[[0, 2]], VIRIDIS[[0, 8]], atol=1e-6), col
        assert write_results(str(st.GetRootLayer().realPath), np.arange(4.0), {'columns': ['v'], 'meshes': [{'first': 0, 'count': 4}]},
                             ['/m'], out, on='vertices') == ['/m']
        pv = UsdGeom.PrimvarsAPI(so.GetPrimAtPath('/m')).GetPrimvar('daylight_v')     # `so` follows the rewritten layer
        assert len(pv.Get()) == 4 and pv.GetInterpolation() == 'vertex'
        try:
            write_results(st, np.zeros(5), {'columns': ['v'], 'meshes': [{'first': 0, 'count': 5}]}, ['/m'], out)
            raise AssertionError('mismatch accepted')
        except ValueError as e:
            assert '/m' in str(e), e
        assert st.GetPrimAtPath('/inst/mesh').IsInstanceProxy()
        man = {'columns': ['v'], 'meshes': [{'first': 0, 'count': 1}, {'first': 1, 'count': 1}, {'first': 2, 'count': 1}]}
        assert write_results(st, np.zeros(3), man, ['/inst/mesh', '/pi[0]/proto/mesh', '/nope'], out) == []


def main():
    check_dmesh()
    check_colormap()
    check_write_results()
    check_masking()
    if not FIX.exists():
        print(f'skip: {FIX} missing (run validate/fixtures.py)')
    else:
        ms = {name: (p, c, i) for name, p, c, i in meshes(str(FIX))}
        assert {k: len(v[1]) for k, v in ms.items()} == {'/World/Group/Sub/sensors_flat': 100, '/World/context/box_a': 5,
                                                         '/World/context/box_b_mirrored/mesh': 5}, list(ms)
        p, c, i = ms['/World/Group/Sub/sensors_flat']         # 10x10 grid from (-5,-5,0), authored leftHanded under rotations
        assert np.allclose(p[0], (-5, -5, 0), atol=1e-5) and np.allclose(p[120], (5, 5, 0), atol=1e-5), (p[0], p[120])
        assert c.tolist() == [4] * 100 and i[:4].tolist() == [0, 1, 12, 11], i[:4]   # flipped back to the right-handed +Z winding
        p = ms['/World/context/box_a'][0]                     # box_mesh(0, 6, 4, 4, 6): vertex 0 = (x0, y0, 0)
        assert np.allclose(p[0], (-2, 4, 0), atol=1e-5), p[0]
        p, _, i = ms['/World/context/box_b_mirrored/mesh']    # box_mesh(-8, 0, 4, 4, 6) through a scale(-1,1,1) xform
        assert np.allclose(p[0], (-10, -2, 0), atol=1e-5) and np.allclose(p[6], (-6, 2, 6), atol=1e-5), (p[0], p[6])
        assert i[:4].tolist() == [4, 5, 6, 7], i[:4]        # mirror flip undone: top face winds +Z (outward)
    check_exe('run_meshes', check_run)
    check_exe('run_meshes views', check_views_run)
    check_native()
    print('check OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
