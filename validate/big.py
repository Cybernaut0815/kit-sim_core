# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""Large-model bench of the daylight CLI on bhs.usd (14,342 meshes, 17.2M triangles) and a generated 20M-triangle mesh.

  uv run python -I big.py convert path/to/bhs.usd --root R   # -> R/bhs/model.dmesh (all meshes merged) + meshes.json
  uv run python -I big.py hills [--tris 20000000] --root R   # -> R/hills/model.{usdc,dmesh} + meshes.json
  uv run python -I big.py run --exe EXE --mesh bhs [--cases S1,A6,B1,V1] --tag T --root R [--epw EPW]
  uv run python -I big.py table --base base --tag T --root R # markdown, one table per layer
  uv run python -I big.py selftest

`run` runs `EXE R/<mesh>/model.dmesh --epw EPW <case> -v --out R/runs/<T>/<mesh>-<case>.npy`, prints a markdown row
and appends a record per case to R/results.jsonl: wall / CPU s, the -v lines parsed (cli), nvidia-smi every 200 ms (gpu),
free RAM / commit before and the exact peak commit / working set (mem), out.npy's SHA-256 and how it differs from
R/runs/base (values). bench.mjs appends the layers server / chrome / firefox to the same file. Every record has layer,
mesh, case, tag, ok, error, when; `table` shows every other number and short string of the latest record per key."""
import argparse
import ctypes
import hashlib
import json
import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from time import perf_counter

import numpy as np
from daylight import meshes, to_dmesh
from pxr import Usd, UsdGeom, Vt

HERE = Path(__file__).resolve().parent
BOSTON = HERE / '_dl' / 'epw' / 'USA_MA_Boston-City.WSO_TMY.epw'          # golden.py's (python -I cannot import it)
S1 = ['--from', '06-21T08:00', '--to', '06-21T18:00', '--step', '30', '--quantity', 'radiation']
CASES = {'S1': S1, 'A6': ['--quantity', 'radiation,direct,diffuse,irradiance,illuminance,sunhours'],
         'B1': S1 + ['--bounces', '1', '--bounce-samples', '256'], 'V1': S1 + ['--on', 'vertices']}
GB = 1 << 30
SMI = ['nvidia-smi', '-i', '0', '--query-gpu=utilization.gpu,memory.used,temperature.gpu,clocks.sm',
       '--format=csv,noheader,nounits', '-lms', '200']
# the CLI's -v lines: main.cpp read / run total / output, run.cpp scene / GAS / reflected light
VLINES = [r'info: read .*: (?P<meshes>\d+) meshes, (?P<faces>\d+) faces, (?P<points>\d+) points, (?P<read_s>[\d.]+) s',
          r'info: (?P<sensors>\d+) sensors, (?P<triangles>\d+) triangles[^,]*, (?P<steps>\d+) steps',
          r'info: GAS (?P<gas_mb>[\d.]+) MB build (?P<build_ms>\d+) ms, (?P<launches>\d+) launches, (?P<rays>\S+) rays in '
          r'(?P<trace_ms>\d+) ms \((?P<grays>[\d.]+) GRays/s',
          r'info: reflected light: .* (?P<paths>\S+) paths in (?P<bounce_ms>\d+) ms \((?P<bounce_launches>\d+) launches',
          r'info: run (?P<run_s>[\d.]+) s total', r'info: output (?P<output_s>[\d.]+) s']
ROW = ('wall_s cores cli.read_s cli.gas_mb cli.build_ms cli.trace_ms cli.grays cli.bounce_ms cli.run_s cli.output_s '
       'gpu.busy_pct gpu.temp_max_c gpu.sm_mhz gpu.mem_rise_gb mem.peak_commit_gb mem.peak_ws_gb values.vs').split()
SKIP = {'layer', 'mesh', 'case', 'tag', 'when', 'ok', 'error', 'sha256', 'exe'}    # identity, outcome, hashes: no rows


class PMC(ctypes.Structure):                                               # PROCESS_MEMORY_COUNTERS
    _fields_ = [('cb', ctypes.c_ulong), ('faults', ctypes.c_ulong)] + [(k, ctypes.c_size_t) for k in (
        'PeakWorkingSetSize', 'WorkingSetSize', 'peak_paged', 'paged', 'peak_nonpaged', 'nonpaged', 'PagefileUsage',
        'PeakPagefileUsage')]


class MEMSTAT(ctypes.Structure):                                           # MEMORYSTATUSEX
    _fields_ = [('size', ctypes.c_ulong), ('load', ctypes.c_ulong)] + [(k, ctypes.c_ulonglong) for k in (
        'phys', 'free_phys', 'commit_limit', 'free_commit', 'virtual', 'free_virtual', 'free_extended')]


K32 = ctypes.WinDLL('kernel32', use_last_error=True)
K32.K32GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
K32.GetProcessTimes.argtypes = [ctypes.c_void_p] * 5


def sha(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def merge(ms):
    """[(name, points, counts, indices)] -> one (points, counts, indices); each mesh's indices offset by its first point."""
    first = np.cumsum([0] + [len(m[1]) for m in ms])
    return (np.concatenate([m[1] for m in ms]), np.concatenate([m[2] for m in ms]),
            np.concatenate([m[3] + int(o) for m, o in zip(ms, first)]))   # + a Python int keeps int32 (NumPy 2)


def convert(usd, root):
    """USD stage -> R/<stem>/model.dmesh + meshes.json [{prim, points, faces}] in core order.
    ponytail: every mesh merged into one, since the CLI takes mesh files only as arguments and 14,342 paths overflow the
    32k command line; the server bench covers per-mesh runs. Add an @listfile CLI flag if a per-mesh-only regression shows."""
    t0 = perf_counter()
    st = Usd.Stage.Open(str(usd))
    t1 = perf_counter()
    ms = meshes(st)
    t2 = perf_counter()
    P, C, I = merge(ms)
    d = root / usd.stem
    d.mkdir(parents=True, exist_ok=True)
    to_dmesh(P, C, I, d / 'model.dmesh')
    (d / 'meshes.json').write_text(json.dumps([{'prim': n, 'points': len(p), 'faces': len(c)} for n, p, c, _ in ms]))
    print(f'{len(ms):,} meshes, {len(P):,} points, {len(C):,} faces, all triangles: {bool((C == 3).all())}; Stage.Open '
          f'{t1 - t0:.1f} s, meshes() {t2 - t1:.1f} s, merge + write {perf_counter() - t2:.1f} s -> {d}')


def hills(n):
    """n x n grid of 1 m quads, two triangles each wound to face up, z = 8 sin(x/19) cos(y/23) m so the terrain shades
    itself; points rounded to float32 so the .usdc and the .dmesh hold the same coordinates. -> (points, indices)."""
    x, y = np.meshgrid(np.arange(n + 1.0), np.arange(n + 1.0))
    P = np.stack([x, y, 8 * np.sin(x / 19) * np.cos(y / 23)], -1).reshape(-1, 3).astype(np.float32)
    v = (np.arange(n, dtype=np.int32)[:, None] * (n + 1) + np.arange(n, dtype=np.int32)).ravel()   # lower-left corners
    return P, np.stack([v, v + 1, v + n + 2, v, v + n + 2, v + n + 1], 1).ravel()


def write_hills(tris, root):
    """hills(n), 2 n^2 ~ tris -> R/hills/model.dmesh, model.usdc (Z-up, metres, one mesh /hills, written as ingest.py
    writes its grid) and meshes.json."""
    t, n, d = perf_counter(), round((tris / 2) ** 0.5), root / 'hills'
    P, I = hills(n)
    C = np.full(len(I) // 3, 3, np.int32)
    d.mkdir(parents=True, exist_ok=True)
    to_dmesh(P, C, I, d / 'model.dmesh')
    st = Usd.Stage.CreateNew(str(d / 'model.usdc'))
    UsdGeom.SetStageUpAxis(st, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(st, 1.0)
    m = UsdGeom.Mesh.Define(st, '/hills')
    m.CreatePointsAttr(Vt.Vec3fArray.FromNumpy(P))
    m.CreateFaceVertexCountsAttr(Vt.IntArray.FromNumpy(C))
    m.CreateFaceVertexIndicesAttr(Vt.IntArray.FromNumpy(I))
    st.GetRootLayer().Save()
    (d / 'meshes.json').write_text(json.dumps([{'prim': '/hills', 'points': len(P), 'faces': len(C)}]))
    print(f'hills n={n}: {len(P):,} points, {len(C):,} triangles, {perf_counter() - t:.1f} s -> {d}')


def measure(cmd):
    """Run cmd -> (exit code, stderr, wall s, CPU s, peak commit B, peak working set B). Peaks and CPU times come from
    the process handle after exit: exact, no polling. ponytail: Popen._handle is private (open until the Popen is
    collected); a Windows job object replaces it if a Python release drops it."""
    t = perf_counter()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors='replace')
    err = p.communicate()[1]
    wall, pmc, ft = perf_counter() - t, PMC(ctypes.sizeof(PMC)), [ctypes.c_ulonglong() for _ in range(4)]
    if not (K32.K32GetProcessMemoryInfo(p._handle, ctypes.byref(pmc), pmc.cb)
            and K32.GetProcessTimes(p._handle, *map(ctypes.byref, ft))):          # FILETIMEs: start, exit, kernel, user
        raise ctypes.WinError(ctypes.get_last_error())
    return p.returncode, err, wall, (ft[2].value + ft[3].value) / 1e7, pmc.PeakPagefileUsage, pmc.PeakWorkingSetSize


def parse(err):
    """The CLI's stderr -> {name: number} from its -v lines (VLINES)."""
    return {k: float(v) for p in VLINES if (m := re.search(p, err)) for k, v in m.groupdict().items()}


def values(out, ref, tag):
    """out.npy -> {sha256, rows, cols} + vs (tags but base): 'identical' to ref, or the count of differing values, the
    max |diff| and the max relative to max(|ref|, 1e-3 max |ref| of the column)."""
    v = {'sha256': sha(out), **dict(zip(('rows', 'cols'), np.load(out, mmap_mode='r').shape))}
    if tag == 'base' or not ref.is_file():
        return v
    x, y = np.load(ref, mmap_mode='r'), np.load(out, mmap_mode='r')
    if sha(ref) == v['sha256']:
        v['vs'] = 'identical'
    elif x.shape != y.shape:
        v['vs'] = f'{x.shape} -> {y.shape}'
    else:
        d = np.abs(x - y)
        rel = d / np.maximum(np.abs(x), 1e-3 * np.abs(x).max(0)).clip(1e-300)
        v['vs'] = f'{int((x != y).sum())} of {x.size} differ, max abs {d.max():.3g}, max rel {rel.max():.3g}'
    return v


def run(a):
    """Every case of a.cases on R/<mesh>/model.dmesh: a record in R/results.jsonl and a markdown row each."""
    exe, dm, runs = a.exe.resolve(), a.root / a.mesh / 'model.dmesh', a.root / 'runs' / a.tag
    if missing := [str(f) for f in (exe, dm, a.epw) if not f.is_file()]:
        sys.exit(f'error: not found: {", ".join(missing)}')
    build = {'exe': str(exe), 'sha256': sha(exe)}
    runs.mkdir(parents=True, exist_ok=True)
    print('| mesh | case | outcome | ' + ' | '.join(ROW) + ' |\n' + '|---' * (len(ROW) + 3) + '|', flush=True)
    for case in a.cases.split(','):
        out = runs / f'{a.mesh}-{case}.npy'
        out.unlink(missing_ok=True)                    # a failed run must not leave the last run's values behind
        free = MEMSTAT(ctypes.sizeof(MEMSTAT))
        K32.GlobalMemoryStatusEx(ctypes.byref(free))
        smi = subprocess.Popen(SMI, stdout=subprocess.PIPE, text=True)
        if (first := smi.stdout.readline()).count(',') != 3:             # the GPU before the run
            smi.terminate()
            sys.exit(f'error: nvidia-smi: {first.strip()}')
        lines = []
        reader = threading.Thread(target=lambda: lines.extend(smi.stdout), daemon=True)
        reader.start()
        try:
            rc, err, wall, cpu, commit, ws = measure([str(exe), str(dm), '--epw', str(a.epw), *CASES[case], '-v',
                                                      '--out', str(out)])
        finally:
            smi.terminate()                            # a stray sampler would load the GPU of the next run
        reader.join()
        # [busy %, MiB used, deg C, SM MHz]; row 0 before the run; a line cut by terminate() is skipped
        S = np.genfromtxt([first] + lines, delimiter=',', invalid_raise=False, ndmin=2)
        G = S[1:] if len(S) > 1 else S
        busy = G[:, 0] > 0
        rec = {'layer': 'core', 'mesh': a.mesh, 'case': case, 'tag': a.tag, 'ok': rc == 0,
               'error': '' if rc == 0 else f'exit {rc:#x}: ' + (err.strip().splitlines() or [''])[-1],
               'when': time.strftime('%Y-%m-%dT%H:%M:%S'), 'build': build, 'wall_s': wall, 'cpu_s': cpu,
               'cores': cpu / wall, 'cli': parse(err),
               'gpu': {'busy_pct': G[:, 0].mean(), 'mem_used_gb': S[0, 1] / 1024,
                       'mem_rise_gb': (G[:, 1].max() - S[0, 1]) / 1024, 'temp_max_c': G[:, 2].max(),
                       'sm_mhz': G[busy, 3].mean() if busy.any() else 0.0},
               'mem': {'peak_commit_gb': commit / GB, 'peak_ws_gb': ws / GB, 'free_ram_gb': free.free_phys / GB,
                       'free_commit_gb': free.free_commit / GB},
               'values': values(out, a.root / 'runs' / 'base' / out.name, a.tag) if rc == 0 else {},
               'info': [s for s in err.splitlines() if s.startswith(('info:', 'warn:', 'error:'))]}
        with open(a.root / 'results.jsonl', 'a', encoding='utf-8') as f:
            f.write(json.dumps(rec) + '\n')
        leaves = flat(rec)
        print(f'| {a.mesh} | {case} | {outcome(rec)} | ' + ' | '.join(fmt(leaves.get(k)) for k in ROW) + ' |', flush=True)


def flat(r, pre=''):
    """A record's leaves as {dotted path: value}: numbers, bools and strings; lists and SKIP keys left out."""
    out = {}
    for k, v in r.items():
        if isinstance(v, dict):
            out |= flat(v, f'{pre}{k}.')
        elif k not in SKIP and isinstance(v, (int, float, str)):
            out[pre + k] = v
    return out


def fmt(v):
    """A table cell: numbers to 4 significant digits, strings on one line, cut to 80 characters."""
    return '-' if v is None else f'{v:.4g}' if isinstance(v, float) else ' '.join(str(v).split())[:80].replace('|', '/')


def cell(a, b):
    """'a -> b (+x%)' for two numbers, 'a -> b' otherwise, one value when they agree."""
    if a == b:
        return fmt(a)
    pct = f' ({(b - a) / abs(a):+.1%})' if type(a) in (int, float) and type(b) in (int, float) and a else ''
    return f'{fmt(a)} -> {fmt(b)}{pct}'


def outcome(r):
    return None if r is None else 'ok' if r.get('ok') else f"FAIL: {r.get('error') or 'no error text'}"


def table(recs, base, tag):
    """Records -> one markdown table per layer: a column per mesh and case, an outcome row, then a row per leaf of the
    records (flat), cells 'base -> tag (+x%)' or one value where both agree."""
    last = {(r['layer'], r['mesh'], r['case'], r['tag']): r for r in recs}        # appended in time order: latest wins
    out = []
    for layer in dict.fromkeys(['core', 'server', 'chrome', 'firefox'] + [k[0] for k in last]):
        cols = list(dict.fromkeys((m, c) for l, m, c, t in last if l == layer and t in (base, tag)))
        if not cols:
            continue
        pairs = [[last.get((layer, m, c, t)) for t in (base, tag)] for m, c in cols]
        flats = [[flat(r) if r else {} for r in p] for p in pairs]
        out += [f'\n{layer}: {base} -> {tag}\n', '| | ' + ' | '.join(f'{m} {c}' for m, c in cols) + ' |',
                '|---' * (len(cols) + 1) + '|', '| outcome | ' + ' | '.join(cell(*map(outcome, p)) for p in pairs) + ' |']
        out += [f'| {k} | ' + ' | '.join(cell(fa.get(k), fb.get(k)) for fa, fb in flats) + ' |'
                for k in dict.fromkeys(k for fs in flats for f in fs for k in f)]
    return '\n'.join(out)


def selftest():
    # exact peak after exit, on a child allocating 400 MiB: the base interpreter, since a venv's python.exe is a
    # launcher that would allocate in a grandchild
    rc, _, _, cpu, commit, ws = measure([str(Path(sys.base_exec_prefix) / 'python.exe'), '-c', 'b = bytearray(400 << 20)'])
    assert rc == 0 and cpu > 0 and commit >= 400 << 20 and ws >= 400 << 20, (rc, cpu, commit, ws)
    sq = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], float)
    P, C, I = merge([('a', sq[:3], np.array([3], np.int32), np.array([0, 1, 2], np.int32)),
                     ('b', sq, np.array([4], np.int32), np.array([3, 2, 1, 0], np.int32))])
    assert P.shape == (7, 3) and C.tolist() == [3, 4] and I.tolist() == [0, 1, 2, 6, 5, 4, 3] and I.dtype == np.int32
    P, I = hills(2)
    T = P[I.reshape(-1, 3)].astype(float)
    up = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])[:, 2]
    assert P.shape == (9, 3) and P.dtype == np.float32 and len(I) == 24 and (up > 0).all() and 0 <= I.min() <= I.max() < 9
    v = parse('info: read F:/r/model.dmesh: 1 meshes, 8 faces, 9 points, 0.250 s\n'
              'info: 8 sensors, 8 triangles (scene reused), 20 steps (10.0 h)\n'
              'info: GAS 1.5 MB build 12 ms, 3 launches, 1.23e+10 rays in 456 ms (26.97 GRays/s)\n'
              'info: reflected light: 1 bounces, 256 samples per sensor, 2.05e+03 paths in 7 ms (1 launches), a hit '
              'record per slot\ninfo: run 1.25 s total (incl. sky model, GPU, output)\ninfo: output 0.010 s\n')
    assert len(v) == 18 and v['read_s'] == 0.25 and v['steps'] == 20 and v['rays'] == 1.23e10 and v['grays'] == 26.97 \
        and v['paths'] == 2050 and v['bounce_launches'] == 1 and v['run_s'] == 1.25 and v['output_s'] == 0.01, v
    recs = [{'layer': 'core', 'mesh': 'm', 'case': 'S1', 'tag': t, 'ok': t == 'base', 'error': 'exit 0xc0000409',
             'wall_s': w} for t, w in (('base', 2.0), ('new', 9.0), ('new', 1.0))]
    s = table(recs, 'base', 'new')
    assert '| wall_s | 2 -> 1 (-50.0%) |' in s and '| outcome | ok -> FAIL: exit 0xc0000409 |' in s, s
    print(f'selftest ok: child peak commit {commit / 2**20:.0f} MiB, peak working set {ws / 2**20:.0f} MiB, CPU {cpu:.2f} s')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['convert', 'hills', 'run', 'table', 'selftest'])
    ap.add_argument('usd', nargs='?', type=Path, help='convert: the stage; its stem names R/<stem>/')
    ap.add_argument('--root', type=Path, default=HERE / '_out' / 'big', help='bench root R (default _out/big)')
    ap.add_argument('--tris', type=int, default=20_000_000, help='hills: about this many triangles')
    ap.add_argument('--exe', type=Path, help='run: the daylight CLI')
    ap.add_argument('--mesh', default='bhs', help='run: R/<mesh>/model.dmesh')
    ap.add_argument('--cases', default='S1,A6,B1,V1', help=f'run: comma-separated, of {" ".join(CASES)}')
    ap.add_argument('--tag', default='base', help='run: R/runs/<tag>/; table: the new side')
    ap.add_argument('--base', default='base', help='table: the old side')
    ap.add_argument('--epw', type=Path, default=BOSTON, help='run: weather file (default Boston)')
    a = ap.parse_args()
    if a.cmd == 'convert' and not a.usd or a.cmd == 'run' and (not a.exe or set(a.cases.split(',')) - CASES.keys()):
        ap.error(f'convert needs the USD path; run needs --exe and cases of {",".join(CASES)}')
    jsonl = a.root / 'results.jsonl'
    {'convert': lambda: convert(a.usd, a.root), 'hills': lambda: write_hills(a.tris, a.root), 'run': lambda: run(a),
     'table': lambda: print(table([json.loads(s) for s in jsonl.read_text('utf-8').splitlines() if s.strip()], a.base, a.tag)),
     'selftest': selftest}[a.cmd]()


if __name__ == '__main__':
    sys.exit(main())
