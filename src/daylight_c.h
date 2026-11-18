/* SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0 */
/*
 * daylight C ABI (daylight_c.dll): the daylight and views tools in-process, what the C#, Python and Node bindings wrap
 * (bindings/csharp, bindings/python, bindings/ts). A run takes the CLI's own flags (cli.h parses them: every option,
 * default and check is the CLI's) and meshes as handles instead of files. C++ hosts use daylight.h / views.h / cli.h
 * instead. docs/c-abi.md explains this file.
 *
 * Conventions:
 *  - Status codes are the CLI's exit codes: DL_OK 0, DL_USAGE 1 (bad flags or arguments, mesh names the flags do not
 *    resolve), DL_RUNTIME 2 (the run failed: a file, the GPU, "out of memory: ...", "cancelled"). After a failure
 *    dl_last_error() holds the message (per thread, until the next failing call on that thread). Functions returning
 *    a handle return NULL on error. No exception crosses the ABI.
 *  - Strings end with NUL. Names and flags are UTF-8; file paths (dl_mesh_load, --epw, --timeseries, --dump-*) use the
 *    C runtime's narrow encoding (the ANSI code page on Windows), as the CLI's argv does.
 *  - Numbers: dl_run reads the flags and EPW files, and writes the manifest, with '.' as the decimal point whatever C
 *    locale the host set (its thread runs in the "C" locale for the call).
 *  - Mesh arrays are the .dmesh arrays (daylight.h): float64 xyz, int32 counts and 0-based indices, uint16 slots.
 *  - Threads: any function from any thread. Runs take turns behind one lock (the GPU runs one at a time), so a dl_run
 *    waits for the one before it. Free a handle only when no call uses it.
 */
#ifndef DAYLIGHT_C_H
#define DAYLIGHT_C_H

#include <stdint.h>

#if defined(_WIN32)
#ifdef DAYLIGHT_C_BUILD
#define DL_API __declspec(dllexport)
#else
#define DL_API __declspec(dllimport)
#endif
#else
#define DL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DL_ABI_VERSION 1
#define DL_OK 0
#define DL_USAGE 1
#define DL_RUNTIME 2
#define DL_DAYLIGHT 0 /* daylight.exe: annual daylight / solar (docs/cli.md) */
#define DL_VIEWS 1    /* views.exe: view analysis (docs/views.md) */
#define DL_STATS 10   /* doubles dl_result_stats writes */

typedef struct dl_mesh dl_mesh;
typedef struct dl_control dl_control;
typedef struct dl_result dl_result;

/** ABI version of the library (DL_ABI_VERSION of its build), bumped whenever a signature, a constant or the stats
 *  layout changes: bindings refuse a library of another version. */
DL_API int32_t dl_version(void);

/** Message of the last failed call on this thread ("" if none). */
DL_API const char* dl_last_error(void);

/** The tool's usage text, as its CLI's -h prints it (static); NULL for an unknown tool. Mesh paths, --context, --out,
 *  --bench and -h are refused in-process (dl_run). */
DL_API const char* dl_usage(int32_t tool);

/* ---- meshes -------------------------------------------------------------------------------------------------- */

/**
 * A mesh from arrays, copied: xyz[3 n_points] in world metres, Z up; counts[n_faces] vertices per face (>= 3);
 * indices[n_indices] 0-based into the points, right-handed winding, n_indices = sum(counts); slots[n_faces] the optics
 * slot per face (--optics NAME/SLOT), or NULL for slot 0 everywhere. name: what --optics, --no-occlude, --target and
 * the manifest call it. NULL on error (the .dmesh reader's checks). A mesh without faces gives a handle holding no
 * mesh, as the readers drop empty groups. A handle never changes.
 */
DL_API dl_mesh* dl_mesh_create(const char* name, const double* xyz, int64_t n_points, const int32_t* counts,
                               int64_t n_faces, const int32_t* indices, int64_t n_indices, const uint16_t* slots);

/** The meshes of a file, read as the CLI reads a mesh argument: .obj (one mesh per o/g group, named by it) or .dmesh
 *  (one mesh, named by the file stem). NULL on error. */
DL_API dl_mesh* dl_mesh_load(const char* path);

/** Meshes the handle holds (an OBJ's groups; 0 or 1 from dl_mesh_create); -1 for NULL. */
DL_API int32_t dl_mesh_count(const dl_mesh* mesh);

/** Name of its mesh k (owned by the handle); NULL when k is out of range. */
DL_API const char* dl_mesh_name(const dl_mesh* mesh, int32_t k);

/** Frees a handle (NULL is ignored), never while a dl_run given it is running. A GPU scene it was part of stays
 *  resident until a later run replaces it, and no later handle can reuse that scene, whatever its address. */
DL_API void dl_mesh_free(dl_mesh* mesh);

/* ---- run control --------------------------------------------------------------------------------------------- */

/** Cancel flag and progress of one dl_run, for other threads (no callbacks cross the ABI). NULL on error. */
DL_API dl_control* dl_control_create(void);

/** From any thread: the run stops at its next check (the scene, each trace chunk or launch) and fails with DL_RUNTIME
 *  "cancelled"; a run not started yet fails as soon as it starts, a cancel after the run's last check lets it finish
 *  (DL_OK). The flag stays set: one control per run. */
DL_API void dl_control_cancel(dl_control* control);

/** From any thread: the share of sensors traced so far, 0..1 (after each trace chunk, 1 once the run succeeded); 0 for
 *  NULL. */
DL_API double dl_control_progress(const dl_control* control);

/** Frees a control (NULL is ignored) once the dl_run given it has returned. */
DL_API void dl_control_free(dl_control* control);

/* ---- runs ---------------------------------------------------------------------------------------------------- */

/**
 * One run of a tool (DL_DAYLIGHT, DL_VIEWS). argv[0..argc): its CLI flags without the program name ("--epw",
 * "site.epw", "--quantity", "radiation,sunhours", ...). sensors[0..n_sensors) take the place of the CLI's mesh
 * arguments, then context[0..n_context) of its --context meshes; each handle gives its meshes in order, by their names.
 * Refused (DL_USAGE): mesh paths and --context (use dl_mesh_load), --out (the result holds values and manifest),
 * --bench, -h (dl_usage). Every other flag works as in the CLI, files included (--epw, --timeseries, --dump-*: paths of
 * this process; -v writes to its stderr). Names the meshes do not resolve in --optics, --no-occlude or --target give
 * DL_USAGE, as in the CLI.
 * Runs over the same handles in the same roles and order reuse the GPU scene (dl_result_stats[0]); daylight and views
 * share an opaque one. control: NULL or a dl_control. On DL_OK *out is a new result (dl_result_free), else NULL.
 */
DL_API int32_t dl_run(int32_t tool, int32_t argc, const char* const* argv, dl_mesh* const* sensors, int32_t n_sensors,
                      dl_mesh* const* context, int32_t n_context, dl_control* control, dl_result** out);

/* ---- results ------------------------------------------------------------------------------------------------- */

/** Rows: the elements of the sensor meshes (faces, or points with --on vertices) in input order; -1 for NULL. */
DL_API int64_t dl_result_rows(const dl_result* result);

/** Columns, named by the manifest's "columns"; -1 for NULL. */
DL_API int64_t dl_result_cols(const dl_result* result);

/** The manifest the CLI writes to FILE.json, the same text with LF line ends (the CLI's file has CRLF on Windows), owned
 *  by the result; NULL for NULL. */
DL_API const char* dl_result_manifest(const dl_result* result);

/** Copies the values into out (room for n values): rows x cols, row-major float64, as --out FILE.npy holds them.
 *  DL_USAGE when n is too small or an argument is NULL. */
DL_API int32_t dl_result_values(const dl_result* result, double* out, int64_t n);

/** The same, each value rounded to float32. */
DL_API int32_t dl_result_values_f32(const dl_result* result, float* out, int64_t n);

/** out[DL_STATS]: 1 when a resident scene served the run (0: built), the milliseconds of prep, sky, build, trace,
 *  reduce and bounce, then rays, launches and GAS bytes (daylight.h RunStats). */
DL_API int32_t dl_result_stats(const dl_result* result, double* out);

/** Frees a result (NULL is ignored). */
DL_API void dl_result_free(dl_result* result);

#ifdef __cplusplus
}
#endif
#endif
