# View analysis (`views`)

`views.exe` is the second tool on the core library: the same meshes, mesh roles, resident scenes and GPU as
`daylight.exe`, another question. From every face or point of the receiver meshes it casts one ray per cell of a field
of view, about the element's normal (looking out of a window) or all around (turning on the spot), and reports which
share of the field ends on the sky, on the ground, on an obstruction or on each target group (water, a park, ...), and
how far the element sees. Distances measure view quality: short ones (a wall in front of the window) are bad, long or
open ones good; an open ray counts as `--far` (1000 m), good but bounded. All around, the default measure is the
isovist instead: the volume in view (m³), or with horizontal rays only the area in view (m²), as space syntax takes it
([Isovist](#isovist)).

```
views facade.obj --context city.obj --out v.txt                                    # view mode: 120 x 120 deg about each face's normal
views facade.obj --context city.obj --context lake.obj --target lake=water --reach en17037 --out v.npy
views floor.obj --context city.obj --no-occlude floor --fov 360 --offset 1.6 --out v.txt   # a standing eye, all around (flat floor): volume
views grid.obj --context room.obj --no-occlude grid --fov 360,-90,90 --offset 1.2          # interior: the 3D isovist (volume, m3)
views grid.obj --context room.obj --no-occlude grid --fov 360,0,0 --offset 1.2             # the 2D isovist (area, m2), horizontal rays
views grid.obj --context room.obj --no-occlude grid --fov 360 --offset 1.2 --distance mean,min   # distances instead of the volume
views grid.obj --context room.obj --no-occlude grid --fov 360 --offset 1.2 --optics window=0,0.64,glass --transmission on --far 50
views --bench                                                                         # street-grid city, timings only
```

Rows, manifest ranges and mesh names work as in daylight ([daylight.md](daylight.md#output), [input.md](input.md)).
Build: `cmake --build --preset release` makes `build/RelWithDebInfo/views.exe` beside `daylight.exe` (the same
`src/main.cpp`, built with `VIEWS_CLI`). C++ API: [cpp.md](cpp.md#views); the other languages: [api.md](api.md).
Timings: [performance.md](performance.md#views).

## Field of view

`--fov H[,LO,HI]` and `--cell DEG` give one grid for every element: `round(H / DEG)` columns by `round((HI - LO) / DEG)`
rows (at least one each, at most 2^20 = 1,048,576 rays per element), so a cell is `H / columns` by `(HI - LO) / rows`
degrees. The default `120,-70,50` at 2 deg is 60 x 60 = 3,600 rays per element; `--fov 360` (LO and HI kept) is
60 x 180 = 10,800; 1 deg cells are 4x the rays.

- **View mode** (0 < H <= 180, the default): the field is centred on the element's normal (the gaze is the normal; a
  back side, `--sides back|both`, looks along the reversed normal). Forward = the normal, right = horizontal (normal x
  +Z, normalised), up = right x forward. Azimuth runs from -H/2 (left) to +H/2 (right), elevation from LO to HI about
  the plane of forward and right: on a vertical face up is +Z and the default field is a level gaze 60 deg to either
  side, 50 deg up and 70 deg down; on a tilted face the field tilts with it (up = uphill along the face). A face
  looking straight up or down has no uphill and takes a fixed frame: right = the model's east (90 deg clockwise of
  model north, +X at `--north 0`, made square to the normal), so its up is south when it looks up and north when it
  looks down. "Straight" allows a horizontal part of the normal under 1e-3 (about 0.06 deg), so float noise in a face
  modelled horizontal does not turn its field. A face tilted any more takes its right from the tilt, and the field's
  shape (120 deg wide, 50 deg up and 70 deg down by default) turns with it: two nearly horizontal faces sloping
  different ways see differently turned fields. Where that matters, `--fov 360` (one world frame for every element,
  e.g. `--fov 360,0,90` for the hemisphere an upward face sees) does not depend on the tilt. Every ray leaves in front
  of the surface.
- **Level gaze** (`--gaze level`, view mode only; default `normal`): the field is centred on the normal turned level,
  as a person looks out of a window: forward = the normal's horizontal part (normalised), right = the same horizontal
  right as above, up = +Z, so elevation runs from LO to HI about the horizon. A vertical face gets exactly the frame
  above; a tilted face no longer tilts its field. A face looking straight up or down (the same 1e-3 test) has no level
  direction and keeps the frame above. A back side turns its reversed normal level. On a tilted face the level field
  can reach behind the face's plane: with `--offset 0` those directions are its own surface where its mesh blocks rays,
  as in 360 mode. A flat field is one row of horizontal rays about the level gaze. `--fov 360` ignores the flag.
- **360 mode** (H = 360): one world frame for every element: azimuth clockwise from model north (+Y, turned
  counter-clockwise by `--north` as in daylight), elevation about the horizon. With `--offset 0` the directions behind
  the element's own plane are not traced where its mesh blocks rays (own surface, below).
- **Flat field** (`--fov H,0,0`, LO = HI = 0 only): one row of rays along the horizon of the frame, `round(H / DEG)`
  of them: horizontal in 360 mode (and about the normal of a vertical face), the plane of forward and right on a
  tilted one. The 2D isovist: `--fov 360,0,0` gives its area. A flat field traces every direction, also at `--offset
  0` (a horizontal ray from a floor lies in the floor's plane, it is not behind it).

**Weights.** A cell weighs its exact solid angle, `dAz x (sin el_top - sin el_bottom)`, normalised so the field sums
to 1: every share is a share of the field's solid angle (a cell near the zenith or the nadir counts less than one at
the horizon, as it covers less of the view). In a flat field every ray weighs the same, `1 / round(H / DEG)`: a share
of the angle all around (or of the fan).

**Jitter.** `--jitter on` (default) sends each ray through a random point of its cell, uniform in solid angle (azimuth
and sine of elevation uniform), drawn from a hash of the element's row, the cell and the side: a run repeats its
numbers bit for bit however it is chunked, while the same element at another row (other meshes before it) gets other
points. A fixed grid would alias thin, far targets into bands: a lake 200 to 500 m away seen from 10 m up is a
1.7 deg strip, less than a cell. `--jitter off` takes each cell's middle in azimuth and in sine of elevation
(validation, images from `--dump-rays`). Jittered, each element's numbers are an unbiased estimate with some noise:
over 200 copies of the facade example's ground floor (below) about 1e-3 on a share (5 % of the 2.4 % water) and 0.3 %
on `distance` at 2 deg, 2-3x less per halving of `--cell` (4x the rays). Use 1 deg for thin or far targets or
element-by-element comparisons; sums and means over many elements average the noise out.

## Ray classes

Every ray takes the closest hit along its whole length (no maximum distance: `--far` caps the distances, never the
classes) and ends in one class:

| class | the ray | its distance |
|---|---|---|
| `sky` | meets nothing and points above the horizon, or along it | open (`--far`) |
| `ground` | points below the horizon and meets nothing down to the virtual ground; never sky | the virtual plane's; open with `--virtual-ground off` |
| `obstruction` | ends on a mesh that is not a target: context, a receiver (receivers block by default), a modelled terrain | the hit's |
| target group | ends on a mesh that `--target NAME=GROUP` names | the hit's |
| own surface, counted in `obstruction` | 360 mode or `--gaze level` with `--offset 0`, the element's mesh blocking rays: a direction behind its own plane, not traced | none |

The horizon itself counts as sky (as daylight's sky patches and Ladybug's HorizontalRadial count it): a ray exactly
along it that meets nothing is `sky`; the virtual ground ends only rays pointing below it. Every ray of a flat field
all around is such a ray (`--fov 360,0,0` gives sky 1 in the open, jittered or not), and at `--jitter off` every ray
of a row centred on the horizon (`--fov 360,-1,1 --distance mean,min`); jittered, those fall on either side.

A degenerate face (no area, so no normal; `-v` warns) traces no ray: its whole field is own surface, `obstruction` 1
without a distance (daylight gives such a face 0).

Own surface: in 360 mode (or with `--gaze level`) with `--offset 0`, a direction behind the element's plane would only see the surface the
element lies on; it is not traced and counts as `obstruction` without a distance (as daylight's hemisphere cull). With
a non-zero `--offset` these directions are traced like any other: a floor grid lifted 1.6 m sees the floor (unless the
floor is `--no-occlude`) and over a balcony's edge. A receiver that blocks no rays (`--no-occlude`, `--no-self-occlude`)
has no own surface: at `--offset 0` too its elements trace the directions behind their plane, through the mesh (a
floor grid on the virtual ground sees ground below it, one over a context floor that floor). View mode about the
normal has no own-surface rays (every ray leaves in front of the surface). A ray that meets the element's
own plane through float error (an element lying in a large triangle) passes it as in daylight ([input.md](input.md));
after four such crossings it ends there as an obstruction.

### Virtual ground

`--virtual-ground on` (default) puts a horizontal plane at the lowest point of all meshes given (receivers and
context), or at `--ground-z Z` (metres), and ends every downward ray there: the street seen from a facade when no
terrain is modelled. A downward ray is traced to 1 cm below the plane, so geometry lying on the plane (a terrain or a
lake at the lowest point) wins over it; a ray that meets nothing there is `ground` at the plane's exact distance,
(eye height above the plane) / sin(angle below the horizon). Geometry below a raised `--ground-z` is hidden. An eye at
or below the plane (a vertex at the foot of a facade, `--offset 0`) sees `ground` at 0.1 m in every downward direction;
an eye below it (a basement under a raised `--ground-z`, a floor grid wound downward with an `--offset`) still sees up
through the plane (sky or geometry above), a mix that means little: `-v` counts such eyes. The default height is the
lowest point of what the run is given, so a partial model (upper floors only, no context down to the ground) puts the
plane at its own foot: give `--ground-z` there (`-v` prints the height).
`--virtual-ground off` (the flag daylight shares): downward rays that meet nothing are still `ground`, but count as
open (`--far`) and reach every D.

A modelled terrain is a mesh like any other: rays that end on it are `obstruction` unless it is a target
(`--target terrain=terrain`; `ground` is reserved for the virtual ground). A target lying exactly on a terrain mesh
z-fights with it (which of two coplanar triangles is the closest hit is arbitrary): lift it at least 1 cm, or cut it
out of the terrain. Lying on the virtual plane is fine.

## Distances

`--distance STAT[,STAT...]` picks the distance columns: `isovist` (`volume` or `area`, below), `mean` (`distance`),
`min` (`distance_min`), `group` (per target group), or `none`. The default depends on the mode: `isovist` in 360 mode
(the interior and space syntax measure), `mean,min` about the normal (the view out of a window). Any mix works:
`--distance isovist,mean,min` gives all three.

A ray's distance d is its hit distance, its virtual-plane distance, or open; d is clamped to [0.1 m, `--far`] and open
counts as `--far` (default 1000 m, so a skyline 3 km away counts as 1000 m). Own-surface rays have none. Every
statistic weighs the rays by their cells' solid angles.

- `distance` (`--distance mean`): the log (geometric) mean, `exp(sum w ln d / sum w)`. Near walls weigh strongly, open
  rays count as good but bounded, and a uniform view gives its own distance:

  | field | arithmetic mean | `distance` |
  |---|---|---|
  | half sky, half a wall at 5 m | 502.5 m | sqrt(1000 x 5) = 70.7 m |
  | 90 % sky, 10 % a wall at 5 m | 900.5 m | 1000^0.9 x 5^0.1 = 588.7 m |
  | a wall at 20 m everywhere | 20 m | 20 m |
  | sky everywhere | 1000 m | 1000 m |

- `distance_min` (`--distance min`): the smallest d in the field, the nearest thing in view (an all-open view gives
  `--far`). `distance` and `distance_min` are NaN when no ray of the field is traced (all own surface: 360 mode at
  `--offset 0` with the whole field behind the element, a degenerate face).
- `--distance group` adds, per target group g, `<g>_distance` and `<g>_distance_min` for whichever of `mean` and `min`
  are on: the log mean and the minimum over the rays that end on g (how far the visible water is); NaN (`nan` in text)
  when g is not in view. `group` alone is a usage error; `none` drops every distance column and stands alone.

**Reach.** `--reach D[,D...]` (metres, at most 8) adds a `reach_<D>` column per D: the share of the field whose ray
travels at least D before it meets anything, by the true distance (not clamped to `--far`): sky reaches every D, a
ground ray by its virtual-plane distance (every D with `--virtual-ground off`), an own-surface ray none. Two D that
print alike (`reach_%g`) are a usage error. `--reach en17037` is `6,20,50`, the minimum, medium and high view distance
levels of EN 17037. `reach_<D>` is a share of the whole field, the street below a window included at its plane
distance, so an open view stays well under 1 (about 0.47 at 50 m from a ground-floor window with the default field,
more higher up); no EN 17037 level is assigned. For the standard's horizontal outside distance take the field above
the horizon (`--fov H,0,HI`) or `--virtual-ground off`, or pick a share threshold.

Shares, minima and reach are exact over the traced rays. The log means sum ln d with a float logarithm (a double one
at the 0.1 m and `--far` clamps): about 1e-7 relative to a double computation; a mean whose every ray sits at the far
clamp (an all-open view, a target wholly beyond `--far`) is `--far` exactly.

### Isovist

`--distance isovist` (the default in 360 mode) adds the column `volume` (m³): the volume of the region the element
sees within its field, or in a flat field (`--fov H,0,0`) the column `area` (m²): the area it sees around it in the
horizontal plane, the classic 2D isovist of space syntax. Each ray ends at its point, min(d, `--far`) along its
direction, d its true distance (no 0.1 m floor): open sky and open ground count as `--far`, so `--far` caps the isovist
as it caps the distances (an interior looking out through glass: `--far 50`, say). An own-surface ray ends at the eye.
The isovist is built between these end points:

- **area**: the polygon through the end points of neighbouring rays, a triangle eye-A-B per pair: sum of
  ½ d_A d_B sin(angle between them). All around it closes (the last ray to the first); a fan (H < 360) runs from its
  first ray to its last.
- **volume**: the mesh through the end points of the grid, two triangles per cell between four neighbouring rays, a
  tetrahedron from the eye to each: sum of (1/6) det[A, B, C]. All around the columns close.

Between two rays the polygon or mesh takes a straight edge: along a wall that is exact, at a room's corner it cuts a
little off, at a depth jump (a door, a column's edge) it bridges near to far. So the isovist is a little smaller than
the space it stands for, the less the finer `--cell`: in tests/room.obj (6.3 x 5.1 x 2.9 m: 32.13 m², 93.18 m³) the
area at 2 deg comes out 32.09 to 32.12 m² and the volume of the whole sphere 92.68 to 92.90 m³ (0.5 % under). The
field's edges are where the rays are: the half cell outside the first and last row (and a fan's first and last column)
is left out, with the default `360,-70,50` at 2 deg the 1 deg bands under 50 deg and over -70 deg (1 % of the field),
and at `360,-90,90` the cone within half a cell of the zenith and the nadir (under 0.01 % at 2 deg). The volume is the
volume of the field as given: `--fov 360` leaves out what lies above 50 deg and below -70 deg (88.7 m³ of the same
room); the whole 3D isovist is `--fov 360,-90,90`. Jittered, the polygon and mesh run through the jittered points.

The isovist needs a polygon or a mesh: at least 3 columns, and 2 rows unless the field is flat (a usage error, exit 1,
otherwise: `--fov 360,-1,1` at 2 deg is one ring; take `--fov 360,0,0` for the area or `--distance mean,min`). The
sums run in double over float directions: about 1e-7 relative to a double computation.

## Mesh roles

| role | CLI | C++ |
|---|---|---|
| receiver: analysed (a row per element), blocks rays | positional `FILE...` | `SimulationMesh::sensor = true` (`SimulationMeshView::sensor`) |
| context: blocks rays (an obstruction), never analysed | `--context FILE` | `sensor = false` |
| target: rays ending on it count for its group | `--target NAME=GROUP`, NAME a context or receiver mesh | `views::Options::targets` |
| analysed but invisible to rays (casts no shade) | `--no-occlude NAME` (one receiver mesh), `--no-self-occlude` (every receiver) | `noOcclude`, `selfOcclude = false` |
| glass the rays look through (a window) | `--optics NAME[/SLOT]=0,TAU,glass` with `--transmission on` | `optics`, `transmission = true` |
| ignored | not passed | not in the list |

Target rules: NAME as in `--no-occlude` (`.dmesh` file stem, OBJ `o`/`g` group, USD prim path); the token splits at its
last `=`; several meshes may share a GROUP; at most 16 groups. GROUP takes letters, digits, `_` and `-` and is not
`sky`, `ground`, `obstruction`, `obstruction_total`, `glazing`, `volume`, `area`, a name starting with `distance` or
`reach`, or one ending in `_distance` or `_distance_min` (another group's distance columns). Exit 1 after loading (library: `run()` fails) on an unknown or
ambiguous name, a mesh named twice, a mesh that blocks no rays (a receiver under `--no-occlude` or `--no-self-occlude`)
and a face slot `NAME/SLOT` (not supported yet: target the whole mesh). A target made only of passed glass (with
`--transmission on`) fails the run (exit 2): no ray can end on it.

These roles and their flags are daylight's ([daylight.md](daylight.md#mesh-roles)): a grid of measuring points under
`--no-occlude`, a window as a glass `--optics` row, the same meshes and files serve both tools.

## Glass and interiors

By default every surface blocks, glass included: a window is an obstruction at its pane. `--transmission on` lets the
rays pass glass: the faces whose `--optics` row is glass (`NAME[/SLOT]=0,TAU,glass`, daylight's rows and numbers) are
passed whatever their TAU, and a ray's class and distance are those of what lies behind. Porous slots (trees,
screens) and everything else still block: they are what one looks at. Each ray that passed glass also counts for
the column `glazing_<side>`, the share of the field seen through glass (outside the shares that sum to 1: a ray
through a window to the sky counts for `sky` and for `glazing`); `--dump-rays` adds 128 to its class. A ray passes at
most 16 panes, then ends as an obstruction at the next. `--optics` without `--transmission on` changes nothing, and
with `--transmission off` (the default) the numbers are those of the opaque scene, bit for bit. Glass is a label of
the run, not of the scene: the resident scene stays daylight's and is reused.

Interiors: the room as context, its windows as meshes of their own (or face slots) with a glass row, the measuring
points as a receiver mesh that `--no-occlude` keeps out of the rays, and all around from eye height (`--offset` above
the grid). [tests/room.obj](../tests/room.obj) is such a room (6.3 x 5.1 x 2.9 m, a window in its south wall, a tree
7.3 m outside) and [tests/room_grid.obj](../tests/room_grid.obj) its 12-face grid at desk height (0.8 m; the eye
0.4 m above it):

```
views tests/room_grid.obj --context tests/room.obj --no-occlude grid --fov 360 --offset 0.4 --target tree=green --optics window=0,0.64,glass --transmission on --reach en17037 --distance mean
```

Over the 12 faces the field holds on average 1.8 % sky, 0.5 % ground and 0.9 % tree seen through the window, 3.3 %
glazing, the rest the room (log-mean distance 2.7 m; 3.4 % of the field sees at least 6 m, 1.9 % at least 50 m). With
`--transmission off` the room is closed: obstruction 1, nothing reaches 20 m. daylight takes the same files and flags
(`--no-occlude grid --optics window=0,0.64,glass --transmission on`) for the light that comes in through the window.

The same room's isovists (the default column all around, without `--distance`):

```
views tests/room_grid.obj --context tests/room.obj --no-occlude grid --offset 0.4 --fov 360,-90,90     # volume
0 0 1 1 92.74380511
0 0 1 1 92.86041495
...
views tests/room_grid.obj --context tests/room.obj --no-occlude grid --offset 0.4 --fov 360,0,0        # area
0 0 1 1 32.09342185
0 0 1 1 32.09531358
...
views tests/room_grid.obj --context tests/room.obj --no-occlude grid --offset 0.4 --fov 360,0,0 --optics window=0,0.64,glass --transmission on --far 20
0.1055555556 0 0.8944444444 0.8944444444 0.1055555556 155.1003411
0.1166666667 0 0.8833333333 0.8833333333 0.1944444444 189.7407646
...
```

The shut room: 92.81 m³ on average over the 12 faces (the room holds 93.18) and 32.11 m² at eye height (its floor:
32.13; columns `sky ground obstruction obstruction_total volume` and `... area`). Through the window, with the rays
capped at 20 m (`sky ground obstruction obstruction_total glazing area`): 11 % of the horizontal rays see the sky through it, and the area grows to
155 to 190 m², a wedge 20 m deep outside on top of the room.

## Flags

`views --help` prints every flag grouped by topic (meshes, field of view, distances, ground and glass, output) with its
default, a few examples and the exit codes; the server serves the same text at `GET /usage/views`. The
`daylight::views::Options` field is given for in-process callers; the Python, TypeScript and C# bindings also take the
flags as typed options ([api.md](api.md#typed-options)). Numbers are whole tokens with `.` as the decimal point, as in
daylight.

| flag | value | default | meaning | `views::Options` |
|---|---|---|---|---|
| `FILE...` | `.dmesh` `.obj` `.usd*` | required (or `--bench`) | receivers: analysed, and blocking unless `--no-self-occlude` (or `--no-occlude` names them); `.usd*` needs the `-DDAYLIGHT_USD=ON` build | `SimulationMesh::sensor = true` |
| `--context FILE` | mesh file, repeatable | none | blocks rays only, never analysed | `SimulationMesh::sensor = false` |
| `--target NAME=GROUP[;NAME=GROUP...]` | mesh names, repeatable | none | rays ending on mesh NAME count for GROUP: a share column per group, in order of first appearance; `;` packs several into one token; rules above | `targets` (`Target {mesh, group}`) |
| `--fov H[,LO,HI]` | degrees | `120,-70,50` | the field: H wide about the normal (0 < H <= 180, view mode) or 360 (all around), elevation LO to HI (-90 <= LO < HI <= 90, or LO = HI = 0: a flat field of horizontal rays, the 2D isovist); H alone keeps LO, HI | `fovH`, `elevLo`, `elevHi` |
| `--gaze` | `normal` \| `level` | `normal` | view mode: the field about the normal, or about the normal turned level with elevation about the horizon ([Field of view](#field-of-view)); faces looking straight up or down keep the normal; 360 mode ignores it | `level` |
| `--cell DEG` | degrees, > 0 | `2` | degrees per ray, rows and columns (rounded to fill the field; at most 2^20 rays per element) | `cell` |
| `--jitter` | `on` \| `off` | `on` | each ray at a random point of its cell (deterministic); off: the cell's middle | `jitter` |
| `--distance STAT[,STAT...]` | `isovist` \| `mean` \| `min` \| `group` \| `none` | `isovist` in 360 mode, else `mean,min` | distance columns: `volume` or, flat, `area` ([Isovist](#isovist)), `distance` (log mean), `distance_min`; `group`: also per target group (with `mean` and / or `min`); `none` alone: no distance column | `distance` (`Dist` bits: `DistIsovist`, `DistMean`, `DistMin`, `DistGroup`; `DistAuto` the default by mode) |
| `--far M` | metres, > 0.1 | `1000` | what open counts as, and the cap of every distance and of the isovist | `farDist` |
| `--virtual-ground` | `on` \| `off` | `on` | a ground plane ends downward rays (`ground` at its distance); off: they stay open | `virtualGround` |
| `--ground-z Z` | metres | the lowest point of the meshes | the virtual ground's height | `groundZ` (NAN: the lowest point) |
| `--reach D[,D...]` \| `en17037` | metres, > 0, distinct | none | `reach_<D>` columns: the share of the field that sees at least D metres (at most 8); `en17037` = `6,20,50` | `reach` |
| `--on` | `faces` \| `vertices` | `faces` | element = polygon (eye at the centroid, face normal) or point (area-weighted vertex normal) | `onVertices` |
| `--sides` | `front` \| `back` \| `both` | `front` | the viewing side(s) of each element: `back` looks along the reversed normal, `both` gives two columns per value (`_front`, `_back`) | `sides` |
| `--cull-backfaces` | flag | off | occluder triangles block only from their front (winding) side; default both sides | `cullBackfaces` |
| `--no-self-occlude` | flag | off | receivers block no rays (only `--context` meshes do); none of them can then be a target | `selfOcclude = false` |
| `--no-occlude NAME[;NAME...]` | receiver mesh names, repeatable | none | these receivers are analysed but invisible to rays, e.g. a grid of measuring points that must not block | `noOcclude` |
| `--optics SPEC[;SPEC...]` | `NAME[/SLOT]=RHO[,TAU[,glass\|porous]]`, repeatable | none | daylight's rows ([daylight.md](daylight.md)); views uses only which slots are glass with TAU > 0, and only with `--transmission on`; names and ranges checked as in daylight (exit 1) | `optics` |
| `--transmission` | `off` \| `on` | `off` | `on`: rays pass glass slots and the `glazing` column is added ([Glass and interiors](#glass-and-interiors)) | `transmission` |
| `--north DEG` | degrees, counter-clockwise from +Y | `0` | model north (90 = west): 360 mode's azimuth 0, and the east a face looking straight up or down takes as right | `north` |
| `--offset M` | metres | `0` | the eye moved along the normal (e.g. 1.6 above a floor grid: a standing eye); in 360 mode a non-zero offset (or a receiver that blocks no rays) also traces the directions behind the element | `offset` |
| `--out FILE[.npy]` | path | stdout (no manifest) | values: text (one line per element, `%.10g`) or NumPy float64 `[N x C]`; manifest at `FILE.json` | `Result` |
| `--dump-rays F.npy` | path | off | every ray's class (`F.npy`, uint8) and distance (`F.npy.depth.npy`, float32), see Output | `dumpPath` |
| `--bench [NSENS NTRIS]` | counts | `1000000 10000000` | street-grid city benchmark ([performance.md](performance.md#views)), prints timings, no output file | |
| `-v` | flag | off | sizes and timings on stderr (`info: ...`) | `verbose` |
| `-h` `--help` | | | usage | |

Environment: `CUDA_VISIBLE_DEVICES` selects the GPU. Exit codes: 0 ok; 1 usage error (message and usage on stderr): an
unknown flag, a bad value, options out of range (also `--distance group` alone, `none` with another STAT, more than
2^20 rays per element, the isovist on fewer than 3 columns or, not flat, 2 rows), and after loading a target or `--no-occlude` name the meshes do not resolve; 2 runtime error:
a file, the scene, CUDA / OptiX, memory, writing the output, a `--dump-rays` above 4 GB (refused before tracing).

## Output

- Rows: one per element of every receiver mesh, meshes in input order, elements in mesh order, exactly as daylight's;
  the manifest's `meshes` ranges map them back.
- Columns `<name>_<side>` (sides inner: `sky_front`, `sky_back`, ...; `--sides back` gives `_back` only), in this order:
  the shares `sky`, `ground`, `obstruction` and one per target group (first appearance in `--target`), which sum to 1;
  after `obstruction` comes `obstruction_total`, the total view obstruction: everything that is not sky (the ground,
  every obstruction and every target group, 1 - `sky`; it repeats those shares, so it is not another part of that sum);
  with `--transmission on` then `glazing` (the share seen through glass, not part of that sum either); then `volume` or
  `area` (the isovist, m³ / m²); then `distance`, `distance_min`, per group `<g>_distance` and `<g>_distance_min`; then
  `reach_<D>` (D as `%g`: `reach_6`, `reach_2.5`). Default columns with one group: about the normal `sky_front
  ground_front obstruction_front obstruction_total_front water_front distance_front distance_min_front`, all around
  `sky_front ground_front obstruction_front obstruction_total_front water_front volume_front` (`area_front` in a flat
  field).
- Manifest `FILE.json`: `columns`; `view` with the field (`fov` [H, LO, HI], `gaze: "level"` (only with `--gaze
  level` in view mode), `cell` as given, `grid` [rows, columns]),
  `jitter`, `north`, `offset`, `far`, `near` (the 0.1 m floor), `virtual_ground`, `ground_z` (the plane's height in
  input metres, also with the plane off), `transmission: true` (only when on), `reach`, `classes` (the dump's class names in code order) and `image` (the
  dump's ray order); `targets`; `meshes` as daylight's. No `duration_h` or `dirs`. An example is in the examples below.
- `--dump-rays F.npy` (validation, images): `F.npy` uint8 `[S x K]`, the class of every ray: 0 sky, 1 ground,
  2 obstruction, 3 + g the target group `classes[3 + g]`, + 128 when the ray passed glass, 254 own surface;
  `F.npy.depth.npy` float32 `[S x K]`, its
  distance in metres, true (not clamped): the hit's, the virtual plane's (0.1 for an eye at or below the plane), +inf
  for sky, own surface and, with `--virtual-ground off`, ground. S = one row per element and side (`--sides both`:
  element e's front at row 2e, its back at 2e + 1); K = rows x columns, ray `row x columns + column`, so a row reshapes
  to the manifest's `grid`: rows from HI down, columns from the field's left edge (360 mode: from north) clockwise.
  Refused above 4 GB (5 bytes a ray); a run that fails after starting them removes both files.
- Library: `Result::values` row-major `[N x C]` (or `Result::f32`, float32 column-major, with `f32Columns`),
  `Result::columns`, `Result::ranges`; `nSky`, `nSun` and `durationHours` stay 0.

## Examples

The outputs are pasted as printed. [examples/cli/run.ps1](../examples/cli/run.ps1)
runs views on `tests/box.obj` with a target, reach and a standing eye, beside daylight ([examples.md](examples.md#cli)).

**`tests/box.obj`, defaults** (four 2 m ground quads at z 0 facing up, and a 1 m box floating 0.5 m above the fourth;
both meshes are receivers and block). Without `--out` the values go to stdout and no manifest is written; the columns
are `sky_front ground_front obstruction_front obstruction_total_front distance_front distance_min_front`.

```
views tests/box.obj
0.9801668379 0 0.01983316212 0.01983316212 889.2737737 2.346113443
0.9276678942 0 0.07233210584 0.07233210584 633.163981 1.597963929
0.9625543318 0 0.03744566824 0.03744566824 791.1222083 1.740094185
0.4152424522 0 0.5847575478 0.5847575478 13.13519556 0.5001074076
0 0.007954232245 0.9920457678 1 0.7367731993 0.500017643
0 0.008705849663 0.9912941503 1 0.7372735157 0.5002090931
1 0 0 0 1000 1000
1 0 0 0 1000 1000
0.4490987851 0.2114750423 0.3394261726 0.5509012149 35.99476106 0.8871018887
0.4490987851 0.2521230289 0.298778186 0.5509012149 43.51158906 1.24194777
0.4490987851 0.4704659591 0.08043525575 0.5509012149 36.1492153 0.8870893717
0.4490987851 0.5154678305 0.03543338444 0.5509012149 43.46879168 1.242034435
0.4490987851 0.4689028305 0.08199838441 0.5509012149 36.20612728 0.8870113492
0.4490987851 0.5145975929 0.03630362203 0.5509012149 43.41040945 1.241719484
0.4490987851 0.1906883796 0.3602128353 0.5509012149 36.07908135 0.8868554235
0.4490987851 0.2724980931 0.2784031218 0.5509012149 43.40601672 1.241673112
```

Lines 1-4 are the ground quads: looking up, their field stays above the horizon (no ground), the box is their
obstruction, and the fourth, under it, sees it 0.50 m away. Lines 5-6, the box's bottom triangles, look down at that
quad (an obstruction, as a blocking receiver) and, past the quads' 4 x 4 m, at the virtual ground. Lines 7-8, its top,
see open sky: `distance` = `--far`. Lines 9-16 are its sides: nothing rises above the box, so sky is exactly
sin 50 / (sin 50 + sin 70) = 0.4490987851 of the field, and the rest, below the horizon, splits between the ground
quads (obstruction) and the virtual ground beyond them.

**A facade, a lake and a block.** `facade.obj` holds one mesh `facade`, a 10 m wide wall at y = 0 facing north (+Y) of
two 3 m storeys (two quads, z 0-3 and 3-6); `site.obj` holds `lake`, a 50 x 20 m quad on the ground (z 0) 15 to 35 m
north of the wall, and `block`, a 20 x 10 x 15 m box 40 to 50 m north.

```
views facade.obj --context site.obj --target lake=water --distance mean,min,group --reach en17037 --out facade.txt
```

`facade.txt`:

```
0.4061872616 0.5202675996 0.04802612514 0.5938127384 0.02551901372 43.35115397 1.596381426 24.20460493 15.5228529 0.5955391959 0.4947505679 0.4194863059
0.4163650774 0.4696878629 0.04770744403 0.5836349226 0.06623961569 81.22328275 4.789838314 24.50706751 15.70222282 0.8876812754 0.5819547265 0.4585910701
```

`facade.txt.json`:

```json
{
  "columns": ["sky_front", "ground_front", "obstruction_front", "obstruction_total_front", "water_front", "distance_front", "distance_min_front", "water_distance_front", "water_distance_min_front", "reach_6_front", "reach_20_front", "reach_50_front"],
  "view": {"fov": [120, -70, 50], "cell": 2, "grid": [60, 60], "jitter": true, "north": 0, "offset": 0, "far": 1000, "near": 0.1, "virtual_ground": true, "ground_z": 0, "reach": [6, 20, 50], "classes": ["sky", "ground", "obstruction", "water"],
    "image": "rows from HI down, columns clockwise from the field's left edge (360: from north)"},
  "targets": [{"mesh": "lake", "group": "water"}],
  "meshes": [
    {"name": "facade", "first": 0, "count": 2}
  ]
}
```

The block takes some sky (0.406 and 0.416 instead of 0.449). The water fills 2.6 % of the ground floor's field and
6.6 % of the upper floor's, 24 m away on average and 15.5 m at the nearest (the lake starts 15 m out). `distance_min`
is the virtual ground below the field's lower edge, about 1.5 / sin 70 = 1.596 m (upstairs 4.5 / sin 70 = 4.79 m), and
the log means, 43 and 81 m, weigh that near ground against the open sky. 60 % / 89 % of the field sees at least 6 m,
49 % / 58 % at least 20 m, 42 % / 46 % at least 50 m. Two variants (default columns):

```
views facade.obj --context site.obj --target lake=water --virtual-ground off
0.4061872616 0.5202675996 0.04802612514 0.5938127384 0.02551901372 780.3725661 15.5228529
0.4163650774 0.4696878629 0.04770744403 0.5836349226 0.06623961569 671.6198663 15.70222282
views facade.obj --context site.obj --target lake=water --ground-z 0.5
0.4061872616 0.5474912035 0.04632153492 0.5938127384 0 34.7388679 1.064254284
0.4163650774 0.5383020324 0.04533289017 0.5836349226 0 76.25689397 4.257634163
```

With the plane off the classes stay (downward misses are still ground, never sky) but the open ground counts as
1000 m: the log means rise to 780 and 672 m and the nearest thing in view is the lake. Any hit below the horizon (the
water too, or a lawn) is then shorter than open ground, so the upper floor, which sees more of the lake, ranks below
the ground floor: keep the plane on to rank views. A plane raised to 0.5 m hides the lake (water 0) and the block's
lowest half metre, and the ground comes 0.5 m closer (1.0 / sin 70 = 1.064 m).

**A standing eye all around.** `plaza.obj` holds `plaza`, two 2 m quads at z 0 facing up, 4 to 6 m north of the
facade. The facade is now context; `--no-occlude plaza` keeps the plaza out of the rays, so the virtual ground (at its
height) takes its place below the eye. That holds only for a flat grid at the model's lowest point (or one lying on a
context surface): a grid that is itself the terrain or a raised deck must block (class it with `--target
terrain=terrain`), else the eye sees through it down to the plane and its distances come out far too long.

```
views plaza.obj --context facade.obj --context site.obj --no-occlude plaza --fov 360 --offset 1.6 --target lake=water --reach en17037 --distance mean,min --dump-rays plaza_rays.npy --out plaza.txt
```

`plaza.txt` (columns as above without the group distances; all around the default column would be the isovist's
`volume`, so `--distance mean,min` asks for the distances):

```
0.342408523 0.4925232969 0.1486614502 0.657591477 0.01640672997 29.64483201 1.703250408 0.5289157871 0.3956413185 0.354911898
0.342056726 0.4926377925 0.1491264989 0.657943274 0.01617898261 29.58977927 1.702938914 0.5287227802 0.3952893916 0.354560101
```

All around, the eye sees 34 % sky, 49 % ground, 15 % obstruction (the facade 5 m to the south, the block) and 1.6 %
water; the nearest distance is the steepest ray down, 1.6 / sin 70 = 1.70 m. The dump (`grid` [60, 180] in the
manifest) read with NumPy:

```python
import json, numpy as np
m = json.load(open('plaza.txt.json'))
cls = np.load('plaza_rays.npy')                  # uint8 [sensors x rays]
depth = np.load('plaza_rays.npy.depth.npy')      # float32 metres; inf: open, or own surface
img = cls[0].reshape(m['view']['grid'])          # [rows, columns]: from HI down, clockwise from north
print(img.shape, np.bincount(cls[0]))           # rays per class: sky, ground, obstruction, water
print(depth[0][cls[0] == 3].min())               # the nearest water the first quad sees, metres
```

```
(60, 180) [3496 5781 1378  145]
10.280711
```

The same quads at `--offset 0`, blocking again (no `--no-occlude`, no target, no reach): everything below their plane
is their own surface, so ground is
0, obstruction holds the own surface (0.551 of the field) plus the facade and the block, and the distances leave the
own surface out (the nearest thing is the facade, 5 m from the first quad's centre):

```
views plaza.obj --context facade.obj --context site.obj --fov 360 --distance mean,min
0.3238738941 0 0.6761261059 0.6761261059 262.1961371 5.001380444
0.3227416947 0 0.6772583053 0.6772583053 259.2171342 5.001342773
```

## Limits

- Glass is passed or not (`--transmission on`): no refraction, no reflections in it, and a porous slot blocks like an
  opaque one. At most 16 panes per ray.
- Targets are whole meshes: face-slot targets (`NAME/SLOT`) are not supported yet; split such a mesh. At most 16
  target groups and 8 reach distances.
- Coplanar triangles of two meshes z-fight (a target lying on a terrain mesh): lift targets at least 1 cm above the
  terrain or cut them out of it.
- Log means use a float logarithm: about 1e-7 relative (shares, minima and reach are exact).
- The isovist is the polygon or mesh through the rays' end points: it cuts corners, bridges depth jumps and leaves out
  the half cell at the field's edges and at the poles ([Isovist](#isovist)): a little under the true area or volume,
  less at a finer `--cell`. Only the area and the volume: no perimeter, compactness or other isovist measures, and
  no isovist per target group.
- EN 17037: only the shares at its view distance levels (`--reach en17037`, shares of the whole field: see
  Distances); no level is assigned, and the standard's other view criteria (horizontal sight angle, the layers seen)
  are not computed.
- One run at a time per process, shared with daylight (one GPU).
