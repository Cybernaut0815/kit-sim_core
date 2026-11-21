# SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
#
# daylight.exe and views.exe from the command line (guide: docs/examples.md#cli, every flag: docs/daylight.md,
# docs/views.md)
#
# The steps:
#   1. daylight on tests/box.obj: a clear-sky year, the sun hours of one day, one instant at noon
#   2. views on tests/box.obj: 120 deg about each face's normal, then a standing eye looking all around
#   3. an interior, tests/room.obj with its measuring grid: views through the window, daylight through it
#   4. isovists in the same room: the volume and the area in view
#
# tests/box.obj holds two OBJ groups, read as two meshes: "ground" (four 2 m quads at z 0) and "box" (a 1 m box
# floating 0.5 m above the fourth quad). Both are sensors: analysed (a row per face) and blocking.
#
# Every run writes its values (text, a line per face, a number per column) to examples/cli/out/<name>.txt and the
# manifest (columns, each mesh's rows) beside it; this script then prints the command and, per sensor mesh, the mean
# of some columns over that mesh's faces.
#
# Needs the core's build. Run from anywhere:   pwsh examples/cli/run.ps1
# (DAYLIGHT_EXE, VIEWS_EXE: other exes than the build's)
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo 'build/RelWithDebInfo'
$daylight = if ($env:DAYLIGHT_EXE) { $env:DAYLIGHT_EXE } else { Join-Path $bin 'daylight.exe' }
$views = if ($env:VIEWS_EXE) { $env:VIEWS_EXE } else { Join-Path $bin 'views.exe' }
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$inv = [Globalization.CultureInfo]::InvariantCulture   # print 0.5, not 0,5, whatever the machine's language

# Runs one exe and prints the means of its columns per mesh.
#   $Exe      daylight.exe or views.exe
#   $Name     the output's name: examples/cli/out/$Name.txt (values) and $Name.txt.json (manifest)
#   $Flags    the flags, one string (split at spaces)
#   $Columns  the columns to print, without "_front" (empty: every column)
#   $Mesh     only the sensor mesh of this name (default: every one)
#   $Files    the mesh files and --context files, relative to the repo (default: tests/box.obj)
function Invoke-Run([string]$Exe, [string]$Name, [string]$Flags, [string]$Columns, [string]$Mesh = '*',
                    [string]$Files = 'tests/box.obj') {
  # 1. run the exe: mesh files first (paths made absolute; flags such as --context kept as they are), then the flags
  $file = Join-Path $out "$Name.txt"
  $paths = $Files -split ' ' | ForEach-Object { if ($_ -like '-*') { $_ } else { Join-Path $repo $_ } }
  & $Exe $paths ($Flags -split ' ') --out $file
  if ($LASTEXITCODE -ne 0) { Write-Output "$Name failed: exit $LASTEXITCODE"; exit 1 }

  # 2. read the results back: the manifest (JSON) and the values (a line per face, numbers separated by spaces)
  $manifest = Get-Content -Raw "$file.json" | ConvertFrom-Json
  $rows = @(Get-Content $file | ForEach-Object { , [double[]]($_ -split ' ') })
  $wanted = if ($Columns) { $Columns -split ' ' } else { $manifest.columns -replace '_front$' }

  # 3. print the command, then per sensor mesh the mean of each wanted column over its rows: the manifest's meshes
  #    give each mesh's first row and row count
  Write-Output "$((Get-Item $Exe).BaseName) $Files $Flags --out examples/cli/out/$Name.txt"
  foreach ($range in $manifest.meshes | Where-Object name -like $Mesh) {
    $line = [string]::Format($inv, '  {0,-6} {1,2} faces', $range.name, $range.count)
    foreach ($column in $wanted) {
      $c = [array]::IndexOf($manifest.columns, "$($column)_front")   # the column's position in a row
      if ($c -lt 0) { throw "$Name has no column $($column)_front" }
      $sum = 0.0
      foreach ($row in $rows[$range.first..($range.first + $range.count - 1)]) { $sum += $row[$c] }
      $line += [string]::Format($inv, '  {0} {1:0.##}', $column, $sum / $range.count)
    }
    Write-Output $line
  }
}

# ---- Step 1: daylight on tests/box.obj -------------------------------------------------------------------------------
$berlin = '--lat 52.52 --lon 13.41'
# a clear-sky year in Berlin (no weather file: a cloudless year), kWh/m2 and sun hours over the year
Invoke-Run $daylight year "--clear-sky $berlin --quantity radiation,sunhours"
# the sun hours of 21 June in 10 minute steps: they need only the place and its clock (--tz 1: UTC+1), no weather
Invoke-Run $daylight june21 "--quantity sunhours $berlin --tz 1 --from 06-21T00:00 --to 06-22T00:00 --step 10"
# one instant, noon of 21 June under the clear sky: irradiance in W/m2 and illuminance in lux
Invoke-Run $daylight noon "--clear-sky $berlin --quantity irradiance,illuminance --time 06-21T12:00"

# ---- Step 2: views on tests/box.obj ----------------------------------------------------------------------------------
# 120 deg about each face's normal (the default field, written out). --target ground=lawn: rays ending on the mesh
# "ground" count for the group "lawn". --reach en17037: the shares of the field that see at least 6, 20 and 50 m.
# distance: the log mean of the rays' distances, an open ray counting as 1000 m.
Invoke-Run $views view '--fov 120 --target ground=lawn --reach en17037' 'sky ground lawn obstruction distance reach_20'
# A standing eye 1.6 m above each ground quad (--offset 1.6), looking all around (--fov 360); the box is the group
# "cube". All around the default column is the isovist's volume; --distance mean asks for the log-mean distance.
# Only the ground's rows are printed: the box's faces get eyes 1.6 m out too.
$eye = '--fov 360 --offset 1.6 --target ground=lawn --target box=cube --reach en17037 --distance mean'
Invoke-Run $views eye $eye 'sky ground lawn cube distance reach_20' ground

# ---- Step 3: an interior ---------------------------------------------------------------------------------------------
# tests/room.obj: a 6.3 x 5.1 x 2.9 m room (groups room, window, tree: a tree outside), given as context
# (--context: it only blocks). tests/room_grid.obj: 12 measuring points at desk height (0.8 m), the sensors.
$room = 'tests/room_grid.obj --context tests/room.obj'
# --no-occlude grid: the grid is measured, not seen (the rays pass it). The window is glass (--optics
# window=0,0.64,glass: reflectance 0, transmittance 0.64) and --transmission on lets the rays pass it.
$glass = '--no-occlude grid --optics window=0,0.64,glass --transmission on'
# views all around from 0.4 m above the grid: the shares seen through the window (glazing), the volume in view out to
# 20 m (--far 20 caps every ray) and the log-mean distance
Invoke-Run $views interior "--fov 360 --offset 0.4 --target tree=green $glass --reach en17037 --distance isovist,mean --far 20" 'sky ground green glazing volume distance reach_6' '*' $room
# daylight: the light that comes in through the window over a clear-sky year
Invoke-Run $daylight interior_light "--clear-sky $berlin $glass --quantity radiation,sunhours" '' '*' $room

# ---- Step 4: isovists in the room ------------------------------------------------------------------------------------
# The window shut this time (no --transmission). The isovist is what an eye sees around it: all around in 3D the
# volume in view (--fov 360,-90,90, column volume, m3), with horizontal rays only the area in view (--fov 360,0,0,
# column area, m2). It is the default column all around; --distance mean,min gives the distances instead.
Invoke-Run $views isovist_3d '--fov 360,-90,90 --offset 0.4 --no-occlude grid' 'volume' '*' $room
Invoke-Run $views isovist_2d '--fov 360,0,0 --offset 0.4 --no-occlude grid' 'area' '*' $room
