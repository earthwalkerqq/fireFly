#include "findSafe.h"
#include <math.h>

static double min_edge_len2(const Triangle* tri, const point_t* points) {
  const point_t* p1 = points + tri->p1;
  const point_t* p2 = points + tri->p2;
  const point_t* p3 = points + tri->p3;

  double d2_12 = (p1->x - p2->x) * (p1->x - p2->x) + (p1->y - p2->y) * (p1->y - p2->y);
  double d2_13 = (p1->x - p3->x) * (p1->x - p3->x) + (p1->y - p3->y) * (p1->y - p3->y);
  double d2_23 = (p2->x - p3->x) * (p2->x - p3->x) + (p2->y - p3->y) * (p2->y - p3->y);

  double m = d2_12;
  if (d2_13 < m) m = d2_13;
  if (d2_23 < m) m = d2_23;
  return m;
}

// Предикат корректности треугольника
static int isCorrectTriangle_cb(const Triangle* tri, const point_t* points) {
  float h[3] = {
    points[tri->p1].height,
    points[tri->p2].height,
    points[tri->p3].height
  };

  for (int i = 0; i < 3; i++) {
    if (h[i] > MAX_HEIGTH) return 0;
  }

  double maxH = h[0], minH = h[0];
  for (int i = 1; i < 3; i++) {
    if (h[i] > maxH) maxH = h[i];
    if (h[i] < minH) minH = h[i];
  }

  double minD2 = min_edge_len2(tri, points);
  if (minD2 <= 0.0) return 0;
  return ((maxH - minH) / sqrt(minD2) < ANGLE_SLOPE) ? 1 : 0;
}

void varifyTriangles(Triangle* tri, point_t* points, size_t size, TloRender* tlo) {
  (void)tlo;
  if (!tri || !points || size == 0) return;

  polygon_t* polys = NULL;
  // points count is taken from tlo when wired; for now assume ids are 0..N-1 in points array
  // Caller should pass correct countPoints when using triangulation_find_polygons directly.
  (void)triangulation_find_polygons(tri, size, points, (size_t)0, isCorrectTriangle_cb, &polys);
  // This function is not wired into rendering yet; avoid leaks if called.
  if (polys) {
    // count unknown here because countPoints=0 => returns 0; keep safe
    triangulation_free_polygons(polys, 0);
  }
}