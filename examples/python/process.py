# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""daylight.exe and views.exe as processes from Python: daylight.run_meshes (guide: docs/python.md).

run_meshes writes each named numpy mesh to a temporary folder as <name>.dmesh, runs the exe on them and reads its .npy
values and .json manifest back. No DLL in the process: the exes do the work, each call starts one (and the GPU) anew.

The steps:
  1. the meshes: a south facade, a lawn in front of it and a block across the street, as (name, points, counts,
     indices) tuples
  2. daylight.exe: a clear-sky year
  3. views.exe: the view out, the lawn as a target group
  4. reading the results: the facade's rows, averaged per 1 m row of quads

Needs the core's build (daylight.exe, views.exe) and uv. Run from bindings/python:
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

# The flags are the CLIs' (docs/daylight.md, docs/views.md), one string each.
DAYLIGHT = [
    '--clear-sky',                         # no weather file: a cloudless year for the place
    '--lat', '52.52', '--lon', '13.41',    # Berlin
    '--quantity', 'radiation,sunhours',    # the columns: kWh/m2 and sun hours
]
VIEWS = [
    '--fov', '120',                        # 120 deg wide about each face's normal (the default, written out)
    '--target', 'lawn=green',              # rays ending on the mesh "lawn" (its .dmesh file stem) count for "green"
    '--reach', 'en17037',                  # reach_6, reach_20, reach_50: the shares that see at least 6, 20, 50 m
]


def wall(width, height, y, cell):
    """A wall in the plane y facing south (-Y), centred on x = 0, its foot at z = 0, made of cell x cell quads.

    Returns points [N, 3] (metres, Z up), counts (4 per quad) and 0-based indices, counter-clockwise seen from the
    front (the side that is analysed), quads row by row from the foot."""
    nx, nz = round(width / cell), round(height / cell)                  # quads across and up
    x, z = np.meshgrid(np.linspace(-width / 2, width / 2, nx + 1), np.linspace(0, height, nz + 1))
    points = np.column_stack([x.ravel(), np.full(x.size, y), z.ravel()])
    lower_left = (np.arange(nz)[:, None] * (nx + 1) + np.arange(nx)).ravel()   # each quad's lower left corner
    quads = np.column_stack([lower_left, lower_left + 1, lower_left + nx + 2, lower_left + nx + 1])
    return points, np.full(nx * nz, 4), quads.ravel()


def facade_rows(values, manifest):
    """{column: 6 means}: the facade's rows averaged per 1 m row of 10 quads, bottom first.

    values [rows x columns] holds a row per face of every sensor mesh; the manifest names the columns and gives each
    mesh's rows (first, count) under its name, which is the .dmesh file stem run_meshes wrote."""
    m = next(m for m in manifest['meshes'] if m['name'] == 'facade')
    rows = values[m['first']:m['first'] + m['count']]                   # 60 quads: 6 rows of 10
    return dict(zip(manifest['columns'], rows.reshape(6, 10, -1).mean(axis=1).T))


# ---- Step 1: the meshes -----------------------------------------------------------------------------------------
facade = ('facade', *wall(10, 6, 4, 1))                                 # 10 x 6 m of 1 m quads at y = 4: a sensor
context = [                                                             # context meshes: they only block rays
    ('block', *wall(30, 9, -14, 3)),                                    # 30 x 9 m across the street
    ('lawn', [[-5, -2, 0], [5, -2, 0], [5, 2, 0], [-5, 2, 0]], [4], [0, 1, 2, 3]),   # 10 x 4 m on the ground, facing up
]

# ---- Step 2: daylight.exe ---------------------------------------------------------------------------------------
# run_meshes(sensors, context, args=flags) -> (values, manifest)
d = facade_rows(*daylight.run_meshes([facade], context, args=DAYLIGHT))

# ---- Step 3: views.exe ------------------------------------------------------------------------------------------
v = facade_rows(*daylight.run_meshes([facade], context, args=VIEWS, tool='views'))

# ---- Step 4: the results, one line per 1 m row of the facade --------------------------------------------------------
# distance: the log mean of the rays' distances, an open ray counting as 1000 m
print('facade row kWh/m2  sun hours   sky  green  distance  sees 20 m')
for k in range(6):
    print(f'z {k}-{k + 1} m    {d["radiation_front"][k]:6.0f}  {d["sunhours_front"][k]:9.0f}  '
          f'{v["sky_front"][k]:4.2f}  {v["green_front"][k]:5.2f}  {v["distance_front"][k]:6.1f} m  '
          f'{v["reach_20_front"][k]:9.2f}')
