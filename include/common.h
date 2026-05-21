#ifndef COMMON_H
#define COMMON_H

#include <stdarg.h>
#include <stdio.h>
#include <cglm/cglm.h>
#include <stdlib.h>

#define PATH_FRG_SHADER "./shaders/shader.frg"
#define PATH_VRX_SHADER "./shaders/shader.vrx"
#define PATH_DATA "./materials/BLSP_256"

#define TERRAIN_QUAD_SIZE 25

#define FRG_SIZE_X 256
#define FRG_SIZE_Y 256

char drawMode;

typedef struct {
  int i, j;
} FrgIndex;

void* memoryAlloc(size_t size);
void* memoryRealloc(void* ptr, size_t size);
void memDestroy(int count, ...);

#endif
