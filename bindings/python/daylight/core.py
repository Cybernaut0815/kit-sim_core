# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""Run the core's CLIs (daylight.exe, views.exe) on mesh files or raw arrays (.dmesh) and load values + manifest."""
import json
import os
import re
import struct
import subprocess
import tempfile
from pathlib import Path

import numpy as np

from .options import args_of

REPO = Path(__file__).resolve().parents[3]
TOOLS = ('daylight', 'views')                                    # index = the C ABI's tool id (DL_DAYLIGHT, DL_VIEWS)


def _tool(tool):
    """The tool's C ABI id; ValueError unless 'daylight' or 'views'."""
    if tool not in TOOLS:
        raise ValueError(f"tool {tool!r}: 'daylight' or 'views'")
    return TOOLS.index(tool)


def find_exe(tool='daylight'):
    """DAYLIGHT_EXE / VIEWS_EXE if set, else <repo>/build/RelWithDebInfo/<tool>.exe."""
    _tool(tool)
    return os.environ.get(f'{tool.upper()}_EXE') or str(REPO / 'build' / 'RelWithDebInfo' / f'{tool}.exe')


def _paths(p):
    return [] if p is None else [str(p)] if isinstance(p, (str, os.PathLike)) else [str(q) for q in p]


def run(sensors, context=(), epw=None, args=(), out=None, tool='daylight'):
    """`sensors` / `context`: a mesh file (.obj / .dmesh) or an iterable of them; sensors are analysed and block,
    context blocks only. -> (values [N x C] float64, manifest dict). `args` are further flags of `tool` ('daylight' or
    'views'; `epw` is daylight's), raw or as a DaylightOptions / ViewsOptions dict (options.to_args); `out` keeps the
    value file (`.npy` = binary, default; any other suffix = text)."""
    with tempfile.TemporaryDirectory() as d:
        out = Path(out or Path(d) / 'values.npy')
        cmd = [find_exe(tool), *_paths(sensors)] + [a for c in _paths(context) for a in ('--context', c)] \
            + (['--epw', str(epw)] if epw else []) + [str(a) for a in args_of(args)] + ['--out', str(out)]
        subprocess.run(cmd, check=True)
        values = np.load(out) if out.suffix == '.npy' else np.loadtxt(out, ndmin=2)
        return values, json.loads(Path(f'{out}.json').read_text())


VIRIDIS = np.array([[0.267004, 0.004874, 0.329415], [0.278826, 0.175490, 0.483397], [0.229739, 0.322361, 0.545706],
                    [0.172719, 0.448791, 0.557885], [0.127568, 0.566949, 0.550556], [0.157851, 0.683765, 0.501686],
                    [0.369214, 0.788888, 0.382914], [0.678489, 0.863742, 0.189503], [0.993248, 0.906157, 0.143936]])


def colormap(values, vmin=None, vmax=None):
    """values -> float64 [N, 3] RGB in 0..1 (viridis, 9 stops, linear between them); vmin/vmax default to the finite
    min/max (vmax == vmin -> first stop); non-finite values -> 0.5 grey. Same table as the TypeScript binding."""
    v = np.asarray(values, float).ravel()
    ok = np.isfinite(v)
    lo = v[ok].min() if vmin is None and ok.any() else vmin or 0.0
    hi = v[ok].max() if vmax is None and ok.any() else vmax or 0.0
    t = np.where(ok, np.clip((v - lo) / (hi - lo), 0, 1), 0) if hi != lo else np.zeros(len(v))
    i = np.minimum(np.floor(t * 8).astype(int), 7)
    rgb = VIRIDIS[i] + (t * 8 - i)[:, None] * (VIRIDIS[i + 1] - VIRIDIS[i])
    rgb[~ok] = 0.5
    return rgb


def to_dmesh(points, counts, indices, path):
    """One mesh -> .dmesh (little-endian): 'DLM1', uint32 0, uint64 n_points, n_faces, n_indices, then float64
    xyz[3 * n_points] (world metres, Z-up), int32 counts[n_faces] (>= 3, right-handed winding), int32 indices
    [n_indices] (0-based, sum(counts) == n_indices). Mesh name = file stem."""
    p = np.asarray(points, '<f8').reshape(-1, 3)
    c, i = np.asarray(counts, '<i4').ravel(), np.asarray(indices, '<i4').ravel()
    if c.sum() != len(i) or (c < 3).any() or (len(i) and (i.min() < 0 or i.max() >= len(p))):
        raise ValueError('bad mesh: need sum(counts) == len(indices), counts >= 3, 0 <= indices < n_points')
    with open(path, 'wb') as f:
        f.write(struct.pack('<4sIQQQ', b'DLM1', 0, len(p), len(c), len(i)))
        for a in (p, c, i):
            a.tofile(f)


def _stem(name, used):
    """File stem of a mesh name: [^A-Za-z0-9._-] -> '_', leading '_' stripped, made unique with _2, _3 ... against
    `used` (lower-case stems, updated: NTFS file names are case-insensitive)."""
    stem = base = re.sub('[^A-Za-z0-9._-]', '_', name).lstrip('_')
    k = 1
    while stem.lower() in used:
        k += 1
        stem = f'{base}_{k}'
    used.add(stem.lower())
    return stem


def _named(ms):
    """-> [(name or None, points, counts, indices)] from one mesh or an iterable of them, each a (points, counts,
    indices) triple or a (name, points, counts, indices) tuple as usd.meshes() gives."""
    ms = list(ms)
    if ms and (isinstance(ms[0], str) or (len(ms) == 3 and np.ndim(ms[0][0]) == 1)):   # a single mesh?
        ms = [ms]
    return [tuple(m) if len(m) == 4 else (None, *m) for m in ms]


def run_meshes(sensors, context=(), **kw):
    """Raw arrays: `sensors` / `context` are iterables of (points [N,3] world metres Z-up, counts [F], 0-based flat
    indices) triples or (name, points, counts, indices) tuples as usd.meshes() gives (a single one is accepted too).
    Written as <stem>.dmesh and passed to `run(**kw)`; the stems name the meshes in the manifest and in flags such as
    views' --target NAME=GROUP: a name made a file stem as python -m daylight does ('/World/lake' -> 'World_lake'),
    sensor_<i> / context_<i> for a triple."""
    with tempfile.TemporaryDirectory() as d:
        files, used = [], set()
        for kind, ms in ('sensor', _named(sensors)), ('context', _named(context)):
            files.append([])
            for i, (name, *arrays) in enumerate(ms):
                files[-1].append(Path(d) / f'{_stem(name or f"{kind}_{i}", used)}.dmesh')
                to_dmesh(*arrays, files[-1][-1])
        return run(*files, **kw)
