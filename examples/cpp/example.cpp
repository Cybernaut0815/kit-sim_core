// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// daylight and views in-process from C++ (docs/cpp.md): a south facade and a block made in memory, the lawn and box
// of tests/box.obj in front of the facade. A clear-sky year of daylight, then the view out of the same meshes, which
// reuses the GPU scene; progress, cancel, errors. Run from the repo root: build/RelWithDebInfo/example_cpp.exe tests
#include "daylight.h"
#include "views.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using V = std::array<double, 3>;

// nu x nv quads from corner o along the edges u and v, row by row: points in metres, Z up; each quad wound o, o+u,
// o+u+v, o+v (right-handed: counter-clockwise seen from the front), so its normal is u x v.
daylight::SimulationMesh grid(const char* name, V o, V u, V v, int nu, int nv) {
  daylight::SimulationMesh m{name};   // sensor = true: analysed (a row per face) and blocking
  for (int j = 0; j <= nv; ++j)
    for (int i = 0; i <= nu; ++i)
      for (int a = 0; a < 3; ++a) m.points.push_back(o[a] + u[a] * i / nu + v[a] * j / nv);
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) {
      const int p = j * (nu + 1) + i;
      m.counts.push_back(4);
      m.indices.insert(m.indices.end(), {p, p + 1, p + nu + 2, p + nu + 1});
    }
  return m;
}

// The mean of column `col` over the rows of sensor mesh k. Result::ranges holds (name, rows) per sensor mesh in input
// order: mesh k's rows follow those of meshes 0..k-1 (the manifest's "first").
double mean(const daylight::Result& r, size_t k, const std::string& col) {
  size_t first = 0;
  for (size_t i = 0; i < k; ++i) first += r.ranges[i].second;
  const size_t C = r.columns.size();
  const size_t c = static_cast<size_t>(std::find(r.columns.begin(), r.columns.end(), col) - r.columns.begin());
  double sum = 0;
  for (size_t e = first; e < first + r.ranges[k].second; ++e) sum += r.values[e * C + c];   // row-major N x C
  return sum / static_cast<double>(r.ranges[k].second);
}

int fail(const std::string& err) {
  std::fprintf(stderr, "error: %s\n", err.c_str());
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point t) { return 1e3 * std::chrono::duration<double>(Clock::now() - t).count(); };
  std::string err;
  std::vector<daylight::SimulationMesh> meshes = {grid("facade", {-5, 4, 0}, {10, 0, 0}, {0, 0, 6}, 10, 6)};   // y = 4, faces -y
  if (!daylight::read_obj(std::string(argc > 1 ? argv[1] : "tests") + "/box.obj", meshes, err)) return fail(err);
  meshes.push_back(grid("block", {-15, -14, 0}, {30, 0, 0}, {0, 0, 9}, 10, 3));   // across the street
  meshes.back().sensor = false;   // context: blocks rays, no rows; facade, ground and box (sensor = true) get rows

  // daylight: a clear-sky year in Berlin (lat, lon, tz, Linke turbidity, altitude), as the CLI's --clear-sky
  daylight::Weather w;
  daylight::Options opt;
  opt.quantities = {daylight::Quantity::Radiation, daylight::Quantity::SunHours};
  opt.sceneKey = "site";   // these meshes' identity: a later run under the same key reuses their GPU scene
  daylight::Result r;
  if (!daylight::clear_sky_weather(52.52, 13.41, 1, 3, 0, w, err)) return fail(err);
  auto t0 = Clock::now();
  if (!daylight::run(meshes, &w, opt, r, err)) return fail(err);
  std::printf("daylight, clear-sky year: scene reused %s, %.0f ms\n", r.sceneReused ? "true" : "false", ms(t0));
  for (size_t k = 0; k < r.ranges.size(); ++k)
    std::printf("  %-6s %2zu faces  %4.0f kWh/m2  %4.0f sun hours\n", r.ranges[k].first.c_str(), r.ranges[k].second,
                mean(r, k, "radiation_front"), mean(r, k, "sunhours_front"));

  // views: 120 deg about each normal (the default field); rays ending on mesh "ground" count for the group "lawn"
  daylight::views::Options vo;
  vo.targets = {{"ground", "lawn"}};
  vo.reach = {6, 20, 50};             // --reach en17037
  vo.sceneKey = "site";
  std::atomic<double> progress{0};    // after each trace chunk the share of sensors done, for another thread to show
  std::atomic<bool> cancel{false};    // another thread sets it: the run stops at its next check and fails "cancelled"
  vo.progress = &progress;
  vo.cancel = &cancel;
  daylight::Result v;
  t0 = Clock::now();
  if (!daylight::views::run(meshes, vo, v, err)) return fail(err);
  std::printf("views, 120 deg about each normal: scene reused %s, %.0f ms, progress %.0f %%\n",
              v.sceneReused ? "true" : "false", ms(t0), 100 * progress.load());
  for (size_t k = 0; k < v.ranges.size(); ++k)
    std::printf("  %-6s %2zu faces  sky %.2f  lawn %.2f  distance %3.0f m  sees 6 / 20 / 50 m: %.2f %.2f %.2f\n",
                v.ranges[k].first.c_str(), v.ranges[k].second, mean(v, k, "sky_front"), mean(v, k, "lawn_front"),
                mean(v, k, "distance_front"), mean(v, k, "reach_6_front"), mean(v, k, "reach_20_front"),
                mean(v, k, "reach_50_front"));

  // isovists in tests/room.obj (a 6.3 x 5.1 x 2.9 m room) from its measure grid, the eye 0.4 m above it: the volume
  // all around (fovH 360, elevation -90..90: m3) and the area of horizontal rays only (elevation 0..0: m2). The isovist
  // is 360 mode's default column; vo.distance = DistMean | DistMin gives the distances instead, farDist caps both.
  std::vector<daylight::SimulationMesh> room;
  const std::string tests = argc > 1 ? argv[1] : "tests";
  if (!daylight::read_obj(tests + "/room_grid.obj", room, err) || !daylight::read_obj(tests + "/room.obj", room, err)) return fail(err);
  for (size_t k = 1; k < room.size(); ++k) room[k].sensor = false;   // room, window, tree: context
  daylight::views::Options io;
  io.fovH = 360; io.elevLo = -90; io.elevHi = 90;
  io.offset = 0.4;
  io.noOcclude = {"grid"};   // measured, not seen
  daylight::Result iv, ia;
  if (!daylight::views::run(room, io, iv, err)) return fail(err);
  io.elevLo = io.elevHi = 0;
  if (!daylight::views::run(room, io, ia, err)) return fail(err);
  std::printf("isovists in the room: volume %.1f m3 all around, area %.1f m2 at eye height\n", mean(iv, 0, "volume_front"),
              mean(ia, 0, "area_front"));

  // Failures return false with err: here a cancelled run, then a target name the meshes do not resolve
  cancel = true;
  if (!daylight::views::run(meshes, vo, v, err)) std::printf("cancelled run: %s\n", err.c_str());
  cancel = false;
  vo.targets = {{"tree", "green"}};
  if (!daylight::views::run(meshes, vo, v, err)) std::printf("bad target: %s\n", err.c_str());
  return 0;
}
