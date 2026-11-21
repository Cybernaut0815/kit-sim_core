// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { Document, NodeIO } from '@gltf-transform/core';
import { BoxGeometry, Euler, Matrix4, Mesh, Quaternion, Vector3 } from 'three';
import { describe, expect, it } from 'vitest';
import { applyToThree, colormap, dmeshStems, fromGLB, fromThree, meshIds, parseNpy, parseValues, run, runRemote, toDmesh, toObj } from '../src/index.js';

const hex = (b: Uint8Array | string) => createHash('sha256').update(b).digest('hex');

const box = (x = 1) => { const m = new Mesh(new BoxGeometry(1, 1, 1)); m.position.set(0, 2, 0); m.scale.x = x; m.name = 'box'; return m; };

describe('fromThree', () => {
  it('converts a translated BoxGeometry to Z-up', () => {
    const [m] = fromThree([box()]);
    expect(m.name).toBe('box');
    expect(m.positions.length).toBe(24 * 3);
    expect(m.indices.length).toBe(12 * 3);
    const p = m.positions, g = box().geometry.attributes.position; // three (x, y=2.5, z) -> core (x, -z, 2.5)
    expect([p[0], p[1], p[2]]).toEqual([g.getX(0), -g.getZ(0), g.getY(0) + 2]);
    expect(p[2]).toBe(2.5);
  });
  it('reverses winding for a mirrored transform', () => {
    const [a] = fromThree([box()]), [b] = fromThree([box(-1)]);
    expect([b.indices[0], b.indices[1], b.indices[2]]).toEqual([a.indices[0], a.indices[2], a.indices[1]]);
    expect(b.positions[0]).toBe(-a.positions[0]);
  });
});

describe('toObj', () => {
  it('writes one group per mesh with global 1-based indices', () => {
    const tri = { name: 't', positions: [0, 0, 0, 1, 0, 0, 0, 1, 0], indices: [0, 1, 2] };
    const obj = toObj([tri, { ...tri, name: 'u' }]);
    const lines = obj.trim().split('\n');
    expect(lines[0]).toBe('o t');
    expect(lines.filter((l) => l.startsWith('v ')).length).toBe(6);
    expect(lines.filter((l) => l.startsWith('f ')).length).toBe(2);
    expect(lines.filter((l) => l.startsWith('f '))).toEqual(['f 1 2 3', 'f 4 5 6']);
    expect(() => toObj([{ ...tri, indices: [0, 1, 3] }])).toThrow(/out of range/);
  });
});

describe('toDmesh', () => {
  it('writes the DLM1 header and arrays little-endian', () => {
    const [m] = fromThree([box()]);
    const bytes = toDmesh(m);
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    expect(bytes.byteLength).toBe(32 + 24 * 24 + 4 * 12 + 4 * 36);
    expect(String.fromCharCode(...bytes.subarray(0, 4))).toBe('DLM1');
    expect(dv.getUint32(4, true)).toBe(0);
    expect([dv.getBigUint64(8, true), dv.getBigUint64(16, true), dv.getBigUint64(24, true)]).toEqual([24n, 12n, 36n]);
    expect([dv.getFloat64(32, true), dv.getFloat64(40, true), dv.getFloat64(48, true)]).toEqual([m.positions[0], m.positions[1], m.positions[2]]);
    expect(dv.getInt32(32 + 24 * 24, true)).toBe(3);
    expect(dv.getInt32(bytes.byteLength - 4, true)).toBe(m.indices[35]);
    expect(() => toDmesh({ name: 't', positions: [0, 0, 0, 1, 0, 0, 0, 1, 0], indices: [0, 1, 3] })).toThrow(/out of range/);
    expect(() => toDmesh({ name: 't', positions: [0, 0, 0, 1, 0, 0, 0, 1, 0], indices: [0, 1, 1.5] })).toThrow(/out of range/);
  });
  it('with slots: flags 1 and uint16 slot[nFaces] after the indices; one per triangle or it throws', () => {
    const quad = { name: 'q', positions: [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0], indices: [0, 1, 2, 0, 2, 3] };
    const plain = toDmesh(quad), bytes = toDmesh({ ...quad, slots: Uint16Array.of(7, 65535) }), end = 32 + 24 * 4 + 4 * 2 + 4 * 6;
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    expect([plain.byteLength, bytes.byteLength]).toEqual([end, end + 2 * 2]);
    expect([new DataView(plain.buffer).getUint32(4, true), dv.getUint32(4, true)]).toEqual([0, 1]);
    expect(bytes.subarray(8, end)).toEqual(plain.subarray(8)); // the arrays before the block are unchanged
    expect([dv.getUint16(end, true), dv.getUint16(end + 2, true)]).toEqual([7, 65535]);
    expect(() => toDmesh({ ...quad, slots: Uint16Array.of(0) })).toThrow(/one value per triangle \(2\), not 1/);
  });
});

describe('fromGLB', () => {
  it('round-trips a generated glb', async () => {
    const doc = new Document();
    const buf = doc.createBuffer();
    const pos = doc.createAccessor().setType('VEC3').setArray(new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0])).setBuffer(buf);
    const idx = doc.createAccessor().setType('SCALAR').setArray(new Uint16Array([0, 1, 2])).setBuffer(buf);
    const mesh = doc.createMesh('tri').addPrimitive(doc.createPrimitive().setAttribute('POSITION', pos).setIndices(idx));
    doc.createScene().addChild(doc.createNode('n').setMesh(mesh).setTranslation([1, 2, 3]));
    const glb = await new NodeIO().writeBinary(doc);
    const meshes = await fromGLB(glb);
    expect(meshes.length).toBe(1);
    expect(meshes[0].name).toBe('n/tri');
    expect(Array.from(meshes[0].positions)).toEqual([1, -3, 2, 2, -3, 2, 1, -3, 3]);
    expect(Array.from(meshes[0].indices)).toEqual([0, 1, 2]);
  });
});

it('parseValues: one column per quantity', () => {
  expect(parseValues('1 2.5\n\n 3 4 \n').map((c) => Array.from(c))).toEqual([[1, 3], [2.5, 4]]);
  expect(parseValues('\n')).toEqual([]);
});

it('parseNpy: C-order rows in, one column per quantity out', () => {
  const header = "{'descr': '<f8', 'fortran_order': False, 'shape': (2, 2), }".padEnd(117) + '\n'; // 10 + 118 = 128 bytes (64-aligned like NumPy)
  const buf = new Uint8Array(128 + 32), dv = new DataView(buf.buffer);
  buf.set([0x93, 0x4e, 0x55, 0x4d, 0x50, 0x59, 1, 0, 118, 0]); // \x93NUMPY v1.0, header length 118 LE
  buf.set(Array.from(header, (c) => c.charCodeAt(0)), 10);
  [1, 2.5, 3, 4].forEach((v, i) => dv.setFloat64(128 + 8 * i, v, true)); // rows [1, 2.5] and [3, 4]
  expect(parseNpy(buf, 2).map((c) => Array.from(c))).toEqual([[1, 3], [2.5, 4]]);
  expect(parseNpy(Buffer.concat([Buffer.alloc(9), Buffer.from(buf)]).subarray(9), 2).map((c) => Array.from(c))).toEqual([[1, 3], [2.5, 4]]); // Buffer view at an odd byteOffset (fs.readFile pool)
  expect(parseNpy(buf.subarray(0, 128 + 24), 2).map((c) => Array.from(c))).toEqual([[1], [2.5]]); // partial trailing row dropped
  expect(() => parseNpy(new Uint8Array(16), 2)).toThrow(/NumPy/);
  expect(() => parseNpy(buf, 0)).toThrow(/cols/);
});

it('colormap', () => {
  const near = (rgb: Float32Array, i: number, want: number[]) => Array.from(rgb.subarray(3 * i, 3 * i + 3)).forEach((c, k) => expect(c).toBeCloseTo(want[k], 5));
  const rgb = colormap([0, 0.3, 1, NaN], { vmin: 0, vmax: 1 });
  near(rgb, 0, [0.267004, 0.004874, 0.329415]);
  near(rgb, 1, [0.206931, 0.372933, 0.550578]);
  near(rgb, 2, [0.993248, 0.906157, 0.143936]);
  near(rgb, 3, [0.5, 0.5, 0.5]);
  near(colormap([10, 20, NaN]), 1, [0.993248, 0.906157, 0.143936]); // default range = finite min/max
  near(colormap([7, 7]), 1, [0.267004, 0.004874, 0.329415]); // vmin == vmax -> t = 0
  const two = colormap([0, 0.5, 1], { stops: [[0, 0, 0], [1, 1, 1]] }); // any evenly spaced gradient
  near(two, 1, [0.5, 0.5, 0.5]);
  near(two, 2, [1, 1, 1]);
  expect(() => colormap([1], { stops: [[0, 0, 0]] })).toThrow(/two colours/);
});

describe('applyToThree', () => {
  const vals = (n: number) => Float64Array.from({ length: n }, (_, i) => i);
  it('per face: non-indexed, 3 equal colours per triangle', async () => {
    const m = new Mesh(new BoxGeometry());
    const rgb = await applyToThree(m, vals(12));
    expect(m.geometry.index).toBeNull();
    expect(m.geometry.getAttribute('color').count).toBe(36);
    expect(rgb.length).toBe(108);
    expect(Array.from(rgb.subarray(3, 9))).toEqual([rgb[0], rgb[1], rgb[2], rgb[0], rgb[1], rgb[2]]);
    expect((m.material as { vertexColors: boolean }).vertexColors).toBe(true);
  });
  it('per vertex: stays indexed; bad length rejects; material:false keeps the material', async () => {
    const m = new Mesh(new BoxGeometry()), mat = m.material;
    await applyToThree(m, vals(24), { material: false });
    expect(m.geometry.index).not.toBeNull();
    expect(m.geometry.getAttribute('color').count).toBe(24);
    expect(m.material).toBe(mat);
    await expect(applyToThree(new Mesh(new BoxGeometry()), vals(7))).rejects.toThrow(/triangle count/);
  });
  it('rgb override: the caller’s colours land on the faces as given', async () => {
    const m = new Mesh(new BoxGeometry());
    const rgb = new Float32Array(36).map((_, i) => (i % 3 === 0 ? 1 : 0)); // every face red
    const out = await applyToThree(m, vals(12), { rgb });
    expect(Array.from(out.subarray(0, 6))).toEqual([1, 0, 0, 1, 0, 0]);
    await expect(applyToThree(new Mesh(new BoxGeometry()), vals(12), { rgb: new Float32Array(9) })).rejects.toThrow(/3 floats/);
  });
});

it('toDmesh(local, {matrix, scale}) equals fromThree + toDmesh byte for byte: indexed or not, mirrored, slotted', () => {
  for (const geom of [new BoxGeometry(1, 2, 3, 2, 1, 1), new BoxGeometry(1, 2, 3, 2, 1, 1).toNonIndexed()]) {
    for (const sx of [1, -1]) {
      for (const slotted of [false, true]) {
        const W = new Matrix4().compose(new Vector3(4.2e5, 12.5, -5.1e6), new Quaternion().setFromEuler(new Euler(0.3, 1.1, -0.2)), new Vector3(1.5 * sx, 1, 0.7));
        const stand = new Mesh(geom);
        stand.matrixAutoUpdate = false;
        stand.matrix.copy(W);
        const old = fromThree([stand], { scale: 0.0254 })[0], tris = old.indices.length / 3;
        const slots = slotted ? Uint16Array.from({ length: tris }, (_, i) => i % 3) : undefined;
        const local = toDmesh({ positions: geom.attributes.position.array, indices: geom.index?.array ?? null, slots }, { matrix: W.elements, scale: 0.0254 });
        expect(Buffer.from(local).equals(Buffer.from(toDmesh({ ...old, slots })))).toBe(true);
      }
    }
  }
  expect(() => toDmesh({ positions: new Float32Array(12) }, { matrix: new Matrix4().elements })).toThrow(/triangle triples/);
});

it('meshIds: the bytes\' SHA-256 (8 at a time, a name folded in), ids given kept, a context twin of a sensor salted', async () => {
  const m = fromThree([box()])[0], d = hex(toDmesh(m)), lazy = () => ({ dmesh: () => toDmesh({ positions: m.positions, indices: m.indices }) });
  const other = toDmesh(fromThree([box(2)])[0]);
  const ids = await meshIds({ sensors: [lazy(), lazy(), { id: 'given', dmesh: () => { throw new Error('built') } }], context: [lazy(), { dmesh: () => other }] });
  expect(ids).toEqual([d, d, 'given', hex(`${d}\0ctx`), hex(other)]);
  expect(dmeshStems(ids).slice(0, 2)).toEqual([d, `${d}_2`]);
  expect(await meshIds({ sensors: [m] })).toEqual([hex(`${d}\0name\0box`)]); // named: the server keeps one name per id
  expect(m.id).toBeUndefined(); // nothing written back
  expect(await meshIds({ sensors: ids.slice(0, 1).map((id) => ({ id, dmesh: () => other })), context: [{ id: d, dmesh: () => other }] })).toEqual([d, hex(`${d}\0ctx`)]);
});

it('dmeshStems', () => {
  expect(dmeshStems(['a b', 'a_b', 'A_B', '', 'x/y'])).toEqual(['a_b', 'a_b_2', 'A_B_3', 'mesh', 'x_y']); // case kept, uniqueness case-insensitive
  expect(dmeshStems([''], 'sensors')).toEqual(['sensors']);
});

describe('runRemote', () => {
  const manifest = { columns: ['sunhours_front', 'radiation_front'], duration_h: 1, dirs: { sky: 145, ground: 145, sun: 1 }, meshes: [{ name: 'box', first: 0, count: 2 }], run_s: 0.1, cache: { response: 'miss' } };
  const f32 = new Uint8Array(Float32Array.of(1, 3, 2.5, 4).buffer); // GET /jobs/{id}/values: column-major, sunhours rows 0, 1, then radiation rows 0, 1
  const json = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status });
  const stream = (...chunks: Uint8Array[]) => new Response(new ReadableStream({ start(c) { for (const b of chunks) c.enqueue(b); c.close(); } }));
  it('submits a job by mesh ids, uploads what the server lacks first (no 404 round trip), follows the queue, streams the values into columns', async () => {
    const m = fromThree([box()])[0];
    m.id = 'id-1';
    const calls: { url: string; method?: string; body?: unknown; headers?: HeadersInit }[] = [];
    let polls = 0;
    const fetchMock = (async (url: string, init?: RequestInit) => {
      calls.push({ url, method: init?.method, body: init?.body, headers: init?.headers });
      if (url.endsWith('/meshes/missing')) return json({ missing: JSON.parse(init!.body as string).ids });
      if (init?.method === 'PUT') return new Response(null, { status: 204 });
      if (init?.method === 'POST') return json({ id: 'j1', state: 'queued', position: 1 }, 202);
      if (url.endsWith('/values')) return stream(f32.subarray(0, 5), f32.subarray(5)); // two chunks, split inside a float
      return ++polls === 1 ? json({ id: 'j1', state: 'running', position: 0 })
        : json({ id: 'j1', state: 'done', ...manifest, values: { url: '/jobs/j1/values', dtype: '<f4', order: 'column-major', shape: [2, 2], bytes: 16 }, values_b64: 'not base64' });
    }) as unknown as typeof fetch;
    const seen: string[] = [];
    const r = await runRemote('http://h/', { sensors: [m], args: ['--quantity', 'sunhours'], clientId: 'tab-1', onStatus: (j) => seen.push(`${j.state}:${j.position}`), fetch: fetchMock });
    expect(calls.map((c) => `${c.method ?? 'GET'} ${c.url}`)).toEqual([
      'POST http://h/meshes/missing', 'PUT http://h/mesh/id-1?name=box', 'POST http://h/jobs', 'GET http://h/jobs/j1?wait=25', 'GET http://h/jobs/j1?wait=25', 'GET http://h/jobs/j1/values',
    ]);
    expect(JSON.parse(calls[0].body as string)).toEqual({ ids: ['id-1'] });
    expect(seen).toEqual(['queued:1', 'running:0']);
    expect([calls[0].headers, calls[5].headers]).toEqual([{ 'X-Client-Id': 'tab-1' }, { 'X-Client-Id': 'tab-1' }]);
    expect(String.fromCharCode(...(calls[1].body as Uint8Array).subarray(0, 4))).toBe('DLM1');
    const fd = calls[2].body as FormData;
    expect(fd.getAll('sensors')).toEqual(['id-1']);
    expect(fd.getAll('args')).toEqual(['--quantity', 'sunhours']);
    expect(fd.has('epw')).toBe(false);
    expect(r.columns).toEqual(manifest.columns);
    expect(r.data.map((c) => Array.from(c))).toEqual([[1, 3], [2.5, 4]]);
    expect(r.data[1].buffer).toBe(r.data[0].buffer); // views of one buffer
    expect(r.manifest.meshes[0].count).toBe(2);
    expect('values_b64' in r.manifest).toBe(false); // an inline copy is dropped, never decoded
    expect(r.cache).toEqual({ response: 'miss' });
  });
  it('a mesh sent before goes by id alone; one the server lost (404) is put again before one retry', async () => {
    const m = fromThree([box()])[0];
    m.id = 'id-2';
    const calls: string[] = [];
    let posts = 0;
    const fetchMock = (async (url: string, init?: RequestInit) => {
      calls.push(`${init?.method ?? 'GET'} ${url}`);
      if (url.endsWith('/meshes/missing')) return json({ missing: JSON.parse(init!.body as string).ids });
      if (init?.method === 'PUT') return new Response(null, { status: 204 });
      if (init?.method === 'POST') return ++posts === 2 ? json({ missing: ['id-2'] }, 404) : json({ id: 'j9', state: 'done', ...manifest }, 202); // done at once, as a cache hit
      return stream(f32); // no values.url in the job: the default route
    }) as unknown as typeof fetch;
    await runRemote('http://k', { sensors: [m], fetch: fetchMock }); // first run: put, then the job
    await runRemote('http://k', { sensors: [m], fetch: fetchMock }); // the server restarted: 404, put again, retry
    await runRemote('http://k', { sensors: [m], fetch: fetchMock }); // by id alone
    expect(calls).toEqual([
      'POST http://k/meshes/missing', 'PUT http://k/mesh/id-2?name=box', 'POST http://k/jobs', 'GET http://k/jobs/j9/values',
      'POST http://k/jobs', 'PUT http://k/mesh/id-2?name=box', 'POST http://k/jobs', 'GET http://k/jobs/j9/values',
      'POST http://k/jobs', 'GET http://k/jobs/j9/values',
    ]);
  });
  it('the values must be exactly rows x columns float32: a short or long body or a failed GET rejects', async () => {
    const answer = (values: Response) => (async (u: string, init?: RequestInit) => u.endsWith('/meshes/missing') ? json({ missing: [] }) : init?.method === 'PUT' ? new Response(null, { status: 204 })
      : init?.method === 'POST' ? json({ id: 'j5', state: 'done', ...manifest }, 202) : values) as unknown as typeof fetch;
    const go = (values: Response) => runRemote('http://h', { sensors: fromThree([box()]), fetch: answer(values) });
    await expect(go(new Response(f32.subarray(1)))).rejects.toThrow(/values: 15 of 16 bytes/);
    await expect(go(stream(f32, f32.subarray(0, 1)))).rejects.toThrow(/values: 17 of 16 bytes/);
    await expect(go(new Response('error: expired\n', { status: 404 }))).rejects.toThrow(/jobs\/j5\/values failed \(404\): error: expired/);
  });
  it('ids are the bytes\' SHA-256 (name folded in), nothing written back; sends epw_url or an epw blob, rejects with the server text', async () => {
    const seen: FormData[] = [];
    const fetchMock = (async (url: string, init?: RequestInit) => {
      if (url.endsWith('/meshes/missing')) return json({ missing: [] });
      if (init?.method === 'PUT') return new Response(null, { status: 204 });
      seen.push(init!.body as FormData);
      return new Response('error: unknown flag --x\n', { status: 400 });
    }) as unknown as typeof fetch;
    const sensors = fromThree([box()]);
    await expect(runRemote('http://h', { sensors, epw: { url: 'https://x/y.epw' }, args: ['--x'], fetch: fetchMock })).rejects.toThrow(/\(400\): error: unknown flag --x/);
    expect(seen[0].getAll('sensors')).toEqual([hex(`${hex(toDmesh(sensors[0]))}\0name\0box`)]);
    expect(sensors[0].id).toBeUndefined();
    expect(seen[0].get('epw_url')).toBe('https://x/y.epw');
    await expect(runRemote('http://h', { sensors, epw: new Blob(['LOCATION,x']), fetch: fetchMock })).rejects.toThrow(/400/);
    expect(seen[1].get('epw')).toBeInstanceOf(Blob);
    expect(seen[1].has('tool')).toBe(false); // daylight: the request as before
    await expect(runRemote('http://h', { sensors, tool: 'views', fetch: fetchMock })).rejects.toThrow(/400/);
    expect(seen[2].get('tool')).toBe('views');
  });
  it('a failed job rejects with its status and message; abort cancels the job on the server', async () => {
    const failed = (async (u: string, init?: RequestInit) => u.endsWith('/meshes/missing') ? json({ missing: [] }) : init?.method === 'PUT' ? new Response(null, { status: 204 }) : init?.method === 'POST'
      ? json({ id: 'j2', state: 'queued', position: 0 }, 202)
      : json({ id: 'j2', state: 'error', status: 500, error: 'error: out of memory' })) as unknown as typeof fetch;
    await expect(runRemote('http://h', { sensors: fromThree([box()]), fetch: failed })).rejects.toThrow(/job j2 error \(500\): error: out of memory/);

    const ctrl = new AbortController(), calls: string[] = [];
    const hanging = (async (url: string, init?: RequestInit) => {
      if (url.endsWith('/meshes/missing')) return json({ missing: [] });
      if (init?.method === 'PUT') return new Response(null, { status: 204 });
      calls.push(`${init?.method ?? 'GET'} ${url}`);
      if (init?.method === 'POST') return json({ id: 'j3', state: 'queued', position: 2 }, 202);
      if (init?.method === 'DELETE') return json({ id: 'j3', state: 'cancelled' });
      return new Promise<Response>((_res, rej) => { // like fetch: an aborted signal rejects at once, a later abort when it fires
        const stop = () => rej(new DOMException('aborted', 'AbortError'));
        if (init?.signal?.aborted) stop(); else init?.signal?.addEventListener('abort', stop);
      });
    }) as unknown as typeof fetch;
    const p = runRemote('http://h', { sensors: fromThree([box()]), signal: ctrl.signal, onStatus: () => ctrl.abort(), fetch: hanging });
    await expect(p).rejects.toThrow(/aborted/);
    expect(calls).toEqual(['POST http://h/jobs', 'GET http://h/jobs/j3?wait=25', 'DELETE http://h/jobs/j3']);
  });
  it('an abort while the server makes the job still deletes it: the POST runs to its answer', async () => {
    const ctrl = new AbortController(), calls: string[] = [];
    const fetchMock = (async (url: string, init?: RequestInit) => {
      if (url.endsWith('/meshes/missing')) return json({ missing: [] }); // carries the signal, unlike POST /jobs
      if (init?.method === 'PUT') return new Response(null, { status: 204 });
      calls.push(`${init?.method ?? 'GET'} ${url}`);
      if (init?.method === 'POST') { expect(init.signal).toBeUndefined(); ctrl.abort(); return json({ id: 'j4', state: 'queued', position: 0 }, 202); }
      return json({ id: 'j4', state: 'cancelled' });
    }) as unknown as typeof fetch;
    await expect(runRemote('http://h', { sensors: fromThree([box()]), signal: ctrl.signal, fetch: fetchMock })).rejects.toThrow(/abort/i);
    expect(calls).toEqual(['POST http://h/jobs', 'DELETE http://h/jobs/j4']);
  });
  const tri = (k: number) => ({ dmesh: () => toDmesh({ positions: [k, 0, 0, k + 1, 0, 0, k, 1, 0], indices: [0, 1, 2] }) });
  const one = { ...manifest, columns: ['sunhours_front'], meshes: [{ name: 'x', first: 0, count: 1 }] };
  it('uploads only what the server lacks, 6 PUTs at a time with progress; after a reload (a new lib state) none', async () => {
    const store = new Set((await meshIds({ sensors: Array.from({ length: 20 }, (_, k) => tri(k)) })).slice(0, 5)), calls: string[] = [], forms: FormData[] = [];
    let flying = 0, peak = 0;
    const server = (async (url: string, init?: RequestInit) => {
      calls.push(`${init?.method ?? 'GET'} ${url}`);
      if (url.endsWith('/meshes/missing')) return json({ missing: (JSON.parse(init!.body as string).ids as string[]).filter((id) => !store.has(id)) });
      if (init?.method === 'PUT') {
        peak = Math.max(peak, ++flying);
        await new Promise((r) => setTimeout(r, 2));
        flying--;
        store.add(decodeURIComponent(new URL(url).pathname.split('/')[2]));
        return new Response(null, { status: 204 });
      }
      if (init?.method === 'POST') { forms.push(init.body as FormData); return json({ id: 'j7', state: 'done', ...one }, 202); }
      return stream(new Uint8Array(4));
    }) as unknown as typeof fetch;
    const progress: [number, number][] = [];
    await runRemote('http://a', { sensors: Array.from({ length: 20 }, (_, k) => tri(k)), fetch: server, onUpload: (d, n) => progress.push([d, n]) });
    const puts = calls.filter((c) => c.startsWith('PUT'));
    expect(puts.length).toBe(15);
    for (const c of puts) expect(c).toMatch(/^PUT http:\/\/a\/mesh\/([0-9a-f]{64})\?name=\1$/); // unnamed: the core name is the id
    expect(peak).toBe(6);
    expect(progress).toEqual(Array.from({ length: 16 }, (_, k) => [k, 15]));
    calls.length = 0;
    await runRemote('http://b', { sensors: Array.from({ length: 20 }, (_, k) => tri(k)), fetch: server }); // as after a reload: this lib has sent nothing to b
    expect(calls).toEqual(['POST http://b/meshes/missing', 'POST http://b/jobs', 'GET http://b/jobs/j7/values']);
    expect(forms[1].getAll('sensors')).toEqual(forms[0].getAll('sensors'));
  });
  it('an older server without /meshes/missing (404) gets every mesh this process has not sent', async () => {
    const m = { ...fromThree([box()])[0], id: 'id-old' }, calls: string[] = [];
    const old = (async (url: string, init?: RequestInit) => {
      calls.push(`${init?.method ?? 'GET'} ${url}`);
      if (url.endsWith('/meshes/missing')) return new Response('error: not found\n', { status: 404 });
      if (init?.method === 'PUT') return new Response(null, { status: 204 });
      if (init?.method === 'POST') return json({ id: 'j8', state: 'done', ...one }, 202);
      return stream(new Uint8Array(4));
    }) as unknown as typeof fetch;
    await runRemote('http://o', { sensors: [m, { ...m }], fetch: old });
    expect(calls).toEqual(['POST http://o/meshes/missing', 'PUT http://o/mesh/id-old?name=box', 'POST http://o/jobs', 'GET http://o/jobs/j8/values']);
  });
  it("named meshes go up under their own names, never a job's _2 (the server keeps one per id); no progress after a failed PUT", async () => {
    const named = (k: number, name: string) => ({ name, dmesh: () => toDmesh({ positions: [k, 0, 0, k + 1, 0, 0, k, 1, 0], indices: [0, 1, 2] }) });
    const puts: string[] = [], progress: number[] = [];
    let fail = '';
    const server = (async (url: string, init?: RequestInit) => {
      if (url.endsWith('/meshes/missing')) return json({ missing: JSON.parse(init!.body as string).ids });
      if (init?.method === 'PUT') {
        puts.push(new URL(url).searchParams.get('name')!);
        if (fail && url.includes(fail)) return new Response('error: mesh has no faces\n', { status: 400 });
        await new Promise((r) => setTimeout(r, 5));
        return new Response(null, { status: 204 });
      }
      if (init?.method === 'POST') return json({ id: 'j5', state: 'done', ...one }, 202);
      return stream(new Uint8Array(4));
    }) as unknown as typeof fetch;
    await runRemote('http://n1', { sensors: [named(1, 'wall'), named(2, 'wall'), named(3, 'a b')], fetch: server });
    expect(puts).toEqual(['wall', 'wall', 'a_b']);
    const ids = await meshIds({ sensors: Array.from({ length: 8 }, (_, k) => named(10 + k, `m${k}`)) });
    fail = ids[0]; // the first of 8 fails while the next 5 are on their way
    await expect(runRemote('http://n2', { sensors: Array.from({ length: 8 }, (_, k) => named(10 + k, `m${k}`)), fetch: server, onUpload: (d) => progress.push(d) })).rejects.toThrow(/mesh has no faces/);
    await new Promise((r) => setTimeout(r, 30)); // the 5 still on their way finish
    expect(progress).toEqual([0]);
  });
  it('twins: equal bytes go up once, a shade equal to a receiver under its salted id', async () => {
    const d = hex(tri(9).dmesh()), S = hex(`${d}\0ctx`), calls: string[] = [];
    let form: FormData | null = null;
    const server = (async (url: string, init?: RequestInit) => {
      calls.push(`${init?.method ?? 'GET'} ${url}`);
      if (url.endsWith('/meshes/missing')) return json({ missing: JSON.parse(init!.body as string).ids });
      if (init?.method === 'PUT') return new Response(null, { status: 204 });
      if (init?.method === 'POST') { form = init.body as FormData; return json({ id: 'j6', state: 'done', ...one }, 202); }
      return stream(new Uint8Array(4));
    }) as unknown as typeof fetch;
    await runRemote('http://t', { sensors: [tri(9), tri(9)], context: [tri(9)], fetch: server });
    expect(calls.filter((c) => c.startsWith('PUT'))).toEqual([`PUT http://t/mesh/${d}?name=${d}`, `PUT http://t/mesh/${S}?name=${S}`]);
    expect([form!.getAll('sensors'), form!.getAll('context')]).toEqual([[d, d], [S]]);
  });
});

const liveUrl = process.env.DAYLIGHT_URL ?? '';
it.skipIf(!liveUrl)('runRemote() against a live daylight_server (set DAYLIGHT_URL to enable)', async () => {
  const sensors = fromThree([box()]);
  const args = ['--lat', '42.35', '--lon', '-71.07', '--tz', '-5', '--quantity', 'sunhours', '--time', '06-21T12:00'];
  const r = await runRemote(liveUrl, { sensors, args });
  expect(r.columns).toEqual(['sunhours_front']);
  expect(r.data.map((c) => c.length)).toEqual([12]);
  for (const col of r.data) for (const v of col) expect(v).toBeGreaterThanOrEqual(0), expect(v).toBeLessThanOrEqual(1);
  const again = await runRemote(liveUrl, { sensors, args }); // same ids and args: the server answers from its response cache
  expect(again.cache?.response).toBe('hit');
});

it.skipIf(!liveUrl)('twin receivers with per-mesh optics and a twin shade run with --bounces 1 (live)', async () => {
  const lazy = () => ({ dmesh: () => toDmesh(fromThree([box()])[0]) }), sensors = [lazy(), lazy()], context = [lazy()];
  const [a, b, c] = dmeshStems(await meshIds({ sensors, context })); // the core names: h, h_2, and the shade's salted id
  const args = ['--lat', '42.35', '--lon', '-71.07', '--tz', '-5', '--quantity', 'radiation', '--time', '06-21T12:00', '--bounces', '1', '--bounce-samples', '64',
    '--optics', `${a}/0=0.5,0;${b}/0=0.3,0;${c}/0=0.4,0`];
  const epw = new Blob([readFileSync(new URL('../../../validate/_dl/epw/USA_MA_Boston-City.WSO_TMY.epw', import.meta.url))]); // radiation needs weather
  const r = await runRemote(liveUrl, { sensors, context, args, epw });
  expect(r.manifest.meshes.map((g) => [g.name, g.count])).toEqual([[a, 12], [b, 12]]);
  expect(r.data[0].length).toBe(24);
});

const exe = process.env.DAYLIGHT_EXE && existsSync(process.env.DAYLIGHT_EXE) ? process.env.DAYLIGHT_EXE : '';
it.skipIf(!exe)('run() on a box through the core via .dmesh in, .npy out (set DAYLIGHT_EXE to enable)', async () => {
  const args = ['--lat', '42.35', '--lon', '-71.07', '--tz', '-5', '--quantity', 'sunhours', '--time', '06-21T12:00'];
  const r = await run(exe, { sensors: fromThree([box()]), args });
  expect(r.columns).toEqual(['sunhours_front']); // <quantity>_<side>, as the README snippet relies on
  expect(r.data.map((c) => c.length)).toEqual([12]); // one column, a value per triangle
  for (const col of r.data) for (const v of col) expect(v).toBeGreaterThanOrEqual(0), expect(v).toBeLessThanOrEqual(1);
  const slotted = fromThree([box()]).map((m) => ({ ...m, slots: Uint16Array.from({ length: m.indices.length / 3 }, (_, i) => i % 3) }));
  expect((await run(exe, { sensors: slotted, args })).data).toEqual(r.data); // the core reads the slot block; same values
});
