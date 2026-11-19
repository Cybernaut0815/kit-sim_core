# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
# Runs every example (docs/examples.md) and prints PASS or FAIL for each, the output's tail on a failure; exit 1 if any
# fails. Each example's output goes to examples/out/<name>.log. Needs the core's build (cmake --build --preset release,
# which builds the C and C++ examples), the .NET 9 SDK, uv, and Node >= 22.18 with npm install run in bindings/ts.
# Run from anywhere: pwsh examples/run.ps1
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'build/RelWithDebInfo'
$logs = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $logs | Out-Null
$ps = (Get-Process -Id $PID).Path   # this PowerShell, for the CLI script
$inv = [Globalization.CultureInfo]::InvariantCulture

# One build for every example (variables already set are kept): the in-process examples load DAYLIGHT_DLL, the process
# runners start DAYLIGHT_EXE and VIEWS_EXE. VIRTUAL_ENV is cleared so that uv takes the binding's .venv without a
# warning. The caller's values come back at the end.
$vars = 'DAYLIGHT_DLL', 'DAYLIGHT_EXE', 'VIEWS_EXE', 'VIRTUAL_ENV'
$saved = @{}
foreach ($k in $vars) { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
if (-not $env:DAYLIGHT_DLL) { $env:DAYLIGHT_DLL = Join-Path $bin 'daylight_c.dll' }
if (-not $env:DAYLIGHT_EXE) { $env:DAYLIGHT_EXE = Join-Path $bin 'daylight.exe' }
if (-not $env:VIEWS_EXE) { $env:VIEWS_EXE = Join-Path $bin 'views.exe' }
$env:VIRTUAL_ENV = $null

# name -> command, run from the repo root
$examples = [ordered]@{
  'cpp'               = { & (Join-Path $bin 'example_cpp.exe') tests }
  'c'                 = { & (Join-Path $bin 'example_c.exe') tests }
  'csharp'            = { dotnet run --project examples/csharp }
  'python-inprocess'  = { uv run --project bindings/python python examples/python/inprocess.py }
  'python-process'    = { uv run --project bindings/python python examples/python/process.py }
  'typescript-native' = { node examples/typescript/native.ts }
  'javascript-native' = { node examples/javascript/native.mjs }
  'typescript-cli'    = { node examples/typescript/cli.ts }
  'cli'               = { & $ps -NoProfile -File examples/cli/run.ps1 }
}

$failed = 0
Push-Location $repo
try {
  foreach ($name in $examples.Keys) {
    $log = Join-Path $logs "$name.log"
    $global:LASTEXITCODE = 1   # stays 1 when a command cannot start
    $t = [Diagnostics.Stopwatch]::StartNew()
    & $examples[$name] *> $log
    $ok = $LASTEXITCODE -eq 0
    if (-not $ok) { $failed++ }
    Write-Output ([string]::Format($inv, '{0}  {1,-18} {2,5:0.0} s', $(if ($ok) { 'PASS' } else { 'FAIL' }), $name,
                                   $t.Elapsed.TotalSeconds))
    if (-not $ok) { Get-Content $log -Tail 15 | ForEach-Object { "      $_" } }
  }
} finally {
  Pop-Location
  foreach ($k in $vars) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}
Write-Output "$($examples.Count - $failed) of $($examples.Count) passed; logs in examples/out"
exit [int]($failed -gt 0)
