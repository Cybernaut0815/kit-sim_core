# Validation

`validate/` is a uv project (Python 3.12) that downloads Radiance rad5R4 and EPW files, builds fixtures with
ladybug-geometry, computes references (gendaymtx, ladybug Sunpath, RadiationStudy, DirectSunStudy, pvlib, rcontrib with
materials) and compares them with the CLI output layer by layer; `validate/ingest.py` times the mesh transport per
format. See [validate/README.md](../validate/README.md) (`compare.py transmit` and `compare.py reflect` for transmission
and reflected light, after `setup.py`, `fixtures.py` and `reference.py --scene canopy glass_canopy canopy_ground
glass_canopy_ground canyon plane`).

## Results (`validate/compare.py`, Boston + Chicago EPWs)

| layer | reference | result |
|---|---|---|
| sun position | ladybug Sunpath (8760 h, 5 sites) / gendaymtx -D / pvlib SPA | 5e-7 deg, 0 day/night flips / 5e-5 deg / 0.015 deg |
| sky matrix | gendaymtx -A -O1 ASCII (rad5R4) | byte-identical rows |
| cumulative radiation, `--mode ladybug` | ladybug-radiance RadiationStudy (faces and vertices, MF1/MF2, north 0/30) | ~1e-6 relative on every sensor after edge-ray decomposition |
| sun hours, `--mode ladybug` | ladybug-radiance DirectSunStudy | exact |
| visibility bits | rcontrib intersection matrix | all mismatches are edge-grazing or zero-cosine rays |
| `--mode physical` vs ladybug | RadiationStudy | 1-4 % mean difference (exact sun vs 4-patch binning, patch vectors), informational |
| USD (python binding) vs OBJ | same scene through both | identical except edge-grazing rays |
| `.dmesh` vs `.obj` | same meshes through both | byte-identical values |
| `--clear-sky` DNI/DHI | pvlib `Location.get_clearsky('ineichen')`, SPA positions, 8760 h, Boston TL 3 | 0.21 % max (sun > 5 deg), 4e-5 on the annual sums; model goldens 1e-6 |
| analytic | sum(DHI + DNI sin alt), sum(DNI cos) on a south wall | < 1e-6 |
| `--transmission on`, porous sheet (tau 0.5) over ground sensors | rcontrib -ab 1 with Radiance `trans` (tau straight through), gendaymtx -O1 / -O0 skies of a sky-only Boston year (DNI 0) | sums +0.04 % radiation, +0.17 % illuminance (limit 1 %); per sensor p95 0.10 % / 0.23 % (limit 0.5 %, a budget for the sky at the NOAA apparent sun (gendaymtx: sun.c), tau stored as 128/255, 64 samples per patch and a reference noise of 0.019 %) |
| `--bounces 1`, `2` (65536 paths per sensor): street canyon (H/W 1, ground mesh), porous sheet, glass sheet, both sheets over a ground mesh (faces looking up and down) | rcontrib -ab 2, 3 with the same materials (one sample per reflection: `-lw 0.5/AD`; at layer 8's first `-lw` rad5R4 skews reflections away from the surface normal, -15 % in a plane's zenith cap) | a plane's single reflection: rcontrib within 2.0 sigma of the cosine law, ours 1.6e-5 of rho x horizontal. Totals of the sky-only year: sums -0.15 to +0.17 % (limit 2 %), per sensor p95 0.08-0.70 % (limits: the direct pass's budget, canyon walls 0.9 %, + 3 sigma). The glass sheet's mirror +0.01 % (radiation). Reflected light on Boston / Chicago, sun binned on both sides: canyon +0.40 to +0.61 %, sheet -0.43 to +0.02 %, sheet over a ground +0.33 to +0.47 % (limit 3 %); per sensor p95 0.51-0.65 % of the total (limits 0.84-0.97 %: 0.1 % for the sun's NOAA vs sun.c position + 3 sigma of both sides' noise). The N = 0 canyon checks the physical direct pass on walls: sums -0.18 % / -0.03 %, p95 0.74 % (the sky at the NOAA sun, up to -0.41 % on a south wall, and 64 samples per patch where walls cut the sky) |

## Views (`validate/views.py`, `tests/check_views.cpp`)

`views.py rays` runs `views.exe --jitter off --dump-rays` on 10 cases (`tests/box.obj` in view, vertex, 360 and
own-surface modes, the box and tilt30 fixtures, a generated street with a lake and a park as targets: facade, 360 at
1.6 m, `--virtual-ground off`, `--ground-z`) and rebuilds every ray on the host (sensors, frames and cell centres as
the core makes them). Radiance `rtrace -ab 0` traces it against the core's triangles at their float32 positions, one
modifier per class; the virtual ground and the sky / ground split of misses are applied on the host. Hard gates per
case: >= 99.9 % of the compared rays in the same class, >= 99.9 % of those with a distance within
`|dt| <= 1e-5 t + 1e-4 m` (grazing hits and rays within 1e-6 of a sign test not compared), and the output columns
recomputed from the dump (shares and reach 1e-9, log means 1e-6 relative, minima exact). `views.py ladybug` puts the
unobstructed share next to Ladybug's LB View Percent (types 0, 2, 3), printed only. Details:
[validate/README.md](../validate/README.md#viewspy-viewsexe-ray-by-ray). `tests/check_views.cpp` (ctest `check`) checks
the same against an analytic host model of axis-aligned scenes, plus the rules, chunking, jitter and resident-scene
labels; `golden.py` keeps 6 views cases byte for byte.

| check | reference | result |
|---|---|---|
| ray classes and distances, 10 cases | rtrace + the host's virtual ground | 100.000 % in class and distance in every case (worst distance 0.32 x its tolerance) |
| output columns | the run's own dump | shares within 2e-15, log means 2.3e-7 (float logs), minima exact |
| unobstructed share | Ladybug LB View Percent, resolution 3 | street -0.08 / -0.62 / -1.25 points (types 0 / 2 / 3), facade +0.42 / -0.39 / -0.94 (informational: Ladybug's patch rows are not converged) |
