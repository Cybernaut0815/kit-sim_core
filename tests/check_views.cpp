// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// Checks for views (views.h). Runs with --jitter off against a host model of the same cells, traced in double from the
// device's float sensor origins (every ray's class and distance through --dump-rays, then every column): facades,
// tilted, turned, flat and nearly flat faces, vertices, --north, an eye 10 m over a 2 km target quad (360), the virtual
// ground's height and switch, a screen nearer than 0.1 m, a wall past --far, sensors in the plane of a 2 km quad,
// rectangle targets with group distances and --reach (also past --far). Then the horizon, degenerate faces, the jitter
// (in its cells), chunks and sub-launches, progress, f32 columns, sides, a scene shared with daylight, labels on reused,
// renamed, reordered and name-swapped resident scenes, cancel (no dump left), ray counts, the option, target and CLI
// rules, the manifest's escaping and the dump size guard; the isovist (area, volume) against the host model and known
// answers. Self-skips without a CUDA device.
#include "cli.h"
#include "internal.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace daylight;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static const double kDeg = 3.14159265358979323846 / 180;
static const double kSky = 0.4490987851112869;   // sin 50 / (sin 50 + sin 70): the default field's share above the horizon
static std::string tmpPath;                       // the runs' dumps, under tests/ (removed after each run)

static SimulationMesh mesh(const char* name, bool sensor) { SimulationMesh m; m.name = name; m.sensor = sensor; return m; }
// Appends a face over the given corners (x y z each), counterclockwise seen from the side it faces.
static void face(SimulationMesh& m, std::initializer_list<double> xyz) {
  const int b = int(m.numPoints());
  m.points.insert(m.points.end(), xyz);
  m.counts.push_back(int(xyz.size() / 3));
  for (int k = 0; k < m.counts.back(); ++k) m.indices.push_back(b + k);
}
static SimulationMesh quad(const char* name, bool sensor, std::initializer_list<double> xyz) { SimulationMesh m = mesh(name, sensor); face(m, xyz); return m; }
// A 2 x 2 m face from corner (x, y, z), facing -y turned az degrees towards +x and tilted tilt degrees up.
static void tilted(SimulationMesh& m, double x, double y, double z, double az, double tilt) {
  const double n[3] = {std::sin(az * kDeg) * std::cos(tilt * kDeg), -std::cos(az * kDeg) * std::cos(tilt * kDeg), std::sin(tilt * kDeg)};
  const double e[3] = {std::cos(az * kDeg), std::sin(az * kDeg), 0};   // along the bottom edge
  const double v[3] = {n[1] * e[2] - n[2] * e[1], n[2] * e[0] - n[0] * e[2], n[0] * e[1] - n[1] * e[0]};   // n x e: up the face
  face(m, {x, y, z, x + 2 * e[0], y + 2 * e[1], z, x + 2 * (e[0] + v[0]), y + 2 * (e[1] + v[1]), z + 2 * v[2], x + 2 * v[0], y + 2 * v[1], z + 2 * v[2]});
}

// The data of a .npy file after its header.
static bool npy(const std::string& path, std::vector<char>& data) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::vector<char> b;
  char buf[1 << 16];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) b.insert(b.end(), buf, buf + n);
  std::fclose(f);
  const size_t h = b.size() < 10 ? SIZE_MAX : 10 + (size_t(uint8_t(b[8])) | size_t(uint8_t(b[9])) << 8);   // magic, version, length, header
  if (h > b.size()) return false;
  data.assign(b.begin() + std::ptrdiff_t(h), b.end());
  return true;
}

// trace_dev.cu's offset_ray on the host: where the device starts the rays of a sensor at p with normal n.
static float3 offset_ray(float3 p, float3 n) {
  const auto nudge = [](float x, float nx) {
    if (std::fabs(x) < 1.f / 32.f) return x + nx / 65536.f;
    int32_t b;
    std::memcpy(&b, &x, 4);
    b += (x < 0.f ? -1 : 1) * int(256.f * nx);
    std::memcpy(&x, &b, 4);
    return x;
  };
  return float3{nudge(p.x, n.x), nudge(p.y, n.y), nudge(p.z, n.z)};
}

// A blocking face of the host model: axis-aligned (lo[axis] == hi[axis]), recentred, in float as the device has it.
// pass: glass the rays pass (--transmission on).
struct Quad { double lo[3], hi[3]; int axis; uint8_t cls; bool pass = false; };

// Where a ray ends: its class, distance (inf: open or own surface), the cosine between it and the normal of the surface
// it ends on (a direction error moves the end by 1 / that) and how far off that surface the hardware may meet it:
// 2^-22 x the quad's extent, what own_plane in trace_dev.cu allows (4x the hardware's error); 0 on the virtual plane.
// glazed: it passed glass on the way.
struct End { uint8_t cls = 255; double t = INFINITY, cosInc = 1, off = 0; bool glazed = false; };

// Whether rays pass face f of m: --transmission on and its --optics row (NAME/SLOT > NAME > *) is glass with TAU > 0.
static bool glass_face(const views::Options& o, const SimulationMesh& m, size_t f) {
  if (!o.transmission) return false;
  const int s = m.slot.empty() ? 0 : m.slot[f];
  const Optics* best = nullptr;
  int rank = -1;
  for (const Optics& r : o.optics) {
    const int k = r.mesh == m.name ? (r.slot == s ? 2 : r.slot < 0 ? 1 : -1) : r.mesh == "*" ? 0 : -1;
    if (k > rank) { rank = k; best = &r; }
  }
  return best && best->glass && best->tau > 0;
}

// The device's ray from o along unit d (n: the sensor's normal), as views.h defines it: 360 at offset 0 culls the
// sensor's back (254), the closest quad, else sky on or above the horizon, ground below it, ended by the virtual ground (to
// 1 cm past its plane; the eye on or below it: 0.1 m). margin shrinks to how close (radians) the ray passes a quad's
// edge, the horizon or, culled, the sensor's plane.
static End trace(const double o[3], const double d[3], const double n[3], bool cull, bool vground, double plane,
                 const std::vector<Quad>& quads, double& margin) {
  End e;
  if (cull) {
    const double nd = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
    margin = std::min(margin, std::fabs(nd));
    if (nd <= 0) { e.cls = 254; return e; }
  }
  const bool down = vground && d[2] < 0;
  if (down && !(o[2] > plane)) { e.cls = 1; e.t = 0.1; return e; }
  double tmax = down ? (o[2] - plane + 0.01) / -d[2] : INFINITY, tPass = INFINITY;
  const double tmax0 = tmax;
  for (const Quad& q : quads) {
    const int a = q.axis;
    const double tq = (q.lo[a] - o[a]) / d[a];
    if (!(tq > 0) || std::isinf(tq)) continue;
    double in = INFINITY, out = 0;   // the hit point's distance to the quad's edges (inside), or a lower bound of it to the quad
    for (int b = 0; b < 3; ++b)
      if (b != a) {
        const double p = o[b] + tq * d[b];
        in = std::min({in, p - q.lo[b], q.hi[b] - p});
        out = std::max({out, q.lo[b] - p, p - q.hi[b]});
      }
    margin = std::min(margin, (out > 0 ? out : in) / tq);
    if (out <= 0 && q.pass) { tPass = std::min(tPass, tq); continue; }   // glass: passed, if it lies before the end
    if (out <= 0 && tq < tmax) {
      tmax = tq;
      e = {q.cls, tq, std::fabs(d[a]), 0};
      for (int b = 0; b < 3; ++b) e.off = std::max(e.off, (q.hi[b] - q.lo[b]) * 0x1p-22);
    }
  }
  if (e.cls != 255) { e.glazed = tPass < e.t; return e; }
  e.glazed = tPass < tmax0;
  if (d[2] != 0) margin = std::min(margin, std::fabs(d[2]));   // exactly on it (a flat field's rays): sky by rule, no margin
  e.cls = d[2] >= 0 ? 0 : 1;
  if (down) { e.t = (o[2] - plane) / -d[2]; e.cosInc = -d[2]; }
  return e;
}

// One views run with --jitter off and its dump against the host model: the field's cells (centres in azimuth and in sine
// of elevation, rows top-down from HI, columns clockwise from the left edge, weights = solid angles) from each sensor's
// frame (forward = normal, right = horizontal or, within 1e-3 of straight up or down, east turned by --north and made
// square to the normal, up = right x forward; 360: true north, east, up), traced from the device's origin (the sensors of
// make_sensors_into, offset_ray)
// against the blocking meshes' faces. Every ray's class must match and its end lie within tol x its distance (plus End::off)
// of the host's, measured across the surface it ends on; every column as views.h defines it (shares and reach 1e-9,
// distances tol relative). margin: how close any ray comes to another class (radians off a quad's edge, the horizon or the
// culled plane; relative to a reach distance), which must stay well above the device's float error.
static Result expect(const char* what, const std::vector<SimulationMesh>& meshes, views::Options o, double tol = 1e-6) {
  o.jitter = false;
  o.dumpPath = tmpPath;
  Result r;
  std::string err;
  std::vector<char> cb, db;
  const bool ran = views::run(meshes, o, r, err) && npy(o.dumpPath, cb) && npy(o.dumpPath + ".depth.npy", db);
  std::remove(o.dumpPath.c_str());
  std::remove((o.dumpPath + ".depth.npy").c_str());
  if (!ran) { std::printf("views: %s: %s\n", what, err.c_str()); ++failures; return r; }

  // the scene as the device has it: recentred by the bbox centre, in float; the plane at the lowest point or --ground-z
  double c[3];
  bbox_centre(views_of(meshes), c);
  double low = o.groundZ;
  for (const SimulationMesh& m : meshes)
    for (size_t i = 2; std::isnan(o.groundZ) && i < m.points.size(); i += 3) low = std::fmin(low, m.points[i]);
  const double plane = float(low - c[2]);
  std::vector<std::string> groups;   // in order of first appearance
  for (const views::Target& tg : o.targets) if (std::find(groups.begin(), groups.end(), tg.group) == groups.end()) groups.push_back(tg.group);
  std::vector<Quad> quads;
  std::vector<float3> P, N;
  const size_t nS = o.sides == Sides::Both ? 2 : 1;
  for (const SimulationMesh& m : meshes) {
    uint8_t cl = 2;
    for (const views::Target& tg : o.targets)
      if (tg.mesh == m.name) cl = uint8_t(3 + (std::find(groups.begin(), groups.end(), tg.group) - groups.begin()));
    const bool open = std::find(o.noOcclude.begin(), o.noOcclude.end(), m.name) != o.noOcclude.end();
    if (!m.sensor || (o.selfOcclude && !open))
      for (size_t f = 0, at = 0; f < m.numFaces(); at += size_t(m.counts[f++])) {
        Quad q{{INFINITY, INFINITY, INFINITY}, {-INFINITY, -INFINITY, -INFINITY}, -1, cl, glass_face(o, m, f)};
        for (size_t j = at; j < at + size_t(m.counts[f]); ++j)
          for (int a = 0; a < 3; ++a) {
            const double v = float(m.points[3 * size_t(m.indices[j]) + size_t(a)] - c[a]);
            q.lo[a] = std::min(q.lo[a], v);
            q.hi[a] = std::max(q.hi[a], v);
          }
        for (int a = 0; a < 3; ++a) if (q.lo[a] == q.hi[a]) q.axis = a;
        CHECK(q.axis >= 0);   // the host model knows axis-aligned faces only
        if (q.axis >= 0) quads.push_back(q);
      }
    if (m.sensor) {
      const size_t n = (o.onVertices ? m.numPoints() : m.numFaces()) * nS;
      P.resize(P.size() + n);
      N.resize(N.size() + n);
      std::string warn;
      make_sensors_into(m, o, false, c, P.data() + P.size() - n, N.data() + N.size() - n, warn);
    }
  }

  // the grid; a flat field (LO = HI = 0) weighs its rays by angle; the distance columns (DistAuto: isovist in 360 mode)
  const bool world = o.fovH == 360, flat = o.elevLo == 0 && o.elevHi == 0;
  const unsigned dist = o.distance != views::DistAuto ? o.distance : world ? unsigned(views::DistIsovist) : unsigned(views::DistMean | views::DistMin);
  const unsigned nAz = unsigned(std::max(1.0, std::round(o.fovH / o.cell))), nEl = unsigned(std::max(1.0, std::round((o.elevHi - o.elevLo) / o.cell)));
  const size_t K = size_t(nAz) * nEl, nSens = P.size(), nG = groups.size(), C = r.columns.size();
  const double dAz = o.fovH / nAz, dEl = (o.elevHi - o.elevLo) / nEl, az0 = world ? 0 : -o.fovH / 2;
  std::vector<double> sRow(nEl), wRow(nEl);
  double sum = 0;
  for (unsigned k = 0; k < nEl; ++k) {
    const double top = std::sin((o.elevHi - k * dEl) * kDeg), bot = std::sin((o.elevHi - (k + 1) * dEl) * kDeg);
    sRow[k] = (top + bot) / 2;
    wRow[k] = flat ? 1 : top - bot;
    sum += wRow[k] * nAz;
  }
  for (double& w : wRow) w /= sum;
  CHECK(cb.size() == nSens * K && db.size() == 4 * nSens * K && C % nS == 0 && r.values.size() == C * (nSens / nS));
  if (!(cb.size() == nSens * K && db.size() == 4 * nSens * K && C % nS == 0 && r.values.size() == C * (nSens / nS))) return r;

  const double cosN = std::cos(o.north * kDeg), sinN = std::sin(o.north * kDeg);
  size_t badRays = 0, badCols = 0;
  double worstRay = 0, worstCol = 0, worstIso = 0, margin = INFINITY;
  std::vector<std::array<double, 3>> U(K);   // per ray: its direction in the field's frame (x right, y forward, z up)
  std::vector<double> L(K);                 // ... and where it ends: min(t, far), own surface 0 (at the eye)
  for (size_t i = 0; i < nSens; ++i) {
    const float3 of = offset_ray(P[i], N[i]);
    const double O[3] = {of.x, of.y, of.z}, n[3] = {N[i].x, N[i].y, N[i].z}, l = std::hypot(n[0], n[1]);
    double f[3] = {-sinN, cosN, 0}, rt[3] = {cosN, sinN, 0}, u[3] = {0, 0, 1};
    if (!world) {
      std::copy(n, n + 3, f);
      if (l > 1e-3) { rt[0] = n[1] / l; rt[1] = -n[0] / l; rt[2] = 0; }
      else if (l > 0) {
        const double en = rt[0] * n[0] + rt[1] * n[1];
        for (int b = 0; b < 3; ++b) rt[b] -= en * n[b];
        const double len = std::sqrt(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
        for (double& x : rt) x /= len;
      }
      u[0] = rt[1] * f[2] - rt[2] * f[1]; u[1] = rt[2] * f[0] - rt[0] * f[2]; u[2] = rt[0] * f[1] - rt[1] * f[0];
    }
    std::vector<double> w(3 + nG), wl(3 + nG), mn(3 + nG, INFINITY), rc(o.reach.size());
    double own = 0, gz = 0;
    for (size_t d = 0; d < K; ++d) {
      const double s = sRow[d / nAz], cs = std::sqrt(1 - s * s), a = (az0 + (double(d % nAz) + 0.5) * dAz) * kDeg;
      double dir[3];
      for (int b = 0; b < 3; ++b) dir[b] = cs * std::sin(a) * rt[b] + cs * std::cos(a) * f[b] + s * u[b];
      const End h = trace(O, dir, n, world && o.offset == 0 && !flat, o.virtualGround, plane, quads, margin);
      U[d] = {cs * std::sin(a), cs * std::cos(a), s};
      L[d] = h.cls == 254 ? 0 : std::min(h.t, o.farDist);
      // the device's ray: its class, and how far its end lies from the host's across the surface, in tolerances
      const uint8_t gc = uint8_t(cb[i * K + d]);
      float gt;
      std::memcpy(&gt, db.data() + 4 * (i * K + d), 4);
      const double dt = std::isinf(h.t) ? (std::isinf(gt) ? 0 : INFINITY) : std::fabs(gt - h.t) * h.cosInc / (tol * h.t + h.off);
      if (gc != (h.glazed ? h.cls | kViewGlazed : h.cls) || dt > 1) {
        if (badRays++ < 3)
          std::printf("views: %s: sensor %zu row %zu column %zu: class %d at %.9g m, host %d at %.9g m\n", what, i, d / nAz, d % nAz, gc, double(gt), h.cls, h.t);
      } else worstRay = std::max(worstRay, dt);
      // the columns' sums
      const double wd = wRow[d / nAz];
      if (h.cls == 254) { own += wd; continue; }
      if (h.glazed) gz += wd;
      const double dd = std::min(std::max(h.t, 0.1), o.farDist);
      w[h.cls] += wd;
      wl[h.cls] += wd * std::log(dd);
      mn[h.cls] = std::min(mn[h.cls], dd);
      for (size_t q = 0; q < rc.size(); ++q) {
        if (h.t >= o.reach[q]) rc[q] += wd;
        if (!std::isinf(h.t)) margin = std::min(margin, std::fabs(h.t / o.reach[q] - 1));
      }
    }
    // the isovist: the polygon (flat) or the mesh through the rays' ends, tetrahedra eye-A-B-C and eye-B-D-C per quad
    double iso = 0;
    const auto det = [&](size_t a, size_t b, size_t c) {
      const auto& x = U[a]; const auto& y = U[b]; const auto& z = U[c];
      return x[0] * (y[1] * z[2] - y[2] * z[1]) + x[1] * (y[2] * z[0] - y[0] * z[2]) + x[2] * (y[0] * z[1] - y[1] * z[0]);
    };
    for (size_t d = 0; d < K; ++d) {
      const size_t row = d / nAz, col = d % nAz, d1 = d - col + (col + 1) % nAz, d2 = d + nAz, d3 = d1 + nAz;
      if (col + 1 == nAz && !world) continue;
      if (flat) iso += 0.5 * L[d] * L[d1] * (U[d][1] * U[d1][0] - U[d][0] * U[d1][1]);
      else if (row + 1 < nEl) iso += (L[d] * L[d1] * L[d2] * det(d, d1, d2) + L[d1] * L[d3] * L[d2] * det(d1, d3, d2)) / 6;
    }
    // the columns, in views.h's order: shares (obstruction_total after the three classes), glazing, volume / area, distance, distance_min, per group, reach_<D>
    double total = own, W = 0, WL = 0, M = INFINITY;
    for (size_t k = 0; k < w.size(); ++k) { total += w[k]; W += w[k]; WL += wl[k]; M = std::min(M, mn[k]); }
    const auto logMean = [](double sw, double swl) { return sw > 0 ? std::exp(swl / sw) : NAN; };
    const auto least = [](double m) { return std::isinf(m) ? NAN : m; };
    std::vector<double> want = {w[0] / total, w[1] / total, (w[2] + own) / total, (total - w[0]) / total};   // the last: obstruction_total, all but the sky
    for (size_t g = 0; g < nG; ++g) want.push_back(w[3 + g] / total);
    if (o.transmission) want.push_back(gz / total);   // glazing
    const size_t shares = want.size(), isoAt = dist & views::DistIsovist ? shares : SIZE_MAX;
    if (dist & views::DistIsovist) want.push_back(iso);
    if (dist & views::DistMean) want.push_back(logMean(W, WL));
    if (dist & views::DistMin) want.push_back(least(M));
    for (size_t g = 0; (dist & views::DistGroup) && g < nG; ++g) {
      if (dist & views::DistMean) want.push_back(logMean(w[3 + g], wl[3 + g]));
      if (dist & views::DistMin) want.push_back(least(mn[3 + g]));
    }
    const size_t dists = want.size();
    for (double x : rc) want.push_back(x / total);
    CHECK(want.size() * nS == C);
    for (size_t k = 0; k < want.size() && want.size() * nS == C; ++k) {
      const size_t col = k * nS + i % nS;
      const double got = r.values[(i / nS) * C + col], x = want[k];
      const bool isoCol = k == isoAt, relative = k >= shares && k < dists;
      // the isovist: the device's float directions (det of neighbours ~1e-6 relative per tetrahedron) at most 1e-5 off
      const double e = std::isnan(x) || std::isnan(got) ? (std::isnan(x) && std::isnan(got) ? 0 : INFINITY)
                       : relative ? std::fabs(got - x) / std::max(std::fabs(x), 1e-300) : std::fabs(got - x);
      if (e > (isoCol ? std::max(tol, 1e-5) : relative ? tol : 1e-9) && !(x == 0 && got == 0)) {
        if (badCols++ < 3) std::printf("views: %s: sensor %zu %s %.17g, host %.17g\n", what, i, r.columns[col].c_str(), got, x);
      } else if (isoCol) worstIso = std::max(worstIso, x == 0 ? 0 : e);
      else if (relative) worstCol = std::max(worstCol, e);
    }
  }
  std::printf("views: %s: %zu rays end as the host traces them (at most %.2f of the tolerance off), columns within %.1e, isovist %.1e, margin %.1e\n",
              what, nSens * K, worstRay, worstCol, worstIso, margin);
  CHECK(badRays == 0 && badCols == 0 && margin > 1e-6);   // the device's directions are float, ~2e-8 rad off
  return r;
}

static double at(const Result& r, size_t row, const char* column) {   // a row's value of a column (NaN: no such row or column)
  const auto it = std::find(r.columns.begin(), r.columns.end(), column);
  const size_t C = r.columns.size();
  return it == r.columns.end() || (row + 1) * C > r.values.size() ? NAN : r.values[row * C + size_t(it - r.columns.begin())];
}
static bool same(const Result& a, const Result& b) {   // the same columns and values, bit for bit (NaN = NaN)
  if (a.columns != b.columns || a.values.size() != b.values.size()) return false;
  for (size_t k = 0; k < a.values.size(); ++k)
    if (!(a.values[k] == b.values[k] || (std::isnan(a.values[k]) && std::isnan(b.values[k])))) return false;
  return true;
}
static int parse(const std::vector<const char*>& args, views::cli::Args& a, std::string& err) {   // views <args>
  std::vector<char*> argv{const_cast<char*>("views")};
  for (const char* s : args) argv.push_back(const_cast<char*>(s));
  a = views::cli::Args{};
  err.clear();
  return views::cli::parse(int(argv.size()), argv.data(), a, err);
}

// The rules run() and the CLI enforce before any GPU work, each with the words of its message.
static void check_rules() {
  std::vector<SimulationMesh> ms = {quad("s", true, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2}), quad("dup", false, {0, -5, 0, 1, -5, 0, 1, -5, 1, 0, -5, 1})};
  ms.push_back(ms.back());
  for (int k = 0; k < 17; ++k) {
    ms.push_back(quad("t", false, {double(k), -8, 0, k + 0.5, -8, 0, k + 0.5, -8, 1, double(k), -8, 1}));
    ms.back().name += std::to_string(k);
  }
  struct Rule { const char* words; void (*set)(views::Options&); };
  const Rule rules[] = {
      {"--fov H must be in (0, 180]", [](views::Options& o) { o.fovH = 200; }},
      {"--fov H must be in (0, 180]", [](views::Options& o) { o.fovH = 0; }},
      {"needs -90 <= LO < HI <= 90", [](views::Options& o) { o.elevLo = 50; }},
      {"needs -90 <= LO < HI <= 90", [](views::Options& o) { o.elevHi = 91; }},
      {"or LO = HI = 0", [](views::Options& o) { o.elevLo = o.elevHi = 10; }},   // flat only at the horizon
      {"isovist needs at least 3 columns and 2 rows", [](views::Options& o) { o.fovH = 360; o.elevLo = -1; o.elevHi = 1; }},   // one ring
      {"isovist needs at least 3 columns", [](views::Options& o) { o.elevLo = o.elevHi = 0; o.fovH = 4; o.distance = views::DistIsovist; }},
      {"group adds per-group columns", [](views::Options& o) { o.distance = views::DistGroup | views::DistIsovist; }},
      {"--cell must be positive", [](views::Options& o) { o.cell = 0; }},
      {"--cell must be positive", [](views::Options& o) { o.cell = INFINITY; }},
      {"--north and --offset must be finite", [](views::Options& o) { o.north = NAN; }},
      {"--north and --offset must be finite", [](views::Options& o) { o.offset = INFINITY; }},
      {"exceed 2^20", [](views::Options& o) { o.fovH = 360; o.elevLo = -90; o.elevHi = 90; o.cell = 0.1; }},
      {"--far must be more than 0.1", [](views::Options& o) { o.farDist = 0.1; }},
      {"--far must be more than 0.1", [](views::Options& o) { o.farDist = INFINITY; }},
      {"at most 8", [](views::Options& o) { o.reach = {1, 2, 3, 4, 5, 6, 7, 8, 9}; }},
      {"must be positive", [](views::Options& o) { o.reach = {6, 0}; }},
      {"must be positive", [](views::Options& o) { o.reach = {INFINITY}; }},
      {"must differ: two make column reach_6", [](views::Options& o) { o.reach = {6, 20, 6.0000001}; }},   // one %g name
      {"--ground-z must be finite", [](views::Options& o) { o.groundZ = -INFINITY; }},
      {"face-slot targets", [](views::Options& o) { o.targets = {{"t0/1", "a"}}; }},
      {"letters, digits, _ and -", [](views::Options& o) { o.targets = {{"t0", "a b"}}; }},
      {"letters, digits, _ and -", [](views::Options& o) { o.targets = {{"t0", ""}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "sky"}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "ground"}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "obstruction"}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "distance_x"}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "reach6"}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "a"}, {"t1", "a_distance"}}; }},   // a's distance column
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "a_distance_min"}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "volume"}}; }},   // the isovist's column names
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "area"}}; }},
      {"no mesh of that name", [](views::Options& o) { o.targets = {{"nope", "a"}}; }},
      {"several meshes have that name", [](views::Options& o) { o.targets = {{"dup", "a"}}; }},
      {"a target twice", [](views::Options& o) { o.targets = {{"t0", "a"}, {"t1", "b"}, {"t0", "b"}}; }},
      {"blocks no rays", [](views::Options& o) { o.targets = {{"s", "a"}}; o.selfOcclude = false; }},
      {"blocks no rays", [](views::Options& o) { o.targets = {{"s", "a"}}; o.noOcclude = {"s"}; }},
      {"more than 16 target groups", [](views::Options& o) {
         for (int k = 0; k < 17; ++k) o.targets.push_back({"t" + std::to_string(k), "g" + std::to_string(k)});
       }},
      {"no mesh of that name", [](views::Options& o) { o.noOcclude = {"nope"}; }},
  };
  for (const Rule& rule : rules) {
    views::Options o;
    rule.set(o);
    Result r;
    std::string err;
    const bool ok = views::run(ms, o, r, err);
    CHECK(!ok && err.find(rule.words) != std::string::npos);
    if (ok || err.find(rule.words) == std::string::npos) std::printf("views: rule \"%s\": %s\n", rule.words, ok ? "ran" : err.c_str());
  }
  {
    std::vector<SimulationMesh> context = {ms[1]};
    views::Options o;
    Result r;
    std::string err;
    CHECK(!views::run(context, o, r, err) && err.find("no sensor elements") != std::string::npos);
  }
  // the CLI: --target lists, --distance (group adds to mean and min; none alone), --fov H alone keeps LO,HI, en17037
  views::cli::Args a;
  std::string err;
  CHECK(parse({"f.obj", "--target", "a=water;b=park", "--target", "c=water"}, a, err) == 0 && a.opt.targets.size() == 3 &&
        a.opt.targets[0].mesh == "a" && a.opt.targets[0].group == "water" && a.opt.targets[1].mesh == "b" && a.opt.targets[1].group == "park" &&
        a.opt.targets[2].mesh == "c");
  for (const char* bad : {"a", "=x", "a=", "a=x;"}) CHECK(parse({"f.obj", "--target", bad}, a, err) == 1 && err.find("NAME=GROUP") != std::string::npos);
  CHECK(parse({"f.obj", "--distance", "group"}, a, err) == 1);
  CHECK(parse({"f.obj", "--distance", "none,mean"}, a, err) == 1 && parse({"f.obj", "--distance", "mean,max"}, a, err) == 1);
  CHECK(parse({"f.obj", "--distance", "none"}, a, err) == 0 && a.opt.distance == 0);
  CHECK(parse({"f.obj", "--distance", "min,group"}, a, err) == 0 && a.opt.distance == (views::DistMin | views::DistGroup));
  CHECK(parse({"f.obj", "--distance", "isovist,mean"}, a, err) == 0 && a.opt.distance == (views::DistIsovist | views::DistMean));
  CHECK(parse({"f.obj", "--distance", "volume"}, a, err) == 1 && parse({"f.obj", "--distance", "isovist,group"}, a, err) == 1);
  CHECK(parse({"f.obj"}, a, err) == 0 && a.opt.distance == views::DistAuto && a.opt.jitter && a.opt.virtualGround &&
        a.opt.fovH == 120 && a.opt.elevLo == -70 && a.opt.elevHi == 50 && a.opt.cell == 2 && a.opt.farDist == 1000 && a.opt.reach.empty());
  CHECK(parse({"f.obj", "--fov", "360"}, a, err) == 0 && a.opt.fovH == 360 && a.opt.elevLo == -70 && a.opt.elevHi == 50);
  // DistAuto: the isovist all around, mean and min about the normal; a flat field (H,0,0), one row
  {
    views::Field fd;
    views::Options fo;
    CHECK(views::field({}, fo, fd, err) && fd.dist == (views::DistMean | views::DistMin) && !fd.flat && fd.nEl == 60);
    fo.fovH = 360;
    CHECK(views::field({}, fo, fd, err) && fd.dist == views::DistIsovist);
    CHECK(parse({"f.obj", "--fov", "360,0,0"}, a, err) == 0 && a.opt.elevLo == 0 && a.opt.elevHi == 0 && views::field({}, a.opt, fd, err) && fd.flat &&
          fd.nEl == 1 && fd.nAz == 180);
    CHECK(parse({"f.obj", "--fov", "360,-1,1"}, a, err) == 1 && err.find("isovist") != std::string::npos);   // one ring: no mesh
    CHECK(parse({"f.obj", "--fov", "360,-1,1", "--distance", "mean"}, a, err) == 0);
  }
  CHECK(parse({"f.obj", "--fov", "360,-30,30", "--cell", "1"}, a, err) == 0 && a.opt.elevLo == -30 && a.opt.elevHi == 30 && a.opt.cell == 1);
  CHECK(parse({"f.obj", "--reach", "en17037"}, a, err) == 0 && a.opt.reach == std::vector<double>({6, 20, 50}));
  CHECK(parse({"f.obj", "--jitter", "off", "--virtual-ground", "off", "--ground-z", "-2.5", "--far", "300"}, a, err) == 0 && !a.opt.jitter &&
        !a.opt.virtualGround && a.opt.groundZ == -2.5 && a.opt.farDist == 300);
  CHECK(parse({"f.obj", "--north", "-30.5", "--offset", "1.6"}, a, err) == 0 && a.opt.north == -30.5 && a.opt.offset == 1.6);
  for (const std::vector<const char*>& bad : std::vector<std::vector<const char*>>{{"f.obj", "--fov", "1,2"}, {"f.obj", "--fov", "200"}, {"f.obj", "--jitter", "no"},
                                                                                  {"f.obj", "--far", "0.1"}, {"f.obj", "--cell", "x"}, {"f.obj", "--reach", "6,,20"},
                                                                                  {"f.obj", "--fov", "360,-90,90", "--cell", "0.1"}, {"f.obj", "--fov", "120,,50"},
                                                                                  {"f.obj", "--cell", "inf"}, {"f.obj", "--reach", "6,6"}, {"f.obj", "--offset", "1,6"},
                                                                                  {"f.obj", "--offset", "1e999"}, {"f.obj", "--north", "abc"}, {"f.obj", "--north", "nan"}})
    CHECK(parse(bad, a, err) == 1);
  // the manifest writes names as JSON strings: quote, backslash and control bytes escaped
  {
    Result r;
    r.ranges = {{"w\"1\\x\x01", 1}};
    views::Options mo;
    mo.targets = {{"t\"0", "g"}};
    const std::string j = views::cli::manifest_json(r, mo, views::Field{});
    CHECK(j.find("{\"name\": \"w\\\"1\\\\x\\u0001\", \"first\": 0, \"count\": 1}") != std::string::npos &&
          j.find("{\"mesh\": \"t\\\"0\", \"group\": \"g\"}") != std::string::npos);
  }
}

// Glass (--optics ... glass + --transmission on: rays pass it, glazing_<side> = the share that did) and measure meshes the
// rays ignore (--no-occlude), against the host model.
static void check_glass(std::string& err) {
  // a facade looking -y, a pane 2.3 m out, a park wall 9.1 m out
  const SimulationMesh facade = quad("s", true, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2});
  const SimulationMesh pane = quad("pane", false, {-1.7, -2.3, 0.4, 2.9, -2.3, 0.4, 2.9, -2.3, 2.6, -1.7, -2.3, 2.6});
  const SimulationMesh park = quad("park", false, {-6.2, -9.1, 0, 7.3, -9.1, 0, 7.3, -9.1, 5.3, -6.2, -9.1, 5.3});
  const std::vector<SimulationMesh> ms = {facade, pane, park};
  views::Options o;
  o.targets = {{"park", "green"}};
  o.optics = {{"pane", -1, 0, 0.64, true}};
  const Result off = expect("glass, --transmission off: the pane blocks", ms, o);
  views::Options on = o;
  on.transmission = true;
  const Result through = expect("glass, --transmission on: the park behind the pane", ms, on);
  CHECK(at(through, 0, "glazing_front") > 0.05 && at(through, 0, "green_front") > at(off, 0, "green_front") &&
        std::isnan(at(off, 0, "glazing_front")));
  views::Options porous = on;   // porous slots stay opaque: only glass is passed
  porous.optics = {{"pane", -1, 0.2, 0.3, false}};
  const Result rp = expect("porous, --transmission on: the pane still blocks", ms, porous);
  CHECK(at(rp, 0, "glazing_front") == 0 && at(rp, 0, "green_front") == at(off, 0, "green_front"));
  {   // a mixed mesh: face slot 1 glass, slot 0 opaque (its triangles found by triangulating it again)
    SimulationMesh wall = mesh("wall", false);
    face(wall, {-1.7, -2.3, 0.4, 0.6, -2.3, 0.4, 0.6, -2.3, 2.6, -1.7, -2.3, 2.6});
    face(wall, {0.6, -2.3, 0.4, 2.9, -2.3, 0.4, 2.9, -2.3, 2.6, 0.6, -2.3, 2.6});
    wall.slot = {0, 1};
    views::Options os = on;
    os.optics = {{"wall", 1, 0, 0.64, true}};
    const Result rs = expect("glass slot 1 of a two-slot wall", {facade, wall, park}, os);
    CHECK(at(rs, 0, "glazing_front") > 0 && at(rs, 0, "glazing_front") < at(through, 0, "glazing_front"));
  }
  {   // double glazing: two panes passed
    SimulationMesh pane2 = pane;
    pane2.name = "pane2";
    for (size_t i = 1; i < pane2.points.size(); i += 3) pane2.points[i] = -2.47;
    views::Options od = on;
    od.optics = {{"pane", -1, 0, 0.64, true}, {"pane2", -1, 0, 0.64, true}};
    const Result rd = expect("double glazing", {facade, pane, pane2, park}, od);
    CHECK(at(rd, 0, "green_front") == at(through, 0, "green_front"));
  }
  {   // rules: a target all of glass, a reserved group, an unknown --optics name
    Result r;
    views::Options t = on;
    t.targets = {{"pane", "water"}};
    CHECK(!views::run(ms, t, r, err) && err.find("rays pass through it") != std::string::npos);
    t.transmission = false;
    CHECK(views::run(ms, t, r, err) && at(r, 0, "water_front") > 0);
    t = on;
    t.targets = {{"park", "glazing"}};
    CHECK(!views::run(ms, t, r, err) && err.find("reserved") != std::string::npos);
    t = on;
    t.optics = {{"nosuch", -1, 0, 0.5, true}};
    CHECK(!views::run(ms, t, r, err) && err.find("nosuch") != std::string::npos);
    views::cli::Args a;
    CHECK(parse({"f.obj", "--optics", "pane=0,0.64,glass", "--transmission", "on"}, a, err) == 0 && a.opt.transmission &&
          a.opt.optics.size() == 1 && a.opt.optics[0].glass);
    CHECK(parse({"f.obj", "--transmission", "maybe"}, a, err) == 1 && err.find("--transmission") != std::string::npos);
    CHECK(parse({"f.obj", "--optics", "pane=0.2,0.5,glass"}, a, err) == 1 && err.find("RHO must be 0") != std::string::npos);
  }
  {   // labels are per run: off then on under one key reuse the scene, and give the keyless numbers
    views::Options k1 = o, k2 = on;
    k1.sceneKey = k2.sceneKey = "check_glass";
    k1.jitter = k2.jitter = false;   // as expect() ran them
    Result r1, r2;
    CHECK(views::run(ms, k1, r1, err) && views::run(ms, k2, r2, err) && r2.sceneReused && same(r2, through) && same(r1, off));
  }

  // an interior: a 6.3 x 5.1 x 2.9 m room with a window in its south wall, a tree outside, a measure grid at 0.8 m (the
  // eye 0.4 m above it), all around: --no-occlude grid keeps the grid out of the rays
  SimulationMesh room = mesh("room", false);
  face(room, {0, 0, 0, 6.3, 0, 0, 6.3, 5.1, 0, 0, 5.1, 0});
  face(room, {0, 0, 2.9, 0, 5.1, 2.9, 6.3, 5.1, 2.9, 6.3, 0, 2.9});
  face(room, {0, 5.1, 0, 6.3, 5.1, 0, 6.3, 5.1, 2.9, 0, 5.1, 2.9});
  face(room, {0, 0, 0, 0, 5.1, 0, 0, 5.1, 2.9, 0, 0, 2.9});
  face(room, {6.3, 0, 0, 6.3, 0, 2.9, 6.3, 5.1, 2.9, 6.3, 5.1, 0});
  face(room, {0, 0, 0, 6.3, 0, 0, 6.3, 0, 0.93, 0, 0, 0.93});
  face(room, {0, 0, 2.21, 6.3, 0, 2.21, 6.3, 0, 2.9, 0, 0, 2.9});
  face(room, {0, 0, 0.93, 2.07, 0, 0.93, 2.07, 0, 2.21, 0, 0, 2.21});
  face(room, {4.13, 0, 0.93, 6.3, 0, 0.93, 6.3, 0, 2.21, 4.13, 0, 2.21});
  const SimulationMesh window = quad("window", false, {2.07, 0, 0.93, 4.13, 0, 0.93, 4.13, 0, 2.21, 2.07, 0, 2.21});
  const SimulationMesh tree = quad("tree", false, {0.9, -7.3, 0, 5.2, -7.3, 0, 5.2, -7.3, 4.4, 0.9, -7.3, 4.4});
  SimulationMesh grid = mesh("grid", true);
  for (int j = 0; j < 3; ++j)
    for (int i = 0; i < 4; ++i) {
      const double x = 0.75 + 1.2 * i, y = 0.65 + 1.3 * j;
      face(grid, {x, y, 0.8, x + 1.2, y, 0.8, x + 1.2, y + 1.3, 0.8, x, y + 1.3, 0.8});
    }
  const std::vector<SimulationMesh> rm = {grid, room, window, tree};
  views::Options in;
  in.fovH = 360;
  in.offset = 0.4;
  in.noOcclude = {"grid"};
  in.targets = {{"tree", "green"}};
  in.optics = {{"window", -1, 0, 0.64, true}};
  in.reach = {6};
  in.distance = views::DistIsovist | views::DistMean | views::DistMin;
  const Result shut = expect("interior, --transmission off: the window blocks", rm, in);
  in.transmission = true;
  const Result lit = expect("interior, --transmission on: out through the window", rm, in);
  double sky = 0, green = 0, shutOut = 0;
  for (size_t e = 0; e < 12; ++e) {
    sky += at(lit, e, "sky_front");
    green += at(lit, e, "green_front");
    shutOut += at(shut, e, "sky_front") + at(shut, e, "green_front");
    CHECK(at(lit, e, "glazing_front") > 0);   // every point sees the window, not all of them the tree through it
  }
  CHECK(sky > 0 && green > 0 && shutOut == 0);
  views::Options blocking = in;   // without --no-occlude the grid blocks: the rays down end on it 0.4 m below the eye
  blocking.noOcclude = {};
  const Result rb = expect("interior, the grid blocking", rm, blocking);
  CHECK(at(rb, 5, "distance_min_front") < 0.5 && at(lit, 5, "distance_min_front") > 0.5);
}

// The isovist where the answer is known: open fields (every ray at --far: a regular polygon, a fan, the mesh inscribed in a
// sphere), and tests/room.obj shut from its measure grid (the floor's area at eye height, the room's volume), jittered.
static void check_isovist(const char* testsDir, std::string& err) {
  const SimulationMesh s = quad("s", true, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2});   // a facade looking -y
  {   // flat at --far 50: 180 segments of 2 deg all around, 59 over a 120 deg fan about the normal
    views::Options o;
    o.selfOcclude = false;
    o.fovH = 360; o.elevLo = o.elevHi = 0; o.farDist = 50;
    const Result r = expect("isovist: open, flat all around", {s}, o);
    CHECK(r.columns == std::vector<std::string>({"sky_front", "ground_front", "obstruction_front", "obstruction_total_front", "area_front"}));
    CHECK(at(r, 0, "sky_front") == 1 && std::fabs(at(r, 0, "area_front") / (0.5 * 2500 * 180 * std::sin(2 * kDeg)) - 1) <= 1e-5);
    o.fovH = 120;
    o.distance = views::DistIsovist | views::DistMean;
    const Result f = expect("isovist: open, a flat 120 deg fan", {s}, o);
    CHECK(at(f, 0, "distance_front") == 50 && std::fabs(at(f, 0, "area_front") / (0.5 * 2500 * 59 * std::sin(2 * kDeg)) - 1) <= 1e-5);
  }
  {   // the whole sphere, no virtual ground, --far 10: the mesh inscribed in a 10 m ball (and without its poles' caps)
    views::Options o;
    o.selfOcclude = false;
    o.fovH = 360; o.elevLo = -90; o.elevHi = 90; o.farDist = 10; o.offset = 0.5; o.virtualGround = false;
    const Result r = expect("isovist: open, the whole sphere", {s}, o);
    o.jitter = true;
    Result rj;
    CHECK(views::run({s}, o, rj, err));
    const double ball = 4. / 3 * 3.14159265358979323846 * 1000, v = at(r, 0, "volume_front") / ball, vj = at(rj, 0, "volume_front") / ball;
    std::printf("views: isovist: a 10 m ball at 2 deg: %.6f of 4/3 pi r^3, jittered %.6f\n", v, vj);
    CHECK(std::fabs(v - 1) <= 1e-3 && std::fabs(vj - 1) <= 2e-3);
  }
  {   // the shut room (6.3 x 5.1 x 2.9 m) from its 12 grid points 1.2 m up (convex: each sees all of it)
    std::vector<SimulationMesh> ms, room;
    CHECK(read_obj(std::string(testsDir) + "/room_grid.obj", ms, err) && read_obj(std::string(testsDir) + "/room.obj", room, err));
    for (SimulationMesh& m : room) { m.sensor = false; ms.push_back(m); }
    views::Options o;
    o.fovH = 360; o.offset = 0.4; o.noOcclude = {"grid"};
    o.elevLo = o.elevHi = 0; o.cell = 0.5;
    Result ra, rv;
    CHECK(views::run(ms, o, ra, err));
    o.elevLo = -90; o.elevHi = 90; o.cell = 1;
    CHECK(views::run(ms, o, rv, err));
    double worstA = 0, worstV = 0;
    for (size_t e = 0; e < 12; ++e) {
      worstA = std::max(worstA, std::fabs(at(ra, e, "area_front") / (6.3 * 5.1) - 1));
      worstV = std::max(worstV, std::fabs(at(rv, e, "volume_front") / (6.3 * 5.1 * 2.9) - 1));
    }
    std::printf("views: isovist: the shut room from 12 points: area (0.5 deg) within %.1e, volume (1 deg) within %.1e\n", worstA, worstV);
    CHECK(worstA <= 5e-4 && worstV <= 5e-3);   // measured 1.1e-4, 1.3e-3: the polygon / mesh cuts the room's corners
    o.cell = 3;   // the host model: the whole sphere, the default field, flat
    expect("isovist: the shut room, the whole sphere", ms, o);
    o.elevLo = -70; o.elevHi = 50;
    expect("isovist: the shut room, 360,-70,50", ms, o);
    o.elevLo = o.elevHi = 0;
    expect("isovist: the shut room, flat", ms, o);
  }
}

int check_views(const char* testsDir) {
  tmpPath = std::string(testsDir) + "/_check_views_tmp.npy";
  check_rules();
  std::string err;
  {   // without a GPU nothing below can run
    Result r;
    std::vector<SimulationMesh> one = {quad("s", true, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2})};
    views::Options o;
    o.cell = 30;
    if (!views::run(one, o, r, err)) {
      const bool noGpu = err.rfind("no CUDA device", 0) == 0 || err.find("optixInit") != std::string::npos;
      std::printf("%s: views: %s\n", noGpu ? "skip" : "FAIL", err.c_str());
      return failures + (noGpu ? 0 : 1);
    }
  }

  // 1, 4: faces in an empty scene (--no-self-occlude): a facade 1 m over the lowest point, a roof, a tilted, a turned and a
  // tilted-down face, a ceiling 1.5 m up and a face looking down at the lowest point (its eye, nudged along its normal by
  // offset_ray, a hair below the virtual plane)
  SimulationMesh faces = mesh("faces", true);
  face(faces, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2});
  face(faces, {5, 0, 2, 6, 0, 2, 6, 1, 2, 5, 1, 2});
  tilted(faces, 8, 0, 0.5, 0, 30);
  tilted(faces, 12, 0, 0.5, 30, 0);
  tilted(faces, 16, 0, 1.5, -40, -25);
  face(faces, {5, 2, 1.5, 5, 3, 1.5, 6, 3, 1.5, 6, 2, 1.5});
  face(faces, {5, 4, 0, 5, 5, 0, 6, 5, 0, 6, 4, 0});
  views::Options o1;
  o1.selfOcclude = false;
  const Result r1 = expect("faces in an empty scene", {faces}, o1);
  if (r1.columns.size() == 5) {
    // the facade sees the sky above the horizon of the field (-70..50 deg) and the virtual ground below it, at 1 / sin
    // (the host model); the roof sees only sky, at --far exactly; the face on the plane sees the ground at 0.1 m
    CHECK(std::fabs(at(r1, 0, "sky_front") - kSky) <= 1e-9 && std::fabs(at(r1, 0, "ground_front") - (1 - kSky)) <= 1e-9);
    CHECK(at(r1, 1, "sky_front") == 1 && at(r1, 1, "distance_front") == 1000 && at(r1, 1, "distance_min_front") == 1000);
    CHECK(at(r1, 6, "ground_front") == 1 && std::fabs(at(r1, 6, "distance_front") / 0.1 - 1) <= 1e-9 && at(r1, 6, "distance_min_front") == 0.1);
    for (double v : r1.values) CHECK(std::isfinite(v));
  }
  {   // jittered (o1 keeps the default): each ray anywhere in its cell, and still the same shares, the facade's rows lying
      // wholly above or below the horizon
    Result rj;
    CHECK(views::run({faces}, o1, rj, err) && rj.values.size() == r1.values.size());
    if (rj.values.size() == 35) {
      CHECK(std::fabs(at(rj, 0, "sky_front") - kSky) <= 1e-9 && std::fabs(at(rj, 0, "ground_front") - (1 - kSky)) <= 1e-9);
      CHECK(at(rj, 1, "sky_front") == 1 && at(rj, 6, "ground_front") == 1 && at(rj, 6, "distance_min_front") == 0.1);
      CHECK(at(rj, 0, "distance_front") != at(r1, 0, "distance_front"));   // other rays
      for (double v : rj.values) CHECK(std::isfinite(v));
    }
  }
  {   // --virtual-ground off: the facade's ground rays are open, every ray at --far: 1000 exactly
    views::Options off = o1;
    off.virtualGround = false;
    const Result r = expect("faces, --virtual-ground off", {faces}, off);
    CHECK(std::fabs(at(r, 0, "ground_front") - (1 - kSky)) <= 1e-9 && at(r, 0, "distance_front") == 1000 && at(r, 0, "distance_min_front") == 1000);
  }
  {   // on vertices: the facade's foot lies on the virtual plane, an eye there sees ground at 0.1 m in every downward
      // direction
    views::Options ov = o1;
    ov.onVertices = true;
    expect("vertices in an empty scene", {faces}, ov);
  }
  {   // a screen 5 cm in front of a facade: hits nearer than 0.1 m count as 0.1 m in the log mean
    const std::vector<SimulationMesh> scr = {quad("s", true, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2}),
                                   quad("screen", false, {-5, -0.05, -1, 5, -0.05, -1, 5, -0.05, 3, -5, -0.05, 3})};
    expect("a screen 5 cm in front", scr, views::Options{});
  }
  {   // the horizon is sky: --fov 360,-1,1 at --jitter off puts every ray on it (dir.z = 0); in the open the facade sees
      // sky in front and its own surface behind (360, offset 0), no ray is ground (none points below the horizon)
    views::Options oh = o1;
    oh.fovH = 360; oh.elevLo = -1; oh.elevHi = 1; oh.jitter = false;
    oh.distance = views::DistMean;   // one ring: no isovist mesh
    Result rh;
    CHECK(views::run({faces}, oh, rh, err) && std::fabs(at(rh, 0, "sky_front") - 0.5) <= 1e-12 && std::fabs(at(rh, 0, "obstruction_front") - 0.5) <= 1e-12);
    for (size_t e = 0; e < 7; ++e) CHECK(at(rh, e, "ground_front") == 0);
  }
  {   // a degenerate face (no area, no normal) traces no ray, in view mode and 360 at an offset: all own surface, as 360
      // at offset 0 gives it (obstruction 1, no distance, no reach, an isovist of 0: every ray ends at the eye)
    SimulationMesh deg = mesh("deg", true);
    face(deg, {0, 0, 1, 1, 0, 1, 2, 0, 1});
    face(deg, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2});
    for (const double fov : {120.0, 360.0}) {
      views::Options od = o1;
      od.fovH = fov;
      od.offset = 0.5;
      od.reach = {6};
      od.distance = views::DistIsovist | views::DistMean | views::DistMin;
      Result rd;
      CHECK(views::run({deg}, od, rd, err) && at(rd, 0, "obstruction_front") == 1 && at(rd, 0, "sky_front") == 0 && at(rd, 0, "ground_front") == 0 &&
            std::isnan(at(rd, 0, "distance_front")) && std::isnan(at(rd, 0, "distance_min_front")) && at(rd, 0, "reach_6_front") == 0 &&
            at(rd, 0, "volume_front") == 0 && at(rd, 1, "volume_front") > 0 && at(rd, 1, "sky_front") > 0.4);
    }
  }
  {   // a canopy 3 m over the roof, on one side: what the roof sees of it turns with --north (its right: east turned)
    std::vector<SimulationMesh> canopy = {faces, quad("canopy", false, {6, 1.2, 5, 30, 1.2, 5, 30, 30, 5, 6, 30, 5})};
    views::Options n0 = o1, n30 = o1;
    n30.north = 30;
    const Result a = expect("faces under a canopy", canopy, n0), b = expect("faces under a canopy, --north 30", canopy, n30);
    const double o0 = at(a, 1, "obstruction_front"), o30 = at(b, 1, "obstruction_front");
    CHECK(o0 > 0.01 && o30 > 0.01 && std::fabs(o0 - o30) > 0.01);
    // a roof a hair off flat (its north edge 0.1 mm up: float noise, the normal 1e-4 off vertical) takes the flat roof's
    // frame, so it sees the canopy as the flat one does (its tilt's own frame would turn the field 180 deg)
    SimulationMesh roofs = mesh("roofs", true);
    face(roofs, {5, 0, 2, 6, 0, 2, 6, 1, 2, 5, 1, 2});
    face(roofs, {5, 0, 2, 6, 0, 2, 6, 1, 2.0001, 5, 1, 2.0001});
    const Result rr = expect("a flat and a nearly flat roof under a canopy", {roofs, canopy[1]}, n0);
    CHECK(at(rr, 0, "obstruction_front") > 0.01 && std::fabs(at(rr, 1, "obstruction_front") - at(rr, 0, "obstruction_front")) <= 1e-3);
  }

  // 2: 360 degrees from an eye 10 m over a 2 km target quad (a facade sensor at --offset 0.5: its back is traced too). The
  // quad lies on the virtual plane: every ray below the horizon ends on it (the shallowest 573 m away), at 10 / sin, as
  // the hardware meets so large a triangle (End::off: the shallow rays' ends ~1e-4 m off its plane)
  const double sLow = (std::sin(-70 * kDeg) + std::sin(-68 * kDeg)) / 2;   // the lowest row's middle, in sine
  const SimulationMesh eye = quad("eye", true, {-0.1, 0, 9.9, 0.1, 0, 9.9, 0.1, 0, 10.1, -0.1, 0, 10.1});
  const SimulationMesh lake = quad("lake", false, {-1000, -1000, 0, 1000, -1000, 0, 1000, 1000, 0, -1000, 1000, 0});
  views::Options o2;
  o2.fovH = 360;
  o2.offset = 0.5;
  o2.selfOcclude = false;
  o2.targets = {{"lake", "water"}};
  o2.distance = views::DistIsovist | views::DistMean | views::DistMin;
  const Result r2 = expect("360 over a 2 km target quad", {eye, lake}, o2);
  CHECK(r2.columns == std::vector<std::string>({"sky_front", "ground_front", "obstruction_front", "obstruction_total_front", "water_front", "volume_front",
                                                "distance_front", "distance_min_front"}));
  CHECK(std::fabs(at(r2, 0, "sky_front") - kSky) <= 1e-9 && std::fabs(at(r2, 0, "water_front") - (1 - kSky)) <= 1e-9 && at(r2, 0, "ground_front") == 0);
  CHECK(std::fabs(at(r2, 0, "distance_min_front") / (10 / -sLow) - 1) <= 1e-6);
  {   // --ground-z 5: the plane ends the downward rays 5 m down, the quad below it is hidden (not in view: NaN)
    views::Options gz = o2;
    gz.groundZ = 5;
    gz.distance |= views::DistGroup;
    const Result r = expect("360 over a 2 km target quad, --ground-z 5", {eye, lake}, gz);
    CHECK(at(r, 0, "water_front") == 0 && std::fabs(at(r, 0, "ground_front") - (1 - kSky)) <= 1e-9);
    CHECK(std::fabs(at(r, 0, "distance_min_front") / (5 / -sLow) - 1) <= 1e-6 && std::isnan(at(r, 0, "water_distance_front")) &&
          std::isnan(at(r, 0, "water_distance_min_front")));
  }
  {   // walls 2 km north and south: what was sky there is an obstruction now, its distances still count as --far
    SimulationMesh walls = mesh("walls", false);
    face(walls, {-5000, 2000, -1000, 5000, 2000, -1000, 5000, 2000, 1020, -5000, 2000, 1020});
    face(walls, {-5000, -2000, -1000, -5000, -2000, 1020, 5000, -2000, 1020, 5000, -2000, -1000});
    const Result r = expect("360 over a 2 km target quad, walls 2 km away", {eye, lake, walls}, o2);
    const double ob = at(r, 0, "obstruction_front");
    CHECK(ob > 0.1 && std::fabs(at(r, 0, "sky_front") + ob - kSky) <= 1e-9);
    CHECK(std::fabs(at(r, 0, "distance_front") / at(r2, 0, "distance_front") - 1) <= 1e-6);
  }

  // 5: sensors in the plane of a 2 km context quad, at its centre, near it and 500 m out, 360 at offset 0: the sky above
  // them, their own surface below; float error puts the quad (and their own faces) on rays near the plane, none may end
  // there. Jittered too: rays a hair over the plane
  {
    SimulationMesh spots = mesh("spots", true);
    const double xy[3][2] = {{0, 0}, {0.37, -0.21}, {300, -400}};
    for (const auto& p : xy) face(spots, {p[0] - 0.1, p[1] - 0.1, 0, p[0] + 0.1, p[1] - 0.1, 0, p[0] + 0.1, p[1] + 0.1, 0, p[0] - 0.1, p[1] + 0.1, 0});
    const std::vector<SimulationMesh> plaza = {spots, quad("plaza", false, {-1000, -1000, 0, 1000, -1000, 0, 1000, 1000, 0, -1000, 1000, 0})};
    views::Options o5;
    o5.fovH = 360;
    Result r = expect("own plane: sensors in a 2 km quad", plaza, o5), rj;
    CHECK(views::run(plaza, o5, rj, err));
    for (const Result* x : {&r, &rj})
      for (size_t e = 0; e < 3 && x->values.size() == 15; ++e)
        CHECK(std::fabs(at(*x, e, "sky_front") - kSky) <= 1e-9 && std::fabs(at(*x, e, "obstruction_front") - (1 - kSky)) <= 1e-9 &&
              at(*x, e, "ground_front") == 0);
  }

  // 3, 6, 6b: two targets (the CLI's --target a=water;b=park) in front of a facade: a at 10 m, b at 30 m behind it in part
  {
    views::cli::Args args;
    CHECK(parse({"facade.obj", "--target", "a=water;b=park"}, args, err) == 0);
    const std::vector<SimulationMesh> st = {quad("facade", true, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2}), quad("a", false, {-3, -10, 0.5, 5, -10, 0.5, 5, -10, 4, -3, -10, 4}),
                                  quad("b", false, {8, -30, 0.2, 20, -30, 0.2, 20, -30, 6, 8, -30, 6})};
    views::Options o3 = args.opt;
    o3.distance = views::DistMean | views::DistMin | views::DistGroup;
    o3.reach = {6, 20, 50};
    const Result r = expect("two rectangle targets", st, o3);
    CHECK(r.columns == std::vector<std::string>({"sky_front", "ground_front", "obstruction_front", "obstruction_total_front", "water_front", "park_front",
                                                 "distance_front", "distance_min_front", "water_distance_front", "water_distance_min_front", "park_distance_front",
                                                 "park_distance_min_front", "reach_6_front", "reach_20_front", "reach_50_front"}));
    CHECK(at(r, 0, "water_front") > 0.05 && at(r, 0, "park_front") > 0.005);   // most of b hides behind a
    // 360 at offset 0 (north 30): half the field is the facade's own surface, which reaches nothing; the reach of the
    // rest: sky every distance, ground its plane distance, a target its hit
    views::Options ow = o3;
    ow.fovH = 360;
    ow.north = 30;
    const Result rw = expect("two rectangle targets, 360", st, ow);
    CHECK(std::fabs(at(rw, 0, "obstruction_front") - 0.5) <= 1e-9 && at(rw, 0, "reach_6_front") < 0.5);
    // obstruction_total is all that is not sky: the ground, the obstruction (here the own surface) and both targets
    CHECK(at(rw, 0, "ground_front") > 0 && at(rw, 0, "water_front") > 0 &&
          std::fabs(at(rw, 0, "obstruction_total_front") - (at(rw, 0, "ground_front") + at(rw, 0, "obstruction_front") + at(rw, 0, "water_front") +
                                                              at(rw, 0, "park_front"))) <= 1e-12 &&
          std::fabs(at(rw, 0, "obstruction_total_front") - (1 - at(rw, 0, "sky_front"))) <= 1e-12);
    // --virtual-ground off: nothing nearer than the targets, distance_min is the nearer one's
    views::Options of = o3;
    of.virtualGround = false;
    const Result rf = expect("two rectangle targets, --virtual-ground off", st, of);
    CHECK(at(rf, 0, "distance_min_front") == at(rf, 0, "water_distance_min_front") && at(rf, 0, "water_distance_min_front") < at(rf, 0, "park_distance_min_front"));
    CHECK(at(rf, 0, "water_distance_front") > 10 && at(rf, 0, "water_distance_front") < 12 && at(rf, 0, "park_distance_front") > 30 &&
          at(rf, 0, "park_distance_front") < 40);
    // --far 30 under reach 50: the reach by the true distance (sky reaches 50 m); every ray to b (31 m and more) at the
    // far clamp: its log mean 30 exactly
    views::Options fr = o3;
    fr.farDist = 30;
    const Result r30 = expect("two rectangle targets, --far 30", st, fr);
    CHECK(at(r30, 0, "reach_50_front") > 0.3 && at(r30, 0, "park_distance_front") == 30 && at(r30, 0, "park_distance_min_front") == 30);
    // --distance none: the shares and the reach only
    views::Options on = o3;
    on.distance = 0;
    const Result rn = expect("two rectangle targets, --distance none", st, on);
    CHECK(rn.columns == std::vector<std::string>({"sky_front", "ground_front", "obstruction_front", "obstruction_total_front", "water_front", "park_front",
                                                  "reach_6_front", "reach_20_front", "reach_50_front"}));
    // 16 groups, the most there are: 16 strips 10 m ahead
    std::vector<SimulationMesh> strips = {st[0]};
    views::Options o16;
    o16.distance = views::DistMean | views::DistMin | views::DistGroup;
    for (int k = 0; k < 16; ++k) {
      strips.push_back(quad("s", false, {k - 8.0, -10, 0.3, k - 7.2, -10, 0.3, k - 7.2, -10, 3, k - 8.0, -10, 3}));
      strips.back().name += std::to_string(k);
      o16.targets.push_back({strips.back().name, "g" + std::to_string(k)});
    }
    const Result r16 = expect("16 target groups", strips, o16);
    CHECK(r16.columns.size() == 4 + 16 + 2 + 2 * 16);
  }

  // 7, 8, 9 on tests/box.obj (a ground and a box over it, both sensors; both sides, a target, group distances, reach)
  std::vector<SimulationMesh> m;
  CHECK(read_obj(std::string(testsDir) + "/box.obj", m, err) && m.size() == 2);
  if (m.size() != 2) return failures;
  views::Options o;
  o.sides = Sides::Both;
  o.targets = {{"box", "cube"}};
  o.distance = views::DistMean | views::DistMin | views::DistGroup;
  o.reach = {1, 3};
  Result a, b;
  CHECK(views::run(m, o, a, err) && a.stats.rays == 16u * 2 * 3600);   // 16 faces x 2 sides x 60 x 60 cells
  const size_t C = a.columns.size(), N = C ? a.values.size() / C : 0;
  CHECK(C == 2 * 11 && N == 16);   // per side: 4 shares, obstruction_total, 2 distances, 2 of the group, 2 reach
  {   // one sensor per chunk: the same bits, progress up to 1
    views::Options tiny = o;
    tiny.chunkBytes = 64;
    std::atomic<double> progress{0};
    tiny.progress = &progress;
    CHECK(views::run(m, tiny, b, err) && same(a, b) && b.stats.launches >= 32);
    CHECK(progress.load() == 1.0);
  }
  {   // 841 sensors at cell 6 (38 words: 215 sensors in the first launch, then more): sub-launches seed as chunks do
    std::vector<SimulationMesh> grid = {mesh("grid", true)};
    for (int k = 0; k < 29 * 29; ++k) {
      const double x = k % 29, y = k / 29;
      face(grid[0], {x, y, 0, x + 1, y, 0, x + 1, y + 1, 0, x, y + 1, 0});
    }
    views::Options g6;
    g6.fovH = 360; g6.cell = 6; g6.offset = 1;
    views::Options g1 = g6;
    g1.chunkBytes = 4 * 1200 * 100;   // 100 sensors per chunk: one launch each
    Result whole, chunked;
    CHECK(views::run(grid, g6, whole, err) && views::run(grid, g1, chunked, err) && whole.stats.launches >= 3 && same(whole, chunked));
  }
  {   // f32 columns: float of each value, column-major
    views::Options f32 = o;
    f32.f32Columns = true;
    Result c;
    CHECK(views::run(m, f32, c, err) && c.values.empty() && c.f32.size() == a.values.size());
    bool ok = c.f32.size() == N * C;
    for (size_t e = 0; ok && e < N; ++e)
      for (size_t k = 0; k < C; ++k) {
        const float x = c.f32[k * N + e], y = float(a.values[e * C + k]);
        ok = ok && (x == y || (std::isnan(x) && std::isnan(y)));
      }
    CHECK(ok);
  }
  {   // --sides back: the back columns of --sides both
    views::Options back = o;
    back.sides = Sides::Back;
    Result r;
    CHECK(views::run(m, back, r, err) && r.columns.size() * 2 == C && r.values.size() * 2 == a.values.size());
    bool ok = r.columns.size() * 2 == C && r.values.size() * 2 == a.values.size();
    for (size_t e = 0; ok && e < N; ++e)
      for (size_t k = 0; k < C / 2; ++k) {
        const double x = r.values[e * C / 2 + k], y = a.values[e * C + 2 * k + 1];
        ok = ok && (x == y || (std::isnan(x) && std::isnan(y)));
      }
    CHECK(ok);
    for (size_t k = 0; ok && k < C / 2; ++k) CHECK(r.columns[k] == a.columns[2 * k + 1]);   // named as both's back columns
  }
  // a daylight run, then a views run without targets on one key: daylight's scene serves it
  Options d;
  d.quantities = {Quantity::SunHours};
  d.lat = 42.35; d.lon = -71.07; d.tz = -5; d.t0 = 4116;
  d.sceneKey = "views-box";
  Result rd, rv, rt;
  CHECK(run(m, nullptr, d, rd, err) && !rd.sceneReused);
  views::Options keyed = o;
  keyed.targets.clear();
  keyed.sceneKey = "views-box";
  CHECK(views::run(m, keyed, rv, err) && rv.sceneReused);
  // targets on the reused scene: the labels a keyless run has
  keyed.targets = o.targets;
  CHECK(views::run(m, keyed, rt, err) && rt.sceneReused && same(rt, a));
  {   // meshes renamed under the key: the scene lacks the names, built afresh (once), the same numbers
    std::vector<SimulationMeshView> mv = views_of(m);
    mv[0].name = "g2"; mv[1].name = "b2";
    views::Options renamed = keyed;
    renamed.targets = {{"b2", "cube"}};
    Result rn, rn2;
    CHECK(views::run(mv, renamed, rn, err) && !rn.sceneReused && same(rn, a));
    CHECK(views::run(mv, renamed, rn2, err) && rn2.sceneReused);
  }
  {   // meshes reordered under the key (its scene built in the other order): labels by name, the reordered rows
    const std::vector<SimulationMeshView> rev = {views_of(m)[1], views_of(m)[0]};
    Result r1r, r2r;
    CHECK(views::run(rev, o, r1r, err));   // keyless
    CHECK(views::run(views_of(m), keyed, rt, err));   // the key's scene in the order ground, box
    CHECK(views::run(rev, keyed, r2r, err) && r2r.sceneReused && same(r1r, r2r));
  }
  {   // the names swapped under the key ("ground" now names the box): the scene holds "ground" for the other mesh, so it
      // is built afresh and the box is the target, as in a keyless run
    std::vector<SimulationMeshView> sw = views_of(m);
    std::swap(sw[0].name, sw[1].name);
    views::Options swapped = keyed;
    swapped.targets = {{"ground", "cube"}};
    Result rs;
    CHECK(views::run(sw, swapped, rs, err) && !rs.sceneReused && same(rs, a));
  }
  {   // cancel: the dump files it had opened go too
    std::atomic<bool> stop{true};
    views::Options oc = o;
    oc.cancel = &stop;
    oc.dumpPath = tmpPath;
    Result rc;
    CHECK(!views::run(m, oc, rc, err) && err == "cancelled");
    for (const std::string& p : {tmpPath, tmpPath + ".depth.npy"}) {
      std::FILE* f = std::fopen(p.c_str(), "rb");
      CHECK(!f);
      if (f) std::fclose(f);
    }
  }
  {   // from a 2 cm facade (360, offset 0) a target wall 10 m out, +-30 m high, spans azimuths 90..180 exactly: jittered
      // rays stay in their cells, so it takes exactly a quarter of the field
    const std::vector<SimulationMesh> qs = {quad("p", true, {-0.01, 0, -0.01, 0.01, 0, -0.01, 0.01, 0, 0.01, -0.01, 0, 0.01}),
                                  quad("w", false, {10, 0, -30, 0, -10, -30, 0, -10, 30, 10, 0, 30})};
    views::Options oq;
    oq.fovH = 360;
    oq.targets = {{"w", "w"}};
    Result rq;
    CHECK(views::run(qs, oq, rq, err) && std::fabs(at(rq, 0, "w_front") - 0.25) <= 1e-12);
  }
  {   // --dump-rays above 4 GB (29 x 29 faces x 1440 x 720 rays x 5 bytes) is refused before anything is traced or written
    std::vector<SimulationMesh> grid = {mesh("grid", true)};
    for (int k = 0; k < 29 * 29; ++k) {
      const double x = k % 29, y = k / 29;
      face(grid[0], {x, y, 0, x + 1, y, 0, x + 1, y + 1, 0, x, y + 1, 0});
    }
    views::Options big;
    big.fovH = 360; big.elevLo = -90; big.elevHi = 90; big.cell = 0.25;
    big.dumpPath = tmpPath;
    std::remove(tmpPath.c_str());
    Result rb;
    CHECK(!views::run(grid, big, rb, err) && err.rfind("--dump-rays:", 0) == 0);
    std::FILE* f = std::fopen(tmpPath.c_str(), "rb");
    CHECK(!f);
    if (f) std::fclose(f);
  }
  check_glass(err);
  check_isovist(testsDir, err);
  return failures;
}
