#include <GL/glew.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <dirent.h>
#include <float.h>
#include <cglm/cglm.h>

#include "tlo.h"
#include "common.h"
#include "rzp.h"
#include "triangulation.h"
#include "findSafe.h"

#define BLOCK_SIZE (3 * sizeof(double))
#define COUNT_TLO_ATTR 4 // x, y, z, height

#define TRIANGLE_ATTR COUNT_TLO_ATTR * 3

// draw mode
#define POINT_MODE 1
#define TRIANGLE_MODE 2


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
    fclose(fd);
    if (!hasBeenRead)
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

static void buildTloTriangles(TloRender *tlo, Triangle* tri, point_t* triVerts,
                              int numTriVerts, const unsigned char* triIsSafe) {
  if (!tlo->vao_tri || !tlo->ebo || !tlo->ebo_safe || !tlo->vbo_tri)
    return;

  glBindVertexArray(tlo->vao_tri);

  glBindBuffer(GL_ARRAY_BUFFER, tlo->vbo_tri);
  glBufferData(GL_ARRAY_BUFFER, (size_t)numTriVerts * sizeof(point_t),
               triVerts, GL_STATIC_DRAW);

  size_t idxSize = (size_t)tlo->countTriangles * 3 * sizeof(unsigned);
  unsigned *indices = (unsigned*)malloc(idxSize);
  unsigned *indicesSafe = (unsigned*)malloc(idxSize);
  if (!indices || !indicesSafe) {
    memDestroy(2, (void*)indices, (void*)indicesSafe);
    return;
  }
  int safeTriCount = 0;
  for (int i = 0; i < tlo->countTriangles; i++) {
    int a = tri[i].p1, b = tri[i].p2, c = tri[i].p3;
    float ax = triVerts[a].x, ay = triVerts[a].y;
    float bx = triVerts[b].x, by = triVerts[b].y;
    float cx = triVerts[c].x, cy = triVerts[c].y;
    // обеспечиваем положительную ориентацию
    float area2 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area2 < 0) { int t = b; b = c; c = t; }
    indices[i * 3]     = (unsigned)a;
    indices[i * 3 + 1] = (unsigned)b;
    indices[i * 3 + 2] = (unsigned)c;

    if (triIsSafe && triIsSafe[i]) {
      indicesSafe[safeTriCount * 3]     = (unsigned)a;
      indicesSafe[safeTriCount * 3 + 1] = (unsigned)b;
      indicesSafe[safeTriCount * 3 + 2] = (unsigned)c;
      safeTriCount++;
    }
  }

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tlo->ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)idxSize, indices, GL_STATIC_DRAW);
  free(indices);

  tlo->countTrianglesSafe = (GLsizei)safeTriCount;
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tlo->ebo_safe);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)safeTriCount * 3 * sizeof(unsigned)),
               indicesSafe, GL_STATIC_DRAW);
  free(indicesSafe);

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

static int isCorrectTriangle_local(const Triangle* tri, const point_t* points) {
  // В tlo точки лежат на поверхности: используем реальную высоту поверхности (z),
  // а не матрицу рельефа (height), которая может быть в других единицах.
  float h[3] = { points[tri->p1].z, points[tri->p2].z, points[tri->p3].z };
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
  return ((maxH - minH) / sqrt(minD2) < ANGLE_SLOPE) ? 1 : 0;
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

static char triangTlo(TloRender *tlo, point_t* vrt, int countPoints) {
  int countTriangles = 0;

  point_t* triPoints = (point_t*)malloc((size_t)countPoints * sizeof(point_t));
  if (!triPoints) {
    fprintf(stderr, "FAILED FROM MEMORY ALLOCATE (triPoints)\n");
    return 0;
  }

  memcpy(triPoints, vrt, (size_t)countPoints * sizeof(point_t));

  Triangle *triangles =
      delaunay_triangulation(triPoints, &countPoints, &countTriangles);

  if (!triangles || countTriangles <= 0) {
    fprintf(stderr, "TRIANGULATION FAILED (COUNT=%d), POINTS ONLY\n", countTriangles);
    memDestroy(2, (void*)triangles, (void*)triPoints);
    tlo->countTriangles = 0;
    return 0;
  }

  tlo->countTriangles = (GLsizei)countTriangles;

  // 1) строим полигоны (контуры областей из "корректных" треугольников)
  polygon_t* polys = NULL;
  size_t polyCount = triangulation_find_polygons(triangles, (size_t)countTriangles,
                                                 triPoints, (size_t)countPoints,
                                                 isCorrectTriangle_local,
                                                 &polys);
#ifdef DEBUG
  size_t correctTri = 0;
  for (int i = 0; i < countTriangles; i++) if (triangles[i].numPolygon != 0) correctTri++;
  printf("[safe] points=%d tris=%d correctTris=%zu polys=%zu\n",
         countPoints, countTriangles, correctTri, polyCount);
#endif

  // 2) определяем "безопасные" области: существует ли вершина с minDist(до контура) >= 9
  const double aircraftR = 9.0;
  unsigned maxPid = 0;
  for (int i = 0; i < countTriangles; i++) if (triangles[i].numPolygon > maxPid) maxPid = triangles[i].numPolygon;
  unsigned char* safePid = (unsigned char*)calloc((size_t)maxPid + 1, 1);
  unsigned char* triIsSafe = (unsigned char*)calloc((size_t)countTriangles, 1);

  if (safePid && triIsSafe && polyCount > 0 && maxPid > 0) {
    unsigned char* seenV = (unsigned char*)calloc((size_t)countPoints, 1);
    for (unsigned pid = 1; pid <= maxPid; pid++) {
      // собрать кандидатов-вершин из треугольников области
      if (seenV) memset(seenV, 0, (size_t)countPoints);
      double best = 0.0;
      int found = 0;
      for (int ti = 0; ti < countTriangles && !found; ti++) {
        if (triangles[ti].numPolygon != pid) continue;
        int vs[3] = { triangles[ti].p1, triangles[ti].p2, triangles[ti].p3 };
        for (int vi = 0; vi < 3 && !found; vi++) {
          int v = vs[vi];
          if ((unsigned)v >= (unsigned)countPoints) continue;
          if (seenV && seenV[v]) continue;
          if (seenV) seenV[v] = 1;

          // минимальная дистанция от кандидата до всех точек контуров области
          double minD2 = 1e300;
          const point_t* pv = &triPoints[v];
          for (size_t pi = 0; pi < polyCount; pi++) {
            if (polys[pi].numPolygon != pid) continue;
            size_t m = polys[pi].cntVerts;
            if (m < 2) continue;
            for (size_t cj = 0; cj < m; cj++) {
              int c0 = polys[pi].verts[cj];
              int c1 = polys[pi].verts[(cj + 1) % m];
              if ((unsigned)c0 >= (unsigned)countPoints) continue;
              if ((unsigned)c1 >= (unsigned)countPoints) continue;
              double d2 = dist2_point_segment_xy(pv, &triPoints[c0], &triPoints[c1]);
              if (d2 < minD2) {
                minD2 = d2;
                if (minD2 < aircraftR * aircraftR) break;
              }
            }
            if (minD2 < aircraftR * aircraftR) break;
          }
          double minD = sqrt(minD2);
          if (minD > best) best = minD;
          if (best >= aircraftR) {
            safePid[pid] = 1;
            found = 1;
          }
        }
      }
    }
    free(seenV);

    for (int ti = 0; ti < countTriangles; ti++) {
      unsigned pid = triangles[ti].numPolygon;
      if (pid > 0 && pid <= maxPid && safePid[pid]) triIsSafe[ti] = 1;
    }
#ifdef DEBUG
    size_t safeAreas = 0;
    for (unsigned pid = 1; pid <= maxPid; pid++) if (safePid[pid]) safeAreas++;
    size_t safeTris = 0;
    for (int ti = 0; ti < countTriangles; ti++) if (triIsSafe[ti]) safeTris++;
    printf("[safe] areas=%u safeAreas=%zu safeTris=%zu\n", maxPid, safeAreas, safeTris);
#endif
  }

  buildTloTriangles(tlo, triangles, triPoints, countPoints, triIsSafe);

  triangulation_free_polygons(polys, polyCount);
  memDestroy(2, (void*)safePid, (void*)triIsSafe);

  memDestroy(2, (void*)triangles, (void*)triPoints);

  return 1;
}

char initTlo(TloRender* tlo, const char* path, size_t* sizeTlo, int* countTlo) {
  if (!varifyTloData(path, sizeTlo, countTlo)) return 0;

  glGenVertexArrays(1, &tlo->vao_points);
  glGenVertexArrays(1, &tlo->vao_tri);
  
  glGenBuffers(1, &tlo->vbo_points);
  glGenBuffers(1, &tlo->vbo_tri);
  glGenBuffers(1, &tlo->ebo);
  glGenBuffers(1, &tlo->ebo_safe);

  tlo->totalPoints = tlo->countTriangles = tlo->countTrianglesSafe = 0;
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

  if (!triangTlo(tlo, vrt, tlo->totalPoints)) {
    free(vrt);
    return 0;
  }

  free(vrt);
  tlo->is_loaded = 1;

  printf("avg count poinst = %d\n", tlo->totalPoints / countTlo);

  return 1;
}

static void drawPoints(TloRender* tlo) {
  glBindVertexArray(tlo->vao_points);
  glDrawArrays(GL_POINTS, 0, tlo->totalPoints);
}

static void drawTriangles(TloRender* tlo, GLint u_wireOverrideLoc, GLint u_wireColorLoc) {
  glBindVertexArray(tlo->vao_tri);

  // base: depth prepass + wireframe
  if (u_wireOverrideLoc >= 0) glUniform1i(u_wireOverrideLoc, 0);

  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tlo->ebo);
  glDrawElements(GL_TRIANGLES, tlo->countTriangles * 3, GL_UNSIGNED_INT, 0);

  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthFunc(GL_LEQUAL);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glLineWidth(1.0f);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tlo->ebo);
  glDrawElements(GL_TRIANGLES, tlo->countTriangles * 3, GL_UNSIGNED_INT, 0);

  // overlay safe zones in blue
  if (tlo->countTrianglesSafe > 0 && u_wireOverrideLoc >= 0 && u_wireColorLoc >= 0) {
    glUniform1i(u_wireOverrideLoc, 1);
    glUniform3f(u_wireColorLoc, 0.20f, 0.45f, 1.00f);
    glLineWidth(1.6f);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tlo->ebo_safe);
    glDrawElements(GL_TRIANGLES, tlo->countTrianglesSafe * 3, GL_UNSIGNED_INT, 0);
    glLineWidth(1.0f);
    glUniform1i(u_wireOverrideLoc, 0);
  }

  glDepthFunc(GL_LESS);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

char drawTlo(TloRender* tlo, GLuint shaderProg) {
  if (!tlo->is_loaded || !tlo->totalPoints) {
    fprintf(stderr, "DATA TLO NOT LOADED OR NOT POINTS\n");
    return 0;
  }

  glUseProgram(shaderProg);

  GLint renderModeLoc = glGetUniformLocation(shaderProg, "u_renderMode");
  GLint wireOverrideLoc = glGetUniformLocation(shaderProg, "u_wireOverride");
  GLint wireColorLoc = glGetUniformLocation(shaderProg, "u_wireOverrideColor");

  glUniform1i(renderModeLoc, drawMode);
  
  if (drawMode == POINT_MODE || tlo->countTriangles <= 0) {
    drawPoints(tlo);
  } else {
    drawTriangles(tlo, wireOverrideLoc, wireColorLoc);
  }

  glBindVertexArray(0);

  return 1;
}

void freeTlo(TloRender* tlo) {
  if (tlo->vao_points) glDeleteVertexArrays(1, &tlo->vao_points);
  if (tlo->vao_tri) glDeleteVertexArrays(1, &tlo->vao_tri);
  if (tlo->vbo_points) glDeleteBuffers(1, &tlo->vbo_points);
  if (tlo->vbo_tri) glDeleteBuffers(1, &tlo->vbo_tri);
  if (tlo->ebo) glDeleteBuffers(1, &tlo->ebo);
  if (tlo->ebo_safe) glDeleteBuffers(1, &tlo->ebo_safe);
  tlo->totalPoints = tlo->countTriangles = 0;
  tlo->countTrianglesSafe = 0;
  tlo->is_loaded = 0;
}
