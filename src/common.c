#include <stdlib.h>
#include <stdio.h>

#include "common.h"

void* memoryAlloc(size_t size) {
    void* res = malloc(size);
    if (!res) {
        fprintf(stderr, "FAILED FROM MEMORY ALLOCATE\n");
        return NULL;
    }
    return res;
}

void* memoryRealloc(void* ptr, size_t size) {
    void* res = realloc(ptr, size);
    if (!res) {
        free(ptr);
        fprintf(stderr, "FAILED FROM MEMORY ALLOCATE\n");
        return NULL;
    }
    return res;
}

void memDestroy(int count, ...) {
  va_list args;
  va_start(args, count);
  for (int i = 0; i < count; i++) {
    free(va_arg(args, void*));
  }
  va_end(args);
}

void* expandMemory(void* ptr, size_t* bufferSize) {
    *bufferSize *= 2;
    return memoryRealloc(ptr, *bufferSize);
}