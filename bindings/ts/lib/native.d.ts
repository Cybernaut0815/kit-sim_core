import type { ObjMesh } from './index.ts';
import type { RunArgs } from './options.ts';
export type { CommonOptions, DaylightOptions, OpticsSpec, RunArgs, TargetSpec, ViewsOptions } from './options.ts';
/**
 * Loads daylight_c.dll for this process, once: `dll`, else env DAYLIGHT_DLL, else <repo>/build/RelWithDebInfo/
 * daylight_c.dll (found from this file, in src/ or lib/). SimulationMesh, run and usage call it without a path. Throws when the
 * library does not load, has another ABI version, or another path is loaded already. Kept loaded while Node runs (the
 * static CUDA runtime does not support unloading); koffi frees it as Node exits, measured harmless.
 */
export declare function init(dll?: string): void;
export type Tool = 'daylight' | 'views';
/** The tool's usage text, as its CLI's -h prints it (docs/daylight.md, docs/views.md). */
export declare function usage(tool: Tool): string;
/** The C ABI version of the loaded daylight_c.dll (dl_version, DL_ABI_VERSION; init() checks it is this binding's). */
export declare function version(): number;
/**
 * Meshes on the C side (copied once). Pass them to any number of runs: runs over the same handles in the same roles and
 * order reuse the GPU scene (stats.sceneReused). Freed by free(), else when garbage collected.
 */
export declare class SimulationMesh {
    /** Names of the meshes the handle holds, in order (an OBJ's groups): what --target, --optics, --no-occlude and the
     *  manifest call them. Empty for a mesh without faces. */
    readonly names: string[];
    private constructor();
    /** A mesh from the .dmesh arrays: xyz per point in world metres, Z up; vertices per face (>= 3); 0-based indices,
     *  right-handed; an optics slot per face (absent: slot 0). Throws the core's message (a bad index, a short face). */
    static create(name: string, positions: ArrayLike<number>, counts: ArrayLike<number>, indices: ArrayLike<number>, slots?: ArrayLike<number>): SimulationMesh;
    /** An ObjMesh (fromThree, fromGLB): its triangles and slots, its name as it is (run(exe) passes names through
     *  dmeshStems; do the same for flags that must work with both). */
    static fromObj(m: ObjMesh): SimulationMesh;
    /** The meshes of a .obj (one per o/g group, named by it) or .dmesh (named by its stem), read as the CLI reads them.
     *  ASCII paths only: koffi sends UTF-8, the C runtime reads the ANSI code page (the CLI's argv is ANSI already). */
    static load(path: string): SimulationMesh;
    /** Frees the handle now (again: nothing); throws while a run uses it. */
    free(): void;
}
/** daylight.h RunStats: whether a resident scene served the run, milliseconds per phase, rays, launches, GAS bytes. */
export interface RunStats {
    sceneReused: boolean;
    prepMs: number;
    skyMs: number;
    buildMs: number;
    traceMs: number;
    reduceMs: number;
    bounceMs: number;
    rays: number;
    launches: number;
    gasBytes: number;
}
/** A run's values as columns, data[c][row] for columns[c] (the CLI's --out FILE.npy: float64, or float32 with run's
 *  `float32`), its manifest (FILE.json) parsed and as text (LF), and its stats. */
export interface NativeResult<Values extends Float64Array | Float32Array = Float64Array> {
    data: Values[];
    columns: string[];
    manifest: unknown;
    manifestText: string;
    stats: RunStats;
}
/** run()'s hooks: an abort signal, progress, and float32 values (dl_result_values_f32: half the memory). */
export interface RunOptions {
    signal?: AbortSignal;
    onProgress?: (p: number) => void;
    float32?: boolean;
}
/** A failed run: status 1 (usage: flags, arguments, mesh names the flags do not resolve) or 2 (runtime: a file, the GPU,
 *  out of memory, cancelled), the CLI's message. */
export declare class DaylightError extends Error {
    readonly status: number;
    constructor(message: string, status: number);
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
export declare function run(tool: Tool, args: RunArgs, input: {
    sensors: SimulationMesh[];
    context?: SimulationMesh[];
}, opts?: RunOptions & {
    float32?: false;
}): Promise<NativeResult>;
export declare function run(tool: Tool, args: RunArgs, input: {
    sensors: SimulationMesh[];
    context?: SimulationMesh[];
}, opts: RunOptions & {
    float32: true;
}): Promise<NativeResult<Float32Array>>;
