// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// Launch parameters and hit-record data shared by the OptiX device program (trace_dev.cu) and the host (gpu.cu, run.cpp).
#pragma once
#include <vector_types.h>

// Optics of one global slot (mesh base + face slot), the data of its hit record. Rewritten every run.
struct Slot {
  float rho = 0.2f;       // diffuse reflectance (the bounce pass)
  float tau = 0.f;        // transmittance at normal incidence; > 0 = translucent (any-hit), 0 = opaque
  float tn = 0.f;         // glass transmissivity (tn_from_Tn(tau)), used with kSlotGlass
  unsigned flags = 0;     // kSlot*
};
enum : unsigned {
  kSlotGlass = 1,         // transmittance by angle (Radiance glass); else tau at every angle (porous, --fresnel off)
  kSlotThin = 2,          // closed thin solid (a pane modelled as a box): the exit face passes everything
  kSlotInward = 4,        // ... wound inward: entry is the back face
  kSlotTwin = 8,          // ... of no volume, faces in coincident pairs (a double-faced sheet): closest hits meet either twin
                          // first, so the bounce pass takes every face as an entry (one event per pair); any-hit sees both
};
// views: the label of a triangle rays pass (glass with --transmission on), the panes a ray passes at most (then an
// obstruction at the last), and the class bit of a ray that passed glass (cls | kViewGlazed)
constexpr unsigned char kLabelPass = 255;
constexpr unsigned kMaxPanes = 16;
constexpr unsigned char kViewGlazed = 0x80;

struct Params {
  unsigned long long gas; // OptixTraversableHandle
  const float3* P;        // sensor origins of this launch (device, already offset by i0)
  const float3* N;        // sensor normals
  const float3* D;        // direction table
  unsigned nDir;
  unsigned nc;            // sensors in the chunk (row stride of bits / bytes)
  unsigned rayFlags;      // OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT [| DISABLE_ANYHIT] [| CULL_BACK_FACING_TRIANGLES]
  unsigned pad;
  unsigned* bits;         // __raygen__bits: word-major bits[w*nc + i], bit k of word w = direction w*32+k visible
  unsigned char* bytes;   // __raygen__bytes (any-hit on): transmittance bytes[d*nc + i], 255 = 1
  // __raygen__bounce (reflected light): rows dc[b*nc + i] of the chunk, samples [s0, s1) of M per sensor
  float* dc;              // bin b = 0 ground (below the horizon), 1..nBins-1 the Reinhart patches of mf (gendaymtx order)
  unsigned i0;            // global index of the chunk's first sensor (seeds its samples)
  unsigned s0, s1, M;
  unsigned bounces;       // diffuse reflections a path may have
  unsigned mf, nBins;
  float alpha;            // patch row height (rad), Patches::alpha
  float cosN, sinN;       // cos / sin of --north: turns escaping directions back to true north (views: true north's frame)
  // __raygen__view (views): direction-major cls[d*nc + i] / dist[d*nc + i] (from the launch's first sensor; nc the
  // chunk's), K = nAz x nEl directions, d = row * nAz + column, rows top-down, columns clockwise from az0
  unsigned char* cls;     // 0 sky, 1 ground, 2 obstruction, 3 + g target group g (+ kViewGlazed: passed glass), 254 own
                          // surface (not traced)
  float* dist;            // true distance: the hit's t, the virtual ground's, +inf open sky / own surface
  const unsigned char* label;   // class per triangle (primitive index), kLabelPass: passed; null: every hit an obstruction
  const float2* rowSin;   // per row: sin of its bottom and top elevation
  unsigned nAz, nEl;
  float az0, dAz;         // degrees: the first column's left edge, the column width
  unsigned world;         // 360 mode: the world frame (true north, horizon); else the sensor's (gaze = normal)
  unsigned cull;          // own surface: directions behind the sensor (n . d <= 0) are not traced
  unsigned jitter;        // one point per cell uniform in solid angle, seeded by (element, cell, side); else the centre
  unsigned nS, back;      // sides per element; the one side is the back (seeds)
  unsigned vground;       // the virtual ground plane at z = groundZ (recentred) ends downward rays
  float groundZ;
};

#ifdef __CUDACC__
// Jarzynski & Olano 2020 (JCGT 9(3), "Hash Functions for GPU Rendering"): counter-based, 3 x 32 random bits per call.
static __forceinline__ __device__ uint3 pcg3d(unsigned x, unsigned y, unsigned z) {
  x = x * 1664525u + 1013904223u; y = y * 1664525u + 1013904223u; z = z * 1664525u + 1013904223u;
  x += y * z; y += z * x; z += x * y;
  x ^= x >> 16; y ^= y >> 16; z ^= z >> 16;
  x += y * z; y += z * x; z += x * y;
  return make_uint3(x, y, z);
}
static __forceinline__ __device__ float u01(unsigned v) { return float(v >> 8) * (1.f / 16777216.f); }   // [0, 1)

// views: direction d = row * nAz + column of the grid in the field's own frame (x right, y forward, z up): one point per
// cell, jittered uniform in its solid angle (seeded by the global element, d and the side) or the cell's middle in
// azimuth and in sine of elevation. __raygen__view turns it into the sensor's frame, k_view's isovist uses it as is.
// rs: the row's sines (bottom, top), col: d % nAz.
static __forceinline__ __device__ float3 view_cell(unsigned d, unsigned col, float2 rs, float az0, float dAz, unsigned jitter, unsigned element,
                                                   unsigned side) {
  float a = 0.5f, s = 0.5f * (rs.x + rs.y);
  if (jitter) { const uint3 h = pcg3d(element, d, side); a = u01(h.x); s = rs.x + u01(h.y) * (rs.y - rs.x); }
  float sa, ca;
  sincospif((az0 + (float(col) + a) * dAz) * (1.f / 180.f), &sa, &ca);
  const float c = sqrtf(fmaxf(0.f, 1.f - s * s));
  return make_float3(c * sa, c * ca, s);
}
static __forceinline__ __device__ float3 view_local(unsigned d, unsigned nAz, const float2* rowSin, float az0, float dAz, unsigned jitter,
                                                    unsigned element, unsigned side) {
  return view_cell(d, d % nAz, rowSin[d / nAz], az0, dAz, jitter, element, side);
}
#endif
