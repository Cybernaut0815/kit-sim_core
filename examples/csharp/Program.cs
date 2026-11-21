// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
//
// daylight and views in-process from C#, through Daylight.dll (guide: docs/csharp.md)
//
// The steps:
//   1. the meshes: tests/box.obj from its file, a small facade from arrays
//   2. daylight: a clear-sky year in Berlin, kWh/m2 per face
//   3. reading a result: a column, the rows of each mesh
//   4. views: 120 deg about each face's normal, the box as a target group; this run reuses the GPU scene of step 2
//   5. isovists: the volume and the area a standing eye sees in the room of tests/room.obj
//   6. a run in the background: RunAsync with a cancellation token and progress
//   7. errors (the catch blocks at the end)
//
// Needs the core's build (daylight_c.dll) and the .NET 9 SDK.
// Run from the repo root:   dotnet run --project examples/csharp
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Daylight;

// Print 0.5, not 0,5, whatever the machine's language. Flags need a '.' too: "--lat 52,52" would read 52.
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

// Find the repo: go up from bin/Debug/net9.0 to the folder that holds tests/box.obj.
string repo = AppContext.BaseDirectory;
while (!File.Exists(Path.Combine(repo, "tests", "box.obj")))
    repo = Path.GetDirectoryName(repo) ?? throw new FileNotFoundException("no tests/box.obj above the program");

// Daylight.dll loads daylight_c.dll on the first call: from DAYLIGHT_DLL if set, here the CMake build's.
if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("DAYLIGHT_DLL")))
    Environment.SetEnvironmentVariable("DAYLIGHT_DLL", Path.Combine(repo, "build", "RelWithDebInfo", "daylight_c.dll"));

try
{
    // ---- Step 1: the meshes ------------------------------------------------------------------------------------
    // A SimulationMesh holds meshes the core keeps (copied once). Both here are sensors: their faces are analysed (a
    // result row each) and block rays. `using`: freed at the end of the program.

    // tests/box.obj: two OBJ groups, two meshes: "ground" (four 2 m quads at z 0) and "box" (a 1 m box, 12 triangles)
    using SimulationMesh model = SimulationMesh.Load(Path.Combine(repo, "tests", "box.obj"));

    // A facade of two 2 x 1 m panels at y = 3, facing south (-y) to the box, from arrays:
    double[] xyz = { 0, 3, 0,   2, 3, 0,   2, 3, 1,   0, 3, 1,   2, 3, 2,   0, 3, 2 };   // points: x y z, metres, Z up
    int[] counts = { 4, 4 };                                                       // vertices per face: two quads
    int[] indices = { 0, 1, 2, 3,   3, 2, 4, 5 };   // 0-based, counter-clockwise seen from the front (the analysed side)
    using SimulationMesh facade = SimulationMesh.Create("facade", xyz, counts, indices);

    SimulationMesh[] sensors = { model, facade };

    // ---- Step 2: daylight, a clear-sky year in Berlin ----------------------------------------------------------
    // The flags are the CLI's (docs/daylight.md). No weather file: --clear-sky makes a cloudless year for the place.
    // The default column is radiation, kWh/m2 over the year. The first run starts the GPU.
    string[] clearSkyYear = { "--clear-sky", "--lat", "52.52", "--lon", "13.41" };   // Berlin
    var clock = Stopwatch.StartNew();
    Result year = Core.Run(Tool.Daylight, clearSkyYear, sensors);
    Console.WriteLine($"daylight: {clock.ElapsedMilliseconds} ms, scene reused {year.SceneReused}");

    // ---- Step 3: reading the result ----------------------------------------------------------------------------
    // year.Meshes lists each sensor mesh (Name, First row, row Count), year.Column(name) gives one column, a value per
    // row: the mean over a mesh is its rows' average (Mean, at the end of this file).
    foreach (MeshRange m in year.Meshes)
        Console.WriteLine($"  {m.Name,-6} {m.Count,2} faces  radiation {Mean(year, "radiation_front", m),6:F1} kWh/m2");

    // ---- Step 4: views about each face's normal ----------------------------------------------------------------
    // From every face a ray per 2 deg cell of a field 120 deg wide about its normal: the shares that end on the sky,
    // the ground, an obstruction or a target group, the log-mean distance (an open ray counts as 1000 m) and the shares
    // that see at least 6, 20 and 50 m. The same meshes in the same roles as step 2: the resident scene serves it.
    string[] viewFlags =
    {
        "--target", "box=block",   // rays ending on the mesh "box" count for the group "block" (column block_front)
        "--reach", "en17037",      // reach_6, reach_20, reach_50: the shares that see at least 6, 20, 50 m
    };
    clock.Restart();
    Result view = Core.Run(Tool.Views, viewFlags, sensors);
    Console.WriteLine($"views: {clock.ElapsedMilliseconds} ms, scene reused {view.SceneReused}");
    foreach (MeshRange m in view.Meshes)
        Console.WriteLine($"  {m.Name,-6} {m.Count,2} faces  sky {Mean(view, "sky_front", m):F2}  block " +
                          $"{Mean(view, "block_front", m):F2}  distance {Mean(view, "distance_front", m),5:F1} m  " +
                          $"sees 20 m {Mean(view, "reach_20_front", m):F2}");

    // ---- Step 5: isovists in a room ----------------------------------------------------------------------------
    // tests/room.obj: a 6.3 x 5.1 x 2.9 m room with a window and a tree outside; tests/room_grid.obj: 12 measuring
    // points at desk height. The isovist is what an eye sees around it: all around in 3D the volume in view (column
    // volume_front, m3), with horizontal rays only the area in view (area_front, m2).
    using SimulationMesh grid = SimulationMesh.Load(Path.Combine(repo, "tests", "room_grid.obj"));   // sensor: the points
    using SimulationMesh room = SimulationMesh.Load(Path.Combine(repo, "tests", "room.obj"));        // context: room, window, tree
    string[] volumeFlags =
    {
        "--fov", "360,-90,90",    // all around, the whole sphere
        "--offset", "0.4",        // the eye 0.4 m above each grid face
        "--no-occlude", "grid",   // the grid is measured, not seen: the rays pass it
    };
    string[] areaFlags = { "--fov", "360,0,0", "--offset", "0.4", "--no-occlude", "grid" };   // horizontal rays only
    // The isovist is the default column all around; --distance mean,min gives the distances instead, --far M caps both.
    Result volume = Core.Run(Tool.Views, volumeFlags, new[] { grid }, new[] { room });   // sensors, then context
    Result area = Core.Run(Tool.Views, areaFlags, new[] { grid }, new[] { room });
    Console.WriteLine($"isovists in the room: volume {volume.Column("volume_front").Average():F1} m3 all around, " +
                      $"area {area.Column("area_front").Average():F1} m2 at eye height");

    // ---- Step 6: a run in the background -----------------------------------------------------------------------
    // RunAsync returns a Task: the program (a UI) stays free meanwhile. The token cancels the run (here a 30 s
    // timeout, in an app a Cancel button), the RunControl reports its progress. Sun hours need no weather, only the
    // place and its clock (--tz), here every 10 minutes of 21 June.
    string[] june21 =
    {
        "--quantity", "sunhours",
        "--lat", "52.52", "--lon", "13.41", "--tz", "1",           // Berlin, UTC+1
        "--from", "06-21T00:00", "--to", "06-22T00:00", "--step", "10",
    };
    using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(30));
    using var control = new RunControl();
    clock.Restart();
    Task<Result> run = Core.RunAsync(Tool.Daylight, june21, sensors, control: control, token: cancel.Token);
    while (await Task.WhenAny(run, Task.Delay(100)) != run)   // every 100 ms until the run is done
        Console.WriteLine($"  {control.Progress:P0} traced");
    Result sun = await run;
    Console.WriteLine($"sun hours, 21 June: {clock.ElapsedMilliseconds} ms, scene reused {sun.SceneReused}, " +
                      $"progress {control.Progress:P0}");
    foreach (MeshRange m in sun.Meshes)
        Console.WriteLine($"  {m.Name,-6} {m.Count,2} faces  {Mean(sun, "sunhours_front", m),4:F1} h");
    return 0;
}
// ---- Step 7: errors ------------------------------------------------------------------------------------------------
catch (DaylightException e)   // Status: the CLI's exit code, 1 bad flags or arrays, 2 the run failed (a file, the GPU)
{
    Console.Error.WriteLine($"daylight error {e.Status}: {e.Message}");
    return e.Status;
}
catch (OperationCanceledException)   // the token cancelled RunAsync: its task ends cancelled
{
    Console.Error.WriteLine("cancelled");
    return 2;
}

// The mean of one column over one mesh's rows: Count rows from First (the manifest's range for that mesh).
static double Mean(Result r, string column, MeshRange m) =>
    r.Column(column).Skip((int)m.First).Take((int)m.Count).Average();
