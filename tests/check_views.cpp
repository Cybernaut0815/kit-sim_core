// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// Checks for views (views.h). Runs with --jitter off against a host model of the same cells, traced in double from the
// device's float sensor origins (every ray's class and distance through --dump-rays, then every column): facades,
// tilted, turned and flat faces, --north, an eye 10 m over a 2 km target quad (360), the virtual ground's height and
// switch, a wall past --far, sensors in the plane of a 2 km quad, rectangle targets with group distances and --reach.
// Then the jitter, chunking, progress, f32 columns, sides, a scene shared with daylight, labels on reused, renamed and
// reordered resident scenes, cancel, ray counts, the option, target and CLI rules and the dump size guard. Self-skips
// without a CUDA device.
#include "cli.h"
#include "internal.h"
#include <algorithm>
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

static Mesh mesh(const char* name, bool sensor) { Mesh m; m.name = name; m.sensor = sensor; return m; }
// Appends a face over the given corners (x y z each), counterclockwise seen from the side it faces.
static void face(Mesh& m, std::initializer_list<double> xyz) {
  const int b = int(m.numPoints());
  m.points.insert(m.points.end(), xyz);
  m.counts.push_back(int(xyz.size() / 3));
  for (int k = 0; k < m.counts.back(); ++k) m.indices.push_back(b + k);
}
static Mesh quad(const char* name, bool sensor, std::initializer_list<double> xyz) { Mesh m = mesh(name, sensor); face(m, xyz); return m; }
// A 2 x 2 m face from corner (x, y, z), facing -y turned az degrees towards +x and tilted tilt degrees up.
static void tilted(Mesh& m, double x, double y, double z, double az, double tilt) {
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
struct Quad { double lo[3], hi[3]; int axis; uint8_t cls; };

// Where a ray ends: its class, distance (inf: open or own surface), the cosine between it and the normal of the surface
// it ends on (a direction error moves the end by 1 / that) and how far off that surface the hardware may meet it:
// 2^-22 x the quad's extent, what own_plane in trace_dev.cu allows (8x the hardware's error); 0 on the virtual plane.
struct End { uint8_t cls = 255; double t = INFINITY, cosInc = 1, off = 0; };

// The device's ray from o along unit d (n: the sensor's normal), as views.h defines it: 360 at offset 0 culls the
// sensor's back (254), the closest quad, else sky above the horizon, ground below it, ended by the virtual ground (to
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
  double tmax = down ? (o[2] - plane + 0.01) / -d[2] : INFINITY;
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
    if (out <= 0 && tq < tmax) {
      tmax = tq;
      e = {q.cls, tq, std::fabs(d[a]), 0};
      for (int b = 0; b < 3; ++b) e.off = std::max(e.off, (q.hi[b] - q.lo[b]) * 0x1p-22);
    }
  }
  if (e.cls != 255) return e;
  margin = std::min(margin, std::fabs(d[2]));
  e.cls = d[2] > 0 ? 0 : 1;
  if (down) { e.t = (o[2] - plane) / -d[2]; e.cosInc = -d[2]; }
  return e;
}

// One views run with --jitter off and its dump against the host model: the field's cells (centres in azimuth and in sine
// of elevation, rows top-down from HI, columns clockwise from the left edge, weights = solid angles) from each sensor's
// frame (forward = normal, right = horizontal or, looking straight up or down, east turned by --north, up = right x
// forward; 360: true north, east, up), traced from the device's origin (the sensors of make_sensors_into, offset_ray)
// against the blocking meshes' faces. Every ray's class must match and its end lie within tol x its distance (plus End::off)
// of the host's, measured across the surface it ends on; every column as views.h defines it (shares and reach 1e-9,
// distances tol relative). margin: how close any ray comes to another class (radians off a quad's edge, the horizon or the
// culled plane; relative to a reach distance), which must stay well above the device's float error.
static Result expect(const char* what, const std::vector<Mesh>& meshes, views::Options o, double tol = 1e-6) {
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
  for (const Mesh& m : meshes)
    for (size_t i = 2; std::isnan(o.groundZ) && i < m.points.size(); i += 3) low = std::fmin(low, m.points[i]);
  const double plane = float(low - c[2]);
  std::vector<std::string> groups;   // in order of first appearance
  for (const views::Target& tg : o.targets) if (std::find(groups.begin(), groups.end(), tg.group) == groups.end()) groups.push_back(tg.group);
  std::vector<Quad> quads;
  std::vector<float3> P, N;
  const size_t nS = o.sides == Sides::Both ? 2 : 1;
  for (const Mesh& m : meshes) {
    uint8_t cl = 2;
    for (const views::Target& tg : o.targets)
      if (tg.mesh == m.name) cl = uint8_t(3 + (std::find(groups.begin(), groups.end(), tg.group) - groups.begin()));
    const bool open = std::find(o.noOcclude.begin(), o.noOcclude.end(), m.name) != o.noOcclude.end();
    if (!m.sensor || (o.selfOcclude && !open))
      for (size_t f = 0, at = 0; f < m.numFaces(); at += size_t(m.counts[f++])) {
        Quad q{{INFINITY, INFINITY, INFINITY}, {-INFINITY, -INFINITY, -INFINITY}, -1, cl};
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

  // the grid
  const bool world = o.fovH == 360;
  const unsigned nAz = unsigned(std::max(1.0, std::round(o.fovH / o.cell))), nEl = unsigned(std::max(1.0, std::round((o.elevHi - o.elevLo) / o.cell)));
  const size_t K = size_t(nAz) * nEl, nSens = P.size(), nG = groups.size(), C = r.columns.size();
  const double dAz = o.fovH / nAz, dEl = (o.elevHi - o.elevLo) / nEl, az0 = world ? 0 : -o.fovH / 2;
  std::vector<double> sRow(nEl), wRow(nEl);
  double sum = 0;
  for (unsigned k = 0; k < nEl; ++k) {
    const double top = std::sin((o.elevHi - k * dEl) * kDeg), bot = std::sin((o.elevHi - (k + 1) * dEl) * kDeg);
    sRow[k] = (top + bot) / 2;
    wRow[k] = top - bot;
    sum += wRow[k] * nAz;
  }
  for (double& w : wRow) w /= sum;
  CHECK(cb.size() == nSens * K && db.size() == 4 * nSens * K && C % nS == 0 && r.values.size() == C * (nSens / nS));
  if (!(cb.size() == nSens * K && db.size() == 4 * nSens * K && C % nS == 0 && r.values.size() == C * (nSens / nS))) return r;

  const double cosN = std::cos(o.north * kDeg), sinN = std::sin(o.north * kDeg);
  size_t badRays = 0, badCols = 0;
  double worstRay = 0, worstCol = 0, margin = INFINITY;
  for (size_t i = 0; i < nSens; ++i) {
    const float3 of = offset_ray(P[i], N[i]);
    const double O[3] = {of.x, of.y, of.z}, n[3] = {N[i].x, N[i].y, N[i].z}, l = std::hypot(n[0], n[1]);
    double f[3] = {-sinN, cosN, 0}, rt[3] = {cosN, sinN, 0}, u[3] = {0, 0, 1};
    if (!world) {
      std::copy(n, n + 3, f);
      if (l > 1e-6) { rt[0] = n[1] / l; rt[1] = -n[0] / l; rt[2] = 0; }
      u[0] = rt[1] * f[2] - rt[2] * f[1]; u[1] = rt[2] * f[0] - rt[0] * f[2]; u[2] = rt[0] * f[1] - rt[1] * f[0];
    }
    std::vector<double> w(3 + nG), wl(3 + nG), mn(3 + nG, INFINITY), rc(o.reach.size());
    double own = 0;
    for (size_t d = 0; d < K; ++d) {
      const double s = sRow[d / nAz], cs = std::sqrt(1 - s * s), a = (az0 + (double(d % nAz) + 0.5) * dAz) * kDeg;
      double dir[3];
      for (int b = 0; b < 3; ++b) dir[b] = cs * std::sin(a) * rt[b] + cs * std::cos(a) * f[b] + s * u[b];
      const End h = trace(O, dir, n, world && o.offset == 0, o.virtualGround, plane, quads, margin);
      // the device's ray: its class, and how far its end lies from the host's across the surface, in tolerances
      const uint8_t gc = uint8_t(cb[i * K + d]);
      float gt;
      std::memcpy(&gt, db.data() + 4 * (i * K + d), 4);
      const double dt = std::isinf(h.t) ? (std::isinf(gt) ? 0 : INFINITY) : std::fabs(gt - h.t) * h.cosInc / (tol * h.t + h.off);
      if (gc != h.cls || dt > 1) {
        if (badRays++ < 3)
          std::printf("views: %s: sensor %zu row %zu column %zu: class %d at %.9g m, host %d at %.9g m\n", what, i, d / nAz, d % nAz, gc, double(gt), h.cls, h.t);
      } else worstRay = std::max(worstRay, dt);
      // the columns' sums
      const double wd = wRow[d / nAz];
      if (h.cls == 254) { own += wd; continue; }
      const double dd = std::min(std::max(h.t, 0.1), o.farDist);
      w[h.cls] += wd;
      wl[h.cls] += wd * std::log(dd);
      mn[h.cls] = std::min(mn[h.cls], dd);
      for (size_t q = 0; q < rc.size(); ++q) {
        if (h.t >= o.reach[q]) rc[q] += wd;
        if (!std::isinf(h.t)) margin = std::min(margin, std::fabs(h.t / o.reach[q] - 1));
      }
    }
    // the columns, in views.h's order: shares, distance, distance_min, per group, reach_<D>
    double total = own, W = 0, WL = 0, M = INFINITY;
    for (size_t k = 0; k < w.size(); ++k) { total += w[k]; W += w[k]; WL += wl[k]; M = std::min(M, mn[k]); }
    const auto logMean = [](double sw, double swl) { return sw > 0 ? std::exp(swl / sw) : NAN; };
    const auto least = [](double m) { return std::isinf(m) ? NAN : m; };
    std::vector<double> want = {w[0] / total, w[1] / total, (w[2] + own) / total};
    for (size_t g = 0; g < nG; ++g) want.push_back(w[3 + g] / total);
    const size_t shares = want.size();
    if (o.distMean) want.push_back(logMean(W, WL));
    if (o.distMin) want.push_back(least(M));
    for (size_t g = 0; o.distGroup && g < nG; ++g) {
      if (o.distMean) want.push_back(logMean(w[3 + g], wl[3 + g]));
      if (o.distMin) want.push_back(least(mn[3 + g]));
    }
    const size_t dists = want.size();
    for (double x : rc) want.push_back(x / total);
    CHECK(want.size() * nS == C);
    for (size_t k = 0; k < want.size() && want.size() * nS == C; ++k) {
      const size_t col = k * nS + i % nS;
      const double got = r.values[(i / nS) * C + col], x = want[k];
      const bool dist = k >= shares && k < dists;
      const double e = std::isnan(x) || std::isnan(got) ? (std::isnan(x) && std::isnan(got) ? 0 : INFINITY) : dist ? std::fabs(got - x) / x : std::fabs(got - x);
      if (e > (dist ? tol : 1e-9)) {
        if (badCols++ < 3) std::printf("views: %s: sensor %zu %s %.17g, host %.17g\n", what, i, r.columns[col].c_str(), got, x);
      } else if (dist) worstCol = std::max(worstCol, e);
    }
  }
  std::printf("views: %s: %zu rays end as the host traces them (at most %.2f of the tolerance off), columns within %.1e, margin %.1e\n",
              what, nSens * K, worstRay, worstCol, margin);
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
  std::vector<Mesh> ms = {quad("s", true, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2}), quad("dup", false, {0, -5, 0, 1, -5, 0, 1, -5, 1, 0, -5, 1})};
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
      {"--cell must be positive", [](views::Options& o) { o.cell = 0; }},
      {"exceed 2^20", [](views::Options& o) { o.fovH = 360; o.elevLo = -90; o.elevHi = 90; o.cell = 0.1; }},
      {"--far must be more than 0.1", [](views::Options& o) { o.farDist = 0.1; }},
      {"--far must be more than 0.1", [](views::Options& o) { o.farDist = INFINITY; }},
      {"at most 8", [](views::Options& o) { o.reach = {1, 2, 3, 4, 5, 6, 7, 8, 9}; }},
      {"must be positive", [](views::Options& o) { o.reach = {6, 0}; }},
      {"must be positive", [](views::Options& o) { o.reach = {INFINITY}; }},
      {"--ground-z must be finite", [](views::Options& o) { o.groundZ = -INFINITY; }},
      {"face-slot targets", [](views::Options& o) { o.targets = {{"t0/1", "a"}}; }},
      {"letters, digits, _ and -", [](views::Options& o) { o.targets = {{"t0", "a b"}}; }},
      {"letters, digits, _ and -", [](views::Options& o) { o.targets = {{"t0", ""}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "sky"}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "ground"}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "obstruction"}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "distance_x"}}; }},
      {"is reserved", [](views::Options& o) { o.targets = {{"t0", "reach6"}}; }},
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
    std::vector<Mesh> context = {ms[1]};
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
  CHECK(parse({"f.obj", "--distance", "none"}, a, err) == 0 && !a.opt.distMean && !a.opt.distMin && !a.opt.distGroup);
  CHECK(parse({"f.obj", "--distance", "min,group"}, a, err) == 0 && !a.opt.distMean && a.opt.distMin && a.opt.distGroup);
  CHECK(parse({"f.obj"}, a, err) == 0 && a.opt.distMean && a.opt.distMin && !a.opt.distGroup && a.opt.jitter && a.opt.virtualGround &&
        a.opt.fovH == 120 && a.opt.elevLo == -70 && a.opt.elevHi == 50 && a.opt.cell == 2 && a.opt.farDist == 1000 && a.opt.reach.empty());
  CHECK(parse({"f.obj", "--fov", "360"}, a, err) == 0 && a.opt.fovH == 360 && a.opt.elevLo == -70 && a.opt.elevHi == 50);
  CHECK(parse({"f.obj", "--fov", "360,-30,30", "--cell", "1"}, a, err) == 0 && a.opt.elevLo == -30 && a.opt.elevHi == 30 && a.opt.cell == 1);
  CHECK(parse({"f.obj", "--reach", "en17037"}, a, err) == 0 && a.opt.reach == std::vector<double>({6, 20, 50}));
  CHECK(parse({"f.obj", "--jitter", "off", "--virtual-ground", "off", "--ground-z", "-2.5", "--far", "300"}, a, err) == 0 && !a.opt.jitter &&
        !a.opt.virtualGround && a.opt.groundZ == -2.5 && a.opt.farDist == 300);
  for (const std::vector<const char*>& bad : std::vector<std::vector<const char*>>{{"f.obj", "--fov", "1,2"}, {"f.obj", "--fov", "200"}, {"f.obj", "--jitter", "no"},
                                                                                  {"f.obj", "--far", "0.1"}, {"f.obj", "--cell", "x"}, {"f.obj", "--reach", "6,,20"},
                                                                                  {"f.obj", "--fov", "360,-90,90", "--cell", "0.1"}})
    CHECK(parse(bad, a, err) == 1);
}

int check_views(const char* testsDir) {
  tmpPath = std::string(testsDir) + "/_check_views_tmp.npy";
  check_rules();
  std::string err;
  {   // without a GPU nothing below can run
    Result r;
    std::vector<Mesh> one = {quad("s", true, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2})};
    views::Options o;
    o.cell = 30;
    if (!views::run(one, o, r, err)) {
      const bool noGpu = err.rfind("no CUDA device", 0) == 0 || err.find("optixInit") != std::string::npos;
      std::printf("%s: views: %s\n", noGpu ? "skip" : "FAIL", err.c_str());
      return failures + (noGpu ? 0 : 1);
    }
  }

  // 1, 4: faces in an empty scene (--no-self-occlude): a facade 1 m over the lowest point, a roof, a tilted, a turned and a
  // tilted-down face, a ceiling 1.5 m up and a face looking down at the lowest point (its eye on the virtual plane)
  Mesh faces = mesh("faces", true);
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
    // (the host model); the roof sees only sky, at --far; the face on the plane sees the ground at 0.1 m
    CHECK(std::fabs(at(r1, 0, "sky_front") - kSky) <= 1e-9 && std::fabs(at(r1, 0, "ground_front") - (1 - kSky)) <= 1e-9);
    CHECK(at(r1, 1, "sky_front") == 1 && std::fabs(at(r1, 1, "distance_front") / 1000 - 1) <= 1e-9 && at(r1, 1, "distance_min_front") == 1000);
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
  {   // --virtual-ground off: the facade's ground rays are open
    views::Options off = o1;
    off.virtualGround = false;
    const Result r = expect("faces, --virtual-ground off", {faces}, off);
    CHECK(std::fabs(at(r, 0, "ground_front") - (1 - kSky)) <= 1e-9 && std::fabs(at(r, 0, "distance_front") / 1000 - 1) <= 1e-9 &&
          at(r, 0, "distance_min_front") == 1000);
  }
  {   // a canopy 3 m over the roof, on one side: what the roof sees of it turns with --north (its right: east turned)
    std::vector<Mesh> canopy = {faces, quad("canopy", false, {6, 1.2, 5, 30, 1.2, 5, 30, 30, 5, 6, 30, 5})};
    views::Options n0 = o1, n30 = o1;
    n30.north = 30;
    const Result a = expect("faces under a canopy", canopy, n0), b = expect("faces under a canopy, --north 30", canopy, n30);
    const double o0 = at(a, 1, "obstruction_front"), o30 = at(b, 1, "obstruction_front");
    CHECK(o0 > 0.01 && o30 > 0.01 && std::fabs(o0 - o30) > 0.01);
  }

  // 2: 360 degrees from an eye 10 m over a 2 km target quad (a facade sensor at --offset 0.5: its back is traced too). The
  // quad lies on the virtual plane: every ray below the horizon ends on it (the shallowest 573 m away), at 10 / sin, as
  // the hardware meets so large a triangle (End::off: the shallow rays' ends ~1e-4 m off its plane)
  const double sLow = (std::sin(-70 * kDeg) + std::sin(-68 * kDeg)) / 2;   // the lowest row's middle, in sine
  const Mesh eye = quad("eye", true, {-0.1, 0, 9.9, 0.1, 0, 9.9, 0.1, 0, 10.1, -0.1, 0, 10.1});
  const Mesh lake = quad("lake", false, {-1000, -1000, 0, 1000, -1000, 0, 1000, 1000, 0, -1000, 1000, 0});
  views::Options o2;
  o2.fovH = 360;
  o2.offset = 0.5;
  o2.selfOcclude = false;
  o2.targets = {{"lake", "water"}};
  const Result r2 = expect("360 over a 2 km target quad", {eye, lake}, o2);
  CHECK(r2.columns == std::vector<std::string>({"sky_front", "ground_front", "obstruction_front", "water_front", "distance_front", "distance_min_front"}));
  CHECK(std::fabs(at(r2, 0, "sky_front") - kSky) <= 1e-9 && std::fabs(at(r2, 0, "water_front") - (1 - kSky)) <= 1e-9 && at(r2, 0, "ground_front") == 0);
  CHECK(std::fabs(at(r2, 0, "distance_min_front") / (10 / -sLow) - 1) <= 1e-6);
  {   // --ground-z 5: the plane ends the downward rays 5 m down, the quad below it is hidden (not in view: NaN)
    views::Options gz = o2;
    gz.groundZ = 5;
    gz.distGroup = true;
    const Result r = expect("360 over a 2 km target quad, --ground-z 5", {eye, lake}, gz);
    CHECK(at(r, 0, "water_front") == 0 && std::fabs(at(r, 0, "ground_front") - (1 - kSky)) <= 1e-9);
    CHECK(std::fabs(at(r, 0, "distance_min_front") / (5 / -sLow) - 1) <= 1e-6 && std::isnan(at(r, 0, "water_distance_front")) &&
          std::isnan(at(r, 0, "water_distance_min_front")));
  }
  {   // walls 2 km north and south: what was sky there is an obstruction now, its distances still count as --far
    Mesh walls = mesh("walls", false);
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
    Mesh spots = mesh("spots", true);
    const double xy[3][2] = {{0, 0}, {0.37, -0.21}, {300, -400}};
    for (const auto& p : xy) face(spots, {p[0] - 0.1, p[1] - 0.1, 0, p[0] + 0.1, p[1] - 0.1, 0, p[0] + 0.1, p[1] + 0.1, 0, p[0] - 0.1, p[1] + 0.1, 0});
    const std::vector<Mesh> plaza = {spots, quad("plaza", false, {-1000, -1000, 0, 1000, -1000, 0, 1000, 1000, 0, -1000, 1000, 0})};
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
    const std::vector<Mesh> st = {quad("facade", true, {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2}), quad("a", false, {-3, -10, 0.5, 5, -10, 0.5, 5, -10, 4, -3, -10, 4}),
                                  quad("b", false, {8, -30, 0.2, 20, -30, 0.2, 20, -30, 6, 8, -30, 6})};
    views::Options o3 = args.opt;
    o3.distGroup = true;
    o3.reach = {6, 20, 50};
    const Result r = expect("two rectangle targets", st, o3);
    CHECK(r.columns == std::vector<std::string>({"sky_front", "ground_front", "obstruction_front", "water_front", "park_front", "distance_front",
                                                 "distance_min_front", "water_distance_front", "water_distance_min_front", "park_distance_front",
                                                 "park_distance_min_front", "reach_6_front", "reach_20_front", "reach_50_front"}));
    CHECK(at(r, 0, "water_front") > 0.05 && at(r, 0, "park_front") > 0.005);   // most of b hides behind a
    // 360 at offset 0 (north 30): half the field is the facade's own surface, which reaches nothing; the reach of the
    // rest: sky every distance, ground its plane distance, a target its hit
    views::Options ow = o3;
    ow.fovH = 360;
    ow.north = 30;
    const Result rw = expect("two rectangle targets, 360", st, ow);
    CHECK(std::fabs(at(rw, 0, "obstruction_front") - 0.5) <= 1e-9 && at(rw, 0, "reach_6_front") < 0.5);
    // --virtual-ground off: nothing nearer than the targets, distance_min is the nearer one's
    views::Options of = o3;
    of.virtualGround = false;
    const Result rf = expect("two rectangle targets, --virtual-ground off", st, of);
    CHECK(at(rf, 0, "distance_min_front") == at(rf, 0, "water_distance_min_front") && at(rf, 0, "water_distance_min_front") < at(rf, 0, "park_distance_min_front"));
    CHECK(at(rf, 0, "water_distance_front") > 10 && at(rf, 0, "water_distance_front") < 12 && at(rf, 0, "park_distance_front") > 30 &&
          at(rf, 0, "park_distance_front") < 40);
    // --distance none: the shares and the reach only
    views::Options on = o3;
    on.distMean = on.distMin = on.distGroup = false;
    const Result rn = expect("two rectangle targets, --distance none", st, on);
    CHECK(rn.columns == std::vector<std::string>({"sky_front", "ground_front", "obstruction_front", "water_front", "park_front", "reach_6_front",
                                                  "reach_20_front", "reach_50_front"}));
    // 16 groups, the most there are: 16 strips 10 m ahead
    std::vector<Mesh> strips = {st[0]};
    views::Options o16;
    o16.distGroup = true;
    for (int k = 0; k < 16; ++k) {
      strips.push_back(quad("s", false, {k - 8.0, -10, 0.3, k - 7.2, -10, 0.3, k - 7.2, -10, 3, k - 8.0, -10, 3}));
      strips.back().name += std::to_string(k);
      o16.targets.push_back({strips.back().name, "g" + std::to_string(k)});
    }
    const Result r16 = expect("16 target groups", strips, o16);
    CHECK(r16.columns.size() == 3 + 16 + 2 + 2 * 16);
  }

  // 7, 8, 9 on tests/box.obj (a ground and a box over it, both sensors; both sides, a target, group distances, reach)
  std::vector<Mesh> m;
  CHECK(read_obj(std::string(testsDir) + "/box.obj", m, err) && m.size() == 2);
  if (m.size() != 2) return failures;
  views::Options o;
  o.sides = Sides::Both;
  o.targets = {{"box", "cube"}};
  o.distGroup = true;
  o.reach = {1, 3};
  Result a, b;
  CHECK(views::run(m, o, a, err) && a.stats.rays == 16u * 2 * 3600);   // 16 faces x 2 sides x 60 x 60 cells
  const size_t C = a.columns.size(), N = C ? a.values.size() / C : 0;
  CHECK(C == 2 * 10 && N == 16);   // per side: 4 shares, 2 distances, 2 of the group, 2 reach
  {   // one sensor per chunk: the same bits, progress up to 1
    views::Options tiny = o;
    tiny.chunkBytes = 64;
    std::atomic<double> progress{0};
    tiny.progress = &progress;
    CHECK(views::run(m, tiny, b, err) && same(a, b) && b.stats.launches >= 32);
    CHECK(progress.load() == 1.0);
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
    std::vector<MeshView> mv = views_of(m);
    mv[0].name = "g2"; mv[1].name = "b2";
    views::Options renamed = keyed;
    renamed.targets = {{"b2", "cube"}};
    Result rn, rn2;
    CHECK(views::run(mv, renamed, rn, err) && !rn.sceneReused && same(rn, a));
    CHECK(views::run(mv, renamed, rn2, err) && rn2.sceneReused);
  }
  {   // meshes reordered under the key (its scene built in the other order): labels by name, the reordered rows
    const std::vector<MeshView> rev = {views_of(m)[1], views_of(m)[0]};
    Result r1r, r2r;
    CHECK(views::run(rev, o, r1r, err));   // keyless
    CHECK(views::run(views_of(m), keyed, rt, err));   // the key's scene in the order ground, box
    CHECK(views::run(rev, keyed, r2r, err) && r2r.sceneReused && same(r1r, r2r));
  }
  {   // cancel
    std::atomic<bool> stop{true};
    views::Options oc = o;
    oc.cancel = &stop;
    Result rc;
    CHECK(!views::run(m, oc, rc, err) && err == "cancelled");
  }
  {   // --dump-rays above 4 GB (29 x 29 faces x 1440 x 720 rays x 5 bytes) is refused before anything is traced or written
    std::vector<Mesh> grid = {mesh("grid", true)};
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
  return failures;
}
