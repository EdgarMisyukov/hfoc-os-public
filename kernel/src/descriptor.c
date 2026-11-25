#include "descriptor.h"

#include <string.h>

static struct descriptor_entry *descriptor_entry(
    struct descriptor_table *table, uint32_t descriptor)
{
    return &table->entries[descriptor];
}

enum descriptor_status descriptor_table_init(struct descriptor_table *table) {
    if (table == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (table->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    memset(table, 0, sizeof(*table));
    table->published = true;
    return DESCRIPTOR_OK;
}

static enum descriptor_status descriptor_table_close_one(
    struct descriptor_table *table, uint32_t descriptor)
{
    struct descriptor_entry *entry;
    struct file_description *description;

    if (descriptor >= PROCESS_DESCRIPTOR_CAPACITY) {
        return DESCRIPTOR_BAD_DESCRIPTOR;
    }
    if ((table->used[descriptor / 64U] & (1ULL << (descriptor % 64U))) == 0U) {
        return DESCRIPTOR_BAD_DESCRIPTOR;
    }
    entry = descriptor_entry(table, descriptor);
    description = entry->description;
    if (description != NULL) {
        if (!description->published || description->refcount == 0U) {
            return DESCRIPTOR_BAD_DESCRIPTION;
        }
        --description->refcount;
        if (description->refcount == 0U) {
            if (description->ops != NULL && description->ops->close != NULL &&
                description->context != NULL) {
                description->ops->close(description);
            }
            description->published = false;
            description->ops = NULL;
            description->context = NULL;
        }
    }
    entry->used = false;
    entry->description = NULL;
    table->used[descriptor / 64U] &= ~(1ULL << (descriptor % 64U));
    if (table->open_count != 0U) {
        --table->open_count;
    }
    return DESCRIPTOR_OK;
}

enum descriptor_status descriptor_table_close_all(
    struct descriptor_table *table)
{
    uint32_t descriptor;

    if (table == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (!table->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    for (descriptor = 0U; descriptor < PROCESS_DESCRIPTOR_CAPACITY;
         ++descriptor) {
        if ((table->used[descriptor / 64U] &
             (1ULL << (descriptor % 64U))) != 0U) {
            const enum descriptor_status status =
                descriptor_table_close_one(table, descriptor);

            if (status != DESCRIPTOR_OK) {
                return status;
            }
        }
    }
    return DESCRIPTOR_OK;
}

enum descriptor_status descriptor_table_destroy(struct descriptor_table *table) {
    enum descriptor_status status;

    if (table == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (!table->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    status = descriptor_table_close_all(table);
    if (status != DESCRIPTOR_OK) {
        return status;
    }
    memset(table, 0, sizeof(*table));
    return DESCRIPTOR_OK;
}

enum descriptor_status descriptor_table_reserve(
    struct descriptor_table *table,
    uint32_t *descriptor_out)
{
    uint32_t descriptor;

    if (table == NULL || descriptor_out == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (!table->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    for (descriptor = 0U; descriptor < PROCESS_DESCRIPTOR_CAPACITY;
         ++descriptor) {
        const uint32_t word = descriptor / 64U;
        const uint64_t bit = 1ULL << (descriptor % 64U);
        if ((table->used[word] & bit) == 0U) {
            table->used[word] |= bit;
            table->entries[descriptor].used = true;
            table->entries[descriptor].description = NULL;
            ++table->open_count;
            *descriptor_out = descriptor;
            return DESCRIPTOR_OK;
        }
    }
    return DESCRIPTOR_FULL;
}

enum descriptor_status descriptor_table_release(
    struct descriptor_table *table,
    uint32_t descriptor)
{
    if (table == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (!table->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    return descriptor_table_close_one(table, descriptor);
}

uint32_t descriptor_table_open_count(const struct descriptor_table *table) {
    return table != NULL ? table->open_count : 0U;
}

bool descriptor_table_is_published(const struct descriptor_table *table) {
    return table != NULL && table->published;
}

enum descriptor_status descriptor_description_init(
    struct file_description *description,
    const struct descriptor_ops *ops, void *context, bool readable,
    bool writable)
{
    if (description == NULL || ops == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (description->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    memset(description, 0, sizeof(*description));
    description->published = true;
    description->readable = readable;
    description->writable = writable;
    description->refcount = 1U;
    description->ops = ops;
    description->context = context;
    return DESCRIPTOR_OK;
}

void descriptor_description_release(struct file_description *description)
{
    if (description == NULL || !description->published ||
        description->refcount == 0U) {
        return;
    }
    --description->refcount;
    if (description->refcount == 0U) {
        if (description->ops != NULL && description->ops->close != NULL &&
            description->context != NULL) {
            description->ops->close(description);
        }
        description->published = false;
        description->ops = NULL;
        description->context = NULL;
    }
}

struct file_description *descriptor_description_retain(
    struct file_description *description)
{
    if (description == NULL || !description->published ||
        description->refcount == UINT32_MAX) {
        return NULL;
    }
    ++description->refcount;
    return description;
}

enum descriptor_status descriptor_table_bind(
    struct descriptor_table *table, struct file_description *description,
    uint32_t *descriptor_out)
{
    uint32_t descriptor;
    enum descriptor_status status;

    if (table == NULL || description == NULL || descriptor_out == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (!table->published || !description->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    status = descriptor_table_reserve(table, &descriptor);
    if (status != DESCRIPTOR_OK) {
        return status;
    }
    table->entries[descriptor].description = description;
    *descriptor_out = descriptor;
    return DESCRIPTOR_OK;
}

enum descriptor_status descriptor_table_bind_at(
    struct descriptor_table *table, struct file_description *description,
    uint32_t descriptor)
{
    const uint64_t bit = 1ULL << (descriptor % 64U);

    if (table == NULL || description == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (!table->published || !description->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    if (descriptor >= PROCESS_DESCRIPTOR_CAPACITY) {
        return DESCRIPTOR_BAD_DESCRIPTOR;
    }
    if ((table->used[descriptor / 64U] & bit) != 0U) {
        return DESCRIPTOR_BAD_STATE;
    }
    table->used[descriptor / 64U] |= bit;
    table->entries[descriptor].used = true;
    table->entries[descriptor].description = description;
    ++table->open_count;
    return DESCRIPTOR_OK;
}

enum descriptor_status descriptor_table_adopt_at(
    struct descriptor_table *table, struct file_description *description,
    uint32_t descriptor)
{
    struct file_description *existing = NULL;
    enum descriptor_status status;

    if (table == NULL || description == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (descriptor >= 3U) {
        return DESCRIPTOR_BAD_DESCRIPTOR;
    }
    if (descriptor_table_lookup(table, descriptor, &existing) ==
        DESCRIPTOR_OK) {
        status = descriptor_table_close(table, descriptor);
        if (status != DESCRIPTOR_OK) {
            return status;
        }
    }
    if (descriptor_description_retain(description) == NULL) {
        return DESCRIPTOR_BAD_DESCRIPTION;
    }
    status = descriptor_table_bind_at(table, description, descriptor);
    if (status != DESCRIPTOR_OK) {
        descriptor_description_release(description);
    }
    return status;
}

enum descriptor_status descriptor_table_lookup(
    struct descriptor_table *table, uint32_t descriptor,
    struct file_description **description_out)
{
    if (table == NULL || description_out == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (!table->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    if (descriptor >= PROCESS_DESCRIPTOR_CAPACITY ||
        (table->used[descriptor / 64U] &
         (1ULL << (descriptor % 64U))) == 0U) {
        return DESCRIPTOR_BAD_DESCRIPTOR;
    }
    *description_out = table->entries[descriptor].description;
    return *description_out != NULL ? DESCRIPTOR_OK
                                    : DESCRIPTOR_BAD_DESCRIPTION;
}

enum descriptor_status descriptor_table_close(struct descriptor_table *table,
                                              uint32_t descriptor)
{
    return descriptor_table_release(table, descriptor);
}

enum descriptor_status descriptor_table_dup2(struct descriptor_table *table,
                                             uint32_t old_descriptor,
                                             uint32_t new_descriptor)
{
    struct file_description *description;
    enum descriptor_status status;

    if (table == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (!table->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    status = descriptor_table_lookup(table, old_descriptor, &description);
    if (status != DESCRIPTOR_OK) {
        return status;
    }
    if (new_descriptor >= PROCESS_DESCRIPTOR_CAPACITY) {
        return DESCRIPTOR_BAD_DESCRIPTOR;
    }
    if (old_descriptor == new_descriptor) {
        return DESCRIPTOR_OK;
    }
    if ((table->used[new_descriptor / 64U] &
         (1ULL << (new_descriptor % 64U))) != 0U) {
        status = descriptor_table_close_one(table, new_descriptor);
        if (status != DESCRIPTOR_OK) {
            return status;
        }
    }
    if (descriptor_description_retain(description) == NULL) {
        return DESCRIPTOR_BAD_DESCRIPTION;
    }
    table->used[new_descriptor / 64U] |= 1ULL << (new_descriptor % 64U);
    table->entries[new_descriptor].used = true;
    table->entries[new_descriptor].description = description;
    ++table->open_count;
    return DESCRIPTOR_OK;
}

enum descriptor_status descriptor_table_clone(
    struct descriptor_table *source, struct descriptor_table *destination)
{
    uint32_t descriptor;

    if (source == NULL || destination == NULL) {
        return DESCRIPTOR_NULL;
    }
    if (!source->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    if (destination->published) {
        return DESCRIPTOR_BAD_STATE;
    }
    memset(destination, 0, sizeof(*destination));
    destination->published = true;
    for (descriptor = 0U; descriptor < PROCESS_DESCRIPTOR_CAPACITY;
         ++descriptor) {
        const uint64_t bit = 1ULL << (descriptor % 64U);

        if ((source->used[descriptor / 64U] & bit) == 0U) {
            continue;
        }
        if (source->entries[descriptor].description != NULL) {
            if (descriptor_description_retain(
                    source->entries[descriptor].description) == NULL) {
                (void)descriptor_table_destroy(destination);
                return DESCRIPTOR_BAD_DESCRIPTION;
            }
            destination->entries[descriptor].description =
                source->entries[descriptor].description;
        }
        destination->entries[descriptor].used = true;
        destination->used[descriptor / 64U] |= bit;
        ++destination->open_count;
    }
    return DESCRIPTOR_OK;
}

const char *descriptor_status_string(enum descriptor_status status) {
    switch (status) {
    case DESCRIPTOR_OK:
        return "DESCRIPTOR_OK";
    case DESCRIPTOR_NULL:
        return "DESCRIPTOR_NULL";
    case DESCRIPTOR_BAD_STATE:
        return "DESCRIPTOR_BAD_STATE";
    case DESCRIPTOR_FULL:
        return "DESCRIPTOR_FULL";
    case DESCRIPTOR_BAD_DESCRIPTOR:
        return "DESCRIPTOR_BAD_DESCRIPTOR";
    case DESCRIPTOR_BAD_DESCRIPTION:
        return "DESCRIPTOR_BAD_DESCRIPTION";
    case DESCRIPTOR_CLOSE_FAILED:
        return "DESCRIPTOR_CLOSE_FAILED";
    default:
        return "DESCRIPTOR_UNKNOWN";
    }
}
