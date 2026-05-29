#ifndef TRIANGULATION_H
#define TRIANGULATION_H

#include <stddef.h>

/**
 * @brief Точка облака для построения и отрисовки поверхности.
 * @details
 * Координаты @c x и @c y используются алгоритмом триангуляции. Поля @c z и
 * @c height сохраняются для рендера и фильтрации безопасных областей.
 */
typedef struct {
  float x, y, z;
  float height;
  int id;
} point_t;

/**
 * @brief Треугольник поверх массива @ref point_t.
 */
typedef struct {
  int p1, p2, p3; /**< Индексы вершин в массиве точек. */
  unsigned numPolygon; /**< Идентификатор найденной связной области или @c 0. */
} Triangle;

/**
 * @brief Контур связной области корректных треугольников.
 */
typedef struct {
  unsigned numPolygon; /**< Идентификатор области, которой принадлежит контур. */
  int* verts;          /**< Индексы вершин из массива @ref point_t. */
  size_t cntVerts;     /**< Количество элементов в @ref verts. */
} polygon_t;

/**
 * @brief Предикат пригодности треугольника.
 * @param tri Проверяемый треугольник.
 * @param points Массив точек, на который ссылаются индексы треугольника.
 * @return Ненулевое значение, если треугольник считается корректным.
 */
typedef int (*tri_correct_fn)(const Triangle* tri, const point_t* points);

/**
 * @brief Строит триангуляцию Делоне алгоритмом Bowyer-Watson.
 * @details
 * Реализация самодостаточная и не использует сторонние библиотеки из @c deps.
 * При большом количестве точек входное облако прореживается до внутреннего
 * лимита, очищается от XY-дубликатов и упорядочивается вдоль кривой Гильберта.
 * Благодаря пространственной локальности этого порядка каждая вставляемая
 * точка находит свою каверну локальным обходом смежных треугольников (поиск
 * по соседям + обход в ширину), не перебирая все треугольники.
 *
 * @param[in,out] points Массив точек. Функция может переупорядочить и
 *   проредить первые @p num_points элементов.
 * @param[in,out] num_points На входе количество точек, на выходе количество
 *   точек, реально участвующих в триангуляции.
 * @param[out] num_triangles Количество построенных треугольников.
 * @return Массив треугольников с индексами в диапазоне
 *   @c 0..(*num_points-1). Память освобождается через @c free().
 *   Возвращает @c NULL, если триангуляция невозможна или не хватило памяти.
 */
Triangle *delaunay_triangulation(point_t *points, int *num_points,
                                 int *num_triangles);

/**
 * @brief Устанавливает предельное число точек, передаваемых в триангуляцию.
 * @details
 * Этот предел управляет густотой триангуляционной сетки: при превышении
 * входное облако прореживается до указанного значения. Ранее предел
 * задавался макросом на этапе компиляции; теперь он изменяется в реальном
 * времени. Значение ограничивается снизу безопасным минимумом.
 * @param maxPoints Желаемый предел числа точек триангуляции.
 */
void triangulation_set_max_points(int maxPoints);

/**
 * @brief Возвращает текущий предел числа точек триангуляции (густоту сетки).
 */
int triangulation_get_max_points(void);

/**
 * @brief Находит связные области корректных треугольников и их контуры.
 * @details
 * Всем корректным треугольникам проставляется @c tri[i].numPolygon = 1..K,
 * некорректным — @c 0. Возвращаемый массив может содержать несколько контуров
 * на одну область. Первичное применение @p is_correct распараллелено через
 * POSIX threads.
 *
 * @param[in,out] tri Массив треугольников.
 * @param countTriangle Количество треугольников.
 * @param points Массив точек.
 * @param countPoints Количество точек.
 * @param is_correct Предикат корректности треугольника. Может быть @c NULL,
 *   если задан @p precomputed.
 * @param precomputed Необязательная готовая классификация треугольников
 *   (массив 0/1 размера @p countTriangle), полученная внешним модулем
 *   (например, на устройстве OpenCL). Если не @c NULL, используется вместо
 *   вызова @p is_correct; в этом случае распараллеленная классификация на
 *   CPU не выполняется.
 * @param[out] outPolys Массив контуров. Освобождается через
 *   @ref triangulation_free_polygons.
 * @return Количество контуров в @p outPolys.
 */
size_t triangulation_find_polygons(Triangle* tri, size_t countTriangle,
                                   const point_t* points, size_t countPoints,
                                   tri_correct_fn is_correct,
                                   const unsigned char* precomputed,
                                   polygon_t** outPolys);

/**
 * @brief Освобождает массив контуров, созданный triangulation_find_polygons().
 * @param polys Массив контуров или @c NULL.
 * @param countPolys Количество элементов в @p polys.
 */
void triangulation_free_polygons(polygon_t* polys, size_t countPolys);

#endif
