// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// daylight-mesh/native against daylight_c.dll, the cases of tests/c_abi.c: errors, both tools on tests/box.obj equal to
// the CLI's smokes (build/smoke.npy, build/views_smoke.npy: values and manifests byte for byte), resident scenes, the
// views facade, cancel, progress, a free event loop, a dead run thread. Skipped without the DLL (DAYLIGHT_DLL or the
// default build).
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Worker } from 'node:worker_threads';
import { describe, expect, it } from 'vitest';
import { run as runExe } from '../src/index.js';
import { DaylightError, SimulationMesh, init, run, usage, type NativeResult } from '../src/native.js';

const repo = (p: string) => fileURLToPath(new URL(`../../../${p}`, import.meta.url));
const dll = process.env.DAYLIGHT_DLL || repo('build/RelWithDebInfo/daylight_c.dll');
const build = dirname(dirname(dll)), tests = repo('tests/');
const viewsExe = process.env.VIEWS_EXE && existsSync(process.env.VIEWS_EXE) ? process.env.VIEWS_EXE : '';
const sun = ['--lat', '41.98', '--lon', '-87.92', '--tz', '-6', '--quantity', 'sunhours', '--time', '06-21T12:00'];
const view360 = ['--fov', '360', '--offset', '1.6', '--sides', 'both'];
const open = ['--no-self-occlude', '--jitter', 'off'];
const quad = [-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2]; // a 2 x 2 m facade facing -y, its foot at z 0

// The result equals the CLI's NAME (.npy data after its header, row-major) and NAME.json (CRLF -> LF) byte for byte.
function sameAsCli(r: NativeResult, name: string) {
  const npy = readFileSync(join(build, name)), rows = r.data[0].length, cols = r.data.length;
  const v = new Float64Array(rows * cols);
  r.data.forEach((col, c) => col.forEach((x, i) => (v[i * cols + c] = x)));
  expect(Buffer.from(v.buffer).equals(npy.subarray(10 + npy.readUInt16LE(8)))).toBe(true);
  expect(r.manifestText).toBe(readFileSync(join(build, name + '.json'), 'utf8').replace(/\r\n/g, '\n'));
}

describe.skipIf(!existsSync(dll))('native (daylight_c.dll)', { timeout: 60_000 }, () => {
  const facade = () => SimulationMesh.create('facade', quad, [4], [0, 1, 2, 3]);
  let boxMesh: SimulationMesh | undefined;
  const box = () => (boxMesh ??= SimulationMesh.load(join(tests, 'box.obj')));
  const slow = () => run('views', [...view360, '--cell', '0.5'], { sensors: [box()] }); // ~13 ms on an RTX 2060
  const fails = (p: Promise<unknown>, status: number, msg: RegExp) =>
    expect(p).rejects.toSatisfy((e) => e instanceof DaylightError && e.status === status && msg.test(e.message));

  it('loads the DLL (ABI version checked) once; usage texts', () => {
    expect(() => init(join(build, 'nosuch.dll'))).toThrow(/cannot load .*nosuch\.dll: no such file/);
    init(dll);
    expect(() => init(join(build, 'other.dll'))).toThrow(/loaded already/);
    expect(usage('daylight')).toContain('usage: daylight');
    expect(usage('views')).toContain('usage: views');
  });

  it('mesh errors carry the core message; names; an empty mesh holds none; free twice is fine', () => {
    const f = facade(), tri = [0, 0, 0, 1, 0, 0, 0, 1, 0];
    expect(f.names).toEqual(['facade']);
    expect(() => SimulationMesh.create('bad', quad, [4], [0, 1, 2, 4])).toThrow('bad: vertex index out of range');
    expect(() => SimulationMesh.create('two', tri, [2], [0, 1])).toThrow(/fewer than 3 vertices/);
    expect(() => SimulationMesh.create('odd', [0, 0], [3], [0, 0, 0])).toThrow(/xyz triples/);
    expect(() => SimulationMesh.create('s', tri, [3], [0, 1, 2], [1, 2])).toThrow(/one value per face \(1\), not 2/);
    expect(SimulationMesh.create('empty', [], [], []).names).toEqual([]);
    expect(() => SimulationMesh.load(join(tests, 'nosuch.obj'))).toThrow(/cannot open/);
    expect(() => SimulationMesh.load('x.usd')).toThrow(/unknown extension/);
    const t = SimulationMesh.fromObj({ name: 't', positions: tri, indices: Uint32Array.of(0, 1, 2), slots: Uint16Array.of(3) });
    expect(t.names).toEqual(['t']);
    f.free();
    f.free();
  });

  it('run errors: status 1 (usage) or 2 (runtime) with the CLI message', async () => {
    const f = facade(), gone = facade();
    gone.free();
    await fails(run('nope' as 'views', [], { sensors: [f] }), 1, /unknown tool/);
    await fails(run('views', [], { sensors: [gone] }), 1, /NULL mesh handle/);
    await fails(run('views', [], { sensors: [] }), 1, /no mesh given/);
    await fails(run('daylight', ['--bogus'], { sensors: [f] }), 1, /unknown flag --bogus/);
    await fails(run('views', ['x.obj'], { sensors: [f] }), 1, /dl_mesh_load/);
    await fails(run('views', ['--context', 'c.obj'], { sensors: [f] }), 1, /--context/);
    await fails(run('daylight', ['--out', 'v.npy'], { sensors: [f] }), 1, /--out/);
    await fails(run('views', ['--bench'], { sensors: [f] }), 1, /--bench/);
    await fails(run('daylight', ['-h'], { sensors: [f] }), 1, /dl_usage/);
    await fails(run('daylight', ['--optics', 'nosuch=0.5'], { sensors: [f] }), 1, /nosuch/);
    await fails(run('views', ['--target', 'nosuch=g'], { sensors: [f] }), 1, /nosuch/);
    await fails(run('daylight', ['--epw', join(tests, 'nosuch.epw')], { sensors: [f] }), 2, /nosuch\.epw/);
  });

  describe('the GPU: tests/box.obj as the CLI smokes run it', () => {
    let first: NativeResult | undefined;

    it('daylight equals smoke.npy / .json byte for byte; onProgress reaches 1', async () => {
      expect(box().names).toEqual(['ground', 'box']);
      const seen: number[] = [];
      first = await run('daylight', sun, { sensors: [box()] }, { onProgress: (p) => seen.push(p) });
      sameAsCli(first, 'smoke.npy');
      expect(first.stats.sceneReused).toBe(false);
      expect(first.stats.rays).toBeGreaterThan(0);
      expect(seen.at(-1)).toBe(1);
      expect(seen.every((p, i) => p >= 0 && p <= 1 && (i === 0 || p >= seen[i - 1]))).toBe(true);
    });

    it('the same handle again: the resident scene serves it, same values; free() waits for the run', async () => {
      const p = run('daylight', sun, { sensors: [box()] });
      expect(() => box().free()).toThrow(/while a run uses/);
      const again = await p;
      expect(again.stats.sceneReused).toBe(true);
      expect(again.data).toEqual(first!.data);
    });

    it('views 360 on the same handle shares the scene and equals views_smoke.npy / .json byte for byte', async () => {
      const r = await run('views', view360, { sensors: [box()] });
      expect(r.stats.sceneReused).toBe(true);
      sameAsCli(r, 'views_smoke.npy');
    });

    it('the event loop runs while the GPU does: a timer fires before the run settles', async () => {
      let fired = false;
      const p = slow().then(() => fired);
      setTimeout(() => (fired = true));
      expect(await p).toBe(true);
    });
  });

  it('the views facade: sky above the horizon, the virtual ground below, no obstruction', async () => {
    const r = await run('views', open, { sensors: [facade()] });
    expect(r.columns).toEqual(['sky_front', 'ground_front', 'obstruction_front', 'ground_obstruction_front', 'distance_front', 'distance_min_front']);
    expect(r.data.map((c) => c.length)).toEqual([1, 1, 1, 1, 1, 1]);
    expect(r.data[0][0]).toBeCloseTo(0.4490987851112869, 9);
    expect(r.data[1][0]).toBeCloseTo(0.5509012148887131, 9);
    expect(r.data[2][0]).toBe(0);
  });

  it('cancel: a signal aborted before the call or while it is in flight rejects it with 2 "cancelled"', async () => {
    const f = facade(), ac = new AbortController();
    await fails(run('views', open, { sensors: [f] }, { signal: AbortSignal.abort() }), 2, /cancelled/);
    // the core cancelled it before its scene (run() alone would reject too): the next run on the new handle builds it
    expect((await run('views', open, { sensors: [f] })).stats.sceneReused).toBe(false);
    const p = run('views', open, { sensors: [f] }, { signal: ac.signal });
    ac.abort();
    await fails(p, 2, /cancelled/);
    expect((await run('views', open, { sensors: [f] })).data[0].length).toBe(1); // the next run works
  });

  it('a run thread that dies fails its own jobs only: a run started from the rejection gets a new thread', async () => {
    const post = Worker.prototype.postMessage, f = facade();
    Worker.prototype.postMessage = function (this: Worker) {
      Worker.prototype.postMessage = post;
      return post.call(this, null); // the job arrives as null: the thread's handler throws, uncaught
    };
    const again = await run('views', open, { sensors: [f] }).then(() => undefined,
      () => run('views', open, { sensors: [f] })); // before the dead thread's 'exit'
    expect(again?.data[0].length).toBe(1);
  });

  it.skipIf(!viewsExe)('run(views.exe) and run("views") agree on an ObjMesh: fromObj is toDmesh in-process', async () => {
    const m = { name: 'facade', positions: quad, indices: [0, 1, 2, 0, 2, 3] };
    const cli = await runExe(viewsExe, { sensors: [m], args: open });
    const own = await run('views', open, { sensors: [SimulationMesh.fromObj(m)] });
    expect(own.data).toEqual(cli.data);
    expect(own.manifest).toEqual(cli.manifest);
  });
});
