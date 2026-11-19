// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// views: GPU view analysis on the daylight library. From every face or point of the receiver meshes, rays over a field
// of view (about the normal, or all around): the shares that end on sky, ground, an obstruction or a target group, and
// how far the sensor sees. Same meshes, roles, resident scenes and GPU as daylight's run().
#pragma once
#include "daylight.h"

namespace daylight::views {

// Rays ending on mesh `mesh` (Mesh::name, as --no-occlude) count for `group` ([A-Za-z0-9_-]+, not sky, ground,
// obstruction, glazing, distance*, reach*, *_distance or *_distance_min); several meshes may share a group, at most 16 groups.
struct Target { std::string mesh, group; };

struct Options : RunOptions {
  // The field, degrees: H wide (0 < H <= 180 about the sensor's normal, or 360: all around, azimuth from north) from
  // elevation LO to HI (-90 <= LO < HI <= 90), one ray per cell of `cell` degrees, at most 2^20 per sensor.
  double fovH = 120, elevLo = -70, elevHi = 50, cell = 2;
  bool jitter = true;                 // each ray at a seeded random point of its cell (uniform in solid angle); false = the centre
  std::vector<Target> targets;
  bool distMean = true, distMin = true, distGroup = false;   // distance columns: log mean, minimum (group: also per group)
  double farDist = 1000;              // metres (> 0.1): what open counts as, and the cap of every distance
  double groundZ = NAN;               // the virtual ground's height (metres); NAN = the lowest point of the meshes
  std::vector<double> reach;          // reach_<D> columns: the share of the field that sees at least D metres (at most 8)
  std::string dumpPath;               // classes uint8 [rows x rays] (.npy, a row per element and side) + dumpPath.depth.npy
                                      // float32 distances; empty = off; a failed run removes both
};

// What run() derives from opt before tracing (a manifest needs it too): the option checks (false with err), the grid of
// nEl rows x nAz columns of rays per sensor and the virtual ground's height (opt.groundZ, else the meshes' lowest point).
struct Field { unsigned nAz = 0, nEl = 0; double groundZ = NAN; };
bool field(const std::vector<MeshView>& meshes, const Options& opt, Field& f, std::string& err);

// Rows as daylight's run(): sensor meshes in input order, a row per face or point. Columns <name>_<side> (sides inner):
// the shares sky, ground, obstruction and one per group (first appearance), summing to 1; distance, distance_min (log mean
// and minimum of the distances clamped to [0.1, farDist]; NaN when no ray is traced: all own surface, as a degenerate
// element's); per group <g>_distance, <g>_distance_min (NaN: not in view); reach_<D>. Result::nSky, nSun and
// durationHours stay 0. Fails on bad options, target or --no-occlude names the meshes do not resolve, and memory running
// out ("out of memory: ...", never the process). One call at a time per process.
bool run(const std::vector<MeshView>& meshes, const Options& opt, Result& out, std::string& err);
bool run(const std::vector<Mesh>& meshes, const Options& opt, Result& out, std::string& err);

}  // namespace daylight::views
