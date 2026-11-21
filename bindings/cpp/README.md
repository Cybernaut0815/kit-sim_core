# bindings/cpp

Guide: [docs/cpp.md](../../docs/cpp.md) (setup, both tools worked through, results, speed, errors); example:
[examples/cpp/example.cpp](../../examples/cpp/example.cpp) ([docs/examples.md](../../docs/examples.md)).

C++ callers skip files: fill `daylight::SimulationMesh` and call `daylight::run` (daylight) or `daylight::views::run` (views)
in-process. CMake: `add_subdirectory(<repo>)` + `target_link_libraries(app PRIVATE daylightcore)` (`daylight.h` and
`views.h` come with it).

```cpp
#include "daylight.h"
#include "views.h"
#include <cstdio>
#include <stdexcept>
daylight::SimulationMesh m;                                  // one quad, metres, Z-up, right-handed winding
m.name = "roof"; m.points = {0,0,0, 1,0,0, 1,1,0, 0,1,0}; m.counts = {4}; m.indices = {0,1,2,3};
m.sensor = true;                                   // false = context: blocks rays only
daylight::Options opt; opt.quantities = {daylight::Quantity::Radiation, daylight::Quantity::SunHours};
daylight::Weather w; daylight::Result r; std::string err;
if (!daylight::read_epw("boston.epw", w, err) || !daylight::run({m}, &w, opt, r, err)) throw std::runtime_error(err);
for (size_t e = 0; e < r.values.size() / r.columns.size(); ++e)   // row-major: element e, column c
  std::printf("%g %s\n", r.values[e * r.columns.size()], r.columns[0].c_str());
daylight::views::Options vo;                       // views: 120 deg about the normal, sky / ground / obstruction shares
daylight::Result v;
if (!daylight::views::run({m}, vo, v, err)) throw std::runtime_error(err);   // v.columns: sky_front, ground_front, ...
```

## USD (optional, LightUSD)

`daylight_usd` is a reader library: USD stage (sublayers, references, payloads, xforms, up axis, units) ->
`daylight::SimulationMesh[]` in world metres, Z-up. Configure with `-DDAYLIGHT_USD=ON` (preset `vs2022-usd`): the CLI then
takes `.usd/.usda/.usdc/.usdz` directly, `ctest` adds `check_usd` + `cli_usd`. In-process:

```cpp
#include "daylight_usd.h"                          // target_link_libraries(app PRIVATE daylight_usd)
#include <stdexcept>
std::vector<daylight::SimulationMesh> meshes; std::string err;
if (!daylight::read_usd("scene.usda", 'A', -1, meshes, err)) throw std::runtime_error(err);   // 'A' = stage up axis, -1 = authored metersPerUnit
for (auto& m : meshes) m.sensor = m.name.rfind("/World/Bldg", 0) == 0;   // prim path prefix picks the sensors
daylight::Weather w; daylight::Result r;
daylight::read_epw("boston.epw", w, err) && daylight::run(meshes, &w, daylight::Options{}, r, err);
```

Speed (1M quads, 1 002 001 points, `-v` "info: read"): `.usdc` 0.10 s, `.usda` 0.58 s (LightUSD, zero-copy
attribute refs); the Python converter is only needed for stages LightUSD cannot compose (see
[docs/python.md](../../docs/python.md#a-usd-stage-in-and-out)).
