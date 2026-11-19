// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// The daylight and views tools for C#, in-process through daylight_c.dll (docs/c-abi.md): meshes as handles, the
// CLIs' own flags, results as arrays.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Daylight
{
    /// <summary>The tools: daylight.exe (annual daylight / solar, docs/daylight.md), views.exe (views, docs/views.md).</summary>
    public enum Tool
    {
        /// <summary>daylight.exe.</summary>
        Daylight = 0,
        /// <summary>views.exe.</summary>
        Views = 1,
    }

    /// <summary>A failed call: <see cref="Status"/> is the CLI's exit code, the message the core's.</summary>
    public sealed class DaylightException : Exception
    {
        /// <summary>An exception with <paramref name="status"/> and the core's <paramref name="message"/>.</summary>
        public DaylightException(int status, string message) : base(message)
        {
            Status = status;
        }

        /// <summary>
        /// 1: bad flags, arguments or arrays, names the flags do not resolve; 2: the run failed (a file, the GPU, "cancelled").
        /// </summary>
        public int Status { get; }
    }

    /// <summary>
    /// Meshes held by the core: one from arrays (<see cref="Create"/>, copied) or those of a file (<see cref="Load"/>).
    /// Runs over the same Mesh objects in the same roles and order reuse the GPU scene (<see cref="Result.SceneReused"/>).
    /// Dispose frees them (else the finalizer does), deferred while a run holds them.
    /// </summary>
    public sealed class Mesh : IDisposable
    {
        internal readonly Handle Handle;

        /// <summary>Owns a handle; NULL throws this thread's error with <paramref name="status"/> (the ABI has none).</summary>
        Mesh(IntPtr mesh, int status)
        {
            if (mesh == IntPtr.Zero) throw Native.Error(status);
            Handle = new Handle(mesh, Native.dl_mesh_free);
            var names = new string[Native.dl_mesh_count(mesh)];
            for (int k = 0; k < names.Length; k++) names[k] = Native.FromUtf8(Native.dl_mesh_name(mesh, k));
            Names = Array.AsReadOnly(names);
        }

        /// <summary>
        /// One mesh from arrays, copied: <paramref name="xyz"/> 3 per point (world metres, Z up), <paramref name="counts"/>
        /// vertices per face (&gt;= 3), <paramref name="indices"/> 0-based (right-handed winding), <paramref name="slots"/>
        /// the optics slot per face (--optics NAME/SLOT; null: slot 0). <paramref name="name"/> is what --optics,
        /// --no-occlude, --target and the manifest call it. A mesh without faces holds no mesh (<see cref="Names"/> empty).
        /// Bad arrays throw status 1.
        /// </summary>
        public static Mesh Create(string name, double[] xyz, int[] counts, int[] indices, ushort[] slots = null)
        {
            Native.Init();
            xyz = xyz ?? new double[0];
            counts = counts ?? new int[0];
            indices = indices ?? new int[0];
            if (xyz.Length % 3 != 0)
                throw new DaylightException(Native.Usage, name + ": xyz holds " + xyz.Length + " values, not 3 per point");
            if (slots != null && slots.Length != counts.Length)
                throw new DaylightException(Native.Usage, name + ": " + slots.Length + " slots for " + counts.Length + " faces");
            IntPtr utf8 = Native.ToUtf8(name);
            try
            {
                return new Mesh(Native.dl_mesh_create(utf8, xyz, xyz.Length / 3, counts, counts.Length, indices, indices.Length,
                                                      slots), Native.Usage);
            }
            finally
            {
                Marshal.FreeHGlobal(utf8);
            }
        }

        /// <summary>
        /// The meshes of a file, as the CLI reads a mesh argument: .obj (one per o/g group, named by it) or .dmesh (named
        /// by its stem). The path goes in the ANSI code page, as the CLI's argv (a character outside it throws
        /// ArgumentException). Throws status 2 when it cannot be read, 1 for null.
        /// </summary>
        public static Mesh Load(string path)
        {
            Native.Init();
            IntPtr mesh = Native.dl_mesh_load(path);
            if (mesh == IntPtr.Zero)   // the message holds the path as sent (ANSI); NULL is the core's usage error
                throw new DaylightException(path == null ? Native.Usage : Native.Runtime,
                                            Marshal.PtrToStringAnsi(Native.dl_last_error()));
            return new Mesh(mesh, Native.Runtime);
        }

        /// <summary>The names of the meshes held, in order; their rows follow one another in this order.</summary>
        public IReadOnlyList<string> Names { get; }

        /// <summary>Frees the meshes; a later run given this Mesh throws ObjectDisposedException.</summary>
        public void Dispose()
        {
            Handle.Dispose();
        }
    }

    /// <summary>Cancel and progress of one run, from any thread; the cancel flag stays set, so one control per run.</summary>
    public sealed class RunControl : IDisposable
    {
        internal readonly Handle Handle;

        /// <summary>A new control.</summary>
        public RunControl()
        {
            Native.Init();
            IntPtr control = Native.dl_control_create();
            if (control == IntPtr.Zero) throw Native.Error(Native.Runtime);
            Handle = new Handle(control, Native.dl_control_free);
        }

        /// <summary>
        /// The run stops at its next check (before the sky, per trace chunk) and throws status 2 "cancelled"; a run not
        /// started yet fails as it starts.
        /// </summary>
        public void Cancel()
        {
            Handle.Use(p =>
            {
                Native.dl_control_cancel(p);
                return 0;
            });
        }

        /// <summary>The share of sensors traced so far, 0..1 (after each trace chunk).</summary>
        public double Progress { get { return Handle.Use(Native.dl_control_progress); } }

        /// <summary>Frees the control (deferred while a run holds it).</summary>
        public void Dispose()
        {
            Handle.Dispose();
        }
    }

    /// <summary>The rows of one sensor mesh, <see cref="Count"/> from <see cref="First"/> (the manifest's "meshes").</summary>
    [DataContract]
    public sealed class MeshRange
    {
        MeshRange() { }

        /// <summary>The mesh's name.</summary>
        [DataMember(Name = "name")] public string Name { get; private set; }
        /// <summary>Its first row.</summary>
        [DataMember(Name = "first")] public long First { get; private set; }
        /// <summary>Its rows (faces, or points with --on vertices).</summary>
        [DataMember(Name = "count")] public long Count { get; private set; }
    }

    /// <summary>What a run gives: the values bit for bit as --out FILE.npy holds them, the manifest as FILE.json.</summary>
    public sealed class Result
    {
        /// <summary>The manifest's members the binding reads (DataContractJsonSerializer skips the others).</summary>
        [DataContract]
        sealed class Parsed
        {
            [DataMember(Name = "columns")] internal string[] Columns { get; set; }
            [DataMember(Name = "meshes")] internal MeshRange[] Meshes { get; set; }
        }

        /// <summary>Copies everything out of a dl_result (the caller frees it).</summary>
        internal Result(IntPtr r)
        {
            Rows = Native.dl_result_rows(r);
            Values = new double[Rows * Native.dl_result_cols(r)];
            Stats = new double[Native.Stats];
            if (Native.dl_result_values(r, Values, Values.LongLength) != Native.Ok ||
                Native.dl_result_stats(r, Stats) != Native.Ok) throw Native.Error(Native.Usage);
            Manifest = Native.FromUtf8(Native.dl_result_manifest(r));
            using (var json = new MemoryStream(Encoding.UTF8.GetBytes(Manifest)))
            {
                var parsed = (Parsed)new DataContractJsonSerializer(typeof(Parsed)).ReadObject(json);
                Columns = parsed.Columns ?? new string[0];
                Meshes = parsed.Meshes ?? new MeshRange[0];
            }
        }

        /// <summary>Column names (the manifest's "columns"), e.g. "sunhours_front".</summary>
        public string[] Columns { get; }
        /// <summary>The elements of the sensor meshes in input order (faces, or points with --on vertices).</summary>
        public long Rows { get; }
        /// <summary>Rows x Columns.Length values, row-major float64.</summary>
        public double[] Values { get; }
        /// <summary>FILE.json as the CLI writes it, with LF line ends (columns, meshes; views: the field, targets).</summary>
        public string Manifest { get; }
        /// <summary>The rows of each sensor mesh, in order.</summary>
        public MeshRange[] Meshes { get; }
        /// <summary>Scene reused (1/0), ms of prep, sky, build, trace, reduce, bounce, then rays, launches, GAS bytes.</summary>
        public double[] Stats { get; }
        /// <summary>A resident GPU scene served the run (no triangulation, upload or build).</summary>
        public bool SceneReused { get { return Stats[0] == 1; } }

        /// <summary>The values of one column, a value per row; an unknown name throws KeyNotFoundException.</summary>
        public double[] Column(string name)
        {
            int c = Array.IndexOf(Columns, name);
            if (c < 0) throw new KeyNotFoundException("no column '" + name + "'; columns: " + string.Join(", ", Columns));
            var column = new double[Rows];
            for (long i = 0; i < Rows; i++) column[i] = Values[i * Columns.Length + c];
            return column;
        }
    }

    /// <summary>
    /// Runs of the tools in this process. They take turns (the GPU runs one at a time); the GPU context and one
    /// resident scene stay until the process exits.
    /// </summary>
    public static class Core
    {
        /// <summary>
        /// One run of <paramref name="tool"/> with its CLI flags without the program name ("--epw", "site.epw", ...):
        /// <paramref name="sensors"/> take the place of the CLI's mesh arguments, <paramref name="context"/> of its
        /// --context meshes. Refused (status 1): mesh paths, --context, --out, --bench, -h. Every other flag works as in
        /// the CLI, files included. Failures throw <see cref="DaylightException"/> with the CLI's exit code and message.
        /// </summary>
        public static Result Run(Tool tool, IReadOnlyList<string> args, IReadOnlyList<Mesh> sensors,
                                 IReadOnlyList<Mesh> context = null, RunControl control = null)
        {
            Native.Init();
            var argv = new IntPtr[args == null ? 0 : args.Count];
            var held = new List<Handle>();
            IntPtr result = IntPtr.Zero;
            try
            {
                for (int i = 0; i < argv.Length; i++) argv[i] = Native.ToUtf8(args[i]);
                IntPtr[] s = Hold(sensors, held), c = Hold(context, held);
                IntPtr ctl = control == null ? IntPtr.Zero : Hold(control.Handle, held);
                int status = Native.dl_run((int)tool, argv.Length, argv, s, s.Length, c, c.Length, ctl, out result);
                if (status != Native.Ok) throw Native.Error(status);
                return new Result(result);
            }
            finally
            {
                Native.dl_result_free(result);
                foreach (IntPtr p in argv) Marshal.FreeHGlobal(p);
                foreach (Handle h in held) h.DangerousRelease();
            }
        }

        /// <summary>
        /// <see cref="Run"/> on the thread pool. A cancelled <paramref name="token"/> cancels the run (through
        /// <paramref name="control"/>, or a control of its own) and the task ends cancelled.
        /// </summary>
        public static Task<Result> RunAsync(Tool tool, IReadOnlyList<string> args, IReadOnlyList<Mesh> sensors,
                                            IReadOnlyList<Mesh> context = null, RunControl control = null,
                                            CancellationToken token = default(CancellationToken))
        {
            return Task.Run(() =>
            {
                using (RunControl own = control == null && token.CanBeCanceled ? new RunControl() : null)
                using (token.Register(() => (control ?? own).Cancel()))
                {
                    try
                    {
                        return Run(tool, args, sensors, context, control ?? own);
                    }
                    catch (DaylightException e) when (e.Status == Native.Runtime && token.IsCancellationRequested)
                    {
                        throw new OperationCanceledException(e.Message, e, token);
                    }
                }
            }, token);
        }

        /// <summary>The tool's usage text, as its CLI's -h prints it; null for an unknown tool.</summary>
        public static string Usage(Tool tool)
        {
            Native.Init();
            return Native.FromUtf8(Native.dl_usage((int)tool));
        }

        /// <summary>The meshes' pointers, each held (a null Mesh gives NULL, which the core refuses).</summary>
        static IntPtr[] Hold(IReadOnlyList<Mesh> meshes, List<Handle> held)
        {
            var p = new IntPtr[meshes == null ? 0 : meshes.Count];
            for (int i = 0; i < p.Length; i++) p[i] = meshes[i] == null ? IntPtr.Zero : Hold(meshes[i].Handle, held);
            return p;
        }

        /// <summary>The handle's pointer, held until the run returns.</summary>
        static IntPtr Hold(Handle h, List<Handle> held)
        {
            IntPtr p = h.Hold();
            held.Add(h);
            return p;
        }
    }
}
