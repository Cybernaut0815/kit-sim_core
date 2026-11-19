# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
# daylight.exe and views.exe on tests/box.obj (OBJ groups "ground": four 2 m quads at z 0, "box": a 1 m box floating
# above the fourth; both are sensors and block), a few flag sets each, then both on an interior with a glass window
# (tests/room.obj, its measure grid tests/room_grid.obj) (docs/examples.md#cli). Values (text) and
# manifests go to examples/cli/out; per sensor mesh the mean of each column is printed.
# Run after the build, from anywhere: pwsh examples/cli/run.ps1 (DAYLIGHT_EXE, VIEWS_EXE: other exes)
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo 'build/RelWithDebInfo'
$daylight = if ($env:DAYLIGHT_EXE) { $env:DAYLIGHT_EXE } else { Join-Path $bin 'daylight.exe' }
$views = if ($env:VIEWS_EXE) { $env:VIEWS_EXE } else { Join-Path $bin 'views.exe' }
$box = Join-Path $repo 'tests/box.obj'
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$inv = [Globalization.CultureInfo]::InvariantCulture   # print 0.5, not 0,5, whatever the machine's locale

# One run of $Exe on tests/box.obj with $Flags: values to out/$Name.txt (a line per face, a number per column), the
# manifest to out/$Name.txt.json. Then per sensor mesh (the manifest's meshes: name, first row, row count; only $Mesh
# if given) the mean over its rows of each of $Columns (names without _front; default: every column).
function Invoke-Run([string]$Exe, [string]$Name, [string]$Flags, [string]$Columns, [string]$Mesh = '*', [string]$Files = 'tests/box.obj') {
  $file = Join-Path $out "$Name.txt"
  $paths = $Files -split ' ' | ForEach-Object { if ($_ -like '-*') { $_ } else { Join-Path $repo $_ } }   # files of the repo
  & $Exe $paths ($Flags -split ' ') --out $file
  if ($LASTEXITCODE -ne 0) { Write-Output "$Name failed: exit $LASTEXITCODE"; exit 1 }
  $m = Get-Content -Raw "$file.json" | ConvertFrom-Json
  $rows = @(Get-Content $file | ForEach-Object { , [double[]]($_ -split ' ') })
  $cols = if ($Columns) { $Columns -split ' ' } else { $m.columns -replace '_front$' }
  Write-Output "$((Get-Item $Exe).BaseName) $Files $Flags --out examples/cli/out/$Name.txt"
  foreach ($r in $m.meshes | Where-Object name -like $Mesh) {
    $line = [string]::Format($inv, '  {0,-6} {1,2} faces', $r.name, $r.count)
    foreach ($col in $cols) {
      $c = [array]::IndexOf($m.columns, "$($col)_front")
      if ($c -lt 0) { throw "$Name has no column $($col)_front" }
      $sum = 0.0
      foreach ($row in $rows[$r.first..($r.first + $r.count - 1)]) { $sum += $row[$c] }
      $line += [string]::Format($inv, '  {0} {1:0.##}', $col, $sum / $r.count)
    }
    Write-Output $line
  }
}

$berlin = '--lat 52.52 --lon 13.41'
# daylight: a clear-sky year in Berlin (no weather file): kWh/m2 and sun hours over the year
Invoke-Run $daylight year "--clear-sky $berlin --quantity radiation,sunhours"
# daylight: sun hours of 21 June in 10 minute steps; sun hours need only the place and its clock (--tz), no weather
Invoke-Run $daylight june21 "--quantity sunhours $berlin --tz 1 --from 06-21T00:00 --to 06-22T00:00 --step 10"
# daylight: one instant, noon of 21 June under the clear sky: W/m2 and lux
Invoke-Run $daylight noon "--clear-sky $berlin --quantity irradiance,illuminance --time 06-21T12:00"
# views: 120 deg about each face's normal (the default field); rays ending on the mesh "ground" count for the group
# "lawn"; reach: the shares of the field that see 6, 20 and 50 m (EN 17037)
Invoke-Run $views view '--fov 120 --target ground=lawn --reach en17037' 'sky ground lawn obstruction distance reach_20'
# views: a standing eye 1.6 m above each ground quad, all around; the box is the group "cube". Only the ground's rows
# are printed: the box's faces get eyes 1.6 m out too.
$eye = '--fov 360 --offset 1.6 --target ground=lawn --target box=cube --reach en17037'
Invoke-Run $views eye $eye 'sky ground lawn cube distance reach_20' ground
# an interior, tests/room.obj (a room, its window, a tree outside) with the measure grid tests/room_grid.obj at 0.8 m,
# which --no-occlude keeps out of the rays; the window is glass: views looks through it (--transmission on: glazing =
# the share of the field seen through glass), all around from 0.4 m above the grid; daylight: the light through it
$room = 'tests/room_grid.obj --context tests/room.obj'
$glass = '--no-occlude grid --optics window=0,0.64,glass --transmission on'
Invoke-Run $views interior "--fov 360 --offset 0.4 --target tree=green $glass --reach en17037" 'sky ground green glazing distance reach_6' '*' $room
Invoke-Run $daylight interior_light "--clear-sky $berlin $glass --quantity radiation,sunhours" '' '*' $room
