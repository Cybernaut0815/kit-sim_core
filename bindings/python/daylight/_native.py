# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""Internal: ctypes loader of daylight_c.dll, the core's C ABI (src/daylight_c.h, docs/c-abi.md). The DLL loads on first
use, by absolute path: DAYLIGHT_DLL, else next to this package, else <repo>/build/RelWithDebInfo. A failed call raises
DaylightError with the CLI's exit status and the core's message."""
import ctypes
import functools
import os
from ctypes import POINTER, c_char_p, c_double, c_float, c_int32, c_int64, c_uint16, c_void_p
from pathlib import Path

from .core import REPO

ABI_VERSION, USAGE, RUNTIME, STATS = 1, 1, 2, 10              # DL_ABI_VERSION, DL_USAGE, DL_RUNTIME, DL_STATS
NAME = 'daylight_c.dll'
NARROW = 'mbcs' if os.name == 'nt' else 'utf-8'               # the C runtime's narrow encoding (ANSI): the core's paths
H = c_void_p                                                  # dl_mesh*, dl_control*, dl_result*: Python int or None

# Every function gets argtypes / restype: ctypes would otherwise pass and return C ints (truncated pointers, int64s).
_SIGNATURES = {
    'dl_version': ([], c_int32),
    'dl_last_error': ([], c_char_p),
    'dl_usage': ([c_int32], c_char_p),
    'dl_mesh_create': ([c_char_p, POINTER(c_double), c_int64, POINTER(c_int32), c_int64, POINTER(c_int32), c_int64,
                        POINTER(c_uint16)], H),
    'dl_mesh_load': ([c_char_p], H),
    'dl_mesh_count': ([H], c_int32),
    'dl_mesh_name': ([H, c_int32], c_char_p),
    'dl_mesh_free': ([H], None),
    'dl_control_create': ([], H),
    'dl_control_cancel': ([H], None),
    'dl_control_progress': ([H], c_double),
    'dl_control_free': ([H], None),
    'dl_run': ([c_int32, c_int32, POINTER(c_char_p), POINTER(H), c_int32, POINTER(H), c_int32, H, POINTER(H)], c_int32),
    'dl_result_rows': ([H], c_int64),
    'dl_result_cols': ([H], c_int64),
    'dl_result_manifest': ([H], c_char_p),
    'dl_result_values': ([H, POINTER(c_double), c_int64], c_int32),
    'dl_result_values_f32': ([H, POINTER(c_float), c_int64], c_int32),
    'dl_result_stats': ([H, POINTER(c_double)], c_int32),
    'dl_result_free': ([H], None),
}


class DaylightError(Exception):
    """A failed call: `status` is the CLI's exit code (1 usage: flags, arguments, names; 2 runtime: a file, the GPU,
    out of memory, 'cancelled'), the message the core's."""

    def __init__(self, status, message):
        super().__init__(message)
        self.status = status

    def __reduce__(self):      # pickled from both, else a process pool loses it (BrokenProcessPool, a hung Pool)
        return type(self), (self.status, *self.args), self.__dict__


@functools.cache
def lib():
    """The DLL with every signature set and its ABI version checked. FileNotFoundError when it, or a DLL it imports (the
    VC++ runtime; the NVIDIA driver loads at the first run), is missing; OSError naming the file for another ABI
    version or not a DLL."""
    env, here = os.environ.get('DAYLIGHT_DLL'), Path(__file__).resolve().parent / NAME
    path = Path(env).resolve() if env else here if here.exists() else REPO / 'build' / 'RelWithDebInfo' / NAME
    if not path.exists():
        raise FileNotFoundError(f'{path} not found: set DAYLIGHT_DLL, put {NAME} next to the package or build the core')
    # ctypes.CDLL (not PyDLL) releases the GIL during every foreign call: while dl_run traces, other threads run, e.g.
    # one that cancels the run or reads its progress (native.Control).
    try:
        dll = ctypes.CDLL(str(path))
        dll.dl_version.restype = c_int32
        version = dll.dl_version()
    except FileNotFoundError:
        raise                                                 # ctypes names the DLL: it, or a DLL it needs, is missing
    except (OSError, AttributeError) as e:                    # not a DLL, 32-bit, no dl_version (WinError 193 says %1)
        raise OSError(f'{path}: not a loadable {NAME} ({e})') from None
    if version != ABI_VERSION:                                # before the signatures: another ABI may lack a function
        raise OSError(f'{path}: C ABI {version}, this binding expects {ABI_VERSION} (rebuild one of them)')
    for name, (args, res) in _SIGNATURES.items():
        f = getattr(dll, name)
        f.argtypes, f.restype = args, res
    return dll


def path(p):
    """A file path (str or os.PathLike) as the core opens it: bytes in the narrow encoding, as the CLI's argv (a
    character outside it becomes '?', which no file name holds: the core fails to open it, as the CLI)."""
    return os.fsdecode(p).encode(NARROW, 'replace')


def text(b):
    """A C string of the core as str: UTF-8 (names, flags), else the narrow encoding (a path it echoes); None stays
    None."""
    try:
        return None if b is None else b.decode('utf-8')
    except UnicodeDecodeError:
        return b.decode(NARROW, 'replace')


def check(status):
    """Raise DaylightError(status, the core's message) unless status is 0 (DL_OK); call it right after the failing call,
    on its thread (dl_last_error is per thread)."""
    if status:
        raise DaylightError(status, text(lib().dl_last_error()))
