# daylight-mesh (TypeScript binding)

Turns three.js scenes or `.glb` files into the raw `.dmesh` arrays (or OBJ) the daylight core CLI reads, and (Node only) runs the core.

Node (three.js scene -> values):

```ts
import { fromThree, run } from 'daylight-mesh';

const sensors = fromThree([scene]);                 // every descendant Mesh, world metres, Y-up -> Z-up
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
  `Uint16Array`, one optics slot per triangle (the core's `Mesh::slot`; absent = every triangle slot 0), written by
  `toDmesh` (so by `run` and `runRemote`: part of the bytes the server caches under `id`); `toObj` drops it.
- `fromThree(objects, { scale })`: `updateMatrixWorld(true)`, applies `matrixWorld`, converts `(x, y, z) -> (x, -z, y)`,
  reverses winding when `det(matrixWorld) < 0`. Triangle `BufferGeometry` only (indexed or not); no strips, fans or lines.
- `fromGLB(data, { scale })`: via `@gltf-transform/core`; `TRIANGLES` primitives of every node with a mesh,
  name `node/mesh`. Same frame conversion and winding rule.
- `toDmesh(mesh, { matrix?, scale? })`: one mesh -> `Uint8Array` in the `.dmesh` layout below (flags 1 + the slot block
  when `mesh.slots` is set, else flags 0); throws on bad indices or a `slots` length other than the triangle count.
  With `matrix` (column-major 4x4) the positions are local three.js ones and take fromThree's conversion: the same bytes
  as `toDmesh(fromThree(...)[0])` without the Float64 copy; `indices` may be absent (a triangle soup).
- `meshIds({ sensors, context?, signal? })`: a job's mesh ids, sensors then context: a mesh's `id` if set, else the
  SHA-256 hex of its `.dmesh` bytes (8 bodies at a time; a named mesh's name folded in, since the server keeps one name
  per id); a context id equal to a sensor's is salted (`sha256(id \0 ctx)`: the server refuses a mesh in both roles).
- `toObj(meshes)`: one `o` group per mesh, 1-based global indices; throws on bad indices (interop with other OBJ tools).
- `run(exe, { sensors, context?, args })`: temp dir, `exe <sensors...> [--context <file>]... ...args --out out.npy`,
  returns `{ data: Float64Array[] (one per column: data[c][element] for columns[c]), columns, manifest }`; rejects
  with the core's stderr.
  `sensors`/`context` accept `ObjMesh[]` (written as one `<name>.dmesh` per mesh, name sanitised to `[A-Za-z0-9._-]`
  and made unique; the manifest's mesh names are the file stems) or OBJ text (written as one `.obj`).
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
           int32     counts[n_faces]        vertices per face (>= 3), right-handed winding; toDmesh writes 3
           int32     indices[n_indices]     0-based, sum(counts) == n_indices
           uint16    slot[n_faces]          only with flags bit 0: optics slot of each face (ObjMesh.slots)
```

The core rejects other flag bits and a short slot block ([docs/input.md](../../docs/input.md)).

```sh
npm install && npm run build && npm test
DAYLIGHT_EXE=../../build/RelWithDebInfo/daylight.exe npm test    # also runs the box through the core via .dmesh (12 rows)
```
