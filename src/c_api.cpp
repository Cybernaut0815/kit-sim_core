// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// The C ABI (daylight_c.h): main.cpp's run without its files. The CLI's own parser, rules and manifests (cli.h) run on
// argv, meshes come as handles, one lock surrounds the core's run, and nothing thrown crosses the ABI.
// ponytail: the ~20 lines after parsing repeat main.cpp (whose output must stay byte-identical); the c_abi test compares
// both, move them into cli.cpp if a fourth front end appears.
#include "daylight_c.h"
#include "cli.h"
#include "internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

using namespace daylight;

struct dl_mesh { uint64_t serial = 0; std::vector<SimulationMesh> meshes; };   // never changes: the serial names the content
struct dl_control { std::atomic<bool> cancel{false}; std::atomic<double> progress{0}; };
struct dl_result { int64_t rows = 0, cols = 0; std::string manifest; std::vector<double> values; double stats[DL_STATS] = {}; };

namespace {

thread_local char lastError[2048];   // a plain array: setting it never allocates (the out-of-memory path)
std::mutex runMu;                    // ponytail: one lock, as the core runs one call at a time; not FIFO, queue if order matters
std::atomic<uint64_t> serials{0};    // never reused (an address is)

int32_t fail(int32_t rc, const char* msg) {
  std::snprintf(lastError, sizeof lastError, "%s", msg);
  return rc;
}

// Every export runs its body through here: anything thrown (copies, parsing, the result) becomes DL_RUNTIME.
template <class F> int32_t guard(F&& f) noexcept {
  try {
    return f();
  } catch (const std::bad_alloc&) {
    return fail(DL_RUNTIME, "out of memory: host memory ran out");
  } catch (const std::exception& e) {
    return fail(DL_RUNTIME, e.what());
  } catch (...) {
    return fail(DL_RUNTIME, "internal error");
  }
}

std::string lower_ext(const std::string& path) {   // main.cpp's
  std::string ext = path.substr(std::min(path.find_last_of('.'), path.size()));
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// The core reads the flags' and the EPW's numbers and prints the manifest with the C library, in the calling thread's C
// locale: "C" for the call, whatever the host set (a decimal comma, e.g. Python's locale.setlocale, would read --lat
// 41.98 as 41). Per thread, so the host's other threads keep theirs.
struct CLocale {
#ifdef _WIN32
  int mode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
  std::string saved;
  CLocale() {
    const char* l = std::setlocale(LC_ALL, nullptr);
    saved = l ? l : "C";
    std::setlocale(LC_ALL, "C");
  }
  ~CLocale() {
    std::setlocale(LC_ALL, saved.c_str());
    _configthreadlocale(mode);
  }
#endif   // ponytail: Windows only, as the build; uselocale(newlocale(...)) on POSIX
};

// What main.cpp reads from files or writes, handles and the result do instead (the server refuses the same).
int32_t refused(const cli::CommonArgs& a) {
  if (a.help) return fail(DL_USAGE, "-h: dl_usage(tool) gives the usage text");
  if (!a.sensors.empty())
    return fail(DL_USAGE, ("argument '" + a.sensors[0] + "': mesh files are not read in-process; dl_mesh_load(path) gives a handle").c_str());
  if (!a.context.empty()) return fail(DL_USAGE, "--context: in-process the context meshes are dl_run's context handles");
  if (!a.out.empty()) return fail(DL_USAGE, "--out: in-process the result holds the values and the manifest");
  if (a.bench) return fail(DL_USAGE, "--bench: the CLIs' benchmark, not in-process");
  return DL_OK;
}

// Scene identity: the handles (serials) in order and role, and selfOcclude; the core adds slots and the open set.
void set_run(RunOptions& o, const std::string& key, dl_control* control) {
  o.sceneKey = key + (o.selfOcclude ? " self" : " noself");
  o.cancel = control ? &control->cancel : nullptr;
  o.progress = control ? &control->progress : nullptr;
}

int32_t finish(Result& r, std::string manifest, dl_result** out) {
  auto res = std::make_unique<dl_result>();
  res->cols = static_cast<int64_t>(r.columns.size());
  res->rows = res->cols ? static_cast<int64_t>(r.values.size()) / res->cols : 0;
  res->manifest = std::move(manifest);
  res->values = std::move(r.values);
  const RunStats& s = r.stats;
  const double st[DL_STATS] = {r.sceneReused ? 1.0 : 0.0, s.prepMs, s.skyMs, s.buildMs, s.traceMs, s.reduceMs, s.bounceMs,
                               static_cast<double>(s.rays), static_cast<double>(s.launches), static_cast<double>(s.gasBytes)};
  std::memcpy(res->stats, st, sizeof st);
  *out = res.release();
  return DL_OK;
}

}  // namespace

extern "C" {

int32_t dl_version(void) { return DL_ABI_VERSION; }

const char* dl_last_error(void) { return lastError; }

const char* dl_usage(int32_t tool) {
  return tool == DL_DAYLIGHT ? cli::kUsage : tool == DL_VIEWS ? views::cli::kUsage : nullptr;
}

dl_mesh* dl_mesh_create(const char* name, const double* xyz, int64_t n_points, const int32_t* counts, int64_t n_faces,
                        const int32_t* indices, int64_t n_indices, const uint16_t* slots) {
  dl_mesh* made = nullptr;
  guard([&]() -> int32_t {
    if (!name) return fail(DL_USAGE, "dl_mesh_create: name is NULL");
    if (n_points < 0 || n_faces < 0 || n_indices < 0 || (n_points && !xyz) || (n_faces && !counts) || (n_indices && !indices))
      return fail(DL_USAGE, "dl_mesh_create: a negative count or a NULL array");
    SimulationMesh m;
    m.name = name;
    m.points.assign(xyz, xyz + 3 * n_points);
    m.counts.assign(counts, counts + n_faces);
    m.indices.assign(indices, indices + n_indices);
    if (slots) m.slot.assign(slots, slots + n_faces);
    std::string err;
    if (!mesh_ok(m, m.name, err)) return fail(DL_USAGE, err.c_str());
    auto h = std::make_unique<dl_mesh>();
    if (m.numFaces()) h->meshes.push_back(std::move(m));   // empty meshes dropped, as the readers drop empty groups
    h->serial = ++serials;
    made = h.release();
    return DL_OK;
  });
  return made;
}

dl_mesh* dl_mesh_load(const char* path) {
  dl_mesh* made = nullptr;
  guard([&]() -> int32_t {
    if (!path) return fail(DL_USAGE, "dl_mesh_load: path is NULL");
    auto h = std::make_unique<dl_mesh>();
    const std::string p = path, ext = lower_ext(p);
    std::string err;
    bool ok = false;
    if (ext == ".obj") ok = read_obj(p, h->meshes, err);
    else if (ext == ".dmesh") ok = read_dmesh(p, h->meshes, err);
    else err = p + ": unknown extension; .obj or .dmesh in-process (USD: bindings/python writes .dmesh)";
    if (!ok) return fail(DL_RUNTIME, err.c_str());   // the CLI exits 2 when a mesh does not load
    h->serial = ++serials;
    made = h.release();
    return DL_OK;
  });
  return made;
}

int32_t dl_mesh_count(const dl_mesh* mesh) { return mesh ? static_cast<int32_t>(mesh->meshes.size()) : -1; }

const char* dl_mesh_name(const dl_mesh* mesh, int32_t k) {
  return mesh && k >= 0 && static_cast<size_t>(k) < mesh->meshes.size() ? mesh->meshes[static_cast<size_t>(k)].name.c_str() : nullptr;
}

void dl_mesh_free(dl_mesh* mesh) { delete mesh; }

dl_control* dl_control_create(void) {
  dl_control* made = nullptr;
  guard([&]() -> int32_t { made = new dl_control; return DL_OK; });
  return made;
}

void dl_control_cancel(dl_control* control) { if (control) control->cancel = true; }

double dl_control_progress(const dl_control* control) { return control ? control->progress.load() : 0.0; }

void dl_control_free(dl_control* control) { delete control; }

int32_t dl_run(int32_t tool, int32_t argc, const char* const* argv, dl_mesh* const* sensors, int32_t n_sensors,
               dl_mesh* const* context, int32_t n_context, dl_control* control, dl_result** out) {
  return guard([&]() -> int32_t {
    if (!out) return fail(DL_USAGE, "dl_run: out is NULL");
    *out = nullptr;
    if (tool != DL_DAYLIGHT && tool != DL_VIEWS) return fail(DL_USAGE, "dl_run: unknown tool (DL_DAYLIGHT 0, DL_VIEWS 1)");
    if (argc < 0 || n_sensors < 0 || n_context < 0 || (argc && !argv) || (n_sensors && !sensors) || (n_context && !context))
      return fail(DL_USAGE, "dl_run: a negative count or a NULL array");
    // argv as main() receives it (cli::parse takes char**)
    std::vector<std::string> store;
    store.reserve(static_cast<size_t>(argc) + 1);
    store.emplace_back(tool == DL_VIEWS ? "views" : "daylight");
    for (int32_t i = 0; i < argc; ++i) {
      if (!argv[i]) return fail(DL_USAGE, "dl_run: a NULL flag");
      store.emplace_back(argv[i]);
    }
    std::vector<char*> av;
    for (std::string& s : store) av.push_back(s.data());
    // the meshes in the CLI's order (sensors, then context) without copies; the key names the handles in that order
    std::vector<SimulationMeshView> mv;
    std::string key = "dl";
    for (int32_t pass = 0; pass < 2; ++pass) {
      dl_mesh* const* hs = pass ? context : sensors;
      const int32_t n = pass ? n_context : n_sensors;
      for (int32_t k = 0; k < n; ++k) {
        if (!hs[k]) return fail(DL_USAGE, "dl_run: a NULL mesh handle");
        key += (pass ? " c" : " s") + std::to_string(hs[k]->serial);
        for (const SimulationMesh& m : hs[k]->meshes) mv.push_back({&m, m.name, pass == 0});
      }
    }
    const CLocale cLocale;
    std::string err;
    Result r;
    if (tool == DL_DAYLIGHT) {
      cli::Args a;
      if (cli::parse(static_cast<int>(av.size()), av.data(), a, err)) return fail(DL_USAGE, err.c_str());
      if (const int32_t rc = refused(a)) return rc;
      if (!n_sensors) return fail(DL_USAGE, "no mesh given");
      Weather w;
      bool haveWeather = false;
      if (const int rc = cli::prepare(a, w, haveWeather, err)) return fail(rc, err.c_str());
      std::vector<Slot> slots;
      std::vector<char> open;
      if (!resolve_optics(mv, a.opt.optics, slots, err) || !resolve_no_occlude(mv, a.opt.noOcclude, open, err))
        return fail(DL_USAGE, err.c_str());
      set_run(a.opt, key, control);
      {
        std::lock_guard<std::mutex> lock(runMu);
        if (!run(mv, haveWeather ? &w : nullptr, a.opt, r, err)) return fail(DL_RUNTIME, err.c_str());
      }
      if (control) control->progress = 1;   // also a run with nothing to trace (sun hours at night)
      return finish(r, cli::manifest_json(r), out);
    }
    views::cli::Args a;
    if (views::cli::parse(static_cast<int>(av.size()), av.data(), a, err)) return fail(DL_USAGE, err.c_str());
    if (const int32_t rc = refused(a)) return rc;
    if (!n_sensors) return fail(DL_USAGE, "no mesh given");
    std::vector<char> open;
    std::vector<std::string> groups;
    std::vector<uint8_t> cls;
    std::vector<Slot> slots;
    if (!resolve_no_occlude(mv, a.opt.noOcclude, open, err) || !resolve_targets(mv, a.opt.targets, a.opt.selfOcclude, open, groups, cls, err) ||
        !resolve_optics(mv, a.opt.optics, slots, err))
      return fail(DL_USAGE, err.c_str());
    set_run(a.opt, key, control);
    {
      std::lock_guard<std::mutex> lock(runMu);
      if (!views::run(mv, a.opt, r, err)) return fail(DL_RUNTIME, err.c_str());
    }
    if (control) control->progress = 1;
    views::Field f;
    views::field(mv, a.opt, f, err);   // the run passed the same checks
    return finish(r, views::cli::manifest_json(r, a.opt, f), out);
  });
}

int64_t dl_result_rows(const dl_result* result) { return result ? result->rows : -1; }

int64_t dl_result_cols(const dl_result* result) { return result ? result->cols : -1; }

const char* dl_result_manifest(const dl_result* result) { return result ? result->manifest.c_str() : nullptr; }

int32_t dl_result_values(const dl_result* result, double* out, int64_t n) {
  if (!result || !out) return fail(DL_USAGE, "dl_result_values: a NULL argument");
  if (n < static_cast<int64_t>(result->values.size())) return fail(DL_USAGE, "dl_result_values: out holds fewer than rows x cols values");
  std::memcpy(out, result->values.data(), result->values.size() * sizeof(double));
  return DL_OK;
}

int32_t dl_result_values_f32(const dl_result* result, float* out, int64_t n) {
  if (!result || !out) return fail(DL_USAGE, "dl_result_values_f32: a NULL argument");
  if (n < static_cast<int64_t>(result->values.size())) return fail(DL_USAGE, "dl_result_values_f32: out holds fewer than rows x cols values");
  for (size_t i = 0; i < result->values.size(); ++i) out[i] = static_cast<float>(result->values[i]);
  return DL_OK;
}

int32_t dl_result_stats(const dl_result* result, double* out) {
  if (!result || !out) return fail(DL_USAGE, "dl_result_stats: a NULL argument");
  std::memcpy(out, result->stats, sizeof result->stats);
  return DL_OK;
}

void dl_result_free(dl_result* result) { delete result; }

}  // extern "C"
