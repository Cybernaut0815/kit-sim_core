// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// examples/typescript/native.ts in plain JavaScript: the compiled binding in bindings/ts/lib, the same calls, no types.
// daylight and views in-process (Node, daylight_c.dll through koffi) on tests/box.obj and a facade from arrays.
// Run from the repo root after the build: node examples/javascript/native.mjs (Node >= 20)
import { fileURLToPath } from 'node:url';
import { DaylightError, SimulationMesh, run } from '../../bindings/ts/lib/native.js';

// Meshes are copied to the C side once; runs over the same handles in the same roles and order reuse the GPU scene.
const site = SimulationMesh.load(fileURLToPath(new URL('../../tests/box.obj', import.meta.url))); // OBJ groups: ground, box
const facade = SimulationMesh.create('facade', // a 4 x 3 m wall at y = 3 facing south (-y), two 1.5 m rows
  [-2, 3, 0, 2, 3, 0, 2, 3, 1.5, -2, 3, 1.5, 2, 3, 3, -2, 3, 3], // xyz per point: metres, Z up
  [4, 4], // vertices per face
  [0, 1, 2, 3, 3, 2, 4, 5]); // 0-based, counter-clockwise seen from the front
const sensors = [site, facade];
console.log('meshes:', sensors.flatMap((m) => m.names).join(', '));

// The rows of one mesh in one column: the manifest's ranges, one { name, first, count } per mesh in sensor order.
function rows(r, mesh, column) {
  const { first, count } = r.manifest.meshes.find((m) => m.name === mesh);
  return r.data[r.columns.indexOf(column)].subarray(first, first + count);
}
const mean = (v) => v.reduce((a, b) => a + b, 0) / v.length;

// daylight: a clear-sky year in Berlin (no weather file), annual radiation and sun hours per face.
const args = ['--clear-sky', '--lat', '52.52', '--lon', '13.41', '--quantity', 'radiation,sunhours'];
let t = performance.now();
const sun = await run('daylight', args, { sensors },
  { signal: AbortSignal.timeout(10_000), onProgress: (p) => console.log(`  traced ${Math.round(100 * p)} %`) });
console.log(`daylight: ${(performance.now() - t).toFixed(0)} ms, scene reused ${sun.stats.sceneReused}`);
for (const { name } of sun.manifest.meshes) {
  const kwh = mean(rows(sun, name, 'radiation_front')), hours = mean(rows(sun, name, 'sunhours_front'));
  console.log(`  ${name.padEnd(6)} ${kwh.toFixed(0)} kWh/m2, ${hours.toFixed(0)} h (mean per face)`);
}

// views: 120 deg about each face's normal; rays ending on the mesh "box" count for the group "cube".
t = performance.now();
const view = await run('views', ['--target', 'box=cube'], { sensors }, { signal: AbortSignal.timeout(10_000) });
console.log(`views: ${(performance.now() - t).toFixed(0)} ms, scene reused ${view.stats.sceneReused}`);
console.log(`  columns ${view.columns.join(' ')}`);
const sky = rows(view, 'facade', 'sky_front'), cube = rows(view, 'facade', 'cube_front');
const distance = rows(view, 'facade', 'distance_front'), nearest = rows(view, 'facade', 'distance_min_front');
sky.forEach((s, i) => console.log(`  facade row ${i}: sky ${s.toFixed(3)}, cube ${cube[i].toFixed(3)}, ` +
  `distance ${distance[i].toFixed(1)} m, nearest ${nearest[i].toFixed(2)} m`));

// isovists in tests/room.obj (a 6.3 x 5.1 x 2.9 m room) from its measure grid, the eye 0.4 m above it: the volume all
// around (--fov 360,-90,90: m3) and the area of horizontal rays only (--fov 360,0,0: m2). The isovist is 360 mode's
// default column; --distance mean,min gives the distances instead, --far M caps both.
const tests = (file) => fileURLToPath(new URL(`../../tests/${file}`, import.meta.url));
const grid = SimulationMesh.load(tests('room_grid.obj')), room = SimulationMesh.load(tests('room.obj')); // room: room, window, tree
const eye = ['--offset', '0.4', '--no-occlude', 'grid']; // the grid is measured, not seen
const vol = await run('views', ['--fov', '360,-90,90', ...eye], { sensors: [grid], context: [room] });
const area = await run('views', ['--fov', '360,0,0', ...eye], { sensors: [grid], context: [room] });
console.log(`isovists in the room: volume ${mean(rows(vol, 'grid', 'volume_front')).toFixed(1)} m3 all around, ` +
  `area ${mean(rows(area, 'grid', 'area_front')).toFixed(1)} m2 at eye height`);
grid.free();
room.free();

// Errors: DaylightError, status 1 usage (flags, names the meshes do not resolve) or 2 runtime (files, GPU, cancelled).
try {
  await run('views', ['--target', 'tree=green'], { sensors });
} catch (e) {
  if (!(e instanceof DaylightError)) throw e;
  console.log(`error ${e.status}: ${e.message}`);
}
site.free(); // now; else when garbage collected
facade.free();
