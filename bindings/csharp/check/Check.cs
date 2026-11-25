// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// Check of the C# binding: tests/c_abi.c's cases through Daylight.dll, the in-process results against the CLI's
// build/smoke.npy and build/views_smoke.npy (ctest cli_smoke_npy, views_smoke_npy) byte for byte. One line per
// failure, then "check OK"; exit code 1 on a failure. DAYLIGHT_DLL defaults to build/RelWithDebInfo/daylight_c.dll.
// Run: dotnet run --project bindings/csharp/check -f net9.0   (or -f net48)
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using Daylight;

/// <summary>The cases and their helpers.</summary>
static class Check
{
    static int failures;

    static int Main()
    {
        try
        {
            Cases();
        }
        catch (Exception e)
        {
            failures++;
            Console.WriteLine("FAIL: " + e);
        }
        Console.WriteLine(failures == 0 ? "check OK" : "check: " + failures + " failure(s)");
        return failures == 0 ? 0 : 1;
    }

    static void Cases()
    {
        string repo = AppContext.BaseDirectory;
        while (repo != null && !File.Exists(Path.Combine(repo, "tests", "box.obj"))) repo = Path.GetDirectoryName(repo);
        if (repo == null) throw new Exception("no tests/box.obj above " + AppContext.BaseDirectory);
        string tests = Path.Combine(repo, "tests"), build = Path.Combine(repo, "build");
        string dll = Environment.GetEnvironmentVariable("DAYLIGHT_DLL");
        if (string.IsNullOrEmpty(dll)) dll = Path.Combine(build, "RelWithDebInfo", "daylight_c.dll");

        // A. loading, version and usage: a missing library throws a clear exception, a later call loads the right one
        Environment.SetEnvironmentVariable("DAYLIGHT_DLL", Path.Combine(build, "nosuch", "daylight_c.dll"));
        DllNotFoundException missing = Thrown<DllNotFoundException>(() => Core.Usage(Tool.Daylight));
        Expect(missing != null && missing.Message.Contains("no such file"), "a missing DAYLIGHT_DLL: " + missing?.Message);
        Environment.SetEnvironmentVariable("DAYLIGHT_DLL", Path.Combine(build, "nosuch", "daylight_c_v2.dll"));
        missing = Thrown<DllNotFoundException>(() => Core.Usage(Tool.Daylight));   // DllImport would not find it renamed
        Expect(missing != null && missing.Message.Contains("called daylight_c.dll"),
               "a renamed DAYLIGHT_DLL: " + missing?.Message);
        Environment.SetEnvironmentVariable("DAYLIGHT_DLL", dll);
        Expect(Core.Usage(Tool.Daylight).Contains("usage: daylight"), "daylight usage");   // checks dl_version() == 1 first
        Expect(Core.Usage(Tool.Views).Contains("usage: views"), "views usage");
        Expect(Core.Usage((Tool)7) == null, "an unknown tool's usage");

        // B. errors before the GPU
        double[] fxyz = { -1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2 };   // a 2 x 2 m facade facing -y, its foot at z 0
        int[] quad = { 4 }, fidx = { 0, 1, 2, 3 };
        SimulationMesh facade = SimulationMesh.Create("facade", fxyz, quad, fidx);
        Expect(facade.Names.SequenceEqual(new[] { "facade" }), "the facade's name");
        ExpectError(() => SimulationMesh.Create("bad", fxyz, quad, new[] { 0, 1, 2, 4 }), 1, "bad: vertex index out of range");
        ExpectError(() => SimulationMesh.Create("two", fxyz, new[] { 2 }, new[] { 0, 1 }), 1, "fewer than 3 vertices");
        ExpectError(() => SimulationMesh.Create(null, fxyz, quad, fidx), 1, "name is NULL");
        ExpectError(() => SimulationMesh.Create("odd", new double[4], quad, fidx), 1, "odd: xyz holds 4 values");
        ExpectError(() => SimulationMesh.Create("slots", fxyz, quad, fidx, new ushort[2]), 1, "slots: 2 slots for 1 faces");
        using (SimulationMesh empty = SimulationMesh.Create("empty", null, null, null)) Expect(empty.Names.Count == 0, "a mesh without faces");
        ExpectError(() => SimulationMesh.Load(Path.Combine(tests, "nosuch.obj")), 2, "cannot open");
        ExpectError(() => SimulationMesh.Load("x.usd"), 2, "unknown extension");
        ExpectError(() => SimulationMesh.Load(null), 1, "path is NULL");
        // the message names the path as given, in the ANSI code page (one without e-diaeresis throws ArgumentException)
        string ansi = Path.Combine(tests, "nosuch-ë.obj");
        Exception unread = Thrown<Exception>(() => SimulationMesh.Load(ansi));
        Expect(unread is ArgumentException || unread is DaylightException && unread.Message.Contains(ansi),
               "a Load error names its path: " + unread?.Message);
        ExpectError(() => Core.Run((Tool)7, null, new[] { facade }), 1, "unknown tool");
        ExpectError(() => Core.Run(Tool.Views, null, new SimulationMesh[] { null }), 1, "NULL mesh handle");
        ExpectError(() => Core.Run(Tool.Views, null, new SimulationMesh[0]), 1, "no mesh given");
        ExpectError(() => Run(Tool.Daylight, facade, "--bogus"), 1, "unknown flag --bogus");
        ExpectError(() => Run(Tool.Views, facade, "x.obj"), 1, "dl_mesh_load");
        ExpectError(() => Run(Tool.Views, facade, "--context", "c.obj"), 1, "--context");
        ExpectError(() => Run(Tool.Daylight, facade, "--out", "v.npy"), 1, "--out");
        ExpectError(() => Run(Tool.Views, facade, "--bench"), 1, "--bench");
        ExpectError(() => Run(Tool.Daylight, facade, "-h"), 1, "dl_usage");
        ExpectError(() => Run(Tool.Daylight, facade, "--optics", "nosuch=0.5"), 1, "nosuch");
        ExpectError(() => Run(Tool.Views, facade, "--target", "nosuch=g"), 1, "nosuch");
        ExpectError(() => Run(Tool.Daylight, facade, "--epw", Path.Combine(tests, "nosuch.epw")), 2, "nosuch.epw");

        // typed options: the flags bindings/ts/test/flags.test.ts expects of toArgs, in Options.cs's fixed order
        var dopt = new DaylightOptions
        {
            Epw = "site.epw", Quantities = new[] { Quantity.Radiation, Quantity.Sunhours }, SkyDensity = 2, CullBackfaces = true,
            VirtualGround = false, Fresnel = true, Lat = 51.5, From = "06-21T08:00", Verbose = true,
            Optics = new[] { new OpticsSpec("*", 0.5), new OpticsSpec("Glass", 0, 0.64, OpticsKind.Glass, 2), new OpticsSpec("Screen", 0.1, kind: OpticsKind.Porous) },
        };
        Expect(dopt.ToArgs().SequenceEqual(new[] { "--cull-backfaces", "--virtual-ground", "off", "--optics",
            "*=0.5;Glass/2=0,0.64,glass;Screen=0.1,0,porous", "-v", "--epw", "site.epw", "--lat", "51.5", "--from", "06-21T08:00",
            "--quantity", "radiation,sunhours", "--sky-density", "2", "--fresnel", "on" }), "DaylightOptions.ToArgs: " + string.Join(" ", dopt.ToArgs()));
        var vopt = new ViewsOptions
        {
            Targets = new[] { new TargetSpec("park", "green") }, NoOcclude = new[] { "a", "b" }, Fov = new[] { 360.0, 0, 0 },
            Reach = new double[0], GroundZ = -1, Gaze = Gaze.Level, Distances = new[] { DistanceStat.Isovist },
        };
        Expect(vopt.ToArgs().SequenceEqual(new[] { "--no-occlude", "a;b", "--target", "park=green", "--fov", "360,0,0",
            "--gaze", "level", "--distance", "isovist", "--ground-z", "-1" }), "ViewsOptions.ToArgs: " + string.Join(" ", vopt.ToArgs()));
        ExpectError(() => new ViewsOptions { NoOcclude = new[] { "a;b" } }.ToArgs(), 1, "--no-occlude: 'a;b' holds a ';'");
        ExpectError(() => Core.Run(new DaylightOptions { Optics = new[] { new OpticsSpec("nosuch", 0.5) } }, new[] { facade }), 1, "nosuch");

        // C. the GPU: tests/box.obj (a ground of 4 quads, a floating box) as the CLI's smokes run it
        SimulationMesh box = SimulationMesh.Load(Path.Combine(tests, "box.obj"));
        Expect(box.Names.SequenceEqual(new[] { "ground", "box" }), "box.obj's groups: " + string.Join(", ", box.Names));
        string[] sun = { "--lat", "41.98", "--lon", "-87.92", "--tz", "-6", "--quantity", "sunhours",
                         "--time", "06-21T12:00" };
        Result r1;
        using (var ctl = new RunControl())
        {
            try
            {
                r1 = Core.Run(Tool.Daylight, sun, new[] { box }, control: ctl);
            }
            catch (DaylightException e) when (new[] { "CUDA", "OptiX", "optix" }.Any(e.Message.Contains))
            {
                Console.WriteLine("skip: no usable GPU (" + e.Message + ")");
                return;
            }
            Expect(!r1.SceneReused && ctl.Progress == 1.0, "the first run builds its scene, progress 1");
        }
        Expect(SameAsCli(r1, build, "smoke.npy"), "daylight on box.obj != build/smoke.npy(.json) (or they are missing)");
        Expect(r1.Rows == 16 && r1.Columns.SequenceEqual(new[] { "sunhours_front" }) &&
               r1.Column("sunhours_front").SequenceEqual(r1.Values), "rows, columns, Column");
        Expect(r1.Meshes.Length == 2 && r1.Meshes[0].Name == "ground" && r1.Meshes[0].First == 0 && r1.Meshes[0].Count == 4 &&
               r1.Meshes[1].Name == "box" && r1.Meshes[1].First == 4 && r1.Meshes[1].Count == 12, "Meshes");
        Expect(Thrown<KeyNotFoundException>(() => r1.Column("nosuch")) != null, "an unknown column");
        Expect(r1.Stats.Rays > 0 && r1.Stats.Launches > 0 && r1.Stats.TraceMs >= 0, "stats: rays, launches, trace ms");
        Expect(Core.Version == 1, "dl_version");

        // the same handle again: the resident scene serves it, with the same values
        Result r2 = Core.Run(Tool.Daylight, sun, new[] { box });
        Expect(r2.SceneReused && Bytes(r2.Values).SequenceEqual(Bytes(r1.Values)), "a second run: resident scene, same values");

        // views on the same handle: an opaque scene is views' too; its values and manifest are views_smoke.npy's
        Result r3 = Core.Run(Tool.Views, new[] { "--fov", "360", "--offset", "1.6", "--sides", "both" }, new[] { box });
        Expect(r3.SceneReused, "views reuses daylight's opaque scene");
        Expect(SameAsCli(r3, build, "views_smoke.npy"), "views on box.obj != build/views_smoke.npy(.json) (or they are missing)");
        Result typed = Core.Run(new ViewsOptions { Fov = new[] { 360.0 }, Offset = 1.6, Sides = Sides.Both }, new[] { box });
        Expect(Bytes(typed.Values).SequenceEqual(Bytes(r3.Values)) && typed.Manifest == r3.Manifest, "typed ViewsOptions: the same run");

        // a disposed SimulationMesh is refused; a reloaded one builds its scene again, with the same values
        box.Dispose();
        Expect(Thrown<ObjectDisposedException>(() => Core.Run(Tool.Daylight, sun, new[] { box })) != null, "a disposed mesh");
        SimulationMesh box2 = SimulationMesh.Load(Path.Combine(tests, "box.obj"));
        Result r4 = Core.Run(Tool.Daylight, sun, new[] { box2 });
        Expect(!r4.SceneReused && Bytes(r4.Values).SequenceEqual(Bytes(r1.Values)), "a reloaded mesh: built scene, same values");

        // the views facade (check_views case 1): sky above the field's horizon, the virtual ground below, no obstruction
        string[] open = { "--no-self-occlude", "--jitter", "off" };
        Result r5 = Core.Run(Tool.Views, open, new[] { facade });
        string[] columns = { "sky_front", "ground_front", "obstruction_front", "obstruction_total_front", "distance_front", "distance_min_front" };
        Expect(r5.Rows == 1 && r5.Columns.SequenceEqual(columns), "the facade's columns: " + string.Join(", ", r5.Columns));
        Expect(Math.Abs(r5.Column("sky_front")[0] - 0.4490987851112869) < 1e-9 &&
               Math.Abs(r5.Column("ground_front")[0] - 0.5509012148887131) < 1e-9 && r5.Column("obstruction_front")[0] == 0,
               "the facade's sky, ground, obstruction: " + string.Join(", ", r5.Values));
        Result r32 = Core.Run(Tool.Views, open, new[] { facade }, float32: true);
        Expect(r32.Values == null && r32.Values32.SequenceEqual(r5.Values.Select(v => (float)v)) &&
               r32.Column("sky_front")[0] == (float)r5.Column("sky_front")[0], "float32: the same values, rounded");
        Result rc = Core.Run(Tool.Views, open, new[] { facade }, new[] { box2 });   // box.obj's ground lies before it
        Expect(rc.Rows == 1 && rc.Column("obstruction_front")[0] > 0, "context meshes add no rows but block rays");

        // cancel: a cancelled control fails the run as it starts; so does a token cancelled before RunAsync or during it
        var stop = new RunControl();
        stop.Cancel();
        ExpectError(() => Core.Run(Tool.Views, open, new[] { facade }, control: stop), 2, "cancelled");
        stop.Dispose();
        Expect(Thrown<ObjectDisposedException>(stop.Cancel) != null, "a disposed control");
        using (var cts = new CancellationTokenSource())
        {
            cts.Cancel();
            Expect(Canceled(Core.RunAsync(Tool.Views, open, new[] { facade }, token: cts.Token)), "RunAsync, cancelled token");
        }
        using (var cts = new CancellationTokenSource())
        {
            Task<Result> task = Core.RunAsync(Tool.Views, open, new CancelOnRead(facade, cts), token: cts.Token);
            Expect(Canceled(task), "RunAsync, a token cancelled during the run");
        }

        // the next run works, through RunAsync
        Expect(Bytes(Core.RunAsync(Tool.Views, open, new[] { facade }).Result.Values).SequenceEqual(Bytes(r5.Values)),
               "RunAsync after the cancels");
        box2.Dispose();
        facade.Dispose();
    }

    /// <summary>Counts and prints a failure unless <paramref name="ok"/>.</summary>
    static void Expect(bool ok, string what, [CallerLineNumber] int line = 0)
    {
        if (ok) return;
        failures++;
        Console.WriteLine("FAIL line " + line + ": " + what);
    }

    /// <summary>Expects <paramref name="body"/> to throw DaylightException with this status and message text.</summary>
    static void ExpectError(Action body, int status, string message, [CallerLineNumber] int line = 0)
    {
        DaylightException e = Thrown<DaylightException>(body);
        Expect(e != null && e.Status == status && e.Message.Contains(message),
               (e == null ? "no DaylightException" : "status " + e.Status + " '" + e.Message + "'") +
               ", want " + status + " '" + message + "'", line);
    }

    /// <summary>The exception of type T that <paramref name="body"/> throws; null if none.</summary>
    static T Thrown<T>(Action body) where T : Exception
    {
        try
        {
            body();
        }
        catch (T e)
        {
            return e;
        }
        return null;
    }

    /// <summary>One run over one sensor mesh.</summary>
    static Result Run(Tool tool, SimulationMesh sensor, params string[] args)
    {
        return Core.Run(tool, args, new[] { sensor });
    }

    /// <summary>The values' bytes (little-endian float64, as an .npy holds them).</summary>
    static byte[] Bytes(double[] values)
    {
        var bytes = new byte[values.Length * sizeof(double)];
        Buffer.BlockCopy(values, 0, bytes, 0, bytes.Length);
        return bytes;
    }

    /// <summary>The result equals the CLI's NAME (the data after its .npy v1.0 header) and NAME.json (CRLF as LF).</summary>
    static bool SameAsCli(Result r, string build, string name)
    {
        string npy = Path.Combine(build, name);
        if (!File.Exists(npy) || !File.Exists(npy + ".json")) return false;
        byte[] file = File.ReadAllBytes(npy);
        int data = 10 + (file[8] | file[9] << 8);
        return file.Skip(data).SequenceEqual(Bytes(r.Values)) &&
               File.ReadAllText(npy + ".json").Replace("\r\n", "\n") == r.Manifest;
    }

    /// <summary>The task ends cancelled.</summary>
    static bool Canceled(Task task)
    {
        try
        {
            task.Wait();
        }
        catch (AggregateException)
        {
        }
        return task.IsCanceled;
    }
}

/// <summary>One sensor mesh in a list that cancels the token when the run reads it: past Task.Run's own check.</summary>
sealed class CancelOnRead : IReadOnlyList<SimulationMesh>
{
    readonly SimulationMesh mesh;
    readonly CancellationTokenSource cts;

    public CancelOnRead(SimulationMesh mesh, CancellationTokenSource cts)
    {
        this.mesh = mesh;
        this.cts = cts;
    }

    public int Count
    {
        get
        {
            cts.Cancel();
            return 1;
        }
    }

    public SimulationMesh this[int index] { get { return mesh; } }

    public IEnumerator<SimulationMesh> GetEnumerator()
    {
        yield return mesh;
    }

    IEnumerator IEnumerable.GetEnumerator()
    {
        return GetEnumerator();
    }
}
