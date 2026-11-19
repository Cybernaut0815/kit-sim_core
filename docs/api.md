# Library and bindings

Both tools of the core, daylight ([daylight.md](daylight.md)) and views ([views.md](views.md)), are one library,
`daylightcore`, with several ways in. Each takes the same meshes (points in metres, Z up; faces; 0-based indices), the
same mesh roles (sensors are analysed and block, context only blocks) and the same flags (the CLIs' own; C++ sets the
matching `Options` fields), and gives the same result: a row per element of the sensor meshes, a column per quantity
or share, and a manifest that maps the rows to the meshes. In-process runs give the CLI's values bit for bit.

| way in | for | runs | resident scene | docs |
|---|---|---|---|---|
| `daylight.exe`, `views.exe` | any language, scripts, batch jobs: mesh files in, text or `.npy` out | a process per run | none: built every run | [daylight.md](daylight.md), [views.md](views.md) |
| C++: `daylightcore` | C++17 hosts (CMake, MSVC): apps, servers; USD stages through `bindings/cpp` | in-process | per `sceneKey`, up to `residentScenes` | [cpp.md](cpp.md), [bindings/cpp](../bindings/cpp/README.md) |
| C ABI: `daylight_c.dll` | C, and any language with a C FFI | in-process | one, keyed by the mesh handles | [c-abi.md](c-abi.md) |
| C#: `Daylight.dll` | .NET 8 / 9, .NET Framework 4.8 (Rhino 6 / 7, Grasshopper), Rhino 8, Unity | in-process, over the C ABI | one, as the C ABI | [csharp.md](csharp.md), [bindings/csharp](../bindings/csharp/README.md) |
| Python: `daylight` | Python >= 3.10 (numpy, usd-core): arrays, notebooks, USD stages in and out | in-process over the C ABI (`daylight.native`), or a process per run (`run_meshes`) | in-process: one | [python.md](python.md), [bindings/python](../bindings/python/README.md) |
| TypeScript / JavaScript: `daylight-mesh` | Node >= 20: arrays, three.js and glTF meshes | in-process over the C ABI (`daylight-mesh/native`), or a process per run (`run(exe, ...)`) | in-process: one | [javascript.md](javascript.md), [bindings/ts](../bindings/ts/README.md) |
| browsers: `runRemote` (`daylight-mesh`) | web pages away from the GPU (CUDA cannot run in a browser) | a job on the web app's HTTP server, which runs the core in-process; daylight only | the server's: meshes by id, `--gpu-scenes` scenes | [javascript.md](javascript.md#in-a-browser) |

- **In-process or a process.** A CLI run pays the process start, the GPU start (the CUDA context and the OptiX
  pipeline, about 0.35 s on an RTX 2060), the mesh and result files and the scene build, every time: right for one
  large run. In-process the GPU context lives as long as the host and the scene of meshes that are run again stays on
  the GPU, so repeated runs (parameter studies, sliders, a server) take milliseconds to a fraction of a second
  ([performance.md](performance.md#in-process-vs-cli)).
- **Resident scene**: the occluder triangles and the OptiX acceleration structure, kept between runs over the same
  meshes in the same roles; daylight and views share it. C++ names a scene by `sceneKey` and keeps up to
  `residentScenes`; the C ABI, and every binding over it, keys it by the mesh handles and keeps one.
- **The C ABI underneath.** The C#, Python and Node bindings load `daylight_c.dll` for their in-process runs; none
  changes the core. Their process runners write `.dmesh` files and start the CLIs.
- **Platform.** Everything but the browser runs on the GPU host: Windows x64, an NVIDIA GPU of compute capability 7.5
  or newer, driver R570+ and the VC++ 2015-2022 x64 runtime ([README](../README.md#build)). One GPU run per process at
  a time, both tools together.
- **Results onto meshes**: USD primvars (Python), three.js vertex colours (TypeScript):
  [results-on-meshes.md](results-on-meshes.md). Mesh formats and roles: [input.md](input.md).

The language guides share one layout: setup, a worked daylight example, a worked views example, results, speed,
errors / cancel / progress, reference, examples. [examples.md](examples.md) lists a program per way in, each running
both tools, and `examples/run.ps1`, which runs them all.
