#ifndef RZP_H
#define RZP_H

#include "common.h"

#define RZP_MATRIX_SIZE ((FRG_SIZE_X + 2) * (FRG_SIZE_Y + 2))

extern const FrgIndex RZP_TILES[];

char getRZPMtrx(const char* path, FrgIndex index, int* heights);
int getRZPHeight(const int *height, int ix, int iy);

#endif
