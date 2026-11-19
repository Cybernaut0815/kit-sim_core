// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0
// CLI contract shared by main.cpp and other front ends (an HTTP server): flag parsing, the weather / time rules that
// follow parsing, and the output manifest, of daylight and of views. Return codes mirror the exit codes: 0 ok, 1 usage
// error, 2 runtime error.
#pragma once
#include "daylight.h"
#include "views.h"
#include <functional>
#include <string>
#include <vector>

namespace daylight::cli {

extern const char* const kUsage;

// What every CLI of the library parses alike (parse_common): mesh files, the output, --bench, -h.
struct CommonArgs {
  std::vector<std::string> sensors, context;   // positional mesh files / --context files
  std::string out;
  bool bench = false, help = false;
  size_t benchSens = 1000000, benchTris = 10000000;
};

struct Args : CommonArgs {
  Options opt;
  std::string epw, timeStr, fromStr, toStr;
  bool clearSky = false;
  double turbidity = 3, altitude = 0;
};

// argv[1..argc): the shared flags (-v --context --on --sides --cull-backfaces --no-self-occlude --no-occlude --offset
// --north --virtual-ground --optics --transmission --out --bench [NSENS NTRIS]) into a and o, -h/--help sets a.help and
// stops, other arguments that do not start with '-' + a non-digit into a.sensors. Every other flag goes to tool(i, flag):
// 0 taken (i moved past its value), 1 usage error (err set), -1 not its flag ("unknown flag"). 0 ok, 1 usage error with
// err set.
int parse_common(int argc, char** argv, CommonArgs& a, RunOptions& o, std::string& err,
                 const std::function<int(int& i, const std::string& flag)>& tool);
// argv[1..argc) -> a. 0 ok (-h/--help sets a.help and stops), 1 usage error with err set (the caller prints kUsage),
// also for --optics rows, --bounces or --bounce-samples out of range, switches the mode refuses and an empty
// --no-occlude NAME. Mesh names in --optics and --no-occlude are checked by run().
int parse(int argc, char** argv, Args& a, std::string& err);
// The rules that follow parsing: --clear-sky xor --epw; --clear-sky needs --lat --lon; tz defaults to round(lon / 15)
// when --lat --lon come without --tz; weather from a.epw or the clear-sky model unless haveWeather is already true on
// entry (a caller that parsed the EPW itself); --time / --from / --to parsed with the weather's leap flag into a.opt.
// 0 ok, 1 usage error, 2 runtime error (err set).
int prepare(Args& a, Weather& w, bool& haveWeather, std::string& err);
// The FILE.json manifest exactly as the CLI writes it; extraFields (e.g. "\"run_s\": 1.2") go before the closing brace.
std::string manifest_json(const Result& r, const std::string& extraFields = "");

}  // namespace daylight::cli

namespace daylight::views::cli {

extern const char* const kUsage;

struct Args : daylight::cli::CommonArgs {
  Options opt;
};

// argv[1..argc) -> a: parse_common's flags and the views flags. 0 ok (-h/--help sets a.help and stops), 1 usage error with
// err set (the caller prints kUsage), also for options field() refuses. Target and --no-occlude names are checked by run().
int parse(int argc, char** argv, Args& a, std::string& err);
// The FILE.json manifest: columns, the field ("view": f from field() over the run's meshes), the targets, the meshes.
std::string manifest_json(const Result& r, const Options& opt, const Field& f);

}  // namespace daylight::views::cli
