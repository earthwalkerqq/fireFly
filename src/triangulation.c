#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cglm/cglm.h>

#include "triangulation.h"

#define EPS 1e-9

/* Триангуляция в плоскости (x, y): горизонталь рельефа — (X, Y), Z — высота */

// Сортировка по x, затем по y
int compare_points(const void *a, const void *b) {
  point_t *pa = (point_t *)a;
  point_t *pb = (point_t *)b;

  if (pa->x < pb->x)
    return -1;
  if (pa->x > pb->x)
    return 1;
  if (pa->y < pb->y)
    return -1;
  if (pa->y > pb->y)
    return 1;
  return 0;
}

double orient(point_t *a, point_t *b, point_t *c) {
  return (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
}

double dist2(point_t *a, point_t *b) {
  double dx = a->x - b->x, dy = a->y - b->y;
  return dx * dx + dy * dy;
}

int in_circle(point_t *a, point_t *b, point_t *c, point_t *d) {
  // Эквивалентный детерминант 3x3, но без матриц/cglm — быстрее и без аллокаций.
  // Все вычисления происходят в плоскости XY.
  double ax = a->x - d->x;
  double ay = a->y - d->y;
  double bx = b->x - d->x;
  double by = b->y - d->y;
  double cx = c->x - d->x;
  double cy = c->y - d->y;

  double det =
      (ax * ax + ay * ay) * (bx * cy - by * cx) -
      (bx * bx + by * by) * (ax * cy - ay * cx) +
      (cx * cx + cy * cy) * (ax * by - ay * bx);

  return det > EPS;
}

// Вычисление описанной окружности для треугольника (плоскость xy)
void compute_circumcircle(point_t *points, Triangle *tri) {
  point_t* p1 = points + tri->p1;
  point_t* p2 = points + tri->p2;
  point_t* p3 = points + tri->p3;

  double d = 2.0 * (p1->x * (p2->y - p3->y) +
                    p2->x * (p3->y - p1->y) +
                    p3->x * (p1->y - p2->y));

  if (fabs(d) < 1e-12) {
    tri->cx = tri->cy = 0;
    tri->r2 = INFINITY;
    return;
  }

  double ux =
      ( (p1->x * p1->x + p1->y * p1->y) * (p2->y - p3->y) +
        (p2->x * p2->x + p2->y * p2->y) * (p3->y - p1->y) +
        (p3->x * p3->x + p3->y * p3->y) * (p1->y - p2->y) ) / d;

  double uy =
      ( (p1->x * p1->x + p1->y * p1->y) * (p3->x - p2->x) +
        (p2->x * p2->x + p2->y * p2->y) * (p1->x - p3->x) +
        (p3->x * p3->x + p3->y * p3->y) * (p2->x - p1->x) ) / d;

  tri->cx = ux;
  tri->cy = uy;
  tri->r2 = dist2(p1, &(point_t){ux, uy, 0, 0, 0});
}

// Проверка, является ли ребро локально оптимальным (условие Делоне)
int isLocalOptimalEdge(point_t *points, Triangle *tri1, Triangle *tri2,
                       int edge_p1, int edge_p2) {
  // Находим вершины, не принадлежащие общему ребру
  int opp1 = -1, opp2 = -1;

  // В tri1 ищем вершину, не входящую в ребро
  if (tri1->p1 != edge_p1 && tri1->p1 != edge_p2)
    opp1 = tri1->p1;
  else if (tri1->p2 != edge_p1 && tri1->p2 != edge_p2)
    opp1 = tri1->p2;
  else if (tri1->p3 != edge_p1 && tri1->p3 != edge_p2)
    opp1 = tri1->p3;

  // В tri2 ищем вершину, не входящую в ребро
  if (tri2->p1 != edge_p1 && tri2->p1 != edge_p2)
    opp2 = tri2->p1;
  else if (tri2->p2 != edge_p1 && tri2->p2 != edge_p2)
    opp2 = tri2->p2;
  else if (tri2->p3 != edge_p1 && tri2->p3 != edge_p2)
    opp2 = tri2->p3;

  if (opp1 == -1 || opp2 == -1)
    return 1; // Не нашли противоположные вершины

  // Проверяем, не попадает ли opp2 в окружность треугольника с opp1
  return !in_circle(&points[tri1->p1], &points[tri1->p2], &points[tri1->p3],
                    &points[opp2]);
}

// Флип ребра (перестроение двух треугольников)
void flipEdge(Triangle *tri1, Triangle *tri2, int edge_p1, int edge_p2) {
  // Находим четвертую вершину (противоположную ребру в каждом треугольнике)
  int opp1 = -1, opp2 = -1;

  // Ищем вершину, не входящую в ребро в tri1
  if (tri1->p1 != edge_p1 && tri1->p1 != edge_p2)
    opp1 = tri1->p1;
  else if (tri1->p2 != edge_p1 && tri1->p2 != edge_p2)
    opp1 = tri1->p2;
  else if (tri1->p3 != edge_p1 && tri1->p3 != edge_p2)
    opp1 = tri1->p3;

  // Ищем вершину, не входящую в ребро в tri2
  if (tri2->p1 != edge_p1 && tri2->p1 != edge_p2)
    opp2 = tri2->p1;
  else if (tri2->p2 != edge_p1 && tri2->p2 != edge_p2)
    opp2 = tri2->p2;
  else if (tri2->p3 != edge_p1 && tri2->p3 != edge_p2)
    opp2 = tri2->p3;

  if (opp1 == -1 || opp2 == -1)
    return;

  // Перестраиваем треугольники: (a,c,b) и (a,b,d) -> (a,c,d) и (b,c,d)
  tri1->p1 = edge_p1;
  tri1->p2 = opp1;
  tri1->p3 = opp2;

  tri2->p1 = edge_p2;
  tri2->p2 = opp1;
  tri2->p3 = opp2;
}

static char isContainEdge(Triangle* t, Edge* e, int* v) {
    char res = 0;
    int vert[3] = {t->p1, t->p2, t->p3};

    for (int i = 0; i < 3 && !res; i++) {
      int secondEdge;
      if (vert[i] == e->p1) {
        secondEdge = e->p2;
      } else if (vert[i] == e->p2) {
        secondEdge = e->p1;
      } else continue;

      for (int j = (i + 1) % 3, k = 0; k < 2; j = (j + 1) % 3, k++) {
        if (vert[j] == secondEdge) {
          *v = vert[(j + 1) % 3];
          res = 1;
          break;
        }
      }
    }

  return res;
}

// Рекурсивное построение триангуляции Делоне методом "разделяй и властвуй"
void build_delaunay(point_t *points, int start, int end, Triangle **triangles,
                    int *num_triangles) {
  int n = end - start;

  if (n <= 2)
    return;

  if (n == 3) {
    // Три точки - создаем треугольник
    *triangles = realloc(*triangles, (*num_triangles + 1) * sizeof(Triangle));
    Triangle *tri = &(*triangles)[*num_triangles];
    tri->p1 = start;
    tri->p2 = start + 1;
    tri->p3 = start + 2;

    // Проверяем ориентацию
    if (orient(&points[start], &points[start + 1], &points[start + 2]) < 0) {
      // Меняем порядок для положительной ориентации
      tri->p2 = start + 2;
      tri->p3 = start + 1;
    }

    compute_circumcircle(points, tri);
    (*num_triangles)++;
    return;
  }

  // Разделяем точки пополам
  int mid = start + n / 2;

  // Рекурсивно строим триангуляцию для левой и правой половин
  Triangle *left_tri = NULL;
  Triangle *right_tri = NULL;
  int num_left = 0, num_right = 0;

  build_delaunay(points, start, mid, &left_tri, &num_left);
  build_delaunay(points, mid, end, &right_tri, &num_right);

  // Находим нижнюю общую касательную (в плоскости xy: min y, затем x)
  int left_lower = start;
  int right_lower = mid;

  for (int i = start + 1; i < mid; i++) {
    if (points[i].y < points[left_lower].y ||
        (points[i].y == points[left_lower].y &&
        points[i].x < points[left_lower].x)) {
        left_lower = i;
    }
  }

  for (int i = mid + 1; i < end; i++) {
    if (points[i].y < points[right_lower].y ||
        (points[i].y == points[right_lower].y &&
         points[i].x < points[right_lower].x)) {
        right_lower = i;
    }
  }

  // Находим нижнюю касательную LR
  int l = left_lower, r = right_lower;
  int changed;

  do {
    changed = 0;
    int next;
    // Поворачиваем по часовой стрелке с левой стороны
    char flag = 0;
    for (int i = 0; i < num_left && !flag; i++) {
        Triangle *t = &left_tri[i];
        // Проверяем, содержит ли треугольник точку l
        if (t->p1 == l || t->p2 == l || t->p3 == l) {
            // Находим следующую вершину в треугольнике
            int vert[3] = {t->p1, t->p2, t->p3};
            for (int j = 0; j < 3; j++) {
                if (l == vert[j]) continue;
                next = vert[j];
                // Если точка next справа от линии l-r, обновляем l
                if (orient(&points[l], &points[r], &points[next]) > 0) {
                    l = next;
                    changed = 1;
                    flag = 1;
                }
            }
        }
    }

    for (int i = 0; i < num_left && !flag; i++) {
      Triangle *t = &left_tri[i];
      // Проверяем, содержит ли треугольник точку l
      if (t->p1 == l || t->p2 == l || t->p3 == l) {
        // Находим следующую вершину в треугольнике
        int vert[3] = {t->p1, t->p2, t->p3};
        for (int j = 0; j < 3; j++) {
          if (l == vert[j])
            continue;
          next = vert[j];
          // Если точка next справа от линии l-r, обновляем l
          if (orient(&points[l], &points[r], &points[next]) > 0) {
            l = next;
            changed = 1;
            flag = 1;
          }
        }
      }
    }
  } while (changed);

  // Теперь у нас есть нижняя касательная (l, r)
  // Начинаем слияние

  // Создаем временный массив для хранения всех треугольников
  Triangle *merged = malloc((num_left + num_right + n) * sizeof(Triangle));
  int num_merged = 0;

  // Копируем левые и правые треугольники
  memcpy(merged, left_tri, num_left * sizeof(Triangle));
  num_merged += num_left;
  memcpy(merged + num_merged, right_tri, num_right * sizeof(Triangle));
  num_merged += num_right;

  free(left_tri);
  free(right_tri);

  // Стек для активных ребер
  Edge *stack = malloc(n * sizeof(Edge));
  int stack_size = 0;

  // Добавляем нижнюю касательную
  stack[stack_size].p1 = l;
  stack[stack_size++].p2 = r;

  double max_angle = -1e9;
  // Основной цикл слияния
  while (stack_size > 0) {
    Edge e = stack[stack_size - 1];
    stack_size--;

    // Ищем треугольник слева от ребра
    int left_candidate = -1;
    // Для максимального угла достаточно минимизировать cos(angle),
    // избавляясь от вызова acos().
    // double best_left_cos = 1.0; // cos(angle) ∈ [-1,1], ищем минимум

    Triangle *t1 = NULL, *t2 = NULL;

    int third;
    for (int i = 0; i < num_merged; i++) {
      // Проверяем, содержит ли треугольник ребро e
      // Находим третью вершину
      if (isContainEdge(&merged[i], &e, &third)) {
        // Проверяем, что точка третья находится слева от ребра
        if (orient(&points[e.p1], &points[e.p2], &points[third]) > 0) {
          // Вычисляем угол (плоскость xy)
          double dx1 = points[third].x - points[e.p1].x;
          double dy1 = points[third].y - points[e.p1].y;
          double dx2 = points[third].x - points[e.p2].x;
          double dy2 = points[third].y - points[e.p2].y;

          double dot = dx1 * dx2 + dy1 * dy2;
          double len1 = sqrt(dx1 * dx1 + dy1 * dy1);
          double len2 = sqrt(dx2 * dx2 + dy2 * dy2);

          if (len1 > 1e-12 && len2 > 1e-12) {
            double angle = acos(dot / (len1 * len2));
            if (angle > max_angle) {
              max_angle = angle;
              left_candidate = third;
              t1 = &merged[i];
            }
          }
        }
      }
    }

    if (left_candidate >= 0) {
      // Ищем правого кандидата
      int right_candidate = -1;
      double best_right_cos = 1.0;

      for (int i = 0; i < num_merged; i++) {
        if (isContainEdge(&merged[i], &e, &third)) {

            if (orient(&points[e.p1], &points[e.p2], &points[third]) < 0) {
              double dx1 = points[third].x - points[e.p1].x;
              double dy1 = points[third].y - points[e.p1].y;
              double dx2 = points[third].x - points[e.p2].x;
              double dy2 = points[third].y - points[e.p2].y;

              double dot = dx1 * dx2 + dy1 * dy2;
              double len1 = sqrt(dx1 * dx1 + dy1 * dy1);
              double len2 = sqrt(dx2 * dx2 + dy2 * dy2);

              if (len1 > 1e-12 && len2 > 1e-12) {
                double cosang = dot / (len1 * len2);
                if (cosang < best_right_cos) {
                  best_right_cos = cosang;
                  right_candidate = third;
                  t2 = &merged[i];
                }
              }
            }
          }
      }

      if (right_candidate >= 0) {
        // Проверяем условие Делоне
        point_t *p1 = &points[e.p1];
        point_t *p2 = &points[e.p2];
        point_t *pc = &points[left_candidate];
        point_t *pd = &points[right_candidate];

        if (in_circle(p1, p2, pc, pd)) {
          // Нужен флип
          // Находим треугольники, содержащие ребро
          if (t1 && t2) {
            // Флипаем ребро
            flipEdge(t1, t2, e.p1, e.p2);
            compute_circumcircle(points, t1);
            compute_circumcircle(points, t2);

            // Добавляем новые ребра в стек
            stack[stack_size].p1 = e.p1;
            stack[stack_size].p2 = left_candidate;
            stack_size++;

            stack[stack_size].p1 = e.p2;
            stack[stack_size].p2 = left_candidate;
            stack_size++;

            stack[stack_size].p1 = e.p1;
            stack[stack_size].p2 = right_candidate;
            stack_size++;

            stack[stack_size].p1 = e.p2;
            stack[stack_size].p2 = right_candidate;
            stack_size++;
          }
        } else {
          // Создаем новый треугольник
          merged = realloc(merged, (num_merged + 1) * sizeof(Triangle));
          Triangle *new_tri = &merged[num_merged];
          new_tri->p1 = e.p1;
          new_tri->p2 = e.p2;
          new_tri->p3 = left_candidate;

          if (orient(&points[e.p1], &points[e.p2], &points[left_candidate]) <
              0) {
            new_tri->p2 = left_candidate;
            new_tri->p3 = e.p2;
          }

          compute_circumcircle(points, new_tri);
          num_merged++;

          // Добавляем новые ребра в стек
          stack[stack_size].p1 = e.p1;
          stack[stack_size].p2 = left_candidate;
          stack_size++;

          stack[stack_size].p1 = e.p2;
          stack[stack_size].p2 = left_candidate;
          stack_size++;
        }
      }
    }
  }

  free(stack);

  // Обновляем выходные параметры
  *triangles = merged;
  *num_triangles = num_merged;
}

// Функция для построения триангуляции Делоне из массива точек
Triangle *delaunay_triangulation(point_t *points, int num_points,
                                 int *num_triangles) {
  if (num_points < 3) {
    *num_triangles = 0;
    return NULL;
  }

  // Сортируем точки по x, затем по y
  qsort(points, num_points, sizeof(point_t), compare_points);

  // Удаляем дубликаты (плоскость xy)
  int unique_count = 1;
  for (int i = 1; i < num_points; i++) {
    if (points[i].x != points[i - 1].x || points[i].y != points[i - 1].y) {
      points[unique_count++] = points[i];
    }
  }
  num_points = unique_count;

  // Строим триангуляцию
  Triangle *triangles = NULL;

  build_delaunay(points, 0, num_points, &triangles, num_triangles);

  return triangles;
}