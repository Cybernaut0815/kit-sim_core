/* SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0 */
/* ctest c_abi: the C ABI (src/daylight_c.h) from plain C through daylight_c.dll. Argument and flag errors, both tools on
   tests/box.obj against the CLI's own outputs (cli_smoke_npy, views_smoke_npy: values and manifests byte for byte),
   resident scenes over handles, the views facade, cancel. argv: the tests folder, the build folder (smoke.npy, ...). */
#include "daylight_c.h"

#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(c)                                                                                  \
  do {                                                                                            \
    if (!(c)) {                                                                                   \
      fprintf(stderr, "FAIL line %d: %s (last error: %s)\n", __LINE__, #c, dl_last_error());      \
      ++failures;                                                                                 \
    }                                                                                             \
  } while (0)

static int has(const char* s, const char* sub) { return s && strstr(s, sub) != NULL; }

static const char* join(char* buf, size_t size, const char* dir, const char* name) {
  snprintf(buf, size, "%s/%s", dir, name);
  return buf;
}

/* The whole file (text mode turns the CLI's CRLF manifest back into LF), NUL-terminated; NULL when missing. */
static char* slurp(const char* path, int text, size_t* n) {
  FILE* f = fopen(path, text ? "r" : "rb");
  if (!f) return NULL;
  size_t cap = 1 << 16, len = 0, got;
  char* b = (char*)malloc(cap + 1);
  while (b && (got = fread(b + len, 1, cap - len, f)) > 0) {
    len += got;
    if (len == cap) {
      char* nb = (char*)realloc(b, 2 * cap + 1);
      if (!nb) { free(b); b = NULL; break; }
      b = nb;
      cap *= 2;
    }
  }
  fclose(f);
  if (b) { b[len] = 0; if (n) *n = len; }
  return b;
}

/* The data of an .npy v1.0 image (after its header). */
static const unsigned char* npy_data(const char* npy, size_t n, size_t* bytes) {
  if (!npy || n < 10) return NULL;
  const size_t off = 10 + ((size_t)(unsigned char)npy[8] | ((size_t)(unsigned char)npy[9] << 8));
  if (off > n) return NULL;
  *bytes = n - off;
  return (const unsigned char*)npy + off;
}

static int32_t run1(int32_t tool, const char* const* argv, int32_t argc, dl_mesh* sensor, dl_control* control, dl_result** out) {
  return dl_run(tool, argc, argv, sensor ? &sensor : NULL, sensor ? 1 : 0, NULL, 0, control, out);
}

static void expect_fail(int32_t tool, const char* const* argv, int32_t argc, dl_mesh* sensor, int32_t want, const char* msg, int line) {
  dl_result* r = NULL;
  const int32_t rc = run1(tool, argv, argc, sensor, NULL, &r);
  if (rc != want || r != NULL || !has(dl_last_error(), msg)) {
    fprintf(stderr, "FAIL line %d: status %d (want %d), message '%s' (want '%s')\n", line, rc, want, dl_last_error(), msg);
    ++failures;
    dl_result_free(r);
  }
}
#define EXPECT_FAIL(tool, want, msg, sensor, ...)                                                       \
  do {                                                                                                  \
    const char* const a_[] = {__VA_ARGS__};                                                             \
    expect_fail(tool, a_, (int32_t)(sizeof a_ / sizeof *a_), sensor, want, msg, __LINE__);              \
  } while (0)

/* The result's values (rows x cols, malloc'd); NULL on failure. */
static double* values_of(const dl_result* r, size_t* count) {
  const int64_t n = dl_result_rows(r) * dl_result_cols(r);
  double* v = n > 0 ? (double*)malloc((size_t)n * sizeof(double)) : NULL;
  if (v && dl_result_values(r, v, n) != DL_OK) { free(v); v = NULL; }
  *count = v ? (size_t)n : 0;
  return v;
}

/* The result equals the CLI's NAME.npy and NAME.npy.json in the build folder, byte for byte. */
static void same_as_cli(const dl_result* r, const char* build, const char* name, int line) {
  char p[1024], q[1024];
  size_t nNpy = 0, nData = 0, n = 0;
  char* npy = slurp(join(p, sizeof p, build, name), 0, &nNpy);
  snprintf(q, sizeof q, "%s.json", p);
  char* man = slurp(q, 1, NULL);
  const unsigned char* data = npy_data(npy, nNpy, &nData);
  double* v = values_of(r, &n);
  if (!data || !man || !v || nData != n * sizeof(double) || memcmp(v, data, nData) || strcmp(dl_result_manifest(r), man)) {
    fprintf(stderr, "FAIL line %d: the in-process result differs from %s (or it is missing)\n", line, p);
    ++failures;
  }
  free(npy);
  free(man);
  free(v);
}

static double stat0(const dl_result* r) {
  double st[DL_STATS] = {0};
  return r && dl_result_stats(r, st) == DL_OK ? st[0] : -1;
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: c_abi <tests folder> <build folder>\n"); return 2; }
  const char* tests = argv[1];
  const char* build = argv[2];
  char p[1024];

  /* A. version and usage */
  CHECK(dl_version() == DL_ABI_VERSION);
  CHECK(has(dl_usage(DL_DAYLIGHT), "usage: daylight"));
  CHECK(has(dl_usage(DL_VIEWS), "usage: views"));
  CHECK(dl_usage(7) == NULL);

  /* B. errors before the GPU */
  const double fxyz[] = {-1, 0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 2};   /* a 2 x 2 m facade facing -y, its foot at z 0 */
  const int32_t quad[] = {4}, fidx[] = {0, 1, 2, 3}, two[] = {2}, bad[] = {0, 1, 2, 4};
  dl_mesh* facade = dl_mesh_create("facade", fxyz, 4, quad, 1, fidx, 4, NULL);
  CHECK(facade && dl_mesh_count(facade) == 1 && strcmp(dl_mesh_name(facade, 0), "facade") == 0);
  CHECK(dl_mesh_name(facade, 1) == NULL && dl_mesh_count(NULL) == -1);
  CHECK(dl_mesh_create("bad", fxyz, 4, quad, 1, bad, 4, NULL) == NULL && has(dl_last_error(), "bad: vertex index out of range"));
  CHECK(dl_mesh_create("two", fxyz, 4, two, 1, fidx, 2, NULL) == NULL && has(dl_last_error(), "fewer than 3 vertices"));
  CHECK(dl_mesh_create(NULL, fxyz, 4, quad, 1, fidx, 4, NULL) == NULL && has(dl_last_error(), "name is NULL"));
  dl_mesh* empty = dl_mesh_create("empty", NULL, 0, NULL, 0, NULL, 0, NULL);
  CHECK(empty && dl_mesh_count(empty) == 0);
  dl_mesh_free(empty);
  CHECK(dl_mesh_load(join(p, sizeof p, tests, "nosuch.obj")) == NULL && has(dl_last_error(), "cannot open"));
  CHECK(dl_mesh_load("x.usd") == NULL && has(dl_last_error(), "unknown extension"));
  dl_mesh_free(NULL);
  dl_control_free(NULL);
  dl_result_free(NULL);
  {
    dl_result* r = NULL;
    dl_mesh* none = NULL;
    CHECK(dl_run(7, 0, NULL, &facade, 1, NULL, 0, NULL, &r) == DL_USAGE && r == NULL && has(dl_last_error(), "unknown tool"));
    CHECK(dl_run(DL_VIEWS, 0, NULL, &none, 1, NULL, 0, NULL, &r) == DL_USAGE && has(dl_last_error(), "NULL mesh handle"));
    CHECK(dl_run(DL_VIEWS, 0, NULL, NULL, 0, NULL, 0, NULL, &r) == DL_USAGE && has(dl_last_error(), "no mesh given"));
    CHECK(dl_run(DL_VIEWS, 0, NULL, &facade, 1, NULL, 0, NULL, NULL) == DL_USAGE && has(dl_last_error(), "out is NULL"));
  }
  EXPECT_FAIL(DL_DAYLIGHT, DL_USAGE, "unknown flag --bogus", facade, "--bogus");
  EXPECT_FAIL(DL_VIEWS, DL_USAGE, "dl_mesh_load", facade, "x.obj");
  EXPECT_FAIL(DL_VIEWS, DL_USAGE, "--context", facade, "--context", "c.obj");
  EXPECT_FAIL(DL_DAYLIGHT, DL_USAGE, "--out", facade, "--out", "v.npy");
  EXPECT_FAIL(DL_VIEWS, DL_USAGE, "--bench", facade, "--bench");
  EXPECT_FAIL(DL_DAYLIGHT, DL_USAGE, "dl_usage", facade, "-h");
  EXPECT_FAIL(DL_DAYLIGHT, DL_USAGE, "nosuch", facade, "--optics", "nosuch=0.5");
  EXPECT_FAIL(DL_VIEWS, DL_USAGE, "nosuch", facade, "--target", "nosuch=g");
  EXPECT_FAIL(DL_DAYLIGHT, DL_RUNTIME, "nosuch.epw", facade, "--epw", join(p, sizeof p, tests, "nosuch.epw"));

  /* C. the GPU: tests/box.obj (a ground of 4 quads, a floating box) as the CLI's smokes run it */
  dl_mesh* box = dl_mesh_load(join(p, sizeof p, tests, "box.obj"));
  CHECK(box && dl_mesh_count(box) >= 1);
  const char* const sun[] = {"--lat", "41.98", "--lon", "-87.92", "--tz", "-6", "--quantity", "sunhours", "--time", "06-21T12:00"};
  dl_control* ctl = dl_control_create();
  dl_result* r1 = NULL;
  const int32_t rc = run1(DL_DAYLIGHT, sun, 10, box, ctl, &r1);
  if (rc != DL_OK && (has(dl_last_error(), "CUDA") || has(dl_last_error(), "OptiX") || has(dl_last_error(), "optix"))) {
    printf("skip: no usable GPU (%s)\n", dl_last_error());
    return failures ? 1 : 0;
  }
  CHECK(rc == DL_OK && r1 != NULL);
  if (!r1) return 1;
  same_as_cli(r1, build, "smoke.npy", __LINE__);
  size_t n1 = 0;
  double* v1 = values_of(r1, &n1);
  CHECK(v1 && n1 > 0 && dl_result_values(r1, v1, (int64_t)n1 - 1) == DL_USAGE);
  {
    float* f = (float*)malloc(n1 * sizeof(float));
    CHECK(f && dl_result_values_f32(r1, f, (int64_t)n1) == DL_OK);
    for (size_t i = 0; f && v1 && i < n1; ++i) CHECK(f[i] == (float)v1[i]);
    free(f);
  }
  CHECK(stat0(r1) == 0 && dl_control_progress(ctl) == 1.0);

  /* the same handle again: the resident scene serves it, with the same values */
  dl_result* r2 = NULL;
  CHECK(run1(DL_DAYLIGHT, sun, 10, box, NULL, &r2) == DL_OK && stat0(r2) == 1);
  size_t n2 = 0;
  double* v2 = r2 ? values_of(r2, &n2) : NULL;
  CHECK(v1 && v2 && n2 == n1 && memcmp(v1, v2, n1 * sizeof(double)) == 0);

  /* a host's decimal-comma C locale changes nothing (dl_run reads and writes numbers in "C"), and stays the host's */
  if (setlocale(LC_ALL, "de-DE")) {
    dl_result* rl = NULL;
    CHECK(run1(DL_DAYLIGHT, sun, 10, box, NULL, &rl) == DL_OK);
    if (rl) same_as_cli(rl, build, "smoke.npy", __LINE__);
    CHECK(localeconv()->decimal_point[0] == ',');
    dl_result_free(rl);
    setlocale(LC_ALL, "C");
  } else {
    printf("info: no de-DE locale here, the locale case skipped\n");
  }

  /* progress ends at 1 also for a run with nothing to trace (sun hours at night) */
  {
    const char* const night[] = {"--lat", "41.98", "--lon", "-87.92", "--tz", "-6", "--quantity", "sunhours", "--time", "06-21T00:00"};
    dl_control* c = dl_control_create();
    dl_result* rn = NULL;
    CHECK(run1(DL_DAYLIGHT, night, 10, box, c, &rn) == DL_OK && dl_control_progress(c) == 1.0);
    dl_result_free(rn);
    dl_control_free(c);
  }

  /* views on the same handle: an opaque scene is views' too; its values and manifest are views_smoke_npy's */
  const char* const view360[] = {"--fov", "360", "--offset", "1.6", "--sides", "both"};
  dl_result* r3 = NULL;
  CHECK(run1(DL_VIEWS, view360, 6, box, NULL, &r3) == DL_OK && stat0(r3) == 1);
  if (r3) same_as_cli(r3, build, "views_smoke.npy", __LINE__);

  /* a freed handle's scene is never reused, even when the new handle lands at the same address */
  const uintptr_t old = (uintptr_t)box;
  dl_mesh_free(box);
  dl_mesh* box2 = dl_mesh_load(join(p, sizeof p, tests, "box.obj"));
  printf("info: the reloaded handle is at the %s address\n", (uintptr_t)box2 == old ? "same" : "another");
  dl_result* r4 = NULL;
  CHECK(run1(DL_DAYLIGHT, sun, 10, box2, NULL, &r4) == DL_OK && stat0(r4) == 0);
  size_t n4 = 0;
  double* v4 = r4 ? values_of(r4, &n4) : NULL;
  CHECK(v1 && v4 && n4 == n1 && memcmp(v1, v4, n1 * sizeof(double)) == 0);

  /* the views facade (check_views case 1): sky above the field's horizon, the virtual ground below, no obstruction */
  const char* const open[] = {"--no-self-occlude", "--jitter", "off"};
  dl_result* r5 = NULL;
  CHECK(run1(DL_VIEWS, open, 3, facade, NULL, &r5) == DL_OK && dl_result_rows(r5) == 1 && dl_result_cols(r5) == 5);
  size_t n5 = 0;
  double* v5 = r5 ? values_of(r5, &n5) : NULL;
  CHECK(v5 && n5 == 5 && fabs(v5[0] - 0.4490987851112869) < 1e-9 && fabs(v5[1] - 0.5509012148887131) < 1e-9 && v5[2] == 0);
  CHECK(r5 && has(dl_result_manifest(r5), "\"columns\": [\"sky_front\", \"ground_front\", \"obstruction_front\""));

  /* an interior through the DLL: the measure grid ignored by the rays, the window glass passed (glazing_front) */
  {
    dl_mesh* grid = dl_mesh_load(join(p, sizeof p, tests, "room_grid.obj"));
    dl_mesh* room = dl_mesh_load(join(p, sizeof p, tests, "room.obj"));
    const char* const in[] = {"--fov", "360", "--offset", "0.4", "--no-occlude", "grid", "--optics", "window=0,0.64,glass",
                              "--transmission", "on", "--cell", "6"};
    dl_result* ri = NULL;
    CHECK(grid && room && dl_run(DL_VIEWS, 12, in, &grid, 1, &room, 1, NULL, &ri) == DL_OK && dl_result_rows(ri) == 12);
    CHECK(ri && has(dl_result_manifest(ri), "\"glazing_front\"") && has(dl_result_manifest(ri), "\"transmission\": true"));
    const char* const unknown[] = {"--optics", "nosuch=0,0.5,glass", "--transmission", "on"};
    dl_result* rx = NULL;
    CHECK(dl_run(DL_VIEWS, 4, unknown, &grid, 1, &room, 1, NULL, &rx) == DL_USAGE && rx == NULL && has(dl_last_error(), "nosuch"));
    dl_result_free(ri);
    dl_mesh_free(grid);
    dl_mesh_free(room);
  }

  /* cancel: a cancelled control fails the run as it starts; the next run works */
  dl_control* stop = dl_control_create();
  dl_control_cancel(stop);
  dl_result* r6 = NULL;
  CHECK(run1(DL_VIEWS, open, 3, facade, stop, &r6) == DL_RUNTIME && r6 == NULL && has(dl_last_error(), "cancelled"));
  dl_control_free(stop);
  dl_result* r7 = NULL;
  CHECK(run1(DL_VIEWS, open, 3, facade, NULL, &r7) == DL_OK);

  free(v1); free(v2); free(v4); free(v5);
  dl_result_free(r1); dl_result_free(r2); dl_result_free(r3); dl_result_free(r4); dl_result_free(r5); dl_result_free(r7);
  dl_control_free(ctl);
  dl_mesh_free(box2);
  dl_mesh_free(facade);
  if (failures) printf("c_abi: %d failure(s)\n", failures);
  else printf("c_abi: OK\n");
  return failures ? 1 : 0;
}
