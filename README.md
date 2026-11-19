# kit-sim_core

GPU simulation core for meshes: rays from every face or point of the analysed meshes, traced through the whole model
on NVIDIA RT cores with OptiX 9. Two tools share one library, `daylightcore` (mesh readers, sensors, mesh roles, the
OptiX pipeline and the resident scenes): **daylight**, annual daylight and solar simulation, and **views**, view
analysis. Both run as CLIs (`daylight.exe`, `views.exe`), from C++, through the C ABI `daylight_c.dll` and in-process
from C#, Python and TypeScript / JavaScript; browsers reach daylight through the web app's server.

## Tools

| | daylight: `daylight.exe` | views: `views.exe` |
|---|---|---|
| asks | how much sun and daylight reaches each face or point | what each face or point sees, and how far |
| rays per element | to each sky patch (145 / 577 / 2305, `--sky-density`), to the ground below the horizon and to the sun at every time step | one per 2 deg cell (`--cell`) of a field of view: 120 deg wide about the normal, 70 deg down to 50 deg up (3,600 rays), or all around (`--fov 360`: 10,800) |
| besides the meshes | weather and place: an EPW (`--epw`), a clear-sky year for `--lat --lon` (`--clear-sky`) or none for sun hours; a year, an instant (`--time`) or a range (`--from --to --step`) | target groups (`--target lake=water`); the eye moved along the normal (`--offset 1.6` above a floor: standing); a virtual ground plane that ends downward rays (`--virtual-ground`, `--ground-z`) |
| columns | `radiation` kWh/m2 over the run (also `direct`, `diffuse`), `irradiance` mean W/m2, `illuminance` mean lux, `sunhours` h | the shares of the field that end on `sky`, `ground`, `obstruction` and each target group (summing to 1); `distance` (log mean) and `distance_min` in m, an open ray counting as `--far` (1000 m); `reach_<D>`, the share that sees at least D m (`--reach en17037`: 6, 20, 50) |
| model | the sky a port of Radiance `gendaymtx` (Perez all-weather); `--mode physical` or `ladybug` (Ladybug's studies bit for bit); glass and porous slots (`--optics`, `--transmission on`), reflected light (`--bounces N`) | opaque surfaces; each ray through a random point of its cell (`--jitter`, deterministic); every ray's class and distance on request (`--dump-rays`) |
| validated against | Ladybug Tools (`--mode ladybug`: radiation ~1e-6 relative, sun hours exact), Radiance (the `gendaymtx` sky byte for byte, `rcontrib`), pvlib (sun position, clear sky) | Radiance `rtrace`, ray by ray (10 cases: 100.000 % of the compared rays in class and distance); the unobstructed share next to Ladybug's View Percent |
| in code | C++ `daylight::run` (`src/daylight.h`); C ABI and bindings: `DL_DAYLIGHT`, `Tool.Daylight`, `'daylight'` | C++ `daylight::views::run` (`src/views.h`); C ABI and bindings: `DL_VIEWS`, `Tool.Views`, `'views'` |
| docs | [docs/daylight.md](docs/daylight.md): every flag, output, worked examples | [docs/views.md](docs/views.md): field of view, ray classes, distances, every flag, output, worked examples |

Both CLIs are built from the same `src/main.cpp` and treat meshes and results alike:

- **Meshes**: `.dmesh` (raw arrays read with one `fread`: the native input), `.obj` (text interop) or `.usd*` (the
  `-DDAYLIGHT_USD=ON` build); points in metres, Z up, faces with right-handed winding.
- **Roles**: positional files are analysed (sensor meshes) and block rays; `--context FILE` files only block;
  `--no-occlude NAME` keeps an analysed mesh out of the rays.
- **Rows and columns**: a row per element of the analysed meshes in input order (`--on faces`, the default, or
  `vertices`), a column per quantity or share and side (`--sides front|back|both`: `radiation_front`, `sky_back`).
- **Files**: `--out v.txt` writes a text line per row, `--out v.npy` a float64 `[N x C]` NumPy array, and either a
  manifest beside it (`v.txt.json`, `v.npy.json`: the columns, each mesh's rows); without `--out` the values go to
  stdout. `daylight --help` and `views --help` list the flags.

```
daylight roof.obj --context city.obj --epw site.epw --out rad.txt                   # annual kWh/m2 per face
daylight roof.obj --clear-sky --lat 52.52 --lon 13.41 --quantity radiation,sunhours --out v.npy   # no weather file
daylight roof.obj --lat 52.52 --lon 13.41 --quantity sunhours --from 06-21T00:00 --to 06-22T00:00   # 21 June
views facade.obj --context city.obj --out v.txt                                     # 120 deg about each face's normal
views facade.obj --context city.obj --context lake.obj --target lake=water --reach en17037 --out v.npy
views floor.obj --context city.obj --no-occlude floor --fov 360 --offset 1.6 --out v.txt   # a standing eye, all around
```

`daylight` and `views` stand for `build\RelWithDebInfo\daylight.exe` and `views.exe`.

## Quick start

On Windows x64 with an NVIDIA GPU and what [Build](#build) lists (VS 2022, CUDA 12.6, the OptiX 9 headers): build,
then run both tools on [tests/box.obj](tests/box.obj), two meshes (OBJ groups): `ground`, four 2 m quads at z 0 facing
up, and `box`, a 1 m box floating 0.5 m above the fourth quad, both analysed and blocking. Nothing to download: views
needs only the meshes, and daylight synthesises a clear-sky year for Berlin.

```
cmake --preset vs2022 && cmake --build --preset release
build\RelWithDebInfo\daylight.exe tests\box.obj --clear-sky --lat 52.52 --lon 13.41 --quantity radiation,sunhours
build\RelWithDebInfo\views.exe tests\box.obj
```

Each prints a line per face, 16 in all: the four quads, then the box's 12 triangles (bottom, top, then two per side).
daylight's columns are `radiation_front` (kWh/m2 over the year) and `sunhours_front`: about 1600 kWh/m2 and 4456 sun
hours on the open quads, 926 and 3587 on the quad under the box. views' columns are the shares of each face's field
(120 deg about its normal) that end on `sky`, `ground` and `obstruction`, then `distance` (the log mean, m) and
`distance_min`: the quad under the box sees 42 % sky, the rest is the box, 0.50 m above it. Row by row:
[daylight.md](docs/daylight.md#examples), [views.md](docs/views.md#examples).

```
pwsh examples/cli/run.ps1   # both tools, five flag sets on box.obj, the mean of each column per mesh
pwsh examples/run.ps1       # every example, a program per way in: PASS / FAIL each
```

`pwsh` is PowerShell 7. `examples/run.ps1` also needs the .NET 9 SDK, uv and Node >= 22.18 with `npm install` run in
`bindings/ts` ([examples.md](docs/examples.md#running-them)).

## Use it from your language

| way in | runs | where | guide | example |
|---|---|---|---|---|
| CLIs: any language, scripts, batch jobs | a process per run: mesh files in, text or `.npy` out | `daylight.exe`, `views.exe` in `build/RelWithDebInfo/` | [daylight.md](docs/daylight.md), [views.md](docs/views.md) | [cli/run.ps1](examples/cli/run.ps1) |
| C++17 (CMake, MSVC) | in-process | the static library `daylightcore` (`add_subdirectory(<repo>)`): `src/daylight.h`, `src/views.h`, `src/cli.h`; USD stages: `bindings/cpp` | [cpp.md](docs/cpp.md) | [cpp/example.cpp](examples/cpp/example.cpp) |
| C, any language with a C FFI | in-process | `daylight_c.dll` (`src/daylight_c.h`) | [c-abi.md](docs/c-abi.md) | [c/example.c](examples/c/example.c) |
| C#: .NET 8 / 9, .NET Framework 4.8 (Rhino 6 / 7, Grasshopper), Rhino 8, Unity | in-process, over the C ABI | `bindings/csharp`: `Daylight.dll` (netstandard2.0) | [csharp.md](docs/csharp.md) | [csharp/Program.cs](examples/csharp/Program.cs) |
| Python >= 3.10: numpy arrays, USD stages in and out | in-process (`daylight.native`) or a process per run (`run_meshes`) | `bindings/python`: package `daylight` | [python.md](docs/python.md) | [python/inprocess.py](examples/python/inprocess.py), [python/process.py](examples/python/process.py) |
| TypeScript, JavaScript: Node >= 20, three.js, glTF | in-process (`daylight-mesh/native`) or a process per run (`run(exe, ...)`) | `bindings/ts`: package `daylight-mesh` | [javascript.md](docs/javascript.md) | [typescript/native.ts](examples/typescript/native.ts), [javascript/native.mjs](examples/javascript/native.mjs), [typescript/cli.ts](examples/typescript/cli.ts) |
| browsers (CUDA cannot run in one) | a job on the web app's HTTP server, which runs the core in-process; daylight only | `runRemote` in `daylight-mesh` | [javascript.md](docs/javascript.md#in-a-browser) | none |

Every way in takes the same meshes and the tools' own flags (C++ sets the matching `Options` fields) and gives the same
rows, columns and manifest. In-process runs give the CLI's values bit for bit without a process or files, pay the GPU
start once per host process, and keep the scene of meshes that are run again on the GPU, shared by both tools. Python,
both tools on the same meshes:

```python
from daylight import Mesh, native            # uv run --project bindings/python python, from the repo root
with Mesh.load('tests/box.obj') as box:      # the meshes "ground" and "box", both sensors
    sun = native.run('daylight', ['--clear-sky', '--lat', '52.52', '--lon', '13.41'], box)
    view = native.run('views', ['--reach', 'en17037'], box)       # the same meshes: the GPU scene is reused
print(sun.values.shape, view.values.shape, view.stats['scene_reused'])   # (16, 1) (16, 8) True
```

The bindings also put results back onto meshes: USD primvars (Python), three.js vertex colours (TypeScript)
([results-on-meshes.md](docs/results-on-meshes.md)). The C++, C#, Python and TypeScript / JavaScript guides share one
layout: setup, a daylight and a views example worked through, results, speed, errors / cancel / progress, reference
(the full API is in the binding's README) and examples. Choosing a way in: [api.md](docs/api.md); every example with
its run command and output: [examples.md](docs/examples.md).

## Performance

RTX 2060 6 GB (Turing); expect 5-10x on Ada / Blackwell GPUs. More scenes and how they were measured:
[docs/performance.md](docs/performance.md).

| run | size | time |
|---|---|---|
| `daylight --bench`: a year of Boston weather, radiation + sun hours | 1 M sensors, 12 M triangles: 4.7 G rays | trace 2.5 s, 4.2 s in all |
| `views --bench`: a street-grid city, 120 deg about each facade quad | 1.02 M sensors, 10 M triangles: 3.66 G rays | trace 1.84 s, 3.6 s in all |
| in-process (Python), a run after the first over the same meshes | 4 quads and a box: daylight sun hours of 21 June / views `--fov 360 --offset 1.6` | 0.9 / 1.0 ms (a CLI run: 0.70 / 0.61 s) |
| the same | 206,480 facade quads: daylight sun hours of 21 June / views 120 deg (743 M rays) | 103 / 504 ms (a CLI run: 0.92 / 1.31 s) |

The first in-process run pays the GPU start (the CUDA context and the OptiX pipeline) once, about 0.35 s.

## Build

| | needs |
|---|---|
| to build the core | VS 2022 (MSVC 14.44), CUDA 12.6 (at its default path, as the presets name it), the OptiX SDK 9.0 headers (`OPTIX_INSTALL_DIR`, default `C:/ProgramData/NVIDIA Corporation/OptiX SDK 9.0.0`; the runtime is in the driver), CMake 3.27+, git (FetchContent pulls earcut.hpp, and LightUSD for the USD build) |
| to run | Windows 10/11 x64, an NVIDIA GPU of compute capability 7.5 or newer (Turing and later: GTX 16, RTX 20-50, A100, H100; RT cores make it fast, without them OptiX runs on the CUDA cores), an NVIDIA driver R570+ (OptiX 9.0) and the Microsoft VC++ 2015-2022 x64 runtime |
| the bindings | C#: the .NET 9 SDK; Python: uv (Python >= 3.10, numpy, usd-core); TypeScript / JavaScript: Node >= 20 (>= 22.18 runs the `.ts` sources) and npm |

Nothing from the CUDA toolkit or the OptiX SDK is needed to run: cudart is linked statically and OptiX ships in the
driver. The same exe runs on every such card without a rebuild ([performance.md](docs/performance.md#other-gpus)).

```
cmake --preset vs2022 && cmake --build --preset release && ctest --preset release           # build/RelWithDebInfo/
cmake --preset vs2022-usd && cmake --build --preset release-usd && ctest --preset release-usd   # + USD input
cd bindings/python && uv sync && uv run python -m daylight.check                            # Python (usd-core, numpy)
cd bindings/ts && npm install && npm run build && npm test                                  # TypeScript + JavaScript
dotnet run --project bindings/csharp/check -f net9.0                                        # C# (also -f net48)
pwsh examples/run.ps1                                                                       # every example
```

`build/RelWithDebInfo/` then holds `daylight.exe`, `views.exe`, `daylight_c.dll` (the C ABI), `daylightcore.lib` (the
static library, both tools) and the examples `example_cpp.exe` and `example_c.exe`. The build compiles
`src/trace_dev.cu` to OptiX-IR with `nvcc -optix-ir -arch=compute_75` and embeds it; the rest is plain C++17
(`/W4 /WX`) plus one CUDA host file, `src/gpu.cu`, built for `DAYLIGHT_CUDA_ARCHITECTURES` (default
`75-real;75-virtual`: native code for Turing, portable PTX for every newer card; `-DDAYLIGHT_CUDA_ARCHITECTURES=...`
overrides it). The default build has no USD / glTF dependency.

## Deployment

Windows only today. To ship: the exes or `daylight_c.dll` (the CUDA runtime linked in); the target machine needs a
supported GPU, the NVIDIA driver and the VC++ runtime (installed, or its DLLs beside them;
[c-abi.md](docs/c-abi.md#loading-the-dll)).

- **Containers on Windows** (Podman, Docker: Linux containers on WSL2) cannot run it: NVIDIA's WSL driver has no
  OptiX. On driver 621.01 `C:\Windows\System32\lxss\lib\libnvoptix.so.1` is a loader stub without
  `optixQueryFunctionTable`, and NVIDIA gives no date for OptiX on WSL; re-check after driver updates. Windows
  containers have no CUDA.
- **A Linux GPU host** (Podman or Docker with the NVIDIA Container Toolkit) is the container path, after a port: about
  50 lines for the core, its CLIs and `libdaylight_c.so` (MSVC-only CMake flags, `-fPIC`, a Linux preset,
  `_ftelli64`, TBB for the parallel loops, the C ABI's locale guard), about 160 with the bindings, tests, `validate/`
  and docs (library and exe names, paths), and about 170 in the web app's server (its Win32 process and browser
  layer). The host needs an NVIDIA driver R570+ with its GL libraries (OptiX's `libnvoptix.so.1` ships there;
  compute-only installs lack it), CDI (`nvidia-ctk cdi generate`, then `podman run --device nvidia.com/gpu=all`) and
  a Turing or newer GPU. Not tried yet: there is no Linux GPU machine here.
- Before offering it to third parties as a hosted service, check the OptiX SDK and NVIDIA driver licences (the SDK
  asks for notice before commercial use).

## Documentation

| page | |
|---|---|
| [daylight.md](docs/daylight.md) | the daylight tool: what it computes, every flag, exit codes, output (rows, columns, manifest, units), worked examples |
| [views.md](docs/views.md) | the views tool: field of view, ray classes, the virtual ground, distances and reach, mesh roles, every flag, output, worked examples, limits |
| [api.md](docs/api.md) | library and bindings: the ways in compared (in-process or a process, the resident scene, platform) |
| [cpp.md](docs/cpp.md) | C++: `daylightcore`, both tools in-process, USD stages through `bindings/cpp` |
| [c-abi.md](docs/c-abi.md) | C ABI: `daylight_c.dll`, both tools in-process for C and any C FFI; what the bindings load |
| [csharp.md](docs/csharp.md) | C#: `Daylight.dll` for .NET, Rhino / Grasshopper and Unity |
| [python.md](docs/python.md) | Python: `daylight.native` in-process, `run_meshes` through the CLIs, USD stages in and out |
| [javascript.md](docs/javascript.md) | TypeScript and JavaScript: `daylight-mesh/native` in Node, `run(exe, ...)`, `runRemote` for browsers, three.js / glTF |
| [examples.md](docs/examples.md) | every example: what it shows, its run command and output; `examples/run.ps1` |
| [results-on-meshes.md](docs/results-on-meshes.md) | values back onto meshes: USD primvars, three.js colours, colormaps |
| [input.md](docs/input.md) | `.dmesh` / `.obj` / `.usd*` formats, sensor vs context, culling, faces vs vertices |
| [modes.md](docs/modes.md) | daylight's `physical` and `ladybug` modes, surfaces, EPW vs `--clear-sky` |
| [validation.md](docs/validation.md) | the harness and its results: daylight against Radiance, Ladybug Tools and pvlib, views against Radiance `rtrace` and Ladybug |
| [performance.md](docs/performance.md) | timings: both `--bench` scenes, transmission, reflected light, mesh transport, in-process vs CLI, other GPUs |
| [layout.md](docs/layout.md) | source layout |

The full API of each binding is in its README: [bindings/csharp](bindings/csharp/README.md),
[bindings/python](bindings/python/README.md), [bindings/ts](bindings/ts/README.md),
[bindings/cpp](bindings/cpp/README.md) (the USD reader). The validation harness:
[validate/README.md](validate/README.md).

## Licence

Apache 2.0 with the Commons Clause, see [LICENSE](LICENSE): free to use, modify and share, not to sell (no paid product,
hosting or consulting service whose value comes substantially from this software). Third-party notices:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
