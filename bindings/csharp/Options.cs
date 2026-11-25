// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// Typed run options: every flag of the daylight and views CLIs as a property (docs/daylight.md, docs/views.md) and
// ToArgs(), which turns them into those flags; Core.Run / RunAsync take them in place of a tool and raw flags.
// Not here: mesh files and --context (sensors / context), --out (the values come back), --bench and -h (no run).
// bindings/ts/test/flags.test.ts keeps this file, options.ts, options.py and the docs in step with src/cli.cpp.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace Daylight
{
    /// <summary>What a row stands for (--on).</summary>
    public enum Elements
    {
        /// <summary>A row per face, the sensor or eye at its centroid.</summary>
        Faces,
        /// <summary>A row per vertex.</summary>
        Vertices,
    }

    /// <summary>The receiving or viewing side(s) of each element (--sides).</summary>
    public enum Sides
    {
        /// <summary>The side the right-hand normal points to.</summary>
        Front,
        /// <summary>The other side.</summary>
        Back,
        /// <summary>Both: two columns per value.</summary>
        Both,
    }

    /// <summary>daylight's sky and sun model (--mode).</summary>
    public enum SkyMode
    {
        /// <summary>Perez sky, exact sun (the default).</summary>
        Physical,
        /// <summary>Radiance gendaymtx / Ladybug's model (validation).</summary>
        Ladybug,
    }

    /// <summary>A daylight column (--quantity).</summary>
    public enum Quantity
    {
        /// <summary>Total radiation, kWh/m2.</summary>
        Radiation,
        /// <summary>Direct (sun) radiation, kWh/m2.</summary>
        Direct,
        /// <summary>Diffuse (sky) radiation, kWh/m2.</summary>
        Diffuse,
        /// <summary>Mean irradiance, W/m2.</summary>
        Irradiance,
        /// <summary>Mean illuminance, lux.</summary>
        Illuminance,
        /// <summary>Hours of direct sun, h.</summary>
        Sunhours,
    }

    /// <summary>The views field's direction (--gaze).</summary>
    public enum Gaze
    {
        /// <summary>About the normal.</summary>
        Normal,
        /// <summary>About the normal turned level with the horizon, as a person looks.</summary>
        Level,
    }

    /// <summary>A views distance statistic (--distance).</summary>
    public enum DistanceStat
    {
        /// <summary>Volume (m3) of the mesh through the rays' ends; area (m2) with a 0,0 elevation.</summary>
        Isovist,
        /// <summary>Log-mean distance.</summary>
        Mean,
        /// <summary>Nearest distance.</summary>
        Min,
        /// <summary>Also per target group.</summary>
        Group,
        /// <summary>No distance columns.</summary>
        None,
    }

    /// <summary>How light passes a transmitting slot (--optics KIND).</summary>
    public enum OpticsKind
    {
        /// <summary>Radiance glass, n 1.52 (rho 0, 0 &lt; tau &lt;= 0.918).</summary>
        Glass,
        /// <summary>Tau at every angle (screens, trees).</summary>
        Porous,
    }

    /// <summary>One --optics SPEC, NAME[/SLOT]=RHO[,TAU[,glass|porous]]: the numbers of a mesh, or of one face slot of it.</summary>
    public sealed class OpticsSpec
    {
        /// <summary>Optics of mesh <paramref name="name"/> (or "*", every mesh), or of its face <paramref name="slot"/>.</summary>
        public OpticsSpec(string name, double rho, double? tau = null, OpticsKind? kind = null, int? slot = null)
        {
            Name = name;
            Rho = rho;
            Tau = tau;
            Kind = kind;
            Slot = slot;
        }

        /// <summary>Core mesh name (.dmesh stem, OBJ group, USD prim path, SimulationMesh name), or "*" for every mesh.</summary>
        public string Name { get; }
        /// <summary>Diffuse reflectance, 0..1.</summary>
        public double Rho { get; }
        /// <summary>Transmittance at normal incidence, rho + tau &lt;= 1 (null: 0).</summary>
        public double? Tau { get; }
        /// <summary>Glass or porous (null: porous when tau &gt; 0).</summary>
        public OpticsKind? Kind { get; }
        /// <summary>Face slot, 0..65535 (null: the whole mesh).</summary>
        public int? Slot { get; }

        /// <summary>The SPEC as the CLI takes it (a kind needs a tau: 0 when null).</summary>
        public override string ToString()
        {
            string s = Name + (Slot.HasValue ? "/" + Slot.Value.ToString(CultureInfo.InvariantCulture) : "") + "=" + ArgList.Format(Rho);
            if (Tau.HasValue || Kind.HasValue) s += "," + ArgList.Format(Tau ?? 0);
            return Kind.HasValue ? s + "," + Kind.Value.ToString().ToLowerInvariant() : s;
        }
    }

    /// <summary>One --target NAME=GROUP: rays ending on mesh <see cref="Name"/> count for <see cref="Group"/>.</summary>
    public sealed class TargetSpec
    {
        /// <summary>Mesh <paramref name="name"/> counts for <paramref name="group"/>.</summary>
        public TargetSpec(string name, string group)
        {
            Name = name;
            Group = group;
        }

        /// <summary>Core mesh name.</summary>
        public string Name { get; }
        /// <summary>The group: a share column per group (at most 16).</summary>
        public string Group { get; }

        /// <summary>NAME=GROUP.</summary>
        public override string ToString() { return Name + "=" + Group; }
    }

    /// <summary>Flags both tools take. Null (false for the bare flags) leaves a flag out: the CLI's default.</summary>
    public abstract class CommonOptions
    {
        /// <summary>--on: a row per face (default) or per vertex.</summary>
        public Elements? On { get; set; }
        /// <summary>--sides: the receiving / viewing side(s); default front.</summary>
        public Sides? Sides { get; set; }
        /// <summary>--offset M: sensors / eyes moved M metres along their normal (default 0).</summary>
        public double? Offset { get; set; }
        /// <summary>--cull-backfaces: occluders block only from their front side.</summary>
        public bool CullBackfaces { get; set; }
        /// <summary>--no-self-occlude: sensor meshes cast no shade, block no rays (only context meshes do).</summary>
        public bool NoSelfOcclude { get; set; }
        /// <summary>--no-occlude NAME;...: these sensor meshes are analysed but cast no shade, block no rays.</summary>
        public IList<string> NoOcclude { get; set; }
        /// <summary>--north DEG: model north, degrees counterclockwise from +Y (90 = west; default 0).</summary>
        public double? North { get; set; }
        /// <summary>--virtual-ground on|off: a ground plane below the meshes (default on).</summary>
        public bool? VirtualGround { get; set; }
        /// <summary>--optics SPEC;...: optics per mesh or face slot (default rho 0.2, tau 0).</summary>
        public IList<OpticsSpec> Optics { get; set; }
        /// <summary>--transmission on|off: light (daylight) or rays (views) through glass and porous slots (default off).</summary>
        public bool? Transmission { get; set; }
        /// <summary>-v: timings and sizes on stderr.</summary>
        public bool Verbose { get; set; }

        /// <summary>The tool these options are for.</summary>
        public abstract Tool Tool { get; }

        /// <summary>The CLI flags these options stand for, in a fixed order (an empty list leaves its flag out).</summary>
        public string[] ToArgs()
        {
            var a = new ArgList();
            AddTo(a);
            return a.ToArray();
        }

        /// <summary>Appends the flags; each tool's options add theirs after these.</summary>
        internal virtual void AddTo(ArgList a)
        {
            a.Choice("--on", On);
            a.Choice("--sides", Sides);
            a.Number("--offset", Offset);
            a.Flag("--cull-backfaces", CullBackfaces);
            a.Flag("--no-self-occlude", NoSelfOcclude);
            a.Packed("--no-occlude", NoOcclude);
            a.Number("--north", North);
            a.OnOff("--virtual-ground", VirtualGround);
            a.Packed("--optics", Optics);
            a.OnOff("--transmission", Transmission);
            a.Flag("-v", Verbose);
        }
    }

    /// <summary>daylight's flags (docs/daylight.md): site, weather and time, quantities, sky, materials, output.</summary>
    public sealed class DaylightOptions : CommonOptions
    {
        /// <summary>Daylight.</summary>
        public override Tool Tool { get { return Tool.Daylight; } }

        /// <summary>--epw FILE: hourly weather and location.</summary>
        public string Epw { get; set; }
        /// <summary>--clear-sky: a synthetic cloudless year (Ineichen) at Lat / Lon instead of an EPW.</summary>
        public bool ClearSky { get; set; }
        /// <summary>--turbidity TL: Linke turbidity for ClearSky (2 very clear .. 6-7 polluted; default 3).</summary>
        public double? Turbidity { get; set; }
        /// <summary>--altitude M: site altitude in metres for ClearSky (default 0).</summary>
        public double? Altitude { get; set; }
        /// <summary>--lat DEG: latitude, north positive; needed without an EPW, else overrides its location.</summary>
        public double? Lat { get; set; }
        /// <summary>--lon DEG: longitude, east positive.</summary>
        public double? Lon { get; set; }
        /// <summary>--tz H: the clock of the times, standard time (default: the EPW's zone, else round(lon / 15)).</summary>
        public double? Tz { get; set; }
        /// <summary>--time [YYYY-]MM-DDTHH:MM: one instant (default: annual, every EPW row).</summary>
        public string Time { get; set; }
        /// <summary>--from TS: start of the range [From, To).</summary>
        public string From { get; set; }
        /// <summary>--to TS: end of the range [From, To).</summary>
        public string To { get; set; }
        /// <summary>--step MIN: minutes per step of the range (default 60).</summary>
        public int? Step { get; set; }
        /// <summary>--hours HH:MM-HH:MM: with From / To, only these hours of every day.</summary>
        public string Hours { get; set; }
        /// <summary>--quantity Q,...: a column each (default radiation).</summary>
        public IList<Quantity> Quantities { get; set; }
        /// <summary>--mode: sky and sun model (default physical).</summary>
        public SkyMode? Mode { get; set; }
        /// <summary>--sky-density 1|2|4: 145 / 577 / 2305 sky patches (default 1).</summary>
        public int? SkyDensity { get; set; }
        /// <summary>--patch-samples K: physical mode, K stratified rays per sky patch, a square number (default 1).</summary>
        public int? PatchSamples { get; set; }
        /// <summary>--ground RHO: ground reflectance 0..1 (default 0.2).</summary>
        public double? Ground { get; set; }
        /// <summary>--fresnel on|off: glass by angle (off: tau at every angle; default on).</summary>
        public bool? Fresnel { get; set; }
        /// <summary>--bounces N: up to N = 0..20 diffuse reflections (physical mode; default 0).</summary>
        public int? Bounces { get; set; }
        /// <summary>--bounce-samples M: paths per sensor for Bounces, 64..65536 (default 1024).</summary>
        public int? BounceSamples { get; set; }
        /// <summary>--timeseries F.npy: per-step values, float32 [N x T], and F.npy.hoys.txt.</summary>
        public string Timeseries { get; set; }
        /// <summary>--dump-bits F.npy: visibility per sensor and direction (validation).</summary>
        public string DumpBits { get; set; }
        /// <summary>--dump-sky F: the gendaymtx sky blocks (ladybug mode; validation).</summary>
        public string DumpSky { get; set; }
        /// <summary>--dump-sun F: per step date, sun positions, DNI and DHI (validation).</summary>
        public string DumpSun { get; set; }

        internal override void AddTo(ArgList a)
        {
            base.AddTo(a);
            a.Text("--epw", Epw);
            a.Flag("--clear-sky", ClearSky);
            a.Number("--turbidity", Turbidity);
            a.Number("--altitude", Altitude);
            a.Number("--lat", Lat);
            a.Number("--lon", Lon);
            a.Number("--tz", Tz);
            a.Text("--time", Time);
            a.Text("--from", From);
            a.Text("--to", To);
            a.Number("--step", Step);
            a.Text("--hours", Hours);
            a.Joined("--quantity", Quantities == null ? null : Quantities.Select(q => q.ToString().ToLowerInvariant()));
            a.Choice("--mode", Mode);
            a.Number("--sky-density", SkyDensity);
            a.Number("--patch-samples", PatchSamples);
            a.Number("--ground", Ground);
            a.OnOff("--fresnel", Fresnel);
            a.Number("--bounces", Bounces);
            a.Number("--bounce-samples", BounceSamples);
            a.Text("--timeseries", Timeseries);
            a.Text("--dump-bits", DumpBits);
            a.Text("--dump-sky", DumpSky);
            a.Text("--dump-sun", DumpSun);
        }
    }

    /// <summary>views' flags (docs/views.md): field of view, distances, targets, ground and glass.</summary>
    public sealed class ViewsOptions : CommonOptions
    {
        /// <summary>Views.</summary>
        public override Tool Tool { get { return Tool.Views; } }

        /// <summary>--target NAME=GROUP;...: rays ending on a mesh count for its group, a share column per group (at most 16).</summary>
        public IList<TargetSpec> Targets { get; set; }
        /// <summary>--fov H[,LO,HI]: H degrees wide about the normal (up to 180) or 360 all around, elevation LO..HI
        /// (one or three numbers; default 120, -70, 50).</summary>
        public IList<double> Fov { get; set; }
        /// <summary>--gaze: about the normal, or about the normal turned level (default normal).</summary>
        public Gaze? Gaze { get; set; }
        /// <summary>--cell DEG: degrees per ray, rows and columns (default 2).</summary>
        public double? Cell { get; set; }
        /// <summary>--jitter on|off: each ray at a random point of its cell (off: the cell centres; default on).</summary>
        public bool? Jitter { get; set; }
        /// <summary>--distance STAT,...: the distance columns (default: isovist with a 360 field, else mean, min).</summary>
        public IList<DistanceStat> Distances { get; set; }
        /// <summary>--far M: what open counts as, and the cap of every distance (default 1000).</summary>
        public double? Far { get; set; }
        /// <summary>--reach D,...: share of the field that sees at least D metres (at most 8; EN 17037: 6, 20, 50).</summary>
        public IList<double> Reach { get; set; }
        /// <summary>--ground-z Z: height of the virtual ground in metres (default: the lowest point of the meshes).</summary>
        public double? GroundZ { get; set; }
        /// <summary>--dump-rays F.npy: classes uint8 [rows x rays] and F.npy.depth.npy.</summary>
        public string DumpRays { get; set; }

        internal override void AddTo(ArgList a)
        {
            base.AddTo(a);
            a.Packed("--target", Targets);
            a.Joined("--fov", Fov == null ? null : Fov.Select(ArgList.Format));
            a.Choice("--gaze", Gaze);
            a.Number("--cell", Cell);
            a.OnOff("--jitter", Jitter);
            a.Joined("--distance", Distances == null ? null : Distances.Select(d => d.ToString().ToLowerInvariant()));
            a.Number("--far", Far);
            a.Joined("--reach", Reach == null ? null : Reach.Select(ArgList.Format));
            a.Number("--ground-z", GroundZ);
            a.Text("--dump-rays", DumpRays);
        }
    }

    /// <summary>Flags being built: each adder leaves its flag out for null (and false, an empty list).</summary>
    internal sealed class ArgList : List<string>
    {
        /// <summary>A number as the core reads it: invariant culture, round-trip precision.</summary>
        internal static string Format(double v) { return v.ToString("R", CultureInfo.InvariantCulture); }

        internal void Flag(string flag, bool on) { if (on) Add(flag); }

        internal void OnOff(string flag, bool? on) { if (on.HasValue) { Add(flag); Add(on.Value ? "on" : "off"); } }

        internal void Number(string flag, double? v) { if (v.HasValue) { Add(flag); Add(Format(v.Value)); } }

        internal void Number(string flag, int? v) { if (v.HasValue) { Add(flag); Add(v.Value.ToString(CultureInfo.InvariantCulture)); } }

        internal void Text(string flag, string v) { if (v != null) { Add(flag); Add(v); } }

        internal void Choice<T>(string flag, T? v) where T : struct, Enum
        {
            if (v.HasValue) { Add(flag); Add(v.Value.ToString().ToLowerInvariant()); }
        }

        internal void Joined(string flag, IEnumerable<string> items)
        {
            string[] list = items == null ? new string[0] : items.ToArray();
            if (list.Length > 0) { Add(flag); Add(string.Join(",", list)); }
        }

        /// <summary>Specs joined by ';', the CLI's packing; one holding ';' cannot be addressed: status 1.</summary>
        internal void Packed<T>(string flag, IEnumerable<T> items)
        {
            string[] list = items == null ? new string[0] : items.Select(x => x.ToString()).ToArray();
            foreach (string s in list)
                if (s.Contains(";")) throw new DaylightException(Native.Usage, flag + ": '" + s + "' holds a ';', which cannot be addressed");
            if (list.Length > 0) { Add(flag); Add(string.Join(";", list)); }
        }
    }
}
