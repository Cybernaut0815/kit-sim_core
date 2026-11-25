// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// Flag parity: every flag of the two usage texts in src/cli.cpp is a typed option in options.ts, options.py and
// Options.cs (the shared ones in the common type), no option names a flag the CLI lacks, and every flag is in its
// tool's doc. No GPU, no DLL. Then toArgs() itself.
import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { toArgs } from '../src/options.js';

const read = (path: string) => readFileSync(new URL(`../../../${path}`, import.meta.url), 'utf8').replace(/\r\n/g, '\n');
const BOUND = new Set(['--context', '--out', '--bench', '--help', '-h']); // the runners own these: no option

/** The usage text of `ns` (daylight::cli or daylight::views::cli): its string literals joined, escapes undone. */
function usage(cpp: string, ns: string): string {
  const at = cpp.indexOf('const char* const kUsage =', cpp.indexOf(`namespace ${ns} {`));
  const body = cpp.slice(at, cpp.indexOf('";\n', at) + 2);
  return [...body.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((m) => m[1].replace(/\\n/g, '\n').replace(/\\"/g, '"')).join('');
}
/** Every flag a usage text names: --long ones and the single-letter -v / -h. */
const flagsOf = (text: string) => new Set([...text.matchAll(/(?<![\w-])(--[a-z][a-z0-9-]*|-[a-z])(?![\w-])/g)].map((m) => m[1]));

const cpp = read('src/cli.cpp');
const tools = { daylight: flagsOf(usage(cpp, 'daylight::cli')), views: flagsOf(usage(cpp, 'daylight::views::cli')) };
const bound = (s: Set<string>) => new Set([...s].filter((f) => !BOUND.has(f)));
const want = {
  common: new Set([...bound(tools.daylight)].filter((f) => tools.views.has(f))),
  daylight: new Set([...bound(tools.daylight)].filter((f) => !tools.views.has(f))),
  views: new Set([...bound(tools.views)].filter((f) => !tools.daylight.has(f))),
};

/** options.ts: the flags each interface's fields stand for (toArgs' rule: camelCase -> --kebab-case, verbose -> -v). */
function tsFlags(): Record<keyof typeof want, Set<string>> {
  const src = read('bindings/ts/src/options.ts');
  const block = (name: string) => src.slice(src.indexOf(`export interface ${name} `), src.indexOf('\n}\n', src.indexOf(`export interface ${name} `)));
  const fields = (name: string) => new Set([...block(name).matchAll(/^ {2}(\w+)\?:/gm)].map((m) =>
    m[1] === 'verbose' ? '-v' : `--${m[1].replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`)}`));
  return { common: fields('CommonOptions'), daylight: fields('DaylightOptions'), views: fields('ViewsOptions') };
}

/** options.py: the flags each TypedDict's keys stand for (to_args' rule: snake_case -> --kebab-case, trailing _ off). */
function pyFlags(): Record<keyof typeof want, Set<string>> {
  const src = read('bindings/python/daylight/options.py');
  const block = (name: string) => { const at = src.indexOf(`class ${name}(`), end = src.indexOf('\nclass ', at + 1); return src.slice(at, end < 0 ? undefined : end); };
  const fields = (name: string) => new Set([...block(name).matchAll(/^ {4}(\w+): /gm)].map((m) =>
    m[1] === 'verbose' ? '-v' : `--${m[1].replace(/_$/, '').replace(/_/g, '-')}`));
  return { common: fields('CommonOptions'), daylight: fields('DaylightOptions'), views: fields('ViewsOptions') };
}

/** Options.cs: the flag literals each class's AddTo adds. */
function csFlags(): Record<keyof typeof want, Set<string>> {
  const src = read('bindings/csharp/Options.cs');
  const block = (name: string) => { const at = src.search(new RegExp(` class ${name}\\b`)), end = src.indexOf(' class ', at + 1); return src.slice(at, end); };
  const fields = (name: string) => new Set([...block(name).matchAll(/a\.\w+(?:<\w+>)?\("(-{1,2}[a-z][a-z0-9-]*)"/g)].map((m) => m[1]));
  return { common: fields('CommonOptions'), daylight: fields('DaylightOptions'), views: fields('ViewsOptions') };
}

describe('flag parity with src/cli.cpp', () => {
  it('reads both usage texts', () => {
    expect(tools.daylight.has('--epw') && tools.daylight.has('--bounce-samples') && tools.daylight.has('-v')).toBe(true);
    expect(tools.views.has('--fov') && tools.views.has('--ground-z') && !tools.views.has('--epw')).toBe(true);
  });
  for (const [lang, of] of [['options.ts', tsFlags], ['options.py', pyFlags], ['Options.cs', csFlags]] as const) {
    it(`${lang} declares every flag, in the right type, and no other`, () => {
      const got = of();
      for (const k of ['common', 'daylight', 'views'] as const) expect([...got[k]].sort(), `${lang} ${k}`).toEqual([...want[k]].sort());
    });
  }
  it('every flag is in its tool doc', () => {
    for (const [tool, flags] of Object.entries(tools)) {
      const doc = read(`docs/${tool}.md`);
      expect([...flags].filter((f) => !doc.includes(`\`${f}`)), `docs/${tool}.md`).toEqual([]);
    }
  });
});

describe('toArgs', () => {
  it('turns each kind of field into flags, in field order', () => {
    expect(toArgs({
      epw: 'site.epw', quantity: ['radiation', 'sunhours'], skyDensity: 2, cullBackfaces: true, noSelfOcclude: false,
      virtualGround: false, fresnel: true, lat: 51.5, from: '06-21T08:00', verbose: true, offset: undefined,
    })).toEqual(['--epw', 'site.epw', '--quantity', 'radiation,sunhours', '--sky-density', '2', '--cull-backfaces',
      '--virtual-ground', 'off', '--fresnel', 'on', '--lat', '51.5', '--from', '06-21T08:00', '-v']);
  });
  it('packs optics, targets and names with ;', () => {
    expect(toArgs({ optics: [{ name: '*', rho: 0.5 }, { name: 'Glass', slot: 2, rho: 0, tau: 0.64, kind: 'glass' }, 'Tree=0.2,0.3'] }))
      .toEqual(['--optics', '*=0.5;Glass/2=0,0.64,glass;Tree=0.2,0.3']);
    expect(toArgs({ optics: [{ name: 'Screen', rho: 0.1, kind: 'porous' }] })).toEqual(['--optics', 'Screen=0.1,0,porous']);
    expect(toArgs({ target: [{ name: 'park', group: 'green' }, 'lake=blue'], noOcclude: ['a', 'b'], fov: [360, 0, 0], reach: 'en17037', groundZ: -1 }))
      .toEqual(['--target', 'park=green;lake=blue', '--no-occlude', 'a;b', '--fov', '360,0,0', '--reach', 'en17037', '--ground-z', '-1']);
  });
  it('leaves out empty lists and refuses a name with ;', () => {
    expect(toArgs({ quantity: [], noOcclude: [], distance: [] })).toEqual([]);
    expect(() => toArgs({ noOcclude: ['a;b'] })).toThrow(/--no-occlude: 'a;b' holds a ';'/);
  });
});
