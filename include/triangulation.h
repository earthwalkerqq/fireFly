#ifndef TRIANGULATION_H
#define TRIANGULATION_H

#include <stdio.h>

// Структура для точки (x,y — для триангуляции, z/height — для рендера)
typedef struct {
  float x, y, z;
  float height;
  int id;
} point_t;

// Структура для треугольника
typedef struct {
  int p1, p2, p3; // индексы вершин
  unsigned numPolygon;
} Triangle;

typedef struct {
  unsigned numPolygon; // идентификатор области, которой принадлежит контур
  int* verts; // вершины из point_t (point_t* points)
  size_t cntVerts;
} polygon_t;

// Предикат корректности треугольника (например, из findSafe.c)
typedef int (*tri_correct_fn)(const Triangle* tri, const point_t* points);

/**
 * @brief Построение триангуляции Делоне (алгоритм Боуэра-Ватсона)
 * @param points массив точек (модифицируется: сортировка, удаление дубликатов)
 * @param num_points количество точек (вход); после вызова — число уникальных
 * @param num_triangles выход: количество треугольников
 * @return массив треугольников (индексы вершин 0..*num_points-1 в points)
 */
Triangle *delaunay_triangulation(point_t *points, int *num_points,
                                 int *num_triangles);

/**
 * @brief Найти связные области корректных треугольников и их контуры.
 * @details
 * - всем корректным треугольникам проставляется tri[i].numPolygon = 1..K
 * - некорректным проставляется 0
 * - возвращается массив контуров (по индексам вершин points), один или несколько на область
 *
 * @param tri массив треугольников
 * @param countTriangle количество треугольников
 * @param points массив точек
 * @param countPoints количество точек
 * @param is_correct предикат корректности треугольника
 * @param outPolys выход: массив контуров (нужно освобождать через triangulation_free_polygons)
 * @return количество контуров в outPolys
 */
size_t triangulation_find_polygons(Triangle* tri, size_t countTriangle,
                                   const point_t* points, size_t countPoints,
                                   tri_correct_fn is_correct,
                                   polygon_t** outPolys);

void triangulation_free_polygons(polygon_t* polys, size_t countPolys);

#endif
