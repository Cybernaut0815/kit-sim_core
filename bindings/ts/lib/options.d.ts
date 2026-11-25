/** One `--optics` SPEC, `NAME[/SLOT]=RHO[,TAU[,glass|porous]]`: the numbers of a mesh, or of one face slot of it. */
export interface OpticsSpec {
    /** Core mesh name (`.dmesh` stem, OBJ group, USD prim path; dmeshStems() of an ObjMesh name), or `*` for every mesh. */
    name: string;
    /** Face slot (0..65535) instead of the whole mesh. */
    slot?: number;
    /** Diffuse reflectance, 0..1. */
    rho: number;
    /** Transmittance at normal incidence, rho + tau <= 1 (default 0). */
    tau?: number;
    /** `glass` (Radiance glass, n 1.52: rho 0, 0 < tau <= 0.918) or `porous` (tau at every angle); default porous when tau > 0. */
    kind?: 'glass' | 'porous';
}
/** One `--target NAME=GROUP`: rays ending on mesh `name` count for `group` (a share column per group). */
export interface TargetSpec {
    name: string;
    group: string;
}
/** Flags both tools take. */
export interface CommonOptions {
    /** `--on`: a row per face (at its centroid) or per vertex. Default faces. */
    on?: 'faces' | 'vertices';
    /** `--sides`: the receiving / viewing side(s); both: two columns per value. Default front. */
    sides?: 'front' | 'back' | 'both';
    /** `--offset M`: sensors / eyes moved M metres along their normal. Default 0. */
    offset?: number;
    /** `--cull-backfaces`: occluders block only from their front side. */
    cullBackfaces?: boolean;
    /** `--no-self-occlude`: sensor meshes cast no shade, block no rays (only context meshes do). */
    noSelfOcclude?: boolean;
    /** `--no-occlude NAME;...`: these sensor meshes are analysed but cast no shade, block no rays. */
    noOcclude?: string[];
    /** `--north DEG`: model north, degrees counterclockwise from +Y (90 = west). Default 0. */
    north?: number;
    /** `--virtual-ground on|off`: a ground plane below the meshes (daylight: reflecting `ground`; views: ends downward rays). Default on. */
    virtualGround?: boolean;
    /** `--optics SPEC;...`: optics per mesh or face slot (a string is one SPEC as the CLI takes it). Default rho 0.2, tau 0. */
    optics?: (OpticsSpec | string)[];
    /** `--transmission on|off`: light (daylight) or rays (views) through glass and porous slots. Default off. */
    transmission?: boolean;
    /** `-v`: timings and sizes on stderr. */
    verbose?: boolean;
}
/** daylight's flags (docs/daylight.md): site, weather and time, quantities, sky, materials, output. */
export interface DaylightOptions extends CommonOptions {
    /** `--epw FILE`: hourly weather and location (runRemote: pass `epw` instead). */
    epw?: string;
    /** `--clear-sky`: a synthetic cloudless year (Ineichen) at `lat` / `lon` instead of an EPW. */
    clearSky?: boolean;
    /** `--turbidity TL`: Linke turbidity for clearSky (2 very clear .. 6-7 polluted). Default 3. */
    turbidity?: number;
    /** `--altitude M`: site altitude in metres for clearSky. Default 0. */
    altitude?: number;
    /** `--lat DEG`: latitude, north positive; needed without an EPW, else overrides its location. */
    lat?: number;
    /** `--lon DEG`: longitude, east positive. */
    lon?: number;
    /** `--tz H`: the clock of the times, standard time (default: the EPW's zone, else round(lon / 15)). */
    tz?: number;
    /** `--time [YYYY-]MM-DDTHH:MM`: one instant (default: annual, every EPW row). */
    time?: string;
    /** `--from TS`: start of the range [from, to). */
    from?: string;
    /** `--to TS`: end of the range [from, to). */
    to?: string;
    /** `--step MIN`: minutes per step of the range. Default 60. */
    step?: number;
    /** `--hours HH:MM-HH:MM`: with from / to, only these hours of every day. */
    hours?: string;
    /** `--quantity Q,...`: a column each (radiation kWh/m2, direct, diffuse, irradiance W/m2, illuminance lux, sunhours h). Default radiation. */
    quantity?: ('radiation' | 'direct' | 'diffuse' | 'irradiance' | 'illuminance' | 'sunhours')[];
    /** `--mode`: sky and sun model. Default physical. */
    mode?: 'physical' | 'ladybug';
    /** `--sky-density`: 145 / 577 / 2305 sky patches. Default 1. */
    skyDensity?: 1 | 2 | 4;
    /** `--patch-samples K`: physical mode, K stratified rays per sky patch, a square number. Default 1. */
    patchSamples?: number;
    /** `--ground RHO`: ground reflectance 0..1. Default 0.2. */
    ground?: number;
    /** `--fresnel on|off`: glass by angle (off: tau at every angle). Default on. */
    fresnel?: boolean;
    /** `--bounces N`: up to N = 0..20 diffuse reflections (physical mode). Default 0. */
    bounces?: number;
    /** `--bounce-samples M`: paths per sensor for bounces, 64..65536. Default 1024. */
    bounceSamples?: number;
    /** `--timeseries F.npy`: per-step values, float32 [N x T], and F.npy.hoys.txt. */
    timeseries?: string;
    /** `--dump-bits F.npy`: visibility per sensor and direction (validation). */
    dumpBits?: string;
    /** `--dump-sky F`: the gendaymtx sky blocks (ladybug mode; validation). */
    dumpSky?: string;
    /** `--dump-sun F`: per step date, sun positions, DNI and DHI (validation). */
    dumpSun?: string;
}
/** views' flags (docs/views.md): field of view, distances, targets, ground and glass. */
export interface ViewsOptions extends CommonOptions {
    /** `--target NAME=GROUP;...`: rays ending on a mesh count for its group, a share column per group (at most 16). */
    target?: (TargetSpec | string)[];
    /** `--fov H[,LO,HI]`: H degrees wide about the normal (up to 180) or 360 all around, elevation LO..HI. Default 120,-70,50. */
    fov?: number | [h: number, lo: number, hi: number];
    /** `--gaze`: the field about the normal, or about the normal turned level (as a person looks). Default normal. */
    gaze?: 'normal' | 'level';
    /** `--cell DEG`: degrees per ray, rows and columns. Default 2. */
    cell?: number;
    /** `--jitter on|off`: each ray at a random point of its cell (off: the cell centres). Default on. */
    jitter?: boolean;
    /** `--distance STAT,...`: isovist, mean, min, group or none (default: isovist with fov 360, else mean,min). */
    distance?: ('isovist' | 'mean' | 'min' | 'group' | 'none')[];
    /** `--far M`: what open counts as, and the cap of every distance. Default 1000. */
    far?: number;
    /** `--reach D,...|en17037`: share of the field that sees at least D metres (at most 8; en17037 = 6,20,50). */
    reach?: number[] | 'en17037';
    /** `--ground-z Z`: height of the virtual ground in metres (default: the lowest point of the meshes). */
    groundZ?: number;
    /** `--dump-rays F.npy`: classes uint8 [rows x rays] and F.npy.depth.npy. */
    dumpRays?: string;
}
/** Raw CLI flags, or typed options turned into them by toArgs(). */
export type RunArgs = readonly string[] | DaylightOptions | ViewsOptions;
/**
 * Typed options -> CLI flags, in field order: camelCase becomes `--kebab-case` (`verbose` is `-v`), true adds a bare
 * flag (false adds none), the on/off flags (virtualGround, transmission, fresnel, jitter) take `on` / `off`, arrays are
 * comma-joined (optics, target and noOcclude `;`-joined), undefined, null and empty arrays are left out. The values are
 * not checked here: the core refuses a bad one with its usage message (DaylightError 1, HTTP 400).
 */
export declare function toArgs(options: DaylightOptions | ViewsOptions): string[];
/** Raw flags as they are, typed options through toArgs(). */
export declare const argsOf: (args: RunArgs | undefined) => string[];
