#ifndef TRIANGULATION_H
#define TRIANGULATION_H

// Структура для точки (x,y — для триангуляции, z/height — для рендера)
typedef struct {
  float x, y, z;
  float height;
  int id;
} point_t;

// Структура для треугольника
typedef struct {
  int p1, p2, p3; // индексы вершин
  double cx, cy;  // центр описанной окружности
  double r2;      // квадрат радиуса описанной окружности
} Triangle;

// Структура для ребра
typedef struct {
  int p1, p2;
} Edge;

// Вспомогательная структура для сортировки точек
typedef struct {
  point_t *points;
  int *indices;
} SortContext;

// Функция сравнения для сортировки по x, затем по y
int compare_points(const void *a, const void *b);
/**
 * @brief определяет, лежит ли точка C слева или справа от
  направленного ребра AB
 * @return Положительное значение — поворот против часовой стрелки,
  отрицательное — по часовой, ноль — коллинеарность
 */
double orient(point_t *a, point_t *b, point_t *c);
/**
 * @brief вычисляет квадрат евклидова расстояния между точками
 */
double dist2(point_t *a, point_t *b);
/**
 * @brief проверка условия Делоне
 * @return Возвращает 1, если точка d внутри/на окружности описанной вокруг abc
 */
int in_circle(point_t *a, point_t *b, point_t *c, point_t *d);
// Вычисление описанной окружности для треугольника
void compute_circumcircle(point_t *points, Triangle *tri);
// Проверка, является ли ребро локально оптимальным (условие Делоне)
int isLocalOptimalEdge(point_t *points, Triangle *tri1, Triangle *tri2,
                       int edge_p1, int edge_p2);
/**
 * @brief Флип ребра
 * @details Эта операция используется при слиянии левой и правой триангуляций,
когда обнаруживается, что общее ребро нарушает условие Делоне.
 */
void flipEdge(Triangle *tri1, Triangle *tri2, int edge_p1, int edge_p2);
// Рекурсивное построение триангуляции Делоне методом "разделяй и властвуй"
void build_delaunay(point_t *points, int start, int end, Triangle **triangles,
                    int *num_triangles);
// Функция для построения триангуляции Делоне из массива точек
Triangle *delaunay_triangulation(point_t *points, int num_points,
                                 int *num_triangles);
// Функция для преобразования входного массива с координатами (x,y,z)
point_t *prepare_points(float *input, int num_points);

void print_triangles(Triangle *triangles, int num_triangles, point_t *points);

#endif
