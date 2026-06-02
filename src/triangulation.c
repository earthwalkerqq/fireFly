#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hash.h"
#include "triangulation.h"
#include "common.h"

#define MAX_TRIANG_POINTS_DEFAULT 12000
#define TRI_MAX_WORKERS 8
#define TRI_PARALLEL_MIN_ITEMS 512
#define MAX_HILBERT_ORDER 16          /* разрешение сетки Гильберта: сторона 2^16 */
#define POINT_EPS 1e-6
#define AREA_EPS 1e-12
#define CIRCLE_EPS 1e-9
#define CAP_DEFAULT 64

#ifndef INFINITY
#define INFINITY 1e30
#endif

/*
 * Предельное число точек, передаваемых в триангуляцию.
 * управляется в реальном времени через triangulation_set_max_points().
 */
static int g_max_triang_points = MAX_TRIANG_POINTS_DEFAULT;

void triangulation_set_max_points(int maxPoints) {
  if (maxPoints < 64) maxPoints = 64;
  g_max_triang_points = maxPoints;
}

int triangulation_get_max_points(void) {
  return g_max_triang_points;
}

/**
 * @brief Внутреннее представление треугольника Bowyer-Watson.
 * @details Помимо индексов вершин и описанной окружности хранит индексы
 * соседних треугольников по каждому ребру: @c n[0] — сосед по ребру
 * (p[0],p[1]), @c n[1] — по (p[1],p[2]), @c n[2] — по (p[2],p[0]); @c -1,
 * если соседа нет. Смежность позволяет находить каверну вставляемой точки
 * локальным обходом, не перебирая все треугольники.
 */
typedef struct {
  int p[3];               /* Индексы вершин (обход против часовой стрелки) */
  int n[3];               /* Соседи по рёбрам (если нет соседа, то -1) */
  double cx, cy, r2;      /* Центр и квадрат радиуса описанной окружности */
  unsigned char alive;    /* Жив ли треугольник */
  unsigned char visited;  /* Метка обхода (сбрасывается после вставки) */
} bw_triangle_t;

/**
 * @brief Сетка треугольников Bowyer-Watson с переиспользованием слотов.
 * @details Удалённые треугольники не сдвигают массив (это разрушило бы
 * индексы смежности), а складываются в список свободных слотов и
 * переиспользуются при создании новых треугольников.
 */
typedef struct {
  bw_triangle_t* tris;
  size_t count, cap;
  int* freeList;          /**< Стек индексов освобождённых слотов. */
  size_t freeCount, freeCap;
} bw_mesh_t;

/**
 * @brief Сортировочный ключ для упорядочивания точек по кривой Гильберта.
 */
typedef struct {
  uint64_t key;
  int idx;
} hkey_t;

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
 * @brief Вычисляет ориентированную удвоенную площадь треугольника в XY
 * @details будет использоваться для определения, с какой стороны от ребра
 * находится точка.
 */
static double orient2d(const point_t* a, const point_t* b, const point_t* c) {
  return ((double)b->x - (double)a->x) * ((double)c->y - (double)a->y) -
         ((double)b->y - (double)a->y) * ((double)c->x - (double)a->x);
}

/**
 * @brief Сравнивает точки по XY для qsort
 * @return 1 - т. a больше т. b; -1 - т. a меньше т. b; 0 - точки равны и по x и по y
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
 * @brief Переводит координаты ячейки (x,y) в расстояние вдоль кривой Гильберта.
 * @details Классический алгоритм d = xy2d для квадрата со стороной @p n
 */
static uint64_t hilbert_xy2d(uint32_t n, uint32_t x, uint32_t y) {
  uint64_t d = 0;
  for (uint32_t s = n / 2; s > 0; s /= 2) {
    uint32_t rx = (x & s) ? 1u : 0u;
    uint32_t ry = (y & s) ? 1u : 0u;
    d += (uint64_t)s * (uint64_t)s * ((3u * rx) ^ ry);
    /* поворот квадранта */
    if (ry == 0) {
      if (rx == 1) {
        x = n - 1 - x;
        y = n - 1 - y;
      }
      uint32_t t = x;
      x = y;
      y = t;
    }
  }
  return d;
}

/**
 * @brief Функция предикат для qsort
 */
static int cmp_hkey(const void* lhs, const void* rhs) {
  const hkey_t* a = (const hkey_t*)lhs;
  const hkey_t* b = (const hkey_t*)rhs;
  if (a->key < b->key) return -1;
  if (a->key > b->key) return 1;
  return 0;
}

/**
 * @brief Переупорядочивает точки вдоль кривой Гильберта.
 * @details Координаты XY отображаются на квадратную сетку 2^HILBERT_ORDER,
 * для каждой ячейки вычисляется её номер на кривой Гильберта, после чего
 * точки сортируются по этому номеру. В результате последовательно
 * вставляемые в триангуляцию точки оказываются пространственно близкими,
 * что позволяет находить каверну локальным обходом смежных треугольников.
 * При нехватке памяти исходный порядок сохраняется (на корректность не влияет).
 */
static void hilbert_sort(point_t* points, int n, int hil_order) {
  if (n < 2)
    return;

  /* нахождение ограничевающего прямоугольника */
  double min_x = points[0].x, max_x = points[0].x;
  double min_y = points[0].y, max_y = points[0].y;
  for (int i = 1; i < n; i++) {
    if (points[i].x < min_x) min_x = points[i].x;
    else if (points[i].x > max_x) max_x = points[i].x;
    if (points[i].y < min_y) min_y = points[i].y;
    else if (points[i].y > max_y) max_y = points[i].y;
  }

  /* размер сетки - степень двойки */
  const uint32_t side = 1u << hil_order;
  double sx = (max_x > min_x) ? (max_x - min_x) : 1.0;
  double sy = (max_y > min_y) ? (max_y - min_y) : 1.0;

  hkey_t* keys = (hkey_t*)malloc((size_t)n * sizeof(hkey_t));
  point_t* tmp = (point_t*)malloc((size_t)n * sizeof(point_t));
  if (!keys || !tmp) {
    fprintf(stderr, "FAIL FROM MEMORY ALLOCATE\n");
    free(keys);
    free(tmp);
    return;
  }

  /* точки на кривой Гильберта близки в пространстве, но наоборот работет не всегда */
  for (int i = 0; i < n; i++) {
    uint32_t gx = (uint32_t)(((double)points[i].x - min_x) / sx * (side - 1));
    uint32_t gy = (uint32_t)(((double)points[i].y - min_y) / sy * (side - 1));
    keys[i].key = hilbert_xy2d(side, gx, gy);
    keys[i].idx = i;
  }

  qsort(keys, (size_t)n, sizeof(hkey_t), cmp_hkey);

  for (int i = 0; i < n; i++)
    tmp[i] = points[keys[i].idx];
  memcpy(points, tmp, (size_t)n * sizeof(point_t));

  free(keys);
  free(tmp);
}

/**
 * @brief Возвращает рекомендуемое число потоков для заданного объема работы.
 * @details По умолчанию используется число доступных CPU, но не больше TRI_MAX_WORKERS.
 * Для отладки можно задать переменную окружения
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
 * @brief Прореживает большое облако точек до текущего предела густоты сетки.
 * @details
  Область разбивается на равномерную сетку; все точки, попавшие в одну
  ячейку, заменяются одной точкой — усреднённым центроидом ячейки
  (стандартное воксельное прореживание облаков точек).
  Предел задаётся переменной @c g_max_triang_points и
  регулируется в реальном времени.
  p.s. Пробовал делать 1. с выборором той точки, которая будет ближе всех к центру ячейки;
  2. с выбором точки, которая явлсяется локальным максимумом в ячейке. В первом случае, при выборе
  большого размера конечного элемента сетки, не учитываются возможные припятствия, а во втором
  случает сетка получается сильно шероховатой.
 */
static int subsample(point_t* points, int* n) {
  int maxPoints = g_max_triang_points;
  if (*n <= maxPoints)
    return (int)ceil(sqrt((double)*n));

  double min_x = points[0].x, max_x = points[0].x;
  double min_y = points[0].y, max_y = points[0].y;
  for (int i = 1; i < *n; i++) {
    if (points[i].x < min_x) min_x = points[i].x;
    else if (points[i].x > max_x) max_x = points[i].x;
    if (points[i].y < min_y) min_y = points[i].y;
    else if (points[i].y > max_y) max_y = points[i].y;
  }

  int g = (int)ceil(sqrt((double)maxPoints)); // размер регулярной сетки
  if (g < 3) g = 3;

  /* аккумудяторы по ячейкам */
  size_t cells = (size_t)g * (size_t)g;
  double* accX = (double*)calloc(cells, sizeof(double));
  double* accY = (double*)calloc(cells, sizeof(double));
  double* accZ = (double*)calloc(cells, sizeof(double));
  double* accH = (double*)calloc(cells, sizeof(double));
  int*    cnt  = (int*)calloc(cells, sizeof(int));
  int*    cntH = (int*)calloc(cells, sizeof(int));
  point_t* tmp = (point_t*)malloc(cells * sizeof(point_t));
  if (!accX || !accY || !accZ || !accH || !cnt || !cntH || !tmp) {
    fprintf(stderr, "FAIL FROM MEMORY ALLOCATE\n");
    memDestroy(6, accX, accY, accZ, accH, cnt, cntH);
    free(tmp);
    return g;
  }

  // размер ячейки
  double sx = (max_x > min_x) ? (max_x - min_x) / g : 1.0;
  double sy = (max_y > min_y) ? (max_y - min_y) / g : 1.0;

  // распределяем точки по ячейкам
  for (int i = 0; i < *n; i++) {
    int gx = (int)(((double)points[i].x - min_x) / sx);
    int gy = (int)(((double)points[i].y - min_y) / sy);

    if (gx >= g) gx = g - 1;
    else if (gx < 0) gx = 0;
    if (gy >= g) gy = g - 1;
    else if (gy < 0) gy = 0;

    int idx = gy * g + gx; // линейный индекс
    accX[idx] += points[i].x;
    accY[idx] += points[i].y;
    accZ[idx] += points[i].z;
    cnt[idx]++;
    /* высота рельефа усредняется только по точкам с её определённым
       значением; ячейку без таких точек считаем без данных рельефа */
    if (points[i].height >= 0.0f) {
      accH[idx] += points[i].height;
      cntH[idx]++;
    }
  }

  int out = 0;
  for (size_t i = 0; i < cells; i++) {
    if (cnt[i] == 0) continue;
    point_t p = {
      .x = (float)(accX[i] / cnt[i]),
      .y = (float)(accY[i] / cnt[i]),
      .z = (float)(accZ[i] / cnt[i]),
      .height = (cntH[i] > 0) ? (float)(accH[i] / cntH[i]) : -1.0f,
      .id = out
    };
    tmp[out++] = p;
  }

  memDestroy(6, accX, accY, accZ, accH, cnt, cntH);

  if (out >= 3) {
    memcpy(points, tmp, (size_t)out * sizeof(point_t));
    *n = out;
  }

  free(tmp);
  return g;
}

/**
 * @brief Порядок (число бит) сетки Гильберта, сторона которой не меньше @p g.
 * @details hilbert_sort использует side = 1u << order, поэтому здесь нужен
 * именно показатель степени: наименьший order, при котором 2^order >= g.
 * Значение ограничено сверху 16 битами (сторона до 65536) и снизу 1.
 */
static int hilbert_order_for(int g) {
  int order = 1;
  while ((1 << order) < g && order < MAX_HILBERT_ORDER)
    order++;
  return order;
}

/**
 * @brief Сортирует точки и удаляет XY-дубликаты.
 */
static int normalize_points(point_t* points, int n) {
  int g = subsample(points, &n);
  if (n < 3)
    return n;

  /* сортировка по XY нужна только для удаления дубликатов */
  qsort(points, (size_t)n, sizeof(point_t), cmp_point_xy);

  int out = 1;
  for (int i = 1; i < n; i++) {
    if (!same_point_xy(&points[out - 1], &points[i]))
      points[out++] = points[i];
  }

  int hil_order = hilbert_order_for(g);
  #ifdef DEBUG
  printf("\nHILBERT_ORDER = %d\n", hil_order);
  #endif
  /* итоговый порядок вставки — вдоль кривой Гильберта */
  hilbert_sort(points, out, hil_order);

  return out;
}

/**
 * @brief Вычисляет окружность, описанную вокруг треугольника.
 * @details вычисляется через определитель матрицы, который равен удвоенной
 * ориентированной площади треугольника:
 *         | ax ay 1 |
 * d = 2 * | bx by 1 |
 *         | cx cy 1 |
 * @param cpx - координата центра окр-ти по X
 * @param cpy - координата центра окр-ти по Y
 * @param r2 - удвоенный радиус окр-ти
 */
static int compute_circumcircle(const point_t* points, int a, int b, int c,
                                double* cpx, double* cpy, double* r2) {
  const point_t* p1 = &points[a];
  const point_t* p2 = &points[b];
  const point_t* p3 = &points[c];

  double ax = p1->x, ay = p1->y;
  double bx = p2->x, by = p2->y;
  double cx = p3->x, cy = p3->y;

  double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (fabs(d) <= AREA_EPS) // вырожденный случай
    return 0;

  /* Центр описанной окружности находится на пересечении серединных перпендикуляров.
  В координатной форме решение системы уравнений:
   (x - ax)² + (y - ay)² = r2
   (x - bx)² + (y - by)² = r2
   (x - cx)² + (y - cy)² = r2
  */

  double ax2ay2 = ax * ax + ay * ay;
  double bx2by2 = bx * bx + by * by;
  double cx2cy2 = cx * cx + cy * cy;

  *cpx = (ax2ay2 * (by - cy) + bx2by2 * (cy - ay) +
         cx2cy2 * (ay - by)) / d;
  *cpy = (ax2ay2 * (cx - bx) + bx2by2 * (ax - cx) +
         cx2cy2 * (bx - ax)) / d;

  double dx = *cpx - ax;
  double dy = *cpy - ay;
  *r2 = dx * dx + dy * dy;
  return *r2 > AREA_EPS;
}

/**
 * @brief Расширяет массив внутренних треугольников.
 */
/**
 * @brief Расширяет массив треугольников сетки до требуемого размера.
 */
static int reserve_bw_triangles(bw_mesh_t* m, size_t need) {
  if (need <= m->cap)
    return 1;

  size_t new_cap = m->cap ? m->cap : 64;
  while (new_cap < need)
    new_cap *= 2;

  bw_triangle_t* resized =
      (bw_triangle_t*)realloc(m->tris, new_cap * sizeof(bw_triangle_t));
  if (!resized)
    return 0;

  m->tris = resized;
  m->cap = new_cap;
  return 1;
}

/**
 * @brief Складывает индекс освобождённого слота в список свободных.
 */
static int bw_push_free(bw_mesh_t* m, int idx) {
  if (m->freeCount == m->freeCap) {
    size_t nc = m->freeCap ? m->freeCap * 2 : 64;
    int* r = (int*)realloc(m->freeList, nc * sizeof(int));
    if (!r)
      return 0;
    m->freeList = r;
    m->freeCap = nc;
  }
  m->freeList[m->freeCount++] = idx;
  return 1;
}

/**
 * @brief Создаёт треугольник с заданным порядком вершин (без переориентации).
 * @details Вызывающий обязан передать вершины против часовой стрелки.
 * Свободный слот переиспользуется, иначе массив растёт. Вырожденный
 * треугольник получает r2 = -1 и никогда не считается «плохим», что
 * сохраняет целостность смежности.
 * @return Индекс созданного треугольника или -1 при нехватке памяти.
 */
static int bw_new_triangle(bw_mesh_t* m, const point_t* points,
                           int a, int b, int c) {
  double cx = 0.0, cy = 0.0, r2 = -1.0;
  if (!compute_circumcircle(points, a, b, c, &cx, &cy, &r2)) {
    cx = 0.0; cy = 0.0; r2 = -1.0;
  }

  size_t idx;
  if (m->freeCount > 0) {
    idx = (size_t)m->freeList[--m->freeCount];
  } else {
    if (!reserve_bw_triangles(m, m->count + 1))
      return -1;
    idx = m->count++;
  }

  m->tris[idx] = (bw_triangle_t){
    .p = { a, b, c },
    .n = { -1, -1, -1 },
    .cx = cx, .cy = cy, .r2 = r2,
    .alive = 1, .visited = 0
  };
  return (int)idx;
}

/**
 * @brief Проверяет попадание точки внутрь описанной окружности треугольника.
 * @details Если расстояние от точки до центра больше радиуса — не входит.
 */
static int point_in_circumcircle(const bw_triangle_t* tri,
                                 const point_t* point) {
  if (!tri->alive || tri->r2 < 0.0)
    return 0;

  double dx = (double)point->x - tri->cx;
  double dy = (double)point->y - tri->cy;
  double d2 = dx * dx + dy * dy;
  double eps = fmax(1.0, tri->r2) * CIRCLE_EPS;
  return d2 <= tri->r2 + eps;
}

/**
 * @brief Находит ребро соседа @p nb, ведущее обратно в треугольник @p t.
 */
static int bw_back_edge(const bw_mesh_t* m, int nb, int t) {
  const bw_triangle_t* tn = &m->tris[nb];
  for (int e = 0; e < 3; e++)
    if (tn->n[e] == t)
      return e;
  return -1;
}

/**
 * @brief Локализует треугольник, содержащий точку, шагая по смежности.
 * @details Старт из подсказки @p hint (обычно треугольник предыдущей
 * вставки — при гильбертовом порядке он рядом). На каждом шаге переходим
 * через ребро, относительно которого точка лежит снаружи. Так как точки
 * вставляются внутрь супертреугольника, обход всегда завершается; счётчик
 * @c guard страхует от зацикливания на вырожденных конфигурациях.
 * @return Индекс найденного треугольника или -1.
 */
static int bw_locate(const bw_mesh_t* m, int hint, const point_t* points,
                     const point_t* p) {
  int cur = (hint >= 0 && (size_t)hint < m->count && m->tris[hint].alive)
                ? hint : -1;
  if (cur < 0) {
    for (size_t i = 0; i < m->count; i++)
      if (m->tris[i].alive) { cur = (int)i; break; }
  }
  if (cur < 0)
    return -1;

  /* Формула-защита от бесконечного цикла. Кажется, что в худшем случае нахождение треугольника
   * происходит за m->count операций, но ведь он может пойти каким-то супер неоптимальным путем
   * и тогда возможно повторение треугольников -> больше итераций. Пока обход сделан с небольшим запасом,
   * наверное лучше сделать с отслеживанием пройденных треугольников.
   */
  size_t guard = 4 * m->count + 16;
  for (size_t step = 0; step < guard; step++) {
    const bw_triangle_t* t = &m->tris[cur];
    int moved = 0;
    for (int e = 0; e < 3; e++) {
      const point_t* u = &points[t->p[e]];
      const point_t* v = &points[t->p[(e + 1) % 3]];
      if (orient2d(u, v, p) < 0.0) {
        // точка слева от ребра u-v -> нужно перейти к соседнему треугольнику
        int nb = t->n[e];
        if (nb != -1) {
          cur = nb;
          moved = 1;
          break;
        }
      }
    }
    if (!moved)
      return cur;
  }
  return cur;
}

/* Динамическое расширение массива. Нужно лишь при работе
 * функции вставки точки, поэтому в виде макроса */
#define BW_GROW_INT(buf, cap, cnt)                         \
  do {                                                     \
    if ((cnt) == *(cap)) {                                 \
      size_t nc = *(cap) ? *(cap) * 2 : CAP_DEFAULT;                \
      int* r = (int*)realloc(*(buf), nc * sizeof(int));    \
      if (!r) return 0;                                    \
      *(buf) = r; *(cap) = nc;                             \
    }                                                      \
  } while (0)

/**
 * @brief Вставляет точку @p pi, перестраивая каверну Делоне локально.
 * @details
 *  1. Находим стартовый плохой треугольник (его описанная окружность
 *     содержит точку) локализацией по смежности — без перебора всех
 *     треугольников.
 *  2. Обходом в ширину собираем всю каверну — связную область плохих
 *     треугольников.
 *  3. Снимаем граничные рёбра каверны и достраиваем веер новых
 *     треугольников к точке, восстанавливая смежность.
 *
 * Все рабочие буферы передаются извне и переиспользуются между вставками,
 * чтобы не выделять память на каждую точку.
 * @param[in,out] hint Подсказка локализации; на выходе — один из новых
 *   треугольников рядом с точкой.
 * @return 1 при успехе, 0 при нехватке памяти.
 */
static int bw_insert_point(bw_mesh_t* m, const point_t* points, int pi,
                           int* hint,
                           int** stackBuf, size_t* stackCap,
                           int** touchedBuf, size_t* touchedCap,
                           int** newBuf, size_t* newCap,
                           int** bndA, int** bndB, int** bndOt, int** bndOe,
                           size_t* bndCap) {
  const point_t* p = &points[pi];

  int seed = bw_locate(m, *hint, points, p);
  if (seed < 0 || !point_in_circumcircle(&m->tris[seed], p)) {
    /* если не удалось найти по локали, ищем любой плохой треугольник перебором */
    seed = -1;
    for (size_t i = 0; i < m->count; i++) {
      if (m->tris[i].alive && point_in_circumcircle(&m->tris[i], p)) {
        seed = (int)i;
        break;
      }
    }
    if (seed < 0)
      return 1; /* точка не нарушает ни одной окружности — пропускаем */
  }

  /* обход в ширину: собираем каверну плохих треугольников */
  size_t stackCnt = 0, touchedCnt = 0;

  BW_GROW_INT(stackBuf, stackCap, stackCnt);
  (*stackBuf)[stackCnt++] = seed;
  m->tris[seed].visited = 1;
  BW_GROW_INT(touchedBuf, touchedCap, touchedCnt);
  (*touchedBuf)[touchedCnt++] = seed;

  while (stackCnt > 0) {
    int t = (*stackBuf)[--stackCnt];
    for (int e = 0; e < 3; e++) {
      int nb = m->tris[t].n[e];
      if (nb == -1 || m->tris[nb].visited)
        continue;
      if (m->tris[nb].alive && point_in_circumcircle(&m->tris[nb], p)) {
        m->tris[nb].visited = 1;
        BW_GROW_INT(touchedBuf, touchedCap, touchedCnt);
        (*touchedBuf)[touchedCnt++] = nb;
        BW_GROW_INT(stackBuf, stackCap, stackCnt);
        (*stackBuf)[stackCnt++] = nb;
      }
    }
  }

  /* помечаем плохие треугольники мёртвыми — для теста границы каверны */
  for (size_t i = 0; i < touchedCnt; i++)
    m->tris[(*touchedBuf)[i]].alive = 0;

  /* граничные рёбра каверны */
  size_t bndCnt = 0;
  for (size_t i = 0; i < touchedCnt; i++) {
    int t = (*touchedBuf)[i];
    for (int e = 0; e < 3; e++) {
      int nb = m->tris[t].n[e];
      /* ребро граничное, если снаружи нет треугольника либо он жив
         (то есть не входит в каверну) */
      if (nb != -1 && !m->tris[nb].alive)
        continue;

      if (bndCnt == *bndCap) {
        size_t nc = *bndCap ? *bndCap * 2 : 64;
        int* ra = (int*)realloc(*bndA, nc * sizeof(int));
        int* rb = (int*)realloc(*bndB, nc * sizeof(int));
        int* rt = (int*)realloc(*bndOt, nc * sizeof(int));
        int* re = (int*)realloc(*bndOe, nc * sizeof(int));
        if (ra) *bndA = ra;
        if (rb) *bndB = rb;
        if (rt) *bndOt = rt;
        if (re) *bndOe = re;
        if (!ra || !rb || !rt || !re)
          return 0;
        *bndCap = nc;
      }

      (*bndA)[bndCnt]  = m->tris[t].p[e];
      (*bndB)[bndCnt]  = m->tris[t].p[(e + 1) % 3];
      (*bndOt)[bndCnt] = nb;
      (*bndOe)[bndCnt] = (nb >= 0) ? bw_back_edge(m, nb, t) : -1;
      bndCnt++;
    }
  }

  /* освобождённые слоты плохих треугольников можно переиспользовать */
  for (size_t i = 0; i < touchedCnt; i++) {
    m->tris[(*touchedBuf)[i]].visited = 0;
    if (!bw_push_free(m, (*touchedBuf)[i]))
      return 0;
  }

  /* --- веер новых треугольников (a, b, p) + восстановление смежности --- */
  if (bndCnt > *newCap) {
    int* r = (int*)realloc(*newBuf, bndCnt * sizeof(int));
    if (!r) return 0;
    *newBuf = r;
    *newCap = bndCnt;
  }

  for (size_t i = 0; i < bndCnt; i++) {
    /* (a,b) — ребро каверны (интерьер слева), p внутри => (a,b,p) уже CCW */
    int nt = bw_new_triangle(m, points, (*bndA)[i], (*bndB)[i], pi);
    if (nt < 0)
      return 0;
    (*newBuf)[i] = nt;
    /* ребро 0 = (a,b) — наружу каверны */
    m->tris[nt].n[0] = (*bndOt)[i];
    if ((*bndOt)[i] >= 0 && (*bndOe)[i] >= 0)
      m->tris[(*bndOt)[i]].n[(*bndOe)[i]] = nt;
  }

  /* связываем «спицы»: рёбра (b,p)=1 и (p,a)=2 соседних новых треугольников
     по их общей вершине, отличной от p */
  for (size_t i = 0; i < bndCnt; i++) {
    for (size_t j = i + 1; j < bndCnt; j++) {
      int ai = (*bndA)[i], bi = (*bndB)[i];
      int aj = (*bndA)[j], bj = (*bndB)[j];
      int v = -1;
      if (ai == aj || ai == bj) v = ai;
      else if (bi == aj || bi == bj) v = bi;
      if (v < 0)
        continue;
      int ei = (v == bi) ? 1 : 2;
      int ej = (v == bj) ? 1 : 2;
      m->tris[(*newBuf)[i]].n[ei] = (*newBuf)[j];
      m->tris[(*newBuf)[j]].n[ej] = (*newBuf)[i];
    }
  }

  if (bndCnt > 0)
    *hint = (*newBuf)[bndCnt - 1];
  return 1;
}

#undef BW_GROW_INT

/**
 * @brief Создает супер-треугольник, покрывающий все входные точки.
 */
static int append_super_triangle(point_t* work_points, int n, bw_mesh_t* m) {
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

  work_points[n] = (point_t){
    .x = (float)(mid_x - pad), .y = (float)(mid_y - pad),
    .z = 0.f, .height = 0.f, .id = -1
  };
  work_points[n + 1] = (point_t){
    .x = (float)mid_x, .y = (float)(mid_y + pad),
    .z = 0.f, .height = 0.f, .id = -1
  };
  work_points[n + 2] = (point_t){
    .x = (float)(mid_x + pad), .y = (float)(mid_y - pad),
    .z = 0.f, .height = 0.f, .id = -1
  };

  /* порядок (n, n+2, n+1) гарантированно против часовой стрелки */
  return bw_new_triangle(m, work_points, n, n + 2, n + 1) >= 0;
}

/**
 * @brief Конвертирует рабочие треугольники в публичный формат.
 */
static Triangle* collect_result_triangles(const bw_mesh_t* m, int point_count,
                                          int* num_triangles) {
  size_t out_count = 0;
  for (size_t i = 0; i < m->count; i++) {
    const bw_triangle_t* t = &m->tris[i];
    if (t->alive && t->p[0] < point_count && t->p[1] < point_count &&
        t->p[2] < point_count)
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
  for (size_t i = 0; i < m->count; i++) {
    const bw_triangle_t* t = &m->tris[i];
    if (!t->alive || t->p[0] >= point_count || t->p[1] >= point_count ||
        t->p[2] >= point_count)
      continue;

    out[out_i++] = (Triangle){
      .p1 = t->p[0], .p2 = t->p[1], .p3 = t->p[2], .numPolygon = 0
    };
  }

  *num_triangles = (int)out_count;
  return out;
}

Triangle* delaunay_triangulation(point_t* points, int* num_points,
                                 int* num_triangles) {
  /* Алгоритм Боуэра — Ватсона:
   *  1. Нормализация точек (прореживание, удаление дубликатов,
   *     упорядочивание вдоль кривой Гильберта).
   *  2. Создание супер-треугольника.
   *  3. Поочерёдная вставка точек: локализация каверны по смежности и её
   *     локальная перестройка (без перебора всех треугольников).
   *  4. Удаление супер-треугольника и возврат результата.
   */
  if (num_triangles)
    *num_triangles = 0;
  if (!points || !num_points || !num_triangles || *num_points < 3)
    return NULL;

  *num_points = normalize_points(points, *num_points);
  if (*num_points < 3) {
    fprintf(stderr, "COUNT POINTS IS LOSS\n");
    return NULL;
  }

  /* все точки, включая вершины супертреугольника */
  point_t* work_points =
      (point_t*)malloc((size_t)(*num_points + 3) * sizeof(point_t));
  if (!work_points)
    return NULL;
  memcpy(work_points, points, (size_t)*num_points * sizeof(point_t));

  bw_mesh_t mesh = { 0 };
  if (!append_super_triangle(work_points, *num_points, &mesh)) {
    free(mesh.tris);
    free(mesh.freeList);
    free(work_points);
    return NULL;
  }

  /* переиспользуемые между вставками рабочие буферы */
  int *stackBuf = NULL, *touchedBuf = NULL, *newBuf = NULL;
  int *bndA = NULL, *bndB = NULL, *bndOt = NULL, *bndOe = NULL;
  size_t stackCap = 0, touchedCap = 0, newCap = 0, bndCap = 0;
  int hint = 0; /* супертреугольник */

  int ok = 1;
  for (int pidx = 0; pidx < *num_points; pidx++) {
    if (!bw_insert_point(&mesh, work_points, pidx, &hint,
                         &stackBuf, &stackCap, &touchedBuf, &touchedCap,
                         &newBuf, &newCap, &bndA, &bndB, &bndOt, &bndOe,
                         &bndCap)) {
      ok = 0;
      break;
    }
  }

  memDestroy(7, stackBuf, touchedBuf, newBuf, bndA, bndB, bndOt, bndOe);

  Triangle* result = NULL;
  if (ok)
    result = collect_result_triangles(&mesh, *num_points, num_triangles);

  free(mesh.tris);
  free(mesh.freeList);
  free(work_points);
  return result;
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
                                   const unsigned char* precomputed,
                                   polygon_t** outPolys) {
  if (!outPolys) return 0;
  *outPolys = NULL;
  if (!tri || countTriangle == 0 || !points || countPoints == 0 ||
      (!is_correct && !precomputed))
    return 0;

  unsigned char* good = (unsigned char*)malloc(countTriangle);
  if (!good) return 0;
  if (precomputed) {
    /* классификация уже выполнена внешним модулем */
    for (size_t i = 0; i < countTriangle; i++) {
      tri[i].numPolygon = 0;
      good[i] = precomputed[i] ? 1 : 0;
    }
  } else {
    mark_good_triangles(tri, countTriangle, points, is_correct, good);
  }

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
        /* Первое появление ребра — запоминаем его треугольник и номер ребра. */
        keys[idx] = k;
        valsTri[idx] = (int)ti;
        valsEdge[idx] = (signed char)e;
      } else {
        /* Ребро уже встречалось у другого треугольника — связываем их как
           соседей по этому общему ребру (взаимные ссылки смежности). */
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
