#ifndef HFOC_CAPABILITY_H
#define HFOC_CAPABILITY_H

#include <stdbool.h>
#include <stdint.h>

#define PROCESS_CAPABILITY_CAPACITY 32U
#define PROCESS_CAPABILITY_HANDLE_INVALID 0U

#define CAPABILITY_TYPE_SYSCALL_WRITE 1U
#define CAPABILITY_TYPE_SYSCALL_YIELD 2U
#define CAPABILITY_TYPE_SYSCALL_EXIT  3U
/* ITEM-624: one capability gates the native file-call surface; the fd table
   and the owner process remain the per-operation checks. */
#define CAPABILITY_TYPE_SYSCALL_FILE  4U

enum capability_status {
    CAPABILITY_OK = 0,
    CAPABILITY_NULL = 1,
    CAPABILITY_BAD_STATE = 2,
    CAPABILITY_DUPLICATE = 3,
    CAPABILITY_FULL = 4,
    CAPABILITY_BAD_HANDLE = 5,
    CAPABILITY_NOT_PUBLISHED = 6,
};

struct capability {
    uint32_t type;
    uint32_t flags;
    uint64_t object;
};

struct capability_entry {
    bool pending;
    bool published;
    uint64_t handle;
    struct capability capability;
};

struct capability_table {
    bool published;
    uint32_t open_count;
    uint64_t next_handle;
    struct capability_entry entries[PROCESS_CAPABILITY_CAPACITY];
};

enum capability_status capability_table_init(struct capability_table *table);
enum capability_status capability_table_destroy(struct capability_table *table);
enum capability_status capability_table_reserve(
    struct capability_table *table,
    uint64_t *handle_out);
enum capability_status capability_table_commit(
    struct capability_table *table,
    uint64_t handle,
    const struct capability *capability);
enum capability_status capability_table_rollback(
    struct capability_table *table,
    uint64_t handle);
enum capability_status capability_table_add(
    struct capability_table *table,
    const struct capability *capability,
    uint64_t *handle_out);
enum capability_status capability_table_close(
    struct capability_table *table,
    uint64_t handle);
enum capability_status capability_table_revoke(
    struct capability_table *table,
    uint64_t handle);
enum capability_status capability_table_get(
    const struct capability_table *table,
    uint64_t handle,
    struct capability *capability_out);
bool capability_table_has(const struct capability_table *table,
                          uint32_t type);
uint32_t capability_table_open_count(const struct capability_table *table);
bool capability_table_is_published(const struct capability_table *table);
const char *capability_status_string(enum capability_status status);

#endif
