// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// CLI flag parsing, the post-parse weather / time rules and the output manifest, shared by main.cpp and the server;
// daylight's, then views'.
#include "cli.h"
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>

namespace daylight::cli {

const char* const kUsage =
    "usage: daylight <mesh>... [flags]\n"
    "Annual daylight and solar simulation: a row per face (or vertex) of the meshes, a column per quantity and side.\n"
    "\n"
    "meshes\n"
    "  <mesh>...                analysed (sensor) meshes, which also cast shade: .obj (one o/g group per mesh), .dmesh\n"
    "                           (raw arrays, layout in daylight.h) or .usd/.usda/.usdc/.usdz (-DDAYLIGHT_USD=ON builds;\n"
    "                           instances and point instancers expanded); world metres, Z-up\n"
    "  --context FILE           a mesh that only casts shade, never analysed (repeatable)\n"
    "  --on faces|vertices      a row per face (sensor at its centroid) or per vertex (default faces)\n"
    "  --sides front|back|both  receiving side(s) of each element; both: two columns per quantity (default front)\n"
    "  --offset M               sensors moved M metres along their normal (default 0)\n"
    "  --cull-backfaces         occluders block only from their front side (default both sides)\n"
    "  --no-self-occlude        sensor meshes cast no shade (only --context meshes do)\n"
    "  --no-occlude NAME[;NAME...]  these sensor meshes are analysed but cast no shade (NAME as in --optics; repeatable)\n"
    "\n"
    "site and weather\n"
    "  --epw FILE               hourly weather and location (EnergyPlus weather file)\n"
    "  --clear-sky              synthetic cloudless year (Ineichen) at --lat --lon instead of --epw: an upper bound\n"
    "  --turbidity TL           Linke turbidity for --clear-sky (default 3; 2 very clear .. 6-7 polluted)\n"
    "  --altitude M             site altitude in metres for --clear-sky (default 0)\n"
    "  --lat DEG  --lon DEG     location, N and E positive: needed without --epw, else overrides its location\n"
    "  --tz H                   clock of the times below, standard time (default: the EPW's zone, else round(lon / 15))\n"
    "  --north DEG              model north, degrees counterclockwise from +Y (default 0; 90 = west)\n"
    "\n"
    "time (default: annual, every EPW row)\n"
    "  --time [YYYY-]MM-DDTHH:MM       one instant\n"
    "  --from TS --to TS [--step MIN]  the range [from, to) in steps of MIN minutes (default 60)\n"
    "  --hours HH:MM-HH:MM             with --from/--to: only these hours of every day (an analysis period)\n"
    "\n"
    "quantities and sky\n"
    "  --quantity Q[,Q...]      a column each: radiation (kWh/m2), direct, diffuse, irradiance (W/m2), illuminance (lux),\n"
    "                           sunhours (h) (default radiation; sunhours alone needs no weather)\n"
    "  --mode physical|ladybug  sky and sun model (default physical)\n"
    "  --sky-density 1|2|4      sky patches 145 / 577 / 2305 (default 1)\n"
    "  --patch-samples K        physical mode: K stratified rays per sky patch, a square number (default 1)\n"
    "  --ground RHO             ground reflectance 0..1 (default 0.2)\n"
    "  --virtual-ground on|off  ground below the horizon reflecting --ground RHO (default on; off: black, as --ground 0)\n"
    "\n"
    "materials and reflected light\n"
    "  --optics SPEC[;SPEC...]  NAME[/SLOT]=RHO[,TAU[,glass|porous]] per mesh or face slot, * = every mesh (repeatable;\n"
    "                           default rho 0.2, tau 0)\n"
    "  --transmission off|on    light through glass and porous slots (default off: every surface opaque)\n"
    "  --fresnel on|off         glass by angle (default on; off: tau at every angle, no glass mirror in --bounces)\n"
    "  --bounces N              up to N = 0..20 diffuse reflections by RHO (physical mode; default 0: none)\n"
    "  --bounce-samples M       paths per sensor for --bounces, 64..65536 (default 1024; noise falls as 1 / sqrt(M))\n"
    "\n"
    "output\n"
    "  --out FILE[.npy]         values as text rows, or float64 [N x C] NumPy when .npy, and the FILE.json manifest\n"
    "                           (default: text on stdout, no manifest)\n"
    "  --timeseries F.npy       per-step values, float32 [N x T], and F.npy.hoys.txt (the steps' hours of year)\n"
    "  -v                       timings and sizes on stderr\n"
    "\n"
    "diagnostics\n"
    "  --dump-bits F.npy        visibility per sensor and direction (validation)\n"
    "  --dump-sky F             the gendaymtx sky blocks (ladybug mode; validation)\n"
    "  --dump-sun F             per step: date, sun positions, DNI and DHI (validation)\n"
    "  --bench [NSENS NTRIS]    a synthetic scene (default 1000000 sensors, 10000000 triangles): timings only\n"
    "  -h, --help               this text\n"
    "\n"
    "examples\n"
    "  daylight facade.obj --context site.obj --epw site.epw --quantity radiation,sunhours --out facade.npy\n"
    "  daylight roof.dmesh --lat 51.5 --lon -0.13 --from 06-21T08:00 --to 06-21T18:00 --quantity sunhours\n"
    "  daylight room.obj --epw site.epw --quantity illuminance --bounces 5 --optics \"*=0.5\" --out room.txt\n"
    "\n"
    "exit codes: 0 ok, 1 usage error (the message and this text on stderr), 2 runtime error\n";

namespace {

// Q[,Q...] as kQuantityNames (no blanks), in the order given; false at the first unknown name.
bool parse_quantities(const std::string& s, std::vector<Quantity>& out) {
  out.clear();
  for (size_t b = 0, e; b <= s.size(); b = e + 1) {
    e = std::min(s.find(',', b), s.size());
    size_t q = 0;
    while (q < std::size(kQuantityNames) && s.compare(b, e - b, kQuantityNames[q])) ++q;
    if (q == std::size(kQuantityNames)) return false;
    out.push_back(static_cast<Quantity>(q));
  }
  return true;
}

// SPEC[;SPEC...], SPEC = NAME[/SLOT]=RHO[,TAU[,glass|porous]]: split at the last '=', /SLOT only as a trailing '/' + digits
// (Facade/East is a name), kind porous when TAU > 0 unless given. Ranges are optics_ok's.
// ponytail: names containing ';' cannot be addressed (app mesh names are ids)
bool parse_optics(const std::string& s, std::vector<Optics>& out, std::string& err) {
  for (size_t b = 0, e; b <= s.size(); b = e + 1) {
    e = std::min(s.find(';', b), s.size());
    const std::string spec = s.substr(b, e - b);
    const auto bad = [&](const std::string& what) { err = "--optics " + spec + ": " + what; return false; };
    const size_t eq = spec.rfind('=');
    if (eq == std::string::npos || eq == 0) return bad("NAME[/SLOT]=RHO[,TAU[,glass|porous]]");
    Optics o;
    o.mesh = spec.substr(0, eq);
    const size_t sl = o.mesh.rfind('/');
    if (sl != std::string::npos && sl + 1 < o.mesh.size() && o.mesh.find_first_not_of("0123456789", sl + 1) == std::string::npos) {
      unsigned long v = 0;
      const auto r = std::from_chars(o.mesh.data() + sl + 1, o.mesh.data() + o.mesh.size(), v);
      if (r.ec != std::errc() || v > 65535) return bad("SLOT 0..65535");
      o.slot = int(v);
      o.mesh.resize(sl);
      if (o.mesh.empty() || o.mesh == "*") return bad("/SLOT needs a mesh NAME");
    }
    std::vector<std::string> v;   // RHO [TAU [kind]]
    for (size_t f = eq + 1, c; f <= spec.size(); f = c + 1) { c = std::min(spec.find(',', f), spec.size()); v.push_back(spec.substr(f, c - f)); }
    if (v.size() > 3) return bad("NAME[/SLOT]=RHO[,TAU[,glass|porous]]");
    double x[2] = {0, 0};
    for (size_t k = 0; k < v.size() && k < 2; ++k) {
      char* end = nullptr;
      x[k] = std::strtod(v[k].c_str(), &end);
      if (v[k].empty() || *end) return bad("bad number '" + v[k] + "'");
    }
    if (v.size() == 3 && v[2] != "glass" && v[2] != "porous") return bad("kind glass or porous");
    o.rho = x[0]; o.tau = x[1];
    o.glass = v.size() == 3 && v[2] == "glass";   // porous by default (opaque when TAU = 0)
    out.push_back(o);
  }
  return true;
}

// The value of the flag at argv[i] (i moves to it), else null with err = "missing value for FLAG".
const char* value(int argc, char** argv, int& i, const char* flag, std::string& err) {
  if (i + 1 >= argc) { err = std::string("missing value for ") + flag; return nullptr; }
  return argv[++i];
}

// The value of the flag at argv[i] as one of `words` (i moves to it): the word's index, else -1 with err set
// ("missing value for FLAG", or "FLAG w0|w1|...").
int word(int argc, char** argv, int& i, const char* flag, std::initializer_list<const char*> words, std::string& err) {
  const char* v = value(argc, argv, i, flag, err);
  if (!v) return -1;
  int k = 0;
  for (const char* w : words) { if (!std::strcmp(v, w)) return k; ++k; }
  err = flag;
  k = 0;
  for (const char* w : words) err += std::string(k++ ? "|" : " ") + w;
  return -1;
}

// Comma-separated numbers, each a whole token and finite ('.' is the decimal point; "1,5" is two numbers).
bool numbers(const std::string& s, std::vector<double>& out) {
  out.clear();
  for (size_t b = 0, e; b <= s.size(); b = e + 1) {
    e = std::min(s.find(',', b), s.size());
    const std::string t = s.substr(b, e - b);
    char* end = nullptr;
    out.push_back(std::strtod(t.c_str(), &end));
    if (t.empty() || *end || !std::isfinite(out.back())) return false;
  }
  return true;
}

// The value of the flag at argv[i] as one finite number into `to` (i moves to it), else false with err set ("missing
// value for FLAG", or "FLAG WHAT: 'v' is not a number", e.g. "--lat DEG: ...": a decimal comma or trailing text is none).
bool real(int argc, char** argv, int& i, const char* flag, const char* what, double& to, std::string& err) {
  const char* v = value(argc, argv, i, flag, err);
  if (!v) return false;
  std::vector<double> x;
  if (!numbers(v, x) || x.size() != 1) { err = std::string(flag) + " " + what + ": '" + v + "' is not a number"; return false; }
  to = x[0];
  return true;
}

// The value of the flag at argv[i] as one whole number into `to` (i moves to it), else false with err set ("missing
// value for FLAG", or "FLAG needs a whole number, not 'v'"). Ranges are checked after parsing.
bool whole(int argc, char** argv, int& i, const char* flag, int& to, std::string& err) {
  const char* v = value(argc, argv, i, flag, err);
  if (!v) return false;
  const char* e = v + std::strlen(v);
  if (const auto r = std::from_chars(v, e, to); v == e || r.ec != std::errc() || r.ptr != e) {
    err = std::string(flag) + " needs a whole number, not '" + v + "'";
    return false;
  }
  return true;
}

// s as a JSON string: quote, backslash and control bytes escaped, other bytes as given.
std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (const char ch : s) {
    if (ch == '"' || ch == '\\') o += '\\';
    if (static_cast<unsigned char>(ch) < 0x20) { char u[8]; std::snprintf(u, sizeof u, "\\u%04x", unsigned(ch)); o += u; }
    else o += ch;
  }
  return o + "\"";
}

// The manifest's "meshes" array: {name, first row, row count} per sensor mesh, in input order.
std::string manifest_meshes(const Result& r) {
  std::string m = "[";
  char buf[160];
  size_t first = 0;
  for (size_t i = 0; i < r.ranges.size(); ++i) {
    std::snprintf(buf, sizeof buf, ", \"first\": %zu, \"count\": %zu}", first, r.ranges[i].second);
    m += std::string(i ? ",\n    {\"name\": " : "\n    {\"name\": ") + json_str(r.ranges[i].first) + buf;
    first += r.ranges[i].second;
  }
  return m + "\n  ]";
}

}  // namespace

int parse_common(int argc, char** argv, CommonArgs& a, RunOptions& o, std::string& err,
                 const std::function<int(int& i, const std::string& flag)>& tool) {
  const auto need = [&](int& i, const char* flag) { return value(argc, argv, i, flag, err); };
  const auto pick = [&](int& i, const char* flag, std::initializer_list<const char*> words) { return word(argc, argv, i, flag, words, err); };
  for (int i = 1; i < argc; ++i) {
    std::string f = argv[i];
    const char* v = nullptr;
    int k = 0;
    if (f == "-h" || f == "--help") { a.help = true; return 0; }
    else if (f == "-v") o.verbose = true;
    else if (f == "--context") { if (!(v = need(i, "--context"))) return 1; a.context.push_back(v); }
    else if (f == "--on") { if ((k = pick(i, "--on", {"faces", "vertices"})) < 0) return 1; o.onVertices = k == 1; }
    else if (f == "--sides") { if ((k = pick(i, "--sides", {"front", "back", "both"})) < 0) return 1; o.sides = static_cast<Sides>(k); }
    else if (f == "--cull-backfaces") o.cullBackfaces = true;
    else if (f == "--no-self-occlude") o.selfOcclude = false;
    else if (f == "--no-occlude") {   // NAME[;NAME...]; the names are checked by run()
      if (!(v = need(i, "--no-occlude"))) return 1;
      const std::string s = v;
      for (size_t b = 0, e; b <= s.size(); b = e + 1) {
        e = std::min(s.find(';', b), s.size());
        if (e == b) { err = "--no-occlude " + s + ": empty NAME"; return 1; }
        o.noOcclude.push_back(s.substr(b, e - b));
      }
    }
    else if (f == "--north") { if (!real(argc, argv, i, "--north", "DEG", o.north, err)) return 1; }
    else if (f == "--offset") { if (!real(argc, argv, i, "--offset", "M", o.offset, err)) return 1; }
    else if (f == "--virtual-ground") { if ((k = pick(i, "--virtual-ground", {"on", "off"})) < 0) return 1; o.virtualGround = k == 0; }
    else if (f == "--optics") { if (!(v = need(i, "--optics"))) return 1; if (!parse_optics(v, o.optics, err)) return 1; }
    else if (f == "--transmission") { if ((k = pick(i, "--transmission", {"off", "on"})) < 0) return 1; o.transmission = k == 1; }
    else if (f == "--out") { if (!(v = need(i, "--out"))) return 1; a.out = v; }
    else if (f == "--bench") {
      a.bench = true;
      if (i + 2 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0])) && std::isdigit(static_cast<unsigned char>(argv[i + 2][0]))) {
        a.benchSens = static_cast<size_t>(std::atoll(argv[++i]));
        a.benchTris = static_cast<size_t>(std::atoll(argv[++i]));
      }
    }
    else if (f.size() > 1 && f[0] == '-' && !std::isdigit(static_cast<unsigned char>(f[1]))) {
      const int rc = tool(i, f);
      if (rc < 0) { err = "unknown flag " + f; return 1; }
      if (rc) return rc;
    }
    else a.sensors.push_back(f);
  }
  return 0;
}

int parse(int argc, char** argv, Args& a, std::string& err) {
  const auto need = [&](int& i, const char* flag) { return value(argc, argv, i, flag, err); };
  const auto num = [&](int& i, const char* flag, const char* what, double& to) { return real(argc, argv, i, flag, what, to, err); };
  const auto count = [&](int& i, const char* flag, int& to) { return whole(argc, argv, i, flag, to, err); };
  const int rc = parse_common(argc, argv, a, a.opt, err, [&](int& i, const std::string& f) {
    const char* v = nullptr;
    int k = 0;
    if (f == "--mode") { if ((k = word(argc, argv, i, "--mode", {"physical", "ladybug"}, err)) < 0) return 1; a.opt.mode = static_cast<Mode>(k); }
    else if (f == "--quantity") {
      if (!(v = need(i, "--quantity"))) return 1;
      if (!parse_quantities(v, a.opt.quantities)) { err = "--quantity Q[,Q...]: unknown quantity (radiation|direct|diffuse|irradiance|illuminance|sunhours)"; return 1; }
    }
    else if (f == "--epw") { if (!(v = need(i, "--epw"))) return 1; a.epw = v; }
    else if (f == "--clear-sky") a.clearSky = true;
    else if (f == "--turbidity") { if (!num(i, "--turbidity", "TL", a.turbidity)) return 1; }
    else if (f == "--altitude") { if (!num(i, "--altitude", "M", a.altitude)) return 1; }
    else if (f == "--lat") { if (!num(i, "--lat", "DEG", a.opt.lat)) return 1; }
    else if (f == "--lon") { if (!num(i, "--lon", "DEG", a.opt.lon)) return 1; }
    else if (f == "--tz") { if (!num(i, "--tz", "H", a.opt.tz)) return 1; }
    else if (f == "--time") { if (!(v = need(i, "--time"))) return 1; a.timeStr = v; }
    else if (f == "--from") { if (!(v = need(i, "--from"))) return 1; a.fromStr = v; }
    else if (f == "--to") { if (!(v = need(i, "--to"))) return 1; a.toStr = v; }
    else if (f == "--step") { if (!count(i, "--step", a.opt.stepMin)) return 1; if (a.opt.stepMin <= 0) { err = "--step must be positive minutes"; return 1; } }
    else if (f == "--hours") {
      if (!(v = need(i, "--hours"))) return 1;
      int h0, m0, h1, m1;
      char end;
      if (std::sscanf(v, "%d:%d-%d:%d%c", &h0, &m0, &h1, &m1, &end) != 4 || h0 < 0 || m0 < 0 || m0 > 59 || m1 < 0 || m1 > 59 ||
          h0 * 60 + m0 >= h1 * 60 + m1 || h1 * 60 + m1 > 1440) { err = "--hours HH:MM-HH:MM with start < end <= 24:00"; return 1; }
      a.opt.dayFrom = h0 * 60 + m0;
      a.opt.dayTo = h1 * 60 + m1;
    }
    else if (f == "--sky-density") { if (!count(i, "--sky-density", a.opt.mf)) return 1; }   // values: run()
    else if (f == "--patch-samples") { if (!count(i, "--patch-samples", a.opt.patchSamples)) return 1; }
    else if (f == "--ground") { if (!num(i, "--ground", "RHO", a.opt.ground)) return 1; }
    else if (f == "--fresnel") { if ((k = word(argc, argv, i, "--fresnel", {"on", "off"}, err)) < 0) return 1; a.opt.fresnel = k == 0; }
    else if (f == "--bounces") { if (!count(i, "--bounces", a.opt.bounces)) return 1; }   // ranges: optics_ok below
    else if (f == "--bounce-samples") { if (!count(i, "--bounce-samples", a.opt.bounceSamples)) return 1; }
    else if (f == "--timeseries") { if (!(v = need(i, "--timeseries"))) return 1; a.opt.seriesPath = v; }
    else if (f == "--dump-bits") { if (!(v = need(i, "--dump-bits"))) return 1; a.opt.dumpBitsPath = v; }
    else if (f == "--dump-sky") { if (!(v = need(i, "--dump-sky"))) return 1; a.opt.dumpSky = v; }
    else if (f == "--dump-sun") { if (!(v = need(i, "--dump-sun"))) return 1; a.opt.dumpSun = v; }
    else return -1;
    return 0;
  });
  if (rc || a.help) return rc;
  return optics_ok(a.opt, err) ? 0 : 1;
}

int prepare(Args& a, Weather& weather, bool& haveWeather, std::string& err) {
  if (a.clearSky && !a.epw.empty()) { err = "--clear-sky and --epw are exclusive"; return 1; }
  if (a.clearSky && (std::isnan(a.opt.lat) || std::isnan(a.opt.lon))) { err = "--clear-sky needs --lat and --lon"; return 1; }
  // Without --tz an EPW's own zone applies (run() reads it); only runs without an EPW guess it from the longitude.
  if (!std::isnan(a.opt.lat) && !std::isnan(a.opt.lon) && std::isnan(a.opt.tz) && a.epw.empty() && !haveWeather) {
    a.opt.tz = std::round(a.opt.lon / 15);
    if (a.opt.verbose) std::fprintf(stderr, "info: tz defaulted to %g\n", a.opt.tz);
  }
  if (!a.epw.empty()) {
    if (haveWeather) { err = "--epw cannot be combined with weather supplied by the caller"; return 1; }
    if (!read_epw(a.epw, weather, err)) return 2;
    haveWeather = true;
    if (a.opt.verbose && weather.missing) std::fprintf(stderr, "warn: %d EPW rows with missing radiation treated as 0\n", weather.missing);
  } else if (a.clearSky) {
    if (haveWeather) { err = "--clear-sky cannot be combined with weather supplied by the caller"; return 1; }
    if (!clear_sky_weather(a.opt.lat, a.opt.lon, a.opt.tz, a.turbidity, a.altitude, weather, err)) return 1;
    haveWeather = true;
  }
  const bool leap = haveWeather && weather.leap;
  if (!a.timeStr.empty()) {
    if (!a.fromStr.empty() || !a.toStr.empty()) { err = "--time cannot be combined with --from/--to"; return 1; }
    if (!parse_hoy(a.timeStr, leap, a.opt.t0, err)) return 1;
  } else if (!a.fromStr.empty()) {
    if (a.toStr.empty()) { err = "--from needs --to"; return 1; }
    if (!parse_hoy(a.fromStr, leap, a.opt.t0, err) || !parse_hoy(a.toStr, leap, a.opt.t1, err)) return 1;
  } else if (!a.toStr.empty()) { err = "--to needs --from"; return 1; }
  if ((a.opt.dayFrom > 0 || a.opt.dayTo < 1440) && a.fromStr.empty()) { err = "--hours needs --from/--to"; return 1; }
  return 0;
}

std::string manifest_json(const Result& r, const std::string& extraFields) {
  std::string m = "{\n  \"columns\": [";
  for (size_t c = 0; c < r.columns.size(); ++c) m += (c ? ", \"" : "\"") + r.columns[c] + "\"";
  char buf[160];
  std::snprintf(buf, sizeof buf, "],\n  \"duration_h\": %.6f,\n  \"dirs\": {\"sky\": %zu, \"ground\": %zu, \"sun\": %zu},\n  \"meshes\": ",
                r.durationHours, r.nSky, r.nSky, r.nSun);
  m += buf + manifest_meshes(r);
  if (!extraFields.empty()) m += ",\n  " + extraFields;
  m += "\n}\n";
  return m;
}

}  // namespace daylight::cli

namespace daylight::views::cli {

const char* const kUsage =
    "usage: views <mesh>... [flags]\n"
    "View analysis: from every face (or vertex) of the meshes, rays over a field of view. Per row and side the shares\n"
    "of the field on sky, ground, obstruction and each target group, obstruction_total (1 - sky), how far it sees.\n"
    "\n"
    "meshes\n"
    "  <mesh>...                analysed (receiver) meshes, which also block rays: .obj (one o/g group per mesh), .dmesh\n"
    "                           (raw arrays, layout in daylight.h) or .usd/.usda/.usdc/.usdz (-DDAYLIGHT_USD=ON builds;\n"
    "                           instances and point instancers expanded); world metres, Z-up\n"
    "  --context FILE           a mesh that only blocks rays, never analysed (repeatable)\n"
    "  --target NAME=GROUP[;NAME=GROUP...]  rays ending on mesh NAME count for GROUP, a share column per group\n"
    "                           (repeatable; at most 16 groups)\n"
    "  --on faces|vertices      a row per face (eye at its centroid) or per vertex (default faces)\n"
    "  --sides front|back|both  viewing side(s) of each element; both: two columns per value (default front)\n"
    "  --offset M               eyes moved M metres along the normal (default 0; 1.6 above a floor: a standing eye)\n"
    "  --cull-backfaces         occluders block only from their front side (default both sides)\n"
    "  --no-self-occlude        receivers block no rays (only --context meshes do)\n"
    "  --no-occlude NAME[;NAME...]  these receivers are analysed but block no rays (repeatable)\n"
    "  --north DEG              model north, degrees counterclockwise from +Y (default 0): azimuth 0 of --fov 360\n"
    "\n"
    "field of view\n"
    "  --fov H[,LO,HI]          H degrees wide about the normal (up to 180) or 360 all around, elevation LO..HI\n"
    "                           (default 120,-70,50; H alone keeps LO,HI; H,0,0: horizontal rays only, the 2D isovist)\n"
    "  --gaze normal|level      H < 360: the field about the normal, or about the normal turned level with elevation\n"
    "                           about the horizon, as a person looks (default normal; faces looking straight up or down\n"
    "                           keep their normal)\n"
    "  --cell DEG               degrees per ray, rows and columns (default 2; at most 2^20 rays per element)\n"
    "  --jitter on|off          each ray at a random point of its cell (default on; off: the cell centres)\n"
    "\n"
    "distances\n"
    "  --distance STAT[,STAT...]  isovist|mean|min|group|none (default: isovist with --fov 360, else mean,min)\n"
    "                           isovist: volume (m3) of the mesh through the rays' ends, area (m2) with H,0,0;\n"
    "                           mean: log-mean distance; min: nearest; group: also per target group\n"
    "  --far M                  what open counts as, and the cap of every distance and isovist (default 1000)\n"
    "  --reach D[,D...]|en17037 share of the field that sees at least D metres (at most 8; en17037 = 6,20,50)\n"
    "\n"
    "ground and glass\n"
    "  --virtual-ground on|off  a ground plane ends downward rays (default on; off: they stay open)\n"
    "  --ground-z Z             its height in metres (default: the lowest point of the meshes)\n"
    "  --optics SPEC[;SPEC...]  NAME[/SLOT]=RHO[,TAU[,glass|porous]] per mesh or face slot, as daylight's (repeatable)\n"
    "  --transmission off|on    rays pass glass slots (default off: every surface blocks); adds glazing_<side>\n"
    "\n"
    "output\n"
    "  --out FILE[.npy]         values as text rows, or float64 [N x C] NumPy when .npy, and the FILE.json manifest\n"
    "                           (default: text on stdout, no manifest)\n"
    "  --dump-rays F.npy        classes uint8 [rows x rays] (a row per element and side; + 128: passed glass) and\n"
    "                           F.npy.depth.npy, float32 distances\n"
    "  -v                       sizes and timings on stderr\n"
    "  --bench [NSENS NTRIS]    a synthetic city (default 1000000 sensors, 10000000 triangles): timings only\n"
    "  -h, --help               this text\n"
    "\n"
    "examples\n"
    "  views facades.obj --context city.obj --reach en17037 --out facades.npy\n"
    "  views facades.obj --context city.obj --target park=green --gaze level --out facades.npy\n"
    "  views plaza.obj --context city.obj --fov 360 --offset 1.6 --distance isovist,mean --out plaza.txt\n"
    "\n"
    "exit codes: 0 ok, 1 usage error (the message and this text on stderr), 2 runtime error\n";

namespace {

using daylight::cli::numbers;

// NAME=GROUP[;NAME=GROUP...], split at the last '='. The names and groups are checked by run().
bool parse_targets(const std::string& s, std::vector<Target>& out, std::string& err) {
  for (size_t b = 0, e; b <= s.size(); b = e + 1) {
    e = std::min(s.find(';', b), s.size());
    const std::string spec = s.substr(b, e - b);
    const size_t eq = spec.rfind('=');
    if (spec.empty()) { err = "--target " + s + ": empty NAME=GROUP (a stray ;)"; return false; }
    if (eq == std::string::npos || eq == 0 || eq + 1 == spec.size()) { err = "--target " + spec + ": NAME=GROUP"; return false; }
    out.push_back({spec.substr(0, eq), spec.substr(eq + 1)});
  }
  return true;
}

}  // namespace

int parse(int argc, char** argv, Args& a, std::string& err) {
  Options& o = a.opt;
  const auto need = [&](int& i, const char* flag) { return daylight::cli::value(argc, argv, i, flag, err); };
  const auto num = [&](int& i, const char* flag, const char* what, double& to) { return daylight::cli::real(argc, argv, i, flag, what, to, err); };
  const auto pick = [&](int& i, const char* flag, std::initializer_list<const char*> words) { return daylight::cli::word(argc, argv, i, flag, words, err); };
  std::vector<double> x;
  const int rc = daylight::cli::parse_common(argc, argv, a, o, err, [&](int& i, const std::string& f) {
    const char* v = nullptr;
    int k = 0;
    if (f == "--target") { if (!(v = need(i, "--target")) || !parse_targets(v, o.targets, err)) return 1; }
    else if (f == "--fov") {
      if (!(v = need(i, "--fov"))) return 1;
      if (!numbers(v, x) || (x.size() != 1 && x.size() != 3)) { err = "--fov H[,LO,HI] in degrees"; return 1; }
      o.fovH = x[0];
      if (x.size() == 3) { o.elevLo = x[1]; o.elevHi = x[2]; }
    }
    else if (f == "--gaze") { if ((k = pick(i, "--gaze", {"normal", "level"})) < 0) return 1; o.level = k == 1; }
    else if (f == "--cell") { if (!num(i, "--cell", "DEG", o.cell)) return 1; }
    else if (f == "--jitter") { if ((k = pick(i, "--jitter", {"on", "off"})) < 0) return 1; o.jitter = k == 0; }
    else if (f == "--distance") {
      if (!(v = need(i, "--distance"))) return 1;
      const std::string s = v;
      o.distance = 0;
      for (size_t b = 0, e; s != "none" && b <= s.size(); b = e + 1) {
        e = std::min(s.find(',', b), s.size());
        const std::string t = s.substr(b, e - b);
        if (t == "isovist") o.distance |= DistIsovist;
        else if (t == "mean") o.distance |= DistMean;
        else if (t == "min") o.distance |= DistMin;
        else if (t == "group") o.distance |= DistGroup;
        else { err = "--distance isovist|mean|min|group[,...] or none"; return 1; }
      }
    }
    else if (f == "--far") { if (!num(i, "--far", "M", o.farDist)) return 1; }
    else if (f == "--ground-z") { if (!num(i, "--ground-z", "Z", o.groundZ)) return 1; }
    else if (f == "--reach") {
      if (!(v = need(i, "--reach"))) return 1;
      if (!std::strcmp(v, "en17037")) o.reach = {6, 20, 50};   // EN 17037's view distance levels: minimum, medium, high
      else if (!numbers(v, o.reach)) { err = "--reach D[,D...] in metres, or en17037"; return 1; }
    }
    else if (f == "--dump-rays") { if (!(v = need(i, "--dump-rays"))) return 1; o.dumpPath = v; }
    else return -1;
    return 0;
  });
  if (rc || a.help) return rc;
  Field fd;
  return field({}, o, fd, err) ? 0 : 1;
}

std::string manifest_json(const Result& r, const Options& opt, const Field& f) {
  const auto num = [](double v) { char s[32]; std::snprintf(s, sizeof s, "%.10g", v); return std::string(std::isfinite(v) ? s : "null"); };
  std::string m = "{\n  \"columns\": [";
  for (size_t c = 0; c < r.columns.size(); ++c) m += (c ? ", \"" : "\"") + r.columns[c] + "\"";
  m += "],\n  \"view\": {\"fov\": [" + num(opt.fovH) + ", " + num(opt.elevLo) + ", " + num(opt.elevHi) + "], " +
       (opt.level && opt.fovH < 360 ? "\"gaze\": \"level\", " : "") + "\"cell\": " + num(opt.cell) +
       ", \"grid\": [" + std::to_string(f.nEl) + ", " + std::to_string(f.nAz) + "], \"jitter\": " + (opt.jitter ? "true" : "false") +
       ", \"north\": " + num(opt.north) + ", \"offset\": " + num(opt.offset) + ", \"far\": " + num(opt.farDist) + ", \"near\": 0.1, \"virtual_ground\": " +
       (opt.virtualGround ? "true" : "false") + ", \"ground_z\": " + num(f.groundZ) + (opt.transmission ? ", \"transmission\": true" : "") +
       ", \"reach\": [";
  for (size_t k = 0; k < opt.reach.size(); ++k) m += (k ? ", " : "") + num(opt.reach[k]);
  m += "], \"classes\": [\"sky\", \"ground\", \"obstruction\"";
  std::vector<std::string> groups;   // first appearance, as run() orders them
  for (const Target& t : opt.targets)
    if (std::find(groups.begin(), groups.end(), t.group) == groups.end()) { groups.push_back(t.group); m += ", \"" + t.group + "\""; }
  m += "],\n    \"image\": \"rows from HI down, columns clockwise from the field's left edge (360: from north)\"},\n  \"targets\": [";
  for (size_t k = 0; k < opt.targets.size(); ++k)
    m += std::string(k ? ", " : "") + "{\"mesh\": " + daylight::cli::json_str(opt.targets[k].mesh) + ", \"group\": \"" + opt.targets[k].group + "\"}";
  m += "],\n  \"meshes\": " + daylight::cli::manifest_meshes(r) + "\n}\n";
  return m;
}

}  // namespace daylight::views::cli
