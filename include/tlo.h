#ifndef TLO_H
#define TLO_H

#include <stdio.h>

#include <GL/glew.h>

#include "triangulation.h"

typedef struct {
  GLuint vao_points, vbo_points;
  GLuint vao_tri, vbo_tri;
  GLuint ebo;                 // индексы всех треугольников триангуляции
  GLuint vao_zone, vbo_zone;  // безопасные зоны: заливка с цветом по рангу
  GLuint vao_circles, vbo_circles; // окружности радиуса ЛА в центрах зон
  GLsizei totalPoints, countTriangles, countZoneVerts, countCircleVerts;
  char is_loaded;

  // сохранённое облако точек — нужно для пересборки сетки с другой густотой
  point_t* cloud;
  int      cloudCount;

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

/* Запускает фоновую (неблокирующую) пересборку триангуляции из сохранённого
 * облака точек с текущей густотой сетки (см. triangulation_set_max_points).
 * Тяжёлое вычисление выполняется в отдельном потоке — главный поток не
 * блокируется и продолжает рендеринг и обработку управления камерой.
 * Возвращает 1, если пересборка запущена, 0 — если она уже выполняется. */
char rebuildTloTriangulation(TloRender *tlo);

/* Проверяет завершение фоновой пересборки и, если она готова, загружает
 * результат в видеопамять. Вызывается каждый кадр из главного потока. */
void pollTloRebuild(TloRender *tlo);

/* Возвращает ненулевое значение, если фоновая пересборка ещё выполняется. */
int tloRebuildInProgress(void);

#endif
