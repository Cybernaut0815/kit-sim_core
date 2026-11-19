# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""daylight and views in-process (daylight.native over daylight_c.dll): a south facade made with numpy, the lawn and box
of tests/box.obj in front of it, a block across the street. A clear-sky year of daylight on a worker thread (its
progress read through a Control), then the view out of the same meshes, which reuses the GPU scene.

    cd bindings/python && uv run python ../../examples/python/inprocess.py
"""
import time
from concurrent.futures import ThreadPoolExecutor, wait
from pathlib import Path

import numpy as np
from daylight import Mesh, native

BOX = Path(__file__).resolve().parents[2] / 'tests' / 'box.obj'      # groups "ground" (4 quads at z 0) and "box"
DAYLIGHT = ['--clear-sky', '--lat', '52.52', '--lon', '13.41', '--quantity', 'radiation,sunhours']     # Berlin
VIEWS = ['--fov', '120', '--target', 'ground=lawn', '--reach', 'en17037']     # mesh "ground" counts as group "lawn"


def wall(width, height, y, cell):
    """A wall at y facing south (-Y), centred on x 0, foot at z 0, in cell x cell quads from the bottom row up:
    points [N, 3] (metres, Z up), counts, 0-based indices (right-handed winding: counter-clockwise from the front)."""
    nx, nz = round(width / cell), round(height / cell)
    x, z = np.meshgrid(np.linspace(-width / 2, width / 2, nx + 1), np.linspace(0, height, nz + 1))
    P = np.column_stack([x.ravel(), np.full(x.size, y), z.ravel()])
    k = (np.arange(nz)[:, None] * (nx + 1) + np.arange(nx)).ravel()          # each quad's lower left point
    return P, np.full(nx * nz, 4), np.column_stack([k, k + 1, k + nx + 2, k + nx + 1]).ravel()


def per_mesh(r):
    """(name, faces, {column: mean}) per sensor mesh: its rows are first .. first + count of the manifest's range."""
    for m in r.manifest['meshes']:
        rows = r.values[m['first']:m['first'] + m['count']]
        yield m['name'], m['count'], dict(zip(r.manifest['columns'], rows.mean(axis=0)))


with Mesh('facade', *wall(10, 6, 4, 1)) as facade, Mesh.load(BOX) as box, Mesh('block', *wall(30, 9, -14, 3)) as block:
    sensors, context = [facade, box], [block]                  # facade, ground, box analysed; the block only blocks
    t0 = time.perf_counter()
    with native.Control() as ctl, ThreadPoolExecutor(1) as pool:
        job = pool.submit(native.run, 'daylight', DAYLIGHT, sensors, context, ctl)    # ctl.cancel() would stop it
        seen = [0.0]
        while wait([job], timeout=0.02).not_done:     # this thread stays free while the GPU works (a UI redraws)
            seen.append(ctl.progress)
        r = job.result()                              # a failed run raises native.DaylightError (.status 1 or 2)
        seen.append(ctl.progress)                     # 1 once the run succeeded
    progress = ' -> '.join(f'{p:.0%}' for p in dict.fromkeys(seen))
    print(f"daylight, clear-sky year: scene_reused {r.stats['scene_reused']}, "
          f"{1000 * (time.perf_counter() - t0):.0f} ms, progress {progress}")
    for name, n, a in per_mesh(r):
        print(f"  {name:6} {n:2} faces  {a['radiation_front']:4.0f} kWh/m2  {a['sunhours_front']:4.0f} sun hours")

    t0 = time.perf_counter()
    v = native.run('views', VIEWS, sensors, context)           # the same meshes in the same roles: the same scene
    print(f"views, 120 deg about each normal: scene_reused {v.stats['scene_reused']}, "
          f"{1000 * (time.perf_counter() - t0):.0f} ms")
    for name, n, a in per_mesh(v):
        print(f"  {name:6} {n:2} faces  sky {a['sky_front']:.2f}  lawn {a['lawn_front']:.2f}  distance "
              f"{a['distance_front']:3.0f} m  sees 6 / 20 / 50 m: {a['reach_6_front']:.2f} {a['reach_20_front']:.2f} "
              f"{a['reach_50_front']:.2f}")
