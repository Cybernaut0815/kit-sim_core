// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// lib/ is committed (plain JavaScript and .d.ts for JS and TS users): it must be exactly what `npm run build` emits.
import { readdirSync, readFileSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import ts from 'typescript';
import { expect, it } from 'vitest';

it('lib/ is the build of src/ (after editing src: npm run build)', { timeout: 60_000 }, () => {
  const file = fileURLToPath(new URL('../tsconfig.json', import.meta.url));
  const cfg = ts.parseJsonConfigFileContent(ts.readConfigFile(file, ts.sys.readFile).config, ts.sys, dirname(file));
  const program = ts.createProgram(cfg.fileNames, cfg.options);
  expect(ts.getPreEmitDiagnostics(program).map((d) => ts.flattenDiagnosticMessageText(d.messageText, '\n'))).toEqual([]);
  const out = cfg.options.outDir!, emitted: Record<string, string> = {}, committed: Record<string, string> = {};
  program.emit(undefined, (path, text) => (emitted[relative(out, path)] = text));
  for (const f of readdirSync(out)) committed[f] = readFileSync(join(out, f), 'utf8').replace(/\r\n/g, '\n');
  expect(Object.keys(emitted).sort()).toEqual(['index.d.ts', 'index.js', 'native.d.ts', 'native.js', 'options.d.ts', 'options.js']);
  expect(committed).toEqual(emitted);
});

it('src/ imports its siblings as .ts (Node runs src/*.ts as they are; tsc rewrites them to .js in lib/)', () => {
  const src = fileURLToPath(new URL('../src', import.meta.url));
  for (const f of readdirSync(src)) {
    const specifiers = [...readFileSync(join(src, f), 'utf8').matchAll(/(?:from |import\()'(\.{1,2}\/[^']*)'/g)].map((m) => m[1]);
    expect(specifiers.filter((s) => !s.endsWith('.ts')), f).toEqual([]);
  }
});
