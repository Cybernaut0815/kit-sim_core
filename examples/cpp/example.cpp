// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
//
// daylight and views in-process from C++ (guide: docs/cpp.md)
//
// The steps:
//   1. the meshes: a south facade and a block made in code, the lawn and box of tests/box.obj read from a file
//   2. daylight: a clear-sky year in Berlin, kWh/m2 and sun hours per face
//   3. reading a result: rows per mesh, the mean of a column
//   4. views: what each face sees within 120 deg of its normal; this run reuses the GPU scene of step 2
//   5. isovists: the volume and the area a standing eye sees in the room of tests/room.obj
//   6. cancel and errors
//
// Build: the core's build makes this example too (cmake --build --preset release).
// Run from the repo root:   build\RelWithDebInfo\example_cpp.exe tests
// It prints the mean of a few columns per mesh for each run, then the messages of a cancelled run and a bad target.
#include "daylight.h"   // daylight::SimulationMesh, daylight::run, daylight::Options, daylight::Result, read_obj
#include "views.h"      // daylight::views::run, daylight::views::Options

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using Vec3 = std::array<double, 3>;

// A flat grid of nu x nv quads as one SimulationMesh, e.g. a facade of 1 m panels.
//   corner          one corner of the grid (metres, Z up)
//   alongU, alongV  its two edges from that corner: nu quads along alongU, nv along alongV
// Each quad is wound corner, +u, +u+v, +v: counter-clockwise seen from the side it faces, so its normal (the side
// that is analysed) is alongU x alongV. The core reads this right-handed winding to know which side is the front.
daylight::SimulationMesh grid(const char* name, Vec3 corner, Vec3 alongU, Vec3 alongV, int nu, int nv) {
  daylight::SimulationMesh mesh{name};   // sensor = true by default: analysed (a result row per face) and blocking

  // the (nu + 1) x (nv + 1) corner points, row by row; points holds x, y, z of each point one after another
  for (int j = 0; j <= nv; ++j)
    for (int i = 0; i <= nu; ++i)
      for (int axis = 0; axis < 3; ++axis) mesh.points.push_back(corner[axis] + alongU[axis] * i / nu + alongV[axis] * j / nv);

  // the faces: counts holds the number of vertices of each face, indices their point numbers (0-based), in order
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) {
      const int first = j * (nu + 1) + i;   // the quad's corner point
      mesh.counts.push_back(4);
      mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + nu + 2, first + nu + 1});
    }
  return mesh;
}

// The mean of one column over the rows of sensor mesh number k.
// A Result holds one row per face of every sensor mesh, the meshes in the order given. Result::ranges lists each sensor
// mesh (name, row count) in that order, so the rows of mesh k start after the rows of meshes 0 .. k-1.
double mean(const daylight::Result& result, size_t k, const std::string& column) {
  size_t firstRow = 0;
  for (size_t i = 0; i < k; ++i) firstRow += result.ranges[i].second;
  const size_t rows = result.ranges[k].second;
  const size_t nColumns = result.columns.size();
  const size_t c = static_cast<size_t>(std::find(result.columns.begin(), result.columns.end(), column) - result.columns.begin());
  double sum = 0;
  for (size_t row = firstRow; row < firstRow + rows; ++row) sum += result.values[row * nColumns + c];   // row-major: rows x columns
  return sum / static_cast<double>(rows);
}

int fail(const std::string& err) {
  std::fprintf(stderr, "error: %s\n", err.c_str());
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string tests = argc > 1 ? argv[1] : "tests";   // the folder holding box.obj, room.obj and room_grid.obj
  std::string err;   // every call that can fail returns false and puts the reason here

  using Clock = std::chrono::steady_clock;
  const auto msSince = [](Clock::time_point start) { return 1e3 * std::chrono::duration<double>(Clock::now() - start).count(); };

  // ---- Step 1: the meshes ---------------------------------------------------------------------------------------
  // Every mesh has a role. Sensor meshes (SimulationMesh::sensor = true, the default) are analysed: a result row per
  // face. Context meshes (sensor = false) are not analysed, they only block rays. Both cast shade.
  std::vector<daylight::SimulationMesh> meshes;

  // a 10 x 6 m facade of 1 m panels in the plane y = 4, facing south (-y): from x = -5, 10 m along x, 6 m up
  meshes.push_back(grid("facade", {-5, 4, 0}, {10, 0, 0}, {0, 0, 6}, 10, 6));

  // tests/box.obj holds two OBJ groups, read as two meshes (both sensors): "ground", four 2 m quads at z = 0, and
  // "box", a 1 m box floating 0.5 m above the fourth quad
  if (!daylight::read_obj(tests + "/box.obj", meshes, err)) return fail(err);

  // a 30 x 9 m block across the street (y = -14), facing north to the facade: context, it only blocks
  meshes.push_back(grid("block", {-15, -14, 0}, {30, 0, 0}, {0, 0, 9}, 10, 3));
  meshes.back().sensor = false;

  // ---- Step 2: daylight, a clear-sky year in Berlin ------------------------------------------------------------
  // No weather file needed: clear_sky_weather makes a cloudless year for a place, as the CLI's --clear-sky does.
  // Its arguments: latitude 52.52, longitude 13.41 (Berlin), time zone +1, Linke turbidity 3 (the haze of the clear
  // sky), altitude 0 m.
  daylight::Weather weather;
  if (!daylight::clear_sky_weather(52.52, 13.41, 1, 3, 0, weather, err)) return fail(err);

  daylight::Options options;
  options.quantities = {daylight::Quantity::Radiation, daylight::Quantity::SunHours};   // kWh/m2 and sun hours
  // sceneKey names these meshes: the GPU scene built for them (triangles, acceleration structure) stays resident under
  // this key, and a later run with the same key and the same meshes in the same roles skips that work (step 4).
  options.sceneKey = "site";

  daylight::Result year;
  auto start = Clock::now();
  if (!daylight::run(meshes, &weather, options, year, err)) return fail(err);
  std::printf("daylight, clear-sky year: scene reused %s, %.0f ms\n", year.sceneReused ? "true" : "false", msSince(start));

  // ---- Step 3: reading the result ---------------------------------------------------------------------------------
  // year.columns names the columns ("radiation_front", "sunhours_front": a quantity and the side of the face),
  // year.ranges the sensor meshes and their row counts (facade 60, ground 4, box 12; the block, context, has none) and
  // year.values the numbers, row by row. Here: the mean over each mesh's faces.
  for (size_t k = 0; k < year.ranges.size(); ++k)
    std::printf("  %-6s %2zu faces  %4.0f kWh/m2  %4.0f sun hours\n", year.ranges[k].first.c_str(), year.ranges[k].second,
                mean(year, k, "radiation_front"), mean(year, k, "sunhours_front"));

  // ---- Step 4: views about each face's normal ---------------------------------------------------------------------
  // From every sensor face a ray per 2 deg cell of a field 120 deg wide about its normal, 70 deg down to 50 deg up (the
  // defaults). The columns: the shares of the field that end on the sky, the ground, an obstruction or a target group,
  // and how far the face sees.
  daylight::views::Options view;
  view.targets = {{"ground", "lawn"}};   // rays ending on the mesh "ground" count for the group "lawn" (column lawn_front)
  view.reach = {6, 20, 50};              // reach_<D>: the share of the field that sees at least D metres (EN 17037's levels)
  view.sceneKey = "site";                // the same key and meshes as step 2: daylight's GPU scene is reused, not rebuilt

  // Progress and cancel are atomics, so another thread (a UI's progress bar, its Cancel button) may read and set them
  // while the run works. Here progress is read after the run, cancel is set before a later run (step 6).
  std::atomic<double> progress{0};   // the share of the sensors done, updated after every traced chunk
  std::atomic<bool> cancel{false};   // true: the run stops at its next check and fails with "cancelled"
  view.progress = &progress;
  view.cancel = &cancel;

  daylight::Result seen;
  start = Clock::now();
  if (!daylight::views::run(meshes, view, seen, err)) return fail(err);
  std::printf("views, 120 deg about each normal: scene reused %s, %.0f ms, progress %.0f %%\n", seen.sceneReused ? "true" : "false",
              msSince(start), 100 * progress.load());
  // distance_front: the log (geometric) mean of the rays' distances, an open ray counting as 1000 m (Options::farDist)
  for (size_t k = 0; k < seen.ranges.size(); ++k)
    std::printf("  %-6s %2zu faces  sky %.2f  lawn %.2f  distance %3.0f m  sees 6 / 20 / 50 m: %.2f %.2f %.2f\n",
                seen.ranges[k].first.c_str(), seen.ranges[k].second, mean(seen, k, "sky_front"), mean(seen, k, "lawn_front"),
                mean(seen, k, "distance_front"), mean(seen, k, "reach_6_front"), mean(seen, k, "reach_20_front"),
                mean(seen, k, "reach_50_front"));

  // ---- Step 5: isovists in a room ---------------------------------------------------------------------------------
  // tests/room.obj is a 6.3 x 5.1 x 2.9 m room (groups "room", "window", "tree"), tests/room_grid.obj a grid of 12
  // measuring points at desk height (group "grid"). The isovist is what an eye sees around it: all around in 3D the
  // volume in view (column volume_front, m3), with horizontal rays only the area in view (area_front, m2).
  std::vector<daylight::SimulationMesh> room;
  if (!daylight::read_obj(tests + "/room_grid.obj", room, err)) return fail(err);   // room[0]: the grid, a sensor
  if (!daylight::read_obj(tests + "/room.obj", room, err)) return fail(err);
  for (size_t k = 1; k < room.size(); ++k) room[k].sensor = false;   // room, window, tree: context

  daylight::views::Options isovist;
  isovist.fovH = 360;                          // all around (azimuth from north) instead of about each face's normal
  isovist.elevLo = -90;                        // the whole sphere: from straight down ...
  isovist.elevHi = 90;                         // ... to straight up
  isovist.offset = 0.4;                        // the eye 0.4 m above each grid face (1.2 m above the floor)
  isovist.noOcclude = {"grid"};                // the grid is measured, not seen: the rays pass through it
  // The isovist is 360 mode's default column; isovist.distance = DistMean | DistMin would give the distances instead,
  // isovist.farDist caps every ray's length (open rays count as farDist, 1000 m by default).
  daylight::Result volume, area;
  if (!daylight::views::run(room, isovist, volume, err)) return fail(err);
  isovist.elevLo = isovist.elevHi = 0;         // a flat field: one row of horizontal rays, the 2D isovist
  if (!daylight::views::run(room, isovist, area, err)) return fail(err);
  std::printf("isovists in the room: volume %.1f m3 all around, area %.1f m2 at eye height\n", mean(volume, 0, "volume_front"),
              mean(area, 0, "area_front"));

  // ---- Step 6: cancel and errors ----------------------------------------------------------------------------------
  // A run that fails returns false with the reason in err; the program goes on. First a cancelled run (cancel set
  // before it starts, as a Cancel button would during it), then a target name no mesh has.
  cancel = true;
  if (!daylight::views::run(meshes, view, seen, err)) std::printf("cancelled run: %s\n", err.c_str());
  cancel = false;
  view.targets = {{"tree", "green"}};
  if (!daylight::views::run(meshes, view, seen, err)) std::printf("bad target: %s\n", err.c_str());
  return 0;
}
