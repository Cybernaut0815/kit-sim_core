# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""Typed run options: every flag of the daylight and views CLIs as a key (docs/daylight.md, docs/views.md), and
`to_args`, which turns them into those flags. `run`, `run_meshes` and `native.run` take such a dict or raw flags::

    from daylight import run
    from daylight.options import DaylightOptions
    opts: DaylightOptions = {'epw': 'site.epw', 'quantity': ['radiation', 'sunhours'], 'sky_density': 2}
    values, manifest = run('facade.obj', args=opts)

Not here: mesh files and --context (the runners' sensors / context), --out (the runners read the values), --bench and
-h (no run). bindings/ts/test/flags.test.ts keeps this file, options.ts, Options.cs and the docs in step with
src/cli.cpp."""
import os
from collections.abc import Mapping
from typing import List, Literal, Tuple, TypedDict, Union

__all__ = ['Optics', 'CommonOptions', 'DaylightOptions', 'ViewsOptions', 'to_args', 'args_of']


class _OpticsName(TypedDict):
    name: str
    """Core mesh name (.dmesh stem, OBJ group, USD prim path), or '*' for every mesh."""
    rho: float
    """Diffuse reflectance, 0..1."""


class Optics(_OpticsName, total=False):
    """One --optics SPEC, NAME[/SLOT]=RHO[,TAU[,glass|porous]]: the numbers of a mesh, or of one face slot of it."""
    slot: int
    """Face slot (0..65535) instead of the whole mesh."""
    tau: float
    """Transmittance at normal incidence, rho + tau <= 1 (default 0)."""
    kind: Literal['glass', 'porous']
    """glass (Radiance glass, n 1.52: rho 0, 0 < tau <= 0.918) or porous (tau at every angle); default porous when tau > 0."""


class CommonOptions(TypedDict, total=False):
    """Flags both tools take."""
    on: Literal['faces', 'vertices']
    """--on: a row per face (at its centroid) or per vertex. Default faces."""
    sides: Literal['front', 'back', 'both']
    """--sides: the receiving / viewing side(s); both: two columns per value. Default front."""
    offset: float
    """--offset M: sensors / eyes moved M metres along their normal. Default 0."""
    cull_backfaces: bool
    """--cull-backfaces: occluders block only from their front side."""
    no_self_occlude: bool
    """--no-self-occlude: sensor meshes cast no shade, block no rays (only context meshes do)."""
    no_occlude: List[str]
    """--no-occlude NAME;...: these sensor meshes are analysed but cast no shade, block no rays."""
    north: float
    """--north DEG: model north, degrees counterclockwise from +Y (90 = west). Default 0."""
    virtual_ground: bool
    """--virtual-ground on|off: a ground plane below the meshes (daylight: reflecting `ground`; views: ends downward
    rays). Default on."""
    optics: List[Union[Optics, str]]
    """--optics SPEC;...: optics per mesh or face slot (a str is one SPEC as the CLI takes it). Default rho 0.2, tau 0."""
    transmission: bool
    """--transmission on|off: light (daylight) or rays (views) through glass and porous slots. Default off."""
    verbose: bool
    """-v: timings and sizes on stderr."""


class DaylightOptions(CommonOptions, total=False):
    """daylight's flags (docs/daylight.md): site, weather and time, quantities, sky, materials, output. `from_` is
    --from (`from` is a Python keyword)."""
    epw: Union[str, os.PathLike]
    """--epw FILE: hourly weather and location."""
    clear_sky: bool
    """--clear-sky: a synthetic cloudless year (Ineichen) at lat / lon instead of an EPW."""
    turbidity: float
    """--turbidity TL: Linke turbidity for clear_sky (2 very clear .. 6-7 polluted). Default 3."""
    altitude: float
    """--altitude M: site altitude in metres for clear_sky. Default 0."""
    lat: float
    """--lat DEG: latitude, north positive; needed without an EPW, else overrides its location."""
    lon: float
    """--lon DEG: longitude, east positive."""
    tz: float
    """--tz H: the clock of the times, standard time (default: the EPW's zone, else round(lon / 15))."""
    time: str
    """--time [YYYY-]MM-DDTHH:MM: one instant (default: annual, every EPW row)."""
    from_: str
    """--from TS: start of the range [from, to)."""
    to: str
    """--to TS: end of the range [from, to)."""
    step: int
    """--step MIN: minutes per step of the range. Default 60."""
    hours: str
    """--hours HH:MM-HH:MM: with from_ / to, only these hours of every day."""
    quantity: List[Literal['radiation', 'direct', 'diffuse', 'irradiance', 'illuminance', 'sunhours']]
    """--quantity Q,...: a column each (radiation kWh/m2, direct, diffuse, irradiance W/m2, illuminance lux,
    sunhours h). Default radiation."""
    mode: Literal['physical', 'ladybug']
    """--mode: sky and sun model. Default physical."""
    sky_density: Literal[1, 2, 4]
    """--sky-density: 145 / 577 / 2305 sky patches. Default 1."""
    patch_samples: int
    """--patch-samples K: physical mode, K stratified rays per sky patch, a square number. Default 1."""
    ground: float
    """--ground RHO: ground reflectance 0..1. Default 0.2."""
    fresnel: bool
    """--fresnel on|off: glass by angle (off: tau at every angle). Default on."""
    bounces: int
    """--bounces N: up to N = 0..20 diffuse reflections (physical mode). Default 0."""
    bounce_samples: int
    """--bounce-samples M: paths per sensor for bounces, 64..65536. Default 1024."""
    timeseries: Union[str, os.PathLike]
    """--timeseries F.npy: per-step values, float32 [N x T], and F.npy.hoys.txt."""
    dump_bits: Union[str, os.PathLike]
    """--dump-bits F.npy: visibility per sensor and direction (validation)."""
    dump_sky: Union[str, os.PathLike]
    """--dump-sky F: the gendaymtx sky blocks (ladybug mode; validation)."""
    dump_sun: Union[str, os.PathLike]
    """--dump-sun F: per step date, sun positions, DNI and DHI (validation)."""


class ViewsOptions(CommonOptions, total=False):
    """views' flags (docs/views.md): field of view, distances, targets, ground and glass."""
    target: Union[Mapping[str, str], List[Union[Tuple[str, str], str]]]
    """--target NAME=GROUP;...: rays ending on a mesh count for its group, a share column per group (at most 16); a
    mapping or (name, group) pairs in column order."""
    fov: Union[float, Tuple[float, float, float]]
    """--fov H[,LO,HI]: H degrees wide about the normal (up to 180) or 360 all around, elevation LO..HI.
    Default (120, -70, 50)."""
    gaze: Literal['normal', 'level']
    """--gaze: the field about the normal, or about the normal turned level (as a person looks). Default normal."""
    cell: float
    """--cell DEG: degrees per ray, rows and columns. Default 2."""
    jitter: bool
    """--jitter on|off: each ray at a random point of its cell (off: the cell centres). Default on."""
    distance: List[Literal['isovist', 'mean', 'min', 'group', 'none']]
    """--distance STAT,...: isovist, mean, min, group or none (default: isovist with fov 360, else mean,min)."""
    far: float
    """--far M: what open counts as, and the cap of every distance. Default 1000."""
    reach: Union[List[float], Literal['en17037']]
    """--reach D,...|en17037: share of the field that sees at least D metres (at most 8; en17037 = 6,20,50)."""
    ground_z: float
    """--ground-z Z: height of the virtual ground in metres (default: the lowest point of the meshes)."""
    dump_rays: Union[str, os.PathLike]
    """--dump-rays F.npy: classes uint8 [rows x rays] and F.npy.depth.npy."""


_ON_OFF = {'virtual_ground', 'transmission', 'fresnel', 'jitter'}


def _packed(flag, items):
    """Names joined by ';' (the CLI's packing); ValueError for a name holding ';', which cannot be addressed."""
    items = list(items)
    for s in items:
        if ';' in s:
            raise ValueError(f"{flag}: {s!r} holds a ';', which cannot be addressed")
    return ';'.join(items)


def _optics(o):
    """NAME[/SLOT]=RHO[,TAU[,KIND]] of an Optics dict (a kind needs a tau: 0 when not given); a str as it is."""
    if isinstance(o, str):
        return o
    slot = f"/{o['slot']}" if 'slot' in o else ''
    tau = f",{o.get('tau', 0)}" if 'tau' in o or 'kind' in o else ''
    return f"{o['name']}{slot}={o['rho']}{tau}{',' + o['kind'] if 'kind' in o else ''}"


def to_args(options):
    """Typed options -> CLI flags, in key order: snake_case becomes --kebab-case (a trailing '_' dropped, `verbose` is
    -v), True adds a bare flag (False adds none), the on/off flags (virtual_ground, transmission, fresnel, jitter) take
    'on' / 'off', lists and tuples are comma-joined (optics, target and no_occlude ';'-joined), None and empty lists
    are left out. Paths stay os.PathLike (native.run sends them in the narrow encoding). Values are not checked here:
    the core refuses a bad one with its usage message (DaylightError 1)."""
    args = []
    for key, value in options.items():
        if value is None or (isinstance(value, (list, tuple, Mapping)) and not value):
            continue
        flag = '-v' if key == 'verbose' else '--' + key.rstrip('_').replace('_', '-')
        if isinstance(value, bool):
            args += [flag, 'on' if value else 'off'] if key in _ON_OFF else [flag] if value else []
        elif key == 'optics':
            args += [flag, _packed(flag, map(_optics, value))]
        elif key == 'target':
            pairs = value.items() if isinstance(value, Mapping) else value
            args += [flag, _packed(flag, (t if isinstance(t, str) else f'{t[0]}={t[1]}' for t in pairs))]
        elif key == 'no_occlude':
            args += [flag, _packed(flag, value)]
        elif isinstance(value, (list, tuple)):
            args += [flag, ','.join(str(v) for v in value)]
        else:
            args += [flag, value if isinstance(value, os.PathLike) else str(value)]
    return args


def args_of(args):
    """Raw flags (a sequence of str / os.PathLike) as a list, a dict of typed options through to_args."""
    return to_args(args) if isinstance(args, Mapping) else list(args)
