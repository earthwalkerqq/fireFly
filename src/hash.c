#include "hash.h"

uint64_t edge_key_u32(int a, int b) {
  uint32_t x = (uint32_t)a, y = (uint32_t)b;
  return ((uint64_t)x << 32) | (uint64_t)y;
}

size_t next_pow2(size_t v) {
  size_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

size_t hash_u64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  x = x ^ (x >> 31);
  return (size_t)x;
}
