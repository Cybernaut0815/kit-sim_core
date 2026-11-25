# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""daylight and views in-process from Python: daylight.native over daylight_c.dll (guide: docs/python.md).

The steps:
  1. the meshes: a south facade and a block made with numpy, the lawn and box of tests/box.obj read from a file
  2. daylight: a clear-sky year in Berlin on a worker thread, its progress read through a Control
  3. reading a result: values, columns and each mesh's rows (per_mesh)
  4. views: 120 deg about each face's normal; this run reuses the GPU scene of step 2
  5. isovists: the volume and the area a standing eye sees in the room of tests/room.obj

Needs the core's build (daylight_c.dll) and uv. Run from bindings/python:
    cd bindings/python && uv run python ../../examples/python/inprocess.py
"""
import time
from concurrent.futures import ThreadPoolExecutor, wait
from pathlib import Path

import numpy as np
from daylight import DaylightOptions, SimulationMesh, ViewsOptions, native

TESTS = Path(__file__).resolve().parents[2] / 'tests'   # box.obj, room.obj, room_grid.obj

# The CLIs' flags (docs/daylight.md, docs/views.md) as typed dicts, a key per flag (docs/api.md#typed-options);
# native.run takes the flags as strings too (['--clear-sky', '--lat', '52.52', ...]).
DAYLIGHT: DaylightOptions = {
    'clear_sky': True,                       # no weather file: a cloudless year for the place
    'lat': 52.52, 'lon': 13.41,              # Berlin
    'quantity': ['radiation', 'sunhours'],   # the columns: kWh/m2 and sun hours
}
VIEWS: ViewsOptions = {
    'fov': 120,                              # 120 deg wide about each face's normal (the default, written out)
    'target': {'ground': 'lawn'},            # rays ending on the mesh "ground" count for the group "lawn"
    'reach': 'en17037',                      # reach_6, reach_20, reach_50: the shares that see at least 6, 20, 50 m
}


def wall(width, height, y, cell):
    """A wall in the plane y facing south (-Y), centred on x = 0, its foot at z = 0, made of cell x cell quads.

    Returns the three arrays a SimulationMesh takes:
      points   [N, 3] float, x y z per point (metres, Z up)
      counts   vertices per face (4: quads)
      indices  each face's points, 0-based, counter-clockwise seen from the front (the side that is analysed)
    """
    nx, nz = round(width / cell), round(height / cell)                  # quads across and up
    x, z = np.meshgrid(np.linspace(-width / 2, width / 2, nx + 1), np.linspace(0, height, nz + 1))
    points = np.column_stack([x.ravel(), np.full(x.size, y), z.ravel()])  # the grid's corners, row by row from the foot
    lower_left = (np.arange(nz)[:, None] * (nx + 1) + np.arange(nx)).ravel()   # each quad's lower left corner
    quads = np.column_stack([lower_left, lower_left + 1, lower_left + nx + 2, lower_left + nx + 1])
    return points, np.full(nx * nz, 4), quads.ravel()


def per_mesh(result):
    """(name, faces, {column: mean}) for each sensor mesh.

    result.values holds a row per face of every sensor mesh, [rows x columns]. The manifest names the columns and gives
    each mesh's rows: first .. first + count."""
    for m in result.manifest['meshes']:
        rows = result.values[m['first']:m['first'] + m['count']]
        yield m['name'], m['count'], dict(zip(result.manifest['columns'], rows.mean(axis=0)))


# ---- Step 1: the meshes ---------------------------------------------------------------------------------------
# A SimulationMesh holds meshes the core keeps (copied once); `with` frees them at the end. Sensor meshes are analysed
# (a row per face) and block rays, context meshes only block.
with SimulationMesh('facade', *wall(10, 6, 4, 1)) as facade, \
        SimulationMesh.load(TESTS / 'box.obj') as box, \
        SimulationMesh('block', *wall(30, 9, -14, 3)) as block:
    # facade: 10 x 6 m of 1 m quads at y = 4; box: the groups "ground" (four quads at z 0) and "box" (a 1 m box);
    # block: 30 x 9 m across the street at y = -14
    sensors, context = [facade, box], [block]              # facade, ground, box analysed; the block only blocks

    # ---- Step 2: daylight on a worker thread, progress through a Control ----------------------------------------
    # native.run blocks until the run is done. On a worker thread the main thread stays free (a UI redraws) and reads
    # the Control's progress meanwhile; ctl.cancel() would stop the run.
    t0 = time.perf_counter()
    with native.Control() as ctl, ThreadPoolExecutor(1) as pool:
        job = pool.submit(native.run, 'daylight', DAYLIGHT, sensors, context, ctl)
        seen = [0.0]
        while wait([job], timeout=0.02).not_done:          # every 20 ms until the run is done
            seen.append(ctl.progress)
        r = job.result()                                   # a failed run raises native.DaylightError (.status 1 or 2)
        seen.append(ctl.progress)                          # 1 once the run succeeded
    progress = ' -> '.join(f'{p:.0%}' for p in dict.fromkeys(seen))
    print(f"daylight, clear-sky year: scene_reused {r.stats['scene_reused']}, "
          f"{1000 * (time.perf_counter() - t0):.0f} ms, progress {progress}")

    # ---- Step 3: reading the result -------------------------------------------------------------------------------
    for name, n, a in per_mesh(r):
        print(f"  {name:6} {n:2} faces  {a['radiation_front']:4.0f} kWh/m2  {a['sunhours_front']:4.0f} sun hours")

    # ---- Step 4: views about each face's normal -------------------------------------------------------------------
    # The same meshes in the same roles and order as step 2: the GPU scene daylight built is reused (scene_reused).
    # distance: the log mean of the rays' distances, an open ray counting as 1000 m.
    t0 = time.perf_counter()
    v = native.run('views', VIEWS, sensors, context)
    print(f"views, 120 deg about each normal: scene_reused {v.stats['scene_reused']}, "
          f"{1000 * (time.perf_counter() - t0):.0f} ms")
    for name, n, a in per_mesh(v):
        print(f"  {name:6} {n:2} faces  sky {a['sky_front']:.2f}  lawn {a['lawn_front']:.2f}  distance "
              f"{a['distance_front']:3.0f} m  sees 6 / 20 / 50 m: {a['reach_6_front']:.2f} {a['reach_20_front']:.2f} "
              f"{a['reach_50_front']:.2f}")

# ---- Step 5: isovists in a room -----------------------------------------------------------------------------------
# tests/room.obj: a 6.3 x 5.1 x 2.9 m room with a window and a tree outside; tests/room_grid.obj: 12 measuring points
# at desk height. The isovist is what an eye sees around it: all around in 3D the volume in view (column volume_front,
# m3), with horizontal rays only the area in view (area_front, m2).
with SimulationMesh.load(TESTS / 'room_grid.obj') as grid, SimulationMesh.load(TESTS / 'room.obj') as room:
    eye: ViewsOptions = {
        'offset': 0.4,              # the eye 0.4 m above each grid face
        'no_occlude': ['grid'],     # the grid is measured, not seen: the rays pass it
    }
    # The isovist is the default column all around; 'distance': ['mean', 'min'] gives the distances instead, 'far' caps both.
    vol = native.run('views', {**eye, 'fov': (360, -90, 90)}, grid, context=room)   # the whole sphere
    area = native.run('views', {**eye, 'fov': (360, 0, 0)}, grid, context=room)     # horizontal rays only
    print(f"isovists in the room: volume {next(per_mesh(vol))[2]['volume_front']:.1f} m3 all around, "
          f"area {next(per_mesh(area))[2]['area_front']:.1f} m2 at eye height")
