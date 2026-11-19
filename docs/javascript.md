# TypeScript and JavaScript

`bindings/ts` is one npm package, `daylight-mesh`, for TypeScript and plain JavaScript: the sources in `src/*.ts`,
compiled to ES modules in `lib/*.js` with `lib/*.d.ts` beside them (committed; `package.json` `exports` maps
`daylight-mesh` to `lib/index.js` and `daylight-mesh/native` to `lib/native.js`). It runs both tools of the core,
daylight ([daylight.md](daylight.md)) and views ([views.md](views.md)), with their CLI flags, three ways:

| way | import | runs on | how |
|---|---|---|---|
| in-process | `daylight-mesh/native`: `Mesh`, `run(tool, args, { sensors, context }, { signal, onProgress })` | Node on the GPU host | `daylight_c.dll` ([c-abi.md](c-abi.md)) through koffi, on a worker thread; meshes copied once, their scene kept on the GPU between runs |
| process runner | `daylight-mesh`: `run(exe, { sensors, context, args })` | Node on the GPU host | `daylight.exe` or `views.exe` as a child process: a `.dmesh` per mesh in a temp folder, the `.npy` read back |
| server | `daylight-mesh`: `runRemote(url, { sensors, context, epw, args, ... })` | browsers (and Node) | a daylight job on the web app's `daylight_server`, which runs the core in-process on the GPU host; views is not served |

CUDA cannot run in a browser, so browsers use the server. All three return the values as columns, `data[c][row]`
for `columns[c]`, and the manifest. Meshes come from arrays (`Mesh.create`, `ObjMesh`), files (`Mesh.load`), three.js
(`fromThree`) or glTF (`fromGLB`); `applyToThree` colours a three.js mesh by its values
([three.js and glTF](#meshes-from-threejs-or-gltf)).

## Setup

```sh
cmake --preset vs2022 && cmake --build --preset release   # daylight.exe, views.exe, daylight_c.dll
cd bindings/ts && npm install && npm run build && npm test  # koffi, three, @gltf-transform/core; src -> lib
```

| | |
|---|---|
| GPU host | Windows x64, an NVIDIA GPU of compute capability 7.5 or newer, driver R570+, the VC++ 2015-2022 x64 runtime ([c-abi.md](c-abi.md#loading-the-dll)) |
| Node | >= 20 for `lib/`; >= 22.18 runs the `.ts` sources as they are (type stripping) |
| `daylight_c.dll` | `init(path)`, else env `DAYLIGHT_DLL`, else `<repo>/build/RelWithDebInfo/daylight_c.dll` (found from `src/` or `lib/`); loaded by the first call, once per process, until Node exits |
| `daylight.exe`, `views.exe` | the process runner's first argument is the exe's path |
| dependencies | `@gltf-transform/core` (`fromGLB`); peer dependencies `three` (`applyToThree` only, imported lazily) and `koffi` 3 (optional; `daylight-mesh/native` only) |
| environment | `DAYLIGHT_DLL`; `CUDA_VISIBLE_DEVICES` picks the GPU; the process runner writes under `os.tmpdir()` (`TEMP`) |

The package is not published (`"private": true`). Import its files by path, as the examples do (TypeScript
`bindings/ts/src/*.ts`, JavaScript `bindings/ts/lib/*.js`; Node resolves koffi and `@gltf-transform/core` from
`bindings/ts/node_modules`, beside them), or link the folder into a project (`npm install <repo>/bindings/ts`) and
import `daylight-mesh` and `daylight-mesh/native`. Plain JavaScript uses the same names and calls without the types:
`examples/javascript/native.mjs` is `examples/typescript/native.ts` without them. Bundlers: keep
`daylight-mesh/native` and koffi external (its worker thread runs the module's own file); browser code imports
`daylight-mesh` only (its `run` loads Node builtins lazily and stays on Node; without a bundler `@gltf-transform/core`
needs an import map, see the [README](../bindings/ts/README.md)).

## Daylight

`examples/typescript/native.ts`, in-process: `tests/box.obj` (the meshes `ground`, four 2 m quads at z 0, and `box`,
a 1 m box floating above the fourth) and a facade from arrays, all sensors (analysed, and blocking each other); a
clear-sky year in Berlin:

```ts
import { fileURLToPath } from 'node:url';
import { DaylightError, Mesh, run, type NativeResult } from '../../bindings/ts/src/native.ts';

// Meshes are copied to the C side once; runs over the same handles in the same roles and order reuse the GPU scene.
const site = Mesh.load(fileURLToPath(new URL('../../tests/box.obj', import.meta.url))); // OBJ groups: ground, box
const facade = Mesh.create('facade', // a 4 x 3 m wall at y = 3 facing south (-y), two 1.5 m rows
  [-2, 3, 0, 2, 3, 0, 2, 3, 1.5, -2, 3, 1.5, 2, 3, 3, -2, 3, 3], // xyz per point: metres, Z up
  [4, 4], // vertices per face
  [0, 1, 2, 3, 3, 2, 4, 5]); // 0-based, counter-clockwise seen from the front
const sensors = [site, facade];
console.log('meshes:', sensors.flatMap((m) => m.names).join(', '));

// The rows of one mesh in one column: the manifest's ranges, one { name, first, count } per mesh in sensor order.
type Ranges = { meshes: { name: string; first: number; count: number }[] };
function rows(r: NativeResult, mesh: string, column: string): Float64Array {
  const { first, count } = (r.manifest as Ranges).meshes.find((m) => m.name === mesh)!;
  return r.data[r.columns.indexOf(column)].subarray(first, first + count);
}
const mean = (v: Float64Array) => v.reduce((a, b) => a + b, 0) / v.length;

// daylight: a clear-sky year in Berlin (no weather file), annual radiation and sun hours per face.
const args = ['--clear-sky', '--lat', '52.52', '--lon', '13.41', '--quantity', 'radiation,sunhours'];
let t = performance.now();
const sun = await run('daylight', args, { sensors },
  { signal: AbortSignal.timeout(10_000), onProgress: (p) => console.log(`  traced ${Math.round(100 * p)} %`) });
console.log(`daylight: ${(performance.now() - t).toFixed(0)} ms, scene reused ${sun.stats.sceneReused}`);
for (const { name } of (sun.manifest as Ranges).meshes) {
  const kwh = mean(rows(sun, name, 'radiation_front')), hours = mean(rows(sun, name, 'sunhours_front'));
  console.log(`  ${name.padEnd(6)} ${kwh.toFixed(0)} kWh/m2, ${hours.toFixed(0)} h (mean per face)`);
}
```

```text
meshes: ground, box, facade
  traced 0 %
  traced 100 %
daylight: 436 ms, scene reused false
  ground 1384 kWh/m2, 4137 h (mean per face)
  box    915 kWh/m2, 2207 h (mean per face)
  facade 1554 kWh/m2, 3522 h (mean per face)
```

- `Mesh.load(path)` reads a `.obj` (a mesh per `o`/`g` group, named by it) or a `.dmesh` (named by its stem) as the
  CLI does; ASCII paths. `Mesh.create(name, positions, counts, indices, slots?)` takes the `.dmesh` arrays
  ([input.md](input.md)): xyz in metres, Z up; vertices per face (any polygon, >= 3); 0-based indices, right-handed
  (counter-clockwise seen from the front); an optional optics slot per face. `mesh.names` is what the manifest and
  `--target`, `--optics`, `--no-occlude` call the meshes.
- `run('daylight', args, { sensors, context })`: `args` are daylight's flags without the program name; the handles
  take the place of the mesh files, `context` that of `--context` (blocks rays, never analysed). Mesh paths,
  `--context`, `--out`, `--bench` and `-h` are refused. The values and the manifest are the CLI's, bit for bit.
- Weather: `--epw FILE` (an ASCII path), `--clear-sky --lat --lon` (a cloudless year, an upper bound 20-50 % above a
  typical year: [modes.md](modes.md)), or none for `--quantity sunhours` with `--lat --lon` (`--tz` defaults to
  `round(lon / 15)`, 1 here). Time: annual by default, else `--time`, or `--from --to` with `--step` and `--hours`.
- `sceneReused` is false: the first run started the GPU and built the scene. The box's mean takes in its bottom, which
  sees no sun.

### Through `daylight.exe`

`examples/typescript/cli.ts` runs the exes through the process runner. Its meshes are `ObjMesh`es (`{ name, positions,
indices }`, triangles): two storeys of a facade as sensors, a lake 15 m out and a block's face 40 m out as context.

```ts
import { fileURLToPath } from 'node:url';
import { dmeshStems, run, type ObjMesh } from '../../bindings/ts/src/index.ts';

const exe = (name: string) => fileURLToPath(new URL(`../../build/RelWithDebInfo/${name}`, import.meta.url));
// A quad as two triangles (an ObjMesh holds triangles): corners counter-clockwise seen from its front, metres, Z up.
const quad = (name: string, ...corners: number[][]): ObjMesh =>
  ({ name, positions: corners.flat(), indices: [0, 1, 2, 0, 2, 3] });

const sensors = [ // two storeys of a 10 m wide facade at y = 0, facing south (-y)
  quad('facade/floor 0', [0, 0, 0], [10, 0, 0], [10, 0, 3], [0, 0, 3]),
  quad('facade/floor 1', [0, 0, 3], [10, 0, 3], [10, 0, 6], [0, 0, 6]),
];
const context = [
  quad('site/lake', [-20, -35, 0], [30, -35, 0], [30, -15, 0], [-20, -15, 0]), // 50 x 20 m on the ground, 15 m out
  quad('site/block', [15, -40, 0], [-5, -40, 0], [-5, -40, 15], [15, -40, 15]), // a block's 20 x 15 m face, 40 m out
];

// One column's mean per sensor mesh (a storey: two triangle rows), from the manifest's ranges.
type Result = { data: Float64Array[]; columns: string[]; manifest: unknown };
type Ranges = { meshes: { name: string; first: number; count: number }[] };
const means = (r: Result, column: string) => (r.manifest as Ranges).meshes.map(({ first, count }) =>
  r.data[r.columns.indexOf(column)].subarray(first, first + count).reduce((a, b) => a + b, 0) / count);
const names = (r: Result) => (r.manifest as Ranges).meshes.map((m) => m.name);

// daylight.exe: a clear-sky year in Berlin. Every call starts the exe, and with it the GPU, again.
let t = performance.now();
const sun = await run(exe('daylight.exe'), { sensors, context,
  args: ['--clear-sky', '--lat', '52.52', '--lon', '13.41', '--quantity', 'radiation,sunhours'] });
console.log(`daylight.exe: ${(performance.now() - t).toFixed(0)} ms, columns ${sun.columns.join(' ')}`);
const kwh = means(sun, 'radiation_front'), hours = means(sun, 'sunhours_front');
names(sun).forEach((n, k) => console.log(`  ${n}: ${kwh[k].toFixed(0)} kWh/m2, ${hours[k].toFixed(0)} h`));
```

```text
daylight.exe: 772 ms, columns radiation_front sunhours_front
  facade_floor_0: 1573 kWh/m2, 3516 h
  facade_floor_1: 1631 kWh/m2, 3605 h
```

The block, 15 m tall and 40 m out, takes the low winter sun from the ground floor. The manifest names the meshes by
their `.dmesh` stems ([Views](#views) has the rule).

### Meshes from three.js or glTF

`fromThree(objects)` takes every descendant three.js `Mesh` (named by its `name`, else its `uuid`; world matrices
applied, Y up turned to Z up, mirrored transforms rewound), `fromGLB(bytes)` every triangle primitive of a `.glb` (named
`node/mesh`); `{ scale }` multiplies the coordinates into metres. Both give `ObjMesh`es for the process runner and the
server, or handles through `Mesh.fromObj` (which keeps the name as it is: what `--target`, `--optics` and
`--no-occlude` name). views takes the same meshes.

```ts
import { applyToThree, fromGLB, fromThree } from 'daylight-mesh';
import { Mesh, run } from 'daylight-mesh/native';
import fs from 'node:fs/promises';

const roof = Mesh.fromObj(fromThree([roofMesh])[0]);                          // a three.js Mesh without child meshes
const city = (await fromGLB(await fs.readFile('city.glb'))).map(Mesh.fromObj); // a handle per primitive
const r = await run('daylight', ['--epw', 'site.epw'], { sensors: [roof], context: city });
await applyToThree(roofMesh, r.data[r.columns.indexOf('radiation_front')]);   // a value per triangle: flat colours
```

### In a browser

`runRemote` sends the job to the web app's server (`daylight_server`; its API is documented at `/docs` and
`/openapi.json` on the server), which queues the jobs of all its users for the GPU:

```ts
import { fromThree, runRemote } from 'daylight-mesh';

const ctrl = new AbortController();                                      // ctrl.abort(): the server cancels the job
const { data, columns, manifest } = await runRemote(location.origin, {  // the page comes from the server (--web)
  sensors: fromThree([facadeGroup]), context: fromThree([cityGroup]),
  args: ['--clear-sky', '--lat', '52.52', '--lon', '13.41', '--quantity', 'radiation,sunhours'], // or epw: a File
  signal: ctrl.signal,
  onUpload: (done, total) => console.log(`meshes ${done} / ${total}`),   // only the ones the server lacks
  onStatus: (job) => console.log(job.state, job.position ?? '', job.progress ?? ''), // queued (position), running
});
```

- The server sends no CORS headers: the page must come from it (`daylight_server --web DIR`) or its origin. Mesh ids
  are SHA-256 hashes, which need `crypto.subtle`: a secure context (`http://localhost` or HTTPS).
- Meshes go up once per server, by id (their bytes' hash with the name folded in): a re-run with other flags, or a
  reload of the same model, sends no geometry. Weather: `epw` as a `Blob` (a `File` from `<input type="file">`,
  uploaded) or `{ url }` (fetched by the server from its allowlist), or `--clear-sky` in `args`.
- The values come as float32, a `Float32Array` per column (views of one buffer); `manifest` is a `RemoteManifest`
  (`columns`, the `meshes` ranges, `duration_h`, `dirs`, `cache`).
- Daylight only: the server runs no views jobs, and `runRemote` takes no tool. Run views from Node.

## Views

The same meshes in `examples/typescript/native.ts`, after the daylight run:

```ts
// views: 120 deg about each face's normal; rays ending on the mesh "box" count for the group "cube".
t = performance.now();
const view = await run('views', ['--target', 'box=cube'], { sensors }, { signal: AbortSignal.timeout(10_000) });
console.log(`views: ${(performance.now() - t).toFixed(0)} ms, scene reused ${view.stats.sceneReused}`);
console.log(`  columns ${view.columns.join(' ')}`);
const sky = rows(view, 'facade', 'sky_front'), cube = rows(view, 'facade', 'cube_front');
const distance = rows(view, 'facade', 'distance_front'), nearest = rows(view, 'facade', 'distance_min_front');
sky.forEach((s, i) => console.log(`  facade row ${i}: sky ${s.toFixed(3)}, cube ${cube[i].toFixed(3)}, ` +
  `distance ${distance[i].toFixed(1)} m, nearest ${nearest[i].toFixed(2)} m`));
```

```text
views: 2 ms, scene reused true
  columns sky_front ground_front obstruction_front cube_front distance_front distance_min_front
  facade row 0: sky 0.386, cube 0.085, distance 21.9 m, nearest 0.80 m
  facade row 1: sky 0.449, cube 0.067, distance 59.2 m, nearest 1.77 m
```

- `run('views', args, { sensors, context })`: views' flags ([views.md](views.md)), under the same rules as daylight's.
  From every face (or point, `--on vertices`) it casts one ray per 2 deg cell of a field of view: by default 120 deg
  wide about the face's normal, from 70 deg down to 50 deg up (3,600 rays); `--fov 360` turns all around (with
  `--offset 1.6` a standing eye on a floor grid, which `--no-occlude` keeps out of the rays).
- Every ray ends in one class: `sky`; `ground`, the virtual ground plane at the meshes' lowest point; `obstruction`, a
  mesh that is not a target (sensor meshes block, so the ground quads count here); or a target group:
  `--target box=cube` counts the rays that end on the mesh `box` for the group `cube`. The shares weigh each ray by
  its cell's solid angle and sum to 1. `distance` is the log mean of the ray distances (an open ray counts as `--far`,
  1000 m), `distance_min` the nearest thing in view.
- Row 1, the upper half, sees nothing rise above its horizon: its sky is sin 50 / (sin 50 + sin 70) = 0.449 of the
  field. Row 0's eye is 0.75 m up: its nearest is the virtual ground under the field's lower edge, 0.75 / sin 70 =
  0.80 m, and its log mean, 22 m, weighs that near ground and the box against the open sky.
- A few milliseconds: the same handles in the same roles and order, so the scene the daylight run built served it
  (`stats.sceneReused`).

### Through `views.exe`: targets by stem

The rest of `examples/typescript/cli.ts`:

```ts
// views.exe: the core's mesh names are the .dmesh file stems run() writes, sensors then context ('site/lake' ->
// 'site_lake'), so --target names the lake by its stem.
const stems = dmeshStems([...sensors, ...context].map((m) => m.name));
console.log(`core names: ${stems.join(', ')}`);
t = performance.now();
const view = await run(exe('views.exe'), { sensors, context,
  args: ['--target', `${stems[sensors.length]}=water`, '--reach', 'en17037'] });
console.log(`views.exe: ${(performance.now() - t).toFixed(0)} ms, columns ${view.columns.join(' ')}`);
const sky = means(view, 'sky_front'), water = means(view, 'water_front'), distance = means(view, 'distance_front');
const reach = means(view, 'reach_50_front');
names(view).forEach((n, k) => console.log(`  ${n}: sky ${sky[k].toFixed(3)}, water ${water[k].toFixed(3)}, ` +
  `distance ${distance[k].toFixed(1)} m, sees 50 m ${reach[k].toFixed(3)}`));
```

```text
core names: facade_floor_0, facade_floor_1, site_lake, site_block
views.exe: 568 ms, columns sky_front ground_front obstruction_front water_front distance_front distance_min_front reach_6_front reach_20_front reach_50_front
  facade_floor_0: sky 0.406, water 0.024, distance 42.7 m, sees 50 m 0.420
  facade_floor_1: sky 0.415, water 0.069, distance 80.4 m, sees 50 m 0.456
```

- `run(exe, ...)` writes each `ObjMesh` as `<stem>.dmesh`, the stems being `dmeshStems` of the sensors' and then the
  context's names (`[^A-Za-z0-9._-]` to `_`, made unique ignoring case): the core and the manifest name the meshes so,
  and `--target`, `--optics` and `--no-occlude` must too. `Mesh.fromObj` keeps a name as it is: where one flag list
  serves both runners, name the meshes by `dmeshStems` first.
- `--reach en17037` adds `reach_6`, `reach_20` and `reach_50`: the share of the field that sees at least 6, 20 and
  50 m, EN 17037's view distance levels (a share of the whole field, the street below included at its plane distance).
- The water fills 2.4 % of the ground floor's field and 6.9 % of the upper floor's; the block takes some sky (0.406
  and 0.415 instead of 0.449). [views.md](views.md#examples) walks through a street like it, flag by flag.

## Results

| | in-process `run` | process runner `run` | `runRemote` |
|---|---|---|---|
| `data` | a `Float64Array` per column | a `Float64Array` per column | a `Float32Array` per column (views of one buffer) |
| `columns` | `data[c]` holds `columns[c]` | the same | the same |
| `manifest` | the CLI's `FILE.json`, parsed (typed `unknown`); `manifestText`: its text (LF) | the same, parsed (`unknown`) | `RemoteManifest` |
| reuse | `stats`: `sceneReused`, `prepMs`, `skyMs`, `buildMs`, `traceMs`, `reduceMs`, `bounceMs`, `rays`, `launches`, `gasBytes` | none | `cache`: `response` (`hit`, `miss`), `scene` (`built`, `reused`), `weather` |

- Rows: one per element of every sensor mesh, the meshes in the order given (a handle's meshes in its order: an OBJ's
  groups), the elements in mesh order: a face (`Mesh.create`: an entry of `counts`, so a quad is one row; `ObjMesh`,
  `fromThree`, `fromGLB`: a triangle) or, with `--on vertices`, a point. Nothing is reordered or dropped; context
  meshes have no rows. `manifest.meshes[k]` is `{ name, first, count }`: mesh k's rows are `first` to
  `first + count - 1` (`rows()` and `means()` in the examples).
- Columns end in the side: `_front` (`--sides back`: `_back`; `both`: both).

| tool | columns | units |
|---|---|---|
| daylight | `<quantity>_<side>` in `--quantity` order | `radiation`, `direct`, `diffuse` kWh/m2 over the run; `irradiance` W/m2 and `illuminance` lux, means over the evaluated hours; `sunhours` hours |
| views | the shares `sky`, `ground`, `obstruction` and one per target group (sum 1); `distance`, `distance_min`; with `--distance group` `<g>_distance`, `<g>_distance_min`; `reach_<D>` | shares 0..1 of the field's solid angle; distances in metres, from 0.1 to `--far` |

- NaN: never in daylight (a face without area gives 0). In views, `distance` and `distance_min` are NaN when no ray of
  the field is traced (360 mode at `--offset 0` with the whole field behind the element, a face without area), and
  `<g>_distance`, `<g>_distance_min` when group g is not in view. Test with `Number.isNaN`; `JSON.stringify` writes
  NaN as `null`.
- The examples average the faces of a mesh, all of one size there; an area-weighted mean needs the face areas.
- On a three.js mesh: `applyToThree(mesh, values, { vmin, vmax })` with the mesh's rows (a value per triangle: flat
  colours; with `--on vertices` a value per vertex); `colormap(values)` gives the RGB triples alone
  ([results-on-meshes.md](results-on-meshes.md)).

## Speed

The examples on an RTX 2060, and the same calls repeated:

| run | time |
|---|---|
| in-process, the first run in the process (daylight, clear-sky year) | 0.45-0.8 s |
| in-process, daylight again over the same meshes | 0.15 s, of which the clear-sky year's sky model 0.12 s |
| in-process, views over the same meshes | 2-4 ms |
| in-process, daylight sun hours of 21 June over the same meshes | 1 ms |
| process runner, `daylight.exe` (clear-sky year) | 0.8-0.9 s |
| process runner, `views.exe` | 0.6 s |

- The first in-process run pays the GPU start (the CUDA context and the OptiX pipeline, about 0.35 s), once per
  process: the DLL stays loaded until Node exits.
- Keep the `Mesh` objects and pass them again: runs over the same handles in the same roles and order reuse the scene
  on the GPU (`stats.sceneReused`: no triangulation, upload or acceleration-structure build), daylight and views
  alike. The key is the handles, not their contents (a new `Mesh` of the same arrays builds again), with
  `--no-self-occlude` and the `--no-occlude` set; one scene stays resident, so alternating between two sets of meshes
  rebuilds each time.
- Every process-runner call pays the process, the GPU start, the files and the scene, about 0.5 s before tracing:
  fine for one large run, slow for many small ones (parameter studies, sliders).
- Runs go one at a time per process (one GPU), on the module's worker thread: the event loop stays free meanwhile.
- The server keeps meshes by id and `--gpu-scenes` (4) scenes resident, and answers a repeated request from its cache.
- [performance.md](performance.md#in-process-vs-cli) measures the same comparison from Python, a city model included.

## Errors, cancel, progress

The end of `examples/typescript/native.ts` (it prints `error 1: --target tree=green: no mesh of that name`):

```ts
// Errors: DaylightError, status 1 usage (flags, names the meshes do not resolve) or 2 runtime (files, GPU, cancelled).
try {
  await run('views', ['--target', 'tree=green'], { sensors });
} catch (e) {
  if (!(e instanceof DaylightError)) throw e;
  console.log(`error ${e.status}: ${e.message}`);
}
site.free(); // now; else when garbage collected
facade.free();
```

| | in-process | process runner | server |
|---|---|---|---|
| failure | rejects with `DaylightError`: `status` 1 usage (flags, names the meshes do not resolve) or 2 runtime (a file, the GPU, out of memory, `cancelled`), `message` the CLI's | rejects with `Error`: `<exe> failed: <stderr>` | rejects with `Error`: the server's text; a failed job's `status` is 400 usage, 500 runtime, 507 out of memory, 409 cancelled |
| cancel | `signal` (an `AbortController`, `AbortSignal.timeout(ms)`): the run stops at its next check and rejects with 2 `cancelled` | none | `signal`: the job is deleted on the server |
| progress | `onProgress(p)`: the share of sensors traced, 0..1, every 100 ms and once at the end | none | `onStatus(job)`: `state`, queue `position`, `progress`, `eta_s`; `onUpload(done, total)` |

- `Mesh.create`, `Mesh.fromObj` and `Mesh.load` throw an `Error` with the core's message (`facade: vertex index out of
  range`, `cannot open`); `init` throws when the DLL is missing (`no such file (init(path) or DAYLIGHT_DLL)`), has
  another ABI version, or another path is loaded already.
- `mesh.free()` throws while a run uses the mesh; a freed mesh fails a run with 1 (`NULL mesh handle`).

## Reference

- [bindings/ts/README.md](../bindings/ts/README.md): the full API of both entries (`init`, `usage`, `Mesh`, `run`,
  `DaylightError`; `fromThree`, `fromGLB`, `toDmesh`, `toObj`, `dmeshStems`, `run`, `runRemote`, `meshIds`,
  `colormap`, `applyToThree`, `parseNpy`, `parseValues`), the `.dmesh` layout, the tests and timings; `lib/*.d.ts`
  carry the same as doc comments.
- Flags: [daylight.md](daylight.md) and [views.md](views.md) (every flag, output rows, columns, manifest, units);
  `usage('daylight')` and `usage('views')` return the CLIs' `-h` text.
- [c-abi.md](c-abi.md) (the DLL underneath: resident scenes, threads, loading), [input.md](input.md) (meshes, roles,
  culling), [results-on-meshes.md](results-on-meshes.md), [performance.md](performance.md).

## Examples

Every example, in every language, and `examples/run.ps1`, which runs them all: [examples.md](examples.md).

Build the core and run `npm install` in `bindings/ts` first; run them from the repo root. Each exits 0 and prints the
output shown above (times vary). The `.ts` files import the sources with their extensions and use erasable syntax only,
so Node runs them without a build step.

| file | shows | run |
|---|---|---|
| [examples/typescript/native.ts](../examples/typescript/native.ts) | in-process: `Mesh.load`, `Mesh.create`, daylight (clear-sky year) and views (a target) on one resident scene, rows per mesh, progress, a timeout, an error | `node examples/typescript/native.ts` (Node >= 22.18) |
| [examples/javascript/native.mjs](../examples/javascript/native.mjs) | the same in plain JavaScript, from `bindings/ts/lib` | `node examples/javascript/native.mjs` (Node >= 20) |
| [examples/typescript/cli.ts](../examples/typescript/cli.ts) | the process runner: `daylight.exe` and `views.exe` on `ObjMesh`es, `--target` by `dmeshStems`, `--reach en17037` | `node examples/typescript/cli.ts` (Node >= 22.18) |
