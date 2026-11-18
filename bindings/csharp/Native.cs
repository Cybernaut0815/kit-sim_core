// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// P/Invoke surface of the C ABI (src/daylight_c.h documents every function), how daylight_c.dll is found, and the
// helpers the wrappers share.
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

// No DllImport probe of Daylight.dll's folder, which would load a daylight_c.dll there ahead of DAYLIGHT_DLL (two copies,
// the variable ignored): by bare name the loader returns the one Init preloaded, else searches as LoadLibrary does.
[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.LegacyBehavior)]

namespace Daylight
{
    /// <summary>
    /// Raw declarations of src/daylight_c.h. Strings the library returns come back as IntPtr (UTF-8, owned by the
    /// library); <see cref="Init"/> runs before the first call.
    /// </summary>
    internal static class Native
    {
        // daylight_c, not daylight: a native daylight.dll would collide with the managed Daylight.dll on
        // case-insensitive file systems (Windows, Unity's Plugins).
        const string Lib = "daylight_c";
        const CallingConvention Cdecl = CallingConvention.Cdecl;
        internal const int AbiVersion = 1, Ok = 0, Usage = 1, Runtime = 2, Stats = 10;

        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern int dl_version();
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern IntPtr dl_last_error();
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern IntPtr dl_usage(int tool);

        [DllImport(Lib, CallingConvention = Cdecl)]
        internal static extern IntPtr dl_mesh_create(IntPtr name, double[] xyz, long nPoints, int[] counts, long nFaces,
                                                     int[] indices, long nIndices, ushort[] slots);
        // The path in the ANSI code page, as the CLI's argv; a character outside it throws instead of becoming a lookalike.
        [DllImport(Lib, CallingConvention = Cdecl, CharSet = CharSet.Ansi, BestFitMapping = false,
                   ThrowOnUnmappableChar = true)]
        internal static extern IntPtr dl_mesh_load(string path);
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern int dl_mesh_count(IntPtr mesh);
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern IntPtr dl_mesh_name(IntPtr mesh, int k);
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern void dl_mesh_free(IntPtr mesh);

        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern IntPtr dl_control_create();
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern void dl_control_cancel(IntPtr control);
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern double dl_control_progress(IntPtr control);
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern void dl_control_free(IntPtr control);

        [DllImport(Lib, CallingConvention = Cdecl)]
        internal static extern int dl_run(int tool, int argc, IntPtr[] argv, IntPtr[] sensors, int nSensors, IntPtr[] context,
                                          int nContext, IntPtr control, out IntPtr result);

        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern long dl_result_rows(IntPtr result);
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern long dl_result_cols(IntPtr result);
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern IntPtr dl_result_manifest(IntPtr result);
        [DllImport(Lib, CallingConvention = Cdecl)]
        internal static extern int dl_result_values(IntPtr result, [Out] double[] output, long n);
        [DllImport(Lib, CallingConvention = Cdecl)]
        internal static extern int dl_result_values_f32(IntPtr result, [Out] float[] output, long n);
        [DllImport(Lib, CallingConvention = Cdecl)]
        internal static extern int dl_result_stats(IntPtr result, [Out] double[] output);
        [DllImport(Lib, CallingConvention = Cdecl)] internal static extern void dl_result_free(IntPtr result);

        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);

        static readonly object gate = new object();
        static volatile bool ready;

        /// <summary>
        /// Loads daylight_c.dll once by its absolute path, DAYLIGHT_DLL or else beside Daylight.dll (a Grasshopper or
        /// Unity plug-in folder), so that DllImport finds it loaded; without either the host's own search finds it (a
        /// Unity player's Plugins). Then checks its ABI version. Throws, and tries again on the next call, until it
        /// succeeds; it is never unloaded.
        /// </summary>
        internal static void Init()
        {
            if (ready) return;
            lock (gate)
            {
                if (ready) return;
                string env = Environment.GetEnvironmentVariable("DAYLIGHT_DLL"), at = typeof(Native).Assembly.Location;
                string beside = string.IsNullOrEmpty(at) ? null : Path.Combine(Path.GetDirectoryName(at), "daylight_c.dll");
                string path = !string.IsNullOrEmpty(env) ? Path.GetFullPath(env) : File.Exists(beside) ? beside : null;
                if (path != null && !"daylight_c.dll".Equals(Path.GetFileName(path), StringComparison.OrdinalIgnoreCase))
                    throw new DllNotFoundException("daylight: DAYLIGHT_DLL must name a file called daylight_c.dll (DllImport "
                                                   + "finds the library by that name), not " + path);
                // LOAD_WITH_ALTERED_SEARCH_PATH (8): its own dependencies (a VC++ runtime copied beside it) are found there.
                if (path != null && LoadLibraryExW(path, IntPtr.Zero, 8) == IntPtr.Zero)
                {
                    int error = Marshal.GetLastWin32Error();
                    throw new DllNotFoundException("daylight: cannot load " + path + (!File.Exists(path) ? ": no such file"
                        : " (Win32 error " + error + "): it needs a 64-bit process, the NVIDIA driver and the VC++ 2015-2022 "
                          + "x64 runtime"));
                }
                int version;
                try
                {
                    version = dl_version();
                }
                catch (DllNotFoundException e)
                {
                    throw new DllNotFoundException("daylight: daylight_c.dll not found; set DAYLIGHT_DLL to its path or put "
                                                   + "it beside " + (string.IsNullOrEmpty(at) ? "Daylight.dll" : at), e);
                }
                if (version != AbiVersion)
                    throw new DaylightException(Runtime, "daylight: daylight_c.dll has ABI " + version + ", this binding expects "
                                                         + AbiVersion);
                ready = true;
            }
        }

        /// <summary>A NUL-terminated UTF-8 copy in unmanaged memory, for Marshal.FreeHGlobal; IntPtr.Zero for null.</summary>
        internal static IntPtr ToUtf8(string s)
        {
            if (s == null) return IntPtr.Zero;
            byte[] bytes = Encoding.UTF8.GetBytes(s + "\0");
            IntPtr p = Marshal.AllocHGlobal(bytes.Length);
            Marshal.Copy(bytes, 0, p, bytes.Length);
            return p;
        }

        /// <summary>A library-owned UTF-8 string, copied; null for NULL.</summary>
        internal static string FromUtf8(IntPtr p)
        {
            if (p == IntPtr.Zero) return null;
            int n = 0;
            while (Marshal.ReadByte(p, n) != 0) n++;
            var bytes = new byte[n];
            Marshal.Copy(p, bytes, 0, n);
            return Encoding.UTF8.GetString(bytes);
        }

        /// <summary>The exception of a failed call: its status and this thread's dl_last_error.</summary>
        internal static DaylightException Error(int status)
        {
            return new DaylightException(status, FromUtf8(dl_last_error()));
        }
    }

    /// <summary>A mesh or control handle, freed once (Dispose or the finalizer), never while a call holds it.</summary>
    internal sealed class Handle : SafeHandle
    {
        readonly Action<IntPtr> free;

        /// <summary>Takes ownership of <paramref name="ptr"/>, which <paramref name="free"/> frees.</summary>
        internal Handle(IntPtr ptr, Action<IntPtr> free) : base(IntPtr.Zero, true)
        {
            this.free = free;
            SetHandle(ptr);
        }

        /// <summary>True for NULL.</summary>
        public override bool IsInvalid { get { return handle == IntPtr.Zero; } }

        /// <summary>dl_mesh_free or dl_control_free.</summary>
        protected override bool ReleaseHandle()
        {
            free(handle);
            return true;
        }

        /// <summary>The pointer, held until DangerousRelease; a disposed handle throws ObjectDisposedException.</summary>
        internal IntPtr Hold()
        {
            bool added = false;
            DangerousAddRef(ref added);
            return handle;
        }

        /// <summary><paramref name="f"/> of the pointer, which no Dispose frees meanwhile.</summary>
        internal T Use<T>(Func<IntPtr, T> f)
        {
            IntPtr p = Hold();
            try
            {
                return f(p);
            }
            finally
            {
                DangerousRelease();
            }
        }
    }
}
