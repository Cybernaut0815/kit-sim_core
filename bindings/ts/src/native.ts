// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// daylight-mesh/native (Node only): the daylight and views tools in-process through daylight_c.dll (src/daylight_c.h,
// docs/c-abi.md) via koffi 3. SimulationMesh handles keep their GPU scene resident between runs; run() keeps the event loop free.
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Worker, parentPort, workerData } from 'node:worker_threads';
import koffi from 'koffi';
import type { ObjMesh } from './index.ts';
import type { RunArgs } from './options.ts';
export type { CommonOptions, DaylightOptions, OpticsSpec, RunArgs, TargetSpec, ViewsOptions } from './options.ts';

const ABI = 1; // DL_ABI_VERSION this binding was written against

/**
 * Every dl_* function of daylight_c.h, declared on this thread's koffi. koffi 3: pointers (handles) are BigInt, NULL is
 * null; int64 arguments take numbers (or BigInt), int64 results come back as numbers (BigInt only beyond 2^53, never
 * for a mesh); strings are UTF-8 both ways.
 */
function bind(path: string) {
  const lib = koffi.load(path), f = (decl: string) => lib.func(decl);
  const dl_version = f('int32_t dl_version(void)'), v: number = dl_version(); // first: another ABI may lack the others
  if (v !== ABI) throw new Error(`ABI version ${v}, this binding wants ${ABI}`);
  for (const name of ['dl_mesh', 'dl_control', 'dl_result']) try { koffi.type(name); } catch { koffi.opaque(name); }
  return {
    dl_version,
    dl_last_error: f('const char *dl_last_error(void)'),
    dl_usage: f('const char *dl_usage(int32_t tool)'),
    dl_mesh_create: f('dl_mesh *dl_mesh_create(const char *name, const double *xyz, int64_t n_points, ' +
      'const int32_t *counts, int64_t n_faces, const int32_t *indices, int64_t n_indices, const uint16_t *slots)'),
    dl_mesh_load: f('dl_mesh *dl_mesh_load(const char *path)'),
    dl_mesh_count: f('int32_t dl_mesh_count(const dl_mesh *mesh)'),
    dl_mesh_name: f('const char *dl_mesh_name(const dl_mesh *mesh, int32_t k)'),
    dl_mesh_free: f('void dl_mesh_free(dl_mesh *mesh)'),
    dl_control_create: f('dl_control *dl_control_create(void)'),
    dl_control_cancel: f('void dl_control_cancel(dl_control *control)'),
    dl_control_progress: f('double dl_control_progress(const dl_control *control)'),
    dl_control_free: f('void dl_control_free(dl_control *control)'),
    dl_run: f('int32_t dl_run(int32_t tool, int32_t argc, const char *const *argv, dl_mesh *const *sensors, ' +
      'int32_t n_sensors, dl_mesh *const *context, int32_t n_context, dl_control *control, _Out_ dl_result **out)'),
    dl_result_rows: f('int64_t dl_result_rows(const dl_result *result)'),
    dl_result_cols: f('int64_t dl_result_cols(const dl_result *result)'),
    dl_result_manifest: f('const char *dl_result_manifest(const dl_result *result)'),
    dl_result_values: f('int32_t dl_result_values(const dl_result *result, double *out, int64_t n)'),
    dl_result_values_f32: f('int32_t dl_result_values_f32(const dl_result *result, float *out, int64_t n)'),
    dl_result_stats: f('int32_t dl_result_stats(const dl_result *result, double *out)'),
    dl_result_free: f('void dl_result_free(dl_result *result)'),
  };
}

let dl: ReturnType<typeof bind> | undefined, dllPath = '';

/**
 * Loads daylight_c.dll for this process, once: `dll`, else env DAYLIGHT_DLL, else <repo>/build/RelWithDebInfo/
 * daylight_c.dll (found from this file, in src/ or lib/). SimulationMesh, run and usage call it without a path. Throws when the
 * library does not load, has another ABI version, or another path is loaded already. Kept loaded while Node runs (the
 * static CUDA runtime does not support unloading); koffi frees it as Node exits, measured harmless.
 */
export function init(dll?: string): void {
  const path = resolve(dll ?? (process.env.DAYLIGHT_DLL ||
    fileURLToPath(new URL('../../../build/RelWithDebInfo/daylight_c.dll', import.meta.url))));
  if (dl) {
    if (dll !== undefined && path !== dllPath) throw new Error(`daylight: ${dllPath} is loaded already, not ${path}`);
    return;
  }
  if (!existsSync(path)) throw new Error(`daylight: cannot load ${path}: no such file (init(path) or DAYLIGHT_DLL)`);
  let d: ReturnType<typeof bind>;
  try { d = bind(path); } catch (e) { throw new Error(`daylight: cannot load ${path}: ${(e as Error).message}`); }
  dl = d;
  dllPath = path;
}

const native = () => dl ?? (init(), dl!);

export type Tool = 'daylight' | 'views';
const toolId = (tool: Tool) => (tool === 'daylight' ? 0 : tool === 'views' ? 1 : -1); // -1: the core's "unknown tool"

/** The tool's usage text, as its CLI's -h prints it (docs/daylight.md, docs/views.md). */
export function usage(tool: Tool): string {
  return native().dl_usage(toolId(tool)) ?? '';
}

/** The C ABI version of the loaded daylight_c.dll (dl_version, DL_ABI_VERSION; init() checks it is this binding's). */
export function version(): number {
  return native().dl_version();
}

// Module state of each SimulationMesh: its handle (null once freed) and the runs using it.
const state = new WeakMap<SimulationMesh, { handle: bigint | null; runs: number }>();
const finalizer = new FinalizationRegistry<bigint>((handle) => native().dl_mesh_free(handle));
type Typed = Float64Array | Int32Array | Uint16Array;
const typed = <T extends Typed>(C: { new (n: number): T; from(a: ArrayLike<number>): T }, a: ArrayLike<number>) =>
  a instanceof C ? a : C.from(a); // a typed array of the right kind goes as it is

/**
 * Meshes on the C side (copied once). Pass them to any number of runs: runs over the same handles in the same roles and
 * order reuse the GPU scene (stats.sceneReused). Freed by free(), else when garbage collected.
 */
export class SimulationMesh {
  /** Names of the meshes the handle holds, in order (an OBJ's groups): what --target, --optics, --no-occlude and the
   *  manifest call them. Empty for a mesh without faces. */
  readonly names: string[];

  private constructor(handle: bigint | null) {
    const d = native();
    if (!handle) throw new Error(d.dl_last_error());
    this.names = Array.from({ length: d.dl_mesh_count(handle) }, (_, k) => d.dl_mesh_name(handle, k) as string);
    state.set(this, { handle, runs: 0 });
    finalizer.register(this, handle, this);
  }

  /** A mesh from the .dmesh arrays: xyz per point in world metres, Z up; vertices per face (>= 3); 0-based indices,
   *  right-handed; an optics slot per face (absent: slot 0). Throws the core's message (a bad index, a short face). */
  static create(name: string, positions: ArrayLike<number>, counts: ArrayLike<number>, indices: ArrayLike<number>,
    slots?: ArrayLike<number>): SimulationMesh {
    if (positions.length % 3) throw new Error(`${name}: positions must be xyz triples`);
    if (slots && slots.length !== counts.length) { // the core reads one per face
      throw new Error(`${name}: slots must hold one value per face (${counts.length}), not ${slots.length}`);
    }
    const xyz = typed(Float64Array, positions), c = typed(Int32Array, counts), i = typed(Int32Array, indices);
    const s = slots ? typed(Uint16Array, slots) : null;
    return new SimulationMesh(native().dl_mesh_create(name, xyz, xyz.length / 3, c, c.length, i, i.length, s));
  }

  /** An ObjMesh (fromThree, fromGLB): its triangles and slots, its name as it is (run(exe) passes names through
   *  dmeshStems; do the same for flags that must work with both). */
  static fromObj(m: ObjMesh): SimulationMesh {
    const counts = new Int32Array(Math.floor(m.indices.length / 3)).fill(3);
    return SimulationMesh.create(m.name, m.positions, counts, m.indices, m.slots);
  }

  /** The meshes of a .obj (one per o/g group, named by it) or .dmesh (named by its stem), read as the CLI reads them.
   *  ASCII paths only: koffi sends UTF-8, the C runtime reads the ANSI code page (the CLI's argv is ANSI already). */
  static load(path: string): SimulationMesh {
    return new SimulationMesh(native().dl_mesh_load(path));
  }

  /** Frees the handle now (again: nothing); throws while a run uses it. */
  free(): void {
    const s = state.get(this)!;
    if (s.runs) throw new Error('daylight: free() while a run uses this mesh');
    if (s.handle) {
      finalizer.unregister(this);
      native().dl_mesh_free(s.handle);
      s.handle = null;
    }
  }
}

/** daylight.h RunStats: whether a resident scene served the run, milliseconds per phase, rays, launches, GAS bytes. */
export interface RunStats {
  sceneReused: boolean;
  prepMs: number; skyMs: number; buildMs: number; traceMs: number; reduceMs: number; bounceMs: number;
  rays: number; launches: number; gasBytes: number;
}

/** A run's values as columns, data[c][row] for columns[c] (the CLI's --out FILE.npy: float64, or float32 with run's
 *  `float32`), its manifest (FILE.json) parsed and as text (LF), and its stats. */
export interface NativeResult<Values extends Float64Array | Float32Array = Float64Array> {
  data: Values[]; columns: string[]; manifest: unknown; manifestText: string; stats: RunStats;
}

/** run()'s hooks: an abort signal, progress, and float32 values (dl_result_values_f32: half the memory). */
export interface RunOptions { signal?: AbortSignal; onProgress?: (p: number) => void; float32?: boolean }

/** A failed run: status 1 (usage: flags, arguments, mesh names the flags do not resolve) or 2 (runtime: a file, the GPU,
 *  out of memory, cancelled), the CLI's message. */
export class DaylightError extends Error {
  readonly status: number;
  constructor(message: string, status: number) {
    super(message);
    this.name = 'DaylightError';
    this.status = status;
  }
}

interface Job {
  id: number; tool: number; args: string[]; sensors: (bigint | null)[]; context: (bigint | null)[]; control: bigint; float32: boolean;
}
interface Reply { id: number; status: number; error?: string; manifest: string; data: (Float64Array | Float32Array)[]; stats: Float64Array }

// One run thread per process: dl_run blocks it, not the event loop, and a failure's message is read on the thread that
// failed. dl_last_error is per thread, so koffi's .async (libuv's pool) would lose it.
let thread: Worker | undefined, seq = 0;
const pending = new Map<number, { resolve: (r: Reply) => void; reject: (e: unknown) => void }>();

// The thread died: its jobs fail. A dead thread's late 'exit' (after its 'error') leaves the next thread's jobs alone.
function stop(w: Worker, e: unknown): void {
  if (thread !== w) return;
  thread = undefined;
  for (const p of pending.values()) p.reject(e);
  pending.clear();
}

function post(job: Omit<Job, 'id'>): Promise<Reply> {
  if (!thread) {
    const w = (thread = new Worker(new URL(import.meta.url), { workerData: { daylightRunThread: dllPath } }));
    w.on('message', (r: Reply) => {
      const p = pending.get(r.id);
      pending.delete(r.id);
      if (!pending.size) w.unref(); // idle: the process may exit
      p?.resolve(r);
    });
    w.on('error', (e) => stop(w, e));
    w.on('exit', (code) => stop(w, new Error(`daylight: the run thread exited (${code})`)));
  }
  const w = thread, id = ++seq;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    w.ref();
    w.postMessage({ ...job, id });
  });
}

/**
 * One run of `tool` with its CLI flags (`args`, without the program name, raw or as DaylightOptions / ViewsOptions;
 * mesh paths, --context, --out, --bench and -h are refused) over mesh handles: `sensors` in place of the CLI's mesh
 * arguments, `context` of its --context meshes.
 * Flags go as UTF-8 (mesh names match SimulationMesh.names); file paths in them (--epw, ...) must be ASCII, as for SimulationMesh.load.
 * Runs go one at a time on a worker thread, so the event loop stays free. Aborting `signal` cancels: the run stops at
 * its next check, and the call rejects with DaylightError 2 "cancelled" (also when the run got past its last check).
 * `onProgress` gets the share of sensors traced, 0..1, polled every 100 ms and once at the end. `float32: true` gives
 * the values as Float32Array columns. The meshes stay referenced (free() throws) until it settles. Rejects with a
 * DaylightError.
 */
export async function run(tool: Tool, args: RunArgs, input: { sensors: SimulationMesh[]; context?: SimulationMesh[] },
  opts?: RunOptions & { float32?: false }): Promise<NativeResult>;
export async function run(tool: Tool, args: RunArgs, input: { sensors: SimulationMesh[]; context?: SimulationMesh[] },
  opts: RunOptions & { float32: true }): Promise<NativeResult<Float32Array>>;
export async function run(tool: Tool, args: RunArgs, input: { sensors: SimulationMesh[]; context?: SimulationMesh[] },
  opts: RunOptions = {}): Promise<NativeResult<Float64Array> | NativeResult<Float32Array>> {
  const d = native(), meshes = [...input.sensors, ...(input.context ?? [])];
  const handles = meshes.map((m) => state.get(m)?.handle ?? null); // a freed mesh: the core's "NULL mesh handle"
  const control: bigint | null = d.dl_control_create();
  if (!control) throw new Error(d.dl_last_error());
  const count = (k: number) => { for (const m of meshes) { const s = state.get(m); if (s) s.runs += k; } };
  let last = -1;
  const progress = () => {
    const p: number = d.dl_control_progress(control);
    if (p !== last) opts.onProgress?.((last = p));
  };
  const cancel = () => d.dl_control_cancel(control);
  if (opts.signal?.aborted) cancel(); else opts.signal?.addEventListener('abort', cancel);
  const timer = opts.onProgress ? setInterval(progress, 100) : undefined;
  count(1);
  try {
    // imported here, after the meshes are counted in: this file is also the run thread, which loads no sibling module
    const { argsOf } = await import('./options.ts');
    const n = input.sensors.length;
    const r = await post({ tool: toolId(tool), args: argsOf(args), sensors: handles.slice(0, n), context: handles.slice(n), control, float32: !!opts.float32 });
    if (r.error !== undefined) throw r.status > 0 ? new DaylightError(r.error, r.status) : new Error(r.error);
    if (opts.signal?.aborted) throw new DaylightError('cancelled', 2);
    progress();
    const manifest = JSON.parse(r.manifest) as { columns: string[] };
    const [reused, prepMs, skyMs, buildMs, traceMs, reduceMs, bounceMs, rays, launches, gasBytes] = r.stats;
    return {
      data: r.data as Float64Array[], columns: manifest.columns, manifest, manifestText: r.manifest,
      stats: { sceneReused: reused === 1, prepMs, skyMs, buildMs, traceMs, reduceMs, bounceMs, rays, launches, gasBytes },
    };
  } finally {
    clearInterval(timer);
    opts.signal?.removeEventListener('abort', cancel);
    d.dl_control_free(control); // the run has returned
    count(-1);
  }
}

// The run thread (this file again, as a worker): the run, its message or its result, all on this one thread.
function serve(path: string): void {
  const d = bind(path);
  parentPort!.on('message', ({ id, tool, args, sensors, context, control, float32 }: Job) => {
    let reply: Omit<Reply, 'id'> | { status: number; error: string }, transfer: ArrayBuffer[] = [];
    try {
      const out: (bigint | null)[] = [null];
      const status: number = d.dl_run(tool, args.length, args, sensors, sensors.length, context, context.length, control,
        out);
      if (status) reply = { status, error: d.dl_last_error() };
      else {
        const res = out[0];
        try {
          const rows = Number(d.dl_result_rows(res)), cols = Number(d.dl_result_cols(res));
          const Values = float32 ? Float32Array : Float64Array, values = new Values(rows * cols), stats = new Float64Array(10);
          const copy = float32 ? d.dl_result_values_f32 : d.dl_result_values;
          if ((values.length && copy(res, values, values.length)) || d.dl_result_stats(res, stats)) {
            throw new Error(d.dl_last_error());
          }
          const data = Array.from({ length: cols }, () => new Values(rows)); // row-major -> columns, off the loop
          for (let i = 0, k = 0; i < rows; i++) for (let c = 0; c < cols; c++) data[c][i] = values[k++];
          reply = { status, manifest: d.dl_result_manifest(res), data, stats };
          transfer = data.map((col) => col.buffer);
        } finally {
          d.dl_result_free(res);
        }
      }
    } catch (e) {
      reply = { status: -1, error: e instanceof Error ? e.message : String(e) };
    }
    parentPort!.postMessage({ ...reply, id }, transfer);
  });
}

if (workerData?.daylightRunThread) serve(workerData.daylightRunThread);
