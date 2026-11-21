// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// views: the field (grid, cell weights), target labels on the shared scene, the chunked GPU loop, the columns and the
// ray dump.
#include "views.h"
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <new>
#include <unordered_map>

namespace daylight::views {
namespace {

using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
using Clk = std::chrono::steady_clock;
double ms(Clk::time_point t0) { return std::chrono::duration<double, std::milli>(Clk::now() - t0).count(); }
constexpr double kDeg = 3.14159265358979323846 / 180;
std::string reach_name(double D) { char s[48]; std::snprintf(s, sizeof s, "reach_%g", D); return s; }

}  // namespace

bool field(const std::vector<SimulationMeshView>& meshes, const Options& opt, Field& f, std::string& err) {
  if (!(opt.fovH > 0 && opt.fovH <= 180) && opt.fovH != 360) { err = "--fov H must be in (0, 180] (about the normal) or 360 (all around)"; return false; }
  const bool flat = opt.elevLo == 0 && opt.elevHi == 0;   // one row of horizontal rays: the 2D isovist
  if (!flat && !(opt.elevLo >= -90 && opt.elevLo < opt.elevHi && opt.elevHi <= 90)) {
    err = "--fov H,LO,HI needs -90 <= LO < HI <= 90, or LO = HI = 0 (horizontal rays only)";
    return false;
  }
  if (!(opt.cell > 0) || std::isinf(opt.cell)) { err = "--cell must be positive and finite (degrees)"; return false; }
  if (!std::isfinite(opt.north) || !std::isfinite(opt.offset)) { err = "--north and --offset must be finite"; return false; }
  if (!optics_rows_ok(opt.optics, err)) return false;
  const double nAz = std::max(1.0, std::round(opt.fovH / opt.cell)), nEl = std::max(1.0, std::round((opt.elevHi - opt.elevLo) / opt.cell));
  if (nAz * nEl > double(1 << 20)) {
    char s[160];
    std::snprintf(s, sizeof s, "--fov / --cell: %.0f x %.0f rays per sensor exceed 2^20 (1048576); use a coarser --cell", nEl, nAz);
    err = s;
    return false;
  }
  if (!(opt.farDist > 0.1) || std::isinf(opt.farDist)) { err = "--far must be more than 0.1 (metres) and finite"; return false; }
  if (opt.reach.size() > 8) { err = "--reach takes at most 8 distances"; return false; }
  for (size_t k = 0; k < opt.reach.size(); ++k) {
    if (!(opt.reach[k] > 0) || std::isinf(opt.reach[k])) { err = "--reach distances must be positive (metres) and finite"; return false; }
    for (size_t j = 0; j < k; ++j)
      if (reach_name(opt.reach[j]) == reach_name(opt.reach[k])) { err = "--reach distances must differ: two make column " + reach_name(opt.reach[k]); return false; }
  }
  if (std::isinf(opt.groundZ)) { err = "--ground-z must be finite"; return false; }
  const unsigned dist = opt.distance != DistAuto ? opt.distance : opt.fovH == 360 ? unsigned(DistIsovist) : unsigned(DistMean | DistMin);
  if ((dist & DistGroup) && !(dist & (DistMean | DistMin))) { err = "--distance group adds per-group columns to mean and min: name one of them"; return false; }
  if ((dist & DistIsovist) && (nAz < 3 || (!flat && nEl < 2))) {
    char s[256];
    std::snprintf(s, sizeof s, "--distance isovist needs at least 3 columns%s, the field has %.0f x %.0f: a finer --cell, --fov H,0,0 (the area) or --distance mean,min",
                  flat ? "" : " and 2 rows (a mesh)", nEl, nAz);
    err = s;
    return false;
  }
  f.nAz = unsigned(nAz);
  f.nEl = unsigned(nEl);
  f.dist = dist;
  f.flat = flat;
  f.groundZ = opt.groundZ;
  if (std::isnan(f.groundZ))   // the lowest point: fmin skips the NAN start
    for (const SimulationMeshView& v : meshes)
      for (size_t i = 2; i < v.mesh->points.size(); i += 3) f.groundZ = std::fmin(f.groundZ, v.mesh->points[i]);
  return true;
}

// The run itself; `stage` names what it is doing, for the out-of-memory message of run() below; `dumping` once the
// --dump-rays files exist.
static bool run_views(const std::vector<SimulationMeshView>& meshes, const Options& opt, Result& out, std::string& err, const char*& stage,
                      bool& dumping) {
  const auto cancelled = [&] { if (opt.cancel && opt.cancel->load()) { err = "cancelled"; return true; } return false; };
  out = Result{};   // nSky, nSun and durationHours stay 0
  // ---- options and roles -------------------------------------------------------------------------------------------
  Field fd;
  std::vector<char> open;              // per mesh: --no-occlude names it
  std::vector<std::string> groups;     // target groups, first appearance
  std::vector<uint8_t> clsOf;          // per mesh: 2 obstruction, 3 + its target group
  if (!field(meshes, opt, fd, err) || !resolve_no_occlude(meshes, opt.noOcclude, open, err) ||
      !resolve_targets(meshes, opt.targets, opt.selfOcclude, open, groups, clsOf, err)) return false;
  const auto blocks = [&](size_t k) { return !meshes[k].sensor || (opt.selfOcclude && !open[k]); };   // mesh k goes into the GAS
  // glass: with --transmission on rays pass the global slots that are glass with tau > 0 (resolve_optics, as daylight
  // numbers them: each mesh's base is the slot count of the meshes before it); per blocking mesh, all or some faces
  std::vector<Slot> slots;
  if (!resolve_optics(meshes, opt.optics, slots, err)) return false;
  std::vector<size_t> base(meshes.size() + 1, 0);
  for (size_t k = 0; k < meshes.size(); ++k) base[k + 1] = base[k] + slot_count(*meshes[k].mesh);
  const auto passes = [&](size_t s) { return opt.transmission && (slots[s].flags & kSlotGlass) && slots[s].tau > 0; };
  std::vector<char> passAll(meshes.size(), 0), passAny(meshes.size(), 0);
  bool anyPass = false;
  for (size_t k = 0; opt.transmission && k < meshes.size(); ++k) {
    if (!blocks(k)) continue;   // not in the scene anyway
    const SimulationMesh& m = *meshes[k].mesh;
    size_t n = 0;
    for (size_t f = 0; f < m.numFaces(); ++f) n += passes(base[k] + (m.slot.empty() ? 0 : m.slot[f])) ? 1 : 0;
    passAny[k] = n > 0;
    passAll[k] = n > 0 && n == m.numFaces();
    anyPass = anyPass || n > 0;
    if (clsOf[k] != 2 && passAll[k]) {
      err = "--target " + meshes[k].name + ": rays pass through it (glass with --transmission on), none can end on it";
      return false;
    }
  }
  // daylight's key of an opaque scene: a views run shares its GAS whatever the targets (labels are per run)
  const std::string key = opt.sceneKey.empty() ? "" : opt.sceneKey + open_key(open, opt.selfOcclude);
  const size_t nS = opt.sides == Sides::Both ? 2 : 1;
  size_t nElem = 0;
  for (const SimulationMeshView& v : meshes) if (v.sensor) nElem += opt.onVertices ? v.mesh->numPoints() : v.mesh->numFaces();
  const bool iso = fd.dist & DistIsovist, mean = fd.dist & DistMean, minimum = fd.dist & DistMin, perGroup = fd.dist & DistGroup;
  const size_t nSens = nElem * nS, K = size_t(fd.nAz) * fd.nEl, nClass = 3 + groups.size(),
               NA = 3 * nClass + 1 + opt.reach.size() + (opt.transmission ? 1 : 0) + (iso ? 1 : 0);
  if (nSens == 0) { err = "no sensor elements (no sensor meshes, or none with faces / points)"; return false; }
  if (!opt.dumpPath.empty() && nSens * K * 5 > (size_t(4) << 30)) {
    err = "--dump-rays: " + std::to_string(nSens) + " sensors x " + std::to_string(K) + " rays exceed 4 GB (fewer receivers or a coarser --cell)";
    return false;
  }

  // ---- the grid: rows top-down from HI, columns clockwise; a cell weighs its solid angle (a flat field's: its angle),
  // the field sums to 1 ---------------------------------------------------------------------------------------------
  const bool world = opt.fovH == 360;
  const double dAz = opt.fovH / fd.nAz, dEl = (opt.elevHi - opt.elevLo) / fd.nEl;
  std::vector<float> rowSin(2 * size_t(fd.nEl));   // per row: sin of its bottom, top elevation
  std::vector<double> wRow(fd.nEl);
  double sum = 0;
  for (unsigned k = 0; k < fd.nEl; ++k) {
    const double top = std::sin((opt.elevHi - k * dEl) * kDeg), bot = std::sin((opt.elevHi - (k + 1) * dEl) * kDeg);
    rowSin[2 * k] = float(bot);
    rowSin[2 * k + 1] = float(top);
    wRow[k] = dAz * kDeg * (fd.flat ? 1 : top - bot);
    sum += wRow[k] * fd.nAz;
  }
  for (double& w : wRow) w /= sum;

  // classes uint8 [nSens x K] and true distances float32 [nSens x K], sensor-major (the device's chunks transposed)
  File cf(nullptr, std::fclose), df(nullptr, std::fclose);
  const std::string depthPath = opt.dumpPath + ".depth.npy";
  if (!opt.dumpPath.empty()) {
    dumping = true;
    cf.reset(std::fopen(opt.dumpPath.c_str(), "wb"));
    if (!cf || !npy_write_header(cf.get(), nSens, K, "<u1")) { err = "cannot write " + opt.dumpPath; return false; }
    df.reset(std::fopen(depthPath.c_str(), "wb"));
    if (!df || !npy_write_header(df.get(), nSens, K, "<f4")) { err = "cannot write " + depthPath; return false; }
  }

  // ---- GPU: the scene (resident or built) and its labels, the sensors, the result --------------------------------------
  Gpu* g = nullptr;
  Scene sc;
  if (!scene_acquire(meshes, blocks, {Slot{}}, false, key, opt.residentScenes, opt.cancel, &g, sc, out, stage, err)) return false;
  // label[t] = the class of a hit on triangle t, from the scene's build order: a target's triangles are the ones the
  // scene holds under its name for its SimulationMesh (a resident scene may have been built from another mesh order); a target the
  // scene lacks so (built under other names, or its name now on another mesh): a fresh build under the key. Passed glass:
  // kLabelPass, a mixed mesh's triangles found by triangulating it again as the scene did (same centre, same faces kept)
  std::vector<uint8_t> label;
  const auto labelled = [&] {
    label.assign(sc.nTris, 2);
    std::unordered_map<std::string, const SceneMesh*> byName;   // null: the scene has the name twice
    for (const SceneMesh& s : sc.order) if (!byName.emplace(s.name, &s).second) byName[s.name] = nullptr;
    for (size_t k = 0; k < meshes.size(); ++k) {
      if (clsOf[k] == 2 && !passAny[k]) continue;
      const auto it = byName.find(meshes[k].name);
      if (it == byName.end() || !it->second || it->second->mesh != meshes[k].mesh) return false;
      const SceneMesh& s = *it->second;
      std::fill_n(label.data() + s.tri0, s.nTri, passAll[k] ? kLabelPass : clsOf[k]);
      if (passAny[k] && !passAll[k]) {
        Tris tr;
        int skipped = 0;
        triangulate(*meshes[k].mesh, sc.c, tr, skipped, uint32_t(base[k]), true);
        if (tr.slot.size() != s.nTri) return false;
        for (size_t t = 0; t < s.nTri; ++t) if (passes(tr.slot[t])) label[s.tri0 + t] = kLabelPass;
      }
    }
    return true;
  };
  stage = "labelling the targets and the glass";
  if ((!groups.empty() || anyPass) && !labelled()) {
    scene_forget(key);
    if (!scene_acquire(meshes, blocks, {Slot{}}, false, key, opt.residentScenes, opt.cancel, &g, sc, out, stage, err)) return false;
    if (!labelled()) { err = "internal: a target or glass mesh is not in the scene as built"; return false; }
  }
  const size_t nPass = size_t(std::count(label.begin(), label.end(), kLabelPass));
  const GpuStats st0 = gpu_stats(*g);   // the GPU's counters before this run's trace (they add up over the process)
  // Beside the scene: the sensors, the labels, and per sensor of a chunk its classes (1 B per ray), distances (4 B) and
  // sums (NA doubles); chunkBytes caps the distances and the sums
  size_t chunkBytes = opt.chunkBytes;
  const size_t perSensor = std::max(4 * K, NA * sizeof(double));
  if (!run_admission(*g, nSens, perSensor, 5 * K + NA * sizeof(double), label.size(), opt.residentScenes, chunkBytes, err)) return false;

  stage = "making the sensors";
  const auto tSens = Clk::now();
  Sensors sens;
  std::string warn;
  if (!sensors_of(meshes, opt, false, sc.c, nSens, sens, out, warn, err)) return false;
  out.stats.prepMs += ms(tSens);
  const float plane = float(fd.groundZ - sc.c[2]);   // the virtual ground, recentred
  if (opt.verbose) {
    char vg[48] = "off";
    if (opt.virtualGround) std::snprintf(vg, sizeof vg, "at z %g", fd.groundZ);
    std::fprintf(stderr, "info: %zu sensors, %zu triangles%s, %u x %u = %zu rays per sensor, %zu target groups, virtual ground %s\n", nSens,
                 sc.nTris, out.sceneReused ? " (scene reused)" : "", fd.nEl, fd.nAz, K, groups.size(), vg);
    if (opt.transmission) std::fprintf(stderr, "info: --transmission on: rays pass %zu glass triangles\n", nPass);
    if (sc.skipped) std::fprintf(stderr, "warn: %d degenerate faces skipped in occluders\n", sc.skipped);
    if (!warn.empty()) std::fprintf(stderr, "warn: %s\n", warn.c_str());
    const size_t below = opt.virtualGround ? size_t(std::count_if(sens.p.begin(), sens.p.end(), [&](const float3& p) { return p.z < plane; })) : 0;
    if (below) std::fprintf(stderr, "warn: %zu sensors lie below the virtual ground: their downward rays end at 0.1 m, the others pass up through it\n", below);
  }
  if (!gpu_set_scene(*g, sens, {}, err) || !gpu_set_slots(*g, {Slot{}}, err) || !gpu_set_labels(*g, label, err)) return false;
  sens = Sensors{};   // on the device now
  label = {};

  // ---- the result: N elements x (columns x sides), written chunk by chunk --------------------------------------------
  stage = "allocating the result";
  std::vector<std::string> names = {"sky", "ground", "obstruction"};
  names.insert(names.end(), groups.begin(), groups.end());
  if (opt.transmission) names.push_back("glazing");   // the share that passed glass, outside the shares' partition
  if (iso) names.push_back(fd.flat ? "area" : "volume");
  if (mean) names.push_back("distance");
  if (minimum) names.push_back("distance_min");
  for (size_t k = 0; perGroup && k < groups.size(); ++k) {
    if (mean) names.push_back(groups[k] + "_distance");
    if (minimum) names.push_back(groups[k] + "_distance_min");
  }
  for (double D : opt.reach) names.push_back(reach_name(D));
  out.columns.clear();
  for (const std::string& n : names)
    for (size_t s = 0; s < nS; ++s) out.columns.push_back(n + ((opt.sides == Sides::Back || (opt.sides == Sides::Both && s == 1)) ? "_back" : "_front"));
  const size_t C = out.columns.size();
  out.values.clear();
  out.f32.clear();
  if (opt.f32Columns) out.f32.assign(C * nElem, 0.f);
  else out.values.assign(nElem * C, 0.0);
  // sensor si (element si / nS, side si % nS) from its sums a (gpu_reduce_view): shares of the whole field (own-surface
  // rays are obstructions without a distance), log means exp(sum w ln d / sum w) (every ray at the far clamp, min d =
  // far: farDist exactly, not to double rounding), minima (+inf: none, NaN; the float clamps as given)
  const double nearF = 0.1f, farF = float(opt.farDist);
  const auto least = [&](double m) { return std::isinf(m) ? NAN : m <= nearF ? 0.1 : m >= farF ? opt.farDist : m; };
  const auto store = [&](size_t si, const double* a) {
    const size_t e = si / nS;
    size_t col = si % nS;
    const auto put = [&](double v) {
      if (opt.f32Columns) out.f32[col * nElem + e] = float(v);
      else out.values[e * C + col] = v;
      col += nS;
    };
    const auto logMean = [&](double w, double wl, double m) { return !(w > 0) ? NAN : m >= farF ? opt.farDist : std::exp(wl / w); };
    const double own = a[3 * nClass];
    double w = 0, wl = 0, mn = INFINITY;
    for (size_t c = 0; c < nClass; ++c) { w += a[3 * c]; wl += a[3 * c + 1]; mn = std::min(mn, a[3 * c + 2]); }
    const double total = w + own;
    put(a[0] / total);
    put(a[3] / total);
    put((a[6] + own) / total);
    for (size_t k = 0; k < groups.size(); ++k) put(a[3 * (3 + k)] / total);
    if (opt.transmission) put(a[3 * nClass + 1 + opt.reach.size()] / total);
    if (iso) put(a[NA - 1]);
    if (mean) put(logMean(w, wl, mn));
    if (minimum) put(least(mn));
    for (size_t k = 0; perGroup && k < groups.size(); ++k) {
      if (mean) put(logMean(a[3 * (3 + k)], a[3 * (3 + k) + 1], a[3 * (3 + k) + 2]));
      if (minimum) put(least(a[3 * (3 + k) + 2]));
    }
    for (size_t r = 0; r < opt.reach.size(); ++r) put(a[3 * nClass + 1 + r] / total);
  };

  stage = "tracing";
  const size_t ncMax = std::max<size_t>(1, chunkBytes / perSensor);
  ViewLaunch vl;
  vl.nAz = fd.nAz; vl.nEl = fd.nEl; vl.az0 = float(world ? 0 : -opt.fovH / 2); vl.dAz = float(dAz); vl.rowSin = rowSin.data();
  vl.world = world; vl.cull = world && opt.offset == 0 && !fd.flat; vl.jitter = opt.jitter; vl.back = opt.sides == Sides::Back;
  vl.vground = opt.virtualGround; vl.nS = unsigned(nS); vl.groundZ = plane; vl.north = opt.north; vl.cancel = opt.cancel;
  const unsigned flags = gpu_bounce_flags(opt.cullBackfaces);   // closest hits, no any-hit
  std::vector<float> reach;   // compared with the float distances
  for (double D : opt.reach) reach.push_back(float(D));
  std::vector<double> sums;
  std::vector<uint8_t> clsBuf;
  std::vector<float> distBuf;
  size_t launch = 0;
  for (size_t i0 = 0; i0 < nSens; i0 += ncMax) {
    if (cancelled()) return false;
    const size_t nc = std::min(ncMax, nSens - i0);
    if (!gpu_view(*g, i0, nc, flags, vl, launch, err)) return false;
    const auto tRed = Clk::now();
    sums.resize(nc * NA);
    if (!gpu_reduce_view(*g, nc, wRow, unsigned(nClass), opt.farDist, reach, opt.transmission, iso ? (fd.flat ? 2u : 3u) : 0u, sums.data(), err)) return false;
    for (size_t i = 0; i < nc; ++i) store(i0 + i, sums.data() + i * NA);
    out.stats.reduceMs += ms(tRed);
    if (opt.progress) opt.progress->store(double(i0 + nc) / double(nSens));
    if (cf) {
      if (!gpu_download_view(*g, nc, clsBuf, distBuf, err)) return false;
      if (std::fwrite(clsBuf.data(), 1, clsBuf.size(), cf.get()) != clsBuf.size()) { err = "write failed: " + opt.dumpPath; return false; }
      if (std::fwrite(distBuf.data(), 4, distBuf.size(), df.get()) != distBuf.size()) { err = "write failed: " + depthPath; return false; }
    }
  }
  const GpuStats st = gpu_stats(*g);
  out.stats.traceMs = st.traceMs - st0.traceMs;
  out.stats.rays = st.rays - st0.rays;
  out.stats.launches = st.launches - st0.launches;
  out.stats.gasBytes = st.gasBytes;
  if (opt.verbose)
    std::fprintf(stderr, "info: GAS %.1f MB build %.0f ms, %zu launches, %.3g rays in %.0f ms (%.2f GRays/s), reduce %.0f ms, chunk cap %.0f MB\n",
                 st.gasBytes / 1048576.0, st.buildMs, out.stats.launches, double(out.stats.rays), out.stats.traceMs,
                 out.stats.traceMs > 0 ? double(out.stats.rays) / out.stats.traceMs / 1e6 : 0.0, out.stats.reduceMs, chunkBytes / 1048576.0);
  return true;
}

bool run(const std::vector<SimulationMeshView>& meshes, const Options& opt, Result& out, std::string& err) {
  // as daylight's run(): running out of host memory fails the run, never the caller
  const char* stage = "preparing the run";
  bool dumping = false, ok = false;
  try {
    ok = run_views(meshes, opt, out, err, stage, dumping);
  } catch (const std::bad_alloc&) {
    out = Result{};
    err = std::string("out of memory: host memory ran out while ") + stage;
  } catch (const std::exception& e) {
    out = Result{};
    err = std::string("internal error while ") + stage + ": " + e.what();
  }
  // a failed run leaves no dump (closed by now): its headers promise rows it never wrote
  if (!ok && dumping) { std::remove(opt.dumpPath.c_str()); std::remove((opt.dumpPath + ".depth.npy").c_str()); }
  return ok;
}

bool run(const std::vector<SimulationMesh>& meshes, const Options& opt, Result& out, std::string& err) {
  return run(views_of(meshes), opt, out, err);
}

}  // namespace daylight::views
