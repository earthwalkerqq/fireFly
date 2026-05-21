#ifndef TLO_H
#define TLO_H

#include <stdio.h>

#include <GL/glew.h>

typedef struct {
  GLuint vao_points, vbo_points;
  GLuint vao_tri, vbo_tri;
  GLuint ebo;                 // индексы всех треугольников триангуляции
  GLuint vao_zone, vbo_zone;  // безопасные зоны: заливка с цветом по рангу
  GLsizei totalPoints, countTriangles, countZoneVerts;
  char is_loaded;

  // границы облака точек (в мировых координатах)
  float minX, maxX;
  float minY, maxY;
  float minZ, maxZ;
} TloRender;

// Режимы отрисовки: 1 = облако точек, 2 = серая триангуляционная сетка,
//                   3 = сетка с безопасными зонами, окрашенными по рангу
#define TLO_RENDER_POINTS 1
#define TLO_RENDER_TRIANG 2
#define TLO_RENDER_ZONES  3

char initTlo(TloRender *tlo, const char *path, size_t *sizeTlo, int *countTlo);
char loadAllTloData(TloRender *tlo, const char *path, size_t sizeTlo,
                    int countTlo, const int *heights);
char drawTlo(TloRender* tlo, GLuint shaderProg);
void freeTlo(TloRender *tlo);

#endif
