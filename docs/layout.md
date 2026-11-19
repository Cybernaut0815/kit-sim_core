# Source layout

```
src/daylight.h   public API (daylight::run, RunOptions)   src/views.h   public API of views (daylight::views::run)
src/internal.h   module contracts                         src/params.h  OptiX launch params
src/mesh.cpp     OBJ + .dmesh readers, triangulation, sensors, mesh names (--optics, --no-occlude, --target)
src/solar.cpp    NOAA sun, EPW, clear sky, steps          src/sky.cpp   patches, gendaymtx + sun.c ports
src/run.cpp      daylight: composition, weights, chunk loop, dumps; the scene, admission and sensors both tools use
src/views.cpp    views: field, grid + cell weights, target labels, chunk loop, columns, ray dump
src/gpu.cu       OptiX host + CUDA kernels                src/trace_dev.cu  OptiX raygens (bits, bytes, bounce, view)
src/cli.cpp      CLI flags, weather / time rules, manifests of both tools (shared with servers)
src/main.cpp     both CLIs (format dispatch, output, --bench scenes): daylight.exe, and views.exe with VIEWS_CLI
src/daylight_c.h the C ABI (daylight_c.dll)       src/c_api.cpp  its implementation: main.cpp's run over mesh handles
tests/check*.cpp assert-style checks (ctest)      tests/c_abi.c  the C ABI from plain C through the DLL (ctest c_abi)
bindings/python  USD (pxr) in and out, .dmesh writer, CLI wrapper, daylight.native (in-process, ctypes)
bindings/cpp     USD reader (LightUSD) for the CLI / C++
bindings/csharp  Daylight.dll (netstandard2.0, P/Invoke on daylight_c.dll), check/ on net9.0 + net48
bindings/ts      three.js / glTF -> .dmesh, CLI wrapper, runRemote, vertex colours; lib/ = compiled JavaScript;
                 src/native.ts = daylight-mesh/native (Node, koffi, in-process)
examples/        a program per way in, both tools each: cli/ (PowerShell), cpp/ and c/ (CMake targets and ctests
                 example_cpp, example_c), csharp/, python/, typescript/, javascript/; run.ps1 runs them all
validate/        Python harness + ingest benchmark
docs/            the tools: daylight.md, views.md; the ways in: api.md, then cpp.md, c-abi.md, csharp.md, python.md,
                 javascript.md, examples.md; input.md, modes.md, results-on-meshes.md, validation.md, performance.md
```

Views ([views.md](views.md)) came in one branch, `feature/views`, with these files and functions:

- `src/views.h`: `daylight::views::Target`, `Options : RunOptions`, `Field`, `field()`, `run()`.
- `src/views.cpp`: `field()` (option checks, grid, the virtual ground's height); `run_views` behind `run()`: roles
  (`resolve_no_occlude`, `resolve_targets` in `mesh.cpp`), cell weights, the scene and its admission (`scene_acquire`,
  `run_admission`, `open_key`, `sensors_of` in `run.cpp`, shared with daylight), target labels from the scene's build
  order, the chunk loop, the columns and `--dump-rays`.
- `src/trace_dev.cu` `__raygen__view`: the frame (normal or world), the jittered cell points, the closest hit with the
  own-plane walk, the virtual ground, a class and a distance per ray.
- `src/gpu.cu`: `gpu_set_labels`, `gpu_view` (launches sized for the Windows TDR), `k_view` / `gpu_reduce_view` (per
  sensor and class: weight, weight x ln d, minimum d; the own-surface weight; the reach sums) and `gpu_download_view`.
- `src/cli.cpp` `daylight::views::cli` (`kUsage`, `parse` on the shared `parse_common`, `manifest_json`); `src/main.cpp`
  built a second time with `VIEWS_CLI` as the CMake target `views_cli` (`views.exe`), its `--bench` scene `city()`;
  ctest `views_smoke` and `views_smoke_npy`.
- `tests/check_views.cpp` (views' checks, part of `check`) and `validate/views.py` (views ray by ray against Radiance
  `rtrace`, and next to Ladybug's View Percent) are added by the same branch.
