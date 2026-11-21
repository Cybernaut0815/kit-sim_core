/* SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0 */
/* daylight and views in-process from plain C through daylight_c.dll (docs/c-abi.md): tests/box.obj and a facade from
   arrays, a clear-sky year, a view target, rows per mesh from the manifest, the resident scene, progress, cancel,
   errors. Run from the repo root: build/RelWithDebInfo/example_c.exe tests */
#include "daylight_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per sensor mesh the mean of every column. The manifest names the columns and gives each mesh's rows in its fixed
   layout: "columns": [...], then {"name": "...", "first": F, "count": N} per mesh (a JSON parser reads it too). */
static void per_mesh(const dl_result* r) {
  const int64_t rows = dl_result_rows(r), cols = dl_result_cols(r);
  const char* m = dl_result_manifest(r);
  const char* c0 = strchr(m, '[');
  double* v = malloc((size_t)(rows * cols) * sizeof *v);
  if (!v || dl_result_values(r, v, rows * cols) != DL_OK) { free(v); return; }
  printf("  columns %.*s\n", (int)(strchr(c0, ']') - c0 - 1), c0 + 1);
  for (const char* p = strstr(m, "\"meshes\""); (p = strstr(p + 1, "{\"name\": ")) != NULL;) {
    char name[64];
    long long first, count;
    if (sscanf(p, "{\"name\": \"%63[^\"]\", \"first\": %lld, \"count\": %lld", name, &first, &count) != 3) break;
    printf("  %-6s %2lld faces:", name, count);
    for (int64_t c = 0; c < cols; ++c) {
      double sum = 0;
      for (long long e = first; e < first + count; ++e) sum += v[e * cols + c];   /* row-major rows x cols */
      printf(" %9.4g", sum / (double)count);
    }
    printf("\n");
  }
  free(v);
}

static int fail(const char* what) {
  fprintf(stderr, "%s: %s\n", what, dl_last_error());
  return 2;   /* the handles made so far go with the process */
}

int main(int argc, char** argv) {
  char box[1024];
  snprintf(box, sizeof box, "%s/box.obj", argc > 1 ? argv[1] : "tests");   /* OBJ groups: ground, box */
  /* a 4 x 3 m wall at y = 3 facing south (-y), two 1.5 m rows: xyz in metres (Z up), vertices per face, 0-based indices
     counter-clockwise seen from the front */
  const double xyz[] = {-2, 3, 0, 2, 3, 0, 2, 3, 1.5, -2, 3, 1.5, 2, 3, 3, -2, 3, 3};
  const int32_t counts[] = {4, 4}, indices[] = {0, 1, 2, 3, 3, 2, 4, 5};
  dl_mesh* sensors[2] = {dl_mesh_load(box), dl_mesh_create("facade", xyz, 6, counts, 2, indices, 8, NULL)};
  if (!sensors[0] || !sensors[1]) return fail("mesh");

  /* daylight: a clear-sky year in Berlin; another thread may read the control's progress or cancel the run */
  const char* const sun[] = {"--clear-sky", "--lat", "52.52", "--lon", "13.41", "--quantity", "radiation,sunhours"};
  dl_control* control = dl_control_create();
  dl_result *r = NULL, *v = NULL;
  double stats[DL_STATS];
  if (dl_run(DL_DAYLIGHT, 7, sun, sensors, 2, NULL, 0, control, &r) != DL_OK) return fail("daylight");
  dl_result_stats(r, stats);
  printf("daylight: %lld rows, scene reused %g, progress %g\n", (long long)dl_result_rows(r), stats[0],
         dl_control_progress(control));
  per_mesh(r);

  /* views: 120 deg about each normal; rays ending on mesh "box" count for group "cube". The same handles in the same
     roles: the resident scene serves it (stats[0] = 1) */
  const char* const view[] = {"--target", "box=cube"};
  if (dl_run(DL_VIEWS, 2, view, sensors, 2, NULL, 0, NULL, &v) != DL_OK) return fail("views");
  dl_result_stats(v, stats);
  printf("views: %lld rows, scene reused %g\n", (long long)dl_result_rows(v), stats[0]);
  per_mesh(v);

  /* isovists in tests/room.obj (a 6.3 x 5.1 x 2.9 m room) from its measure grid, the eye 0.4 m above it: the volume
     all around (--fov 360,-90,90: column volume, m3) and the area of horizontal rays only (--fov 360,0,0: area, m2). The
     isovist is 360 mode's default column; --distance mean,min gives the distances instead, --far M caps both */
  const char* dir = argc > 1 ? argv[1] : "tests";
  char path[1024];
  snprintf(path, sizeof path, "%s/room_grid.obj", dir);
  dl_mesh* grid = dl_mesh_load(path);
  snprintf(path, sizeof path, "%s/room.obj", dir);
  dl_mesh* room = dl_mesh_load(path);   /* room, window, tree: context */
  if (!grid || !room) return fail("room");
  const char* const vol[] = {"--fov", "360,-90,90", "--offset", "0.4", "--no-occlude", "grid"};
  const char* const area[] = {"--fov", "360,0,0", "--offset", "0.4", "--no-occlude", "grid"};
  dl_result *iv = NULL, *ia = NULL;
  if (dl_run(DL_VIEWS, 6, vol, &grid, 1, &room, 1, NULL, &iv) != DL_OK) return fail("isovist volume");
  if (dl_run(DL_VIEWS, 6, area, &grid, 1, &room, 1, NULL, &ia) != DL_OK) return fail("isovist area");
  printf("isovists in the room, all around and horizontal:\n");
  per_mesh(iv);
  per_mesh(ia);
  dl_result_free(iv);
  dl_result_free(ia);
  dl_mesh_free(grid);
  dl_mesh_free(room);

  /* failures: DL_USAGE 1 (flags, names the meshes do not resolve) or DL_RUNTIME 2 (files, GPU, "cancelled") */
  dl_result* none = NULL;
  dl_control* stop = dl_control_create();
  dl_control_cancel(stop);   /* from another thread while it runs; here before it starts */
  int32_t rc = dl_run(DL_VIEWS, 2, view, sensors, 2, NULL, 0, stop, &none);
  printf("cancelled run: status %d, %s\n", rc, dl_last_error());
  const char* const bad[] = {"--target", "tree=green"};
  rc = dl_run(DL_VIEWS, 2, bad, sensors, 2, NULL, 0, NULL, &none);
  printf("bad target: status %d, %s\n", rc, dl_last_error());

  dl_control_free(stop);
  dl_control_free(control);
  dl_result_free(r);
  dl_result_free(v);
  dl_mesh_free(sensors[0]);
  dl_mesh_free(sensors[1]);
  return 0;
}
