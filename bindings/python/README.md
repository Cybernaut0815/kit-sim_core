# daylight (Python binding)

Guide: [docs/python.md](../../docs/python.md) (setup, both tools worked through, results, speed, errors); examples:
[examples/python](../../examples/python) ([docs/examples.md](../../docs/examples.md)).

USD stage -> raw mesh arrays for the agnostic `daylight` core, a `.dmesh` writer for numpy arrays, a thin
`subprocess` wrapper around the core's two CLIs (`daylight.exe`: daylight / solar, `views.exe`: view analysis), and
`daylight.native`, both tools in-process through the C ABI (`daylight_c.dll`, [docs/c-abi.md](../../docs/c-abi.md)).
uv project (Python >= 3.10, `usd-core==26.8`, `numpy`); no C++ build is needed for the conversion itself. Everything
stays binary: `meshes()` is vectorised numpy, meshes go to the core as `.dmesh` (or as arrays in-process), values come
back as `.npy` (1M quads: `meshes()` 0.1 s + `to_dmesh` 0.15 s after `Stage.Open`).

## Usage

Raw arrays -> values (one quad sensor; `values` is `[N x C]`, N = faces by default, C = `manifest['columns']`):

```python
import numpy as np, daylight
P = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], float)        # world metres, Z-up
values, manifest = daylight.run_meshes([(P, [4], [0, 1, 2, 3])], epw='site.epw')
print(manifest['columns'], manifest['meshes'], values)
```

USD stage -> `meshes()` / `split()` / `run_meshes()` (sensors analysed and blocking, the rest blocking only):

```python
import daylight
sens, ctx = daylight.split(daylight.meshes('scene.usd'), sensors=['/World/Bldg'])
values, m = daylight.run_meshes([x[1:] for x in sens], [x[1:] for x in ctx], epw='site.epw',
                                args=['--quantity', 'radiation'])
print([x[0] for x in sens], m['meshes'], values.shape)                  # prim paths in row order
```

Views (`tool='views'`, [docs/views.md](../../docs/views.md)): `run_meshes` also takes the `(name, points, counts,
indices)` tuples of `meshes()` as they are; each name becomes the file stem `python -m daylight` gives it
(`/World/Lake` -> `World_Lake`), the mesh name in the manifest and in flags such as `--target NAME=GROUP`:

```python
import daylight
sens, ctx = daylight.split(daylight.meshes('scene.usd'), sensors=['/World/Bldg'])
values, m = daylight.run_meshes(sens, ctx, tool='views', args=['--target', 'World_Lake=water', '--reach', 'en17037'])
print(m['columns'], m['targets'])                  # sky_front ground_front obstruction_front water_front ...
```

Results back on the stage (`write_results`): a new layer that sublayers `scene.usd` and overrides each sensor prim
with float primvars `daylight_<column>` + `primvars:displayColor` (viridis via `colormap`), so opening the new file
alone in usdview / Kit shows the coloured scene and `scene.usd` stays untouched:

```python
import daylight
sens, ctx = daylight.split(daylight.meshes('scene.usd'), sensors=['/World/Bldg'])
values, m = daylight.run_meshes([x[1:] for x in sens], [x[1:] for x in ctx], epw='site.epw',
                                args=['--quantity', 'radiation'])
daylight.write_results('scene.usd', values, m, [x[0] for x in sens], 'daylight.usda')   # -> usdview daylight.usda
```

One-liner (writes `out/<stem>.dmesh` per mesh + `out/meshes.json`; flags after `--` go to the core ->
`out/values.npy(.json)`, rows in `meshes.json` order; `--write-usd PATH` adds the sublayer file; `--tool views` runs
`views.exe` instead of `daylight.exe`, primvars `views_<column>`):

```
cd bindings/python && uv sync
uv run python -m daylight scene.usd --sensors /World/Bldg -o out --write-usd out/daylight.usda -- --epw x.epw --quantity radiation
uv run python -m daylight scene.usd --sensors /World/Bldg -o out --tool views -- --target World_Lake=water
uv run python -m daylight.check          # self-checks: to_dmesh / masking / converter / colormap / write_results,
                                         # run_meshes end to end (needs the exes), native in-process (needs the DLL)
```

`--sensors`/`--context` are prim-path prefixes (repeatable): no `--sensors` = every mesh is a sensor, no `--context`
= every non-sensor mesh blocks only, both = the rest is dropped. `--time T` picks the USD time code (default: the
stage start time). File stem = prim path with `[^A-Za-z0-9._-]` replaced by `_`, leading `_` stripped, made unique
with `_2`, `_3` ... (`/World/Bldg/roof` -> `World_Bldg_roof.dmesh`); `meshes.json` is the ordered list
`[{"file", "prim", "role": "sensor" | "context"}]`, sensors first.

## In-process (`daylight.native`)

Both tools in this process through `daylight_c.dll`: no process start, no files, the GPU context kept, and runs over
the same `SimulationMesh` objects in the same roles and order reuse the resident GPU scene (no triangulation, upload or GAS
build; daylight and views share it). Flags are the CLIs' own, values and manifest bit for bit what the CLI writes.

```python
from daylight import SimulationMesh, native
with SimulationMesh.load('site.obj') as site, SimulationMesh('grid', P, C, I) as grid:             # P [N,3] float64, C counts, I indices
    r = native.run('daylight', ['--epw', 'site.epw', '--quantity', 'radiation'], grid, context=site)
    v = native.run('views', ['--fov', '360', '--offset', '1.6', '--target', 'lake=water'], grid, context=site)
print(r.values.shape, r.manifest['columns'], v.stats['scene_reused'])             # True: the scene of the first run
```

- `SimulationMesh(name, points, counts, indices, slots=None)`: one mesh from the `.dmesh` arrays (numpy or sequences, made
  C-contiguous float64 points `[N,3]`, int32 counts and 0-based indices, uint16 optics slot per face), copied into the
  DLL; `SimulationMesh.load(path)`: the meshes of a `.obj` (one per `o`/`g` group) or `.dmesh` file as the CLI reads them.
  `.names` = what the manifest, `--target`, `--optics` and `--no-occlude` call them. `close()`, a `with` block or
  garbage collection frees the handle; never close one a run on another thread is using.
- `native.run(tool, args, sensors, context=(), control=None)` -> `Result(values, manifest, stats)`: `tool` `'daylight'`
  ([daylight.md](../../docs/daylight.md)) or `'views'` ([views.md](../../docs/views.md)), `args` its flags as strings
  (mesh paths, `--context`, `--out`, `--bench` and `-h` are refused: the meshes are `sensors` / `context`, each a
  `SimulationMesh` or an iterable of them), `values` `[N x C]` float64, `manifest` the CLI's `FILE.json` as a dict, `stats`
  `{scene_reused, prep_ms, sky_ms, build_ms, trace_ms, reduce_ms, bounce_ms, rays, launches, gas_bytes}`. Files in
  flags (`--epw`, `--timeseries`, `--dump-*`) are paths of this process; a `str` goes as UTF-8 (names), a
  `pathlib.Path` in the ANSI code page the core opens files with, as the CLI's argv: give a non-ASCII path as a `Path`.
  `native.usage(tool)`: the CLI's `-h` text.
- `Control()`: pass it as `control`, then from another thread `cancel()` (the run stops at its next check and raises
  `DaylightError` 2 `cancelled`) or read `progress` (0..1). One control per run; `close()` / `with` as for `SimulationMesh`.
- Errors raise `native.DaylightError` with `.status` = the CLI's exit code (1: flags, arguments, mesh names the flags
  do not resolve; 2: a file, the GPU, out of memory, `cancelled`) and the core's message. Numbers are read and written
  with `.` whatever `locale.setlocale` set (the DLL runs each call in the `"C"` locale).
- Threads: ctypes releases the GIL during every DLL call, so other threads keep running while a run traces (that is
  how `cancel()` and `progress` reach it; Ctrl+C waits for the run to return). Runs take turns: one GPU run per process.
- The DLL loads on first use, by absolute path: `DAYLIGHT_DLL` if set, else `daylight/daylight_c.dll` next to the
  package, else `<repo>/build/RelWithDebInfo/daylight_c.dll`. It needs the NVIDIA driver and the VC++ 2015-2022 x64
  runtime ([docs/c-abi.md](../../docs/c-abi.md#loading-the-dll)); another ABI version is refused (`OSError`).

## API

- `meshes(stage, time=None)` -> `[(name, points [N,3] float64, counts int32 [F], indices int32 [sum(counts)])]`
  (the `.dmesh` arrays, 0-based flat indices); `split(meshes, sensors, context)`; `to_obj(meshes, path)` (interop).
- `to_dmesh(points, counts, indices, path)`: one mesh as raw arrays -> `.dmesh` (layout below). `ValueError` unless
  `sum(counts) == len(indices)`, every count >= 3 and every index in `[0, n_points)`.
- `run(sensors, context=(), epw=None, args=(), out=None, tool='daylight')` -> `(values [N x C], manifest)`;
  `sensors`/`context` are a mesh file (`.obj` / `.dmesh`) or an iterable of them, passed positional / as repeated
  `--context`. `tool='views'` runs `views.exe` (`epw` is daylight's). `out` keeps the value file: `.npy` (default,
  `np.load`) or any other suffix for text (`np.loadtxt`).
- `run_meshes(sensors, context=(), **run kwargs)`: each argument an iterable of `(points, counts, indices)` triples
  or `(name, points, counts, indices)` tuples as `meshes()` gives them (a single one is accepted too); written as
  `<stem>.dmesh` (= the manifest names): the name's file stem as `python -m daylight` makes it, `sensor_<i>` /
  `context_<i>` for a triple.
- `native`: the in-process API above (`SimulationMesh`, `Control`, `run`, `usage`, `DaylightError`); `daylight.SimulationMesh` is
  `native.SimulationMesh`.
- `write_results(stage, values, manifest, prims, out, on='faces', name='daylight', vmin=None, vmax=None)` -> written
  prim paths. `stage` = path or `Usd.Stage`; `values` `[N x C]` (1-D = one column) with rows per `manifest['meshes']`,
  `prims` the sensor prim paths in that order; `out` (.usda/.usdc) is created / overwritten with `subLayerPaths =
  [relative path to the stage's root layer]` (absolute when on another drive). Per column one primvar `<name>_<column>` (`float[]`, `uniform` for
  faces / `vertex` for points) plus `displayColor` from `colormap(first column, vmin, vmax)` (default range: finite
  min/max over all written sensors). Faces `meshes()` dropped (holes, < 3 vertices) get `nan` and grey; any other
  count mismatch raises `ValueError` naming the prim; instance proxies and `<instancer>[k]...` names are skipped with a
  warning (no prim to override). Uses the stage start time.
- `colormap(values, vmin=None, vmax=None)` -> float64 `[N, 3]` RGB in 0..1: viridis with 9 stops, linear in
  between, `t = clip((v - vmin) / (vmax - vmin), 0, 1)` (`vmax == vmin` -> first stop), non-finite -> 0.5 grey. The
  same table as the TypeScript binding, so colours match across tools.

## `.dmesh` layout (one mesh per file, little-endian, name = file stem)

```
offset  0  char[4]   "DLM1"                 magic + version
offset  4  uint32    flags                  0 here (this binding writes no slot block); bit 0 = uint16 slot[n_faces] after the indices, see docs/input.md
offset  8  uint64    n_points
offset 16  uint64    n_faces
offset 24  uint64    n_indices
offset 32  float64   xyz[3 * n_points]      world metres, Z-up
           int32     counts[n_faces]        vertices per face (>= 3), right-handed winding
           int32     indices[n_indices]     0-based, sum(counts) == n_indices
```

Conversion: composed stage, instance proxies and PointInstancers expanded (names `<instancer>[k]<proto mesh path>`),
invisible / `guide` / `proxy` / inactive prims dropped, `metersPerUnit` and `upAxis` applied (Y-up -> Z-up as
`(x, -z, y)`), winding flipped for `leftHanded` meshes and mirroring transforms, holes and faces with < 3 vertices
dropped, authored normals ignored, subdivision control cages used as-is (warning on stderr). `ValueError` names the
prim when `faceVertexCounts` / `faceVertexIndices` are inconsistent or out of range.

OBJ contract of the core (`to_obj`, interop only): world metres, Z-up, right-handed (counter-clockwise = front)
winding, one `o` group per mesh, 1-based global vertex indices.

Core executables: `DAYLIGHT_EXE` / `VIEWS_EXE` if set, else `<repo>/build/RelWithDebInfo/daylight.exe` /
`views.exe` (`core.find_exe(tool)`).
