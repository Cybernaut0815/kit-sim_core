# C ABI: `daylight_c.dll`

Both tools in-process, for hosts that are not C++: the C# ([bindings/csharp](../bindings/csharp)), Python
([bindings/python](../bindings/python)) and Node ([bindings/ts](../bindings/ts)) bindings load it. A run takes the CLI's
own flags (the CLI's parser checks them, so every option, default and message is the CLI's) and meshes as handles
instead of files. C++ hosts link `daylightcore` and use `daylight.h` / `views.h` / `cli.h` ([api.md](api.md)) instead.

Files: [src/daylight_c.h](../src/daylight_c.h) (the contract, every function documented),
[src/c_api.cpp](../src/c_api.cpp), test [tests/c_abi.c](../tests/c_abi.c) (ctest `c_abi`). The build puts
`daylight_c.dll` and its import library `daylight_c.lib` beside `daylight.exe` (`build/RelWithDebInfo/`).

Why in-process: a CLI run pays a process start, the GPU context, writing the meshes and reading the results, and builds
its scene every time. In-process the GPU context lives as long as the host, and runs over the same mesh handles keep
their scene resident ([performance.md](performance.md#in-process-vs-cli)). Browsers cannot run CUDA: they use the HTTP
server (`runRemote` in bindings/ts).

## Conventions

- **Status** = the CLI's exit codes: `DL_OK` 0, `DL_USAGE` 1 (bad flags or arguments, mesh names the flags do not
  resolve), `DL_RUNTIME` 2 (the run failed: a file, the GPU, `out of memory: ...`, `cancelled`). After a failure
  `dl_last_error()` holds the message, per thread, until the next failing call on that thread. Functions that return a
  handle return NULL on error. No exception crosses the ABI.
- **Version**: `dl_version()` returns `DL_ABI_VERSION` (1), bumped when a signature, a constant or the stats layout
  changes. Bindings refuse another version.
- **Strings** end with NUL. Names and flags are UTF-8; paths (`dl_mesh_load`, `--epw`, `--timeseries`, `--dump-*`)
  use the C runtime's narrow encoding (the ANSI code page on Windows), as the CLI's arguments do.
- **Numbers**: `dl_run` reads flags and EPW files, and writes the manifest, with `.` as the decimal point whatever C
  locale the host set (its thread runs in the `"C"` locale for the call; the host's locale is back afterwards).
- **Arrays** are the [.dmesh](input.md) arrays: float64 xyz (metres, Z up), int32 counts (>= 3) and 0-based indices
  (right-handed winding), uint16 face slots. Counts are int64.
- **Threads**: any function from any thread. Runs take turns behind one lock (the GPU runs one call at a time), so a
  `dl_run` waits for the one before it. Free a handle only when no call uses it.

## Meshes

| function | |
|---|---|
| `dl_mesh_create(name, xyz, n_points, counts, n_faces, indices, n_indices, slots)` | one mesh, copied; `slots` NULL = slot 0; the .dmesh checks (a bad index, a face below 3 vertices: NULL) |
| `dl_mesh_load(path)` | the meshes of a `.obj` (one per o/g group, named by it) or `.dmesh` (named by the stem), as the CLI reads them |
| `dl_mesh_count(h)`, `dl_mesh_name(h, k)` | the meshes a handle holds and their names (what `--target`, `--optics`, `--no-occlude` use) |
| `dl_mesh_free(h)` | |

A handle never changes. A mesh without faces gives a handle holding no mesh (the readers drop empty groups).

## Runs

`dl_run(tool, argc, argv, sensors, n_sensors, context, n_context, control, &result)`: `tool` is `DL_DAYLIGHT` (0) or
`DL_VIEWS` (1); `argv` the flags of [cli.md](cli.md) or [views.md](views.md) without the program name; the sensor
handles take the place of the CLI's mesh arguments, the context handles of its `--context` meshes, each handle giving
its meshes in order.

| flag | in-process |
|---|---|
| mesh paths, `--context FILE` | refused (1): `dl_mesh_load` gives handles |
| `--out FILE` | refused (1): the result holds the values and the manifest |
| `--bench`, `-h` | refused (1): the CLIs' benchmark; `dl_usage(tool)` gives the usage text |
| everything else | as in the CLI, files included: `--epw`, `--timeseries`, `--dump-*`, `--dump-rays` use paths of this process, `-v` writes to its stderr |

Names that the meshes do not resolve (`--optics`, `--no-occlude`, `--target`) give `DL_USAGE`, as in the CLI.

**Resident scenes.** The scene key is the handles in order and role plus `--no-self-occlude` (the core adds the
transmitting slots and the `--no-occlude` set). A run over the same handles in the same roles and order reuses the
GPU scene: no triangulation, no upload, no GAS build (`dl_result_stats[0]` = 1). daylight and views share an opaque
scene. Each handle has a serial number that is never reused, so a freed handle can never match a later one, whatever
its address. One scene stays resident (the core's `residentScenes` 1); the GPU context and that scene stay until the
process exits.

**Cancel and progress.** `dl_control_create()` makes a control for one run: another thread calls
`dl_control_cancel(c)` (the run stops at its next check, the scene or a trace chunk, and fails with `cancelled`; a
cancel after the last check lets it finish) or reads `dl_control_progress(c)` (0..1 after each trace chunk, 1 once the
run succeeded). Free it after `dl_run` returned.

## Results

| function | |
|---|---|
| `dl_result_rows(r)`, `dl_result_cols(r)` | rows = elements of the sensor meshes in input order (faces, or points with `--on vertices`) |
| `dl_result_manifest(r)` | the manifest the CLI writes to `FILE.json` (columns, meshes; views: the field, targets), with LF line ends (the CLI's file has CRLF) |
| `dl_result_values(r, out, n)` | rows x cols, row-major float64, as `--out FILE.npy` holds them |
| `dl_result_values_f32(r, out, n)` | the same rounded to float32 |
| `dl_result_stats(r, out)` | `DL_STATS` (10) doubles: scene reused (1/0), ms of prep, sky, build, trace, reduce, bounce; rays, launches, GAS bytes |
| `dl_result_free(r)` | |

The values and the manifest are those of the CLI for the same meshes and flags, bit for bit (ctest `c_abi` compares
them with `cli_smoke_npy` and `views_smoke_npy`).

## Example (C)

```c
#include "daylight_c.h"
dl_mesh* box = dl_mesh_load("tests/box.obj");
const char* const args[] = {"--lat", "41.98", "--lon", "-87.92", "--tz", "-6", "--quantity", "sunhours", "--time", "06-21T12:00"};
dl_result* r = NULL;
if (dl_run(DL_DAYLIGHT, 10, args, &box, 1, NULL, 0, NULL, &r) != DL_OK) fprintf(stderr, "%s\n", dl_last_error());
double v[64];
dl_result_values(r, v, 64);   /* 16 faces x 1 column (sunhours_front) */
dl_result_free(r);
dl_mesh_free(box);
```

## Loading the DLL

- x64 only. Needs the NVIDIA driver (`nvcuda.dll`, `nvoptix.dll`) and the Visual C++ 2015-2022 x64 runtime
  (`msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`; Python from python.org lacks
  `msvcp140.dll`: install the redistributable or copy them beside the DLL). No CUDA toolkit DLL: the CUDA runtime is
  linked in.
- C#: next to the managed `Daylight.dll`, or `DAYLIGHT_DLL`. Python: `DAYLIGHT_DLL`, next to the package, or
  `<repo>/build/RelWithDebInfo/`. Node: `DAYLIGHT_DLL` or the same build folder. Each loads it by absolute path.
- Never unload it (`FreeLibrary`): the static CUDA runtime does not support unloading; process exit is fine.

## Limits

Windows only (MSVC build). One GPU run at a time per process. Paths are opened in the ANSI code page: a host that
passes UTF-8 strings (C#, Node, Python `str`) reaches only ASCII paths in flags; Python passes an `os.PathLike` in the
ANSI code page. A failed `dl_mesh_create` / `dl_mesh_load` / `dl_control_create` returns NULL with the message but no
status (the bindings report 1 for bad arrays, 2 for files and memory). No call releases the GPU memory before the
process exits.
