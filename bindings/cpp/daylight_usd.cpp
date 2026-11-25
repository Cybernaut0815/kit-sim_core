// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// USD reader: LightUSD composed stage (sublayers, references, payloads, ...) -> SimulationMesh[] in world metres, Z-up.
#include "daylight_usd.h"
#include <unordered_map>
#pragma warning(push)
#pragma warning(disable : 4127)   // LightUSD headers: constant conditionals in templates
#include <composition.hh>
#include <core/model-scope.hh>
#include <io-util.hh>
#include <lightusd.hh>
#include <tydra/scene-access.hh>
#include <usdGeom.hh>
#pragma warning(pop)

namespace daylight {
namespace {

// visibility (default time), purpose and specifier of an imageable prim of type T; false when the prim is not a T.
// The specifier is the typed prim's: LightUSD leaves Prim::specifier() at def.
template <class T>
bool imageable_of(const lightusd::Prim& p, lightusd::Visibility& v, lightusd::Purpose& pu, lightusd::Specifier& s) {
  const T* t = p.as<T>();
  if (!t) return false;
  t->visibility.get_value().get(lightusd::value::TimeCode::Default(), &v);
  pu = t->purpose.get_value();
  s = t->spec;
  return true;
}

// True for a prim whose subtree is not drawn: inactive, a class (Usd.PrimRange's default predicate), invisible, or
// proxy / guide purpose. Instanceable prims are drawn: composition puts their prototype's prims under them.
bool prim_skipped(const lightusd::Prim& p) {
  if (!p.metas().get_active()) return true;
  lightusd::Visibility v = lightusd::Visibility::Inherited;
  lightusd::Purpose pu = lightusd::Purpose::Default;
  lightusd::Specifier s = p.specifier();
  if (const auto* untyped = p.as<lightusd::Model>()) s = untyped->spec;
  (void)(imageable_of<lightusd::Xform>(p, v, pu, s) || imageable_of<lightusd::Scope>(p, v, pu, s) ||
         imageable_of<lightusd::GeomMesh>(p, v, pu, s) || imageable_of<lightusd::GeomPointInstancer>(p, v, pu, s));
  return s == lightusd::Specifier::Class || v == lightusd::Visibility::Invisible || pu == lightusd::Purpose::Proxy ||
         pu == lightusd::Purpose::Guide;
}

using Matrix = lightusd::value::matrix4d;

// a x b in the row-vector convention: p * (a x b) applies a first.
Matrix mul(const Matrix& a, const Matrix& b) {
  Matrix r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
  return r;
}

// Row-vector convention: p' = [p,1] * W. Winding flipped when leftHanded XOR det(W3) < 0; authored normals ignored.
// Zero-copy reference to the default value of a typed attribute; time-sampled attributes fall back to a copy in tmp.
template <class T, class F>
const T& scalar_or(const lightusd::TypedAttribute<lightusd::Animatable<T>>& a, T& tmp, F fallback) {
  const auto& v = a.get_value_ref();
  return v && v->is_scalar() ? v->get_scalar_ref() : (tmp = fallback());
}

bool convert(const lightusd::GeomMesh& g, const lightusd::value::matrix4d& W, const std::string& name, bool yup,
             double mpu, std::vector<SimulationMesh>& out, std::string& err) {
  std::vector<lightusd::value::point3f> ptsTmp;
  std::vector<int32_t> cntTmp, idxTmp;
  const auto& pts = scalar_or(g.points, ptsTmp, [&] { return g.get_points(); });
  const auto& cnt = scalar_or(g.faceVertexCounts, cntTmp, [&] { return g.get_faceVertexCounts(); });
  const auto& idx = scalar_or(g.faceVertexIndices, idxTmp, [&] { return g.get_faceVertexIndices(); });
  if (pts.empty() || cnt.empty()) return true;
  const double (*m)[4] = W.m;
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  SimulationMesh mesh;
  mesh.name = name;
  mesh.points.resize(pts.size() * 3);
  for (size_t i = 0; i < pts.size(); ++i) {
    const auto& p = pts[i];
    double v[3];
    for (int j = 0; j < 3; ++j) v[j] = (p.x * m[0][j] + p.y * m[1][j] + p.z * m[2][j] + m[3][j]) * mpu;
    if (yup) { const double y = v[1]; v[1] = -v[2]; v[2] = y; }
    mesh.points[3 * i] = v[0]; mesh.points[3 * i + 1] = v[1]; mesh.points[3 * i + 2] = v[2];
  }
  mesh.counts.reserve(cnt.size()); mesh.indices.reserve(idx.size());
  std::vector<int32_t> holes;
  if (const auto h = g.holeIndices.get_value()) h->get(lightusd::value::TimeCode::Default(), &holes);
  std::vector<char> isHole(cnt.size(), 0);
  for (const int h : holes) if (h >= 0 && size_t(h) < cnt.size()) isHole[size_t(h)] = 1;
  // subdivisionScheme != none: the control cage is used as is.
  const bool flip = (g.orientation.get_value() == lightusd::Orientation::LeftHanded) != (det < 0);
  const int np = int(pts.size());
  size_t off = 0;
  for (size_t f = 0; f < cnt.size(); ++f) {
    const int n = cnt[f];
    if (n < 0 || off + size_t(n) > idx.size()) { err = name + ": faceVertexCounts/faceVertexIndices mismatch"; return false; }
    if (!isHole[f]) {
      mesh.counts.push_back(n);
      for (int k = 0; k < n; ++k) {
        const int i = idx[off + size_t(flip ? n - 1 - k : k)];
        if (i < 0 || i >= np) { err = name + ": faceVertexIndices out of range"; return false; }
        mesh.indices.push_back(i);
      }
    }
    off += size_t(n);
  }
  out.push_back(std::move(mesh));
  return true;
}

// One read: the stage's axis and unit, the output, and every node of the transform tree by path (prototype lookup).
struct Walk {
  bool yup;
  double mpu;
  std::vector<SimulationMesh>& out;
  std::string& err;
  std::unordered_map<std::string, const lightusd::tydra::XformNode*> nodes;
};

void index_nodes(const lightusd::tydra::XformNode& n, Walk& w) {
  w.nodes.emplace(n.absolute_path.full_path_name(), &n);
  for (const auto& c : n.children) index_nodes(c, w);
}

bool walk(const lightusd::tydra::XformNode& n, const Matrix& world, const std::string& prefix, bool inPrototype, int depth, Walk& w);

// Each visible instance of a point instancer: its prototype's subtree again, at prototype xform x scale x orientation
// x position x the instancer's world matrix, named "<instancer>[k]<mesh path>" (daylight.usd's names). The
// instancer's own children are only drawn this way.
bool instances(const lightusd::GeomPointInstancer& pi, const std::string& name, const Matrix& world, int depth, Walk& w) {
  if (depth >= 8) { w.err = name + ": point instancers nested more than 8 deep"; return false; }
  std::vector<std::string> prototypes;
  if (pi.prototypes) {
    if (pi.prototypes->is_path()) prototypes.push_back(pi.prototypes->targetPath.full_path_name());
    else if (pi.prototypes->is_pathvector())
      for (const auto& target : pi.prototypes->targetPathVector) prototypes.push_back(target.full_path_name());
  }
  const double t = lightusd::value::TimeCode::Default();
  const std::vector<int32_t> protoIndices = pi.get_protoIndices(t);
  std::vector<Matrix> placements;
  std::vector<bool> shown;
  std::string e;
  if (!lightusd::ComputeInstanceTransformsAtTime(pi, t, lightusd::value::TimeSampleInterpolationType::Linear, &placements, &e) ||
      !lightusd::ComputeMaskAtTime(pi, t, &shown, &e)) {
    w.err = name + ": " + e;
    return false;
  }
  for (size_t k = 0; k < protoIndices.size() && k < placements.size(); ++k) {
    if (k < shown.size() && !shown[k]) continue;
    const int32_t p = protoIndices[k];
    if (p < 0 || size_t(p) >= prototypes.size()) {
      w.err = name + ": protoIndices[" + std::to_string(k) + "] = " + std::to_string(p) + ", but " +
              std::to_string(prototypes.size()) + " prototypes";
      return false;
    }
    const auto it = w.nodes.find(prototypes[size_t(p)]);
    if (it == w.nodes.end()) continue;   // an inactive or missing prototype draws nothing, as in pxr
    const auto& proto = *it->second;
    const Matrix at = mul(mul(proto.get_local_matrix(), placements[k]), world);
    if (!walk(proto, at, name + "[" + std::to_string(k) + "]", true, depth + 1, w)) return false;
  }
  return true;
}

// Every drawn mesh at or under n into w.out, n at `world`, its names behind `prefix`. Under a point instance
// (inPrototype) the children's world matrices are composed here; elsewhere they are the transform tree's.
bool walk(const lightusd::tydra::XformNode& n, const Matrix& world, const std::string& prefix, bool inPrototype, int depth, Walk& w) {
  if (n.prim) {
    if (prim_skipped(*n.prim)) return true;
    const std::string name = prefix + n.absolute_path.full_path_name();
    if (const auto* g = n.prim->as<lightusd::GeomMesh>())
      if (!convert(*g, world, name, w.yup, w.mpu, w.out, w.err)) return false;
    if (const auto* pi = n.prim->as<lightusd::GeomPointInstancer>()) return instances(*pi, name, world, depth, w);
  }
  for (const auto& c : n.children) {
    const Matrix at = !inPrototype ? c.get_world_matrix() : c.has_resetXformStack() ? c.get_local_matrix() : mul(c.get_local_matrix(), world);
    if (!walk(c, at, prefix, inPrototype, depth, w)) return false;
  }
  return true;
}

}  // namespace

bool read_usd(const std::string& path, char upAxis, double metersPerUnit, std::vector<SimulationMesh>& out, std::string& err) {
  std::string warn, lerr;
  lightusd::USDLoadOptions lopt;
  lopt.max_memory_limit_in_mb = 16384;   // LightUSD default 512 MB rejects 1M-face stages
  lightusd::Layer layer;
  if (!lightusd::LoadLayerFromFile(path, &layer, &warn, &lerr, lopt)) {   // UTF-8 paths: io-util.cc UTF8ToWchar
    err = path + ": " + lerr;
    return false;
  }
  std::string base = lightusd::io::GetBaseDir(path);
  if (base.empty()) base = ".";
  lightusd::AssetResolutionResolver resolver;
  resolver.set_current_working_path(base);
  resolver.set_search_paths({base});
  lightusd::SublayersCompositionOptions sopt;
  sopt.allow_parent_relative_paths = sopt.error_when_asset_not_found = true;   // LightUSD default: warn + drop
  lightusd::AllArcsCompositionOptions aopt;
  aopt.references.allow_parent_relative_paths = aopt.payload.allow_parent_relative_paths = true;
  aopt.references.error_when_asset_not_found = aopt.payload.error_when_asset_not_found = true;
  const std::string hint = ": composition failed, flatten the stage first (usdcat --flatten): ";
  // USD resolves stage metadata over the root layer stack; CompositeSublayers keeps only the root layer's metas.
  // ponytail: one level of sublayers (strongest first), nested subLayers of a sublayer are not visited.
  for (const auto& sl : layer.metas().subLayers) {
    lightusd::LayerMetas& rm = layer.metas();
    if (rm.upAxis.authored() && rm.metersPerUnit.authored()) break;
    lightusd::Layer sub;
    if (!lightusd::LoadLayerFromFile(resolver.resolve(sl.assetPath.GetAssetPath()), &sub, &warn, &lerr, lopt)) continue;
    if (!rm.upAxis.authored()) rm.upAxis = sub.metas().upAxis;
    if (!rm.metersPerUnit.authored()) rm.metersPerUnit = sub.metas().metersPerUnit;
  }
  if (!layer.metas().subLayers.empty()) {
    lightusd::Layer next;
    if (!lightusd::CompositeSublayers(resolver, layer, &next, &warn, &lerr, sopt)) { err = path + hint + lerr; return false; }
    layer = std::move(next);
  }
  for (int it = 0; layer.check_unresolved_references() || layer.check_unresolved_payload() || layer.check_unresolved_inherits() ||
                   layer.check_unresolved_variant() || layer.check_unresolved_specializes(); ++it) {
    lightusd::Layer next;
    if (it == 32 || !lightusd::CompositeAllArcs(resolver, layer, &next, &warn, &lerr, aopt)) {
      err = path + hint + (it == 32 ? "arcs still unresolved after 32 passes" : lerr);
      return false;
    }
    layer = std::move(next);
  }
  lightusd::Stage stage;
  if (!lightusd::LayerToStage(std::move(layer), &stage, &warn, &lerr)) { err = path + hint + lerr; return false; }
  const lightusd::StageMetas& sm = stage.metas();
  // USD spec fallbacks: metersPerUnit 0.01 (LightUSD's own is 1.0, hence authored()), upAxis Y. Arguments override.
  const double mpu = metersPerUnit > 0 ? metersPerUnit : sm.metersPerUnit.authored() ? sm.metersPerUnit.get_value() : 0.01;
  const bool yup = upAxis == 'Z' ? false : upAxis == 'Y' ? true : !(sm.upAxis.authored() && sm.upAxis.get_value() == lightusd::Axis::Z);
  lightusd::tydra::XformNode root;
  if (!lightusd::tydra::BuildXformNodeFromStage(stage, &root)) { err = path + ": cannot build the transform tree"; return false; }
  Walk w{yup, mpu, out, err, {}};
  index_nodes(root, w);
  return walk(root, root.get_world_matrix(), "", false, 0, w);
}

}  // namespace daylight
