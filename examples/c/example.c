/* SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Commons-Clause-1.0 */
/*
 * daylight and views in-process from plain C, through daylight_c.dll (guide: docs/c-abi.md)
 *
 * The steps:
 *   1. the meshes: tests/box.obj from its file, a small facade from arrays
 *   2. daylight: a clear-sky year in Berlin, with a control to read its progress
 *   3. reading a result: values, columns and each mesh's rows from the manifest (per_mesh below)
 *   4. views: 120 deg about each face's normal, the box as a target group; this run reuses the GPU scene of step 2
 *   5. isovists: the volume and the area a standing eye sees in the room of tests/room.obj
 *   6. cancel and errors, then freeing the handles
 *
 * Build: the core's build makes this example too (cmake --build --preset release).
 * Run from the repo root:   build\RelWithDebInfo\example_c.exe tests
 * It prints, per run, the columns and the mean of every column per sensor mesh.
 */
#include "daylight_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Prints the columns of a result and, per sensor mesh, the mean of every column over that mesh's rows.
 *
 * The values come as one array, rows x columns, row by row. Which rows belong to which mesh is in the manifest, a
 * JSON text with a fixed layout:
 *     "columns": ["sky_front", ...], ...
 *     "meshes": [{"name": "ground", "first": 0, "count": 4}, {"name": "box", "first": 4, "count": 12}, ...]
 * A JSON parser reads it too; plain C here finds the pieces with strstr and sscanf. */
static void per_mesh(const dl_result* result) {
  const int64_t rows = dl_result_rows(result), cols = dl_result_cols(result);
  const char* manifest = dl_result_manifest(result);

  /* copy the values out of the result */
  double* values = malloc((size_t)(rows * cols) * sizeof *values);
  if (!values || dl_result_values(result, values, rows * cols) != DL_OK) {
    free(values);
    return;
  }

  /* the column names: the text between the first '[' and its ']' */
  const char* open = strchr(manifest, '[');
  const char* close = strchr(open, ']');
  printf("  columns %.*s\n", (int)(close - open - 1), open + 1);

  /* each {"name": ..., "first": ..., "count": ...} entry after "meshes" */
  const char* entry = strstr(manifest, "\"meshes\"");
  while ((entry = strstr(entry + 1, "{\"name\": ")) != NULL) {
    char name[64];
    long long first, count;
    if (sscanf(entry, "{\"name\": \"%63[^\"]\", \"first\": %lld, \"count\": %lld", name, &first, &count) != 3) break;
    printf("  %-6s %2lld faces:", name, count);
    for (int64_t c = 0; c < cols; ++c) {   /* the mean of column c over rows first .. first + count - 1 */
      double sum = 0;
      for (long long row = first; row < first + count; ++row) sum += values[row * cols + c];
      printf(" %9.4g", sum / (double)count);
    }
    printf("\n");
  }
  free(values);
}

/* A failed call: print what failed and the core's message, then stop (the handles made so far go with the process). */
static int fail(const char* what) {
  fprintf(stderr, "%s: %s\n", what, dl_last_error());
  return 2;
}

int main(int argc, char** argv) {
  const char* tests = argc > 1 ? argv[1] : "tests";   /* the folder holding box.obj, room.obj and room_grid.obj */
  char path[1024];

  /* ---- Step 1: the meshes ------------------------------------------------------------------------------------
   * A dl_mesh handle holds one or more meshes the core keeps (copied once). Both handles here are sensors: their faces
   * are analysed (a result row each) and block rays. */
  snprintf(path, sizeof path, "%s/box.obj", tests);
  dl_mesh* box = dl_mesh_load(path);   /* two OBJ groups, two meshes: "ground" (four 2 m quads at z 0), "box" (a 1 m box) */

  /* A 4 x 3 m wall at y = 3 facing south (-y), two 1.5 m high panels, from arrays:
   *   xyz      the points, x y z each, metres, Z up (6 points)
   *   counts   the vertices of each face (two quads)
   *   indices  each face's points, 0-based, counter-clockwise seen from the front (the side that is analysed) */
  const double xyz[] = {-2, 3, 0,   2, 3, 0,   2, 3, 1.5,   -2, 3, 1.5,   2, 3, 3,   -2, 3, 3};
  const int32_t counts[] = {4, 4};
  const int32_t indices[] = {0, 1, 2, 3,   3, 2, 4, 5};
  dl_mesh* facade = dl_mesh_create("facade", xyz, 6, counts, 2, indices, 8, NULL);   /* NULL: no optics slots */
  if (!box || !facade) return fail("mesh");
  dl_mesh* sensors[2] = {box, facade};

  /* ---- Step 2: daylight, a clear-sky year in Berlin --------------------------------------------------------------
   * The flags are the CLI's (docs/daylight.md), one string each. */
  const char* const sun[] = {
      "--clear-sky",                        /* no weather file: a cloudless year */
      "--lat", "52.52", "--lon", "13.41",   /* Berlin */
      "--quantity", "radiation,sunhours",   /* the columns: kWh/m2 and sun hours */
  };
  /* A control lets another thread read the run's progress (0..1) or cancel it; here progress is read afterwards. */
  dl_control* control = dl_control_create();
  dl_result* year = NULL;
  double stats[DL_STATS];   /* stats[0]: 1 when the run reused a resident GPU scene */
  /* dl_run(tool, flag count, flags, sensors, sensor count, context, context count, control, &result) */
  if (dl_run(DL_DAYLIGHT, 7, sun, sensors, 2, NULL, 0, control, &year) != DL_OK) return fail("daylight");
  dl_result_stats(year, stats);
  printf("daylight: %lld rows, scene reused %g, progress %g\n", (long long)dl_result_rows(year), stats[0], dl_control_progress(control));

  /* ---- Step 3: reading the result ------------------------------------------------------------------------------- */
  per_mesh(year);

  /* ---- Step 4: views about each face's normal --------------------------------------------------------------------
   * From every face a ray per 2 deg cell of a field 120 deg wide about its normal: the shares that end on the sky, the
   * ground, an obstruction or a target group, and how far the face sees. The same handles in the same roles as step
   * 2: the resident GPU scene serves this run (stats[0] = 1), nothing is rebuilt. */
  const char* const view[] = {"--target", "box=cube"};   /* rays ending on the mesh "box" count for the group "cube" */
  dl_result* seen = NULL;
  if (dl_run(DL_VIEWS, 2, view, sensors, 2, NULL, 0, NULL, &seen) != DL_OK) return fail("views");
  dl_result_stats(seen, stats);
  printf("views: %lld rows, scene reused %g\n", (long long)dl_result_rows(seen), stats[0]);
  per_mesh(seen);

  /* ---- Step 5: isovists in a room ----------------------------------------------------------------------------------
   * tests/room.obj: a 6.3 x 5.1 x 2.9 m room with a window and a tree outside, tests/room_grid.obj: 12 measuring points
   * at desk height. The isovist is what an eye sees around it: all around in 3D the volume in view (column volume, m3),
   * with horizontal rays only the area in view (column area, m2). */
  snprintf(path, sizeof path, "%s/room_grid.obj", tests);
  dl_mesh* grid = dl_mesh_load(path);   /* sensor: the measuring points */
  snprintf(path, sizeof path, "%s/room.obj", tests);
  dl_mesh* room = dl_mesh_load(path);   /* context: room, window, tree */
  if (!grid || !room) return fail("room");
  const char* const volumeFlags[] = {
      "--fov", "360,-90,90",     /* all around, the whole sphere */
      "--offset", "0.4",         /* the eye 0.4 m above each grid face */
      "--no-occlude", "grid",    /* the grid is measured, not seen: the rays pass it */
  };
  const char* const areaFlags[] = {
      "--fov", "360,0,0",        /* all around, horizontal rays only: the 2D isovist */
      "--offset", "0.4",
      "--no-occlude", "grid",
  };
  /* The isovist is the default column all around; --distance mean,min gives the distances instead, --far M caps both. */
  dl_result *volume = NULL, *area = NULL;
  if (dl_run(DL_VIEWS, 6, volumeFlags, &grid, 1, &room, 1, NULL, &volume) != DL_OK) return fail("isovist volume");
  if (dl_run(DL_VIEWS, 6, areaFlags, &grid, 1, &room, 1, NULL, &area) != DL_OK) return fail("isovist area");
  printf("isovists in the room, all around and horizontal:\n");
  per_mesh(volume);
  per_mesh(area);
  dl_result_free(volume);
  dl_result_free(area);
  dl_mesh_free(grid);
  dl_mesh_free(room);

  /* ---- Step 6: cancel and errors -----------------------------------------------------------------------------------
   * A failed run returns DL_USAGE 1 (bad flags, names the meshes do not resolve) or DL_RUNTIME 2 (a file, the GPU,
   * "cancelled"); dl_last_error() holds the message. First a run cancelled through its control (from another thread
   * while it runs; here before it starts), then a target name no mesh has. */
  dl_result* none = NULL;
  dl_control* stop = dl_control_create();
  dl_control_cancel(stop);
  int32_t status = dl_run(DL_VIEWS, 2, view, sensors, 2, NULL, 0, stop, &none);
  printf("cancelled run: status %d, %s\n", status, dl_last_error());
  const char* const bad[] = {"--target", "tree=green"};
  status = dl_run(DL_VIEWS, 2, bad, sensors, 2, NULL, 0, NULL, &none);
  printf("bad target: status %d, %s\n", status, dl_last_error());

  /* every handle is freed once; the GPU scene goes with the process */
  dl_control_free(stop);
  dl_control_free(control);
  dl_result_free(year);
  dl_result_free(seen);
  dl_mesh_free(box);
  dl_mesh_free(facade);
  return 0;
}
