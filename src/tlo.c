#include <GL/glew.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <dirent.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <cglm/cglm.h>

#include "tlo.h"
#include "common.h"
#include "rzp.h"
#include "triangulation.h"
#include "findSafe.h"
#include "opencl.h"

#define BLOCK_SIZE (3 * sizeof(double))
#define COUNT_TLO_ATTR 4 // x, y, z, height

#define TRIANGLE_ATTR COUNT_TLO_ATTR * 3

#define RADIUS_LA 15.

// draw mode
#define POINT_MODE 1
#define TRIANGLE_MODE 2
#define ZONE_MODE 3

// для пересборки триангуляционной сетки
#define REBUILD_IDLE    0
#define REBUILD_RUNNING 1
#define REBUILD_READY   2

FrgIndex TLO_TILES[128];

static char* getDirname(const char* path) {
  size_t len = strlen(path);
  size_t i;
  unsigned lenDirname = 0;
  for (i = len - 1; path[i] != '/'; i--);
  for (i -= 1; path[i] != '/'; i--) lenDirname++;

  char* dirname = (char*)malloc(lenDirname + 1);
  if (!dirname) {
    fprintf(stderr, "FAILED FROM MEMORY ALLOCATE\n");
    return NULL;
  }

  i++;
  int j;
  for (j = 0; path[i] != '/'; i++, j++) {
    dirname[j] = path[i];
  }
  dirname[j] = '\0';
  return dirname;
}

static char getTloPoints(const char *path, FrgIndex index, double *buffer,
                         int countPoints) {
  char fullpath[512];
  snprintf(fullpath, 512, "%s/%d/%d", path, index.i, index.j);

  FILE *fd = NULL;
  if (!(fd = fopen(fullpath, "rb"))) {
    fprintf(stderr, "FAILED FROM OPEN FILE %s\n", fullpath);
    return 0;
  }

  size_t sizeFrgData = countPoints * 3 * sizeof(double);
  size_t hasBeenRead = 0;
  if ((hasBeenRead = fread(buffer, 1, sizeFrgData, fd)) != sizeFrgData) {
    fprintf(stderr, "FILE %s HAS BEEN READ NOT FULL\n", fullpath);
    if (!hasBeenRead)
      fclose(fd);
      return 0;
  }
  fclose(fd);
  return 1;
}

static int getSizeofFrg(const char *path, FrgIndex index) {
  char fullpath[512];
  snprintf(fullpath, 512, "%s/%d/%d", path, index.i, index.j);
  struct stat st;
  if (stat(fullpath, &st) != 0)
    return 0;
  return (int)st.st_size;
}

/**
 * @param [in] double wx, double wy - мировые координаты
 */
static float getReliefHeight(const int *height, double wx, double wy) {
  int ix = (int)(wx / TERRAIN_QUAD_SIZE), iy = (int)(wy / TERRAIN_QUAD_SIZE);
  if (ix < 0)
    ix = 0;
  else if (ix > FRG_SIZE_X - 1)
    ix = FRG_SIZE_X - 1;

  if (iy < 0)
    iy = 0;
  else if (iy > FRG_SIZE_Y - 1)
    iy = FRG_SIZE_Y - 1;

  return (float)getRZPHeight(height, ix, iy);
}

static void buildTloPoints(TloRender *tlo, point_t* vrt) {
  if (!tlo->vao_points || !tlo->vbo_points)
    return;
  glBindVertexArray(tlo->vao_points);

  glBindBuffer(GL_ARRAY_BUFFER, tlo->vbo_points);
  glBufferData(GL_ARRAY_BUFFER,
               tlo->totalPoints * sizeof(point_t), vrt,
               GL_STATIC_DRAW);

  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                        sizeof(point_t), (void *)0);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE,
                        sizeof(point_t),
                        (void *)offsetof(point_t, height));
  glEnableVertexAttribArray(1);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

/*
 * Результат вычисления триангуляции — данные, готовые к загрузке в GPU.
 * Формируется на CPU (в т. ч. в фоновом потоке), не содержит вызовов GL,
 * поэтому может вычисляться вне главного потока. Загрузку в видеопамять
 * выполняет uploadTriang() уже в главном потоке.
 */
typedef struct {
  point_t*  meshVerts;     // вершины триангуляционной сетки
  int       meshVertCount;
  unsigned* meshIndices;   // индексы треугольников, по 3 на треугольник
  int       triCount;
  point_t*  zoneVerts;     // вершины безопасных зон (ранг в поле height)
  int       zoneVertCount;
  point_t*  circleVerts;   // вершины окружностей-маркеров посадочных кругов
  int       circleVertCount;
  int       ok;            // 1 — результат корректен
} TriangResult;

static void freeTriangResult(TriangResult* r) {
  if (!r) return;
  free(r->meshVerts);
  free(r->meshIndices);
  free(r->zoneVerts);
  free(r->circleVerts);
  r->meshVerts = NULL;
  r->meshIndices = NULL;
  r->zoneVerts = NULL;
  r->circleVerts = NULL;
  r->meshVertCount = r->triCount = r->zoneVertCount = r->circleVertCount = 0;
  r->ok = 0;
}

// Загружает готовый результат триангуляции в видеопамять.
// Только вызовы OpenGL — выполняется строго в главном потоке.
static void uploadTriang(TloRender* tlo, const TriangResult* res) {
  if (!res->ok || !tlo->vao_tri || !tlo->ebo || !tlo->vbo_tri)
    return;

  tlo->countTriangles = (GLsizei)res->triCount;

  glBindVertexArray(tlo->vao_tri);
  glBindBuffer(GL_ARRAY_BUFFER, tlo->vbo_tri);
  glBufferData(GL_ARRAY_BUFFER,
               (GLsizeiptr)((size_t)res->meshVertCount * sizeof(point_t)),
               res->meshVerts, GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tlo->ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               (GLsizeiptr)((size_t)res->triCount * 3 * sizeof(unsigned)),
               res->meshIndices, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                        sizeof(point_t), (void *)0);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE,
                        sizeof(point_t),
                        (void *)offsetof(point_t, height));
  glEnableVertexAttribArray(1);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  tlo->countZoneVerts = (GLsizei)res->zoneVertCount;
  if (res->zoneVertCount > 0 && res->zoneVerts &&
      tlo->vao_zone && tlo->vbo_zone) {
    glBindVertexArray(tlo->vao_zone);
    glBindBuffer(GL_ARRAY_BUFFER, tlo->vbo_zone);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)res->zoneVertCount * sizeof(point_t)),
                 res->zoneVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(point_t), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE,
                          sizeof(point_t),
                          (void *)offsetof(point_t, height));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
  }

  tlo->countCircleVerts = (GLsizei)res->circleVertCount;
  if (res->circleVertCount > 0 && res->circleVerts &&
      tlo->vao_circles && tlo->vbo_circles) {
    glBindVertexArray(tlo->vao_circles);
    glBindBuffer(GL_ARRAY_BUFFER, tlo->vbo_circles);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)res->circleVertCount * sizeof(point_t)),
                 res->circleVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(point_t), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE,
                          sizeof(point_t),
                          (void *)offsetof(point_t, height));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
  }
}

static double dist2_xy(const point_t* a, const point_t* b) {
  double dx = (double)a->x - (double)b->x;
  double dy = (double)a->y - (double)b->y;
  return dx * dx + dy * dy;
}

static double dist2_point_segment_xy(const point_t* p, const point_t* a, const point_t* b) {
  double px = (double)p->x, py = (double)p->y;
  double ax = (double)a->x, ay = (double)a->y;
  double bx = (double)b->x, by = (double)b->y;
  double abx = bx - ax, aby = by - ay;
  double apx = px - ax, apy = py - ay;
  double ab2 = abx * abx + aby * aby;
  if (ab2 <= 0.0) {
    return apx * apx + apy * apy;
  }
  double t = (apx * abx + apy * aby) / ab2;
  if (t < 0.0) t = 0.0;
  else if (t > 1.0) t = 1.0;
  double cx = ax + t * abx;
  double cy = ay + t * aby;
  double dx = px - cx, dy = py - cy;
  return dx * dx + dy * dy;
}

static double orient_xy(const point_t* a, const point_t* b, const point_t* c) {
  return ((double)b->x - (double)a->x) * ((double)c->y - (double)a->y) -
         ((double)b->y - (double)a->y) * ((double)c->x - (double)a->x);
}

static int point_on_segment_xy(const point_t* p, const point_t* a,
                               const point_t* b) {
  const double eps = 1e-9;
  if (fabs(orient_xy(a, b, p)) > eps) return 0;
  double minX = fmin((double)a->x, (double)b->x) - eps;
  double maxX = fmax((double)a->x, (double)b->x) + eps;
  double minY = fmin((double)a->y, (double)b->y) - eps;
  double maxY = fmax((double)a->y, (double)b->y) + eps;
  return (double)p->x >= minX && (double)p->x <= maxX &&
         (double)p->y >= minY && (double)p->y <= maxY;
}

static int segments_intersect_xy(const point_t* a, const point_t* b,
                                 const point_t* c, const point_t* d) {
  const double eps = 1e-9;
  double o1 = orient_xy(a, b, c);
  double o2 = orient_xy(a, b, d);
  double o3 = orient_xy(c, d, a);
  double o4 = orient_xy(c, d, b);

  if (fabs(o1) <= eps && point_on_segment_xy(c, a, b)) return 1;
  if (fabs(o2) <= eps && point_on_segment_xy(d, a, b)) return 1;
  if (fabs(o3) <= eps && point_on_segment_xy(a, c, d)) return 1;
  if (fabs(o4) <= eps && point_on_segment_xy(b, c, d)) return 1;

  return ((o1 > 0.0) != (o2 > 0.0)) && ((o3 > 0.0) != (o4 > 0.0));
}

static int point_in_triangle_xy(const point_t* p, const point_t* a,
                                const point_t* b, const point_t* c) {
  const double eps = 1e-9;
  double o1 = orient_xy(a, b, p);
  double o2 = orient_xy(b, c, p);
  double o3 = orient_xy(c, a, p);
  int hasNeg = (o1 < -eps) || (o2 < -eps) || (o3 < -eps);
  int hasPos = (o1 > eps) || (o2 > eps) || (o3 > eps);
  return !(hasNeg && hasPos);
}

static double dist2_segment_segment_xy(const point_t* a, const point_t* b,
                                       const point_t* c, const point_t* d) {
  if (segments_intersect_xy(a, b, c, d)) return 0.0;

  double minD2 = dist2_point_segment_xy(a, c, d);
  double d2 = dist2_point_segment_xy(b, c, d);
  if (d2 < minD2) minD2 = d2;
  d2 = dist2_point_segment_xy(c, a, b);
  if (d2 < minD2) minD2 = d2;
  d2 = dist2_point_segment_xy(d, a, b);
  if (d2 < minD2) minD2 = d2;
  return minD2;
}

static double dist2_triangle_segment_xy(const point_t* a, const point_t* b,
                                        const point_t* c, const point_t* s0,
                                        const point_t* s1) {
  if (point_in_triangle_xy(s0, a, b, c) ||
      point_in_triangle_xy(s1, a, b, c)) {
    return 0.0;
  }

  double minD2 = dist2_segment_segment_xy(a, b, s0, s1);
  double d2 = dist2_segment_segment_xy(b, c, s0, s1);
  if (d2 < minD2) minD2 = d2;
  d2 = dist2_segment_segment_xy(c, a, s0, s1);
  if (d2 < minD2) minD2 = d2;
  return minD2;
}

static double min_dist2_triangle_to_contours(const Triangle* tri,
                                             const point_t* points,
                                             size_t countPoints,
                                             const polygon_t* polys,
                                             size_t polyCount,
                                             unsigned pid,
                                             double stopD2) {
  if (tri->p1 < 0 || tri->p2 < 0 || tri->p3 < 0 ||
      (size_t)tri->p1 >= countPoints ||
      (size_t)tri->p2 >= countPoints ||
      (size_t)tri->p3 >= countPoints) {
    return 0.0;
  }

  const point_t* a = &points[tri->p1];
  const point_t* b = &points[tri->p2];
  const point_t* c = &points[tri->p3];
  double minD2 = 1e300;
  int foundContourEdge = 0;

  for (size_t pi = 0; pi < polyCount; pi++) {
    if (polys[pi].numPolygon != pid) continue;
    size_t m = polys[pi].cntVerts;
    if (m < 2) continue;

    for (size_t cj = 0; cj < m; cj++) {
      int c0 = polys[pi].verts[cj];
      int c1 = polys[pi].verts[(cj + 1) % m];
      if (c0 < 0 || c1 < 0) continue;
      if ((size_t)c0 >= countPoints) continue;
      if ((size_t)c1 >= countPoints) continue;

      foundContourEdge = 1;
      double d2 = dist2_triangle_segment_xy(a, b, c, &points[c0], &points[c1]);
      if (d2 < minD2) {
        minD2 = d2;
        if (minD2 < stopD2) return minD2;
      }
    }
  }

  return foundContourEdge ? minD2 : 0.0;
}

static int isCorrectTriangle_local(const Triangle* tri, const point_t* points) {
  // z — высота поверхности TLO, height — высота рельефа RZP.
  // Сначала отбрасываем высокие объекты над рельефом, затем проверяем уклон.
  const point_t* p[3] = {
    &points[tri->p1],
    &points[tri->p2],
    &points[tri->p3]
  };

  float h[3] = { p[0]->z, p[1]->z, p[2]->z };
  for (int i = 0; i < 3; i++) {
    if (p[i]->height < 0.0f) return 0;
    if (p[i]->z - p[i]->height > MAX_HEIGTH) return 0;
  }

  double maxH = h[0], minH = h[0];
  for (int i = 1; i < 3; i++) {
    if (h[i] > maxH) maxH = h[i];
    if (h[i] < minH) minH = h[i];
  }
  // минимальная длина ребра (чтобы нормировать уклон)
  double d2_12 = dist2_xy(&points[tri->p1], &points[tri->p2]);
  double d2_23 = dist2_xy(&points[tri->p2], &points[tri->p3]);
  double d2_31 = dist2_xy(&points[tri->p3], &points[tri->p1]);
  double minD2 = d2_12;
  if (d2_23 < minD2) minD2 = d2_23;
  if (d2_31 < minD2) minD2 = d2_31;
  if (minD2 <= 0.0) return 0;

  // Перепад высот в пределах шума измерений: треугольник считается ровным
  // независимо от размера. Без этого мелкие треугольники густой сетки
  // ложно отбраковываются по уклону из-за шума данных.
  double dz = maxH - minH;
  if (dz <= Z_FLAT_TOLERANCE) return 1;

  return (dz / sqrt(minD2) < ANGLE_SLOPE) ? 1 : 0;
}

static char varifyTloData(const char* path, size_t* sizeTlo, int* countTlo) {
  struct dirent *entry;
  struct stat st;

  DIR* dir = NULL;
  if (!(dir = opendir(path))) {
    fprintf(stderr, "FAILED FROM OPEN DIR %s\n", path);
    return 0;
  }

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 ||
        strcmp(entry->d_name, "..") == 0 ||
        strcmp(entry->d_name, "info") == 0)
      continue;

    char fullname[512];
    snprintf(fullname, 512, "%s/%s", path, entry->d_name);

    if (stat(fullname, &st) != 0)
      continue;

    if (S_ISDIR(st.st_mode)) {
      if (!varifyTloData(fullname, sizeTlo, countTlo))
        return 0;
    } else if (S_ISREG(st.st_mode) && st.st_size > 0) {
      *sizeTlo += st.st_size;
      char* dirname = getDirname(fullname);
      TLO_TILES[(*countTlo)++] = (FrgIndex){atoi(dirname), atoi(entry->d_name)};
      free(dirname);
    }
  }
  closedir(dir);
  return 1;
}

/*
 * Вычисление триангуляции (этап CPU, без вызовов OpenGL).
 * Строит триангуляцию Делоне, классифицирует треугольники, выделяет и
 * ранжирует безопасные зоны и формирует массивы вершин и индексов,
 * готовые к загрузке в видеопамять. Не обращается к структуре TloRender
 * и к OpenGL, поэтому может выполняться в фоновом потоке.
 */
static int computeTriang(const point_t* cloud, int cloudCount,
                         TriangResult* res) {
  memset(res, 0, sizeof(*res));
  if (!cloud || cloudCount < 3)
    return 0;

  int countPoints = cloudCount;
  point_t* triPoints = (point_t*)malloc((size_t)countPoints * sizeof(point_t));
  if (!triPoints) {
    fprintf(stderr, "FAILED FROM MEMORY ALLOCATE (triPoints)\n");
    return 0;
  }
  memcpy(triPoints, cloud, (size_t)countPoints * sizeof(point_t));

  int countTriangles = 0;
  Triangle* triangles =
      delaunay_triangulation(triPoints, &countPoints, &countTriangles);
  if (!triangles || countTriangles <= 0) {
    fprintf(stderr, "TRIANGULATION FAILED (COUNT=%d)\n", countTriangles);
    free(triangles);
    free(triPoints);
    return 0;
  }

  // классификация треугольников (на OpenCL-устройстве либо на CPU)
  unsigned char* clsBuf = (unsigned char*)malloc((size_t)countTriangles);
  const unsigned char* precomputed = NULL;
  if (clsBuf &&
      openclClassifyTriangles(triangles, (size_t)countTriangles,
                              triPoints, (size_t)countPoints,
                              MAX_HEIGTH, ANGLE_SLOPE, Z_FLAT_TOLERANCE,
                              clsBuf))
    precomputed = clsBuf;

  // связные области корректных треугольников и их контуры
  polygon_t* polys = NULL;
  size_t polyCount = triangulation_find_polygons(
      triangles, (size_t)countTriangles, triPoints, (size_t)countPoints,
      isCorrectTriangle_local, precomputed, &polys);
  free(clsBuf);

  // безопасные треугольники и их удаление от опасных границ
  const double aircraftR2 = RADIUS_LA * RADIUS_LA;
  unsigned maxPid = 0;
  for (int i = 0; i < countTriangles; i++)
    if (triangles[i].numPolygon > maxPid) maxPid = triangles[i].numPolygon;

  unsigned char* triIsSafe =
      (unsigned char*)calloc((size_t)countTriangles, 1);
  double* triClearance =
      (double*)calloc((size_t)countTriangles, sizeof(double));
  size_t dbgCorrectTri = 0, dbgSafeTri = 0;
  double dbgMaxClr = 0.0;
  if (triIsSafe && triClearance && polyCount > 0 && maxPid > 0) {
    for (int ti = 0; ti < countTriangles; ti++) {
      unsigned pid = triangles[ti].numPolygon;
      if (pid == 0 || pid > maxPid) continue;
      dbgCorrectTri++;
      double minD2 = min_dist2_triangle_to_contours(
          &triangles[ti], triPoints, (size_t)countPoints,
          polys, polyCount, pid, aircraftR2);
      triClearance[ti] = sqrt(minD2);
      if (triClearance[ti] > dbgMaxClr) dbgMaxClr = triClearance[ti];
      if (minD2 >= aircraftR2) {
        triIsSafe[ti] = 1;
        dbgSafeTri++;
      }
    }
  }

  // индексы треугольников сетки (с правкой ориентации обхода)
  unsigned* indices =
      (unsigned*)malloc((size_t)countTriangles * 3 * sizeof(unsigned));
  if (indices) {
    for (int i = 0; i < countTriangles; i++) {
      int a = triangles[i].p1, b = triangles[i].p2, c = triangles[i].p3;
      float ax = triPoints[a].x, ay = triPoints[a].y;
      float bx = triPoints[b].x, by = triPoints[b].y;
      float cx = triPoints[c].x, cy = triPoints[c].y;
      float area2 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
      if (area2 < 0) { int t = b; b = c; c = t; }
      indices[i * 3]     = (unsigned)a;
      indices[i * 3 + 1] = (unsigned)b;
      indices[i * 3 + 2] = (unsigned)c;
    }
  }

  // ранжирование безопасных зон и вершины зон (ранг — в поле height)
  SafeZone* zones = NULL;
  int* triRank = (int*)malloc((size_t)countTriangles * sizeof(int));
  size_t numZones = 0;
  point_t* zoneVerts = NULL;
  int zoneVertCount = 0;
  if (triRank && triIsSafe) {
    numZones = rankSafeZones(triangles, (size_t)countTriangles, triPoints,
                             triIsSafe, triClearance, maxPid,
                             zoneWeightsDefault(), &zones, triRank);
    size_t safeCount = 0;
    for (int i = 0; i < countTriangles; i++)
      if (triRank[i] >= 0) safeCount++;
    if (safeCount > 0) {
      zoneVerts = (point_t*)malloc(safeCount * 3 * sizeof(point_t));
      if (zoneVerts) {
        double denom = (numZones > 1) ? (double)(numZones - 1) : 1.0;
        size_t v = 0;
        for (int i = 0; i < countTriangles; i++) {
          if (triRank[i] < 0) continue;
          float rankNorm =
              (numZones > 1) ? (float)(triRank[i] / denom) : 0.0f;
          int idx[3] = { triangles[i].p1, triangles[i].p2, triangles[i].p3 };
          for (int k = 0; k < 3; k++) {
            zoneVerts[v] = triPoints[idx[k]];
            zoneVerts[v].height = rankNorm;
            v++;
          }
        }
        zoneVertCount = (int)v;
      }
    }
  }

  /* Вершины окружностей-маркеров: для каждой зоны окружность радиуса
     RADIUS_LA с центром в (zone.cx, zone.cy, zone.cz). Окружность
     выводится как набор отрезков GL_LINES: 64 сегмента × 2 вершины. */
  const int CIRCLE_SEG = 64;
  point_t* circleVerts = NULL;
  int circleVertCount = 0;
  if (zones && numZones > 0) {
    circleVerts =
        (point_t*)malloc((size_t)numZones * CIRCLE_SEG * 2 * sizeof(point_t));
    if (circleVerts) {
      double denomC = (numZones > 1) ? (double)(numZones - 1) : 1.0;
      const double dTheta = 2.0 * M_PI / CIRCLE_SEG;
      const double Z_BIAS = 0.6; /* небольшой подъём, чтобы не сливалось с заливкой */
      for (size_t zi = 0; zi < numZones; zi++) {
        float rankNorm =
            (numZones > 1) ? (float)(zones[zi].rank / denomC) : 0.0f;
        double cx = zones[zi].cx;
        double cy = zones[zi].cy;
        double cz = zones[zi].cz + Z_BIAS;
        for (int s = 0; s < CIRCLE_SEG; s++) {
          double a1 = (double)s * dTheta;
          double a2 = (double)((s + 1) % CIRCLE_SEG) * dTheta;
          point_t v1 = {
            .x = (float)(cx + RADIUS_LA * cos(a1)),
            .y = (float)(cy + RADIUS_LA * sin(a1)),
            .z = (float)cz,
            .height = rankNorm,
            .id = 0
          };
          point_t v2 = {
            .x = (float)(cx + RADIUS_LA * cos(a2)),
            .y = (float)(cy + RADIUS_LA * sin(a2)),
            .z = (float)cz,
            .height = rankNorm,
            .id = 0
          };
          circleVerts[circleVertCount++] = v1;
          circleVerts[circleVertCount++] = v2;
        }
      }
    }
  }

  printf("[zones] points=%d tris=%d polys=%zu correctTri=%zu maxPid=%u "
         "clrLimit=%.1f maxClr=%.2f safeTri=%zu safeZones=%zu zoneVerts=%d circles=%d\n",
         countPoints, countTriangles, polyCount, dbgCorrectTri, maxPid,
         (double)RADIUS_LA, dbgMaxClr, dbgSafeTri, numZones,
         zoneVertCount, circleVertCount / (2 * CIRCLE_SEG));

  triangulation_free_polygons(polys, polyCount);
  free(zones);
  free(triRank);
  memDestroy(2, (void*)triIsSafe, (void*)triClearance);
  free(triangles);

  if (!indices) {
    free(triPoints);
    free(zoneVerts);
    free(circleVerts);
    return 0;
  }

  res->meshVerts       = triPoints;       // владение передаётся результату
  res->meshVertCount   = countPoints;
  res->meshIndices     = indices;
  res->triCount        = countTriangles;
  res->zoneVerts       = zoneVerts;
  res->zoneVertCount   = zoneVertCount;
  res->circleVerts     = circleVerts;
  res->circleVertCount = circleVertCount;
  res->ok = 1;
  return 1;
}

// Синхронная триангуляция: вычисление + загрузка в GPU (главный поток).
static char triangTlo(TloRender* tlo, point_t* vrt, int countPoints) {
  TriangResult res;
  if (!computeTriang(vrt, countPoints, &res)) {
    tlo->countTriangles = 0;
    return 0;
  }
  uploadTriang(tlo, &res);
  freeTriangResult(&res);
  return 1;
}

static struct {
  pthread_t      thread;
  int            state;       // под защитой g_rebuildLock
  int            hasThread;   // поток создан и ещё не присоединён
  const point_t* cloud;       // вход (указывает на TloRender.cloud)
  int            cloudCount;
  TriangResult   result;      // выход фонового потока
} g_rebuild = { .state = REBUILD_IDLE, .hasThread = 0 };

static pthread_mutex_t g_rebuildLock = PTHREAD_MUTEX_INITIALIZER;

// Тело фонового потока: тяжёлое вычисление триангуляции без вызовов GL.
static void* rebuildWorker(void* arg) {
  (void)arg;
  computeTriang(g_rebuild.cloud, g_rebuild.cloudCount, &g_rebuild.result);
  pthread_mutex_lock(&g_rebuildLock);
  g_rebuild.state = REBUILD_READY;
  pthread_mutex_unlock(&g_rebuildLock);
  return NULL;
}

int tloRebuildInProgress(void) {
  pthread_mutex_lock(&g_rebuildLock);
  int busy = (g_rebuild.state != REBUILD_IDLE);
  pthread_mutex_unlock(&g_rebuildLock);
  return busy;
}

char rebuildTloTriangulation(TloRender* tlo) {
  if (!tlo || !tlo->is_loaded || !tlo->cloud || tlo->cloudCount < 3)
    return 0;

  pthread_mutex_lock(&g_rebuildLock);
  if (g_rebuild.state != REBUILD_IDLE) {
    pthread_mutex_unlock(&g_rebuildLock);
    return 0;  // пересборка уже выполняется
  }
  g_rebuild.cloud = tlo->cloud;
  g_rebuild.cloudCount = tlo->cloudCount;
  g_rebuild.state = REBUILD_RUNNING;
  pthread_mutex_unlock(&g_rebuildLock);

  if (pthread_create(&g_rebuild.thread, NULL, rebuildWorker, NULL) != 0) {
    // поток создать не удалось — выполняем синхронно
    pthread_mutex_lock(&g_rebuildLock);
    g_rebuild.state = REBUILD_IDLE;
    pthread_mutex_unlock(&g_rebuildLock);
    return triangTlo(tlo, tlo->cloud, tlo->cloudCount);
  }
  g_rebuild.hasThread = 1;
  return 1;
}

void pollTloRebuild(TloRender* tlo) {
  pthread_mutex_lock(&g_rebuildLock);
  int st = g_rebuild.state;
  pthread_mutex_unlock(&g_rebuildLock);
  if (st != REBUILD_READY)
    return;

  if (g_rebuild.hasThread) {
    pthread_join(g_rebuild.thread, NULL);
    g_rebuild.hasThread = 0;
  }
  if (g_rebuild.result.ok)
    uploadTriang(tlo, &g_rebuild.result);  // загрузка в GPU — главный поток
  freeTriangResult(&g_rebuild.result);

  pthread_mutex_lock(&g_rebuildLock);
  g_rebuild.state = REBUILD_IDLE;
  pthread_mutex_unlock(&g_rebuildLock);
}

char initTlo(TloRender* tlo, const char* path, size_t* sizeTlo, int* countTlo) {
  if (!varifyTloData(path, sizeTlo, countTlo)) return 0;

  glGenVertexArrays(1, &tlo->vao_points);
  glGenVertexArrays(1, &tlo->vao_tri);
  glGenVertexArrays(1, &tlo->vao_zone);
  glGenVertexArrays(1, &tlo->vao_circles);

  glGenBuffers(1, &tlo->vbo_points);
  glGenBuffers(1, &tlo->vbo_tri);
  glGenBuffers(1, &tlo->ebo);
  glGenBuffers(1, &tlo->vbo_zone);
  glGenBuffers(1, &tlo->vbo_circles);

  tlo->totalPoints = tlo->countTriangles = tlo->countZoneVerts = 0;
  tlo->countCircleVerts = 0;
  tlo->cloud = NULL;
  tlo->cloudCount = 0;
  tlo->is_loaded = 0;
  return 1;
}

char loadAllTloData(TloRender* tlo, const char* path, size_t sizeTlo, int countTlo, const int* heights) {
  size_t totalPoints = sizeTlo / BLOCK_SIZE;
  if (!totalPoints) {
    fprintf(stderr, "NO TLO DATA FOUND = %zu)\n", sizeTlo);
    return 0;
  }

  point_t* vrt = (point_t*)malloc(totalPoints * sizeof(point_t));
  if (!vrt) {
    fprintf(stderr, "FAILED FROM MEMORY ALLOCATE\n");
    return 0;
  }

  size_t fragNumber = 0;

  // инициализация границ облака точек
  tlo->minX = tlo->minY = tlo->minZ =  FLT_MAX;
  tlo->maxX = tlo->maxY = tlo->maxZ = -FLT_MAX;
  for (int i = 0; i < countTlo; i++) {
    int countPoints = getSizeofFrg(path, TLO_TILES[i]) / BLOCK_SIZE;
    if (!countPoints) continue;

    double* ptsFull = (double*)malloc(countPoints * 3 * sizeof(double));
    if (!ptsFull) {
      fprintf(stderr, "FAILED FROM MEMORY ALLOCATE\n");
      return 0;
    }

    if (!getTloPoints(path, TLO_TILES[i], ptsFull, countPoints)) {
      free(ptsFull);
      continue;
    }

    for (int j = 0; j < countPoints; j++) {
      double x = ptsFull[j * 3], y = ptsFull[j * 3 + 1], z = ptsFull[j * 3 + 2];
      float curHeight = getReliefHeight(heights, x, y);
      vrt[fragNumber].x = (float)x;
      vrt[fragNumber].y = (float)y;
      vrt[fragNumber].z = (float)z;
      vrt[fragNumber].height = curHeight;
      vrt[fragNumber].id = fragNumber;
      fragNumber++;

      // обновление границ
      if ((float)x < tlo->minX) tlo->minX = (float)x;
      else if ((float)x > tlo->maxX) tlo->maxX = (float)x;
      if ((float)y < tlo->minY) tlo->minY = (float)y;
      else if ((float)y > tlo->maxY) tlo->maxY = (float)y;
      if ((float)z < tlo->minZ) tlo->minZ = (float)z;
      else if ((float)z > tlo->maxZ) tlo->maxZ = (float)z;
    }
    free(ptsFull);
  }
  tlo->totalPoints = fragNumber;

  buildTloPoints(tlo, vrt);

  // облако точек сохраняется в TloRender для пересборки триангуляции
  // с другой густотой сетки; triangTlo копирует данные и не изменяет vrt
  tlo->cloud = vrt;
  tlo->cloudCount = (int)fragNumber;
  tlo->is_loaded = 1;

  if (!triangTlo(tlo, tlo->cloud, tlo->cloudCount)) {
    free(vrt);
    tlo->cloud = NULL;
    tlo->cloudCount = 0;
    tlo->is_loaded = 0;
    return 0;
  }

  printf("avg count poinst in fragment = %d\n", tlo->totalPoints / countTlo);

  return 1;
}

// Режим 1: облако точек.
static void drawPoints(TloRender* tlo, GLuint prog) {
  glUniform1i(glGetUniformLocation(prog, "u_pass"), 0);
  glBindVertexArray(tlo->vao_points);
  glDrawArrays(GL_POINTS, 0, tlo->totalPoints);
  glBindVertexArray(0);
}

// Режимы 2 и 3: триангуляционная сетка. При withZones != 0 поверх серой
// сетки выводятся безопасные зоны, окрашенные по рангу пригодности.
static void drawMesh(TloRender* tlo, GLuint prog, int withZones) {
  if (tlo->countTriangles <= 0) {
    drawPoints(tlo, prog);
    return;
  }
  GLint passLoc = glGetUniformLocation(prog, "u_pass");

  // поверхность видна с обеих сторон — отключаем отсечение граней
  glDisable(GL_CULL_FACE);

  // 1) залитая серая сетка с плоским освещением граней
  glUniform1i(passLoc, 1);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.2f, 1.2f);
  glBindVertexArray(tlo->vao_tri);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tlo->ebo);
  glDrawElements(GL_TRIANGLES, tlo->countTriangles * 3, GL_UNSIGNED_INT, 0);

  // 2) безопасные зоны поверх заливки (режим 3)
  if (withZones && tlo->countZoneVerts > 0) {
    glUniform1i(passLoc, 3);
    glDepthFunc(GL_LEQUAL);
    glBindVertexArray(tlo->vao_zone);
    glDrawArrays(GL_TRIANGLES, 0, tlo->countZoneVerts);
    glDepthFunc(GL_LESS);
  }
  glDisable(GL_POLYGON_OFFSET_FILL);

  // 3) тёмный каркас рёбер поверх заливки — делает сетку чётко видимой
  glUniform1i(passLoc, 2);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glLineWidth(1.3f);
  glDepthFunc(GL_LEQUAL);
  glBindVertexArray(tlo->vao_tri);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tlo->ebo);
  glDrawElements(GL_TRIANGLES, tlo->countTriangles * 3, GL_UNSIGNED_INT, 0);
  glDepthFunc(GL_LESS);

  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glLineWidth(1.0f);

  // 4) посадочные окружности радиуса ЛА в центрах зон (режим 3)
  if (withZones && tlo->countCircleVerts > 0 && tlo->vao_circles) {
    glUniform1i(passLoc, 4);
    glDepthFunc(GL_LEQUAL);
    glLineWidth(3.0f);
    glBindVertexArray(tlo->vao_circles);
    glDrawArrays(GL_LINES, 0, tlo->countCircleVerts);
    glLineWidth(1.0f);
    glDepthFunc(GL_LESS);
  }

  glBindVertexArray(0);
  glEnable(GL_CULL_FACE);
}

char drawTlo(TloRender* tlo, GLuint shaderProg) {
  if (!tlo->is_loaded || !tlo->totalPoints) {
    fprintf(stderr, "DATA TLO NOT LOADED OR NOT POINTS\n");
    return 0;
  }

  glUseProgram(shaderProg);

  // цвет серой сетки и цвет рёбер каркаса
  GLint meshLoc = glGetUniformLocation(shaderProg, "u_meshColor");
  GLint wireLoc = glGetUniformLocation(shaderProg, "u_wireColor");
  if (meshLoc >= 0) glUniform3f(meshLoc, 0.66f, 0.68f, 0.71f);
  if (wireLoc >= 0) glUniform3f(wireLoc, 0.12f, 0.13f, 0.15f);

  if (drawMode == TRIANGLE_MODE) {
    drawMesh(tlo, shaderProg, 0);
  } else if (drawMode == ZONE_MODE) {
    drawMesh(tlo, shaderProg, 1);
  } else {
    drawPoints(tlo, shaderProg);
  }

  return 1;
}

void freeTlo(TloRender* tlo) {
  // дождаться завершения фоновой пересборки, если она ещё идёт
  if (g_rebuild.hasThread) {
    pthread_join(g_rebuild.thread, NULL);
    g_rebuild.hasThread = 0;
    freeTriangResult(&g_rebuild.result);
    g_rebuild.state = REBUILD_IDLE;
  }
  if (tlo->vao_points)  glDeleteVertexArrays(1, &tlo->vao_points);
  if (tlo->vao_tri)     glDeleteVertexArrays(1, &tlo->vao_tri);
  if (tlo->vao_zone)    glDeleteVertexArrays(1, &tlo->vao_zone);
  if (tlo->vao_circles) glDeleteVertexArrays(1, &tlo->vao_circles);
  if (tlo->vbo_points)  glDeleteBuffers(1, &tlo->vbo_points);
  if (tlo->vbo_tri)     glDeleteBuffers(1, &tlo->vbo_tri);
  if (tlo->ebo)         glDeleteBuffers(1, &tlo->ebo);
  if (tlo->vbo_zone)    glDeleteBuffers(1, &tlo->vbo_zone);
  if (tlo->vbo_circles) glDeleteBuffers(1, &tlo->vbo_circles);
  free(tlo->cloud);
  tlo->cloud = NULL;
  tlo->cloudCount = 0;
  tlo->totalPoints = tlo->countTriangles = tlo->countZoneVerts = 0;
  tlo->countCircleVerts = 0;
  tlo->is_loaded = 0;
}
