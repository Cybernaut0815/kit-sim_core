// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// Internal contracts between the translation units. Not part of the public API.
#pragma once
#include "daylight.h"
#include "params.h"         // Slot
#include "views.h"
#include <cstdio>
#include <functional>
#include <vector_types.h>   // float3, uint3 (CUDA toolkit, header-only)

namespace daylight {

// ------------------------------------------------------------------ sky.cpp (Radiance-derived; see THIRD_PARTY_NOTICES.md)
struct Patches {
  int P = 0;                         // sky patches (145 / 577 / 2305), excluding the gendaymtx ground row
  float alpha = 0;                   // row height (rad) = (pi/2)/(7*mf + 0.5)
  std::vector<float> alt, azi;       // patch centre altitude / azimuth (rad), float as in gendaymtx; azi clockwise from +Y
  std::vector<float> omega;          // solid angle (sr); sum over sky patches = 2 pi
  std::vector<int> row, ninrow;      // row index of patch (0 = horizon row, 7*mf = zenith cap), patches in that row
  std::vector<float3> centre;        // gendaymtx centre vectors (cos alt sin azi, cos alt cos azi, sin alt)
  std::vector<float3> lb;            // ladybug trapezoid-normal vectors: same azimuth, tan(altLB) = cos(pi/ninrow) tan(alt)
};
Patches make_patches(int mf);
// Deterministic stratified sample k of K (K = m*m) inside patch p, uniform in solid angle; K == 1 -> exact centre.
float3 patch_sample(const Patches& pa, int p, int k, int K);
// gendaymtx.c ComputeSky + AddDirect for one timestep. out: 3*(P+1) floats RGB; index 0 = ground row, 1..P = sky
// patches in gendaymtx order (= Patches order). alt/azi radians, azi clockwise from north. sun / sky emulate -d / -s
// (both true = total). visible = true: -O0 path (Perez luminous efficacy, dew point 11 C, output = lux/179 per unit);
// false: -O1 radiometric (W/m2/sr). dni/dhi in W/m2. Night (dni+dhi <= 1e-4) -> all zero.
void gendaymtx_step(const Patches& pa, int jd, double alt, double azi, double dni, double dhi, double grefl,
                    bool sun, bool sky, bool visible, float* out);
// Radiance sun.c solar position. lonWestDeg / meridianWestDeg are west-positive degrees (meridian = -15*tz).
// hour = local standard decimal hour. Returns altitude (rad) and azimuth clockwise from north (rad) = sazi + pi.
void sun_radiance(double latDeg, double lonWestDeg, double meridianWestDeg, int jd, double hour, double& alt, double& azi);
double round_fmt(double x, const char* fmt);    // strtod(snprintf(fmt, x)): ASCII round trip, e.g. "%.3g" (gendaymtx), "%.2f" (.wea)
float3 dir_from_altaz(double altRad, double aziRad);   // (cos alt sin azi, cos alt cos azi, sin alt)
float3 rotate_z(float3 v, double deg);                  // counterclockwise about +Z (ladybug north)

// ------------------------------------------------------------------ solar.cpp
struct Sun { double alt = 0, azi = 0; bool up = false; float3 vec{0.f, 0.f, 0.f}; };  // degrees; azi clockwise from north; vec towards the sun (true north; run.cpp rotates)
// NOAA/Meeus solar position with ladybug Sunpath semantics (fixed year 2017 / 2016 leap, 0.01-day rounding of the
// day fraction, NOAA refraction added to altitude, azimuth from the unrefracted zenith, up = refracted alt >= 0).
Sun sun_noaa(double lat, double lon, double tz, int month, int day, double hourFrac, bool leap);
// Evaluation steps from Options (+ Weather). EPW-driven annual: one step per row at row%24 + 0.5 h, dt 1.
// User range / time: stamps and the --hours window in the user's clock, dt = stepMin/60; the sun is evaluated at the
// instant (--time) or the step middle (ranges), moved into the weather's clock by shift = user tz - weather tz (hours),
// with dni/dhi interpolated there (0 without weather).
bool make_steps(const Options& opt, const Weather* w, std::vector<Step>& out, std::string& err, double shift = 0);
// Parse "[YYYY-]MM-DDTHH:MM" to hour of year (leap-aware). Year ignored.
bool parse_hoy(const std::string& s, bool leap, double& hoy, std::string& err);
void hoy_to_date(double hoy, bool leap, int& month, int& day, double& hour);
double extraterrestrial_irradiance(int doy);   // Spencer 1971, W/m2 (pvlib get_extra_radiation default)
// Ineichen-Perez clear sky (pvlib 0.16.1 formulation, no Perez enhancement); apparent zenith clamped to [0, 90] deg,
// Linke turbidity TL, site altitude in metres (standard-atmosphere pressure). Outputs W/m2.
void ineichen(double apparentZenithDeg, double turbidity, double altitudeM, double dniExtra, double& ghi, double& dni, double& dhi);

// ------------------------------------------------------------------ mesh.cpp
struct Tris { std::vector<float3> v; std::vector<uint3> i; std::vector<uint32_t> slot; };   // slot: global slot per triangle (empty: not needed)
struct Sensors { std::vector<float3> p, n; };              // recentred, offset applied; sides already expanded
// Views of meshes for the helpers below (names and roles from the SimulationMesh itself), so run() and the tests share them.
inline std::vector<SimulationMeshView> views_of(const std::vector<SimulationMesh>& meshes) {
  std::vector<SimulationMeshView> v;
  v.reserve(meshes.size());
  for (const SimulationMesh& m : meshes) v.push_back({&m, m.name, m.sensor});
  return v;
}
// f(k) for k in [0, n) on the host's cores (std::execution::par); the first exception any call throws is rethrown here
// after all of them ran. Callers write disjoint output ranges, so the results equal a serial loop's.
void par_for(size_t n, const std::function<void(size_t)>& f);
void bbox_centre(const std::vector<SimulationMeshView>& meshes, double c[3]);
inline void bbox_centre(const std::vector<SimulationMesh>& meshes, double c[3]) { bbox_centre(views_of(meshes), c); }
// Triangles m can give at most: sum of (n - 2) over its faces of 3 or more corners (earcut gives no more).
size_t tri_cap(const SimulationMesh& m);
// Append the triangulated faces of m (recentred by c) to out, each triangle's global slot = slotBase + its face slot
// (none appended when withSlots is false: the GAS needs them only with per-slot records); skipped counts degenerate /
// < 3-vertex faces.
void triangulate(const SimulationMesh& m, const double c[3], Tris& out, int& skipped, uint32_t slotBase = 0, bool withSlots = true);
// The same into caller ranges: m's points (recentred) at v, at most tri_cap(m) triangles at tri (vertex indices + base)
// and their slots (when slots is not null). Returns the triangles written.
size_t triangulate_into(const SimulationMesh& m, const double c[3], float3* v, uint3* tri, uint32_t* slots, unsigned base, uint32_t slotBase, int& skipped);
// Global slots: every mesh in input order takes slot_count(m) = max face slot + 1 (1 without slots); mesh base = the sum
// over the meshes before it.
size_t slot_count(const SimulationMesh& m);
// The .dmesh array rules (daylight.h) on m: faces of 3 or more vertices, sum(counts) = indices, indices within the
// points; err starts with label. read_dmesh and the C ABI's dl_mesh_create share them.
bool mesh_ok(const SimulationMesh& m, const std::string& label, std::string& err);
// Optics rows within range (0..1, rho + tau <= 1; glass rho 0, 0 < tau <= 0.918), --bounces 0..20, --bounce-samples
// 64..65536 and the mode rules (ladybug: no transmission, no bounces). False + err naming the first bad row or flag.
bool optics_ok(const Options& opt, std::string& err);
// The row part of optics_ok (ranges, glass rules), for every tool that takes --optics (views: the rows only).
bool optics_rows_ok(const std::vector<Optics>& rows, std::string& err);
// One Slot per global slot from the rows (NAME/SLOT > NAME > * > rho 0.2 tau 0; glass: tn_from_Tn(tau), kSlotGlass).
// Errors: unknown or ambiguous mesh name, slot out of range, duplicate row, a SimulationMesh::slot not one per face.
bool resolve_optics(const std::vector<SimulationMeshView>& meshes, const std::vector<Optics>& rows, std::vector<Slot>& out, std::string& err);
inline bool resolve_optics(const std::vector<SimulationMesh>& meshes, const std::vector<Optics>& rows, std::vector<Slot>& out, std::string& err) {
  return resolve_optics(views_of(meshes), rows, out, err);
}
// open[k] = 1 when Options::noOcclude names mesh k (a name given twice counts once). Errors: an unknown or ambiguous
// name, a context mesh.
bool resolve_no_occlude(const std::vector<SimulationMeshView>& meshes, const std::vector<std::string>& names, std::vector<char>& open, std::string& err);
inline bool resolve_no_occlude(const std::vector<SimulationMesh>& meshes, const std::vector<std::string>& names, std::vector<char>& open, std::string& err) {
  return resolve_no_occlude(views_of(meshes), names, open, err);
}
// views::Options::targets: cls[k] = 3 + g when mesh k is a target of groups[g] (groups in order of first appearance), else
// 2 (an obstruction). Errors: NAME/SLOT (face-slot targets are not supported), a GROUP not [A-Za-z0-9_-]+ or reserved (sky,
// ground, obstruction, ground_obstruction, glazing, volume, area, distance*, reach*, *_distance, *_distance_min: column
// names), an unknown or ambiguous name, a
// mesh given twice, a mesh that blocks no rays (a sensor mesh under --no-self-occlude or open[k], --no-occlude), more
// than 16 groups.
bool resolve_targets(const std::vector<SimulationMeshView>& meshes, const std::vector<views::Target>& targets, bool selfOcclude,
                     const std::vector<char>& open, std::vector<std::string>& groups, std::vector<uint8_t>& cls, std::string& err);
// Faces of slot s in m (recentred by c) as one closed thin solid: positions welded on a 1e-5 m grid, every edge used
// as often in each direction (closed, consistently wound), 2 |V| / S < 0.1 m -> kSlotThin, | kSlotInward when the signed
// volume is negative, | kSlotTwin instead when 2 |V| / S is below the weld grid (faces repeated with the opposite
// winding: a double-faced sheet, or a box exported so, whose volumes cancel); else 0. ponytail: per slot, not per
// connected component (a pane box sharing its slot with an open sheet is not thin, so it passes tau twice); split the
// slot's faces into components if such slots matter.
unsigned thin_solid(const SimulationMesh& m, unsigned s, const double c[3]);
// Sensor points/normals per face or per point per opt (mode rules, offset, sides). Appends to out: one entry per
// element, or two consecutive entries (front, back) when sides == Both. Returns the element count (faces or points).
size_t make_sensors(const SimulationMesh& m, const Options& opt, const double c[3], Sensors& out, std::string& warn);
// The same into caller ranges p / n of elements x sides entries, by the ladybug mode's rules when ladybug (0 and the
// ladybug error in warn: nothing written).
size_t make_sensors_into(const SimulationMesh& m, const RunOptions& opt, bool ladybug, const double c[3], float3* p, float3* n, std::string& warn);

// ------------------------------------------------------------------ gpu.cu
struct Gpu;
Gpu* gpu_init(std::string& err);
void gpu_destroy(Gpu* g);
// GAS, compacted, inputs freed (t.slot < slots.size()). When a slot transmits (tau > 0) or perSlot is set (the bounce
// pass reads each slot's rho): one SBT record per slot (within the device's records per GAS), any-hit on the
// transmitting ones; else one record, the layout of an all-opaque scene. Geometry flags are fixed at build: a slot that
// starts or stops transmitting rebuilds.
bool gpu_build(Gpu& g, const Tris& t, const std::vector<Slot>& slots, std::string& err, bool perSlot = false);
// Device bytes gpu_build of nTris triangles over nVerts vertices peaks at (OptiX's estimate from the counts alone);
// SIZE_MAX with err when OptiX cannot say.
size_t gpu_build_bytes(Gpu& g, size_t nVerts, size_t nTris, bool slotted, std::string& err);
// Frees the per-run device buffers (chunk buffers; with sensors, the sensor points and normals) before a big allocation.
void gpu_trim(Gpu& g, bool sensors);
bool gpu_set_slots(Gpu& g, const std::vector<Slot>& slots, std::string& err);   // hit records (every run; same count as the build)
// A built acceleration structure moved out of the Gpu, kept in VRAM for a later run of the same scene.
struct GasSlot { void* dGas = nullptr; unsigned long long gas = 0; size_t bytes = 0; };
GasSlot gpu_detach(Gpu& g);              // the current GAS moves out; the Gpu then has none
void gpu_attach(Gpu& g, GasSlot s);      // frees the current GAS and installs s
void gpu_free(GasSlot& s);
size_t gpu_free_bytes();                 // free device memory (cudaMemGetInfo), 0 on error
bool gpu_set_scene(Gpu& g, const Sensors& s, const std::vector<float3>& dirs, std::string& err);   // resident sensors + directions
// Trace sensors [i0, i0+nc) against all directions; bits stay on device, word-major bits[w*nc + i]. Ray flags with
// any-hit on (gpu_ray_flags(.., true)): transmittance bytes[d*nc + i] instead (x255, 255 = 1), which reduce / series
// then weight by. Adaptive TDR-safe sub-launches inside (first 4096 sensors timed, then scaled to ~150 ms per launch;
// byte mode: 2^13 threads first and at least, then 30 ms, at most doubling).
bool gpu_trace(Gpu& g, size_t i0, size_t nc, unsigned rayFlags, std::string& err);
// out[i] = sum_d T(i,d) * (useCos ? max(0, N_i . D_d) : 1) * W[d], double accumulation; W has nDir host floats.
// T = the bit, or byte / 255.
bool gpu_reduce(Gpu& g, size_t nc, const float* W, bool useCos, double* out, std::string& err);
struct SeriesIn {
  unsigned nSky = 0;          // sky directions are 0..nSky-1
  const int* row = nullptr;   // [nSky] patch index per sky direction (host)
  const float* wsky = nullptr;// [nSky] weight per sky direction (omega/K physical, omega ladybug) (host)
  const float* S = nullptr;   // [P x T] patch radiance per step (host), row-major
  int P = 0, T = 0;
  const float* g = nullptr;   // [T] ground term per step (host)
  const float* G = nullptr;   // [nc] ground visibility per sensor (host, from gpu_reduce with ground weights)
  unsigned sun0 = 0, nSun = 0;// sun direction range in the direction table
  const int* sunStep = nullptr;    // [nSun] step (column) index per sun direction
  const float3* sunVec = nullptr;  // [nSun]
  const float* sunW = nullptr;     // [nSun] DNI (W/m2) or direct illuminance (lux) per sun direction
  const float* Sdc = nullptr;      // [(P+1) x T] radiance per gpu_bounce bin and step (host); null = no reflected light
};
// E[i*T + t] (host, nc x T floats) for the current chunk: sum_sky T*max(0,cos)*wsky*S[row,t] + G*g[t] + sun term
// [+ sum_b dc(b, i) * Sdc[b,t], the chunk's gpu_bounce rows].
bool gpu_series(Gpu& g, size_t nc, const SeriesIn& in, float* E, std::string& err);
// Reflected light of sensors [i0, i0+nc): M paths per sensor with up to N diffuse reflections (trace_dev.cu
// __raygen__bounce), binned by where they escape into rows dc(b, i) that stay on device (bin b = 0 ground, 1..P the
// Reinhart patches of mf in gendaymtx order, true north = the model's turned by -north; weight pi / M per path, so
// sum_b dc(b, i) L_b is the reflected irradiance under bin radiances L). Sub-launches over sample ranges of the whole
// chunk, TDR-safe like gpu_trace's byte mode (first 2^13 / nc samples per sensor, 1 to 64: about max(nc, 2^13) paths;
// then ~100 ms, at most doubling); cancel is checked between them.
struct Bounce { unsigned N = 1, M = 1024; int mf = 1; double north = 0; const std::atomic<bool>* cancel = nullptr; };
bool gpu_bounce(Gpu& g, size_t i0, size_t nc, unsigned rayFlags, const Bounce& b, std::string& err);
bool gpu_reduce_dc(Gpu& g, size_t nc, const float* W, double* out, std::string& err);   // out[i] = sum_b dc(b, i) * W[b], double
bool gpu_download_dc(Gpu& g, size_t nc, std::vector<float>& out, std::string& err);    // [(P+1) x nc] as on device
bool gpu_download_bits(Gpu& g, size_t nc, std::vector<uint32_t>& out, std::string& err);   // [nWords x nc] as on device
bool gpu_download_bytes(Gpu& g, size_t nc, std::vector<uint8_t>& out, std::string& err);   // byte mode: [nDir x nc]
// Views (views.cpp, trace_dev.cu __raygen__view): a run's grid of K = nAz x nEl directions per sensor, rows top-down,
// columns clockwise from az0, and how its rays end (see Params).
struct ViewLaunch {
  unsigned nAz = 1, nEl = 1;
  float az0 = 0, dAz = 1;              // degrees: the first column's left edge (-H / 2 about the gaze; 360 mode: 0 = north)
  const float* rowSin = nullptr;       // [2 nEl] host: sin of each row's bottom and top elevation
  bool world = false, cull = false, jitter = true, back = false, vground = true;
  unsigned nS = 1;                     // sides per element (sensor i is element i / nS: the seeds)
  float groundZ = 0;                   // the virtual ground plane (recentred)
  double north = 0;                    // degrees: true north of the 360 frame, a flat sensor's right (east turned)
  const std::atomic<bool>* cancel = nullptr;
};
// Class per triangle of the GAS (build order), read at views' hits; empty: none, every hit an obstruction.
bool gpu_set_labels(Gpu& g, const std::vector<uint8_t>& label, std::string& err);
// Trace sensors [i0, i0+nc) over the grid; classes and distances stay on device, direction-major [K x nc]. Sub-launches
// as gpu_trace's byte mode (2^13 threads first and at least, then ~40 ms, at most doubling), the size carried from chunk
// to chunk in launch (0 = start over); v.cancel is checked between them.
bool gpu_view(Gpu& g, size_t i0, size_t nc, unsigned rayFlags, const ViewLaunch& v, size_t& launch, std::string& err);
// out (host, nc x NA doubles, NA = 3 nClass + 1 + reach.size() + glazing + (isoDim > 0)) for the last gpu_view: per
// sensor and class the weight, weight x ln d and min d (d = the distance clamped to [0.1, farDist]; min +inf: the class
// is not in view), then the own-surface weight, then per reach threshold the weight whose distance is at least it, then
// (glazing) the weight that passed glass, then (isoDim 2 / 3) the isovist: the area of the polygon (one row) / the volume
// of the mesh through the rays' end points, each at min(true distance, farDist). wRow: weight per row.
// Deterministic: the same bits for any chunking.
bool gpu_reduce_view(Gpu& g, size_t nc, const std::vector<double>& wRow, unsigned nClass, double farDist, const std::vector<float>& reach,
                     bool glazing, unsigned isoDim, double* out, std::string& err);
bool gpu_download_view(Gpu& g, size_t nc, std::vector<uint8_t>& cls, std::vector<float>& dist, std::string& err);   // [nc x K] each (transposed)
struct GpuStats {
  double buildMs = 0, traceMs = 0, bounceMs = 0;
  size_t rays = 0, gasBytes = 0, launches = 0, paths = 0, bounceLaunches = 0;
  size_t buildTempBytes = 0, buildOutputBytes = 0;   // the last build's OptiX temp and uncompacted output
};
GpuStats gpu_stats(const Gpu& g);
// TERMINATE_ON_FIRST_HIT [| CULL_BACK_FACING_TRIANGLES], | DISABLE_ANYHIT unless a slot transmits (translucent)
unsigned gpu_ray_flags(bool cullBackfaces, bool translucent);
// The bounce pass's closest hits: DISABLE_ANYHIT [| CULL_BACK_FACING_TRIANGLES] (it handles translucent slots itself)
unsigned gpu_bounce_flags(bool cullBackfaces);

// ------------------------------------------------------------------ run.cpp
bool npy_write_header(std::FILE* f, size_t rows, size_t cols, const char* descr);   // .npy v1.0 [rows x cols] C order, descr "<f8" / "<f4" / "<u4" / "<u1"
// An occluder scene as the process keeps it (installed in the Gpu, or parked in VRAM under its key) and a run sees it.
struct SceneMesh {   // a mesh's triangles [tri0, tri0 + nTri) of the GAS, its name and SimulationMesh as the building run had them
  std::string name; size_t tri0 = 0, nTri = 0; const SimulationMesh* mesh = nullptr;
};
struct Scene {
  std::string key;
  double c[3] = {0, 0, 0};          // bbox centre the scene's triangles were recentred by
  size_t nTris = 0;
  int skipped = 0;                  // faces left out (degenerate / fewer than 3 vertices)
  std::vector<SceneMesh> order;     // build order: every blocking mesh in input order, named as the building run named it
  std::vector<unsigned> thin;       // per global slot: thin_solid() of the transmitting slots, from the build (empty: none)
  GasSlot gas;                      // parked scenes only
};
// A run's occluder scene, shared by the tools. A non-empty key (the caller's sceneKey plus whatever else its GAS depends
// on) reuses the resident scene of that key, installed or parked (out.sceneReused). Else the meshes k with blocks(k) are
// triangulated (recentred by the bbox centre of all meshes; a global slot per triangle when slotted) and built
// (gpu_build(slots, slotted)) once the build's peak fits: the per-run buffers go, then parked scenes, oldest first, else
// "out of memory: building the scene ...". The installed keyed scene is parked first when residentScenes > 1.
// s = the scene's record (no gas). gpu null: a run without rays, the lookup only (no GPU, nothing built or moved); else
// *gpu = the process's GPU (made on first use) with the scene installed. A set cancel fails with "cancelled" before any
// GPU work; out.stats.prepMs / buildMs grow by the triangulation / build; stage names each step (run()'s out-of-memory text).
bool scene_acquire(const std::vector<SimulationMeshView>& meshes, const std::function<bool(size_t)>& blocks, const std::vector<Slot>& slots,
                   bool slotted, const std::string& key, int residentScenes, const std::atomic<bool>* cancel, Gpu** gpu, Scene& s,
                   Result& out, const char*& stage, std::string& err);
// Room on the GPU beside the scene for a run of nSens sensors (points and normals) traced in chunks of chunkBytes /
// perSensor sensors (perSensor: the largest per-chunk buffer's bytes per sensor, chunkBytes caps each), chunkSensor
// device bytes per sensor of a chunk, fixedBytes more per run (views' labels). The last run's buffers go (gpu_trim), then
// parked scenes beyond residentScenes or in the way, then chunkBytes halves down to 32 MiB (no number depends on the
// chunking); else "out of memory: ...".
bool run_admission(Gpu& g, size_t nSens, size_t perSensor, size_t chunkSensor, size_t fixedBytes, int residentScenes, size_t& chunkBytes,
                   std::string& err);
// The --no-occlude part of a scene key ("\nopen k,k..."): the meshes left out of the GAS; none under --no-self-occlude,
// which leaves every sensor mesh out anyway (the caller's key carries selfOcclude), so there another set reuses the scene.
std::string open_key(const std::vector<char>& open, bool selfOcclude);
// The resident scene of key goes (installed: kept in the Gpu without a key; parked: freed): the next scene_acquire of key
// builds afresh.
void scene_forget(const std::string& key);
// The sensors of every sensor mesh (make_sensors_into, a task per mesh) into s, nSens entries in mesh order; out.ranges =
// (name, elements) per sensor mesh; the meshes' warnings joined into warn. False with err on the ladybug error.
bool sensors_of(const std::vector<SimulationMeshView>& meshes, const RunOptions& opt, bool ladybug, const double c[3], size_t nSens, Sensors& s,
                Result& out, std::string& warn, std::string& err);

}  // namespace daylight
