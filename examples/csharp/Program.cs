// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// daylight and views in-process through Daylight.dll (docs/csharp.md) on tests/box.obj (meshes "ground", "box") and a
// facade from arrays: a clear-sky year, the views with the box as a target, then sun hours in the background, the
// three runs on one resident GPU scene. Run: dotnet run --project examples/csharp
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Daylight;

CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;   // print 0.5, not 0,5; flags need '.': --lat 52,52 reads 52

// The repo: up from bin/Debug/net9.0 to the folder holding tests/box.obj. daylight_c.dll: DAYLIGHT_DLL, else the
// CMake build's, set before the first call (which loads it).
string repo = AppContext.BaseDirectory;
while (!File.Exists(Path.Combine(repo, "tests", "box.obj")))
    repo = Path.GetDirectoryName(repo) ?? throw new FileNotFoundException("no tests/box.obj above the program");
if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("DAYLIGHT_DLL")))
    Environment.SetEnvironmentVariable("DAYLIGHT_DLL", Path.Combine(repo, "build", "RelWithDebInfo", "daylight_c.dll"));

try
{
    // Sensor meshes (analysed, and blocking): box.obj's ground (4 quads) and box (12 triangles), and a facade of two
    // 2 x 1 m panels at y = 3 facing south (-y) to the box: xyz (metres, Z up), vertices per face, 0-based indices.
    using Mesh model = Mesh.Load(Path.Combine(repo, "tests", "box.obj"));
    using Mesh facade = Mesh.Create("facade", new double[] { 0, 3, 0, 2, 3, 0, 2, 3, 1, 0, 3, 1, 2, 3, 2, 0, 3, 2 },
                                    new[] { 4, 4 }, new[] { 0, 1, 2, 3, 3, 2, 4, 5 });
    Mesh[] sensors = { model, facade };

    // daylight: a clear-sky year in Berlin (no weather file), kWh/m2 per face. The first run starts the GPU.
    var clock = Stopwatch.StartNew();
    Result year = Core.Run(Tool.Daylight, new[] { "--clear-sky", "--lat", "52.52", "--lon", "13.41" }, sensors);
    Console.WriteLine($"daylight: {clock.ElapsedMilliseconds} ms, scene reused {year.SceneReused}");
    foreach (MeshRange m in year.Meshes)
        Console.WriteLine($"  {m.Name,-6} {m.Count,2} faces  radiation {Mean(year, "radiation_front", m),6:F1} kWh/m2");

    // views: the shares of each face's field (120 deg about its normal) that end on sky, ground, obstruction or the
    // group "block" (rays ending on the mesh "box"), the log-mean distance and the shares that see 6, 20 and 50 m.
    // The same meshes in the same roles: the resident scene serves it.
    clock.Restart();
    Result view = Core.Run(Tool.Views, new[] { "--target", "box=block", "--reach", "en17037" }, sensors);
    Console.WriteLine($"views: {clock.ElapsedMilliseconds} ms, scene reused {view.SceneReused}");
    foreach (MeshRange m in view.Meshes)
        Console.WriteLine($"  {m.Name,-6} {m.Count,2} faces  sky {Mean(view, "sky_front", m):F2}  block " +
                          $"{Mean(view, "block_front", m):F2}  distance {Mean(view, "distance_front", m),5:F1} m  " +
                          $"sees 20 m {Mean(view, "reach_20_front", m):F2}");

    // In the background: the token cancels the run (a timeout here, a Cancel button in an app), the control reports
    // progress meanwhile. Sun hours need no weather, only the site and its clock (--tz).
    string[] june21 = { "--quantity", "sunhours", "--lat", "52.52", "--lon", "13.41", "--tz", "1",
                        "--from", "06-21T00:00", "--to", "06-22T00:00", "--step", "10" };
    using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(30));
    using var control = new RunControl();
    clock.Restart();
    Task<Result> run = Core.RunAsync(Tool.Daylight, june21, sensors, control: control, token: cancel.Token);
    while (await Task.WhenAny(run, Task.Delay(100)) != run) Console.WriteLine($"  {control.Progress:P0} traced");
    Result sun = await run;
    Console.WriteLine($"sun hours, 21 June: {clock.ElapsedMilliseconds} ms, scene reused {sun.SceneReused}, " +
                      $"progress {control.Progress:P0}");
    foreach (MeshRange m in sun.Meshes)
        Console.WriteLine($"  {m.Name,-6} {m.Count,2} faces  {Mean(sun, "sunhours_front", m),4:F1} h");
    return 0;
}
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

// One column's mean over one mesh's rows: Count rows from First (the manifest's range).
static double Mean(Result r, string column, MeshRange m) =>
    r.Column(column).Skip((int)m.First).Take((int)m.Count).Average();
