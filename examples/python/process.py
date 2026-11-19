# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""daylight.exe and views.exe as processes (daylight.run_meshes): named numpy meshes go to a temporary folder as
<name>.dmesh, the exe runs on them, its .npy values and .json manifest come back. A south facade, a lawn in front of
it, a block across the street: a clear-sky year and the view out, per 1 m row of the facade.

    cd bindings/python && uv run python ../../examples/python/process.py

A USD stage in and out, either tool: python -m daylight converts the stage (meshes, split), writes one .dmesh per mesh
prim and meshes.json to -o, runs the exe with the flags after -- (values.npy) and with --write-usd puts the values
back as a layer over the stage (write_results: primvars <tool>_<column> and displayColor). In box.usda /World/Box and
/World/BoxLeft are the sensors; /Floor (file stem Floor) only blocks:

    uv run python -m daylight ../cpp/box.usda --sensors /World -o out --write-usd out/daylight.usda -- --clear-sky --lat 52.52 --lon 13.41
    uv run python -m daylight ../cpp/box.usda --sensors /World -o out --tool views --write-usd out/views.usda -- --target Floor=lawn
"""
import daylight
import numpy as np

DAYLIGHT = ['--clear-sky', '--lat', '52.52', '--lon', '13.41', '--quantity', 'radiation,sunhours']     # Berlin
VIEWS = ['--fov', '120', '--target', 'lawn=green', '--reach', 'en17037']       # mesh "lawn" counts as group "green"


def wall(width, height, y, cell):
    """A wall at y facing south (-Y), centred on x 0, foot at z 0, in cell x cell quads from the bottom row up:
    points [N, 3] (metres, Z up), counts, 0-based indices (right-handed winding: counter-clockwise from the front)."""
    nx, nz = round(width / cell), round(height / cell)
    x, z = np.meshgrid(np.linspace(-width / 2, width / 2, nx + 1), np.linspace(0, height, nz + 1))
    P = np.column_stack([x.ravel(), np.full(x.size, y), z.ravel()])
    k = (np.arange(nz)[:, None] * (nx + 1) + np.arange(nx)).ravel()          # each quad's lower left point
    return P, np.full(nx * nz, 4), np.column_stack([k, k + 1, k + nx + 2, k + nx + 1]).ravel()


def facade_rows(values, manifest):
    """{column: 6 means}: the facade's rows (its manifest range) averaged per 1 m row of 10 quads, bottom first."""
    m = next(m for m in manifest['meshes'] if m['name'] == 'facade')        # the name = the .dmesh file stem
    rows = values[m['first']:m['first'] + m['count']]
    return dict(zip(manifest['columns'], rows.reshape(6, 10, -1).mean(axis=1).T))


facade = ('facade', *wall(10, 6, 4, 1))                                     # (name, points, counts, indices)
context = [('block', *wall(30, 9, -14, 3)),                                 # blocks only
           ('lawn', [[-5, -2, 0], [5, -2, 0], [5, 2, 0], [-5, 2, 0]], [4], [0, 1, 2, 3])]    # 10 x 4 m, faces up
d = facade_rows(*daylight.run_meshes([facade], context, args=DAYLIGHT))                  # daylight.exe
v = facade_rows(*daylight.run_meshes([facade], context, args=VIEWS, tool='views'))       # views.exe
print('facade row kWh/m2  sun hours   sky  green  distance  sees 20 m')
for k in range(6):
    print(f'z {k}-{k + 1} m    {d["radiation_front"][k]:6.0f}  {d["sunhours_front"][k]:9.0f}  '
          f'{v["sky_front"][k]:4.2f}  {v["green_front"][k]:5.2f}  {v["distance_front"][k]:6.1f} m  '
          f'{v["reach_20_front"][k]:9.2f}')
