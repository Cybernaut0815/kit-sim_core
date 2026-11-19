# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""The daylight and views tools in-process through daylight_c.dll (docs/c-abi.md): the CLIs' own flags, meshes as
handles instead of files, the values and manifest the CLI would write, and the GPU scene kept resident across runs over
the same Mesh objects::

    from daylight.native import Mesh, run
    with Mesh.load('tests/box.obj') as box:
        r = run('daylight', ['--epw', 'site.epw', '--quantity', 'radiation'], box)
        v = run('views', ['--fov', '360', '--offset', '1.6'], box)          # v.stats['scene_reused'] is True
    print(r.manifest['columns'], r.values.shape)

The DLL loads on first use (DAYLIGHT_DLL, next to the package, or <repo>/build/RelWithDebInfo). A run releases the GIL:
other threads keep running and can cancel it or read its progress through a Control."""
import json
import os
import weakref
from ctypes import POINTER, byref, c_char_p, c_double, c_int32, c_uint16, c_void_p
from typing import NamedTuple

import numpy as np

from ._native import RUNTIME, STATS, USAGE, DaylightError, check, lib, text
from ._native import path as _path
from .core import _tool

__all__ = ['Control', 'DaylightError', 'Mesh', 'Result', 'run', 'usage']

_STATS = (('scene_reused', bool), ('prep_ms', float), ('sky_ms', float), ('build_ms', float), ('trace_ms', float),
          ('reduce_ms', float), ('bounce_ms', float), ('rays', int), ('launches', int), ('gas_bytes', int))


def _ptr(a, t):
    return a.ctypes.data_as(POINTER(t))


class _Handle:
    """Owns one core handle: freed by close(), at the end of a with block or at garbage collection."""

    def _own(self, handle, free, status):
        if not handle:
            check(status)
        self._h = handle
        self._free = weakref.finalize(self, free, handle)
        self._free.atexit = False      # not at exit, where a daemon thread's run may still use it; the exit frees it

    def close(self):
        """Free the handle now (idempotent); never while a run on another thread uses it."""
        self._free()
        self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class Mesh(_Handle):
    """Meshes the core holds (dl_mesh), copied once, never changed: runs over the same Mesh objects in the same roles
    and order reuse the GPU scene. `names`: what the manifest, --target, --optics and --no-occlude call them (an OBJ's
    groups; a Mesh from arrays holds one mesh, or none without faces)."""

    def __init__(self, name, points, counts, indices, slots=None):
        """One mesh from the .dmesh arrays: points [N, 3] world metres Z up, counts [F] (>= 3), 0-based indices
        [sum(counts)] with right-handed winding, slots [F] the optics slot per face (--optics NAME/SLOT; None: 0).
        DaylightError (status 1) for a bad count or index."""
        p = np.ascontiguousarray(points, np.float64).reshape(-1, 3)
        c, i = (np.ascontiguousarray(a, np.int32).ravel() for a in (counts, indices))
        s = None if slots is None else np.ascontiguousarray(slots, np.uint16).ravel()
        if s is not None and len(s) != len(c):                         # the core reads one slot per face
            raise ValueError(f'{name}: {len(s)} slots for {len(c)} faces')
        self._take(lib().dl_mesh_create(name.encode(), _ptr(p, c_double), len(p), _ptr(c, c_int32), len(c),
                                        _ptr(i, c_int32), len(i), None if s is None else _ptr(s, c_uint16)), USAGE)

    @classmethod
    def load(cls, path):
        """The meshes of a .obj (one per o/g group, named by it) or .dmesh file (one, named by the file stem), read as
        the CLI reads them. DaylightError (status 2) when the file does not load."""
        m = cls.__new__(cls)
        m._take(lib().dl_mesh_load(_path(path)), RUNTIME)
        return m

    def _take(self, handle, status):
        self._own(handle, lib().dl_mesh_free, status)
        self.names = tuple(text(lib().dl_mesh_name(handle, k)) for k in range(lib().dl_mesh_count(handle)))


class Control(_Handle):
    """Cancel flag and progress of one run (dl_control): pass it to run() and call cancel() or read progress from
    another thread. One control per run: the flag stays set."""

    def __init__(self):
        self._own(lib().dl_control_create(), lib().dl_control_free, RUNTIME)

    def cancel(self):
        """The run stops at its next check and raises DaylightError(2, 'cancelled'); one not started yet fails as it
        starts."""
        lib().dl_control_cancel(self._h)

    @property
    def progress(self):
        """The share of sensors traced so far, 0..1 (after each trace chunk)."""
        return lib().dl_control_progress(self._h)


class Result(NamedTuple):
    """One run's output: what the CLI's --out FILE.npy and FILE.json hold."""

    values: np.ndarray
    """[N x C] float64: rows = the sensor meshes' elements (faces, or points with --on vertices) in input order,
    columns = manifest['columns']."""
    manifest: dict
    """The CLI's FILE.json (columns, meshes with their row ranges, ...)."""
    stats: dict
    """scene_reused (a resident GPU scene served the run), prep_ms, sky_ms, build_ms, trace_ms, reduce_ms, bounce_ms,
    rays, launches, gas_bytes."""


def usage(tool):
    """The usage text of 'daylight' or 'views', as its CLI's -h prints it."""
    return text(lib().dl_usage(_tool(tool)))


def run(tool, args, sensors, context=(), control=None):
    """One run of `tool`, 'daylight' (docs/daylight.md) or 'views' (docs/views.md): `args` its CLI flags (str as UTF-8, an
    os.PathLike in the narrow encoding the core opens files with; mesh files, --context, --out, --bench and -h are
    refused), `sensors` / `context` a Mesh or an iterable of them in place of the CLI's mesh files and --context files,
    `control` None or a Control. -> Result, values and manifest bit for bit the CLI's (numbers read and written with
    '.' whatever C locale is set). DaylightError: status 1 for bad flags or names the meshes do not resolve, 2 when the
    run fails (a file, the GPU, 'cancelled'). Runs take turns (one GPU run per process); each releases the GIL."""
    values, manifest, stats = _run(tool, args, sensors, context, control)
    return Result(values, json.loads(manifest), stats)


def _run(tool, args, sensors, context, control):
    """run() with the manifest as the core's text (check.py compares it with the CLI's FILE.json byte for byte)."""
    t = _tool(tool)
    argv = [_path(a) if isinstance(a, os.PathLike) else str(a).encode() for a in args]
    sens, ctx = _meshes(sensors), _meshes(context)               # referenced until dl_run returns, so never freed in it
    dll, out = lib(), c_void_p()
    check(dll.dl_run(t, len(argv), (c_char_p * len(argv))(*argv), _handles(sens), len(sens), _handles(ctx), len(ctx),
                     control._h if control else None, byref(out)))
    try:
        values, st = np.empty((dll.dl_result_rows(out), dll.dl_result_cols(out))), np.empty(STATS)
        check(dll.dl_result_values(out, _ptr(values, c_double), values.size))
        check(dll.dl_result_stats(out, _ptr(st, c_double)))
        return values, text(dll.dl_result_manifest(out)), {k: f(x) for (k, f), x in zip(_STATS, st.tolist())}
    finally:
        dll.dl_result_free(out)


def _meshes(ms):
    ms = [ms] if isinstance(ms, Mesh) else list(ms)
    if not all(isinstance(m, Mesh) for m in ms):
        raise TypeError('sensors / context: a Mesh or Mesh objects (Mesh(...), Mesh.load(path))')
    return ms


def _handles(ms):
    return (c_void_p * len(ms))(*[m._h for m in ms]) if ms else None
