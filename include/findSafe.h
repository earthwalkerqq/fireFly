#ifndef FIND_SAFE_H
#define FIND_SAFE_H

#include "triangulation.h"
#include "tlo.h"

#define MAX_HEIGTH 5.
#define ANGLE_SLOPE 0.1 // 10%

void varifyTriangles(Triangle* tri, point_t* points, size_t size, TloRender* tlo);

#endif