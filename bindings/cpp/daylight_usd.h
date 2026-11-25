// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// USD stage -> daylight::SimulationMesh[] (LightUSD). Optional binding, not part of the core (bindings/cpp/CMakeLists.txt).
#pragma once
#include "daylight.h"
#include <string>
#include <vector>

namespace daylight {
// Appends every drawn UsdGeomMesh of the composed stage at `path` (.usd/.usda/.usdc/.usdz; sublayers, references,
// payloads, inherits, variants resolved) to `out`, in world metres, Z-up, one SimulationMesh per mesh named by its
// prim path. Instanceable prims are drawn under their own path; a PointInstancer draws each visible instance as
// "<instancer>[k]<prototype mesh path>". Skipped: inactive, class, invisible and proxy / guide prims and hole faces.
// Faces wind by the right-hand rule in world space: leftHanded and mirrored meshes are flipped back.
// upAxis: 'A' = stage metadata (fallback Y), 'Y', 'Z'. metersPerUnit <= 0 = authored value (USD fallback 0.01).
// false with `err` ("<path>: ...") on a load, composition or topology error; `out` may then hold the meshes before it.
bool read_usd(const std::string& path, char upAxis, double metersPerUnit, std::vector<SimulationMesh>& out, std::string& err);
}  // namespace daylight
