// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// daylight CLI: meshes + location + time spec in, one value list per element out. Built a second time with VIEWS_CLI as
// views.exe (meshes + field of view in). Flags and rules live in cli.cpp.
#include "cli.h"
#include "daylight.h"
#include "internal.h"
#ifdef DAYLIGHT_USD
#include "daylight_usd.h"
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace daylight;
#ifdef VIEWS_CLI
namespace tool = daylight::views::cli;
#else
namespace tool = daylight::cli;
#endif

namespace {

int usage_error(const char* msg) {
  std::fprintf(stderr, "error: %s\n%s", msg, tool::kUsage);
  return 1;
}

std::string lower_ext(const std::string& path) {
  std::string ext = path.substr(std::min(path.find_last_of('.'), path.size()));
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

bool load(const std::string& path, bool sensor, bool verbose, std::vector<Mesh>& meshes, std::string& err) {
  const size_t n = meshes.size();
  const std::string ext = lower_ext(path);
  const auto t0 = std::chrono::steady_clock::now();
  bool ok = false;
  if (ext == ".obj") ok = read_obj(path, meshes, err);
  else if (ext == ".dmesh") ok = read_dmesh(path, meshes, err);
  else if (ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz") {
#ifdef DAYLIGHT_USD
    ok = read_usd(path, 'A', -1, meshes, err);
#else
    err = path + ": built without USD: configure with -DDAYLIGHT_USD=ON (preset vs2022-usd) or use bindings/python";
#endif
  } else err = path + ": unknown extension; .obj, .dmesh or .usd/.usda/.usdc/.usdz";
  if (!ok) return false;
  size_t F = 0, P = 0;
  for (size_t i = n; i < meshes.size(); ++i) { meshes[i].sensor = sensor; F += meshes[i].numFaces(); P += meshes[i].numPoints(); }
  if (verbose)
    std::fprintf(stderr, "info: read %s: %zu meshes, %zu faces, %zu points, %.3f s\n", path.c_str(), meshes.size() - n, F, P,
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  return true;
}

#ifdef VIEWS_CLI
// Box [x0,x1] x [y0,y1] x [z0,z1] as 6 outward quads.
void box(Mesh& m, double x0, double y0, double z0, double x1, double y1, double z1) {
  const int b = static_cast<int>(m.numPoints());
  for (int k = 0; k < 8; ++k) m.points.insert(m.points.end(), {(k & 1) ? x1 : x0, (k & 2) ? y1 : y0, (k & 4) ? z1 : z0});
  // corner k: bit0 -> +x, bit1 -> +y, bit2 -> +z; quads wound for outward normals
  static const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {1, 3, 7, 5}, {3, 2, 6, 7}, {2, 0, 4, 6}};
  for (const int* q : quads) {
    m.counts.push_back(4);
    m.indices.insert(m.indices.end(), {b + q[0], b + q[1], b + q[2], b + q[3]});
  }
}
// Grid of nu x nv quads from o along u and v (u x v = the normal).
void grid(Mesh& m, const double o[3], const double u[3], const double v[3], int nu, int nv) {
  const int b = static_cast<int>(m.numPoints());
  for (int j = 0; j <= nv; ++j)
    for (int i = 0; i <= nu; ++i)
      for (int a = 0; a < 3; ++a) m.points.push_back(o[a] + u[a] * i / nu + v[a] * j / nv);
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) {
      const int p = b + j * (nu + 1) + i;
      m.counts.push_back(4);
      m.indices.insert(m.indices.end(), {p, p + 1, p + nu + 2, p + nu + 1});
    }
}
// Synthetic city for views --bench: B x B blocks of 40 m between 20 m streets, one box building per block, 10-60 m high
// (mt19937 seed 42), its facades split into quads as receivers (about nSens); roofs, a ground (split into quads until the
// scene has about nTris triangles) and trees (4 m boxes, every 15 m along the streets and on a 10 m grid in the parks) as
// context. Every 8th block is a park (5 cm above the ground, target park), a 50 m water strip runs along the south edge
// (5 cm up, target water).
void city(size_t nSens, size_t nTris, std::vector<Mesh>& meshes, std::vector<views::Target>& targets) {
  const int B = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(nSens) / 1225))));   // ~1400 quads a building at 2 m
  const double W = 60.0 * B + 20;   // the city: blocks at 20 + 60 i .. 60 + 60 i
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> ht(10, 60);
  std::vector<double> h(static_cast<size_t>(B) * B, 0);
  double area = 0;   // facade area of the buildings
  for (size_t k = 0; k < h.size(); ++k) if (k % 8 != 7) { h[k] = ht(rng); area += 160 * h[k]; }
  const double q = std::sqrt(area / static_cast<double>(std::max<size_t>(nSens, 1)));   // facade quad size
  Mesh facades, roofs, park, trees, water, ground;
  facades.name = "facades"; roofs.name = "roofs"; park.name = "park"; trees.name = "trees"; water.name = "water"; ground.name = "ground";
  roofs.sensor = park.sensor = trees.sensor = water.sensor = ground.sensor = false;
  for (int bj = 0; bj < B; ++bj)
    for (int bi = 0; bi < B; ++bi) {
      const double x0 = 20 + 60.0 * bi, y0 = 20 + 60.0 * bj, H = h[static_cast<size_t>(bj) * B + bi];
      if (H == 0) {   // a park: a lawn and trees on a 10 m grid
        const double o[3] = {x0, y0, 0.05}, u[3] = {40, 0, 0}, v[3] = {0, 40, 0};
        grid(park, o, u, v, 1, 1);
        for (int j = 0; j < 4; ++j)
          for (int i = 0; i < 4; ++i) box(trees, x0 + 3 + 10 * i, y0 + 3 + 10 * j, 2, x0 + 7 + 10 * i, y0 + 7 + 10 * j, 8);
        continue;
      }
      const int cols = std::max(1, static_cast<int>(std::lround(40 / q))), rows = std::max(1, static_cast<int>(std::lround(H / q)));
      const double c[5][2] = {{x0, y0}, {x0 + 40, y0}, {x0 + 40, y0 + 40}, {x0, y0 + 40}, {x0, y0}};   // counterclockwise: outward walls
      for (int w = 0; w < 4; ++w) {
        const double o[3] = {c[w][0], c[w][1], 0}, u[3] = {c[w + 1][0] - c[w][0], c[w + 1][1] - c[w][1], 0}, v[3] = {0, 0, H};
        grid(facades, o, u, v, cols, rows);
      }
      const double o[3] = {x0, y0, H}, u[3] = {40, 0, 0}, v[3] = {0, 40, 0};
      grid(roofs, o, u, v, 1, 1);
    }
  for (int s = 0; s <= B; ++s)   // street trees along the middle of every street, both directions
    for (double t = 7.5; t + 4 < W; t += 15) {
      const double m = 60.0 * s + 10;
      box(trees, t, m - 2, 2, t + 4, m + 2, 8);
      box(trees, m - 2, t, 2, m + 2, t + 4, 8);
    }
  const double wo[3] = {0, -60, 0.05}, wu[3] = {W, 0, 0}, wv[3] = {0, 50, 0};
  grid(water, wo, wu, wv, 1, 1);
  size_t made = 0;
  for (const Mesh* m : {&facades, &roofs, &park, &trees, &water}) made += 2 * m->numFaces();
  const int G = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(nTris > made ? nTris - made : 0) / 2)));
  const double go[3] = {-100, -160, 0}, gu[3] = {W + 200, 0, 0}, gv[3] = {0, W + 260, 0};
  grid(ground, go, gu, gv, G, G);
  if (park.numFaces()) targets.push_back({"park", "park"});
  targets.push_back({"water", "water"});
  for (Mesh* m : {&facades, &roofs, &park, &trees, &water, &ground}) if (m->numFaces()) meshes.push_back(std::move(*m));
  size_t tris = 0;
  for (const Mesh& m : meshes) tris += 2 * m.numFaces();
  std::fprintf(stderr, "bench: %zu facade sensors, %zu triangles, %d x %d blocks\n", meshes[0].numFaces(), tris, B, B);
}
#else
// Synthetic city for --bench: NSENS quads on a flat grid plus random boxes (12 triangles each) as context.
void bench_scene(size_t nSens, size_t nTris, std::vector<Mesh>& meshes) {
  const size_t side = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(nSens))));
  const double extent = 1000.0, cell = extent / static_cast<double>(side);
  Mesh grid;
  grid.name = "bench_grid";
  for (size_t j = 0; j <= side; ++j)
    for (size_t i = 0; i <= side; ++i) {
      grid.points.push_back(-extent / 2 + cell * static_cast<double>(i));
      grid.points.push_back(-extent / 2 + cell * static_cast<double>(j));
      grid.points.push_back(0.0);
    }
  for (size_t j = 0; j < side && grid.counts.size() < nSens; ++j)
    for (size_t i = 0; i < side && grid.counts.size() < nSens; ++i) {
      const int a = static_cast<int>(j * (side + 1) + i);
      const int w = static_cast<int>(side + 1);
      grid.counts.push_back(4);
      grid.indices.insert(grid.indices.end(), {a, a + 1, a + w + 1, a + w});
    }
  meshes.push_back(std::move(grid));
  Mesh boxes;
  boxes.name = "bench_boxes";
  boxes.sensor = false;
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> pos(-extent / 2, extent / 2), fp(5.0, 30.0), ht(5.0, 60.0);
  const size_t nBox = nTris / 12;
  for (size_t b = 0; b < nBox; ++b) {
    const double x0 = pos(rng), y0 = pos(rng), w = fp(rng), d = fp(rng), h = ht(rng);
    const int base = static_cast<int>(boxes.points.size() / 3);
    for (int k = 0; k < 8; ++k) {
      boxes.points.push_back(x0 + ((k & 1) ? w : 0.0));
      boxes.points.push_back(y0 + ((k & 2) ? d : 0.0));
      boxes.points.push_back(0.1 + ((k & 4) ? h : 0.0));
    }
    // corner k: bit0 -> +x, bit1 -> +y, bit2 -> +z; quads wound for outward normals
    static const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {1, 3, 7, 5}, {3, 2, 6, 7}, {2, 0, 4, 6}};
    for (const int* q : quads) {
      boxes.counts.push_back(3); boxes.indices.insert(boxes.indices.end(), {base + q[0], base + q[1], base + q[2]});
      boxes.counts.push_back(3); boxes.indices.insert(boxes.indices.end(), {base + q[0], base + q[2], base + q[3]});
    }
  }
  for (size_t f = 0; f < boxes.counts.size(); ++f) boxes.slot.push_back(static_cast<uint16_t>(f % 16));   // a slotted GAS, as app meshes
  meshes.push_back(std::move(boxes));
}
#endif

// values: one line per element, one column per name x side (or a float64 [N x C] .npy); FILE.json manifest
int write_values(const Result& res, const std::string& out, const std::string& manifest, bool verbose) {
  const auto t1 = std::chrono::steady_clock::now();
  const bool npy = lower_ext(out) == ".npy";
  std::FILE* f = out.empty() ? stdout : std::fopen(out.c_str(), npy ? "wb" : "w");
  if (!f) { std::fprintf(stderr, "error: cannot write %s\n", out.c_str()); return 2; }
  const size_t C = res.columns.size(), N = C ? res.values.size() / C : 0;
  if (npy) {
    if (!npy_write_header(f, N, C, "<f8") || std::fwrite(res.values.data(), 8, res.values.size(), f) != res.values.size()) {
      std::fprintf(stderr, "error: cannot write %s\n", out.c_str()); return 2;
    }
  } else {
    static char buf[1 << 16];
    char* p = buf;
    for (size_t e = 0; e < N; ++e) {
      for (size_t c = 0; c < C; ++c) {
        if (p + 32 > std::end(buf)) { std::fwrite(buf, 1, static_cast<size_t>(p - buf), f); p = buf; }
        if (c) *p++ = ' ';
        p = std::to_chars(p, std::end(buf), res.values[e * C + c], std::chars_format::general, 10).ptr;   // == "%.10g"
      }
      *p++ = '\n';
    }
    std::fwrite(buf, 1, static_cast<size_t>(p - buf), f);
  }
  const auto output_secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count(); };
  if (f == stdout) { std::fflush(f); if (verbose) std::fprintf(stderr, "info: output %.3f s\n", output_secs()); return 0; }
  std::fclose(f);
  std::FILE* m = std::fopen((out + ".json").c_str(), "w");
  if (!m) { std::fprintf(stderr, "error: cannot write %s.json\n", out.c_str()); return 2; }
  std::fwrite(manifest.data(), 1, manifest.size(), m);
  std::fclose(m);
  if (verbose) std::fprintf(stderr, "info: output %.3f s\n", output_secs());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  tool::Args a;
  std::string err;
  if (tool::parse(argc, argv, a, err)) return usage_error(err.c_str());
  if (a.help) { std::fputs(tool::kUsage, stdout); return 0; }
  if (a.sensors.empty() && !a.bench) return usage_error("no mesh given");

#ifndef VIEWS_CLI
  Weather weather;
  bool haveWeather = false;
  if (const int rc = cli::prepare(a, weather, haveWeather, err)) {
    if (rc == 1) return usage_error(err.c_str());
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return rc;
  }
#endif

  std::vector<Mesh> meshes;
  if (a.bench) {
#ifdef VIEWS_CLI
    city(a.benchSens, a.benchTris, meshes, a.opt.targets);
    a.opt.verbose = true;
#else
    bench_scene(a.benchSens, a.benchTris, meshes);
    a.opt.verbose = true;
    if (!haveWeather) { a.opt.quantities = {Quantity::SunHours}; if (std::isnan(a.opt.lat)) { a.opt.lat = 42.35; a.opt.lon = -71.07; a.opt.tz = -5; } }
    else a.opt.quantities = {Quantity::Radiation, Quantity::SunHours};
    std::fprintf(stderr, "bench: %zu sensors, %zu triangles\n", a.benchSens, a.benchTris);
#endif
  } else {
    for (const std::string& p : a.sensors)
      if (!load(p, true, a.opt.verbose, meshes, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
    for (const std::string& p : a.context)
      if (!load(p, false, a.opt.verbose, meshes, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
  }
  // names known now
#ifdef VIEWS_CLI
  std::vector<char> open;
  std::vector<std::string> groups;
  std::vector<uint8_t> cls;
  if (!resolve_no_occlude(meshes, a.opt.noOcclude, open, err) || !resolve_targets(views_of(meshes), a.opt.targets, a.opt.selfOcclude, open, groups, cls, err))
    return usage_error(err.c_str());
#else
  if (std::vector<Slot> slots; !resolve_optics(meshes, a.opt.optics, slots, err)) return usage_error(err.c_str());
  if (std::vector<char> open; !resolve_no_occlude(meshes, a.opt.noOcclude, open, err)) return usage_error(err.c_str());
#endif

  const auto t0 = std::chrono::steady_clock::now();
  Result res;
#ifdef VIEWS_CLI
  if (!views::run(meshes, a.opt, res, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
  const char* what = "scene, GPU, columns";
#else
  if (!run(meshes, haveWeather ? &weather : nullptr, a.opt, res, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
  const char* what = "sky model, GPU, output";
#endif
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (a.opt.verbose || a.bench) std::fprintf(stderr, "info: run %.2f s total (incl. %s)\n", secs, what);
  if (a.bench) return 0;
#ifdef VIEWS_CLI
  views::Field f;
  views::field(views_of(meshes), a.opt, f, err);   // the run passed the same checks
  return write_values(res, a.out, tool::manifest_json(res, a.opt, f), a.opt.verbose);
#else
  return write_values(res, a.out, tool::manifest_json(res), a.opt.verbose);
#endif
}
