/*
    MIT License
    Copyright (c) 2021 Adrian Reuter
    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:
    The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
   "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
   LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stddef.h>
#include <stdint.h>

#include "mem_pool.h"

struct block {
  void *next;
};

int mp_init(size_t bs, size_t bc, void *m, mp_pool_t *mp) {
  if (bs < sizeof(size_t)) {
    return -1;
  }
  mp->initialBlocks = bc;
  mp->start = m;
  mp->end = (void *)&((uint8_t *)m)[bs * bc];
  mp->bs = bs;
  mp->ul_bc = bc;
  mp->b = NULL;
  mp->ul_b = m;
  mp->high_water = 0; /* EF */
  mp->count = 0;      /* EF */
  return 0;
}

void *mp_malloc(mp_pool_t *mp) {
  /*
   * 1. First we try to allocate an unlinked block
   * 2. In case there are no more unlinked blocks left we try to return the head
   * from the list of free blocks
   * 3. Otherwise we will have to abort since there are no free blocks left
   */
  void *ret = NULL;/* EF */
  if (mp->ul_bc > 0) {
    mp->ul_bc--;
    void *b = mp->ul_b;
    mp->ul_b = (void *)(((unsigned char *)mp->ul_b) + mp->bs);
    ret = b; /* EF */
  } else if (mp->b) {
    void *b = mp->b;
    mp->b = ((struct block *)mp->b)->next;
    ret = b; /* EF */
  }
  if (ret != NULL) { /* EF */
    mp->count++;
    if (mp->count > mp->high_water) {
      mp->high_water = mp->count;
    }
  }
  return ret;
}

void mp_free(mp_pool_t *mp, void *b) {
  /*
   * We add b as the head of the list of free blocks
   */
  ((struct block *)b)->next = mp->b;
  mp->b = b;
  mp->count--; /* EF */
}

uint32_t mp_integrity(mp_pool_t *mp, mp_info_t *info) {
  info->freeCount = 0;
  info->high_water = mp->high_water;
  info->count = mp->count;
  info->blockCount = mp->initialBlocks;
  // Walk the free list
  void *mp_b = mp->b;
  while (mp_b) {

    void *b = mp_b;
    mp_b = ((struct block *)mp_b)->next;
    // Does next land inside the parameters?
    if (mp_b && (mp_b < mp->start || mp_b >= mp->end)) {
      return 0;
    }
    info->freeCount++;
  }

  if (mp->initialBlocks - mp->count != info->freeCount + mp->ul_bc) {
    return 0;
  }
  // Does it match count?
  return 1;
}
