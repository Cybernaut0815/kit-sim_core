# C# (.NET)

`Daylight.dll` (`bindings/csharp`, netstandard2.0, no package references) runs both tools of the core in-process, on
the same meshes, GPU and resident scenes: `daylight` (annual daylight / solar: radiation, irradiance, illuminance, sun
hours; [daylight.md](daylight.md)) and `views` (view analysis: the shares of a field of view that end on sky, ground,
obstructions or target groups, and how far each face sees; [views.md](views.md)). It calls the C ABI `daylight_c.dll`
([c-abi.md](c-abi.md)): a run takes the CLI's own flags (typed, `DaylightOptions` / `ViewsOptions`, or as strings) and
meshes as handles, and gives the values bit for bit as the CLI's `--out FILE.npy`, with its manifest. Hosts: .NET 8 / 9, .NET Framework 4.8 (Rhino 6 and 7 with Grasshopper),
Rhino 8, Unity.

## Setup

```
cmake --preset vs2022 && cmake --build --preset release && ctest --preset release  # build/RelWithDebInfo/daylight_c.dll
dotnet build bindings/csharp -c Release                       # bindings/csharp/bin/Release/netstandard2.0/Daylight.dll
dotnet run --project bindings/csharp/check -f net9.0          # "check OK"; also -f net48
```

The core's build needs VS 2022, CUDA 12.6 and the OptiX 9 headers ([README](../README.md#build)). The check runs the
binding's cases and compares both tools on `tests/box.obj` with the CLI's `ctest` outputs, byte for byte.

| file | from | |
|---|---|---|
| `daylight_c.dll` | the CMake build, `build/RelWithDebInfo/` | the core and both tools, CUDA runtime linked in; x64 |
| `Daylight.dll` | `bindings/csharp/Daylight.csproj` | the API, namespace `Daylight`: P/Invoke on `daylight_c.dll` |

The first call loads `daylight_c.dll` by its absolute path: `DAYLIGHT_DLL` if set, else the one beside `Daylight.dll`;
without either the host's own search finds it (a Unity player's `Plugins`, the app folder, `PATH`). Then it checks the
ABI version (1). The library is never unloaded: the GPU context and one resident scene live until the process exits.
To run: Windows x64, a 64-bit process, an NVIDIA GPU (Turing or newer) with driver R570+ (no CUDA toolkit), and the
Visual C++ 2015-2022 x64 runtime (installed, or `msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll`,
`vcruntime140_1.dll` copied beside `daylight_c.dll`).

| variable | |
|---|---|
| `DAYLIGHT_DLL` | the path of `daylight_c.dll`; the file keeps that name (DllImport finds it by it). Read until the library is loaded |
| `CUDA_VISIBLE_DEVICES` | the GPU, set before the process starts |

**.NET.** Reference `bindings/csharp/Daylight.csproj` (or `Daylight.dll`), build for x64, and copy `daylight_c.dll`
into the output folder or set `DAYLIGHT_DLL`. The example's project,
[Example.csproj](../examples/csharp/Example.csproj):

```xml
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net9.0</TargetFramework>
    <PlatformTarget>x64</PlatformTarget>
  </PropertyGroup>

  <ItemGroup>
    <ProjectReference Include="../../bindings/csharp/Daylight.csproj" />
  </ItemGroup>
```

Its program finds the repo above its `bin` folder and, unless `DAYLIGHT_DLL` is set, points it at the CMake build
before the first call:

```csharp
string repo = AppContext.BaseDirectory;
while (!File.Exists(Path.Combine(repo, "tests", "box.obj")))
    repo = Path.GetDirectoryName(repo) ?? throw new FileNotFoundException("no tests/box.obj above the program");
if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("DAYLIGHT_DLL")))
    Environment.SetEnvironmentVariable("DAYLIGHT_DLL", Path.Combine(repo, "build", "RelWithDebInfo", "daylight_c.dll"));
```

**Rhino 6 / 7 and Grasshopper** (.NET Framework 4.8). A plug-in (`.gha`, `.rhp`) targets `net48` with
`<PlatformTarget>x64</PlatformTarget>` (Rhino is 64-bit) and references `Daylight.dll`. Copy `Daylight.dll` and
`daylight_c.dll` into the plug-in's folder, beside its `.gha` or `.rhp`, where `Daylight.dll` finds `daylight_c.dll`;
Rhino brings the VC++ runtime. A C# script component references `Daylight.dll` where it lies, `daylight_c.dll` beside
it (Rhino 6 / 7: Manage Assemblies in the component's menu; Rhino 8's script editor: `#r "<folder>/Daylight.dll"`).
A `.gha` that Grasshopper loads from bytes (its COFF loading option) has no folder: set `DAYLIGHT_DLL`. Unblock a
downloaded zip (`Unblock-File`) before unpacking it: .NET Framework refuses a DLL that carries the mark of the web. A
`net48` project compiles C# 7.3 by default: `using (...) { }` blocks and a `Main` instead of the example's `using`
declarations and top-level statements. Rhino is Z up and right-handed like the core; scale a model in millimetres to
metres (`RhinoMath.UnitScale(doc.ModelUnitSystem, UnitSystem.Meters)`). A Rhino mesh goes in as `SimulationMesh.Create`'s
arrays: its vertices (scaled) as `xyz`, per face 4 (`MeshFace.IsQuad`) or 3 as `counts`, and `A`, `B`, `C` (and `D`)
as `indices`, so each Rhino face is a row, in order. `Rhino.Geometry.Mesh` does not clash with `SimulationMesh` (the core's type is named so). Rhino 8,
on .NET 7 / 8 by default, loads the same `Daylight.dll`.

**Unity.** Put both DLLs in `Assets/Plugins/x86_64/` and set `daylight_c.dll` to Editor and Standalone, Windows x86_64
(Inspector). The editor loads it beside `Daylight.dll`, a player from its `Plugins` folder. IL2CPP may need a
`link.xml` that keeps `System.Runtime.Serialization` (`DataContractJsonSerializer` reads the manifest). Unity is Y up
and left-handed: pass world points (`transform.TransformPoint`) as (x, z, y) and reverse each triangle's winding.

Only .NET 9 and .NET Framework 4.8 run the binding's check; Rhino, Grasshopper and Unity have not run it.

## Daylight

[examples/csharp/Program.cs](../examples/csharp/Program.cs), commented step by step, loads `tests/box.obj` (a ground
of four 2 m quads at z 0, a 1 m box floating 0.5 m above the fourth), makes a facade from arrays and runs a clear-sky
year on all three meshes (its steps 1 to 3). The key calls:

```csharp
using SimulationMesh model = SimulationMesh.Load("tests/box.obj");                      // meshes "ground", "box"
using SimulationMesh facade = SimulationMesh.Create("facade", xyz, counts, indices);    // from arrays
SimulationMesh[] sensors = { model, facade };
Result year = Core.Run(new DaylightOptions { ClearSky = true, Lat = 52.52, Lon = 13.41 }, sensors);
// the same with the CLI's flags as strings:
// Core.Run(Tool.Daylight, new[] { "--clear-sky", "--lat", "52.52", "--lon", "13.41" }, sensors);
```

```
daylight: 421 ms, scene reused False
  ground  4 faces  radiation 1394.1 kWh/m2
  box    12 faces  radiation  931.2 kWh/m2
  facade  2 faces  radiation 1489.1 kWh/m2
```

- `SimulationMesh.Load` gives one handle holding the meshes of an `.obj` (one per `o` / `g` group, named by it) or a `.dmesh`
  (named by its stem); `model.Names` is `ground`, `box`. USD is not read in-process: pass its meshes as arrays.
  `SimulationMesh.Create(name, xyz, counts, indices)` copies arrays: 3 doubles per point (metres, Z up), vertices per face
  (>= 3), 0-based indices in right-handed winding (the facade's normal points to -y, south at `--north 0`). The names
  are what the manifest, `--target`, `--optics` and `--no-occlude` use.
- `Core.Run(options, sensors, context = null, control = null, float32 = false)`: `options` are a `DaylightOptions` or
  `ViewsOptions`, a property per CLI flag (`ClearSky` is `--clear-sky`, `Quantities` `--quantity`; unset ones keep the
  core's defaults; [api.md](api.md#typed-options)), and pick the tool. `Core.Run(tool, args, ...)` takes the flags as
  strings, without the program name. `sensors` take the place of the CLI's mesh arguments, `context` (blocks only,
  never analysed) of `--context FILE`.
  Sensor meshes block too, unless `--no-self-occlude` or `--no-occlude NAME`. On the command line this run is
  `daylight tests/box.obj facade.obj --clear-sky --lat 52.52 --lon 13.41`.
- `--clear-sky` makes an hourly cloudless year (Ineichen-Perez) for `--lat` / `--lon`, in the clock `--tz` (default
  `round(lon / 15)`, 1 here): an upper bound, for comparisons ([modes.md](modes.md)). With weather: `--epw site.epw`.
  Without time flags the run is annual; `--time`, `--from` / `--to` / `--step` and `--hours` choose instants.
- The default quantity `radiation` gives the column `radiation_front`, kWh/m2 summed over the year;
  `--quantity radiation,sunhours,...` adds a column each ([daylight.md](daylight.md)).
- Per face: the ground quads get 1599-1605 kWh/m2 in the open, 1462 beside the box and 911 under it; the box from 39
  (its bottom, lit only by the ground light beyond the ground quads, which are black) to 1621 (its south side); the
  facade's lower panel 1360, its upper one 1619: the box shades the lower panel when the sun is low and hides part of
  its sky.

## Views

The same call with `ViewsOptions`, the views CLI's flags, on the same meshes (the example's step 4):

```csharp
var viewFlags = new ViewsOptions { Targets = new[] { new TargetSpec("box", "block") }, Reach = new double[] { 6, 20, 50 } };
Result view = Core.Run(viewFlags, sensors);   // or Core.Run(Tool.Views, new[] { "--target", "box=block", "--reach", "en17037" }, sensors)
```

```
views: 1 ms, scene reused True
  ground  4 faces  sky 0.78  block 0.18  distance 524.4 m  sees 20 m 0.78
  box    12 faces  sky 0.44  block 0.00  distance 185.7 m  sees 20 m 0.45
  facade  2 faces  sky 0.40  block 0.10  distance  28.6 m  sees 20 m 0.42
```

- The default field is 120 deg wide about each face's normal, from 70 deg down to 50 deg up, a ray per 2 deg cell
  (3,600 per face). `--fov 360 --offset 1.6` makes a floor or ground grid a standing eye that looks all around
  ([views.md](views.md#field-of-view)); all around, the default column is the isovist: `volume` (m3), or `area` (m2)
  with horizontal rays only (`--fov 360,0,0`), as the example's isovist runs in tests/room.obj show
  ([views.md](views.md#isovist)).
- `TargetSpec("box", "block")` (`--target box=block`): rays that end on the mesh `box` count for the group `block` (a
  column `block_front`) instead of `obstruction`. NAME is a sensor or context mesh; several meshes may share a group
  (at most 16 groups).
- `Reach` 6, 20, 50 (`--reach en17037`) adds `reach_6`, `reach_20` and `reach_50`: the shares of the field that see at
  least 6, 20 and 50 m, EN 17037's view distance levels (no level is assigned).
- The columns: `sky_front ground_front obstruction_front obstruction_total_front block_front distance_front
  distance_min_front reach_6_front reach_20_front reach_50_front`. The four shares `sky`, `ground`, `obstruction` and
  `block` sum to 1 per face; `obstruction_total` is all that is not sky (1 - `sky`). `distance` is the log (geometric) mean over the
  field, open rays counting as `--far` (1000 m); `distance_min` is the nearest thing in view.
- Per face, the facade's panels (rows 16 and 17):

  | panel | sky | ground | obstruction | block | distance | distance_min | reach_20 |
  |---|---|---|---|---|---|---|---|
  | lower, eye at z 0.5 | 0.349 | 0.423 | 0.128 | 0.100 | 14.3 m | 0.53 m | 0.364 |
  | upper, eye at z 1.5 | 0.449 | 0.291 | 0.161 | 0.099 | 42.9 m | 1.50 m | 0.479 |

  The upper eye is level with the box's top, so nothing rises above its horizon: its sky is exactly
  sin 50 / (sin 50 + sin 70) = 0.449. The lower eye sees the box above its horizon. Obstruction is the ground quads
  (sensor meshes block, and they are no target); ground is the virtual ground at z 0 between them and the facade and
  beyond them. The lower panel's nearest thing is that virtual ground at the field's lower edge, 0.5 / sin 70 =
  0.53 m; the upper one's is the box, 1.5 m ahead.
- `SceneReused` is true: the views run used the scene the daylight run built (the same `SimulationMesh` objects in the same roles
  and order; daylight and views share an opaque scene, and targets are labels per run).

## Results

| `Result` | |
|---|---|
| `Columns` | column names, e.g. `radiation_front`, `sky_front`, `reach_20_front` |
| `Rows` | the elements of all sensor meshes: faces, or points with `--on vertices` |
| `Values` | `Rows x Columns.Length` doubles, row-major: row i, column c at `Values[i * Columns.Length + c]`; null after `float32: true` |
| `Values32` | the same as floats (half the memory) after `Core.Run(..., float32: true)`, else null |
| `Column(name)` | one column, a value per row (from either); an unknown name throws `KeyNotFoundException` |
| `Meshes` | each sensor mesh's `Name`, `First` row and row `Count` (the manifest's `meshes`) |
| `Manifest` | the manifest JSON as the CLI writes it to `FILE.json`, with LF line ends |
| `SceneReused`, `Stats` | a resident scene served the run; `RunStats`: `SceneReused`, `PrepMs`, `SkyMs`, `BuildMs`, `TraceMs`, `ReduceMs`, `BounceMs`, `Rays`, `Launches`, `GasBytes` |

**Rows per mesh.** Rows follow the sensors in the order given, each handle's meshes in order (an OBJ's groups as in
the file), each mesh's faces (or points) in its own order; nothing is reordered or dropped. Here `ground` is rows 0-3,
`box` 4-15 and `facade` 16-17. A quad is one row, its value at its centroid. The example's `Mean` (the last lines of
Program.cs) averages a mesh's rows: `r.Column(column).Skip(First).Take(Count).Average()`, a plain mean of its faces
(weight by face area where they differ in size).

**Columns** are `<name>_<side>`, sides inner (`--sides both`: `_front` and `_back`). daylight: one per `--quantity`, in
its order. views: the shares `sky`, `ground`, `obstruction`, `obstruction_total` (all that is not sky) and one per target group (in order
of first appearance in `--target`), with `--transmission on` `glazing`, then `volume` or `area` (the isovist), `distance` and `distance_min`
(`--distance`; all around the default is the isovist alone, about the normal the two distances), per group
`<g>_distance` and `<g>_distance_min`
(`--distance group`), then `reach_<D>`.

| column | unit |
|---|---|
| `radiation`, `direct`, `diffuse` | kWh/m2 summed over the evaluated steps |
| `irradiance` | W/m2, mean over the evaluated hours (instantaneous for one `--time`) |
| `illuminance` | lux, mean over the evaluated hours |
| `sunhours` | hours (equivalent full-sun hours behind translucent slots, `--transmission on`) |
| `sky`, `ground`, `obstruction`, `obstruction_total`, `<g>`, `reach_<D>` | share of the field's solid angle, 0..1 |
| `distance`, `distance_min`, `<g>_distance`, `<g>_distance_min` | metres, 0.1 to `--far` (1000) |
| `volume`, `area` | m3, m2: the isovist, each ray at most `--far` long |

**NaN.** views: `distance` and `distance_min` are NaN when no ray of a face's field is traced (`--fov 360` at
`--offset 0` with the whole field behind the face, a degenerate face), `<g>_distance` and `<g>_distance_min` when the
group is not in view: skip them (`double.IsNaN`) before a mean. daylight gives a degenerate face, or one looking into a
wall, 0.

**Manifest.** `Columns` and `Meshes` come from it; the rest is there for a JSON reader: daylight's `duration_h` and
`dirs`, views' `view` (the field, `grid`, `far`, `ground_z`, `reach`, `classes`) and `targets`
([daylight.md](daylight.md#output), [views.md](views.md#output)).

## Speed

The first run in a process starts the GPU (CUDA context and OptiX pipeline, about 0.35 s on an RTX 2060) and builds the
scene: triangulation, upload, acceleration structure. Runs over the same `SimulationMesh` objects in the same roles and order
reuse it (`SceneReused`): no triangulation, upload or build, only the run itself. daylight and views share an opaque
scene, and flags (quantities, times, the field, targets) leave it alone. In the example the first run, the clear-sky
year included, took 0.4-0.9 s, the views run 1-5 ms and the sun hours, async (a rebuild: the room's isovist runs
replaced the scene), 4-15 ms; a CLI run of such a model pays
0.6-0.7 s every time ([performance.md](performance.md#in-process-vs-cli)).

The scene is built again for other `SimulationMesh` objects (a new handle, even from the same arrays), other roles or order,
`--no-self-occlude`, another `--no-occlude` set, a slot that starts or stops transmitting (`--optics`,
`--transmission on`), and with `--bounces` slots that start or stop differing. One scene stays resident: a run over
other meshes replaces it. So keep the `SimulationMesh` objects while the model stands (a Grasshopper component: across
solutions, until its geometry input changes) and dispose them when it changes. Runs take turns: one GPU run per
process at a time, a `Run` waits for the one before it.

## Errors, cancel, progress

The example's step 6 runs the sun hours of 21 June in the background:

```csharp
using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(30));   // in an app: a Cancel button
using var control = new RunControl();
Task<Result> run = Core.RunAsync(june21, sensors, control: control, token: cancel.Token);   // june21: DaylightOptions
while (await Task.WhenAny(run, Task.Delay(100)) != run) Console.WriteLine($"  {control.Progress:P0} traced");
Result sun = await run;
```

- `Core.Run` blocks its thread; `Core.RunAsync` runs it on the thread pool, for a UI that has to stay responsive.
- A cancelled token cancels the run (through the control given, or one of its own): the run stops at its next check
  (before the sky, per trace chunk) and the task ends cancelled, so `await` throws `OperationCanceledException`; a
  cancel after the last check lets the run finish. `RunControl.Cancel()` from any thread does the same to `Core.Run`,
  which then throws `DaylightException` with status 2, `cancelled`. One control per run: its flag stays set.
- `RunControl.Progress` is the share of sensors traced, 0..1, after each trace chunk, and 1 once the run succeeded. Poll
  it (no callbacks cross the C ABI). A run as small as the example's ends before the first 100 ms tick.

Errors are exceptions; the example catches `DaylightException` (its `Status` is the CLI's exit code) and
`OperationCanceledException` at its end (step 7):

| exception | when |
|---|---|
| `DaylightException`, `Status` 1 | bad flags or values (`unknown flag --bogus`), bad arrays in `SimulationMesh.Create`, names the flags do not resolve (`--target nosuch=g`), a name holding `;` in `Optics`, `Targets` or `NoOcclude`, flags refused in-process: mesh paths, `--context`, `--out`, `--bench`, `-h` (`Core.Usage(tool)` gives the `-h` text) |
| `DaylightException`, `Status` 2 | the run failed: a file (`cannot open EPW site.epw`, a `SimulationMesh.Load` path), the GPU, `out of memory: ...`, `cancelled` |
| `OperationCanceledException` | `RunAsync`'s token was cancelled |
| `DllNotFoundException` | `daylight_c.dll` missing or not loadable: the message says where it looked and what it needs; the next call tries again |
| `ObjectDisposedException` | a disposed `SimulationMesh` or `RunControl` given to a run |
| `KeyNotFoundException` | `Result.Column` of a column the run does not have |
| `ArgumentException` | a `SimulationMesh.Load` path with a character outside the ANSI code page |

`Message` is the core's, as the CLI prints it. Flags travel as UTF-8 and `SimulationMesh.Load` paths in the ANSI code page, so
paths inside flags (`--epw`, `--timeseries`, `--dump-*`) must be ASCII. Numbers in string flags need `.` as the decimal
point (`--lat 52,52` is a usage error, status 1): format them with `CultureInfo.InvariantCulture`. Typed options write
their numbers so in any culture.

## Reference

- [bindings/csharp/README.md](../bindings/csharp/README.md): the whole API (`SimulationMesh`, `Core`, `DaylightOptions`,
  `ViewsOptions`, `Result`, `RunStats`, `MeshRange`, `RunControl`, `DaylightException`), loading, hosts.
- [api.md](api.md#typed-options): the typed options' rules, shared with Python and TypeScript.
- `Core.Version`: the C ABI version of the loaded `daylight_c.dll` (1).
- [daylight.md](daylight.md): every daylight flag, rows, columns, units, the manifest. [views.md](views.md): every
  views flag, the field, ray classes, distances, roles, outputs, worked examples.
- [c-abi.md](c-abi.md): the C ABI under the binding: resident scenes, cancel, threads, limits.
- `Core.Usage(Tool.Daylight)`, `Core.Usage(Tool.Views)`: the CLIs' `-h` text.

## Examples

Every example, in every language, and `examples/run.ps1`, which runs them all: [examples.md](examples.md).

Run from the repo root after the core's build; `dotnet run` builds `Daylight.dll` too. About 3 s through `dotnet run`
(its build check included), under 1 s as the built `bin/Debug/net9.0/Example.exe`; the three runs take 0.4-0.9 s.

| file | shows | run |
|---|---|---|
| [examples/csharp/Program.cs](../examples/csharp/Program.cs) | commented step by step, on typed options: `tests/box.obj` and a facade from arrays, a clear-sky year (daylight), the views with the box as a target and EN 17037's reach levels on the resident scene, the isovists of `tests/room.obj`, sun hours on 21 June through `RunAsync` with a token and progress; means per mesh from the manifest ranges | `dotnet run --project examples/csharp` |
| [examples/csharp/Example.csproj](../examples/csharp/Example.csproj) | net9.0 console, x64, `ProjectReference` to `bindings/csharp/Daylight.csproj` | |

`DAYLIGHT_DLL` points it at another `daylight_c.dll`. Output on an RTX 2060 (times vary):

```
daylight: 421 ms, scene reused False
  ground  4 faces  radiation 1394.1 kWh/m2
  box    12 faces  radiation  931.2 kWh/m2
  facade  2 faces  radiation 1489.1 kWh/m2
views: 1 ms, scene reused True
  ground  4 faces  sky 0.78  block 0.18  distance 524.4 m  sees 20 m 0.78
  box    12 faces  sky 0.44  block 0.00  distance 185.7 m  sees 20 m 0.45
  facade  2 faces  sky 0.40  block 0.10  distance  28.6 m  sees 20 m 0.42
isovists in the room: volume 92.8 m3 all around, area 32.1 m2 at eye height
sun hours, 21 June: 4 ms, scene reused False, progress 100 %
  ground  4 faces  14.1 h
  box    12 faces   8.3 h
  facade  2 faces   9.3 h
```
