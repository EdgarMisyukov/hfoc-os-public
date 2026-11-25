#ifndef HFOC_HEAP_H
#define HFOC_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HFOC_HEAP_SIZE      (4U * 1024U * 1024U)
#define HFOC_HEAP_ALIGNMENT 16U
#define HFOC_HEAP_MIN_BLOCK 32U

bool heap_init(uint64_t hhdm_offset);
bool heap_ready(void);
void *kmalloc(size_t size);
bool kfree(void *ptr);
size_t heap_capacity_bytes(void);
size_t heap_used_bytes(void);

#endif
