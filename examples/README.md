# Examples

A program per way in to the core, each running both tools, daylight and views, on a small scene: `cli/` (the exes,
PowerShell), `cpp/`, `c/`, `csharp/`, `python/`, `typescript/`, `javascript/`. What each shows, how to run it and a
sample of its output: [docs/examples.md](../docs/examples.md). Each file reads top to bottom as a tutorial: a header
listing its steps, then the steps in order (the meshes, a run, reading the result, ...), every flag and call commented.
The guide of each language explains the API around it.

```
pwsh examples/run.ps1      # every example after the build: PASS / FAIL each, exit 1 if any fails, logs in examples/out
```
