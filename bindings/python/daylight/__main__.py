# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
"""python -m daylight STAGE [--sensors PREFIX]... [--context PREFIX]... [--time T] [--tool daylight|views] -o DIR
                      [--write-usd PATH] [-- <core flags>]

Writes DIR/<stem>.dmesh per mesh (stem = prim path with [^A-Za-z0-9._-] -> '_', '/World/lake' -> '_World_lake', made
unique with _2, _3 ...: the TypeScript binding's dmeshStems) and DIR/meshes.json = [{"file", "prim", "role": "sensor"|"context"}] in core order (sensors
first); with core flags after `--` also runs the core's --tool (daylight.exe or views.exe) -> DIR/values.npy(.json),
rows in meshes.json order, and --write-usd PATH puts them back on the sensor prims as a sublayer file (primvars
<tool>_<column> + displayColor, see usd.write_results)."""
import argparse
import json
import sys
from pathlib import Path

from .core import TOOLS, _stem, run, to_dmesh
from .usd import meshes, split, write_results


def main(argv):
    """The command line above on `argv` (without the program name): convert, write DIR, and run / write back when core
    flags follow `--`. Exits with a message for no sensor mesh or --write-usd without core flags; a failed core run
    raises subprocess.CalledProcessError."""
    core = []
    if '--' in argv:
        i = argv.index('--')
        argv, core = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(prog='python -m daylight', description=__doc__.split('\n')[3])
    ap.add_argument('stage')
    ap.add_argument('--sensors', action='append', default=[], metavar='PREFIX', help='prim path prefix of sensor meshes (repeatable)')
    ap.add_argument('--context', action='append', default=[], metavar='PREFIX', help='prim path prefix of blocking-only meshes (repeatable)')
    ap.add_argument('--time', type=float, help='USD time code (default: the stage start time)')
    ap.add_argument('--tool', choices=TOOLS, default='daylight', help='the tool the flags after -- run (default daylight)')
    ap.add_argument('-o', '--out', required=True, metavar='DIR')
    ap.add_argument('--write-usd', metavar='PATH', help='.usda/.usdc sublayer with the values as primvars + displayColor (needs core flags after --)')
    a = ap.parse_args(argv)
    if a.write_usd and not core:
        sys.exit('error: --write-usd needs core flags after -- (nothing to write without a run)')
    d = Path(a.out)
    d.mkdir(parents=True, exist_ok=True)
    sens, ctx = split(meshes(a.stage, a.time), a.sensors, a.context)
    if not sens:
        sys.exit('error: no sensor mesh (check --sensors against the prim paths; invisible/guide/inactive prims are dropped)')
    files, index, used = {'sensor': [], 'context': []}, [], set()
    for role, ms in ('sensor', sens), ('context', ctx):
        for name, *arrays in ms:
            stem = _stem(name, used)
            to_dmesh(*arrays, d / f'{stem}.dmesh')
            files[role].append(d / f'{stem}.dmesh')
            index.append({'file': f'{stem}.dmesh', 'prim': name, 'role': role})
    (d / 'meshes.json').write_text(json.dumps(index, indent=1))
    msg = f'{len(sens)} sensor and {len(ctx)} context meshes -> {d}'
    if core:
        values, man = run(files['sensor'], files['context'], args=core, out=d / 'values.npy', tool=a.tool)
        msg += f' + {d / "values.npy"}'
        if a.write_usd:
            write_results(a.stage, values, man, [x['prim'] for x in index if x['role'] == 'sensor'], a.write_usd,
                          on='vertices' if '--on vertices' in ' '.join(core) else 'faces', name=a.tool)
            msg += f' + {a.write_usd}'
    print(msg)


if __name__ == '__main__':
    main(sys.argv[1:])
