#include "findSafe.h"

#include <math.h>
#include <stdlib.h>

ZoneWeights zoneWeightsDefault(void) {
  /* Значения настроены под аппарат среднего класса; сумма равна 1.0. */
  ZoneWeights w = {
    .wArea      = 0.30,
    .wMeanSlope = 0.20,
    .wMaxSlope  = 0.15,
    .wCompact   = 0.15,
    .wClearance = 0.20,
  };
  return w;
}

/* Площадь треугольника в плановой проекции XY. */
static double triAreaXY(const Triangle *t, const point_t *p) {
  const point_t *a = &p[t->p1], *b = &p[t->p2], *c = &p[t->p3];
  double cross = (double)(b->x - a->x) * (double)(c->y - a->y) -
                 (double)(b->y - a->y) * (double)(c->x - a->x);
  return 0.5 * fabs(cross);
}

/* Уклон треугольника: перепад высот, нормированный на длину наименьшего ребра. */
static double triSlope(const Triangle *t, const point_t *p) {
  const point_t *a = &p[t->p1], *b = &p[t->p2], *c = &p[t->p3];

  double maxZ = a->z, minZ = a->z;
  if (b->z > maxZ) maxZ = b->z;
  if (b->z < minZ) minZ = b->z;
  if (c->z > maxZ) maxZ = c->z;
  if (c->z < minZ) minZ = c->z;

  double dx, dy;
  dx = (double)a->x - b->x; dy = (double)a->y - b->y;
  double e1 = sqrt(dx * dx + dy * dy);
  dx = (double)b->x - c->x; dy = (double)b->y - c->y;
  double e2 = sqrt(dx * dx + dy * dy);
  dx = (double)c->x - a->x; dy = (double)c->y - a->y;
  double e3 = sqrt(dx * dx + dy * dy);

  double minEdge = e1;
  if (e2 < minEdge) minEdge = e2;
  if (e3 < minEdge) minEdge = e3;
  if (minEdge <= 1e-9) return 0.0;
  return (maxZ - minZ) / minEdge;
}

/* Нормировка критерия "больше — лучше" к диапазону [0..1]. */
static double normHi(double v, double lo, double hi) {
  if (hi - lo <= 1e-12) return 1.0;
  double t = (v - lo) / (hi - lo);
  if (t < 0.0) return 0.0;
  if (t > 1.0) return 1.0;
  return t;
}

/* Нормировка критерия "меньше — лучше" к диапазону [0..1]. */
static double normLo(double v, double lo, double hi) {
  return 1.0 - normHi(v, lo, hi);
}

/* Компаратор для сортировки зон по убыванию интегрального показателя. */
static int cmpZoneByScore(const void *lhs, const void *rhs) {
  const SafeZone *a = (const SafeZone *)lhs;
  const SafeZone *b = (const SafeZone *)rhs;
  if (a->score < b->score) return 1;
  if (a->score > b->score) return -1;
  return 0;
}

size_t rankSafeZones(const Triangle *tri, size_t countTri, const point_t *pts,
                     const unsigned char *triIsSafe, const double *triClearance,
                     unsigned maxPid, ZoneWeights w, double landingRadius,
                     SafeZone **outZones, int *triRank) {
  if (outZones) *outZones = NULL;
  if (triRank) {
    for (size_t i = 0; i < countTri; i++) triRank[i] = -1;
  }
  if (!tri || !pts || !triIsSafe || countTri == 0 || maxPid == 0)
    return 0;

  size_t naccum = (size_t)maxPid + 1;

  /* Аккумуляторы частных критериев по связным областям. */
  size_t *cnt    = (size_t *)calloc(naccum, sizeof(size_t));
  double *areaS  = (double *)calloc(naccum, sizeof(double));
  double *slpS   = (double *)calloc(naccum, sizeof(double));
  double *slpMax = (double *)calloc(naccum, sizeof(double));
  double *clrS   = (double *)calloc(naccum, sizeof(double));
  double *bbMinX = (double *)malloc(naccum * sizeof(double));
  double *bbMaxX = (double *)malloc(naccum * sizeof(double));
  double *bbMinY = (double *)malloc(naccum * sizeof(double));
  double *bbMaxY = (double *)malloc(naccum * sizeof(double));
  /* Лучший по удалённости от границ треугольник внутри зоны —
     его центроид становится «центром» круглой посадочной области. */
  double *bestClr = (double *)calloc(naccum, sizeof(double));
  double *bestCx  = (double *)calloc(naccum, sizeof(double));
  double *bestCy  = (double *)calloc(naccum, sizeof(double));
  double *bestCz  = (double *)calloc(naccum, sizeof(double));
  if (!cnt || !areaS || !slpS || !slpMax || !clrS ||
      !bbMinX || !bbMaxX || !bbMinY || !bbMaxY ||
      !bestClr || !bestCx || !bestCy || !bestCz) {
    free(cnt); free(areaS); free(slpS); free(slpMax); free(clrS);
    free(bbMinX); free(bbMaxX); free(bbMinY); free(bbMaxY);
    free(bestClr); free(bestCx); free(bestCy); free(bestCz);
    return 0;
  }
  for (size_t i = 0; i < naccum; i++) {
    bbMinX[i] = bbMinY[i] =  1e300;
    bbMaxX[i] = bbMaxY[i] = -1e300;
    bestClr[i] = -1.0; /* < 0 — значит ещё не назначен */
  }

  /* Накопление характеристик по безопасным треугольникам. */
  for (size_t ti = 0; ti < countTri; ti++) {
    if (!triIsSafe[ti]) continue;
    unsigned pid = tri[ti].numPolygon;
    if (pid == 0 || pid > maxPid) continue;

    double area = triAreaXY(&tri[ti], pts);
    double slope = triSlope(&tri[ti], pts);
    double clr = triClearance ? triClearance[ti] : 0.0;

    cnt[pid]++;
    areaS[pid] += area;
    slpS[pid]  += slope;
    if (slope > slpMax[pid]) slpMax[pid] = slope;
    clrS[pid]  += clr;

    int idx[3] = { tri[ti].p1, tri[ti].p2, tri[ti].p3 };
    for (int k = 0; k < 3; k++) {
      double x = pts[idx[k]].x, y = pts[idx[k]].y;
      if (x < bbMinX[pid]) bbMinX[pid] = x;
      if (x > bbMaxX[pid]) bbMaxX[pid] = x;
      if (y < bbMinY[pid]) bbMinY[pid] = y;
      if (y > bbMaxY[pid]) bbMaxY[pid] = y;
    }

    /* Запоминаем центроид треугольника, наиболее удалённого от границ
       связной области — это и есть кандидат на центр посадочного круга. */
    if (clr > bestClr[pid]) {
      const point_t *a = &pts[idx[0]];
      const point_t *b = &pts[idx[1]];
      const point_t *c = &pts[idx[2]];
      bestClr[pid] = clr;
      bestCx[pid]  = ((double)a->x + b->x + c->x) / 3.0;
      bestCy[pid]  = ((double)a->y + b->y + c->y) / 3.0;
      bestCz[pid]  = ((double)a->z + b->z + c->z) / 3.0;
    }
  }

  /* Подсчёт числа зон, содержащих безопасные треугольники. */
  size_t numZones = 0;
  for (unsigned pid = 1; pid <= maxPid; pid++)
    if (cnt[pid] > 0) numZones++;

  if (numZones == 0) {
    free(cnt); free(areaS); free(slpS); free(slpMax); free(clrS);
    free(bbMinX); free(bbMaxX); free(bbMinY); free(bbMaxY);
    return 0;
  }

  SafeZone *zones = (SafeZone *)malloc(numZones * sizeof(SafeZone));
  if (!zones) {
    free(cnt); free(areaS); free(slpS); free(slpMax); free(clrS);
    free(bbMinX); free(bbMaxX); free(bbMinY); free(bbMaxY);
    return 0;
  }

  /* Формирование описаний зон и расчёт частных критериев. */
  size_t z = 0;
  for (unsigned pid = 1; pid <= maxPid; pid++) {
    if (cnt[pid] == 0) continue;

    double bw = bbMaxX[pid] - bbMinX[pid];
    double bh = bbMaxY[pid] - bbMinY[pid];
    double bbArea = bw * bh;
    /* Компактность — доля площади зоны в её габаритном прямоугольнике. */
    double comp = (bbArea > 1e-9) ? areaS[pid] / bbArea : 0.0;
    if (comp > 1.0) comp = 1.0;
    if (comp < 0.0) comp = 0.0;

    zones[z].pid         = pid;
    zones[z].rank        = 0;
    zones[z].triCount    = cnt[pid];
    zones[z].area        = areaS[pid];
    zones[z].meanSlope   = slpS[pid] / (double)cnt[pid];
    zones[z].maxSlope    = slpMax[pid];
    zones[z].compactness = comp;
    zones[z].clearance   = clrS[pid] / (double)cnt[pid];
    zones[z].score       = 0.0;
    zones[z].cx          = bestCx[pid];
    zones[z].cy          = bestCy[pid];
    zones[z].cz          = bestCz[pid];
    zones[z].maxInscribed = (bestClr[pid] > 0.0) ? bestClr[pid] : 0.0;
    z++;
  }

  free(cnt); free(areaS); free(slpS); free(slpMax); free(clrS);
  free(bbMinX); free(bbMaxX); free(bbMinY); free(bbMaxY);
  free(bestClr); free(bestCx); free(bestCy); free(bestCz);

  /* Диапазоны частных критериев для нормировки. */
  double aLo  = zones[0].area,        aHi  = zones[0].area;
  double msLo = zones[0].meanSlope,   msHi = zones[0].meanSlope;
  double xsLo = zones[0].maxSlope,    xsHi = zones[0].maxSlope;
  double cLo  = zones[0].compactness, cHi  = zones[0].compactness;
  double clLo = zones[0].clearance,   clHi = zones[0].clearance;
  for (size_t i = 1; i < numZones; i++) {
    if (zones[i].area < aLo) aLo = zones[i].area;
    if (zones[i].area > aHi) aHi = zones[i].area;
    if (zones[i].meanSlope < msLo) msLo = zones[i].meanSlope;
    if (zones[i].meanSlope > msHi) msHi = zones[i].meanSlope;
    if (zones[i].maxSlope < xsLo) xsLo = zones[i].maxSlope;
    if (zones[i].maxSlope > xsHi) xsHi = zones[i].maxSlope;
    if (zones[i].compactness < cLo) cLo = zones[i].compactness;
    if (zones[i].compactness > cHi) cHi = zones[i].compactness;
    if (zones[i].clearance < clLo) clLo = zones[i].clearance;
    if (zones[i].clearance > clHi) clHi = zones[i].clearance;
  }

  /* Интегральный показатель как взвешенная свёртка нормированных критериев. */
  double wsum = w.wArea + w.wMeanSlope + w.wMaxSlope + w.wCompact + w.wClearance;
  if (wsum <= 1e-9) wsum = 1.0;
  for (size_t i = 0; i < numZones; i++) {
    double nArea  = normHi(zones[i].area,        aLo,  aHi);
    double nMean  = normLo(zones[i].meanSlope,   msLo, msHi);
    double nMax   = normLo(zones[i].maxSlope,    xsLo, xsHi);
    double nComp  = normHi(zones[i].compactness, cLo,  cHi);
    double nClear = normHi(zones[i].clearance,   clLo, clHi);
    zones[i].score = (w.wArea      * nArea  +
                      w.wMeanSlope * nMean  +
                      w.wMaxSlope  * nMax   +
                      w.wCompact   * nComp  +
                      w.wClearance * nClear) / wsum;
  }

  /* Сортировка по убыванию показателя и присвоение рангов. */
  qsort(zones, numZones, sizeof(SafeZone), cmpZoneByScore);
  for (size_t i = 0; i < numZones; i++) zones[i].rank = (int)i;

  /* Проставление ранга зоны треугольникам её посадочного диска. */
  if (triRank) {
    int    *pidRank = (int *)malloc((size_t)(maxPid + 1) * sizeof(int));
    double *pidCx   = (double *)malloc((size_t)(maxPid + 1) * sizeof(double));
    double *pidCy   = (double *)malloc((size_t)(maxPid + 1) * sizeof(double));
    if (pidRank && pidCx && pidCy) {
      for (unsigned p = 0; p <= maxPid; p++) pidRank[p] = -1;
      for (size_t i = 0; i < numZones; i++) {
        pidRank[zones[i].pid] = zones[i].rank;
        pidCx[zones[i].pid]   = zones[i].cx;
        pidCy[zones[i].pid]   = zones[i].cy;
      }
      /* Ранг зоны (и заливку) получают треугольники двух категорий:
         1) идеальный посадочный диск — те, что попадают в круг радиуса
            landingRadius вокруг центра зоны (там же белый контур); это
            гарантирует, что подсвеченный круг залит целиком;
         2) прочие места в полигоне, куда вписывается окружность ЛА
            (triIsSafe) — менее релевантные, но тоже пригодные кандидаты.
         Проверка pid не даёт заливке вытечь за пределы своей связной
         области, даже если диск геометрически перекрывает соседнюю. */
      double r2 = landingRadius * landingRadius;
      for (size_t ti = 0; ti < countTri; ti++) {
        unsigned pid = tri[ti].numPolygon;
        if (pid < 1 || pid > maxPid || pidRank[pid] < 0)
          continue;

        int inZone = triIsSafe[ti]; /* допустимый центр посадки */
        if (!inZone) {
          const point_t *a = &pts[tri[ti].p1];
          const point_t *b = &pts[tri[ti].p2];
          const point_t *c = &pts[tri[ti].p3];
          double cx = ((double)a->x + b->x + c->x) / 3.0;
          double cy = ((double)a->y + b->y + c->y) / 3.0;
          double dx = cx - pidCx[pid];
          double dy = cy - pidCy[pid];
          inZone = (dx * dx + dy * dy <= r2); /* внутри идеального диска */
        }
        if (inZone)
          triRank[ti] = pidRank[pid];
      }
    }
    free(pidRank);
    free(pidCx);
    free(pidCy);
  }

  if (outZones) *outZones = zones;
  else free(zones);
  return numZones;
}
