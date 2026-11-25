#ifndef HFOC_LWIP_H
#define HFOC_LWIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HFOC_LWIP_ALLOC_VERSION 1U
#define HFOC_LWIP_POOL_COUNT 4U

#define HFOC_LWIP_POOL_CLASS_TINY 0U
#define HFOC_LWIP_POOL_CLASS_SMALL 1U
#define HFOC_LWIP_POOL_CLASS_MEDIUM 2U
#define HFOC_LWIP_POOL_CLASS_LARGE 3U

#define HFOC_LWIP_POOL_SLOT_SIZE_TINY 64U
#define HFOC_LWIP_POOL_SLOT_SIZE_SMALL 256U
#define HFOC_LWIP_POOL_SLOT_SIZE_MEDIUM 1024U
#define HFOC_LWIP_POOL_SLOT_SIZE_LARGE 4096U

#define HFOC_LWIP_POOL_SLOT_COUNT_TINY 32U
#define HFOC_LWIP_POOL_SLOT_COUNT_SMALL 16U
#define HFOC_LWIP_POOL_SLOT_COUNT_MEDIUM 8U
#define HFOC_LWIP_POOL_SLOT_COUNT_LARGE 4U

#define HFOC_LWIP_POOL_SLOT_COUNT_MAX 32U
#define HFOC_LWIP_ALLOC_MAX_SIZE HFOC_LWIP_POOL_SLOT_SIZE_LARGE

#define HFOC_LWIP_ALLOC_TOTAL_BYTES \
    ((HFOC_LWIP_POOL_SLOT_SIZE_TINY * HFOC_LWIP_POOL_SLOT_COUNT_TINY) + \
     (HFOC_LWIP_POOL_SLOT_SIZE_SMALL * HFOC_LWIP_POOL_SLOT_COUNT_SMALL) + \
     (HFOC_LWIP_POOL_SLOT_SIZE_MEDIUM * HFOC_LWIP_POOL_SLOT_COUNT_MEDIUM) + \
     (HFOC_LWIP_POOL_SLOT_SIZE_LARGE * HFOC_LWIP_POOL_SLOT_COUNT_LARGE))

struct hfoc_lwip_alloc_pool_snapshot {
    uint32_t slot_size;
    uint32_t slot_count;
    uint32_t free_count;
    uint32_t used_count;
    uint32_t high_water_count;
};

struct hfoc_lwip_alloc_snapshot {
    uint32_t version;
    uint32_t total_used_bytes;
    uint32_t total_free_bytes;
    uint32_t total_high_water_bytes;
    struct hfoc_lwip_alloc_pool_snapshot pools[HFOC_LWIP_POOL_COUNT];
};

bool hfoc_lwip_alloc_init(void);
void hfoc_lwip_alloc_shutdown(void);
bool hfoc_lwip_alloc_ready(void);
void *hfoc_lwip_alloc(size_t size);
void *hfoc_lwip_alloc_calloc(size_t count, size_t size);
bool hfoc_lwip_alloc_free(void *ptr);
struct hfoc_lwip_alloc_snapshot hfoc_lwip_alloc_snapshot(void);
uint32_t hfoc_lwip_alloc_free_count(unsigned int pool_class);
uint32_t hfoc_lwip_alloc_used_count(unsigned int pool_class);
uint32_t hfoc_lwip_alloc_high_water_count(unsigned int pool_class);

void *hfoc_lwip_mem_malloc(size_t size);
void *hfoc_lwip_mem_calloc(size_t count, size_t size);
void hfoc_lwip_mem_free(void *ptr);

#endif
