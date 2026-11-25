// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
//
// daylight.exe and views.exe from TypeScript (Node) as processes: the process runner run(exe, ...) (guide:
// docs/javascript.md). run() writes one .dmesh file per mesh into a temp folder, starts the exe on them and reads its
// .npy values and manifest back. No DLL in the process; each call starts the exe (and the GPU) anew.
//
// The steps:
//   1. the meshes: two storeys of a facade, a lake and a block, as ObjMesh triangles
//   2. daylight.exe: a clear-sky year in Berlin
//   3. reading a result: the mean of a column per mesh (means below)
//   4. views.exe: the lake as a target group, named by the file stem the runner gives it
//
// Needs the core's build (the exes), Node >= 22.18 (it runs .ts directly) and `npm install` in bindings/ts.
// Run from the repo root:   node examples/typescript/cli.ts
import { fileURLToPath } from 'node:url';
import { dmeshStems, run, type DaylightOptions, type ObjMesh, type ViewsOptions } from '../../bindings/ts/src/index.ts';

const exe = (name: string) => fileURLToPath(new URL(`../../build/RelWithDebInfo/${name}`, import.meta.url));

// ---- Step 1: the meshes -------------------------------------------------------------------------------------------
// An ObjMesh is a named triangle mesh (positions xyz, indices). A quad is two triangles, its corners counter-clockwise
// seen from its front (the analysed side); metres, Z up.
const quad = (name: string, ...corners: number[][]): ObjMesh =>
  ({ name, positions: corners.flat(), indices: [0, 1, 2, 0, 2, 3] });

// sensors: analysed (a row per triangle) and blocking; two storeys of a 10 m wide facade at y = 0, facing south (-y)
const sensors = [
  quad('facade/floor 0', [0, 0, 0], [10, 0, 0], [10, 0, 3], [0, 0, 3]),
  quad('facade/floor 1', [0, 0, 3], [10, 0, 3], [10, 0, 6], [0, 0, 6]),
];
// context: blocks rays only
const context = [
  quad('site/lake', [-20, -35, 0], [30, -35, 0], [30, -15, 0], [-20, -15, 0]), // 50 x 20 m on the ground, 15 m out
  quad('site/block', [15, -40, 0], [-5, -40, 0], [-5, -40, 15], [15, -40, 15]), // a block's 20 x 15 m face, 40 m out
];

// A result has a Float64Array per column (data[c] for columns[c]), a value per row (a triangle of a sensor mesh), and
// the manifest, whose ranges say which rows are whose: { name, first, count }. means(): one column's mean per mesh.
type Result = { data: Float64Array[]; columns: string[]; manifest: unknown };
type Ranges = { meshes: { name: string; first: number; count: number }[] };
const means = (r: Result, column: string) => (r.manifest as Ranges).meshes.map(({ first, count }) =>
  r.data[r.columns.indexOf(column)].subarray(first, first + count).reduce((a, b) => a + b, 0) / count);
const names = (r: Result) => (r.manifest as Ranges).meshes.map((m) => m.name);

// ---- Step 2: daylight.exe, a clear-sky year in Berlin -------------------------------------------------------------
// The CLI's flags as typed fields (docs/api.md#typed-options); args takes the strings too (['--clear-sky', ...]).
const clearSkyYear: DaylightOptions = {
  clearSky: true, // no weather file: a cloudless year for the place
  lat: 52.52, lon: 13.41, // Berlin
  quantity: ['radiation', 'sunhours'], // the columns: kWh/m2 and sun hours
};
let t = performance.now();
const sun = await run(exe('daylight.exe'), { sensors, context, args: clearSkyYear });
console.log(`daylight.exe: ${(performance.now() - t).toFixed(0)} ms, columns ${sun.columns.join(' ')}`);

// ---- Step 3: reading the result -----------------------------------------------------------------------------------
const kwh = means(sun, 'radiation_front'), hours = means(sun, 'sunhours_front');
names(sun).forEach((n, k) => console.log(`  ${n}: ${kwh[k].toFixed(0)} kWh/m2, ${hours[k].toFixed(0)} h`));

// ---- Step 4: views.exe, the lake as a target group ----------------------------------------------------------------
// The core names each mesh by its .dmesh file stem, which run() makes from the names, sensors then context
// ('site/lake' -> 'site_lake'). dmeshStems gives the same stems, so --target can name the lake.
const stems = dmeshStems([...sensors, ...context].map((m) => m.name));
console.log(`core names: ${stems.join(', ')}`);
const viewFlags: ViewsOptions = {
  target: [{ name: stems[sensors.length], group: 'water' }], // rays ending on the lake (the first context mesh) count for "water"
  reach: 'en17037', // reach_6, reach_20, reach_50: the shares that see at least 6, 20, 50 m
};
t = performance.now();
const view = await run(exe('views.exe'), { sensors, context, args: viewFlags });
console.log(`views.exe: ${(performance.now() - t).toFixed(0)} ms, columns ${view.columns.join(' ')}`);
// distance: the log mean of the rays' distances, an open ray counting as 1000 m
const sky = means(view, 'sky_front'), water = means(view, 'water_front'), distance = means(view, 'distance_front');
const reach = means(view, 'reach_50_front');
names(view).forEach((n, k) => console.log(`  ${n}: sky ${sky[k].toFixed(3)}, water ${water[k].toFixed(3)}, ` +
  `distance ${distance[k].toFixed(1)} m, sees 50 m ${reach[k].toFixed(3)}`));
