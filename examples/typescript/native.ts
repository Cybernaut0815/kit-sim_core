// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
//
// daylight and views in-process from TypeScript (Node): daylight_c.dll through koffi (guide: docs/javascript.md)
//
// The steps:
//   1. the meshes: tests/box.obj from its file, a small facade from arrays
//   2. daylight: a clear-sky year in Berlin, with a timeout and progress
//   3. reading a result: a column, the rows of one mesh (rows below)
//   4. views: 120 deg about each face's normal, the box as a target group; this run reuses the GPU scene of step 2
//   5. isovists: the volume and the area a standing eye sees in the room of tests/room.obj
//   6. errors
//
// Needs the core's build (daylight_c.dll), Node >= 22.18 (it runs .ts directly) and `npm install` in bindings/ts.
// Run from the repo root:   node examples/typescript/native.ts
// examples/javascript/native.mjs is the same program in plain JavaScript.
import { fileURLToPath } from 'node:url';
import { DaylightError, SimulationMesh, run, type NativeResult } from '../../bindings/ts/src/native.ts';

const tests = (file: string) => fileURLToPath(new URL(`../../tests/${file}`, import.meta.url));

// ---- Step 1: the meshes -------------------------------------------------------------------------------------------
// A SimulationMesh holds meshes the core keeps (copied once). Runs over the same handles in the same roles and order
// reuse the GPU scene. Both here are sensors: their faces are analysed (a result row each) and block rays.

// tests/box.obj: two OBJ groups, two meshes: "ground" (four 2 m quads at z 0) and "box" (a 1 m box over the fourth)
const site = SimulationMesh.load(tests('box.obj'));

// A 4 x 3 m wall at y = 3 facing south (-y), two 1.5 m high panels, from arrays:
const facade = SimulationMesh.create('facade',
  [-2, 3, 0, 2, 3, 0, 2, 3, 1.5, -2, 3, 1.5, 2, 3, 3, -2, 3, 3], // the points: x y z each, metres, Z up
  [4, 4], // the vertices of each face: two quads
  [0, 1, 2, 3, 3, 2, 4, 5]); // each face's points, 0-based, counter-clockwise seen from the front (the analysed side)

const sensors = [site, facade];
console.log('meshes:', sensors.flatMap((m) => m.names).join(', '));

// A result has a Float64Array per column (data[c] for columns[c]), a value per row, and the manifest. The rows are
// the faces of every sensor mesh in order; the manifest's ranges say which rows are whose: { name, first, count }.
type Ranges = { meshes: { name: string; first: number; count: number }[] };
function rows(r: NativeResult, mesh: string, column: string): Float64Array {
  const { first, count } = (r.manifest as Ranges).meshes.find((m) => m.name === mesh)!;
  return r.data[r.columns.indexOf(column)].subarray(first, first + count);
}
const mean = (v: Float64Array) => v.reduce((a, b) => a + b, 0) / v.length;

// ---- Step 2: daylight, a clear-sky year in Berlin -----------------------------------------------------------------
// The flags are the CLI's (docs/daylight.md), one string each. run() works on a worker thread: the event loop stays
// free. The signal cancels it (here a 10 s timeout, in an app a Cancel button), onProgress reports the traced share.
const clearSkyYear = [
  '--clear-sky', // no weather file: a cloudless year for the place
  '--lat', '52.52', '--lon', '13.41', // Berlin
  '--quantity', 'radiation,sunhours', // the columns: kWh/m2 and sun hours
];
let t = performance.now();
const sun = await run('daylight', clearSkyYear, { sensors },
  { signal: AbortSignal.timeout(10_000), onProgress: (p) => console.log(`  traced ${Math.round(100 * p)} %`) });
console.log(`daylight: ${(performance.now() - t).toFixed(0)} ms, scene reused ${sun.stats.sceneReused}`);

// ---- Step 3: reading the result -----------------------------------------------------------------------------------
for (const { name } of (sun.manifest as Ranges).meshes) {
  const kwh = mean(rows(sun, name, 'radiation_front')), hours = mean(rows(sun, name, 'sunhours_front'));
  console.log(`  ${name.padEnd(6)} ${kwh.toFixed(0)} kWh/m2, ${hours.toFixed(0)} h (mean per face)`);
}

// ---- Step 4: views about each face's normal -----------------------------------------------------------------------
// From every face a ray per 2 deg cell of a field 120 deg wide about its normal: the shares that end on the sky, the
// ground, an obstruction or a target group, the log-mean distance (an open ray counts as 1000 m) and the nearest
// thing in view. The same handles in the same roles as step 2: the resident scene serves it.
const viewFlags = ['--target', 'box=cube']; // rays ending on the mesh "box" count for the group "cube"
t = performance.now();
const view = await run('views', viewFlags, { sensors }, { signal: AbortSignal.timeout(10_000) });
console.log(`views: ${(performance.now() - t).toFixed(0)} ms, scene reused ${view.stats.sceneReused}`);
console.log(`  columns ${view.columns.join(' ')}`);
const sky = rows(view, 'facade', 'sky_front'), cube = rows(view, 'facade', 'cube_front');
const distance = rows(view, 'facade', 'distance_front'), nearest = rows(view, 'facade', 'distance_min_front');
sky.forEach((s, i) => console.log(`  facade row ${i}: sky ${s.toFixed(3)}, cube ${cube[i].toFixed(3)}, ` +
  `distance ${distance[i].toFixed(1)} m, nearest ${nearest[i].toFixed(2)} m`));

// ---- Step 5: isovists in a room -----------------------------------------------------------------------------------
// tests/room.obj: a 6.3 x 5.1 x 2.9 m room with a window and a tree outside; tests/room_grid.obj: 12 measuring points
// at desk height. The isovist is what an eye sees around it: all around in 3D the volume in view (column
// volume_front, m3), with horizontal rays only the area in view (area_front, m2).
const grid = SimulationMesh.load(tests('room_grid.obj')); // sensor: the measuring points
const room = SimulationMesh.load(tests('room.obj')); // context: room, window, tree
const eye = [
  '--offset', '0.4', // the eye 0.4 m above each grid face
  '--no-occlude', 'grid', // the grid is measured, not seen: the rays pass it
];
// The isovist is the default column all around; --distance mean,min gives the distances instead, --far M caps both.
const vol = await run('views', ['--fov', '360,-90,90', ...eye], { sensors: [grid], context: [room] }); // the whole sphere
const area = await run('views', ['--fov', '360,0,0', ...eye], { sensors: [grid], context: [room] }); // horizontal rays only
console.log(`isovists in the room: volume ${mean(rows(vol, 'grid', 'volume_front')).toFixed(1)} m3 all around, ` +
  `area ${mean(rows(area, 'grid', 'area_front')).toFixed(1)} m2 at eye height`);
grid.free();
room.free();

// ---- Step 6: errors -----------------------------------------------------------------------------------------------
// A failed run rejects with a DaylightError: status 1 usage (flags, names the meshes do not resolve) or 2 runtime
// (files, the GPU, cancelled). Here a target name no mesh has.
try {
  await run('views', ['--target', 'tree=green'], { sensors });
} catch (e) {
  if (!(e instanceof DaylightError)) throw e;
  console.log(`error ${e.status}: ${e.message}`);
}
site.free(); // now; else when garbage collected
facade.free();
