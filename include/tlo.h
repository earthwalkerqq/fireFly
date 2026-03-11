#ifndef TLO_H
#define TLO_H

#include <stdio.h>

#include <GL/glew.h>

typedef struct {
  GLuint vao_points, vbo_points;
  GLuint vao_tri, vbo_tri;
  GLuint ebo; // для триангуляции
  GLsizei totalPoints, countTriangles;
  char is_loaded;

  // границы облака точек (в мировых координатах)
  float minX, maxX;
  float minY, maxY;
  float minZ, maxZ;
} TloRender;

// Режим отрисовки: 1 = облако точек (цвет по высоте), 2 = серая триангуляция
#define TLO_RENDER_POINTS 1
#define TLO_RENDER_TRIANG 2

char initTlo(TloRender *tlo, const char *path, size_t *sizeTlo, int *countTlo);
char loadAllTloData(TloRender *tlo, const char *path, size_t sizeTlo,
                    int countTlo, const int *heights);
char drawTlo(TloRender* tlo, GLuint shaderProg);
void freeTlo(TloRender *tlo);

#endif
