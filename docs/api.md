# Library and bindings

In-process use from C++; the C ABI `daylight_c.dll` ([c-abi.md](c-abi.md)); the C#, Python and TypeScript /
JavaScript bindings, which run both tools in-process through the C ABI or drive the CLIs. CLI flags: [cli.md](cli.md).

## C++

**C++, in-process (no files).** Fill `daylight::Mesh` (points xyz in metres, Z-up; vertices per face; 0-based
indices, right-handed winding) and call `daylight::run`. CMake: `add_subdirectory(<repo>)` +
`target_link_libraries(app PRIVATE daylightcore)`.

```cpp
#include "daylight.h"
#include <stdexcept>
daylight::Mesh m;                                  // one quad; m.sensor = false would make it context (blocks only)
m.name = "roof"; m.points = {0,0,3, 10,0,3, 10,8,3, 0,8,3}; m.counts = {4}; m.indices = {0,1,2,3};
daylight::Options opt; opt.quantities = {daylight::Quantity::Radiation, daylight::Quantity::SunHours};
daylight::Weather w; daylight::Result r; std::string err;
if (!daylight::read_epw("site.epw", w, err) || !daylight::run({m}, &w, opt, r, err)) throw std::runtime_error(err);
// r.values: N x r.columns.size() row-major (N = elements of all sensor meshes, input order; r.ranges = per mesh)
double roofRadiation = r.values[0], roofSunHours = r.values[1];
// no weather file: daylight::clear_sky_weather(52.52, 13.41, 1, 3.0, 0.0, w, err) instead of read_epw (lat, lon, tz, TL, altitude)
```

`daylight::Options` extends `daylight::RunOptions`, the fields every tool of the library takes alike (sensors,
occluders, resident scenes, run control; see [C++: views](#c-views)), with the sky, optics, time and dump fields.

Inputs from memory (uploads, no files): `read_dmesh(data, size, name, meshes, err)` parses one `.dmesh` image,
`read_epw(istream, label, w, err)` parses EPW text. A long-lived process (a server) sets `Options::sceneKey` to an
identity of its meshes + `selfOcclude`: when the key equals the previous run's, the occluder triangles and the OptiX
acceleration structure stay resident and only sensors, directions and the trace are redone; the GPU context is kept
for the whole process either way. `Options::residentScenes = N` keeps up to N keyed scenes in VRAM (least recently
used freed first, and while less than 1 GB is free), so users who alternate between models do not rebuild each other's;
`Result::sceneReused` tells whether the run reused one. `Options::noOcclude` (`--no-occlude`: sensor meshes, by
`Mesh::name`, that are analysed but left out of the acceleration structure) needs no place in the key: `run()` adds the
set, so a scene built for another set is never reused (with `selfOcclude` off every sensor mesh is out anyway and the
set adds nothing), and fails on a name that is unknown, ambiguous or a context mesh. Optics: `Options::optics` holds the `--optics` rows
(`daylight::Optics {mesh, slot, rho, tau, glass}`, mesh = `Mesh::name`, slot -1 = the whole mesh, face slots from
`Mesh::slot`), `Options::transmission` and `Options::fresnel` the switches, `Options::bounces` (0..20) and
`Options::bounceSamples` (64..65536) the reflected light ([cli.md](cli.md)). `run()` fails on rows or switches out of
range (also ladybug mode with transmission or bounces) and on rows the meshes do not resolve (unknown or ambiguous name,
slot out of range, a row given twice; the server reports a job error). Values may change between runs of one key
without a rebuild; a slot that starts or stops transmitting rebuilds, and so does a run with bounces whose slots start
or stop differing in rho or glass (the set of transmitting slots, and with bounces whether the slots differ, are part
of the identity: the acceleration structure then carries a hit record per slot, so switching bounces on and off for a
model with several materials rebuilds it each time, or keeps both copies with `residentScenes` > 1). `Options::cancel` points to an `std::atomic<bool>` another
thread may set: `run()` then stops at its next check (before the sky model, before the GPU, per trace chunk, per
reflected-light launch) and fails with "cancelled". `run()` itself is one call at a time per process (one GPU, one owner thread). `src/cli.h` (`daylight::cli::parse` / `prepare` / `manifest_json`) exposes the CLI's
flag parsing, weather / time rules and manifest for front ends that accept CLI flags verbatim.

**C++ with USD.** `bindings/cpp` adds `read_usd` (LightUSD: sublayers, references, payloads, xforms, up axis,
units); configure with `-DDAYLIGHT_USD=ON` (preset `vs2022-usd`), link `daylight_usd`.

```cpp
#include "daylight_usd.h"
std::vector<daylight::Mesh> meshes; std::string err;
if (!daylight::read_usd("scene.usdc", 'A', -1, meshes, err)) throw std::runtime_error(err);  // 'A' = stage up axis, -1 = authored metersPerUnit
for (auto& m : meshes) m.sensor = m.name.rfind("/World/Bldg", 0) == 0;   // prim path prefix picks the sensors
daylight::Weather w; daylight::Result r;
if (!daylight::read_epw("site.epw", w, err) || !daylight::run(meshes, &w, daylight::Options{}, r, err)) throw std::runtime_error(err);
```

## C++: views

`src/views.h` (namespace `daylight::views`) runs the view analysis of [views.md](views.md) in-process, on the same
library, GPU and resident scenes as `daylight::run`.

```cpp
#include "views.h"
#include <stdexcept>
daylight::Mesh facade, lake;                       // facade: a receiver (Mesh::sensor = true, the default)
facade.name = "facade"; facade.points = {10,0,0, 0,0,0, 0,0,3, 10,0,3}; facade.counts = {4}; facade.indices = {0,1,2,3};
lake.name = "lake"; lake.sensor = false;           // context: blocks only
lake.points = {-20,15,0, 30,15,0, 30,35,0, -20,35,0}; lake.counts = {4}; lake.indices = {0,1,2,3};
daylight::views::Options opt;                      // --fov 120,-70,50 --cell 2, jitter on, distance + distance_min
opt.targets = {{"lake", "water"}};                 // rays ending on mesh "lake" count for group "water"
opt.reach = {6, 20, 50};                           // --reach en17037
daylight::Result r; std::string err;
if (!daylight::views::run({facade, lake}, opt, r, err)) throw std::runtime_error(err);
double water = r.values[3], distance = r.values[4];   // r.values: N x r.columns.size() row-major, as daylight::run
// r.columns: sky_front ground_front obstruction_front water_front distance_front distance_min_front reach_6_front ...
```

`daylight::RunOptions` (`daylight.h`) holds what every mesh-sensor tool of the library takes alike: `sides`,
`cullBackfaces`, `onVertices`, `selfOcclude`, `noOcclude`, `north`, `offset`, `virtualGround`, `sceneKey`,
`residentScenes`, `cancel`, `verbose`, `chunkBytes`, `f32Columns` and `progress`. `daylight::Options` and
`daylight::views::Options` both derive from it, so these fields mean the same in both runs (`virtualGround` is
daylight's ground light and views' ground plane). `views::Options` adds the field (`fovH`, `elevLo`, `elevHi`, `cell`,
`jitter`), `targets` (`views::Target {mesh, group}`, mesh = `Mesh::name` as in `noOcclude`), the distance columns
(`distMean`, `distMin`, `distGroup`), `farDist` (`--far`; `far` itself is a macro of `windef.h`), `groundZ` (NAN = the
meshes' lowest point), `reach` and `dumpPath` (`--dump-rays`), with the CLI's defaults.

`views::run(meshes, opt, out, err)`, over `Mesh` or `MeshView` (a view's name and role count) as `daylight::run`, fills
`Result::values` (or `Result::f32` with `f32Columns`), `columns` and `ranges` as daylight does
([views.md](views.md#output)); `nSky`, `nSun` and `durationHours` stay 0, `stats.rays` is sensors x rays per sensor.
It fails on bad options, on target or `noOcclude` names the meshes do not resolve, and when memory runs out ("out of
memory: ...", never the process); `cancel` stops it at its next check (before the GPU, per chunk, between launches)
with "cancelled", and `progress` goes to 1 chunk by chunk. One call at a time per process, shared with `daylight::run`
(one GPU). `views::field(meshes, opt, f, err)` is what `run()` derives before tracing: the option checks (false with
err), the grid (`f.nEl` rows x `f.nAz` columns of rays per sensor) and the virtual ground's height `f.groundZ`
(`opt.groundZ`, else the lowest point of `meshes`; an empty list checks the options only); the CLI writes its manifest
from it.

Resident scenes: a views run builds daylight's opaque scene under daylight's key (`sceneKey` + the `noOcclude` set), so
under one `sceneKey` it reuses a scene a daylight run built (one without transmitting slots or a hit record per slot)
and the other way round (`Result::sceneReused`). Targets are labels per run, taken from the scene's build order by mesh
(the same `Mesh` object under the same name): changing them never rebuilds the scene, unless the scene lacks a target
so (built under other names, or the name now on another mesh), which rebuilds it under the key. A failed run removes
its `dumpPath` files. `src/cli.h` (`daylight::views::cli::parse` / `manifest_json` / `kUsage`)
exposes the views CLI's flag parsing and manifest for front ends that accept its flags verbatim.

## C# (.NET)

`bindings/csharp` (netstandard2.0, no package dependencies: .NET 8/9, .NET Framework 4.8 hosts such as Rhino 6 /
Grasshopper, Unity): `Mesh.Create` / `Mesh.Load`, `Core.Run` / `Core.RunAsync` with the CLI's flags for either tool,
`Result` (columns, row-major values, `Column(name)`, the manifest, mesh ranges, `SceneReused`), `RunControl` (cancel,
progress), `DaylightException` (`Status` = the CLI's exit code). `Daylight.dll` loads `daylight_c.dll` from
`DAYLIGHT_DLL` or its own folder. See [bindings/csharp/README.md](../bindings/csharp/README.md).

```csharp
using Daylight;
using (Mesh roof = Mesh.Create("roof", new double[] { 0,0,3, 10,0,3, 10,8,3, 0,8,3 }, new[] { 4 }, new[] { 0, 1, 2, 3 }))
using (Mesh city = Mesh.Load("city.obj")) {
    Result r = Core.Run(Tool.Daylight, new[] { "--epw", "site.epw", "--quantity", "radiation,sunhours" }, new[] { roof }, new[] { city });
    double[] radiation = r.Column("radiation_front");
    Result v = Core.Run(Tool.Views, new[] { "--fov", "360" }, new[] { roof }, new[] { city });   // v.SceneReused: true
}
```

## Python

In-process (`daylight.native`, through `daylight_c.dll`): `Mesh(name, points, counts, indices)` / `Mesh.load(path)`
and `native.run(tool, args, sensors, context=(), control=None)` -> `Result(values [N x C], manifest, stats)`; runs
over the same `Mesh` objects reuse the GPU scene, and each run releases the GIL (a `Control` cancels it or reads its
progress from another thread).

```python
from daylight import Mesh, native
with Mesh('roof', P, [4], [0, 1, 2, 3]) as roof, Mesh.load('city.obj') as city:
    r = native.run('daylight', ['--epw', 'site.epw', '--quantity', 'radiation,sunhours'], roof, context=city)
    v = native.run('views', ['--fov', '360', '--target', 'lake=water'], roof, context=city)
print(r.manifest['columns'], r.values, v.stats['scene_reused'])
```

Through the CLI: `run_meshes` takes `(points, counts, indices)` triples or named `(name, points, counts, indices)`
meshes (numpy or lists; names for `--target`, `--optics`, `--no-occlude`), writes `.dmesh` files, runs the CLI
(`tool='views'` for views.exe) and loads its `.npy` output; `meshes` / `split` turn a composed USD stage (`usd-core`) into the same flat arrays;
`write_results` puts the values back on the stage as primvars (see [results-on-meshes.md](results-on-meshes.md)).

```python
import numpy as np, daylight
P = np.array([[0, 0, 3], [10, 0, 3], [10, 8, 3], [0, 8, 3]], float)       # world metres, Z-up
values, manifest = daylight.run_meshes([(P, [4], [0, 1, 2, 3])], context=[(cityP, cityCounts, cityIdx)],
                                       epw='site.epw', args=['--quantity', 'radiation,sunhours'])
print(manifest['columns'], values)                                         # values: [N x C] float64, N = faces

sens, ctx = daylight.split(daylight.meshes('scene.usd'), sensors=['/World/Bldg'])   # [(prim, points, counts, indices)]
values, manifest = daylight.run_meshes([m[1:] for m in sens], [m[1:] for m in ctx], epw='site.epw')
daylight.write_results('scene.usd', values, manifest, [m[0] for m in sens], 'daylight.usda')   # primvars + displayColor
```

```
python -m daylight scene.usd --sensors /World/Bldg -o out --write-usd out/daylight.usda -- --epw site.epw   # USD in, USD out
```

## TypeScript and JavaScript

One package, `daylight-mesh` (`bindings/ts`): the TypeScript sources in `src/`, compiled ES modules with type
declarations in `lib/`, so plain JavaScript imports the same entries (Node, bundlers; a browser needs
`@gltf-transform/core` resolvable, e.g. an import map). `daylight-mesh/native` (Node only, koffi) runs both tools
in-process through `daylight_c.dll` on a worker thread, so the event loop stays free:

```ts
import { Mesh, run } from 'daylight-mesh/native';
const sensors = fromThree([scene]).map(Mesh.fromObj), context = [Mesh.load('city.obj')];
const r = await run('daylight', ['--epw', 'site.epw'], { sensors, context });              // r.data: a Float64Array per column
const v = await run('views', ['--fov', '360'], { sensors, context }, { signal, onProgress }); // v.stats.sceneReused: true
```

`fromThree` / `fromGLB` produce meshes in the core frame (Y-up to Z-up, mirrored transforms
unflipped); `run` (Node) writes `.dmesh` files, runs the CLI and reads its `.npy`; `applyToThree` colours the mesh.
Both runners return the values as columns, `data[c][element]` for `columns[c]`: `run` the core's doubles
(`Float64Array`), `runRemote` float32 (`Float32Array` views of one buffer, read from the server's
`GET /jobs/<id>/values`, float32 little-endian, column-major).

```ts
import { fromThree, fromGLB, run, runRemote, applyToThree, toDmesh } from 'daylight-mesh';
import fs from 'node:fs/promises';
const sensors = fromThree([scene]);                                  // every descendant three.js Mesh
const context = await fromGLB(await fs.readFile('city.glb'));
const { data, columns, manifest } = await run('build/RelWithDebInfo/daylight.exe', { sensors, context, args: ['--epw', 'site.epw'] });
console.log(columns[0], data[0]);                                    // one Float64Array per column, a value per sensor triangle
await applyToThree(roofMesh, data[0].subarray(0, roofTriangles));    // vertex colours (viridis) on the mesh
const remote = await runRemote('http://gpu-box:8080', { sensors, context, epw: epwFile, args: ['--quantity', 'radiation'] }); // browser: daylight_server, meshes cached by id
console.log(remote.columns[0], remote.data[0]);                      // the same columns in float32
```

## Bindings overview

Each binding lives in its own folder with its own tests and README; none changes the core. In-process, every binding
loads the C ABI ([c-abi.md](c-abi.md)): the CLIs' flags, the values bit for bit the CLI's, resident scenes over reused
meshes; browsers use the HTTP server (CUDA cannot run in a browser).

- **C ABI** (`src/daylight_c.h`, `daylight_c.dll`, ctest `c_abi`): mesh handles, `dl_run(tool, flags, sensors,
  context)`, results, cancel / progress. See [c-abi.md](c-abi.md).
- **C#** (`bindings/csharp`, netstandard2.0): `Mesh`, `Core.Run` / `RunAsync`, `Result`, `RunControl`; check
  program on net9.0 and net48. See [bindings/csharp/README.md](../bindings/csharp/README.md).
- **Python + USD** (`bindings/python`, uv project, `usd-core`): `run_meshes` / `to_dmesh` for raw arrays,
  `meshes` / `split` for a composed stage (instancing, visibility, purpose, leftHanded, mirrored transforms,
  metersPerUnit, up axis; all vectorised, flat arrays out), `write_results` / `colormap` for results on the stage,
  `python -m daylight stage.usd --sensors /World/Bldg -o out [--write-usd out/daylight.usda] -- <core flags>` as the
  one-liner (writes one `.dmesh` per prim + `meshes.json`, results in `values.npy`). See [bindings/python/README.md](../bindings/python/README.md).
- **C++ + USD** (`bindings/cpp`, LightUSD): the `daylight_usd` library (`read_usd(path, upAxis, metersPerUnit,
  meshes, err)`) and its `check_usd` test; with `-DDAYLIGHT_USD=ON` the CLI reads `.usd*` directly. See
  [bindings/cpp/README.md](../bindings/cpp/README.md).
- **TypeScript / JavaScript + glTF / three.js** (`bindings/ts`, npm package `daylight-mesh`, compiled JS in `lib/`):
  `fromThree` / `fromGLB` into the core frame, `toDmesh` / `toObj` writers, `run(exe, {sensors, context, args})`
  drives either CLI from Node and reads its `.npy`, `daylight-mesh/native` runs both tools in-process (Node),
  `runRemote` runs daylight on the web app's server (browsers), `applyToThree` / `colormap` for display. See
  [bindings/ts/README.md](../bindings/ts/README.md).
