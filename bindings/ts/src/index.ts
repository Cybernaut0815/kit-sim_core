// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// daylight-mesh: three.js / glTF meshes -> .dmesh / OBJ for the daylight core CLI, plus a Node runner.
// Core contract: world metres, Z-up, right-handed winding; .dmesh = one mesh per file, OBJ = one `o` group per mesh.
import { NodeIO, Primitive } from '@gltf-transform/core';
import type { Mesh, Object3D } from 'three';

export interface ObjMesh {
  name: string;
  positions: ArrayLike<number>; // xyz interleaved, world metres, Z-up
  indices: ArrayLike<number>; // triangle indices, 0-based
  slots?: Uint16Array; // optics slot per triangle (core Mesh::slot; toDmesh writes it, toObj does not); absent = all slot 0
  id?: string; // runRemote(): the server caches the mesh under it; absent: meshIds() derives it from the bytes
}

/**
 * `.dmesh` file stems (= core mesh names) for `names`: sanitised to `[A-Za-z0-9._-]`, empty -> `fallback`, made unique
 * case-insensitively (`_2`, `_3`, ... suffixes; Windows/macOS file systems ignore case). Shared by run() and runRemote().
 */
export function dmeshStems(names: ArrayLike<string>, fallback = 'mesh'): string[] {
  const used = new Set<string>(), out: string[] = [];
  for (let k = 0; k < names.length; k++) {
    const base = names[k].replace(/[^A-Za-z0-9._-]/g, '_') || fallback;
    let stem = base;
    for (let i = 2; used.has(stem.toLowerCase()); i++) stem = `${base}_${i}`;
    used.add(stem.toLowerCase());
    out.push(stem);
  }
  return out;
}

export function toObj(meshes: ObjMesh[]): string {
  let out = '';
  let base = 0;
  for (const m of meshes) {
    const nv = m.positions.length / 3;
    if (!Number.isInteger(nv) || m.indices.length % 3 !== 0) throw new Error(`${m.name}: positions must be xyz triples, indices triangle triples`);
    out += `o ${m.name}\n`;
    for (let i = 0; i < m.positions.length; i += 3) out += `v ${m.positions[i]} ${m.positions[i + 1]} ${m.positions[i + 2]}\n`;
    for (let i = 0; i < m.indices.length; i += 3) {
      const a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
      if (!(a >= 0 && a < nv && b >= 0 && b < nv && c >= 0 && c < nv)) throw new Error(`${m.name}: index out of range in face ${i / 3}`);
      out += `f ${base + a + 1} ${base + b + 1} ${base + c + 1}\n`;
    }
    base += nv;
  }
  return out;
}

/**
 * One mesh -> `.dmesh` bytes (little-endian): "DLM1", uint32 flags, uint64 nPoints/nFaces/nIndices,
 * float64 xyz[3*nPoints], int32 counts[nFaces] (all 3), int32 indices[nIndices], then with `slots` (flags 1, else 0)
 * uint16 slot[nFaces]. Mesh name = file stem. With `matrix` the positions are local (three.js, Y-up) and take fromThree's
 * conversion (world by the column-major matrix, times `scale`, Z-up, mirrored winding reversed): the same bytes as
 * toDmesh(fromThree(...)), without the Float64 copy. `indices` absent: a triangle soup.
 */
export function toDmesh(mesh: { name?: string; positions: ArrayLike<number>; indices?: ArrayLike<number> | null; slots?: Uint16Array },
  opts: { matrix?: ArrayLike<number>; scale?: number } = {}): Uint8Array<ArrayBuffer> {
  const P = mesh.positions, I = mesh.indices, N = P.length / 3, M = I ? I.length : N, F = M / 3, S = mesh.slots, name = mesh.name ?? 'mesh';
  if (!Number.isInteger(N) || !Number.isInteger(F)) throw new Error(`${name}: positions must be xyz triples, indices triangle triples`);
  if (S && S.length !== F) throw new Error(`${name}: slots must hold one value per triangle (${F}), not ${S.length}`);
  const buf = new ArrayBuffer(32 + 24 * N + 4 * F + 4 * M + (S ? 2 * F : 0));
  const dv = new DataView(buf);
  new Uint8Array(buf, 0, 4).set([0x44, 0x4c, 0x4d, 0x31]); // "DLM1"
  dv.setUint32(4, S ? 1 : 0, true); // flags: bit 0 = slot block
  dv.setBigUint64(8, BigInt(N), true);
  dv.setBigUint64(16, BigInt(F), true);
  dv.setBigUint64(24, BigInt(M), true);
  const xyz = new Float64Array(buf, 32, 3 * N);
  if (opts.matrix) place(P, opts.matrix, opts.scale ?? 1, xyz);
  else xyz.set(P);
  new Int32Array(buf, 32 + 24 * N, F).fill(3);
  const idx = new Int32Array(buf, 32 + 24 * N + 4 * F, M);
  for (let i = 0; i < M; i++) {
    const v = I ? I[i] : i;
    if (!(Number.isInteger(v) && v >= 0 && v < N)) throw new Error(`${name}: index out of range in face ${Math.floor(i / 3)}`);
    idx[i] = v;
  }
  if (opts.matrix && mirrored(opts.matrix)) for (let i = 0; i < M; i += 3) { const t = idx[i + 1]; idx[i + 1] = idx[i + 2]; idx[i + 2] = t; }
  if (S) new Uint16Array(buf, 32 + 24 * N + 4 * F + 4 * M, F).set(S);
  return new Uint8Array(buf);
}

// Column-major 4x4 affine `m` (three.js and glTF agree) -> world, * scale, Y-up -> Z-up as (x, -z, y);
// winding reversed when det(m) < 0 (mirrored transform) so the OBJ stays right-handed.
function place(pos: ArrayLike<number>, m: ArrayLike<number>, scale: number, out: Float64Array): void {
  for (let i = 0; i < pos.length; i += 3) {
    const x = pos[i], y = pos[i + 1], z = pos[i + 2];
    const wx = m[0] * x + m[4] * y + m[8] * z + m[12];
    const wy = m[1] * x + m[5] * y + m[9] * z + m[13];
    const wz = m[2] * x + m[6] * y + m[10] * z + m[14];
    out[i] = wx * scale;
    out[i + 1] = -wz * scale;
    out[i + 2] = wy * scale;
  }
}
const mirrored = (m: ArrayLike<number>) => m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]) < 0;
function toMesh(name: string, pos: ArrayLike<number>, idx: ArrayLike<number> | null, m: ArrayLike<number>, scale: number): ObjMesh {
  const positions = new Float64Array(pos.length);
  place(pos, m, scale, positions);
  const indices = idx ? Uint32Array.from(idx) : Uint32Array.from({ length: pos.length / 3 }, (_, i) => i);
  if (mirrored(m)) for (let i = 0; i < indices.length; i += 3) { const t = indices[i + 1]; indices[i + 1] = indices[i + 2]; indices[i + 2] = t; }
  return { name, positions, indices };
}

/** Every descendant Mesh of `objects` (triangle BufferGeometry only; strips/fans/lines are not supported). */
export function fromThree(objects: Object3D[], opts: { scale?: number } = {}): ObjMesh[] {
  const out: ObjMesh[] = [];
  for (const root of objects) {
    root.updateMatrixWorld(true);
    root.traverse((o) => {
      const mesh = o as Mesh;
      if (!mesh.isMesh) return;
      const pos = mesh.geometry.attributes.position;
      if (!pos || pos.itemSize !== 3) throw new Error(`${o.name || o.uuid}: geometry has no xyz position attribute`);
      out.push(toMesh(o.name || o.uuid, pos.array, mesh.geometry.index?.array ?? null, mesh.matrixWorld.elements, opts.scale ?? 1));
    });
  }
  return out;
}

/** All TRIANGLES primitives of every node with a mesh in a .glb; name = node name + '/' + mesh name (or primitive index). */
export async function fromGLB(data: ArrayBuffer | Uint8Array, opts: { scale?: number } = {}): Promise<ObjMesh[]> {
  const doc = await new NodeIO().readBinary(new Uint8Array(data));
  const out: ObjMesh[] = [];
  for (const node of doc.getRoot().listNodes()) {
    const mesh = node.getMesh();
    if (!mesh) continue;
    const m = node.getWorldMatrix();
    mesh.listPrimitives().forEach((prim, i) => {
      if (prim.getMode() !== Primitive.Mode.TRIANGLES) return;
      const pos = prim.getAttribute('POSITION')?.getArray();
      if (!pos) return;
      const name = `${node.getName()}/${mesh.getName() || i}`;
      out.push(toMesh(name, pos, prim.getIndices()?.getArray() ?? null, m, opts.scale ?? 1));
    });
  }
  return out;
}

/** Core `--out FILE` text file -> one Float64Array per column, `data[c][element]` (blank lines skipped). */
export function parseValues(text: string): Float64Array[] {
  const rows = text.split('\n').filter((l) => l.trim()).map((l) => l.trim().split(/\s+/));
  return (rows[0] ?? []).map((_, c) => Float64Array.from(rows, (r) => Number(r[c])));
}

/** Core `--out FILE.npy` (NumPy v1.0, `<f8`, C order [N x cols]) -> one Float64Array per column, `data[c][element]`. */
export function parseNpy(bytes: Uint8Array, cols: number): Float64Array[] {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (dv.getUint8(0) !== 0x93 || dv.getUint8(6) !== 1) throw new Error('not a NumPy v1.0 .npy file');
  if (!(cols >= 1)) throw new RangeError('cols must be >= 1');
  const start = 10 + dv.getUint16(8, true), rows = Math.floor((bytes.byteLength - start) / (8 * cols)); // a partial last row is dropped
  const data = Array.from({ length: cols }, () => new Float64Array(rows));
  // Transposed while read: DataView takes any byteOffset (an fs.readFile Buffer is a view into a pool), so no aligned copy of the file.
  for (let i = 0, at = start; i < rows; i++) for (let c = 0; c < cols; c++, at += 8) data[c][i] = dv.getFloat64(at, true);
  return data;
}

// viridis at t = 0, 0.125, ..., 1 (matplotlib); shared with bindings/python.
const VIRIDIS = [[0.267004, 0.004874, 0.329415], [0.278826, 0.175490, 0.483397], [0.229739, 0.322361, 0.545706], [0.172719, 0.448791, 0.557885], [0.127568, 0.566949, 0.550556], [0.157851, 0.683765, 0.501686], [0.369214, 0.788888, 0.382914], [0.678489, 0.863742, 0.189503], [0.993248, 0.906157, 0.143936]];

/**
 * Values -> RGB triples (Float32Array, 3 per value) over [vmin, vmax] (default: finite min/max); non-finite -> grey.
 * `stops`: evenly spaced RGB stops (0..1) of any other gradient, default viridis.
 */
export function colormap(values: ArrayLike<number>, opts: { vmin?: number; vmax?: number; stops?: ArrayLike<ArrayLike<number>> } = {}): Float32Array {
  const stops = opts.stops ?? VIRIDIS, n = stops.length - 1;
  if (n < 1) throw new RangeError('stops needs at least two colours');
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < values.length; i++) { const v = values[i]; if (Number.isFinite(v)) { if (v < lo) lo = v; if (v > hi) hi = v; } }
  const vmin = opts.vmin ?? lo, span = (opts.vmax ?? hi) - vmin;
  const rgb = new Float32Array(3 * values.length);
  for (let i = 0; i < values.length; i++) {
    const v = values[i];
    if (!Number.isFinite(v)) { rgb.fill(0.5, 3 * i, 3 * i + 3); continue; }
    const t = span ? Math.min(Math.max((v - vmin) / span, 0), 1) : 0;
    const k = Math.min(Math.floor(t * n), n - 1), f = t * n - k, a = stops[k], b = stops[k + 1];
    for (let c = 0; c < 3; c++) rgb[3 * i + c] = a[c] + f * (b[c] - a[c]);
  }
  return rgb;
}

/**
 * Colours `mesh` by `values`: one per triangle (geometry becomes non-indexed so each face gets a flat colour) or one per
 * vertex. Sets the `color` attribute and, unless `material: false`, a `MeshStandardMaterial({ vertexColors: true })`.
 * Colours come from colormap() (`vmin`, `vmax`, `stops`) or, with `rgb` (3 floats per value), from the caller.
 * Imports three lazily (peer dependency). Returns the RGB attribute array.
 */
export async function applyToThree(mesh: Mesh, values: ArrayLike<number>, opts: { vmin?: number; vmax?: number; stops?: ArrayLike<ArrayLike<number>>; rgb?: Float32Array; material?: boolean } = {}): Promise<Float32Array> {
  const { BufferAttribute, MeshStandardMaterial } = await import('three');
  let geometry = mesh.geometry;
  const triangles = (geometry.index ? geometry.index.count : geometry.attributes.position.count) / 3;
  const perFace = values.length === triangles;
  if (!perFace && values.length !== geometry.attributes.position.count) throw new Error('values.length must be the triangle count (N) or the vertex count (M)');
  if (opts.rgb && opts.rgb.length !== 3 * values.length) throw new Error('rgb must hold 3 floats per value');
  let rgb = opts.rgb ?? colormap(values, opts);
  if (perFace) {
    if (geometry.index) geometry = mesh.geometry = geometry.toNonIndexed();
    const face = rgb;
    rgb = new Float32Array(9 * triangles);
    for (let i = 0; i < rgb.length; i++) rgb[i] = face[3 * Math.floor(i / 9) + (i % 3)];
  }
  geometry.setAttribute('color', new BufferAttribute(rgb, 3));
  if (opts.material !== false) mesh.material = new MeshStandardMaterial({ vertexColors: true });
  return rgb;
}

/**
 * Node only: writes the inputs into a temp dir, runs `exe <sensor files> [--context <file>]... ...args --out out.npy`,
 * returns the values as columns, `data[c][element]` for `columns[c]` (the core's doubles; runRemote has the same shape
 * in float32), and the `out.npy.json` manifest. A string input is OBJ text (one .obj); an ObjMesh[] becomes
 * one `<name>.dmesh` per mesh via toDmesh() (name sanitised to [A-Za-z0-9._-], made unique).
 */
export async function run(exe: string, input: { sensors: string | ObjMesh[]; context?: string | ObjMesh[]; args?: string[] }): Promise<{ data: Float64Array[]; columns: string[]; manifest: unknown }> {
  const fs = await import('node:fs/promises'); // dynamic so the browser-safe functions above bundle without node builtins
  const { tmpdir } = await import('node:os');
  const { join } = await import('node:path');
  const { execFile } = await import('node:child_process');
  const dir = await fs.mkdtemp(join(tmpdir(), 'daylight-'));
  try {
    const sens = typeof input.sensors === 'string' ? [] : input.sensors;
    const ctx = !input.context || typeof input.context === 'string' ? [] : input.context;
    const stems = dmeshStems([...sens.map((m) => m.name || 'sensors'), ...ctx.map((m) => m.name || 'context')]);
    const write = async (kind: string, m: string | ObjMesh[], first: number) => {
      if (typeof m === 'string') { const p = join(dir, kind + '.obj'); await fs.writeFile(p, m); return [p]; }
      const paths: string[] = [];
      for (let k = 0; k < m.length; k++) {
        paths.push(join(dir, stems[first + k] + '.dmesh'));
        await fs.writeFile(paths[paths.length - 1], toDmesh(m[k]));
      }
      return paths;
    };
    const out = join(dir, 'out.npy');
    const args = await write('sensors', input.sensors, 0);
    if (input.context) for (const p of await write('context', input.context, sens.length)) args.push('--context', p);
    args.push(...(input.args ?? []), '--out', out);
    await new Promise<void>((res, rej) => execFile(exe, args, { maxBuffer: 1 << 26 }, (e, _o, stderr) => (e ? rej(new Error(`${exe} failed: ${stderr.trim() || e.message}`)) : res())));
    const manifest = JSON.parse(await fs.readFile(out + '.json', 'utf8')) as { columns: string[] };
    return { data: parseNpy(await fs.readFile(out), manifest.columns.length), columns: manifest.columns, manifest };
  } finally {
    await fs.rm(dir, { recursive: true, force: true });
  }
}

/** The core's `FILE.json` manifest as `daylight_server` returns it (plus the server's own fields). */
export interface RemoteManifest {
  columns: string[];
  duration_h: number;
  dirs: { sky: number; ground: number; sun: number };
  meshes: { name: string; first: number; count: number }[];
  run_s?: number;
  cache?: Record<string, string>;
  // where GET reads the values (runRemote does): float32 LE, column 0's rows, then column 1's, ...
  values?: { url: string; dtype: '<f4'; order: 'column-major'; shape: [cols: number, rows: number]; bytes: number };
}

/** A job on `daylight_server` (`/jobs/{id}`): position counts the jobs ahead of a queued one. */
export interface RemoteJob {
  id: string;
  state: 'queued' | 'running' | 'done' | 'error' | 'cancelled';
  position?: number;
  error?: string;  // state error / cancelled: the server's message
  status?: number; // state error / cancelled: the HTTP status POST /run would have answered (400 usage, 500 runtime, 507 out of memory, 409 cancelled)
  progress?: number; // state running, when the server reports it: the share done, 0..1
  eta_s?: number;    // state running, when the server reports it: the seconds still to go
}

/** A mesh for runRemote(): an ObjMesh, or one whose .dmesh bytes are built on demand (each body dropped once hashed or sent). */
export type RemoteMesh = ObjMesh | { id?: string; name?: string; dmesh: () => Uint8Array<ArrayBuffer> };
const bytesOf = (m: RemoteMesh) => ('dmesh' in m ? m.dmesh() : toDmesh(m));
const sha256 = async (b: Uint8Array<ArrayBuffer>) => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', b)), (x) => x.toString(16).padStart(2, '0')).join('');
const salt = (id: string, tag: string) => sha256(new TextEncoder().encode(`${id}\0${tag}`));
/** fn over items, n at a time; the first error stops the rest (the ones running finish) and rejects. */
async function pool<T>(n: number, items: readonly T[], fn: (x: T, k: number) => Promise<void>): Promise<void> {
  let next = 0;
  await Promise.all(Array.from({ length: Math.min(n, items.length) }, async () => {
    while (next < items.length) {
      const k = next++;
      try { await fn(items[k], k); } catch (e) { next = items.length; throw e; }
    }
  }));
}

/**
 * A job's mesh ids, sensors then context: a mesh's own `id` if it has one, else the SHA-256 hex of its .dmesh bytes
 * (8 bodies hashed at a time, each dropped once hashed), with a named mesh's name folded in (sha256(hex \0 name \0
 * name)): the server keeps one name per id. A context id equal to a sensor's is salted (sha256(id \0 ctx), again while
 * it still matches one): the server refuses a mesh in both roles. Nothing is written back; ids given are kept as they
 * are, so passing the result back salts only.
 */
export async function meshIds(input: { sensors: readonly RemoteMesh[]; context?: readonly RemoteMesh[]; signal?: AbortSignal }): Promise<string[]> {
  const all = [...input.sensors, ...(input.context ?? [])], ids = all.map((m) => m.id ?? '');
  await pool(8, all, async (m, k) => {
    if (ids[k]) return;
    input.signal?.throwIfAborted();
    const d = await sha256(bytesOf(m));
    ids[k] = m.name ? await salt(d, `name\0${m.name}`) : d;
  });
  const sensors = new Set(ids.slice(0, input.sensors.length));
  for (let k = input.sensors.length; k < ids.length; k++) while (sensors.has(ids[k])) ids[k] = await salt(ids[k], 'ctx');
  return ids;
}

/** Mesh ids this process has put on, or found on, each server: those skip POST /meshes/missing. */
const uploaded = new Map<string, Set<string>>();

/**
 * Browser or Node: runs the core on a `daylight_server` as a queued job, so several users share the GPU. Meshes travel
 * by id (`RemoteMesh.id`, else meshIds(): their bytes' SHA-256, nothing written back): `POST <url>/jobs` names them. Ids
 * this process has not seen on that server are checked with one `POST /meshes/missing` (a 404 there, from an older
 * server: all missing), and the missing ones go up 6 at a time via `PUT <url>/mesh/<id>?name=<stem>`, each body built
 * just before its PUT (`onUpload(done, total)` from 0 to total); so a re-run, or a reload of the same model, sends no
 * geometry. A mesh's core name is its name, or its id when unnamed, made unique per job (dmeshStems of those: id_2, ...). When the job answers 404
 * `{missing}` anyway (restarted, or it evicted the mesh) those go up again before one retry. The job is then long-polled (`GET /jobs/<id>?wait=25`); `onStatus` sees every
 * queued / running state. Aborting `signal` cancels the job on the server (`DELETE /jobs/<id>`), also one aborted
 * while the server was making it (the POST itself is not aborted, so its id is always known). `epw` is a Blob
 * (uploaded) or `{ url }` (the server fetches it through its allowlisted proxy cache). `args` are CLI flags verbatim
 * (no files). `clientId` (header `X-Client-Id`) identifies this client for the per-client job limit. Rejects with the
 * server's text on 4xx / 5xx or a failed job. Resolves with the values as columns, `data[c][row]` for `columns[c]`
 * (rows of mesh k from `manifest.meshes[k].first`): float32, read from `GET <url>/jobs/<id>/values` as one buffer that
 * every column is a view of; `manifest` is the finished job's JSON.
 */
export async function runRemote(
  url: string,
  input: {
    sensors: RemoteMesh[]; context?: RemoteMesh[]; epw?: Blob | { url: string }; args?: string[];
    signal?: AbortSignal; clientId?: string; onStatus?: (job: RemoteJob) => void; onUpload?: (done: number, total: number) => void;
    fetch?: typeof globalThis.fetch;
  },
): Promise<{ data: Float32Array<ArrayBuffer>[]; columns: string[]; manifest: RemoteManifest; cache?: Record<string, string> }> {
  const f = input.fetch ?? globalThis.fetch;
  const base = url.replace(/\/$/, '');
  const headers: Record<string, string> = input.clientId ? { 'X-Client-Id': input.clientId } : {};
  const req = (path: string, init: RequestInit = {}) => f(`${base}${path}`, { ...init, headers, signal: input.signal });
  const meshes = [...input.sensors, ...(input.context ?? [])], ids = await meshIds(input), first = new Map<string, number>();
  ids.forEach((id, k) => { if (!first.has(id)) first.set(id, k); });
  const sent = uploaded.get(base) ?? uploaded.set(base, new Set()).get(base)!;
  const put = async (todo: readonly string[]) => {
    const list = [...new Set(todo)];
    let done = 0, failed = false;
    if (list.length) input.onUpload?.(0, list.length);
    await pool(6, list, async (id) => {
      const k = first.get(id);
      if (k === undefined) throw new Error(`${base}/jobs asked for an unknown mesh id ${id}`);
      // its own name (the id when unnamed), never a job's _2: the server keeps it for the id, every later job included,
      // and makes names unique per job itself (as dmeshStems)
      const name = dmeshStems([meshes[k].name || id])[0];
      try {
        const res = await req(`/mesh/${encodeURIComponent(id)}?name=${encodeURIComponent(name)}`, { method: 'PUT', body: bytesOf(meshes[k]) });
        if (!res.ok) throw new Error(`PUT ${base}/mesh/${id} failed (${res.status}): ${(await res.text()).trim()}`);
        sent.add(id);
        if (!failed) input.onUpload?.(++done, list.length); // no progress after the upload failed
      } catch (e) { failed = true; throw e; }
    });
  };
  const body = () => {
    const fd = new FormData();
    ids.forEach((id, k) => fd.append(k < input.sensors.length ? 'sensors' : 'context', id));
    if (input.epw instanceof Blob) fd.append('epw', input.epw, 'weather.epw');
    else if (input.epw) fd.append('epw_url', input.epw.url);
    for (const a of input.args ?? []) fd.append('args', a);
    return fd;
  };
  const fresh = [...first.keys()].filter((id) => !sent.has(id)); // new to this server as far as this process knows
  if (fresh.length) {
    const r = await req('/meshes/missing', { method: 'POST', body: JSON.stringify({ ids: fresh }) });
    if (!r.ok && r.status !== 404) throw new Error(`${base}/meshes/missing failed (${r.status}): ${(await r.text()).trim()}`);
    await put(r.ok ? ((await r.json()) as { missing: string[] }).missing : fresh); // 404: a server without the route
    for (const id of fresh) sent.add(id);
  }
  // Not aborted: once the server has made the job its id is known, so an abort meanwhile still deletes it below.
  const post = () => f(`${base}/jobs`, { method: 'POST', body: body(), headers });
  let res = await post();
  if (res.status === 404) { // the server lost some (restart, eviction): put them again, retry once
    const { missing } = (await res.json()) as { missing: string[] };
    for (const id of missing) sent.delete(id);
    await put(missing);
    res = await post();
  }
  if (!res.ok) throw new Error(`${base}/jobs failed (${res.status}): ${(await res.text()).trim()}`);
  let job = (await res.json()) as RemoteJob & Partial<RemoteManifest> & { values_b64?: string };
  try {
    input.signal?.throwIfAborted(); // aborted while the job was being made
    while (job.state === 'queued' || job.state === 'running') {
      input.onStatus?.(job);
      const poll = await req(`/jobs/${encodeURIComponent(job.id)}?wait=25`);
      if (!poll.ok) throw new Error(`${base}/jobs/${job.id} failed (${poll.status}): ${(await poll.text()).trim()}`);
      job = await poll.json();
    }
  } catch (e) {
    if (input.signal?.aborted) f(`${base}/jobs/${encodeURIComponent(job.id)}`, { method: 'DELETE', headers }).catch(() => undefined); // free the GPU for others
    throw e;
  }
  if (job.state !== 'done') throw new Error(`${base} job ${job.id} ${job.state}${job.status ? ` (${job.status})` : ''}: ${(job.error ?? '').trim()}`);
  // The job JSON carries no values (values_b64 only on ?inline=1, and never decoded here: at large sizes that string
  // passes a browser's length limit). They come as raw float32 LE, column-major, streamed into one buffer of the size
  // the manifest implies: a result of up to 0.5 GB is never held twice, and each column is a view of it.
  const { values_b64: _inline, ...manifest } = job as RemoteJob & RemoteManifest & { values_b64?: string };
  const rows = manifest.meshes.reduce((n, g) => n + g.count, 0), size = 4 * rows * manifest.columns.length, buf = new Uint8Array(size);
  const path = manifest.values?.url ?? `/jobs/${encodeURIComponent(job.id)}/values`, got = await req(path);
  if (!got.ok) throw new Error(`${base}${path} failed (${got.status}): ${(await got.text()).trim()}`); // 404: expired (the server's result cap)
  const reader = got.body!.getReader();
  let n = 0;
  for (let r = await reader.read(); !r.done; r = await reader.read()) {
    if (n + r.value.length > size) { n += r.value.length; reader.cancel().catch(() => undefined); break; } // too long: stop reading
    buf.set(r.value, n);
    n += r.value.length;
  }
  if (n !== size) throw new Error(`${base}${path}: ${n} of ${size} bytes`);
  // Float32Array reads host byte order: little-endian on every target (x86, ARM).
  return { data: manifest.columns.map((_, c) => new Float32Array(buf.buffer, 4 * c * rows, rows)), columns: manifest.columns, manifest, cache: manifest.cache };
}
