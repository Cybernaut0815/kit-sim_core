# Daylight (C# binding)

Guide: [docs/csharp.md](../../docs/csharp.md) (setup, both tools worked through, results, speed, errors); example:
[examples/csharp](../../examples/csharp) ([docs/examples.md](../../docs/examples.md)).

`Daylight.dll` runs both tools of the core in-process: `daylight` (annual daylight / solar,
[daylight.md](../../docs/daylight.md)) and `views` (view analysis, [views.md](../../docs/views.md)), through the C ABI
`daylight_c.dll` ([c-abi.md](../../docs/c-abi.md)). A run takes the CLI's own flags (typed `DaylightOptions` /
`ViewsOptions`, or strings) and meshes as handles; the result is a `double[]` (row-major, bit for bit what
`--out FILE.npy` holds; or a `float[]`) and the manifest. Runs over the same `SimulationMesh` objects
keep their GPU scene resident: no triangulation, upload or build the second time. netstandard2.0, no package
references: .NET 8/9, .NET Framework 4.8 (Rhino 6 and 7 with Grasshopper, Rhino 8 on request), Rhino 8 on .NET, Unity.

## Build and check

```sh
cmake --preset vs2022 && cmake --build --preset release && ctest --preset release   # daylight_c.dll, build/*smoke.npy
dotnet build bindings/csharp                                                        # bin/Debug/netstandard2.0/Daylight.dll
dotnet run --project bindings/csharp/check -f net9.0                                # and -f net48: "check OK"
```

The check runs the cases of `tests/c_abi.c` through the binding and compares both tools on `tests/box.obj` with the
CLI's `build/smoke.npy(.json)` and `build/views_smoke.npy(.json)` byte for byte. It loads
`build/RelWithDebInfo/daylight_c.dll` unless `DAYLIGHT_DLL` names another.

## Loading daylight_c.dll

The first call loads `daylight_c.dll` by its absolute path, `DAYLIGHT_DLL` if set (the file keeps that name: DllImport
finds it by it), else the one beside `Daylight.dll`; without either it leaves the search to the host (a Unity player's
`Plugins`, the app folder, `PATH`). Then it checks the ABI version (1). A missing library throws `DllNotFoundException`
saying where it looked, and the next call tries again. It needs Windows, a 64-bit process, the NVIDIA driver (no CUDA
toolkit) and the Visual C++ 2015-2022 x64 runtime (installed, or `msvcp140.dll`, `msvcp140_atomic_wait.dll`,
`vcruntime140.dll`, `vcruntime140_1.dll` copied beside `daylight_c.dll`). It is never unloaded: the GPU context and one
resident scene live until the process exits.

**.NET.** Reference `bindings/csharp/Daylight.csproj` (or `Daylight.dll`), copy `daylight_c.dll` into the output folder
(or set `DAYLIGHT_DLL`), and build the app for x64 (`<PlatformTarget>x64</PlatformTarget>`).

**Rhino and Grasshopper.** Copy `Daylight.dll` and `daylight_c.dll` into the plug-in's folder, beside its `.gha` or
`.rhp`; a C# script starts with `#r "<folder>/Daylight.dll"`. Rhino is x64 and brings the VC++ runtime. A `.gha` that
Grasshopper loads from bytes (its COFF loading option) has no folder: set `DAYLIGHT_DLL`. Unblock a downloaded zip
(`Unblock-File`) before unpacking it: .NET Framework refuses a DLL that carries the mark of the web.

**Unity.** Put both DLLs in `Assets/Plugins/x86_64/` and set `daylight_c.dll` to Editor and Standalone, Windows x86_64
(Inspector). The editor loads it beside `Daylight.dll`, a player from its `Plugins` folder.

Only .NET 9 and .NET Framework 4.8 run the check; Rhino, Grasshopper and Unity have not run it. IL2CPP may need a
`link.xml` that keeps `System.Runtime.Serialization` (`DataContractJsonSerializer` reads the manifest).

## Example

```csharp
using System;
using System.Threading;
using Daylight;

using (SimulationMesh model = SimulationMesh.Load("tests/box.obj"))   // OBJ groups: "ground", "box"
using (SimulationMesh facade = SimulationMesh.Create("facade", new double[] { -1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2 }, new[] { 4 }, new[] { 0, 1, 2, 3 }))
{
    var june = new DaylightOptions { Lat = 41.98, Lon = -87.92, Tz = -6, Quantities = new[] { Quantity.Sunhours }, Time = "06-21T12:00" };
    Result noon = Core.Run(june, new[] { model });
    double[] hours = noon.Column("sunhours_front");   // a value per face; noon.Meshes: each mesh's rows
    Result year = Core.Run(Tool.Daylight, new[] { "--epw", "site.epw" }, new[] { model });   // flags as strings; year.SceneReused: no rebuild

    var cancel = new CancellationTokenSource(TimeSpan.FromMinutes(5));
    Result view = await Core.RunAsync(new ViewsOptions { Fov = new double[] { 360 } }, new[] { facade }, new[] { model }, token: cancel.Token);
    Console.WriteLine(string.Join(", ", view.Columns));   // sky_front, ground_front, obstruction_front, ...
}
```

A failure throws `DaylightException`: `Status` 1 for bad flags, arguments or arrays (`unknown flag --bogus`), 2 when
the run fails (`cannot open EPW site.epw`, the GPU, `cancelled`), and the core's message.

Typed options: a property per CLI flag, PascalCase (`ClearSky` is `--clear-sky`; lists in the plural: `Quantities`,
`Targets`, `Distances`), enums for the choices (`Quantity`, `Sides`, `Elements`, `SkyMode`, `Gaze`, `DistanceStat`,
`OpticsKind`), `OpticsSpec` and `TargetSpec` for `--optics` and `--target`. Unset properties keep the core's defaults;
`options.ToArgs()` gives the flags ([api.md](../../docs/api.md#typed-options)).

## API

Namespace `Daylight`.

| | |
|---|---|
| `enum Tool { Daylight = 0, Views = 1 }` | the tool of a run |
| `SimulationMesh.Create(name, xyz, counts, indices, slots = null)` | one mesh from the `.dmesh` arrays, copied: `double[]` xyz 3 per point (metres, Z up), `int[]` counts (>= 3) and 0-based indices, `ushort[]` optics slot per face or null |
| `SimulationMesh.Load(path)` | the meshes of an `.obj` (one per o/g group, named by it) or `.dmesh` (named by its stem) |
| `mesh.Names` | what `--optics`, `--no-occlude`, `--target` and the manifest call the meshes, in row order |
| `mesh.Dispose()` | frees it (else the finalizer does), deferred while a run holds it |
| `new RunControl()`, `.Cancel()`, `.Progress` | one run's cancel (it throws status 2 `cancelled`) and progress (0..1), from any thread; one control per run |
| `new DaylightOptions { ... }`, `new ViewsOptions { ... }`, `.ToArgs()` | typed flags of one tool (they pick it); `ToArgs` gives the CLI's flags |
| `Core.Run(options, sensors, context = null, control = null, float32 = false)` | one run with typed options |
| `Core.Run(tool, args, sensors, context = null, control = null, float32 = false)` | one run: the CLI's flags without the program name; `sensors` and `context` take the place of its mesh arguments and `--context`. Refused (status 1): mesh paths, `--context`, `--out`, `--bench`, `-h`. `float32`: the values as `float[]` (`Values32`) |
| `Core.RunAsync(options or tool + args, sensors, context = null, control = null, float32 = false, token = default)` | the same on the thread pool; a cancelled token cancels the run and the task ends cancelled |
| `Core.Usage(tool)`, `Core.Version` | the CLI's `-h` text; the loaded DLL's C ABI version (1) |
| `result.Columns`, `.Rows`, `.Values`, `.Values32` | column names; rows (sensor faces, or points with `--on vertices`, in input order); rows x columns, row-major, as doubles or (after `float32: true`) floats, the other null |
| `result.Column(name)` | one column's values |
| `result.Manifest`, `.Meshes` | the manifest JSON as the CLI writes it (LF line ends); each sensor mesh's `Name`, `First` row and row `Count` |
| `result.SceneReused`, `.Stats` | a resident scene served the run; `RunStats`: `SceneReused`, `PrepMs`, `SkyMs`, `BuildMs`, `TraceMs`, `ReduceMs`, `BounceMs`, `Rays`, `Launches`, `GasBytes` |
| `DaylightException` | `Status`, the CLI's exit code, and the core's message |

Runs take turns (one GPU run per process at a time): a `Run` waits for the one before it. Flags travel as UTF-8 and
`SimulationMesh.Load` paths in the ANSI code page, so paths inside flags (`--epw`, `--timeseries`, `--dump-*`) must be ASCII, and
a `SimulationMesh.Load` path with a character outside the code page throws `ArgumentException`.
