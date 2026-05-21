#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "common.h"
#include "rzp.h"

#define BUFFER_SIZE 1024

const FrgIndex RZP_TILES[] = { {0, 0} };


char getRZPMtrx(const char* path, FrgIndex index, int* height) {
  char pathRZP[BUFFER_SIZE];
  snprintf(pathRZP, BUFFER_SIZE, "%s/RZP/%d/%d", path, index.i, index.j);

 struct stat st;
 if (stat(pathRZP, &st)) {
   fprintf(stderr, "FILE %s NOT EXIST\n", pathRZP);
   return 0;
 }

 FILE* fd = NULL;
 if (!(fd = fopen(pathRZP, "rb"))) {
   fprintf(stderr, "FAILED FROM OPEN FILE\n");
   return 0;
 }

 size_t sizeRZPMtrx = RZP_MATRIX_SIZE * sizeof(int);
 size_t hasBeenRead = 0;

 if ((hasBeenRead = fread(height, 1, sizeRZPMtrx, fd)) != sizeRZPMtrx) {
   if (!hasBeenRead) {
     fprintf(stderr, "FAIL FROM READ FILE RZP\n");
     fclose(fd);
     return 0;
   }
   fprintf(stderr, "FILE RZP HAS BEEN READING NOT FULL\n");
 }
 fclose(fd);
 return 1;
}

int getRZPHeight(const int* height, int ix, int iy) {
  return height[(FRG_SIZE_X + 2) * (ix + 1) + iy];
}
