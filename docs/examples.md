# Examples

A program per way in to the core ([api.md](api.md)), each running both tools, daylight ([daylight.md](daylight.md))
and views ([views.md](views.md)), on a small scene in a second or two and printing a short result: means per mesh from
the manifest's ranges and, where the way in has them, the resident scene, progress, cancel and errors. No weather file
is needed: daylight runs a clear-sky year in Berlin (`--clear-sky --lat 52.52 --lon 13.41`) or sun hours. The scenes
build on [tests/box.obj](../tests/box.obj) (OBJ groups `ground`, four 2 m quads at z 0, and `box`, a 1 m box floating
0.5 m above the fourth) and add a facade, a block or a lake made in code. The guides go through them line by line.

| file | way in | daylight | views | guide |
|---|---|---|---|---|
| [cli/run.ps1](../examples/cli/run.ps1) | `daylight.exe`, `views.exe` (PowerShell) | box.obj: a clear-sky year, sun hours of 21 June, irradiance and illuminance at noon | box.obj: 120 deg about each normal with a target and `--reach en17037`; a standing eye all around | [CLI](#cli) |
| [cpp/example.cpp](../examples/cpp/example.cpp) | C++, `daylightcore` | a 10 x 6 m facade, box.obj and a block across the street: a clear-sky year | the same meshes: target `ground=lawn`, reach, the resident scene; progress, cancel, an error | [cpp.md](cpp.md) |
| [c/example.c](../examples/c/example.c) | C, `daylight_c.dll` | box.obj and a 4 x 3 m facade: a clear-sky year, progress | target `box=cube`, the resident scene; cancel, an error | [c-abi.md](c-abi.md#examples) |
| [csharp/Program.cs](../examples/csharp/Program.cs) | C#, `Daylight.dll` | box.obj and a facade of two panels: a clear-sky year; sun hours of 21 June through `RunAsync` with a token and progress | target `box=block`, reach, the resident scene | [csharp.md](csharp.md) |
| [python/inprocess.py](../examples/python/inprocess.py) | Python, `daylight.native` | the C++ example's scene from numpy: a clear-sky year on a worker thread, progress | as the C++ example, on the resident scene | [python.md](python.md) |
| [python/process.py](../examples/python/process.py) | Python, `run_meshes` (the CLIs as processes) | a facade, a lawn and a block: `daylight.exe`, a clear-sky year, means per 1 m row | `views.exe`: target `lawn=green` by file stem, reach; USD one-liners in its docstring | [python.md](python.md#through-daylightexe) |
| [typescript/native.ts](../examples/typescript/native.ts) | TypeScript, `daylight-mesh/native` (Node) | the C example's scene: a clear-sky year, progress, a timeout | target `box=cube`, the resident scene; an error | [javascript.md](javascript.md) |
| [javascript/native.mjs](../examples/javascript/native.mjs) | JavaScript, the same from `bindings/ts/lib` | as `native.ts` | as `native.ts` | [javascript.md](javascript.md) |
| [typescript/cli.ts](../examples/typescript/cli.ts) | TypeScript, `run(exe, ...)` (the CLIs as processes) | two storeys of a facade, a lake, a block: `daylight.exe`, a clear-sky year | `views.exe`: the lake as target `water` by `dmeshStems`, reach | [javascript.md](javascript.md#through-daylightexe) |

## Running them

First the core's build, which also builds the C and C++ examples (`cmake --preset vs2022 && cmake --build --preset
release`, [README](../README.md#build)); then, for C#, the .NET 9 SDK; for Python, uv (`uv run` syncs
`bindings/python` on first use); for TypeScript and JavaScript, `npm install` in `bindings/ts` and Node >= 22.18
(`.mjs`: >= 20); for the `.ps1` scripts, PowerShell 7 (`pwsh`). Each sample below starts with the command that runs it, from the repo root unless it says `cd`; the
times printed vary with the GPU and what else runs on it.

`examples/run.ps1` runs them all and prints PASS or FAIL for each, with the tail of its output on a failure, and exits
1 if any fails; each example's output goes to `examples/out/<name>.log`. It points the in-process examples at
`build/RelWithDebInfo/daylight_c.dll` and the Python process runner and `cli/run.ps1` at the exes beside it, unless
`DAYLIGHT_DLL`, `DAYLIGHT_EXE` or `VIEWS_EXE` are set (`typescript/cli.ts` always runs the build's exes). The C and C++
examples are ctests too: `ctest --preset release -R example` runs the two.

```
pwsh examples/run.ps1
PASS  cpp                  1.0 s
PASS  c                    0.9 s
PASS  csharp               3.8 s
PASS  python-inprocess     3.2 s
PASS  python-process       2.4 s
PASS  typescript-native    1.0 s
PASS  javascript-native    0.8 s
PASS  typescript-cli       1.7 s
PASS  cli                  3.8 s
9 of 9 passed; logs in examples/out
```

## CLI

[examples/cli/run.ps1](../examples/cli/run.ps1) runs `daylight.exe` and `views.exe` on `tests/box.obj`, where both
meshes are sensors and block, then both on an interior (`tests/room.obj` with its measure grid `tests/room_grid.obj`). Each run writes its values (text, a line per face) and manifest to `examples/cli/out/`;
the script prints the command (the exes are in `build\RelWithDebInfo\`) and per mesh the mean of each column over its
faces, without the `_front` of the column names.

```
pwsh examples/cli/run.ps1
daylight tests/box.obj --clear-sky --lat 52.52 --lon 13.41 --quantity radiation,sunhours --out examples/cli/out/year.txt
  ground  4 faces  radiation 1400.59  sunhours 4140.5
  box    12 faces  radiation 951.89  sunhours 2228
daylight tests/box.obj --quantity sunhours --lat 52.52 --lon 13.41 --tz 1 --from 06-21T00:00 --to 06-22T00:00 --step 10 --out examples/cli/out/june21.txt
  ground  4 faces  sunhours 14.29
  box    12 faces  sunhours 8.33
daylight tests/box.obj --clear-sky --lat 52.52 --lon 13.41 --quantity irradiance,illuminance --time 06-21T12:00 --out examples/cli/out/noon.txt
  ground  4 faces  irradiance 665.27  illuminance 71294.07
  box    12 faces  irradiance 301.8  illuminance 32889.54
views tests/box.obj --fov 120 --target ground=lawn --reach en17037 --out examples/cli/out/view.txt
  ground  4 faces  sky 0.82  ground 0  lawn 0  obstruction 0.18  distance 581.67  reach_20 0.82
  box    12 faces  sky 0.47  ground 0.24  lawn 0.29  obstruction 0  distance 193.31  reach_20 0.49
views tests/box.obj --fov 360 --offset 1.6 --target ground=lawn --target box=cube --reach en17037 --out examples/cli/out/eye.txt
  ground  4 faces  sky 0.45  ground 0.3  lawn 0.12  cube 0.13  distance 41.2  reach_20 0.49
views tests/room_grid.obj --context tests/room.obj --fov 360 --offset 0.4 --target tree=green --no-occlude grid --optics window=0,0.64,glass --transmission on --reach en17037 --out examples/cli/out/interior.txt
  grid   12 faces  sky 0.02  ground 0.01  green 0.01  glazing 0.03  distance 2.68  reach_6 0.03
daylight tests/room_grid.obj --context tests/room.obj --clear-sky --lat 52.52 --lon 13.41 --no-occlude grid --optics window=0,0.64,glass --transmission on --quantity radiation,sunhours --out examples/cli/out/interior_light.txt
  grid   12 faces  radiation 52.9  sunhours 198.85
```

- `year`, daylight: a clear-sky year in Berlin without a weather file (cloudless: an upper bound, [modes.md](modes.md)).
  The open ground quads get about 1600 kWh/m2 and every one of the 4456 hours the sun is up, the one under the box 926
  and 3587; the box's mean includes its bottom (41 kWh/m2, no sun). Face by face: [daylight.md](daylight.md#examples).
- `june21`, daylight: sun hours need no weather, only the place and the clock of `--from` and `--to` (`--tz`): 16.7
  hours on the open quads, sunrise to sunset.
- `noon`, daylight: one instant (`--time`), noon of 21 June: irradiance in W/m2 and illuminance in lux, about 875 W/m2
  and 93,500 lux on the open quads, 40 W/m2 under the box.
- `view`, views: the default field, 120 deg wide about each face's normal, 70 deg down to 50 deg up, a ray per 2 deg
  cell. The ground quads look up: 82 % sky, 18 % the box (an obstruction: sensor meshes block). The box's faces see the
  ground quads (the target group `lawn`) in 29 % of their fields and the virtual ground beyond them in 24 %. `distance`
  is the log mean of the rays' distances (an open ray counts as 1000 m), `reach_20` the share of the field that sees at
  least 20 m. Face by face, without the target: [views.md](views.md#examples).
- `eye`, views: an eye 1.6 m above each ground quad (`--offset 1.6`), all around (`--fov 360`, still 70 deg down to
  50 deg up). Nothing rises above it, so its sky is sin 50 / (sin 50 + sin 70) = 0.449 of the field; the ground quads
  below fill 12 %, the box 13 % (most of it seen from the quad it floats over), the virtual ground beyond them 30 %.
  Only the ground's rows are printed: the box's faces get eyes 1.6 m out too, which mean nothing here.
- `interior`, views: a room with a window in its south wall and a tree outside; the measure grid at desk height
  (0.8 m) is kept out of the rays (`--no-occlude grid`), the eye 0.4 m above it, all around. The window is glass
  (`--optics window=0,0.64,glass`) and `--transmission on` lets the rays pass it: 2 % sky, 1 % ground and 1 % tree
  seen through it, `glazing` 3 % (the share seen through glass), the rest the room, 2.7 m away on log mean. Without
  `--transmission on` the window blocks and the room is closed ([views.md](views.md#glass-and-interiors)).
- `interior_light`, daylight: the same files and roles, the light that comes in through the window over a clear-sky
  year (glass passes 0.64 at normal incidence, less at grazing angles).

## C++ and C

```
build\RelWithDebInfo\example_cpp.exe tests
daylight, clear-sky year: scene reused false, 411 ms
  facade 60 faces  1395 kWh/m2  3245 sun hours
  ...
views, 120 deg about each normal: scene reused true, 1 ms, progress 100 %
  facade 60 faces  sky 0.34  lawn 0.06  distance  43 m  sees 6 / 20 / 50 m: 0.73 0.43 0.35
  ...
cancelled run: cancelled
bad target: --target tree=green: no mesh of that name
```

```
build\RelWithDebInfo\example_c.exe tests
daylight: 18 rows, scene reused 0, progress 1
  columns "radiation_front", "sunhours_front"
  ...
  facade  2 faces:      1554      3522
views: 18 rows, scene reused 1
  columns "sky_front", "ground_front", "obstruction_front", "cube_front", "distance_front", "distance_min_front"
  ...
  facade  2 faces:    0.4174     0.325    0.1814   0.07617     40.55     1.282
cancelled run: status 2, cancelled
bad target: status 1, --target tree=green: no mesh of that name
```

## C# (.NET)

```
dotnet run --project examples/csharp
daylight: 590 ms, scene reused False
  ...
  facade  2 faces  radiation 1489.1 kWh/m2
views: 1 ms, scene reused True
  ...
  facade  2 faces  sky 0.40  block 0.10  distance  28.6 m  sees 20 m 0.42
sun hours, 21 June: 4 ms, scene reused True, progress 100 %
  ...
  facade  2 faces   9.3 h
```

## Python

```
cd bindings/python && uv run python ../../examples/python/inprocess.py
daylight, clear-sky year: scene_reused False, 496 ms, progress 0% -> 100%
  facade 60 faces  1395 kWh/m2  3245 sun hours
  ...
views, 120 deg about each normal: scene_reused True, 2 ms
  facade 60 faces  sky 0.34  lawn 0.06  distance  43 m  sees 6 / 20 / 50 m: 0.73 0.43 0.35
  ...
```

```
cd bindings/python && uv run python ../../examples/python/process.py
facade row kWh/m2  sun hours   sky  green  distance  sees 20 m
z 0-1 m      1239       2999  0.30   0.06    14.9 m       0.37
...
z 5-6 m      1563       3578  0.38   0.13    70.3 m       0.50
```

## TypeScript and JavaScript

`node examples/javascript/native.mjs` prints what `native.ts` prints.

```
node examples/typescript/native.ts
meshes: ground, box, facade
  traced 0 %
  traced 100 %
daylight: 634 ms, scene reused false
  ...
  facade 1554 kWh/m2, 3522 h (mean per face)
views: 5 ms, scene reused true
  ...
  facade row 1: sky 0.449, cube 0.067, distance 59.2 m, nearest 1.77 m
error 1: --target tree=green: no mesh of that name
```

```
node examples/typescript/cli.ts
daylight.exe: 778 ms, columns radiation_front sunhours_front
  facade_floor_0: 1573 kWh/m2, 3516 h
  facade_floor_1: 1631 kWh/m2, 3605 h
core names: facade_floor_0, facade_floor_1, site_lake, site_block
views.exe: 640 ms, columns sky_front ground_front obstruction_front water_front distance_front distance_min_front reach_6_front reach_20_front reach_50_front
  facade_floor_0: sky 0.406, water 0.024, distance 42.7 m, sees 50 m 0.420
  facade_floor_1: sky 0.415, water 0.069, distance 80.4 m, sees 50 m 0.456
```
