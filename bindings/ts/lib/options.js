// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// Typed run options: every flag of the daylight and views CLIs as a field (docs/daylight.md, docs/views.md), and
// toArgs(), which turns them into those flags. run(), native run() and runRemote() take these or raw flags.
// Not here: mesh files and --context (the runners' sensors / context), --out (the runners read the values), --bench
// and -h (no run). test/flags.test.ts keeps this file, options.py, Options.cs and the docs in step with src/cli.cpp.
const ON_OFF = new Set(['virtualGround', 'transmission', 'fresnel', 'jitter']);
/** Names joined by `;` (the CLI's packing); a name holding `;` cannot be addressed, so it throws. */
function packed(flag, items) {
    for (const s of items)
        if (s.includes(';'))
            throw new Error(`${flag}: '${s}' holds a ';', which cannot be addressed`);
    return items.join(';');
}
/** `NAME[/SLOT]=RHO[,TAU[,KIND]]` of an OpticsSpec (a kind needs a tau: 0 when not given). */
function opticsSpec(o) {
    if (typeof o === 'string')
        return o;
    const slot = o.slot === undefined ? '' : `/${o.slot}`;
    const tau = o.tau === undefined && !o.kind ? '' : `,${o.tau ?? 0}`;
    return `${o.name}${slot}=${o.rho}${tau}${o.kind ? `,${o.kind}` : ''}`;
}
/**
 * Typed options -> CLI flags, in field order: camelCase becomes `--kebab-case` (`verbose` is `-v`), true adds a bare
 * flag (false adds none), the on/off flags (virtualGround, transmission, fresnel, jitter) take `on` / `off`, arrays are
 * comma-joined (optics, target and noOcclude `;`-joined), undefined, null and empty arrays are left out. The values are
 * not checked here: the core refuses a bad one with its usage message (DaylightError 1, HTTP 400).
 */
export function toArgs(options) {
    const args = [];
    for (const [key, value] of Object.entries(options)) {
        if (value === undefined || value === null || (Array.isArray(value) && !value.length))
            continue;
        const flag = key === 'verbose' ? '-v' : `--${key.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`)}`;
        if (typeof value === 'boolean') {
            if (ON_OFF.has(key))
                args.push(flag, value ? 'on' : 'off');
            else if (value)
                args.push(flag);
        }
        else if (key === 'optics')
            args.push(flag, packed(flag, value.map(opticsSpec)));
        else if (key === 'target')
            args.push(flag, packed(flag, value.map((t) => (typeof t === 'string' ? t : `${t.name}=${t.group}`))));
        else if (key === 'noOcclude')
            args.push(flag, packed(flag, value));
        else
            args.push(flag, Array.isArray(value) ? value.join(',') : String(value));
    }
    return args;
}
/** Raw flags as they are, typed options through toArgs(). */
export const argsOf = (args) => (!args ? [] : Array.isArray(args) ? [...args] : toArgs(args));
