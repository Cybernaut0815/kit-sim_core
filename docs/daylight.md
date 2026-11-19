# Daylight and solar (`daylight`)

`daylight.exe` is the first tool on the core library: the same meshes, mesh roles, resident scenes and GPU as
`views.exe`, another question. From every face or point of the sensor meshes it casts rays to each sky patch, to the
ground below the horizon and to the sun at every time step (the directions in front of the element), and sums the light
of the weather that reaches the element past the model: radiation (kWh/m2 over the run, also its direct and diffuse
parts), irradiance (mean W/m2), illuminance (mean lux) and sun hours, a column each. The sky is a port of Radiance
`gendaymtx` (Perez all-weather); the numbers are validated against Ladybug Tools, Radiance and pvlib
([validation.md](validation.md)).

- **Weather and place**: an EPW file (`--epw`: hourly DNI and DHI, the location from its header), a synthetic
  clear-sky year (`--clear-sky` for `--lat --lon`: cloudless, an upper bound), or none for sun hours (`--lat --lon`).
- **Time**: annual by default (a step per weather hour), one instant (`--time`), or a range (`--from --to --step`),
  optionally only some hours of each day (`--hours`).
- **Modes**: `physical` (default) or `ladybug`, which reproduces Ladybug's radiation and direct-sun studies bit for bit
  ([modes.md](modes.md)). Every surface is opaque and black by default (the ground below the horizon reflects,
  `--ground`); `--optics` gives meshes or face slots a reflectance and a transmittance, which `--transmission on`
  (light through glass and porous slots) and `--bounces N` (the light the model reflects) use, in physical mode.
- **Output**: a row per element of the sensor meshes in input order, a column per quantity and side (`radiation_front`,
  `sunhours_back`); text or NumPy `.npy`, and a JSON manifest that maps the rows to the meshes ([Output](#output)).

```
daylight roof.obj --context city.obj --epw site.epw --out rad.txt                    # annual kWh/m2 per face
daylight facade.obj --context city.obj --epw site.epw --quantity radiation,sunhours --sides both --out v.npy
daylight roof.obj --clear-sky --lat 52.52 --lon 13.41 --out clear.txt                # no weather file: a clear-sky year
daylight facade.obj --lat 52.52 --lon 13.41 --quantity sunhours --from 06-21T00:00 --to 06-22T00:00 --step 10
daylight roof.obj --context city.obj --epw site.epw --mode ladybug --no-self-occlude --out lb.txt   # Ladybug's numbers
daylight --bench                                                                     # grid + boxes, timings only
```

Mesh files, roles and names: [input.md](input.md). Build: `cmake --build --preset release` makes
`build/RelWithDebInfo/daylight.exe` beside `views.exe` (the same `src/main.cpp`). C++ API: [cpp.md](cpp.md#daylight);
the other languages: [api.md](api.md). Timings: [performance.md](performance.md).

## Mesh roles

The same roles and flags as views ([views.md](views.md#mesh-roles)), so one set of meshes serves both tools:

| role | CLI | C++ |
|---|---|---|
| sensor: analysed (a row per element), casts shade | positional `FILE...` | `Mesh::sensor = true` (`MeshView::sensor`) |
| context: casts shade, never analysed | `--context FILE` | `sensor = false` |
| analysed but invisible to rays (casts no shade), e.g. a grid of measuring points in a room | `--no-occlude NAME` (one sensor mesh), `--no-self-occlude` (every sensor mesh) | `noOcclude`, `selfOcclude = false` |
| glass or porous: light passes by its transmittance | `--optics NAME[/SLOT]=RHO,TAU[,glass\|porous]` with `--transmission on` | `optics`, `transmission = true` |
| ignored | not passed | not in the list |

An interior: the room as context, a window as its own mesh with a glass row, the measuring grid under `--no-occlude`,
as in views' [Glass and interiors](views.md#glass-and-interiors) (the same [tests/room.obj](../tests/room.obj) and
[tests/room_grid.obj](../tests/room_grid.obj)):

```
daylight tests/room_grid.obj --context tests/room.obj --no-occlude grid --optics window=0,0.64,glass --transmission on --clear-sky --lat 52.52 --lon 13.41
```

## Flags

`daylight --help` prints the short form; the C++ `daylight::Options` field for each flag is given for in-process
callers. The C ABI and the bindings take the flags verbatim, except mesh paths, `--context`, `--out`, `--bench` and
`-h` (their meshes and results stay in memory). `views.exe` has its own flags: [views.md](views.md#flags).

| flag | value | default | meaning | `Options` |
|---|---|---|---|---|
| `FILE...` | `.dmesh` `.obj` `.usd*` | required (or `--bench`) | sensor meshes: analysed, and blocking unless `--no-self-occlude` (or `--no-occlude` names them) | `Mesh::sensor = true` |
| `--context FILE` | mesh file, repeatable | none | blocks rays only, never analysed | `Mesh::sensor = false` |
| `--on` | `faces` \| `vertices` | `faces` | element = polygon (sensor at the centroid, face normal) or point (area-weighted vertex normal) | `onVertices` |
| `--mode` | `physical` \| `ladybug` | `physical` | sky/sun model, see [modes.md](modes.md) | `mode` |
| `--quantity` | `Q[,Q...]` | `radiation` | one output column per quantity: `radiation` kWh/m2 over the run, `direct` (sun only: beam x transmittance), `diffuse` (sky + ground, x transmittance; with `--bounces` also the light the model reflects, its reflected sun included), `irradiance` mean W/m2 (instantaneous for a single `--time`), `illuminance` mean lux (physical mode), `sunhours` equivalent full-sun hours (an hour behind a surface transmitting 0.6 counts 0.6; with every surface opaque, the default, an exact count of sunlit hours). Transmittance is 1 for open sky and 0 behind an opaque surface; between only with `--transmission on`. Reflected light (`--bounces`) adds to radiation, diffuse, irradiance and illuminance, never to direct or sun hours | `quantities` |
| `--sides` | `front` \| `back` \| `both` | `front` | receiving hemisphere of each element; `both` = two columns per quantity (`_front`, `_back`) | `sides` |
| `--cull-backfaces` | flag | off | occluder triangles block only from their front (winding) side; default both sides. With `--transmission on` a closed translucent solid is then crossed once, through its front faces (a crown counts its tau once, not twice) | `cullBackfaces` |
| `--no-self-occlude` | flag | off | sensor meshes do not block rays (only `--context` files do) | `selfOcclude = false` |
| `--no-occlude NAME[;NAME...]` | sensor mesh names, repeatable | none | these sensor meshes are analysed but invisible to rays, as every sensor mesh is under `--no-self-occlude`: they cast no shade and reflect nothing (`--bounces`), e.g. a grid of measuring points inside a room gets the room's shade and casts none. NAME as in `--optics` (`.dmesh` stem, OBJ `o`/`g` group, USD prim path); `;` packs several into one token (a name containing `;` cannot be addressed). An empty NAME: exit 1 at parse; an unknown or ambiguous name, or a `--context` mesh: exit 1 after loading (library: `run()` fails, server: a job error) | `noOcclude` |
| `--epw FILE` | EPW path | none | hourly weather (DNI/DHI) and location from the header; 8760 or 8784 rows, hour-ending in the file's own zone, sun evaluated at mid-hour. PVGIS files (recognised by their `Irradiance Time Offset` comment) built from its satellite series are written about 0.7 h ahead of their hour: the irradiance is moved back by the offset measured from the daylight edges (`-v` prints it); its ERA5-based files (declared offset -0.5) are aligned and left alone. With `--lat/--lon` elsewhere the file keeps its own solar day: each row meets the site's sun at the same solar time | `read_epw` -> `Weather` |
| `--clear-sky` | flag | off | no weather file: synthesise an hourly clear-sky year (Ineichen-Perez model, pvlib formulation) for `--lat/--lon`; cloudless upper bound, typically 20-50 % above a TMY; exclusive with `--epw` | `clear_sky_weather(lat, lon, tz, TL, alt, w)` |
| `--turbidity TL` | Linke turbidity | `3` | atmosphere for `--clear-sky`: 2 very clear mountain, 3 clear, 4-5 hazy / urban, 6-7 polluted | argument of `clear_sky_weather` |
| `--altitude M` | metres | `0` | site elevation for `--clear-sky` (air pressure, model coefficients) | argument of `clear_sky_weather` |
| `--lat DEG` `--lon DEG` | degrees, N and E positive | EPW header | location; required without `--epw` (sun hours, clear sky); overrides the EPW header | `lat`, `lon` |
| `--tz H` | hours, E positive | EPW header, else `round(lon / 15)` | the clock of `--time`, `--from`, `--to` and `--hours`: standard time, no DST. An EPW keeps its own clock; the timestamps are moved into it, so the weather of an hour stays with the sun of that hour whatever `--tz` says | `tz` |
| `--time TS` | `[YYYY-]MM-DDTHH:MM` | | evaluate one instant (year ignored; Feb 29 only with a leap EPW); DNI/DHI interpolated between the hours around it | `t0` |
| `--from TS --to TS` | timestamps | | evaluate the range `[from, to)` in `--step` increments, the sun at the middle of each step (the last step of the range, or of a day's `--hours` window, may be shorter) | `t0`, `t1` |
| `--step MIN` | minutes | `60` | step of `--from/--to` (DNI/DHI interpolated between the hour centres; 60-min steps on whole hours read the hours exactly) | `stepMin` |
| `--hours HH:MM-HH:MM` | times of day, end up to `24:00` | whole day | with `--from/--to`: keep only the steps in `[start, end)` of every day, e.g. 1 Jun to 31 Aug 09:00 to 17:00 (an analysis period) | `dayFrom`, `dayTo` (minutes) |
| (no time flags) | | | annual: one step per weather row at the mid-hour | `t0 = t1 = NAN` |
| `--sky-density` | `1` \| `2` \| `4` | `1` | sky patches 145 / 577 / 2305 (Reinhart MF); ladybug mode: 1 or 2 | `mf` |
| `--patch-samples K` | square number | `1` | physical mode: K stratified rays per patch (4, 9, 16 smooth the patch discretisation) | `patchSamples` |
| `--ground RHO` | 0..1 | `0.2` | ground reflectance (Lambertian ground = mirrored sky) | `ground` |
| `--virtual-ground` | `on` \| `off` | `on` | the virtual ground below the horizon: rays escaping downward see a Lambertian ground of reflectance `--ground` (so does the `--bounces` ground bin). Off: no ground light, the numbers of `--ground 0`. A modelled ground mesh hides the virtual ground from the rays it stops anyway. `views` takes the same flag for its ground plane ([views.md](views.md#virtual-ground)) | `virtualGround` |
| `--optics SPEC[;SPEC...]` | `NAME[/SLOT]=RHO[,TAU[,glass\|porous]]`, repeatable | every slot `rho 0.2`, `tau 0` | numbers per mesh or face slot (`.dmesh` slot block, see [input.md](input.md)): RHO diffuse reflectance, TAU transmittance at normal incidence, `rho + tau <= 1`. Kind `porous` when TAU > 0 (tau at every angle: screens, trees) unless `glass` (Radiance glass, n 1.52: RHO 0, 0 < TAU <= 0.918; tau 0.64 = double low-e). NAME is the core mesh name (`.dmesh` stem, OBJ `o`/`g` group, USD prim path; for TypeScript callers the `dmeshStems()` output); the SPEC splits at its last `=`, `/SLOT` only as a trailing `/` + digits (`Facade/East` is a name, `/World/Roof/2` is slot 2 of `/World/Roof`); `*` = every mesh; `;` packs several SPECs into one token (a name containing `;` cannot be addressed). Precedence NAME/SLOT > NAME > `*` > built-in. Values out of range: exit 1 at parse; an unknown or ambiguous name, a slot out of range or a row given twice: exit 1 after loading (library: `run()` fails, server: a job error). No effect by itself: TAU needs `--transmission on`, RHO needs `--bounces` (without it every surface is black) | `optics` (`daylight::Optics`) |
| `--transmission` | `off` \| `on` | `off` | on (physical mode): light passes glass / porous slots, weighted by their transmittance at each crossing (a closed thin solid, e.g. a pane modelled as a 1 cm box, counts once, and so does a double-faced sheet, every face repeated with the opposite winding). Free without a translucent slot (TAU > 0 on faces that block: a sensor mesh's slots under `--no-self-occlude` or `--no-occlude` never count); with one, rays continue through it (one any-hit per crossing) and visibility takes 8 bit per direction instead of 1 (8x memory, chunked). Off: every surface opaque, as before | `transmission` |
| `--fresnel` | `on` \| `off` | `on` | glass transmittance by angle (Radiance `glass`: both faces, internal reflections, the longer path in the glass; 0.64 at normal incidence passes 0.54 at 60 deg, 0.12 at 85 deg) and, with `--bounces`, its mirror R(theta); off: glass passes tau at every angle and never mirrors, with or without `--transmission` | `fresnel` |
| `--bounces N` | `0`..`20` | `0` | reflected light (physical mode; ladybug mode refuses N > 0): what reaches a sensor after up to N diffuse reflections off the model, each by its slot's RHO. Glass mirrors its Radiance R(theta) (`--fresnel on`; not counted in N), translucent slots pass light with `--transmission on` (otherwise every slot is opaque: glass then mirrors R(theta) and absorbs the rest), a thin solid's exit face passes it. Paths leave each sensor cosine-weighted and are binned where they escape: a sky patch of `--sky-density` (with the sun binned into the patches as `gendaymtx` does) or the virtual ground (`--ground`), as `rcontrib -ab N+1` daylight coefficients. 1 is the common outdoor setting (Honeybee's exterior studies use `-ab 2`); Russian roulette from the second reflection on keeps higher N cheap outdoors; interiors need 5-8. A ground mesh hides the virtual ground from the rays it stops (without bounces those see a black surface; with them its RHO reflects the sky and sun it receives, shaded by the model). 0: no reflection pass, no glass mirror, today's numbers | `bounces` |
| `--bounce-samples M` | `64`..`65536` | `1024` | paths per sensor for `--bounces`, stratified (Hammersley points, rotated per sensor) and deterministic: a run repeats its numbers, chunking and launch sizes change nothing. Noise falls as 1 / sqrt(M); annual and long-range values average it out, a single `--time` instant does not (the sun sits in a few patches): raise M (4096-16384) for instants and for small differences between variants | `bounceSamples` |
| `--north DEG` | degrees (a finite number), counter-clockwise from +Y | `0` | model north (ladybug convention; 90 = west) | `north` |
| `--offset M` | metres (a finite number) | `0` | sensors moved along their normal (Grasshopper uses 0.1) | `offset` |
| `--out FILE[.npy]` | path | stdout | values: text (one line per element, `%.10g` columns) or NumPy float64 `[N x C]`; manifest at `FILE.json` | `Result` |
| `--timeseries F.npy` | path | off | per-step values, float32 `[N x T]` (W/m2, or lux for illuminance) for the non-night steps; `F.npy.hoys.txt` lists the hours of year of the evaluated instants, in the `--tz` clock | `seriesPath` |
| `--dump-bits F.npy` | path | off | validation: visibility bits uint32 `[N x words]`, direction order sky \| ground \| sun; with a translucent slot (`--transmission on`) the transmittance uint8 `[N x directions]` instead (x255, 0 = culled or blocked) | `dumpBitsPath` |
| `--dump-sky F` | path | off | validation (ladybug mode): the two `gendaymtx -A` blocks | `dumpSky` |
| `--dump-sun F` | path | off | validation: per step hoy, date, NOAA and sun.c positions, DNI, DHI | `dumpSun` |
| `--bench [NSENS NTRIS]` | counts | `1000000 10000000` | synthetic grid + boxes benchmark, prints timings, no output file | |
| `-v` | flag | off | timings and sizes on stderr (`info: read ...`, `info: run ...`, `info: output ...`) | `verbose` |
| `-h` `--help` | | | usage | |

Environment: `CUDA_VISIBLE_DEVICES` selects the GPU. Exit codes: 0 ok, 1 usage error (message + usage on stderr),
2 runtime error (file, weather, scene, CUDA/OptiX). Weather is needed for every quantity except `sunhours`; without
`--epw` or `--clear-sky` only `--quantity sunhours` runs (`--lat --lon` required, `--tz` optional).

## Output

- Rows: one per element of every sensor mesh, meshes in input order, elements in mesh order (`--on faces`: one row
  per input polygon in `counts` order, a quad is one row; `--on vertices`: one row per point in point order). Nothing
  is reordered or dropped, so `values[first:first+count]` from the manifest's `meshes` entry is exactly that mesh's
  face or point attribute.
- Columns: `columns` in the manifest, `<quantity>_<side>` (`radiation_front`, `sunhours_back`, ...), in `--quantity`
  order, sides inner.
- Manifest `FILE.json`: `{"columns": [...], "duration_h": H, "dirs": {"sky": P*K, "ground": P*K, "sun": S},
  "meshes": [{"name": "...", "first": 0, "count": N0}, ...]}`. Mesh names: `.dmesh` file stem, OBJ `o`/`g` group, USD
  prim path (C++ reader) or sanitised prim path (Python converter, see its `meshes.json`).
- Units: `radiation`/`direct`/`diffuse` kWh/m2 summed over the evaluated steps (x1000 = Wh/m2), `irradiance` W/m2
  averaged over the evaluated hours, `illuminance` lux averaged, `sunhours` hours.
- Library: `Result::values` row-major `[N x C]`, `Result::columns`, `Result::ranges` = `(name, count)` per sensor mesh
  in input order.

## Examples

Run with the exe of commit 89b9327; the outputs are pasted as printed. [examples/cli/run.ps1](../examples/cli/run.ps1)
runs these and more flag sets of both tools ([examples.md](examples.md#cli)).

**`tests/box.obj`, a clear-sky year in Berlin** (four 2 m ground quads at z 0 facing up, and a 1 m box floating 0.5 m
above the fourth; both meshes are sensors and block). Without `--out` the values go to stdout and no manifest is
written; the columns are `radiation_front sunhours_front`.

```
daylight tests/box.obj --clear-sky --lat 52.52 --lon 13.41 --quantity radiation,sunhours
1607.184707 4456
1600.866545 4456
1468.185887 4063
926.1058524 3587
41.08718741 0
42.40675941 0
1610.838706 4456
1610.838706 4456
1610.09265 3654
1620.798476 3654
1079.911348 2194
1089.281121 2194
334.4936637 802
346.176471 802
1004.434329 2262
1032.356277 2262
```

Lines 1-4 are the ground quads: the first two, in the open, get about 1600 kWh/m2 and are sunlit in every one of the
4456 hours the sun is up (the manifest's `dirs.sun`); the third, west of the box, loses morning sun to it (4063 hours);
the fourth, under it, is shaded whenever the sun is high (926 kWh/m2, 3587 hours). Lines 5-6, the box's bottom
triangles, face down: no sun, only the virtual ground's light beyond the ground quads, which are black (41 kWh/m2).
Lines 7-8, its top, see the whole sky. Lines 9-16 are its sides, two triangles each: south 1610-1621 kWh/m2 and 3654
hours, east 1080-1089 and 2194, north 334-346 and 802, west 1004-1032 and 2262.

**Sun hours of one day, no weather file.** `--quantity sunhours` needs only the place (`--lat --lon`, and `--tz` for
the clock of `--from` and `--to`): 21 June in 10-minute steps.

```
daylight tests/box.obj --quantity sunhours --lat 52.52 --lon 13.41 --tz 1 --from 06-21T00:00 --to 06-22T00:00 --step 10
16.66666716
16.66666716
13.66666707
10.16666697
0
0
16.66666716
16.66666716
9.333333611
9.333333611
8.333333582
8.333333582
7.333333552
7.333333552
8.333333582
8.333333582
```

The open quads and the box's top have the sun for 16.7 hours, sunrise to sunset; the quad west of the box 13.7, the
one under it 10.2; the box's sides 9.3 (south), 8.3 (east, west) and 7.3 (north: the early morning and the evening
sun); its bottom none.
