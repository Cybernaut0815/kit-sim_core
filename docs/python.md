# Python

`bindings/python` (package `daylight`, a uv project) runs both tools of the core from Python: daylight, annual
daylight / solar simulation ([daylight.md](daylight.md)), and views, view analysis ([views.md](views.md)). Both take
the same meshes, mesh roles and CLI flags, and both return a float64 `[N x C]` array (a row per element of the sensor
meshes, a column per quantity or share) with its manifest.

| way in | how | for |
|---|---|---|
| in-process: `daylight.native` | `daylight_c.dll` through ctypes: `SimulationMesh` handles, `native.run(tool, flags, sensors, context)` | repeated runs over one model (parameter studies, notebooks, a server): no process start, the GPU scene kept |
| process runner: `daylight.run_meshes`, `daylight.run` | writes `.dmesh` files, runs `daylight.exe` / `views.exe`, reads its `.npy` values and `.json` manifest | single runs, a process of its own per run |
| USD: `meshes`, `split`, `write_results`, `python -m daylight` | a composed stage to mesh arrays; the values back as primvars on a layer over the stage | usdview, Omniverse, any USD pipeline: [the one-liner](#a-usd-stage-in-and-out), [in code](#on-a-usd-stage) |

For the same meshes and flags both runners give the same values and manifest, bit for bit: the DLL runs the CLIs'
own code.

## Setup

```
cmake --preset vs2022 && cmake --build --preset release   # daylight.exe, views.exe, daylight_c.dll
cd bindings/python && uv sync                             # .venv: numpy, usd-core, the daylight package (editable)
uv run python -m daylight.check                           # self-checks: converter, write_results, both runners
```

Needs Windows x64, an NVIDIA GPU of compute capability 7.5 or newer with driver R570+ and the VC++ 2015-2022 x64
runtime ([README](../README.md#build)); Python >= 3.10 (`uv sync` takes 3.12 from `.python-version`), `numpy` and
`usd-core` 26.8 (`import daylight` imports `pxr`).

| file | used by | looked up |
|---|---|---|
| `daylight_c.dll` | `daylight.native`, loaded on first use | `DAYLIGHT_DLL`, else `daylight_c.dll` in the package folder (`bindings/python/daylight/`), else `<repo>/build/RelWithDebInfo/` |
| `daylight.exe`, `views.exe` | `run`, `run_meshes`, `python -m daylight` | `DAYLIGHT_EXE`, `VIEWS_EXE`, else `<repo>/build/RelWithDebInfo/` |

`<repo>` is three folders above the package's modules: right for the editable install `uv sync` makes; a copy of the
package installed elsewhere needs the variables (or the DLL in its folder). A python.org Python lacks `msvcp140.dll`:
install the VC++ redistributable or copy its DLLs beside `daylight_c.dll` ([c-abi.md](c-abi.md#loading-the-dll)). A
missing DLL raises `FileNotFoundError`, one of another C ABI version `OSError`.

## Daylight

`examples/python/inprocess.py`: a 10 x 6 m facade at y = 4 m facing south (-Y), 60 quads of 1 m made with numpy;
in front of it `tests/box.obj` (group `ground`: four 2 m quads at z 0; group `box`: a 1 m box floating above one of
them); across the street, 18 m south, a 30 x 9 m wall that only blocks. Berlin, a clear-sky year (no weather file),
radiation and sun hours.

```python
import time
from concurrent.futures import ThreadPoolExecutor, wait
from pathlib import Path

import numpy as np
from daylight import SimulationMesh, native

BOX = Path(__file__).resolve().parents[2] / 'tests' / 'box.obj'      # groups "ground" (4 quads at z 0) and "box"
DAYLIGHT = ['--clear-sky', '--lat', '52.52', '--lon', '13.41', '--quantity', 'radiation,sunhours']     # Berlin
```

A mesh from arrays: points `[N, 3]` (world metres, Z up), the vertex count of each face (>= 3) and the 0-based
indices, faces wound counter-clockwise seen from the side they face (right-handed):

```python
def wall(width, height, y, cell):
    """A wall at y facing south (-Y), centred on x 0, foot at z 0, in cell x cell quads from the bottom row up:
    points [N, 3] (metres, Z up), counts, 0-based indices (right-handed winding: counter-clockwise from the front)."""
    nx, nz = round(width / cell), round(height / cell)
    x, z = np.meshgrid(np.linspace(-width / 2, width / 2, nx + 1), np.linspace(0, height, nz + 1))
    P = np.column_stack([x.ravel(), np.full(x.size, y), z.ravel()])
    k = (np.arange(nz)[:, None] * (nx + 1) + np.arange(nx)).ravel()          # each quad's lower left point
    return P, np.full(nx * nz, 4), np.column_stack([k, k + 1, k + nx + 2, k + nx + 1]).ravel()
```

The run, on a worker thread so that this thread can read its progress ([below](#errors-cancel-progress)); on the
calling thread it is `r = native.run('daylight', DAYLIGHT, sensors, context)`:

```python
with SimulationMesh('facade', *wall(10, 6, 4, 1)) as facade, SimulationMesh.load(BOX) as box, SimulationMesh('block', *wall(30, 9, -14, 3)) as block:
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
```

- `SimulationMesh(name, points, counts, indices, slots=None)`: one mesh, numpy arrays or lists, copied into the DLL once.
  `SimulationMesh.load(path)`: every `o`/`g` group of an `.obj` (here `ground` and `box`, in file order) or one `.dmesh`.
  `.names` lists what a handle holds; these are the names the manifest and flags such as `--target` use.
- `native.run(tool, args, sensors, context=(), control=None)` -> `Result(values, manifest, stats)`. Sensors are
  analysed (a row per face) and block, context only blocks; each a `SimulationMesh` or a list of them.
- `args` are the CLI's flags as strings ([daylight.md](daylight.md)). `--clear-sky --lat --lon` makes a cloudless
  year; with a weather file it is `--epw site.epw` (give a non-ASCII path as a `pathlib.Path`). Mesh paths,
  `--context`, `--out`, `--bench` and `-h` are refused: the meshes are handles and the result comes back.
  `native.usage('daylight')` is the CLI's `-h` text.

```
daylight, clear-sky year: scene_reused False, 475 ms, progress 0% -> 100%
  facade 60 faces  1395 kWh/m2  3245 sun hours
  ground  4 faces  1147 kWh/m2  3058 sun hours
  box    12 faces   754 kWh/m2  1752 sun hours
```

Means over each mesh's faces. The facade gets 1395 kWh/m2 and 3245 hours of sun in a clear-sky year, the ground quads
1147, the box 754 (its mean holds the two bottom triangles, which face down). A clear-sky year is a cloudless upper
bound, typically 20-50 % above a typical weather year. The run took 0.5-0.8 s over several runs of the example, most
of it the GPU start of the process ([Speed](#speed)).

### Through `daylight.exe`

`examples/python/process.py` runs the scene through the CLIs (the same `wall` and `DAYLIGHT`; its `VIEWS` counts the
lawn as the group `green`, [below](#through-viewsexe-names-are-file-stems)), with a 10 x 4 m numpy quad `lawn` in place
of `tests/box.obj`. `run_meshes(sensors, context=(), **kw)` takes `(name, points, counts,
indices)` tuples, or `(points, counts, indices)` triples, writes each as a `.dmesh` file to a temporary folder, runs
the exe and returns `(values, manifest)`; `args`, `tool`, `epw` (`--epw`) and `out` (keep the value file) go to `run`:

```python
facade = ('facade', *wall(10, 6, 4, 1))                                     # (name, points, counts, indices)
context = [('block', *wall(30, 9, -14, 3)),                                 # blocks only
           ('lawn', [[-5, -2, 0], [5, -2, 0], [5, 2, 0], [-5, 2, 0]], [4], [0, 1, 2, 3])]    # 10 x 4 m, faces up
d = facade_rows(*daylight.run_meshes([facade], context, args=DAYLIGHT))                  # daylight.exe
v = facade_rows(*daylight.run_meshes([facade], context, args=VIEWS, tool='views'))       # views.exe
```

`facade_rows` averages the facade's rows per 1 m row of quads ([Results](#results)). The output, both tools:

```
facade row kWh/m2  sun hours   sky  green  distance  sees 20 m
z 0-1 m      1239       2999  0.30   0.06    14.9 m       0.37
z 1-2 m      1269       3073  0.31   0.15    28.3 m       0.39
z 2-3 m      1304       3157  0.33   0.17    39.2 m       0.41
z 3-4 m      1399       3266  0.34   0.17    49.3 m       0.44
z 4-5 m      1498       3441  0.36   0.15    59.7 m       0.47
z 5-6 m      1563       3578  0.38   0.13    70.3 m       0.50
```

The block shades the lower rows: 1239 kWh/m2 at the foot, 1563 at the top. `daylight.run(sensors, context=(),
epw=None, args=(), out=None, tool='daylight')` does the same for mesh files (`.obj`, `.dmesh`).

### A USD stage in and out

In `bindings/python`, the stage's meshes in, the values back as a layer over it:

```
uv run python -m daylight ../cpp/box.usda --sensors /World -o out --write-usd out/daylight.usda -- --clear-sky --lat 52.52 --lon 13.41
```

It converts the stage (`/World/Box` and `/World/BoxLeft` are sensors, `/Floor` only blocks), writes `out/<stem>.dmesh`
per mesh prim and `out/meshes.json`, runs `daylight.exe` with the flags after `--` (`out/values.npy` and `.json`) and
writes `out/daylight.usda`: a layer that sublayers the stage and gives each sensor prim `primvars:daylight_<column>`
(here `daylight_radiation_front`) and a viridis `displayColor`; open it in usdview. The same in code:
[On a USD stage](#on-a-usd-stage).

## Views

The same meshes in the same roles and order as the daylight run, so views reuses its GPU scene:

```python
VIEWS = ['--fov', '120', '--target', 'ground=lawn', '--reach', 'en17037']     # mesh "ground" counts as group "lawn"
```

```python
    t0 = time.perf_counter()
    v = native.run('views', VIEWS, sensors, context)           # the same meshes in the same roles: the same scene
    print(f"views, 120 deg about each normal: scene_reused {v.stats['scene_reused']}, "
          f"{1000 * (time.perf_counter() - t0):.0f} ms")
    for name, n, a in per_mesh(v):
        print(f"  {name:6} {n:2} faces  sky {a['sky_front']:.2f}  lawn {a['lawn_front']:.2f}  distance "
              f"{a['distance_front']:3.0f} m  sees 6 / 20 / 50 m: {a['reach_6_front']:.2f} {a['reach_20_front']:.2f} "
              f"{a['reach_50_front']:.2f}")
```

- `--fov 120` is the default field written out: 120 deg wide about each face's normal, 70 deg down to 50 deg up,
  a ray per 2 deg cell (3,600 per face). `--fov 360` turns all around, e.g. with `--offset 1.6` on a floor grid.
- `--target ground=lawn`: rays that end on the mesh `ground` count for the group `lawn`, a share column of its own.
- `--reach en17037`: the shares of the field that see at least 6, 20 and 50 m.
- Columns: `sky_front ground_front obstruction_front lawn_front distance_front distance_min_front reach_6_front
  reach_20_front reach_50_front`. Every flag: [views.md](views.md).

```
views, 120 deg about each normal: scene_reused True, 2 ms
  facade 60 faces  sky 0.34  lawn 0.06  distance  43 m  sees 6 / 20 / 50 m: 0.73 0.43 0.35
  ground  4 faces  sky 0.61  lawn 0.00  distance 205 m  sees 6 / 20 / 50 m: 0.69 0.61 0.61
  box    12 faces  sky 0.28  lawn 0.29  distance  51 m  sees 6 / 20 / 50 m: 0.37 0.29 0.28
```

Means over each mesh's faces. In the open, a vertical face's default field holds sin 50 / (sin 50 + sin 70) = 0.449
sky; the block across the street brings the facade down to 0.34. The lawn fills 6 % of the facade's field; the log
mean distance of its faces averages 43 m (open rays count as 1000 m); 73 % of the field sees at least 6 m, 43 % at
least 20 m, 35 % at least 50 m (the block is 18 m away). The ground quads look up (sky 0.61), the box looks down onto
the lawn (0.29). The run reused the scene: 2 ms.

All around, the default column is the isovist instead of the distances: `volume` (m³), or `area` (m²) with
horizontal rays only (`--fov 360,0,0`). The example ends with both, in tests/room.obj from its measure grid
([views.md](views.md#isovist)):

```python
with SimulationMesh.load(BOX.parent / 'room_grid.obj') as grid, SimulationMesh.load(BOX.parent / 'room.obj') as room:   # room, window, tree
    eye = ['--offset', '0.4', '--no-occlude', 'grid']                  # the grid is measured, not seen
    vol = native.run('views', ['--fov', '360,-90,90', *eye], grid, context=room)
    area = native.run('views', ['--fov', '360,0,0', *eye], grid, context=room)
```

```
isovists in the room: volume 92.8 m3 all around, area 32.1 m2 at eye height
```

The room holds 93.18 m³ on a 32.13 m² floor: the polygon and mesh through the rays' ends cut its corners a little.
`--distance mean,min` gives the distances instead, `--far M` caps both.

### Through `views.exe`: names are file stems

`tool='views'` runs `views.exe` on the same tuples (the `v = ...` line above), with:

```python
VIEWS = ['--fov', '120', '--target', 'lawn=green', '--reach', 'en17037']       # mesh "lawn" counts as group "green"
```

The tuples' names become the `.dmesh` file stems, and so the mesh names in the manifest and in flags: `--target
lawn=green` names the `lawn` tuple. A name is made a file stem as `python -m daylight` does: `[^A-Za-z0-9._-]` -> `_`,
leading `_` stripped, `_2`, `_3` ... for repeats (`/World/Lake` -> `World_Lake`); an unnamed triple is `sensor_<i>`
or `context_<i>`. In the table above, sky rises from 0.30 at the foot to 0.38 at the top, the green share peaks at
0.17 half way up, the log mean distance grows from 15 to 70 m (the near ground weighs less higher up) and the share
seeing at least 20 m from 0.37 to 0.50.

### A USD stage in and out: `--tool views`

```
uv run python -m daylight ../cpp/box.usda --sensors /World -o out --tool views --write-usd out/views.usda -- --target Floor=lawn
```

`--tool views` runs `views.exe`; the primvars are `views_<column>` (`views_sky_front`, `views_lawn_front`, ...).
`Floor` is the file stem of the context prim `/Floor`.

## Results

- `values`: float64 `[N x C]`, a row per element of the sensor meshes (faces, or points with `--on vertices`),
  meshes in the order given (a handle's meshes in its own order), elements in mesh order; nothing is reordered or
  dropped. In-process `Result(values, manifest, stats)`, from the process runner `(values, manifest)`: the same.
- `manifest['columns']`: the C column names. `manifest['meshes']`: `[{name, first, count}]` per sensor mesh, so
  `values[first:first + count]` is that mesh's face (or point) attribute:

```python
def per_mesh(r):
    """(name, faces, {column: mean}) per sensor mesh: its rows are first .. first + count of the manifest's range."""
    for m in r.manifest['meshes']:
        rows = r.values[m['first']:m['first'] + m['count']]
        yield m['name'], m['count'], dict(zip(r.manifest['columns'], rows.mean(axis=0)))
```

`process.py` does the same by name and then averages the facade per row of 10 quads:

```python
def facade_rows(values, manifest):
    """{column: 6 means}: the facade's rows (its manifest range) averaged per 1 m row of 10 quads, bottom first."""
    m = next(m for m in manifest['meshes'] if m['name'] == 'facade')        # the name = the .dmesh file stem
    rows = values[m['first']:m['first'] + m['count']]
    return dict(zip(manifest['columns'], rows.reshape(6, 10, -1).mean(axis=1).T))
```

| tool | columns | unit |
|---|---|---|
| daylight | `radiation_<side>`, `direct_<side>`, `diffuse_<side>` | kWh/m2, summed over the evaluated steps |
| daylight | `irradiance_<side>` | W/m2, mean over the evaluated hours |
| daylight | `illuminance_<side>` | lux, mean (physical mode) |
| daylight | `sunhours_<side>` | hours |
| views | `sky_<side>`, `ground_<side>`, `obstruction_<side>`, `<group>_<side>` | shares of the field's solid angle, 0..1, summing to 1 |
| views | `distance_<side>`, `distance_min_<side>`, `<group>_distance_<side>`, `<group>_distance_min_<side>` | metres, 0.1 to `--far` (1000); an open ray counts as `--far` |
| views | `reach_<D>_<side>` | share of the field that sees at least D metres |

`<side>` is `front`; `--sides both` gives `_front` and `_back` columns, `--sides back` only `_back`. daylight orders
its columns as `--quantity`, views as the shares, the distances, the reach ([views.md](views.md#output)). The
manifest also holds `duration_h` and `dirs` (daylight), the field `view` and `targets` (views); in-process `stats`
holds `scene_reused`, `prep_ms`, `sky_ms`, `build_ms`, `trace_ms`, `reduce_ms`, `bounce_ms`, `rays`, `launches`,
`gas_bytes`.

NaN: daylight gives none (a degenerate face gets 0). views gives NaN for `distance` and `distance_min` when no ray
of an element's field is traced (a degenerate face; `--fov 360` at `--offset 0` with the whole field behind the face)
and for a group's distances (`--distance group`) when the group is not in view: average such columns with
`np.nanmean`. `write_results` writes NaN (grey) for faces the USD converter dropped (holes, faces under 3 vertices).

### On a USD stage

In-process, from `bindings/python`, both tools, the values back on the stage:

```python
import daylight
from daylight import SimulationMesh, native

sens, ctx = daylight.split(daylight.meshes('../cpp/box.usda'), sensors=['/World'])   # [(prim path, P, C, I)]
sensors, context = [SimulationMesh(*m) for m in sens], [SimulationMesh(*m) for m in ctx]                # named by prim path
r = native.run('daylight', ['--clear-sky', '--lat', '52.52', '--lon', '13.41'], sensors, context)
v = native.run('views', ['--target', '/Floor=lawn'], sensors, context)
daylight.write_results('../cpp/box.usda', r.values, r.manifest, [m[0] for m in sens], 'out/daylight.usda')
daylight.write_results('../cpp/box.usda', v.values, v.manifest, [m[0] for m in sens], 'out/views.usda', name='views')
```

- `meshes(stage, time=None)`: every mesh prim of the composed stage as `(prim path, points, counts, indices)` in the
  core's frame (metres, Z up, right-handed winding; instances expanded; invisible, guide, proxy and inactive prims
  dropped). `split(meshes, sensors=(), context=())` sorts them by prim path prefix.
- In-process a mesh is named by its prim path (`--target /Floor=lawn`); `run_meshes(sens, ctx, ...)` takes the same
  tuples and names them by file stem (`Floor`), as `python -m daylight` does.
- `write_results(stage, values, manifest, prims, out, on='faces', name='daylight', vmin=None, vmax=None)`: `out`
  (`.usda` / `.usdc`, overwritten) sublayers the stage and overrides each prim of `prims` (the sensor prim paths in
  run order) with `primvars:<name>_<column>` (float, per face or per point) and `primvars:displayColor`
  (`colormap(first column)`, viridis); the stage file stays untouched. Returns the prim paths written. More:
  [results-on-meshes.md](results-on-meshes.md).

## Speed

- The first in-process run of a process pays the GPU start (CUDA context, OptiX pipeline): about 0.35 s on the
  RTX 2060. The example's first run took 0.5-0.8 s with it.
- Runs over the same `SimulationMesh` objects in the same roles and order reuse the resident GPU scene
  (`stats['scene_reused']`): no triangulation, upload or GAS build. daylight and views share it: the example's views
  run took 2 ms. Create the meshes once, keep them open and change the flags; the scene's key is the handles, their
  roles and `--no-self-occlude` (plus the `--no-occlude` set and transmitting `--optics` slots). A new or closed
  `SimulationMesh` never matches, and one scene stays resident: two models used in turn rebuild at every switch.
- A reused scene still pays the run itself: daylight's sky (`sky_ms`: about 90 ms for the example's clear-sky year,
  under 1 ms for one day), the trace, the reduce.
- The process runner pays everything on every run: the process, the GPU start, the files, the scene: 0.5-0.6 s per
  run for the scene of `process.py`, either tool.
- One GPU run per process at a time: runs from several threads take turns.

Measured side by side: [performance.md](performance.md#in-process-vs-cli).

## Errors, cancel, progress

In-process:

| exception | when |
|---|---|
| `native.DaylightError`, `.status` 1 | bad flags or arguments, refused flags, names the meshes do not resolve (`--target`, `--optics`, `--no-occlude`), bad arrays in `SimulationMesh(...)` |
| `native.DaylightError`, `.status` 2 | the run failed: a file (`--epw`, `SimulationMesh.load`), the GPU, out of memory, `cancelled` |
| `TypeError` | sensors or context that are not `SimulationMesh` objects |
| `ValueError` | a tool other than `'daylight'` / `'views'`; `slots` not one per face |
| `FileNotFoundError`, `OSError` | the DLL, or one it needs, is missing; not a loadable DLL, or another C ABI version |

`.status` is the CLI's exit code and the message the core's, e.g. status 1 `--target lake=water: no mesh of that
name`. A `DaylightError` pickles (process pools keep status and message).

The process runner raises `subprocess.CalledProcessError` (`.returncode` 1 or 2) and the exe prints its message to
stderr (status 1 also the usage); `run_meshes` raises `ValueError` for bad arrays before it starts the exe. It has no
progress or cancel: it waits for the exe.

Cancel and progress in-process, through a `Control` (the daylight run [above](#daylight)):

- `native.Control()`: one per run, passed as `control`; closed by `with` or `close()`, never while a run uses it.
- `ctl.progress`: 0..1, the share of the sensors traced, after each trace chunk; 1 once the run succeeded. A small
  model is one chunk: 0, then 1.
- `ctl.cancel()`, from any thread: the run stops at its next check (the scene, a trace chunk) and raises
  `DaylightError` 2 `cancelled`; a control cancelled before the run fails it as it starts; a cancel after the last
  check lets it finish.
- ctypes releases the GIL for the whole DLL call, so other threads (a UI, the progress loop) run while the GPU works.
  Ctrl+C waits for the run to return. Close no `SimulationMesh` a run on another thread uses.

## Reference

- [bindings/python/README.md](../bindings/python/README.md): the whole API (`SimulationMesh`, `native.run`, `Control`,
  `usage`, `run`, `run_meshes`, `to_dmesh`, `meshes`, `split`, `to_obj`, `write_results`, `colormap`,
  `python -m daylight`), the `.dmesh` layout and the USD conversion rules.
- Flags, outputs and units: [daylight.md](daylight.md) (daylight), [views.md](views.md) (views); sky and weather:
  [modes.md](modes.md); mesh roles, culling, faces vs vertices: [input.md](input.md).
- The DLL underneath: [c-abi.md](c-abi.md). Timings: [performance.md](performance.md#in-process-vs-cli).

## Examples

Every example, in every language, and `examples/run.ps1`, which runs them all: [examples.md](examples.md).

Run from `bindings/python` after the build and `uv sync`; each takes 2-3 s on the RTX 2060, Python start included.

| file | shows | run |
|---|---|---|
| [examples/python/inprocess.py](../examples/python/inprocess.py) | `daylight.native`: meshes from numpy and from an OBJ, daylight on a worker thread with its progress, views on the resident scene, means per mesh | `uv run python ../../examples/python/inprocess.py` |
| [examples/python/process.py](../examples/python/process.py) | `run_meshes` with named meshes for `daylight.exe` and `views.exe`, means per row of the facade; the USD one-liners in its docstring | `uv run python ../../examples/python/process.py` |
