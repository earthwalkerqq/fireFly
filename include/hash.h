#ifndef HASH_H
#define HASH_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

uint64_t edge_key_u32(int a, int b);
size_t hash_u64(uint64_t x);
size_t next_pow2(size_t v);

#endif
