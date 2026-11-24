import type { Mesh, Object3D } from 'three';
export interface ObjMesh {
    name: string;
    positions: ArrayLike<number>;
    indices: ArrayLike<number>;
    slots?: Uint16Array;
    id?: string;
}
/**
 * `.dmesh` file stems (= core mesh names) for `names`: sanitised to `[A-Za-z0-9._-]`, empty -> `fallback`, made unique
 * case-insensitively (`_2`, `_3`, ... suffixes; Windows/macOS file systems ignore case). Shared by run() and runRemote().
 */
export declare function dmeshStems(names: ArrayLike<string>, fallback?: string): string[];
export declare function toObj(meshes: ObjMesh[]): string;
/**
 * One mesh -> `.dmesh` bytes (little-endian): "DLM1", uint32 flags, uint64 nPoints/nFaces/nIndices,
 * float64 xyz[3*nPoints], int32 counts[nFaces] (all 3 without `counts`), int32 indices[nIndices], then with `slots`
 * (flags 1, else 0) uint16 slot[nFaces]. Mesh name = file stem. With `matrix` the positions are local (three.js, Y-up)
 * and take fromThree's conversion (world by the column-major matrix, times `scale`, Z-up, mirrored winding reversed): the
 * same bytes as toDmesh(fromThree(...)), without the Float64 copy. `indices` absent: a triangle soup. `counts`: the
 * corners of each face (3 or more, summing to the indices), so quads and n-gons reach the core as the faces they are: one
 * receiver per face, and per-vertex receivers that follow the points the faces share; `slots` then holds one per face.
 */
export declare function toDmesh(mesh: {
    name?: string;
    positions: ArrayLike<number>;
    indices?: ArrayLike<number> | null;
    counts?: ArrayLike<number> | null;
    slots?: Uint16Array;
}, opts?: {
    matrix?: ArrayLike<number>;
    scale?: number;
}): Uint8Array<ArrayBuffer>;
/** Every descendant Mesh of `objects` (triangle BufferGeometry only; strips/fans/lines are not supported). */
export declare function fromThree(objects: Object3D[], opts?: {
    scale?: number;
}): ObjMesh[];
/** All TRIANGLES primitives of every node with a mesh in a .glb; name = node name + '/' + mesh name (or primitive index). */
export declare function fromGLB(data: ArrayBuffer | Uint8Array, opts?: {
    scale?: number;
}): Promise<ObjMesh[]>;
/** Core `--out FILE` text file -> one Float64Array per column, `data[c][element]` (blank lines skipped). */
export declare function parseValues(text: string): Float64Array[];
/** Core `--out FILE.npy` (NumPy v1.0, `<f8`, C order [N x cols]) -> one Float64Array per column, `data[c][element]`. */
export declare function parseNpy(bytes: Uint8Array, cols: number): Float64Array[];
/**
 * Values -> RGB triples (Float32Array, 3 per value) over [vmin, vmax] (default: finite min/max); non-finite -> grey.
 * `stops`: evenly spaced RGB stops (0..1) of any other gradient, default viridis.
 */
export declare function colormap(values: ArrayLike<number>, opts?: {
    vmin?: number;
    vmax?: number;
    stops?: ArrayLike<ArrayLike<number>>;
}): Float32Array;
/**
 * Colours `mesh` by `values`: one per triangle (geometry becomes non-indexed so each face gets a flat colour) or one per
 * vertex. Sets the `color` attribute and, unless `material: false`, a `MeshStandardMaterial({ vertexColors: true })`.
 * Colours come from colormap() (`vmin`, `vmax`, `stops`) or, with `rgb` (3 floats per value), from the caller.
 * Imports three lazily (peer dependency). Returns the RGB attribute array.
 */
export declare function applyToThree(mesh: Mesh, values: ArrayLike<number>, opts?: {
    vmin?: number;
    vmax?: number;
    stops?: ArrayLike<ArrayLike<number>>;
    rgb?: Float32Array;
    material?: boolean;
}): Promise<Float32Array>;
/**
 * Node only: writes the inputs into a temp dir, runs `exe <sensor files> [--context <file>]... ...args --out out.npy`,
 * returns the values as columns, `data[c][element]` for `columns[c]` (the core's doubles; runRemote has the same shape
 * in float32), and the `out.npy.json` manifest. A string input is OBJ text (one .obj); an ObjMesh[] becomes
 * one `<name>.dmesh` per mesh via toDmesh() (name sanitised to [A-Za-z0-9._-], made unique).
 */
export declare function run(exe: string, input: {
    sensors: string | ObjMesh[];
    context?: string | ObjMesh[];
    args?: string[];
}): Promise<{
    data: Float64Array[];
    columns: string[];
    manifest: unknown;
}>;
/** The core's `FILE.json` manifest as `daylight_server` returns it (plus the server's own fields): daylight's carries
 *  `duration_h` and `dirs`, views' `view` and `targets` (docs/views.md, Output). */
export interface RemoteManifest {
    columns: string[];
    duration_h?: number;
    dirs?: {
        sky: number;
        ground: number;
        sun: number;
    };
    view?: {
        fov: [h: number, lo: number, hi: number];
        gaze?: 'level';
        cell: number;
        grid: [rows: number, columns: number];
        jitter: boolean;
        north: number;
        offset: number;
        far: number;
        near: number;
        virtual_ground: boolean;
        ground_z: number;
        transmission?: true;
        reach: number[];
        classes: string[];
        image: string;
    };
    targets?: {
        mesh: string;
        group: string;
    }[];
    meshes: {
        name: string;
        first: number;
        count: number;
    }[];
    run_s?: number;
    cache?: Record<string, string>;
    values?: {
        url: string;
        dtype: '<f4';
        order: 'column-major';
        shape: [cols: number, rows: number];
        bytes: number;
    };
}
/** A job on `daylight_server` (`/jobs/{id}`): position counts the jobs ahead of a queued one. */
export interface RemoteJob {
    id: string;
    state: 'queued' | 'running' | 'done' | 'error' | 'cancelled';
    position?: number;
    error?: string;
    status?: number;
    progress?: number;
    eta_s?: number;
}
/** A mesh for runRemote(): an ObjMesh, or one whose .dmesh bytes are built on demand (each body dropped once hashed or sent). */
export type RemoteMesh = ObjMesh | {
    id?: string;
    name?: string;
    dmesh: () => Uint8Array<ArrayBuffer>;
};
/**
 * A job's mesh ids, sensors then context: a mesh's own `id` if it has one, else the SHA-256 hex of its .dmesh bytes
 * (8 bodies hashed at a time, each dropped once hashed), with a named mesh's name folded in (sha256(hex \0 name \0
 * name)): the server keeps one name per id. A context id equal to a sensor's is salted (sha256(id \0 ctx), again while
 * it still matches one): the server refuses a mesh in both roles. Nothing is written back; ids given are kept as they
 * are, so passing the result back salts only.
 */
export declare function meshIds(input: {
    sensors: readonly RemoteMesh[];
    context?: readonly RemoteMesh[];
    signal?: AbortSignal;
}): Promise<string[]>;
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
 * (no files), of the `tool` (daylight, the default, or views: docs/views.md). `clientId` (header `X-Client-Id`) identifies this client for the per-client job limit. Rejects with the
 * server's text on 4xx / 5xx or a failed job. Resolves with the values as columns, `data[c][row]` for `columns[c]`
 * (rows of mesh k from `manifest.meshes[k].first`): float32, read from `GET <url>/jobs/<id>/values` as one buffer that
 * every column is a view of; `manifest` is the finished job's JSON.
 */
export declare function runRemote(url: string, input: {
    sensors: RemoteMesh[];
    context?: RemoteMesh[];
    epw?: Blob | {
        url: string;
    };
    args?: string[];
    signal?: AbortSignal;
    clientId?: string;
    onStatus?: (job: RemoteJob) => void;
    onUpload?: (done: number, total: number) => void;
    fetch?: typeof globalThis.fetch;
    tool?: 'daylight' | 'views';
}): Promise<{
    data: Float32Array<ArrayBuffer>[];
    columns: string[];
    manifest: RemoteManifest;
    cache?: Record<string, string>;
}>;
