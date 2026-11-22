# C++

Both tools in-process from C++, on the library the CLIs are built from: fill `daylight::SimulationMesh` and call `daylight::run`
(daylight: radiation, irradiance, illuminance, sun hours) or `daylight::views::run` (views: the shares of a field of
view that end on sky, ground, obstructions or target groups, and distances). No files and no process; the GPU context,
and the scene of meshes that are run again, stay resident between runs. Plain C and the other languages use the C ABI
([c-abi.md](c-abi.md)). The flags of both CLIs, with the field each one sets: [daylight.md](daylight.md),
[views.md](views.md).

## Setup

Build (Windows x64): VS 2022, CUDA 12.6, the OptiX SDK 9.0 headers (`OPTIX_INSTALL_DIR`), CMake 3.27+ (see the
[README](../README.md#build)).

```
cmake --preset vs2022 && cmake --build --preset release && ctest --preset release
```

`build/RelWithDebInfo/` then holds `daylightcore.lib` (the static library, both tools), `daylight.exe`, `views.exe`,
`daylight_c.dll` (the C ABI) and the examples `example_cpp.exe` and `example_c.exe`. A host project adds the repo and
links the static library, as the `example_cpp` target in [CMakeLists.txt](../CMakeLists.txt) does:

```cmake
add_subdirectory(kit-sim_core)                         # the repo, e.g. a submodule (outside the tree: + a binary dir)
add_executable(app app.cpp)
target_link_libraries(app PRIVATE daylightcore)        # C++17; include path src/: daylight.h, views.h, cli.h
target_link_options(app PRIVATE /NODEFAULTLIB:libcmt)  # cudart_static pulls the static CRT; the library is /MD
```

- The library is built with the `/MD` runtime (CMake's default, `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`); link the
  host the same way. Added with `add_subdirectory`, the repo builds the library, the CLIs, the C ABI and registers its
  ctests; the examples are built only when the repo is the top-level project.
- `DAYLIGHT_CUDA_ARCHITECTURES` (default `75-real;75-virtual`) sets the GPU code of `src/gpu.cu`; the OptiX programs
  are compiled once for compute 7.5 and embedded.
- To run: an NVIDIA GPU of compute capability 7.5 or newer (Turing and later), driver R570+ (OptiX 9.0 ships in it)
  and the VC++ 2015-2022 x64 runtime. The CUDA runtime is linked in: the host needs no CUDA toolkit or OptiX SDK DLL.
- Environment: `CUDA_VISIBLE_DEVICES` selects the GPU; nothing else (`DAYLIGHT_DLL` is for the bindings).

Inputs (`daylight.h`):

| function | |
|---|---|
| `read_obj(path, meshes, err)` | appends one `SimulationMesh` per OBJ `o`/`g` group, named by it |
| `read_dmesh(path, meshes, err)`, `read_dmesh(data, size, name, meshes, err)` | one `.dmesh` mesh from a file (named by its stem) or from memory |
| `read_epw(path, w, err)`, `read_epw(istream, label, w, err)` | EPW weather: hourly DNI / DHI and the location |
| `clear_sky_weather(lat, lon, tz, turbidity, altitude, w, err)` | a cloudless year (Ineichen-Perez), as `--clear-sky` |
| `read_usd(path, upAxis, metersPerUnit, meshes, err)` | USD stages: `bindings/cpp`, below |

A `daylight::SimulationMesh` holds `points` (xyz interleaved, world metres, Z up), `counts` (vertices per face, at least 3),
`indices` (0-based, right-handed winding: the normals come from the geometry), `slot` (an optics slot per face; empty:
slot 0), `name` (what `optics`, `noOcclude` and `targets` refer to, and what `Result::ranges` reports) and `sensor`
(true: analysed, a row per element, and blocking; false: context, blocks rays only). Conventions: [input.md](input.md).

**USD.** `bindings/cpp` adds `read_usd` (LightUSD: sublayers, references, payloads, xforms, up axis, units): configure
with `-DDAYLIGHT_USD=ON` (preset `vs2022-usd`) and link `daylight_usd`
([bindings/cpp/README.md](../bindings/cpp/README.md)).

```cpp
#include "daylight_usd.h"
std::vector<daylight::SimulationMesh> meshes; std::string err;
if (!daylight::read_usd("scene.usdc", 'A', -1, meshes, err)) throw std::runtime_error(err);  // 'A' = stage up axis, -1 = authored metersPerUnit
for (auto& m : meshes) m.sensor = m.name.rfind("/World/Bldg", 0) == 0;   // prim path prefix picks the sensors
```

## Daylight

The worked example is [examples/cpp/example.cpp](../examples/cpp/example.cpp), commented step by step: a 10 x 6 m
facade facing south in 1 m quads (made by its `grid` function), the lawn (four 2 m ground quads) and the floating 1 m
box of [tests/box.obj](../tests/box.obj) in front of it, and a 30 x 9 m block across the street. Its steps 1 to 3 build
the meshes, run a clear-sky year of radiation and sun hours in Berlin and print the mean per mesh. The key calls:

```cpp
std::vector<daylight::SimulationMesh> meshes;
meshes.push_back(grid("facade", {-5, 4, 0}, {10, 0, 0}, {0, 0, 6}, 10, 6));   // a sensor: a row per face
daylight::read_obj("tests/box.obj", meshes, err);                              // + "ground" and "box"
meshes.push_back(grid("block", {-15, -14, 0}, {30, 0, 0}, {0, 0, 9}, 10, 3));
meshes.back().sensor = false;                                                  // context: blocks rays only

daylight::Weather weather;
daylight::clear_sky_weather(52.52, 13.41, 1, 3, 0, weather, err);   // lat, lon, tz, Linke turbidity, altitude
daylight::Options options;
options.quantities = {daylight::Quantity::Radiation, daylight::Quantity::SunHours};
options.sceneKey = "site";                                           // keep the GPU scene for the views run
daylight::Result year;
if (!daylight::run(meshes, &weather, options, year, err)) return fail(err);
```

```
daylight, clear-sky year: scene reused false, 310 ms
  facade 60 faces  1395 kWh/m2  3245 sun hours
  ground  4 faces  1147 kWh/m2  3058 sun hours
  box    12 faces   754 kWh/m2  1752 sun hours
```

Per mesh, the mean over its faces of the annual kWh/m2 on their front side and of the hours they are sunlit. The
numbers are the CLI's for the same meshes (the facade and the block written as OBJ files) and flags (`daylight
facade.obj tests/box.obj --context block.obj --clear-sky --lat 52.52 --lon 13.41 --quantity radiation,sunhours`). A
clear-sky year is an upper bound: every hour is cloudless, typically 20-50 % above a typical year
([modes.md](modes.md)); `read_epw` loads real weather instead. The box's mean includes its bottom (reflected ground
light only) and its north faces. The first run of a process starts the GPU (see [Speed](#speed)); the 8760 hours' sky
model is part of the time too.

`daylight::Options` (with its defaults; the flag each field stands for is in [daylight.md](daylight.md)):

| field | default | |
|---|---|---|
| `quantities` | `{Radiation}` | `Radiation`, `Direct`, `Diffuse`, `Irradiance`, `Illuminance`, `SunHours`: a column each |
| `mode` | `Physical` | or `Ladybug` ([modes.md](modes.md)) |
| `t0`, `t1` | NAN, NAN | NAN: a step per weather row (annual); `t0` alone: one instant (`--time`); `[t0, t1)`: a range (`--from`, `--to`). Hours of year in the `tz` clock, January 1 00:00 = 0: June 21 is [4104, 4128) in a common year |
| `stepMin`, `dayFrom`, `dayTo` | 60, 0, 1440 | the range's step, and the minutes of each day kept (`--hours`) |
| `lat`, `lon`, `tz` | NAN | the weather's; set them for a run without weather (sun hours) or to override it |
| `mf`, `patchSamples` | 1, 1 | sky density (145 patches), stratified rays per patch |
| `ground` | 0.2 | ground reflectance |
| `fresnel`, `bounces`, `bounceSamples` (and `optics`, `transmission` of `RunOptions`) | true, 0, 1024 (none, false) | materials and reflected light, below |
| `seriesPath`, `dumpBitsPath`, `dumpSky`, `dumpSun` | empty | the time series and validation dumps (files) |

`weather` may be `nullptr` only when every quantity is `SunHours` (then `lat`, `lon` and `tz` give the place). Optics:
`optics` holds the `--optics` rows (`daylight::Optics {mesh, slot, rho, tau, glass}`, mesh = `SimulationMesh::name` or `"*"`,
slot -1 = the whole mesh, face slots from `SimulationMesh::slot`), `transmission` and `fresnel` the switches, `bounces` (0..20)
and `bounceSamples` (64..65536) the reflected light. `run()` fails on rows or switches out of range (also ladybug mode
with transmission or bounces) and on rows the meshes do not resolve (an unknown or ambiguous name, a slot out of range,
a row given twice).

The fields both tools take alike are in `daylight::RunOptions`, the base of `daylight::Options` and
`daylight::views::Options`: `sides`, `cullBackfaces`, `onVertices`, `selfOcclude`, `noOcclude`, `north`, `offset`,
`virtualGround` (daylight's ground light, views' ground plane), `optics` and `transmission` (daylight: light through
glass and porous slots; views: rays pass glass slots, [views.md](views.md#glass-and-interiors)), `sceneKey`,
`residentScenes`, `cancel`, `progress`, `verbose`, `chunkBytes`, `f32Columns`.

## Views

The same meshes, from each face 3,600 rays over the default field (120 deg wide about the normal, from 70 deg down to
50 deg up, 2 deg cells); the four ground quads of `box.obj` are a target, group `lawn` (the example's step 4):

```cpp
daylight::views::Options view;
view.targets = {{"ground", "lawn"}};   // rays ending on the mesh "ground" count for the group "lawn"
view.reach = {6, 20, 50};              // --reach en17037
view.sceneKey = "site";                // daylight's scene, reused
view.progress = &progress;             // std::atomic<double>, another thread may read it
view.cancel = &cancel;                 // std::atomic<bool>, another thread may set it
daylight::Result seen;
if (!daylight::views::run(meshes, view, seen, err)) return fail(err);
```

```
views, 120 deg about each normal: scene reused true, 1 ms, progress 100 %
  facade 60 faces  sky 0.34  lawn 0.06  distance  43 m  sees 6 / 20 / 50 m: 0.73 0.43 0.35
  ground  4 faces  sky 0.61  lawn 0.00  distance 205 m  sees 6 / 20 / 50 m: 0.69 0.61 0.61
  box    12 faces  sky 0.28  lawn 0.29  distance  51 m  sees 6 / 20 / 50 m: 0.37 0.29 0.28
```

The CLI gives the same numbers (`views facade.obj tests/box.obj --context block.obj --fov 120 --target ground=lawn
--reach en17037`). The facade sees 34 % sky where an open field has 45 % (sin 50 / (sin 50 + sin 70)): the block and
the box hide a quarter of it. 6 % of its field is lawn, 2 to 6 m in front of it; below the horizon the rest is the
virtual ground beyond the lawn, the block and the box. Its log-mean distance is 43 m, and 73 %, 43 % and 35 % of its
field reach 6, 20 and 50 m. The ground quads look up: 61 % sky, the facade and the box fill the rest, and their field
lies above their own plane (lawn 0). The box's bottom and sides look down onto the lawn. The run took 1 ms: the same
meshes under the same `sceneKey`, so it traced on the scene the daylight run had built.

`daylight::views::Options` (the CLI's defaults; flags in [views.md](views.md#flags)):

| field | default | |
|---|---|---|
| `fovH`, `elevLo`, `elevHi` | 120, -70, 50 | the field, degrees: H wide about the normal (up to 180), or 360 all around (azimuth from north); `elevLo = elevHi = 0`: horizontal rays only (the 2D isovist) |
| `cell`, `jitter` | 2, true | degrees per ray; each ray at a seeded random point of its cell (false: the centre) |
| `targets` | none | `views::Target {mesh, group}`: rays ending on the mesh of that `SimulationMesh::name` count for `group` (at most 16 groups) |
| `distance` | `DistAuto` | the distance columns, `views::Dist` bits: `DistIsovist` (`volume`, or `area` in a flat field), `DistMean` (`distance`, log mean), `DistMin` (`distance_min`), `DistGroup` (per group too); 0: none; `DistAuto`: `DistIsovist` when `fovH = 360`, else `DistMean \| DistMin` |
| `farDist` | 1000 | metres: what open counts as, and the cap of every distance and of the isovist (`far` is a macro of `windef.h`) |
| `groundZ` | NAN | the virtual ground's height; NAN: the lowest point of the meshes |
| `reach` | none | `reach_<D>` columns, at most 8; `{6, 20, 50}` = `--reach en17037` |
| `dumpPath` | empty | every ray's class and distance (`.npy`), validation and images |

A standing eye all around a floor or ground grid: `fovH = 360`, `offset = 1.6` and, for a flat grid at the model's
lowest point, the grid in `noOcclude`, so the virtual ground takes its place ([views.md](views.md#examples) has the
caveats). Isovists of a room from its measure grid: `fovH = 360`, `elevLo = -90`, `elevHi = 90` (volume) or
`elevLo = elevHi = 0` (area), the grid in `noOcclude`, `offset` to eye height; the example program does both.
`views::field(meshes, opt, f, err)` (over `SimulationMeshView`s) is what `run()` derives before tracing: the option checks, the
grid (`f.nEl` rows x `f.nAz` columns of rays), `f.flat`, the distance columns `f.dist` (`DistAuto` resolved) and the
virtual ground's height (an empty mesh list checks the options only). Targets are labels per run: changing them never rebuilds the scene, unless the scene lacks a target
as such (built under other names, or the name now on another `SimulationMesh`), which rebuilds it under the key.

## Results

`daylight::run` and `daylight::views::run` fill the same `daylight::Result`:

| member | |
|---|---|
| `values` | N x C doubles, row-major: N rows = the elements of the sensor meshes in input order (faces in `counts` order, or points with `onVertices`), C = `columns.size()` |
| `f32` | with `f32Columns`: C x N float32, column-major, and `values` stays empty (half the memory) |
| `columns` | daylight: `<quantity>_<side>` in `quantities` order, sides inner (`radiation_front`, `sunhours_back`); views: the shares `sky`, `ground`, `obstruction`, `ground_obstruction` (the two before it joined) and one per group, then `glazing`, `volume` or `area`, `distance`, `distance_min`, `<g>_distance`, `<g>_distance_min`, `reach_<D>` (as `distance` and the flags select), each `_<side>` |
| `ranges` | (mesh name, rows) per sensor mesh in input order: the manifest's `meshes` |
| `durationHours`, `nSky`, `nSun` | daylight: the hours evaluated and the directions traced; views: 0 |
| `sceneReused` | a resident scene served the run |
| `stats` | `RunStats`: milliseconds of prep, sky, build, trace, reduce, bounce; rays (views: sensors x rays per sensor), launches, GAS bytes |

Rows per mesh: mesh k's rows follow those of the meshes before it, so its first row is the sum of the row counts in
`ranges[0 .. k-1]` and value (row, column c) is `values[row * columns.size() + c]`. The example's `mean` (step 3)
averages a column over one mesh that way.

| columns | unit |
|---|---|
| `radiation`, `direct`, `diffuse` | kWh/m2 summed over the evaluated steps |
| `irradiance` | W/m2, mean over the evaluated hours (instantaneous for one instant) |
| `illuminance` | lux, mean (physical mode) |
| `sunhours` | hours (an hour behind a surface transmitting 0.6 counts 0.6) |
| `sky`, `ground`, `obstruction`, `<group>` | shares of the field's solid angle, summing to 1 |
| `ground_obstruction` | `ground` + `obstruction`: the share that is neither sky nor a target |
| `distance`, `distance_min`, `<g>_distance`, `<g>_distance_min` | metres, clamped to [0.1, `farDist`] |
| `volume`, `area` | the isovist: m3 (all around) or m2 (a flat field), each ray at most `farDist` long |
| `glazing` | the share of the field seen through glass (`transmission`) |
| `reach_<D>` | the share of the field that sees at least D metres |

NaN: daylight never gives one (a degenerate face gets 0). Views gives `distance` and `distance_min` NaN when no ray of
the field is traced (a degenerate face, `obstruction` 1; 360 mode at offset 0 with the whole field behind the element)
and `<g>_distance`, `<g>_distance_min` NaN when g is not in view; skip them (`std::isnan`) when averaging such columns.
The manifest the CLIs write, for a host that stores results like them: `daylight::cli::manifest_json(r)` and
`daylight::views::cli::manifest_json(r, opt, f)` (`f` from `views::field`), in `cli.h`.

## Speed

- The first run of a process starts the GPU (the CUDA context and the OptiX pipeline, about 0.35 s on an RTX 2060);
  they stay for the life of the process. The example's daylight time includes it.
- **Resident scenes.** A non-empty `sceneKey` equal to the previous run's keeps the occluder triangles and the OptiX
  acceleration structure on the GPU: no triangulation, upload or build, only the sensors, directions and the trace (the
  example's views run: 1 ms). The caller guarantees that the meshes and `selfOcclude` are unchanged under a key: give a
  new key (a counter, a hash) when the geometry changes. The core adds the `noOcclude` set, the transmitting slots and,
  with bounces, whether the slots differ in rho or glass; optics values may change without a rebuild. With bounces, a
  model whose slots differ gets a hit record per slot, so switching bounces on and off rebuilds it each time, unless
  `residentScenes` > 1 keeps both copies. daylight and views share an opaque scene under one key (one without
  transmitting slots or a hit record per slot). Empty key (the CLIs): rebuilt every run.
- `residentScenes = N` keeps up to N keyed scenes in VRAM (least recently used freed first, and while less than 1 GB is
  free), so users who alternate between models do not rebuild each other's.
- `daylight::SimulationMeshView {mesh, name, sensor}`: both `run`s take views instead of meshes, so a long-lived host (a server)
  shares one cached `SimulationMesh` between runs that name it differently or give it another role, without a copy.
- Memory: `f32Columns` halves the result; `chunkBytes` caps each per-chunk device buffer (any value gives the same
  numbers). `Result::stats` times each stage; `verbose` prints sizes and timings to stderr, as `-v`.
- CLI runs against in-process runs, measured: [performance.md](performance.md#in-process-vs-cli).

## Errors, cancel, progress

Every call returns `false` with `err` set. `run()` reports a failure there, not by an exception: host memory running
out fails the run with `out of memory: ...` (naming the stage), never the process. It fails on bad options (ranges,
mode rules, more than 2^20 rays per sensor), on names the meshes do not resolve (`optics` rows; `noOcclude` names that
are unknown, ambiguous or a context mesh; `targets` that are unknown, ambiguous, named twice or block no rays), on a
missing place (no weather and no `lat`, `lon`, `tz`), on the GPU (CUDA, OptiX, VRAM) and when cancelled.

- `cancel` (`RunOptions`): an `std::atomic<bool>*` another thread sets; the run stops at its next check (daylight:
  before the sky model, before the GPU, per trace chunk, per reflected-light launch; views: before the GPU, per chunk,
  between launches) and fails with `cancelled`.
- `progress`: an `std::atomic<double>*` the run sets after each trace chunk, the share of sensors traced (0..1).
- A run blocks its thread; run it on a worker thread (`std::async`) to keep a UI free to read `progress` and set
  `cancel`. One run at a time per process, both tools together (one GPU): serialise runs from several threads (the C
  ABI holds a lock for this).

The example's step 6 runs once with `cancel` set and once with a target name the meshes do not resolve:

```
cancelled run: cancelled
bad target: --target tree=green: no mesh of that name
```

## Reference

- [src/daylight.h](../src/daylight.h): `SimulationMesh`, `SimulationMeshView`, `Weather`, `RunOptions`, `Options`, `Optics`, `Result`,
  `RunStats`, the readers, `clear_sky_weather`, `run`; every field is documented there.
- [src/views.h](../src/views.h): `views::Options`, `Target`, `Field`, `field`, `run`.
- [src/cli.h](../src/cli.h): the CLIs' flag parsing, weather and time rules and manifests (`daylight::cli::parse`,
  `prepare`, `manifest_json`; `daylight::views::cli::parse`, `manifest_json`; `kUsage`), for front ends that take CLI
  flags verbatim (the C ABI and the web app's server do).
- Flags with the field each sets: [daylight.md](daylight.md) (`Options`), [views.md](views.md#flags)
  (`views::Options`). Meshes and roles: [input.md](input.md); sky models and weather: [modes.md](modes.md).
- USD reader: [bindings/cpp/README.md](../bindings/cpp/README.md). C and the other languages: [c-abi.md](c-abi.md).

## Examples

Every example, in every language, and `examples/run.ps1`, which runs them all: [examples.md](examples.md).

| file | | run (repo root) |
|---|---|---|
| [examples/cpp/example.cpp](../examples/cpp/example.cpp) | this page, commented step by step: meshes in memory and from `read_obj`, a clear-sky year, means per mesh, views with a target on the resident scene, isovists in a room, progress, cancel, errors | `build\RelWithDebInfo\example_cpp.exe tests` |
| [examples/c/example.c](../examples/c/example.c) | both tools from plain C through `daylight_c.dll` ([c-abi.md](c-abi.md#examples)) | `build\RelWithDebInfo\example_c.exe tests` |

Both are targets of the repo's CMake (`example_cpp` links `daylightcore`, `example_c` links `daylight_c`), built by
`cmake --build --preset release` when the repo is the top-level project, and ctests: `ctest --preset release -R
example` runs them with the `tests` folder as argument. Each takes under a second, most of it the GPU start; the times
printed vary with the GPU and what else runs on it.
