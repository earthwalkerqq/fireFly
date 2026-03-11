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

static void buildTloTriangles(TloRender *tlo, Triangle* tri, point_t* vrt) {
  if (!tlo->vao_tri || !tlo->ebo || !tlo->vbo_points)
    return;

  glBindVertexArray(tlo->vao_tri);

  // Используем тот же VBO с вершинами, что и для облака точек
  glBindBuffer(GL_ARRAY_BUFFER, tlo->vbo_points);

  unsigned indices[tlo->countTriangles * 3];

  for (int i = 0; i < tlo->countTriangles; i++) {
    indices[i * 3]     = (unsigned)vrt[tri[i].p1].id;
    indices[i * 3 + 1] = (unsigned)vrt[tri[i].p2].id;
    indices[i * 3 + 2] = (unsigned)vrt[tri[i].p3].id;
  }

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tlo->ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

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

  // Делаем копию массива точек для триангуляции.
  // Важно: исходный массив `vrt` остаётся НЕотсортированным и используется
  // напрямую как вершинный буфер, а в треугольниках мы храним индексы
  // исходных вершин через поле `id`.
  point_t* triPoints = (point_t*)malloc(countPoints * sizeof(point_t));
  if (!triPoints) {
    fprintf(stderr, "FAILED FROM MEMORY ALLOCATE (triPoints)\n");
    return 0;
  }

  memcpy(triPoints, vrt, countPoints * sizeof(point_t));

  Triangle *triangles =
      delaunay_triangulation(triPoints, countPoints, &countTriangles);

  if (!triangles || countTriangles <= 0) {
    fprintf(stderr, "FAILED FROM TRIANGULATION\n");
    if (triangles) free(triangles);
    free(triPoints);
    return 0;
  }

  tlo->countTriangles = (GLsizei)countTriangles;

  // Заполняем VBO исходными точками (в их "родном" порядке),
  // а индексы треугольников берём по полю `id` из массива triPoints.
  buildTloPoints(tlo, vrt);
  buildTloTriangles(tlo, triangles, triPoints);

  free(triangles);
  free(triPoints);

  return 1;
}

char initTlo(TloRender* tlo, const char* path, size_t* sizeTlo, int* countTlo) {
  if (!varifyTloData(path, sizeTlo, countTlo)) return 0;

  glGenVertexArrays(1, &tlo->vao_points);
  glGenVertexArrays(1, &tlo->vao_tri);
  
  glGenBuffers(1, &tlo->vbo_points);
  glGenBuffers(1, &tlo->vbo_tri);
  glGenBuffers(1, &tlo->ebo);

  tlo->totalPoints = tlo->countTriangles = 0;
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

    double* pts = (double*)malloc(countPoints * 3 * sizeof(double));
    if (!pts) {
      fprintf(stderr, "FAILED FROM MEMORY ALLOCATE\n");
      return 0;
    }

    if (!getTloPoints(path, TLO_TILES[i], pts, countPoints)) {
      free(pts);
      continue;
    }

    for (int j = 0; j < countPoints; j++) {
      double x = pts[j * 3], y = pts[j * 3 + 1], z = pts[j * 3 + 2];
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
    free(pts);
  }
  tlo->totalPoints = fragNumber;
  triangTlo(tlo, vrt, tlo->totalPoints);
  free(vrt);
  tlo->is_loaded = 1;
  return 1;
}

static void drawPoints(TloRender* tlo) {
  glBindVertexArray(tlo->vao_points);
  glDrawArrays(GL_POINTS, 0, tlo->totalPoints);
}

static void drawTriangles(TloRender* tlo) {
  glBindVertexArray(tlo->vao_tri);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glDrawElements(GL_TRIANGLES, tlo->countTriangles * 3, GL_UNSIGNED_INT, 0);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

char drawTlo(TloRender* tlo, GLuint shaderProg) {
  if (!tlo->is_loaded || !tlo->totalPoints) {
    fprintf(stderr, "DATA TLO NOT LOADED OR NOT POINTS\n");
    return 0;
  }

  glUseProgram(shaderProg);

  GLint renderModeLoc = glGetUniformLocation(shaderProg, "u_renderMode");

  glUniform1i(renderModeLoc, drawMode);
  
  if (drawMode == POINT_MODE) {
    drawPoints(tlo);  
  } else {
    drawTriangles(tlo);
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
  tlo->totalPoints = tlo->countTriangles = 0;
  tlo->is_loaded = 0;
}
