// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// daylight.exe and views.exe from Node through the process runner: run(exe, ...) writes one .dmesh per mesh into a temp
// folder, starts the exe and reads its .npy back. The core names the meshes dmeshStems(names): --target uses those.
// Run from the repo root after the build: node examples/typescript/cli.ts (Node >= 22.18 strips the types)
import { fileURLToPath } from 'node:url';
import { dmeshStems, run, type ObjMesh } from '../../bindings/ts/src/index.ts';

const exe = (name: string) => fileURLToPath(new URL(`../../build/RelWithDebInfo/${name}`, import.meta.url));
// A quad as two triangles (an ObjMesh holds triangles): corners counter-clockwise seen from its front, metres, Z up.
const quad = (name: string, ...corners: number[][]): ObjMesh =>
  ({ name, positions: corners.flat(), indices: [0, 1, 2, 0, 2, 3] });

const sensors = [ // two storeys of a 10 m wide facade at y = 0, facing south (-y)
  quad('facade/floor 0', [0, 0, 0], [10, 0, 0], [10, 0, 3], [0, 0, 3]),
  quad('facade/floor 1', [0, 0, 3], [10, 0, 3], [10, 0, 6], [0, 0, 6]),
];
const context = [
  quad('site/lake', [-20, -35, 0], [30, -35, 0], [30, -15, 0], [-20, -15, 0]), // 50 x 20 m on the ground, 15 m out
  quad('site/block', [15, -40, 0], [-5, -40, 0], [-5, -40, 15], [15, -40, 15]), // a block's 20 x 15 m face, 40 m out
];

// One column's mean per sensor mesh (a storey: two triangle rows), from the manifest's ranges.
type Result = { data: Float64Array[]; columns: string[]; manifest: unknown };
type Ranges = { meshes: { name: string; first: number; count: number }[] };
const means = (r: Result, column: string) => (r.manifest as Ranges).meshes.map(({ first, count }) =>
  r.data[r.columns.indexOf(column)].subarray(first, first + count).reduce((a, b) => a + b, 0) / count);
const names = (r: Result) => (r.manifest as Ranges).meshes.map((m) => m.name);

// daylight.exe: a clear-sky year in Berlin. Every call starts the exe, and with it the GPU, again.
let t = performance.now();
const sun = await run(exe('daylight.exe'), { sensors, context,
  args: ['--clear-sky', '--lat', '52.52', '--lon', '13.41', '--quantity', 'radiation,sunhours'] });
console.log(`daylight.exe: ${(performance.now() - t).toFixed(0)} ms, columns ${sun.columns.join(' ')}`);
const kwh = means(sun, 'radiation_front'), hours = means(sun, 'sunhours_front');
names(sun).forEach((n, k) => console.log(`  ${n}: ${kwh[k].toFixed(0)} kWh/m2, ${hours[k].toFixed(0)} h`));

// views.exe: the core's mesh names are the .dmesh file stems run() writes, sensors then context ('site/lake' ->
// 'site_lake'), so --target names the lake by its stem.
const stems = dmeshStems([...sensors, ...context].map((m) => m.name));
console.log(`core names: ${stems.join(', ')}`);
t = performance.now();
const view = await run(exe('views.exe'), { sensors, context,
  args: ['--target', `${stems[sensors.length]}=water`, '--reach', 'en17037'] });
console.log(`views.exe: ${(performance.now() - t).toFixed(0)} ms, columns ${view.columns.join(' ')}`);
const sky = means(view, 'sky_front'), water = means(view, 'water_front'), distance = means(view, 'distance_front');
const reach = means(view, 'reach_50_front');
names(view).forEach((n, k) => console.log(`  ${n}: sky ${sky[k].toFixed(3)}, water ${water[k].toFixed(3)}, ` +
  `distance ${distance[k].toFixed(1)} m, sees 50 m ${reach[k].toFixed(3)}`));
