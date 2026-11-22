# daylight-mesh (TypeScript and JavaScript binding)

Guide: [docs/javascript.md](../../docs/javascript.md) (setup, both tools worked through, results, speed, errors);
examples: [examples/typescript](../../examples/typescript), [examples/javascript](../../examples/javascript)
([docs/examples.md](../../docs/examples.md)).

Turns three.js scenes or `.glb` files into the raw `.dmesh` arrays (or OBJ) the core reads, and runs both its tools,
daylight and views: from Node through `daylight.exe` / `views.exe` (`run`) or in-process through `daylight_c.dll`
(`daylight-mesh/native`); daylight also from a browser, through a `daylight_server` (`runRemote`; a browser cannot run
the GPU).

One package for TypeScript and plain JavaScript: `src/*.ts` compiles (`npm run build`) to the committed `lib/*.js` with
`lib/*.d.ts` beside them, and `package.json` `exports` points `daylight-mesh` at `lib/index.js` and `daylight-mesh/native`
at `lib/native.js` (ES modules). Node and bundlers import them as they are; a browser without a bundler needs
`@gltf-transform/core` resolvable, e.g. an import map:

```html
<script type="importmap">{ "imports": { "@gltf-transform/core": "https://esm.sh/@gltf-transform/core@4" } }</script>
<script type="module">import { fromThree, runRemote } from './node_modules/daylight-mesh/lib/index.js';</script>
```

(`three` is imported lazily, by `applyToThree` only.) The web app imports `src/index.ts` directly; it stays browser-safe
and never imports `native`.

Node (three.js scene -> values):

```ts
import { fromThree, run } from 'daylight-mesh';

const sensors = fromThree([scene]);                 // every descendant three.js Mesh, world metres, Y-up -> Z-up
const { data, columns } = await run('daylight.exe', { sensors, args: ['--epw', 'x.epw'] });
console.log(columns[0], data[0]);                   // one Float64Array per column, a value per sensor element
```

glb:

```ts
import { fromGLB, run } from 'daylight-mesh';
import fs from 'node:fs/promises';

const sensors = await fromGLB(await fs.readFile('a.glb'));            // { scale } multiplies metres
const context = await fromGLB(await fs.readFile('city.glb'));
const { data } = await run('daylight.exe', { sensors, context, args: ['--epw', 'x.epw', '--quantity', 'radiation'] });
```

Views (`views.exe`, [docs/views.md](../../docs/views.md)): the same runner, another exe and its flags. Mesh names reach
the core as `dmeshStems(names)` (sanitised to `[A-Za-z0-9._-]`, unique), so `--target`, `--optics` and `--no-occlude`
name them so:

```ts
import { dmeshStems, fromGLB, run } from 'daylight-mesh';
import fs from 'node:fs/promises';

const sensors = await fromGLB(await fs.readFile('facades.glb')), context = await fromGLB(await fs.readFile('city.glb'));
const stems = dmeshStems([...sensors, ...context].map((m) => m.name)); // the core's names: 'park/lawn' -> 'park_lawn'
const lawn = stems[sensors.length + context.findIndex((m) => m.name === 'park/lawn')];
const { data, columns } = await run('views.exe', { sensors, context, args: ['--fov', '360', '--target', `${lawn}=green`] });
// columns: sky_front, ground_front, obstruction_front, ground_obstruction_front, green_front, volume_front (all around:
// the isovist; docs/views.md)
```

In-process (Node only, `daylight-mesh/native`): `daylight_c.dll` runs both tools in this process, so there are no files
and no process start, the GPU context lives as long as the process, and runs over the same mesh handles keep their
scene on the GPU ([docs/c-abi.md](../../docs/c-abi.md)). Needs `koffi` 3 (an optional peer dependency:
`npm install koffi`) and the DLL: `init(path)`, else env `DAYLIGHT_DLL`, else `<repo>/build/RelWithDebInfo/daylight_c.dll`.

```ts
import { fromThree } from 'daylight-mesh';
import { SimulationMesh, run } from 'daylight-mesh/native';

const sensors = fromThree([scene]).map(SimulationMesh.fromObj);                // copied once; names as given (SimulationMesh.names)
const ctrl = new AbortController();
const r = await run('views', ['--fov', '360'], { sensors }, { signal: ctrl.signal, onProgress: (p) => bar(p) });
const again = await run('daylight', ['--epw', 'x.epw', '--quantity', 'radiation'], { sensors }); // same scene, stats.sceneReused
for (const m of sensors) m.free();                                   // else freed when garbage collected
```

Browser (or Node) against a `daylight_server` (the GPU box runs the core in-process; meshes go up once under their
content's SHA-256 and are referenced by it afterwards, so a re-run with other flags, or a reload of the same model,
sends no geometry):

```ts
import { fromThree, runRemote } from 'daylight-mesh';

const sensors = fromThree([scene]);                                   // ids: the bytes' SHA-256 (or set a stable `id`)
const { data, columns, cache } = await runRemote('http://gpu-box:8080', { sensors, epw: epwFile, args: ['--quantity', 'radiation,sunhours'] });
// data[c][element]: one Float32Array per column, as run() returns them (there in float64)
```

Colour a three.js mesh by its results (`applyToThree`):

```ts
import { applyToThree, fromThree, run } from 'daylight-mesh';

const { data, columns, manifest } = await run('daylight.exe', { sensors: fromThree([mesh]), args: ['--epw', 'x.epw'] });
const { first, count } = (manifest as { meshes: { name: string; first: number; count: number }[] }).meshes[0]; // rows of this mesh
const col = columns.indexOf('radiation_front'); // columns are <quantity>_<side>
await applyToThree(mesh, data[col].subarray(first, first + count)); // one value per triangle -> flat face colours
```

## API

- `colormap(values, { vmin, vmax, stops })`: viridis (9 stops, linear) -> `Float32Array` RGB triples; range defaults to the
  finite min/max of `values` (`vmax == vmin` -> first stop); non-finite values -> grey `[0.5, 0.5, 0.5]`; `stops` =
  any evenly spaced RGB list for another gradient. Default identical to `daylight.colormap` in the Python binding.
- `applyToThree(mesh, values, { vmin, vmax, stops, rgb, material })`: `values.length` must be the triangle count (per-face: the
  geometry is converted to non-indexed via `toNonIndexed()` so each face gets one flat colour) or the vertex count
  (per-vertex, geometry unchanged). Colours from `colormap()` or from `rgb` (3 floats per value). Sets the `color`
  attribute and, unless `material: false`, `mesh.material = new MeshStandardMaterial({ vertexColors: true })`.
  Imports `three` lazily; returns the RGB array.
- `ObjMesh { name, positions /* xyz, metres, Z-up */, indices /* 0-based triangles */, slots?, id? }`. `slots`: optional
  `Uint16Array`, one optics slot per triangle (the core's `SimulationMesh::slot`; absent = every triangle slot 0), written by
  `toDmesh` (so by `run` and `runRemote`: part of the bytes the server caches under `id`); `toObj` drops it.
- `fromThree(objects, { scale })`: `updateMatrixWorld(true)`, applies `matrixWorld`, converts `(x, y, z) -> (x, -z, y)`,
  reverses winding when `det(matrixWorld) < 0`; names each mesh by its `name`, else its `uuid`. Triangle
  `BufferGeometry` only (indexed or not); no strips, fans or lines.
- `fromGLB(data, { scale })`: via `@gltf-transform/core`; `TRIANGLES` primitives of every node with a mesh,
  name `node/mesh`. Same frame conversion and winding rule.
- `toDmesh(mesh, { matrix?, scale? })`: one mesh -> `Uint8Array` in the `.dmesh` layout below (flags 1 + the slot block
  when `mesh.slots` is set, else flags 0); throws on bad indices or a `slots` length other than the triangle count.
  With `matrix` (column-major 4x4) the positions are local three.js ones and take fromThree's conversion: the same bytes
  as `toDmesh(fromThree(...)[0])` without the Float64 copy; `indices` may be absent (a triangle soup). With
  `mesh.counts` (the corners of each face, 3 or more, summing to the indices) the faces are written as the polygons they
  are, so a quad is one receiver face and per-vertex receivers follow the points its faces share; `slots` then holds one
  value per face, and a mirrored matrix turns each face around its first corner.
- `meshIds({ sensors, context?, signal? })`: a job's mesh ids, sensors then context: a mesh's `id` if set, else the
  SHA-256 hex of its `.dmesh` bytes (8 bodies at a time; a named mesh's name folded in, since the server keeps one name
  per id); a context id equal to a sensor's is salted (`sha256(id \0 ctx)`: the server refuses a mesh in both roles).
- `toObj(meshes)`: one `o` group per mesh, 1-based global indices; throws on bad indices (interop with other OBJ tools).
- `run(exe, { sensors, context?, args })`: `exe` is `daylight.exe` or `views.exe`; temp dir,
  `exe <sensors...> [--context <file>]... ...args --out out.npy`, returns `{ data: Float64Array[] (one per column:
  data[c][element] for columns[c]), columns, manifest }`; rejects with the core's stderr.
  `sensors`/`context` accept `ObjMesh[]` (written as one `<name>.dmesh` per mesh, name sanitised to `[A-Za-z0-9._-]`
  and made unique: `dmeshStems` of the sensors' then the context's names, which `--target`, `--optics` and
  `--no-occlude` must use; the manifest's mesh names are the file stems) or OBJ text (written as one `.obj`).
  Needs Node >= 20 and the GPU build of the core.
- `runRemote(url, { sensors, context?, epw?, args?, signal?, clientId?, onStatus?, onUpload? })`: `sensors`/`context`
  are `ObjMesh`es or `{ id?, name?, dmesh: () => bytes }` (bodies built on demand). Submits a job (`POST url/jobs`)
  with their ids (meshIds; nothing written back): one `POST url/meshes/missing` for the ids this process has not seen
  on that server (404 there: all missing), `PUT url/mesh/<id>?name=<stem>` 6 at a time for the missing ones (each
  body built just before its PUT; `onUpload(done, total)`); an unnamed mesh's core name is its id (repeats `id_2`, ...:
  `dmeshStems(ids)`, which `--optics` / `--no-occlude` rows must use); a 404 from the job (the server lost some) puts
  those again before one retry; long-polls `GET url/jobs/<id>?wait=25` (`onStatus` sees each
  queued / running state, with the queue position); aborting `signal` cancels the job on the server; `clientId` is
  sent as `X-Client-Id` for the per-client job limit; `epw` is a `Blob` (uploaded) or `{ url }` (fetched by the server
  through its allowlisted proxy cache); once the job is done, reads `GET url/jobs/<id>/values` (float32
  little-endian, column-major) into one buffer of `4 x rows x columns` bytes (rejects with `values: X of Y bytes` on
  any other length) and returns `{ data: Float32Array[] (one view of it per column, as run()'s data), columns,
  manifest, cache }`; `manifest` is the job's JSON (`values_b64` dropped, never decoded). While the job runs,
  `onStatus` also sees `progress` (0..1) and `eta_s` when the server reports them; a failed job's `status` is 400
  usage, 500 runtime or 507 out of memory (409: cancelled). Browser-safe. The full API is self-documented by the
  server at `/docs` and `/openapi.json`.
- `dmeshStems(names, fallback?)`: the stem / mesh-name sanitiser both runners use (`[^A-Za-z0-9._-]` -> `_`, unique
  case-insensitively).
- `parseNpy(bytes, cols)`: parses a core `--out FILE.npy` (NumPy v1.0 `<f8`, C order) into `cols` columns
  (`Float64Array` each).
- `parseValues(text)`: parses a core `--out FILE` text file into columns the same way.

Browser bundles use `toDmesh`, `toObj`, `fromThree`, `fromGLB`, `colormap`, `applyToThree`, `meshIds`, `runRemote` (`meshIds` needs `crypto.subtle`: a secure context such as localhost or https); `run` imports Node builtins lazily and must stay on Node.

### `daylight-mesh/native` (Node only)

`daylight_c.dll` through [koffi](https://koffi.dev) 3 (pointers are `BigInt`; `int64` arguments take numbers, `int64`
results come back as numbers, `BigInt` only beyond 2^53; strings UTF-8). Windows x64, the NVIDIA driver and the
Visual C++ runtime ([docs/c-abi.md](../../docs/c-abi.md#loading-the-dll)).

- `init(dll?)`: loads the DLL once per process: `dll`, else env `DAYLIGHT_DLL`, else
  `<repo>/build/RelWithDebInfo/daylight_c.dll` (found from `src/` or `lib/`); the calls below call it without a path.
  Throws when it does not load (a missing file: `no such file`), has another ABI version, or another path is loaded
  already. Kept loaded while Node runs (koffi frees it as Node exits).
- `usage(tool)`: `'daylight' | 'views'` -> the CLI's `-h` text.
- `SimulationMesh.create(name, positions, counts, indices, slots?)`: the `.dmesh` arrays (xyz, vertices per face >= 3, 0-based
  indices, a slot per face), copied; `SimulationMesh.fromObj(objMesh)`: triangles (counts all 3), its `name` and `slots` as they
  are (not through `dmeshStems`); `SimulationMesh.load(path)`: a `.obj` (a mesh per group) or `.dmesh`, as the CLI reads it
  (ASCII paths). `mesh.names`: what `--target`, `--optics`, `--no-occlude` and the manifest call its meshes.
  `mesh.free()` frees it now (throws while a run uses it); else a `FinalizationRegistry` frees it when collected.
  These throw an `Error` with the core's message (`bad: vertex index out of range`, `cannot open`, ...).
- `run(tool, args, { sensors, context? }, { signal?, onProgress? })`: the CLI's flags without the program name (no mesh
  paths, `--context`, `--out`, `--bench`, `-h`; paths in flags, such as `--epw`, ASCII as for `SimulationMesh.load`), handles in
  place of its mesh arguments and `--context` files. Resolves
  `{ data: Float64Array[] (columns, as run(exe)), columns, manifest, manifestText (FILE.json, LF), stats: { sceneReused,
  prepMs, skyMs, buildMs, traceMs, reduceMs, bounceMs, rays, launches, gasBytes } }`, values and manifest bit for bit
  the CLI's. Runs over the same handles in the same roles and order keep the scene on the GPU (`sceneReused`); daylight
  and views share it. Rejects with a `DaylightError` (`status` 1 usage, 2 runtime, the CLI's message). Aborting
  `signal` cancels the run at its next check (rejects 2 `cancelled`); `onProgress(p)` sees the share of sensors traced,
  polled every 100 ms and at the end (1).

Runs go one at a time on one worker thread that this module starts (unref'd while idle), so the event loop stays free
during the GPU work. The run and `dl_last_error` must share a thread (the message is per thread), which koffi's
`.async` (libuv's pool) does not guarantee; the worker gets the handles as `BigInt` addresses. The worker runs this
module's own file (`import.meta.url`), so keep `daylight-mesh/native` out of bundles (mark it and `koffi` external).

## Performance (2M triangles, RTX 2060)

1000x1000 grid as one glb (1 002 001 Float32 positions, 2M Uint32-indexed triangles), best of 2:

| step | ms |
| --- | --- |
| `fromGLB(bytes)` | 43 |
| `toDmesh(mesh)` | 32 |
| `fs.writeFileSync(.dmesh)` (56 MB) | 181 |
| `run()` end to end (write, core sunhours one timestep, read `.npy`) | 1724 |

`run()` reads the values back from `--out out.npy` (straight from the raw bytes into one `Float64Array` per column)
instead of text: `parseValues` on a 1M-row text file takes ~0.9 s, the `.npy` read is I/O bound. `toObj` (text, ~1.5 s)
stays interop only.

## The `.dmesh` format

One mesh per file, little-endian, no library semantics. Mesh name = file stem.

```text
offset  0  char[4]   "DLM1"                 magic + version
offset  4  uint32    flags                  bit 0: slot block present; other bits must be 0 (keeps the arrays 8-byte aligned)
offset  8  uint64    n_points
offset 16  uint64    n_faces
offset 24  uint64    n_indices
offset 32  float64   xyz[3 * n_points]      world metres, Z-up
           int32     counts[n_faces]        vertices per face (>= 3), right-handed winding; toDmesh writes 3 unless given counts
           int32     indices[n_indices]     0-based, sum(counts) == n_indices
           uint16    slot[n_faces]          only with flags bit 0: optics slot of each face (ObjMesh.slots)
```

The core rejects other flag bits and a short slot block ([docs/input.md](../../docs/input.md)).

```sh
npm install && npm run build && npm test     # build: src -> lib (committed; test/lib.test.ts fails when lib is stale)
B=../../build/RelWithDebInfo
DAYLIGHT_EXE=$B/daylight.exe VIEWS_EXE=$B/views.exe DAYLIGHT_DLL=$B/daylight_c.dll npm test
```

`DAYLIGHT_EXE` runs the box through `daylight.exe` via `.dmesh` (12 rows); `DAYLIGHT_DLL` (or the default build) runs
test/native.test.ts, the cases of the C test (`tests/c_abi.c`): both tools on `tests/box.obj` equal to the CLI smokes
`build/smoke.npy` and `build/views_smoke.npy` (ctest writes them) byte for byte, resident scenes, cancel, progress;
`VIEWS_EXE` also compares `run('views.exe')` with the in-process run. Tests from `src/` start the run thread from
`src/native.ts`: Node >= 22.18 (type stripping); `lib/` needs Node >= 20.
