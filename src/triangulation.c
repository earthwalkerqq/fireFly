#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "triangulation.h"

#define MAX_TRIANG_POINTS 12000
#define TRI_MAX_WORKERS 8
#define TRI_PARALLEL_MIN_ITEMS 512
#define POINT_EPS 1e-6
#define AREA_EPS 1e-12
#define CIRCLE_EPS 1e-9

#ifndef INFINITY
#define INFINITY 1e30
#endif

/**
 * @brief Внутреннее представление треугольника Bowyer-Watson.
 */
typedef struct {
  int p1, p2, p3;
  double cx, cy, r2;
  unsigned char alive;
} bw_triangle_t;

/**
 * @brief Ненаправленное ребро границы "дыры" при вставке точки.
 */
typedef struct {
  int a, b;
} bw_edge_t;

/**
 * @brief Аргументы потока для проверки описанных окружностей.
 */
typedef struct {
  const bw_triangle_t* triangles;
  const point_t* points;
  const point_t* point;
  unsigned char* bad;
  size_t begin;
  size_t end;
} circle_worker_t;

/**
 * @brief Аргументы потока для применения tri_correct_fn.
 */
typedef struct {
  Triangle* triangles;
  unsigned char* good;
  const point_t* points;
  tri_correct_fn is_correct;
  size_t begin;
  size_t end;
} good_worker_t;

typedef struct {
  int a, b; // a < b
  int tri;
  int edge; // 0..2
  int used;
} edge_item_t;

/**
 * @brief Вычисляет ориентированную удвоенную площадь треугольника в XY.
 */
static double orient2d(const point_t* a, const point_t* b, const point_t* c) {
  return ((double)b->x - (double)a->x) * ((double)c->y - (double)a->y) -
         ((double)b->y - (double)a->y) * ((double)c->x - (double)a->x);
}

/**
 * @brief Сравнивает точки по XY для qsort.
 */
static int cmp_point_xy(const void* lhs, const void* rhs) {
  const point_t* a = (const point_t*)lhs;
  const point_t* b = (const point_t*)rhs;
  if (a->x < b->x) return -1;
  if (a->x > b->x) return 1;
  if (a->y < b->y) return -1;
  if (a->y > b->y) return 1;
  return 0;
}

/**
 * @brief Проверяет, совпадают ли две точки по XY с учетом допуска.
 */
static int same_point_xy(const point_t* a, const point_t* b) {
  return fabs((double)a->x - (double)b->x) <= POINT_EPS &&
         fabs((double)a->y - (double)b->y) <= POINT_EPS;
}

/**
 * @brief Возвращает рекомендуемое число потоков для заданного объема работы.
 * @details
 * По умолчанию используется число доступных CPU, но не больше TRI_MAX_WORKERS.
 * Для отладки можно задать переменную окружения FIREFLY_TRI_THREADS.
 */
static int triangulation_worker_count(size_t work_items) {
  if (work_items < TRI_PARALLEL_MIN_ITEMS)
    return 1;

  long requested = 0;
  const char* env = getenv("FIREFLY_TRI_THREADS");
  if (env && *env)
    requested = strtol(env, NULL, 10);

  if (requested <= 0) {
#ifdef _SC_NPROCESSORS_ONLN
    requested = sysconf(_SC_NPROCESSORS_ONLN);
#else
    requested = 1;
#endif
  }

  if (requested < 1) requested = 1;
  if (requested > TRI_MAX_WORKERS) requested = TRI_MAX_WORKERS;

  size_t by_work = work_items / TRI_PARALLEL_MIN_ITEMS;
  if (by_work < 1) by_work = 1;
  if ((size_t)requested > by_work) requested = (long)by_work;

  return (int)requested;
}

/**
 * @brief Прореживает большое облако точек до MAX_TRIANG_POINTS.
 * @details
 * Область разбивается на равномерную сетку; из каждой занятой ячейки остается
 * точка, ближайшая к центру ячейки. Это сохраняет покрытие площади и удерживает
 * Bowyer-Watson в приемлемой сложности для интерактивной отрисовки.
 */
static int subsample(point_t* points, int n) {
  if (n <= MAX_TRIANG_POINTS)
    return n;

  double min_x = points[0].x, max_x = points[0].x;
  double min_y = points[0].y, max_y = points[0].y;
  for (int i = 1; i < n; i++) {
    if (points[i].x < min_x) min_x = points[i].x;
    else if (points[i].x > max_x) max_x = points[i].x;
    if (points[i].y < min_y) min_y = points[i].y;
    else if (points[i].y > max_y) max_y = points[i].y;
  }

  int g = (int)ceil(sqrt((double)MAX_TRIANG_POINTS));
  if (g < 3) g = 3;

  int* best = (int*)calloc((size_t)(g * g), sizeof(int));
  double* d2 = (double*)malloc((size_t)(g * g) * sizeof(double));
  if (!best || !d2) {
    fprintf(stderr, "FAIL FROM MEMORY ALLOCATE\n");
    free(best);
    free(d2);
    return n;
  }

  for (int i = 0; i < g * g; i++)
    d2[i] = INFINITY;

  double sx = (max_x > min_x) ? (max_x - min_x) / g : 1.0;
  double sy = (max_y > min_y) ? (max_y - min_y) / g : 1.0;

  for (int i = 0; i < n; i++) {
    int gx = (int)(((double)points[i].x - min_x) / sx);
    int gy = (int)(((double)points[i].y - min_y) / sy);

    if (gx >= g) gx = g - 1;
    else if (gx < 0) gx = 0;
    if (gy >= g) gy = g - 1;
    else if (gy < 0) gy = 0;

    int idx = gy * g + gx;
    double cx = min_x + (gx + 0.5) * sx;
    double cy = min_y + (gy + 0.5) * sy;
    double dx = (double)points[i].x - cx;
    double dy = (double)points[i].y - cy;
    double dist = dx * dx + dy * dy;

    if (dist < d2[idx]) {
      d2[idx] = dist;
      best[idx] = i;
    }
  }

  point_t* tmp = (point_t*)malloc((size_t)(g * g) * sizeof(point_t));
  if (!tmp) {
    fprintf(stderr, "FAIL FROM MEMORY ALLOCATE\n");
    free(best);
    free(d2);
    return n;
  }

  int out = 0;
  for (int i = 0; i < g * g; i++) {
    if (d2[i] < INFINITY)
      tmp[out++] = points[best[i]];
  }

  free(best);
  free(d2);

  if (out >= 3) {
    memcpy(points, tmp, (size_t)out * sizeof(point_t));
    free(tmp);
    return out;
  }

  free(tmp);
  return n;
}

/**
 * @brief Сортирует точки и удаляет XY-дубликаты.
 */
static int normalize_points(point_t* points, int n) {
  n = subsample(points, n);
  if (n < 3)
    return n;

  qsort(points, (size_t)n, sizeof(point_t), cmp_point_xy);

  int out = 1;
  for (int i = 1; i < n; i++) {
    if (!same_point_xy(&points[out - 1], &points[i]))
      points[out++] = points[i];
  }

  return out;
}

/**
 * @brief Вычисляет окружность, описанную вокруг треугольника.
 */
static int compute_circumcircle(const point_t* points, int a, int b, int c,
                                double* cx, double* cy, double* r2) {
  const point_t* p1 = &points[a];
  const point_t* p2 = &points[b];
  const point_t* p3 = &points[c];

  double ax = p1->x, ay = p1->y;
  double bx = p2->x, by = p2->y;
  double cxp = p3->x, cyp = p3->y;

  double d = 2.0 * (ax * (by - cyp) + bx * (cyp - ay) + cxp * (ay - by));
  if (fabs(d) <= AREA_EPS)
    return 0;

  double ax2ay2 = ax * ax + ay * ay;
  double bx2by2 = bx * bx + by * by;
  double cx2cy2 = cxp * cxp + cyp * cyp;

  *cx = (ax2ay2 * (by - cyp) + bx2by2 * (cyp - ay) +
         cx2cy2 * (ay - by)) / d;
  *cy = (ax2ay2 * (cxp - bx) + bx2by2 * (ax - cxp) +
         cx2cy2 * (bx - ax)) / d;

  double dx = *cx - ax;
  double dy = *cy - ay;
  *r2 = dx * dx + dy * dy;
  return *r2 > AREA_EPS;
}

/**
 * @brief Расширяет массив внутренних треугольников.
 */
static int reserve_bw_triangles(bw_triangle_t** triangles, size_t* cap,
                                size_t need) {
  if (need <= *cap)
    return 1;

  size_t new_cap = *cap ? *cap : 64;
  while (new_cap < need)
    new_cap *= 2;

  bw_triangle_t* resized =
      (bw_triangle_t*)realloc(*triangles, new_cap * sizeof(bw_triangle_t));
  if (!resized)
    return 0;

  *triangles = resized;
  *cap = new_cap;
  return 1;
}

/**
 * @brief Добавляет внутренний треугольник с CCW-ориентацией.
 */
static int append_bw_triangle(bw_triangle_t** triangles, size_t* count,
                              size_t* cap, const point_t* points,
                              int a, int b, int c) {
  double area = orient2d(&points[a], &points[b], &points[c]);
  if (fabs(area) <= AREA_EPS)
    return 1;

  if (area < 0.0) {
    int tmp = b;
    b = c;
    c = tmp;
  }

  double cx = 0.0, cy = 0.0, r2 = 0.0;
  if (!compute_circumcircle(points, a, b, c, &cx, &cy, &r2))
    return 1;

  if (!reserve_bw_triangles(triangles, cap, *count + 1))
    return 0;

  (*triangles)[*count] = (bw_triangle_t){
    .p1 = a, .p2 = b, .p3 = c,
    .cx = cx, .cy = cy, .r2 = r2,
    .alive = 1
  };
  (*count)++;
  return 1;
}

/**
 * @brief Проверяет попадание точки внутрь описанной окружности треугольника.
 */
static int point_in_circumcircle(const bw_triangle_t* tri,
                                 const point_t* point) {
  if (!tri->alive)
    return 0;

  double dx = (double)point->x - tri->cx;
  double dy = (double)point->y - tri->cy;
  double d2 = dx * dx + dy * dy;
  double eps = fmax(1.0, tri->r2) * CIRCLE_EPS;
  return d2 <= tri->r2 + eps;
}

/**
 * @brief Рабочая функция потока для поиска плохих треугольников.
 */
static void* mark_bad_worker(void* arg) {
  circle_worker_t* job = (circle_worker_t*)arg;
  (void)job->points;
  for (size_t i = job->begin; i < job->end; i++)
    job->bad[i] = (unsigned char)point_in_circumcircle(&job->triangles[i],
                                                       job->point);
  return NULL;
}

/**
 * @brief Параллельно помечает треугольники, нарушенные новой точкой.
 */
static void mark_bad_triangles(const bw_triangle_t* triangles, size_t count,
                               const point_t* points, const point_t* point,
                               unsigned char* bad) {
  int workers = triangulation_worker_count(count);
  if (workers <= 1) {
    circle_worker_t job = {
      .triangles = triangles,
      .points = points,
      .point = point,
      .bad = bad,
      .begin = 0,
      .end = count
    };
    (void)mark_bad_worker(&job);
    return;
  }

  pthread_t threads[TRI_MAX_WORKERS];
  circle_worker_t jobs[TRI_MAX_WORKERS];
  size_t chunk = (count + (size_t)workers - 1) / (size_t)workers;
  int created = 0;

  for (int i = 0; i < workers; i++) {
    size_t begin = (size_t)i * chunk;
    size_t end = begin + chunk;
    if (begin >= count) break;
    if (end > count) end = count;

    jobs[i] = (circle_worker_t){
      .triangles = triangles,
      .points = points,
      .point = point,
      .bad = bad,
      .begin = begin,
      .end = end
    };

    if (pthread_create(&threads[i], NULL, mark_bad_worker, &jobs[i]) != 0)
      break;
    created++;
  }

  if (created != workers) {
    for (int i = 0; i < created; i++)
      pthread_join(threads[i], NULL);
    circle_worker_t job = {
      .triangles = triangles,
      .points = points,
      .point = point,
      .bad = bad,
      .begin = 0,
      .end = count
    };
    (void)mark_bad_worker(&job);
    return;
  }

  for (int i = 0; i < created; i++)
    pthread_join(threads[i], NULL);
}

/**
 * @brief Добавляет ребро в границу или удаляет его, если оно уже внутреннее.
 */
static int toggle_boundary_edge(bw_edge_t** edges, size_t* count, size_t* cap,
                                int a, int b) {
  if (a == b)
    return 1;
  if (a > b) {
    int tmp = a;
    a = b;
    b = tmp;
  }

  for (size_t i = 0; i < *count; i++) {
    if ((*edges)[i].a == a && (*edges)[i].b == b) {
      (*edges)[i] = (*edges)[*count - 1];
      (*count)--;
      return 1;
    }
  }

  if (*count == *cap) {
    size_t new_cap = *cap ? *cap * 2 : 64;
    bw_edge_t* resized = (bw_edge_t*)realloc(*edges,
                                             new_cap * sizeof(bw_edge_t));
    if (!resized)
      return 0;
    *edges = resized;
    *cap = new_cap;
  }

  (*edges)[*count] = (bw_edge_t){ .a = a, .b = b };
  (*count)++;
  return 1;
}

/**
 * @brief Удаляет из рабочего массива мертвые треугольники.
 */
static void compact_alive_triangles(bw_triangle_t* triangles, size_t* count) {
  size_t out = 0;
  for (size_t i = 0; i < *count; i++) {
    if (triangles[i].alive)
      triangles[out++] = triangles[i];
  }
  *count = out;
}

/**
 * @brief Создает супер-треугольник, покрывающий все входные точки.
 */
static int append_super_triangle(point_t* work_points, int n,
                                 bw_triangle_t** triangles, size_t* count,
                                 size_t* cap) {
  double min_x = work_points[0].x, max_x = work_points[0].x;
  double min_y = work_points[0].y, max_y = work_points[0].y;

  for (int i = 1; i < n; i++) {
    if (work_points[i].x < min_x) min_x = work_points[i].x;
    else if (work_points[i].x > max_x) max_x = work_points[i].x;
    if (work_points[i].y < min_y) min_y = work_points[i].y;
    else if (work_points[i].y > max_y) max_y = work_points[i].y;
  }

  double dx = max_x - min_x;
  double dy = max_y - min_y;
  double delta = fmax(dx, dy);
  if (delta <= POINT_EPS)
    return 0;

  double mid_x = (min_x + max_x) * 0.5;
  double mid_y = (min_y + max_y) * 0.5;
  double pad = delta * 32.0;

  work_points[n] = (point_t){ .x = (float)(mid_x - pad),
                              .y = (float)(mid_y - pad),
                              .z = 0.f, .height = 0.f, .id = -1 };
  work_points[n + 1] = (point_t){ .x = (float)mid_x,
                                  .y = (float)(mid_y + pad),
                                  .z = 0.f, .height = 0.f, .id = -1 };
  work_points[n + 2] = (point_t){ .x = (float)(mid_x + pad),
                                  .y = (float)(mid_y - pad),
                                  .z = 0.f, .height = 0.f, .id = -1 };

  return append_bw_triangle(triangles, count, cap, work_points,
                            n, n + 2, n + 1);
}

/**
 * @brief Конвертирует рабочие треугольники в публичный формат.
 */
static Triangle* collect_result_triangles(const bw_triangle_t* triangles,
                                          size_t count, int point_count,
                                          int* num_triangles) {
  size_t out_count = 0;
  for (size_t i = 0; i < count; i++) {
    if (triangles[i].alive &&
        triangles[i].p1 < point_count &&
        triangles[i].p2 < point_count &&
        triangles[i].p3 < point_count)
      out_count++;
  }

  if (out_count == 0) {
    *num_triangles = 0;
    return NULL;
  }

  Triangle* out = (Triangle*)malloc(out_count * sizeof(Triangle));
  if (!out) {
    *num_triangles = 0;
    return NULL;
  }

  size_t out_i = 0;
  for (size_t i = 0; i < count; i++) {
    if (!triangles[i].alive ||
        triangles[i].p1 >= point_count ||
        triangles[i].p2 >= point_count ||
        triangles[i].p3 >= point_count)
      continue;

    out[out_i++] = (Triangle){
      .p1 = triangles[i].p1,
      .p2 = triangles[i].p2,
      .p3 = triangles[i].p3,
      .numPolygon = 0
    };
  }

  *num_triangles = (int)out_count;
  return out;
}

Triangle* delaunay_triangulation(point_t* points, int* num_points,
                                 int* num_triangles) {
  if (num_triangles)
    *num_triangles = 0;
  if (!points || !num_points || !num_triangles || *num_points < 3)
    return NULL;

  int n = normalize_points(points, *num_points);
  *num_points = n;
  if (n < 3) {
    fprintf(stderr, "COUNT POINTS IS LOSS\n");
    return NULL;
  }

  point_t* work_points = (point_t*)malloc((size_t)(n + 3) * sizeof(point_t));
  if (!work_points)
    return NULL;
  memcpy(work_points, points, (size_t)n * sizeof(point_t));

  size_t tri_cap = 0;
  size_t tri_count = 0;
  bw_triangle_t* triangles = NULL;
  if (!append_super_triangle(work_points, n, &triangles, &tri_count, &tri_cap)) {
    free(work_points);
    free(triangles);
    return NULL;
  }

  unsigned char* bad = NULL;
  size_t bad_cap = 0;

  for (int p = 0; p < n; p++) {
    if (tri_count > bad_cap) {
      unsigned char* resized = (unsigned char*)realloc(bad, tri_count);
      if (!resized) {
        free(bad);
        free(work_points);
        free(triangles);
        return NULL;
      }
      bad = resized;
      bad_cap = tri_count;
    }

    mark_bad_triangles(triangles, tri_count, work_points, &work_points[p], bad);

    bw_edge_t* boundary = NULL;
    size_t boundary_count = 0;
    size_t boundary_cap = 0;

    for (size_t t = 0; t < tri_count; t++) {
      if (!bad[t])
        continue;

      if (!toggle_boundary_edge(&boundary, &boundary_count, &boundary_cap,
                                triangles[t].p1, triangles[t].p2) ||
          !toggle_boundary_edge(&boundary, &boundary_count, &boundary_cap,
                                triangles[t].p2, triangles[t].p3) ||
          !toggle_boundary_edge(&boundary, &boundary_count, &boundary_cap,
                                triangles[t].p3, triangles[t].p1)) {
        free(boundary);
        free(bad);
        free(work_points);
        free(triangles);
        return NULL;
      }

      triangles[t].alive = 0;
    }

    for (size_t e = 0; e < boundary_count; e++) {
      if (!append_bw_triangle(&triangles, &tri_count, &tri_cap, work_points,
                              boundary[e].a, boundary[e].b, p)) {
        free(boundary);
        free(bad);
        free(work_points);
        free(triangles);
        return NULL;
      }
    }

    free(boundary);
    compact_alive_triangles(triangles, &tri_count);
  }

  Triangle* result =
      collect_result_triangles(triangles, tri_count, n, num_triangles);

  free(bad);
  free(work_points);
  free(triangles);
  return result;
}

static uint64_t edge_key_u32(int a, int b) {
  uint32_t x = (uint32_t)a, y = (uint32_t)b;
  return ((uint64_t)x << 32) | (uint64_t)y;
}

static size_t next_pow2(size_t v) {
  size_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

static size_t hash_u64(uint64_t x) {
  // splitmix64
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  x = x ^ (x >> 31);
  return (size_t)x;
}

static int tri_vert(const Triangle* t, int i) {
  if (i == 0) return t->p1;
  if (i == 1) return t->p2;
  return t->p3;
}

void triangulation_free_polygons(polygon_t* polys, size_t countPolys) {
  if (!polys) return;
  for (size_t i = 0; i < countPolys; i++) {
    free(polys[i].verts);
    polys[i].verts = NULL;
    polys[i].cntVerts = 0;
  }
  free(polys);
}

/**
 * @brief Рабочая функция потока для проверки корректности треугольников.
 */
static void* mark_good_worker(void* arg) {
  good_worker_t* job = (good_worker_t*)arg;
  for (size_t i = job->begin; i < job->end; i++) {
    job->triangles[i].numPolygon = 0;
    job->good[i] =
        (unsigned char)(job->is_correct(&job->triangles[i], job->points) ? 1 : 0);
  }
  return NULL;
}

/**
 * @brief Параллельно применяет пользовательский предикат к треугольникам.
 */
static void mark_good_triangles(Triangle* tri, size_t countTriangle,
                                const point_t* points,
                                tri_correct_fn is_correct,
                                unsigned char* good) {
  int workers = triangulation_worker_count(countTriangle);
  if (workers <= 1) {
    good_worker_t job = {
      .triangles = tri,
      .good = good,
      .points = points,
      .is_correct = is_correct,
      .begin = 0,
      .end = countTriangle
    };
    (void)mark_good_worker(&job);
    return;
  }

  pthread_t threads[TRI_MAX_WORKERS];
  good_worker_t jobs[TRI_MAX_WORKERS];
  size_t chunk = (countTriangle + (size_t)workers - 1) / (size_t)workers;
  int created = 0;

  for (int i = 0; i < workers; i++) {
    size_t begin = (size_t)i * chunk;
    size_t end = begin + chunk;
    if (begin >= countTriangle) break;
    if (end > countTriangle) end = countTriangle;

    jobs[i] = (good_worker_t){
      .triangles = tri,
      .good = good,
      .points = points,
      .is_correct = is_correct,
      .begin = begin,
      .end = end
    };

    if (pthread_create(&threads[i], NULL, mark_good_worker, &jobs[i]) != 0)
      break;
    created++;
  }

  if (created != workers) {
    for (int i = 0; i < created; i++)
      pthread_join(threads[i], NULL);
    good_worker_t job = {
      .triangles = tri,
      .good = good,
      .points = points,
      .is_correct = is_correct,
      .begin = 0,
      .end = countTriangle
    };
    (void)mark_good_worker(&job);
    return;
  }

  for (int i = 0; i < created; i++)
    pthread_join(threads[i], NULL);
}

size_t triangulation_find_polygons(Triangle* tri, size_t countTriangle,
                                   const point_t* points, size_t countPoints,
                                   tri_correct_fn is_correct,
                                   polygon_t** outPolys) {
  if (!outPolys) return 0;
  *outPolys = NULL;
  if (!tri || countTriangle == 0 || !points || countPoints == 0 || !is_correct)
    return 0;

  unsigned char* good = (unsigned char*)malloc(countTriangle);
  if (!good) return 0;
  mark_good_triangles(tri, countTriangle, points, is_correct, good);

  int* neigh = (int*)malloc(countTriangle * 3 * sizeof(int));
  if (!neigh) {
    free(good);
    return 0;
  }
  for (size_t i = 0; i < countTriangle * 3; i++) neigh[i] = -1;

  size_t maxEdges = countTriangle * 3;
  size_t cap = next_pow2(maxEdges * 2 + 1);
  uint64_t* keys = (uint64_t*)malloc(cap * sizeof(uint64_t));
  int* valsTri = (int*)malloc(cap * sizeof(int));
  signed char* valsEdge = (signed char*)malloc(cap * sizeof(signed char));
  if (!keys || !valsTri || !valsEdge) {
    free(keys); free(valsTri); free(valsEdge);
    free(neigh);
    free(good);
    return 0;
  }
  for (size_t i = 0; i < cap; i++) keys[i] = 0ULL;

  for (size_t ti = 0; ti < countTriangle; ti++) {
    int v[3] = { tri[ti].p1, tri[ti].p2, tri[ti].p3 };
    int ea[3] = { v[0], v[1], v[2] };
    int eb[3] = { v[1], v[2], v[0] };
    for (int e = 0; e < 3; e++) {
      int a = ea[e], b = eb[e];
      if (a > b) { int t = a; a = b; b = t; }
      uint64_t k = edge_key_u32(a, b) + 1ULL;
      size_t idx = hash_u64(k) & (cap - 1);
      while (keys[idx] != 0ULL && keys[idx] != k) {
        idx = (idx + 1) & (cap - 1);
      }
      if (keys[idx] == 0ULL) {
        keys[idx] = k;
        valsTri[idx] = (int)ti;
        valsEdge[idx] = (signed char)e;
      } else {
        int ot = valsTri[idx];
        int oe = (int)valsEdge[idx];
        neigh[ti * 3 + e] = ot;
        neigh[(size_t)ot * 3 + oe] = (int)ti;
      }
    }
  }

  free(keys); free(valsTri); free(valsEdge);

  int* queue = (int*)malloc(countTriangle * sizeof(int));
  if (!queue) {
    free(neigh);
    free(good);
    return 0;
  }

  polygon_t* polys = NULL;
  size_t polyCap = 0, polyCnt = 0;

  edge_item_t* boundary = NULL;
  size_t bCap = 0, bCnt = 0;

  unsigned currentId = 0;
  for (size_t start = 0; start < countTriangle; start++) {
    if (!good[start] || tri[start].numPolygon != 0) continue;
    currentId++;

    size_t qh = 0, qt = 0;
    queue[qt++] = (int)start;
    tri[start].numPolygon = currentId;

    bCnt = 0;

    while (qh < qt) {
      int tIdx = queue[qh++];
      Triangle* t = &tri[tIdx];

      for (int e = 0; e < 3; e++) {
        int nb = neigh[(size_t)tIdx * 3 + e];
        if (nb >= 0 && good[(size_t)nb]) {
          if (tri[nb].numPolygon == 0) {
            tri[nb].numPolygon = currentId;
            queue[qt++] = nb;
          }
          continue;
        }

        int a = tri_vert(t, e);
        int b = tri_vert(t, (e + 1) % 3);
        if (a > b) { int tmp = a; a = b; b = tmp; }
        if (bCnt == bCap) {
          size_t newCap = bCap ? bCap * 2 : 256;
          edge_item_t* nbnd =
              (edge_item_t*)realloc(boundary, newCap * sizeof(edge_item_t));
          if (!nbnd) {
            free(boundary);
            triangulation_free_polygons(polys, polyCnt);
            free(queue);
            free(neigh);
            free(good);
            return 0;
          }
          boundary = nbnd;
          bCap = newCap;
        }
        boundary[bCnt++] =
            (edge_item_t){ .a = a, .b = b, .tri = tIdx, .edge = e, .used = 0 };
      }
    }

    if (bCnt == 0) continue;

    int* head = (int*)malloc(countPoints * sizeof(int));
    int* next = (int*)malloc(bCnt * 2 * sizeof(int));
    int* to = (int*)malloc(bCnt * 2 * sizeof(int));
    int* edgeId = (int*)malloc(bCnt * 2 * sizeof(int));
    if (!head || !next || !to || !edgeId) {
      free(head); free(next); free(to); free(edgeId);
      free(boundary);
      triangulation_free_polygons(polys, polyCnt);
      free(queue);
      free(neigh);
      free(good);
      return 0;
    }
    for (size_t i = 0; i < countPoints; i++) head[i] = -1;

    int ec = 0;
    for (size_t i = 0; i < bCnt; i++) {
      int a = boundary[i].a, b = boundary[i].b;
      to[ec] = b; edgeId[ec] = (int)i; next[ec] = head[a]; head[a] = ec++;
      to[ec] = a; edgeId[ec] = (int)i; next[ec] = head[b]; head[b] = ec++;
    }

    for (size_t ei = 0; ei < bCnt; ei++) {
      if (boundary[ei].used) continue;

      int startA = boundary[ei].a;
      int startB = boundary[ei].b;
      boundary[ei].used = 1;

      size_t cCap = 64, cCnt = 0;
      int* contour = (int*)malloc(cCap * sizeof(int));
      if (!contour) continue;
      contour[cCnt++] = startA;
      contour[cCnt++] = startB;

      int prev = startA;
      int cur = startB;
      while (cur != startA) {
        int found = 0;
        for (int it = head[cur]; it != -1; it = next[it]) {
          int eid = edgeId[it];
          int nxt = to[it];
          if (boundary[eid].used) continue;
          if (nxt == prev) continue;
          boundary[eid].used = 1;
          prev = cur;
          cur = nxt;
          if (cCnt == cCap) {
            cCap *= 2;
            int* nc = (int*)realloc(contour, cCap * sizeof(int));
            if (!nc) { found = 0; break; }
            contour = nc;
          }
          contour[cCnt++] = cur;
          found = 1;
          break;
        }
        if (!found) break;
      }

      if (polyCnt == polyCap) {
        size_t newCap = polyCap ? polyCap * 2 : 8;
        polygon_t* np = (polygon_t*)realloc(polys, newCap * sizeof(polygon_t));
        if (!np) { free(contour); break; }
        polys = np;
        polyCap = newCap;
      }
      polys[polyCnt].numPolygon = currentId;
      polys[polyCnt].verts = contour;
      polys[polyCnt].cntVerts = cCnt;
      polyCnt++;
    }

    free(head); free(next); free(to); free(edgeId);
  }

  free(boundary);
  free(queue);
  free(neigh);
  free(good);

  *outPolys = polys;
  return polyCnt;
}
