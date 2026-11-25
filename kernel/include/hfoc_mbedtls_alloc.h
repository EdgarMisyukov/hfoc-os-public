#ifndef HFOC_MBEDTLS_ALLOC_H
#define HFOC_MBEDTLS_ALLOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HFOC_MBEDTLS_ALLOC_VERSION 1U
#define HFOC_MBEDTLS_POOL_COUNT 4U
#define HFOC_MBEDTLS_CONNECTION_COUNT 4U

#define HFOC_MBEDTLS_POOL_CLASS_TINY 0U
#define HFOC_MBEDTLS_POOL_CLASS_SMALL 1U
#define HFOC_MBEDTLS_POOL_CLASS_MEDIUM 2U
#define HFOC_MBEDTLS_POOL_CLASS_LARGE 3U

#define HFOC_MBEDTLS_POOL_SLOT_SIZE_TINY 64U
#define HFOC_MBEDTLS_POOL_SLOT_SIZE_SMALL 256U
#define HFOC_MBEDTLS_POOL_SLOT_SIZE_MEDIUM 1024U
#define HFOC_MBEDTLS_POOL_SLOT_SIZE_LARGE 4096U

/*
 * ITEM-386: the ordinary production boot terminates TLS itself, so the
 * production image needs the connection-sized arena the ITEM-270 QEMU TLS
 * fixture proved, not the tiny verification-only pool.  The host-test and
 * QEMU arms are unchanged.
 */
#if defined(HFOC_MBEDTLS_TLS_QEMU_TEST) || defined(HFOC_PRODUCTION_TLS)
#define HFOC_MBEDTLS_CONNECTION_MAX_TINY 128U
#define HFOC_MBEDTLS_CONNECTION_MAX_SMALL 64U
#define HFOC_MBEDTLS_CONNECTION_MAX_MEDIUM 32U
#define HFOC_MBEDTLS_CONNECTION_MAX_LARGE 16U
#define HFOC_MBEDTLS_POOL_SLOT_COUNT_TINY 256U
#define HFOC_MBEDTLS_POOL_SLOT_COUNT_SMALL 128U
#define HFOC_MBEDTLS_POOL_SLOT_COUNT_MEDIUM 64U
#define HFOC_MBEDTLS_POOL_SLOT_COUNT_LARGE 16U
#define HFOC_MBEDTLS_POOL_SLOT_COUNT_MAX 256U
#else
#ifndef HFOC_MBEDTLS_CONNECTION_MAX_TINY
#define HFOC_MBEDTLS_CONNECTION_MAX_TINY 8U
#endif
#ifndef HFOC_MBEDTLS_CONNECTION_MAX_SMALL
#define HFOC_MBEDTLS_CONNECTION_MAX_SMALL 4U
#endif
#ifndef HFOC_MBEDTLS_CONNECTION_MAX_MEDIUM
#define HFOC_MBEDTLS_CONNECTION_MAX_MEDIUM 2U
#endif
#ifndef HFOC_MBEDTLS_CONNECTION_MAX_LARGE
#define HFOC_MBEDTLS_CONNECTION_MAX_LARGE 1U
#endif

#ifndef HFOC_MBEDTLS_POOL_SLOT_COUNT_TINY
#define HFOC_MBEDTLS_POOL_SLOT_COUNT_TINY 32U
#endif
#ifndef HFOC_MBEDTLS_POOL_SLOT_COUNT_SMALL
#define HFOC_MBEDTLS_POOL_SLOT_COUNT_SMALL 16U
#endif
#ifndef HFOC_MBEDTLS_POOL_SLOT_COUNT_MEDIUM
#define HFOC_MBEDTLS_POOL_SLOT_COUNT_MEDIUM 8U
#endif
#ifndef HFOC_MBEDTLS_POOL_SLOT_COUNT_LARGE
#define HFOC_MBEDTLS_POOL_SLOT_COUNT_LARGE 4U
#endif

#ifndef HFOC_MBEDTLS_POOL_SLOT_COUNT_MAX
#define HFOC_MBEDTLS_POOL_SLOT_COUNT_MAX 32U
#endif
#endif
#define HFOC_MBEDTLS_ALLOC_MAX_SIZE HFOC_MBEDTLS_POOL_SLOT_SIZE_LARGE

#define HFOC_MBEDTLS_ALLOC_TOTAL_BYTES \
    ((HFOC_MBEDTLS_POOL_SLOT_SIZE_TINY * \
      HFOC_MBEDTLS_POOL_SLOT_COUNT_TINY) + \
     (HFOC_MBEDTLS_POOL_SLOT_SIZE_SMALL * \
      HFOC_MBEDTLS_POOL_SLOT_COUNT_SMALL) + \
     (HFOC_MBEDTLS_POOL_SLOT_SIZE_MEDIUM * \
      HFOC_MBEDTLS_POOL_SLOT_COUNT_MEDIUM) + \
     (HFOC_MBEDTLS_POOL_SLOT_SIZE_LARGE * \
      HFOC_MBEDTLS_POOL_SLOT_COUNT_LARGE))

#define HFOC_MBEDTLS_CONNECTION_BUDGET_BYTES \
    ((HFOC_MBEDTLS_POOL_SLOT_SIZE_TINY * \
      HFOC_MBEDTLS_CONNECTION_MAX_TINY) + \
     (HFOC_MBEDTLS_POOL_SLOT_SIZE_SMALL * \
      HFOC_MBEDTLS_CONNECTION_MAX_SMALL) + \
     (HFOC_MBEDTLS_POOL_SLOT_SIZE_MEDIUM * \
      HFOC_MBEDTLS_CONNECTION_MAX_MEDIUM) + \
     (HFOC_MBEDTLS_POOL_SLOT_SIZE_LARGE * \
      HFOC_MBEDTLS_CONNECTION_MAX_LARGE))

struct hfoc_mbedtls_alloc_pool_snapshot {
    uint32_t slot_size;
    uint32_t slot_count;
    uint32_t free_count;
    uint32_t used_count;
    uint32_t high_water_count;
};

struct hfoc_mbedtls_connection_snapshot {
    bool active;
    uint32_t id;
    uint32_t allocation_count;
    uint32_t used_bytes;
    uint32_t budget_bytes;
};

struct hfoc_mbedtls_alloc_snapshot {
    uint32_t version;
    uint32_t total_used_bytes;
    uint32_t total_free_bytes;
    uint32_t total_high_water_bytes;
    struct hfoc_mbedtls_alloc_pool_snapshot
        pools[HFOC_MBEDTLS_POOL_COUNT];
    struct hfoc_mbedtls_connection_snapshot
        connections[HFOC_MBEDTLS_CONNECTION_COUNT];
};

bool hfoc_mbedtls_alloc_init(void);
void hfoc_mbedtls_alloc_shutdown(void);
bool hfoc_mbedtls_alloc_ready(void);
void *hfoc_mbedtls_alloc(size_t size);
void *hfoc_mbedtls_calloc(size_t count, size_t size);
bool hfoc_mbedtls_alloc_free(void *ptr);
void hfoc_mbedtls_free(void *ptr);
struct hfoc_mbedtls_alloc_snapshot hfoc_mbedtls_alloc_snapshot(void);
uint32_t hfoc_mbedtls_alloc_free_count(unsigned int pool_class);
uint32_t hfoc_mbedtls_alloc_used_count(unsigned int pool_class);
uint32_t hfoc_mbedtls_alloc_high_water_count(unsigned int pool_class);

/*
 * ITEM-619: the serving listener terminates more than one TLS connection at a
 * time (the sign-in connection stays open while the cooperative credential job
 * runs).  The context API is therefore a bounded stack: `enter()` pushes the
 * connection whose handshake, read or write is executing, `exit()` restores
 * the previous scope, and an allocation is charged to the connection on top.
 * A held-open connection no longer owns the single scope for its lifetime.
 */
bool hfoc_mbedtls_alloc_connection_open(uint32_t *id);
bool hfoc_mbedtls_alloc_connection_close(uint32_t id);
bool hfoc_mbedtls_alloc_connection_enter(uint32_t id);
bool hfoc_mbedtls_alloc_connection_exit(void);

#endif
