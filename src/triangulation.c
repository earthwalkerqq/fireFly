#include <cstdio>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "triangulation.h"
#include "delaunay.h"
#include "helper.h"
#include "common.h"

#define MAX_TRIANG_POINTS 1200

#ifndef INFINITY
#define INFINITY 1e30
#endif

/*
static double tri_dist2(point_t *a, point_t *b) {
  double dx = a->x - b->x, dy = a->y - b->y;
  return dx * dx + dy * dy;
}

static void compute_circumcircle(point_t *points, Triangle *tri) {
  point_t *p1 = points + tri->p1;
  point_t *p2 = points + tri->p2;
  point_t *p3 = points + tri->p3;
  double d = 2.0 * (p1->x * (p2->y - p3->y) + p2->x * (p3->y - p1->y) +
                    p3->x * (p1->y - p2->y));
  if (d == 0) {
    tri->r2 = 1e30;
    return;
  }
  double ux = ((p1->x * p1->x + p1->y * p1->y) * (p2->y - p3->y) +
               (p2->x * p2->x + p2->y * p2->y) * (p3->y - p1->y) +
               (p3->x * p3->x + p3->y * p3->y) * (p1->y - p2->y)) / d;
  double uy = ((p1->x * p1->x + p1->y * p1->y) * (p3->x - p2->x) +
               (p2->x * p2->x + p2->y * p2->y) * (p1->x - p3->x) +
               (p3->x * p3->x + p3->y * p3->y) * (p2->x - p1->x)) / d;
  tri->cx = ux;
  tri->cy = uy;
  tri->r2 = tri_dist2(p1, &(point_t){.x = (float)ux, .y = (float)uy, .z = 0, .height = 0, .id = 0});
  if (tri->r2 < 1e-20)
    tri->r2 = 1e-20;
}
*/

/*
  Если тло больше чем нужно (n <= MAX_TRIANG_POINTS), производим фильтрацию. Строим сетку,
  в которую поместятся все отфильтрованные точки (для каждой точки выделена ячейка этой сетки).
  Размер сетки sqrt(MAX_TRIANG_POINTS). Из всего массива тло остаются только точки которые будут
  наиболее близки к центру ячейки образовавшейся сетки в которой они лежат.  
*/

static int subsample(point_t *points, int n) { 
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
    
    int g = (int)ceil(sqrt((double)MAX_TRIANG_POINTS)); // размер сетки
    if (g < 3) g = 3;
    
    // массив индексов лучших точкек в каждой ячейке
    int *best = (int *)calloc((size_t)(g * g), sizeof(int));
    // массив расстояния до центра этой точки
    double *d2 = (double *)malloc((size_t)(g * g) * sizeof(double));
    if (!best || !d2) {
        fprintf(stderr, "FAIL FROM MEMORY ALLOCATE\n");
        free(best);
        free(d2);
        return n;
    }

    for (int i = 0; i < g * g; i++)
        d2[i] = INFINITY;
    
    // Размер ячейки
    double sx = (max_x > min_x) ? (max_x - min_x) / g : 1.;
    double sy = (max_y > min_y) ? (max_y - min_y) / g : 1.;
    
    // Проходим по всем точкам
    for (int i = 0; i < n; i++) {
        // Определяем индекс ячейки по левой границе
        int gx = (int)((points[i].x - min_x) / sx);
        int gy = (int)((points[i].y - min_y) / sy);
        
        if (gx >= g) gx = g - 1;
        else if (gx < 0) gx = 0;
        if (gy >= g) gy = g - 1;
        else if (gy < 0) gy = 0;
        
        int idx = gy * g + gx;
        
        // центр ячейки
        double cx = min_x + (gx + 0.5) * sx;
        double cy = min_y + (gy + 0.5) * sy;

        double dx = points[i].x - cx;
        double dy = points[i].y - cy;
        double dist = dx * dx + dy * dy;
        
        if (dist < d2[idx]) {
            d2[idx] = dist;
            best[idx] = i;
        }
    }

    point_t *tmp = (point_t *)malloc((size_t)(g * g) * sizeof(point_t));
    if (!tmp) {
        fprintf(stderr, "FAIL FROM MEMORY ALLOCATE\n");
        memDestroy(2, (void*)best, (void*)d2);
        return n;
    }
    
    int out = 0;
    for (int i = 0; i < g * g; i++) {
        if (d2[i] < INFINITY) {
            tmp[out++] = points[best[i]];
        }
    }


    memDestroy(2, (void*)best, (void*)d2);
    
    if (out >= 3) {
        memcpy(points, tmp, (size_t)out * sizeof(point_t));
        free(tmp);
        return out;
    }
    
    free(tmp);
    return n;
}

Triangle *delaunay_triangulation(point_t *points, int *num_points,
                                 int *num_triangles) {
  int n;
  if ((n = subsample(points, *num_points)) < 3) {
    fprintf(stderr, "COUNT POINTS IS LOSS\n");
    return NULL;
  }

  float *pt = (float *)malloc((size_t)n * 2 * sizeof(float));
  if (!pt) {
    *num_triangles = 0;
    return NULL;
  }

  for (int i = 0; i < n; i++) {
    pt[i * 2]     = points[i].x;
    pt[i * 2 + 1] = points[i].y;
  }

  size_t dsz = (size_t)DELAUNAY_SZ(n) * sizeof(uint32_t);
  uint32_t *delaunay = (uint32_t *)malloc(dsz);
  if (!delaunay) {
    free(pt);
    *num_triangles = 0;
    return NULL;
  }

  int err = triangulate(delaunay, pt, (uint32_t)n);
  free(pt);

  if (err != 0) {
    free(delaunay);
    *num_triangles = 0;
    return NULL;
  }

  uint32_t *triverts = DELAUNAY_TRIVERTS(delaunay, n);  // массив треугольников 
  uint32_t ntrivert = *DELAUNAY_NTRIVERT(delaunay, n);  // кол-во вершин треугольников
  uint32_t ntri = ntrivert / 3;                         // кол-во треугольников

  if (ntri == 0) {
    free(delaunay);
    *num_triangles = 0;
    return NULL;
  }

  Triangle *tri = (Triangle *)malloc((size_t)ntri * sizeof(Triangle));
  if (!tri) {
    free(delaunay);
    *num_triangles = 0;
    return NULL;
  }

  for (uint32_t i = 0; i < ntri; i++) {
    tri[i].p1 = (int)triverts[i * 3];
    tri[i].p2 = (int)triverts[i * 3 + 1];
    tri[i].p3 = (int)triverts[i * 3 + 2];
    // tri[i].flag = 0;
    // compute_circumcircle(points, &tri[i]);
  }
  free(delaunay);

  *num_triangles = (int)ntri;
  return tri;
}

typedef struct {
  int a, b; // a < b
  int tri;
  int edge; // 0..2
  int used;
} edge_item_t;

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

size_t triangulation_find_polygons(Triangle* tri, size_t countTriangle,
                                   const point_t* points, size_t countPoints,
                                   tri_correct_fn is_correct,
                                   polygon_t** outPolys) {
  if (!outPolys) return 0;
  *outPolys = NULL;
  if (!tri || countTriangle == 0 || !points || countPoints == 0 || !is_correct)
    return 0;

  // mark good triangles and reset labels
  unsigned char* good = (unsigned char*)malloc(countTriangle);
  if (!good) return 0;
  for (size_t i = 0; i < countTriangle; i++) {
    tri[i].numPolygon = 0;
    good[i] = (unsigned char)(is_correct(&tri[i], points) ? 1 : 0);
  }

  // triangle neighbors: for each triangle, neighbor across edge 0..2 or -1
  int* neigh = (int*)malloc(countTriangle * 3 * sizeof(int));
  if (!neigh) {
    free(good);
    return 0;
  }
  for (size_t i = 0; i < countTriangle * 3; i++) neigh[i] = -1;

  // hash edges to build adjacency
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

  // insert/find with open addressing; empty key encoded by 0xFFFFFFFF_FFFFFFFF
  // use a bias: store key+1, since our key may be 0.
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

  // BFS over good triangles to assign polygon ids and extract boundary edges per component
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

    // BFS component
    size_t qh = 0, qt = 0;
    queue[qt++] = (int)start;
    tri[start].numPolygon = currentId;

    // reset boundary list for this component
    bCnt = 0;

    while (qh < qt) {
      int tIdx = queue[qh++];
      Triangle* t = &tri[tIdx];

      // for each edge, either internal (neighbor good) or boundary (neighbor missing or not good)
      for (int e = 0; e < 3; e++) {
        int nb = neigh[(size_t)tIdx * 3 + e];
        if (nb >= 0 && good[(size_t)nb]) {
          if (tri[nb].numPolygon == 0) {
            tri[nb].numPolygon = currentId;
            queue[qt++] = nb;
          }
          continue;
        }

        // boundary edge
        int a = tri_vert(t, e);
        int b = tri_vert(t, (e + 1) % 3);
        if (a > b) { int tmp = a; a = b; b = tmp; }
        if (bCnt == bCap) {
          size_t newCap = bCap ? bCap * 2 : 256;
          edge_item_t* nbnd = (edge_item_t*)realloc(boundary, newCap * sizeof(edge_item_t));
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
        boundary[bCnt++] = (edge_item_t){ .a = a, .b = b, .tri = tIdx, .edge = e, .used = 0 };
      }
    }

    if (bCnt == 0) continue;

    // Build adjacency for boundary graph: for each vertex -> list of edge indices
    // We'll index vertices by their id (0..countPoints-1) but boundary may include any within range.
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
      // a -> b
      to[ec] = b; edgeId[ec] = (int)i; next[ec] = head[a]; head[a] = ec; ec++;
      // b -> a
      to[ec] = a; edgeId[ec] = (int)i; next[ec] = head[b]; head[b] = ec; ec++;
    }

    // Extract loops: walk unused boundary edges
    for (size_t ei = 0; ei < bCnt; ei++) {
      if (boundary[ei].used) continue;

      int startA = boundary[ei].a;
      int startB = boundary[ei].b;
      boundary[ei].used = 1;

      // dynamic contour buffer
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
        if (!found) break; // open chain / non-manifold boundary
      }

      // store polygon
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
