#ifndef HFOC_DESCRIPTOR_H
#define HFOC_DESCRIPTOR_H

#include "vfs.h"

#include <stdbool.h>
#include <stdint.h>

#define PROCESS_DESCRIPTOR_CAPACITY 64U
#define PROCESS_DESCRIPTOR_WORDS \
    ((PROCESS_DESCRIPTOR_CAPACITY + 63U) / 64U)

enum descriptor_status {
    DESCRIPTOR_OK = 0,
    DESCRIPTOR_NULL = 1,
    DESCRIPTOR_BAD_STATE = 2,
    DESCRIPTOR_FULL = 3,
    DESCRIPTOR_BAD_DESCRIPTOR = 4,
    DESCRIPTOR_BAD_DESCRIPTION = 5,
    DESCRIPTOR_CLOSE_FAILED = 6,
};

struct file_description;

/*
 * ITEM-623: a descriptor no longer is just a number.  It names a shared
 * open-file description: operations, context, offset and reference count.
 * dup2 and inheritance copy the pointer, so both descriptors observe one
 * offset and one lifetime; two independent opens have separate descriptions.
 * The description storage is caller-owned (bounded, no heap here) and is
 * released exactly once, when the last descriptor drops it.
 */
struct descriptor_ops {
    enum vfs_status (*read)(struct file_description *description,
                            void *buffer, size_t size, size_t *done_out);
    enum vfs_status (*write)(struct file_description *description,
                             const void *buffer, size_t size,
                             size_t *done_out);
    void (*close)(struct file_description *description);
};

struct file_description {
    bool published;
    bool readable;
    bool writable;
    uint32_t refcount;
    const struct descriptor_ops *ops;
    void *context;
};

struct descriptor_entry {
    bool used;
    struct file_description *description;
};

struct descriptor_table {
    bool published;
    uint32_t open_count;
    uint64_t used[PROCESS_DESCRIPTOR_WORDS];
    struct descriptor_entry entries[PROCESS_DESCRIPTOR_CAPACITY];
};

enum descriptor_status descriptor_table_init(struct descriptor_table *table);
enum descriptor_status descriptor_table_destroy(struct descriptor_table *table);
enum descriptor_status descriptor_table_reserve(
    struct descriptor_table *table,
    uint32_t *descriptor_out);
enum descriptor_status descriptor_table_release(
    struct descriptor_table *table,
    uint32_t descriptor);
uint32_t descriptor_table_open_count(const struct descriptor_table *table);
bool descriptor_table_is_published(const struct descriptor_table *table);
const char *descriptor_status_string(enum descriptor_status status);

enum descriptor_status descriptor_description_init(
    struct file_description *description,
    const struct descriptor_ops *ops, void *context, bool readable,
    bool writable);
void descriptor_description_release(struct file_description *description);
struct file_description *descriptor_description_retain(
    struct file_description *description);
enum descriptor_status descriptor_table_bind(
    struct descriptor_table *table, struct file_description *description,
    uint32_t *descriptor_out);
/* ITEM-628: bind at an exact descriptor, for spawn redirection actions. */
enum descriptor_status descriptor_table_bind_at(
    struct descriptor_table *table, struct file_description *description,
    uint32_t descriptor);
/* ITEM-628: another table adopts one already-open description at an exact fd. */
enum descriptor_status descriptor_table_adopt_at(
    struct descriptor_table *table, struct file_description *description,
    uint32_t descriptor);
enum descriptor_status descriptor_table_lookup(
    struct descriptor_table *table, uint32_t descriptor,
    struct file_description **description_out);
enum descriptor_status descriptor_table_close(struct descriptor_table *table,
                                              uint32_t descriptor);
enum descriptor_status descriptor_table_dup2(struct descriptor_table *table,
                                             uint32_t old_descriptor,
                                             uint32_t new_descriptor);
enum descriptor_status descriptor_table_clone(
    struct descriptor_table *source, struct descriptor_table *destination);
enum descriptor_status descriptor_table_close_all(
    struct descriptor_table *table);

#endif
