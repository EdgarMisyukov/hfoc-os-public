#ifndef HFOC_CONFIG_KV_H
#define HFOC_CONFIG_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_KV_KEY_MAX 16U
#define CONFIG_KV_VALUE_MAX 256U
#define CONFIG_KV_CAPACITY_MAX 32U

#define CONFIG_KV_KIND_CONFIG 0U
#define CONFIG_KV_KIND_AUDIT 1U
#define CONFIG_KV_KIND_COUNT 2U

enum config_kv_status {
    CONFIG_KV_OK = 0,
    CONFIG_KV_NULL = 1,
    CONFIG_KV_BAD_CAPACITY = 2,
    CONFIG_KV_BAD_KIND = 3,
    CONFIG_KV_BAD_KEY = 4,
    CONFIG_KV_BAD_VALUE = 5,
    CONFIG_KV_BAD_STATE = 6,
    CONFIG_KV_NOT_FOUND = 7,
    CONFIG_KV_FULL = 8,
    CONFIG_KV_TRUNCATED = 9,
};

struct config_kv_entry {
    bool occupied;
    uint8_t kind;
    uint8_t key_length;
    unsigned char key[CONFIG_KV_KEY_MAX];
    uint16_t value_length;
    unsigned char value[CONFIG_KV_VALUE_MAX];
    uint64_t revision;
};

struct config_kv {
    struct config_kv_entry *entries;
    size_t capacity;
    size_t count;
    bool initialized;
};

enum config_kv_status config_kv_init(
    struct config_kv *kv,
    struct config_kv_entry *entries,
    size_t capacity);
enum config_kv_status config_kv_put(
    struct config_kv *kv,
    uint8_t kind,
    const void *key,
    size_t key_length,
    const void *value,
    size_t value_length,
    bool *replaced);
enum config_kv_status config_kv_get(
    const struct config_kv *kv,
    uint8_t kind,
    const void *key,
    size_t key_length,
    void *value,
    size_t capacity,
    size_t *value_length,
    uint64_t *revision);
enum config_kv_status config_kv_remove(
    struct config_kv *kv,
    uint8_t kind,
    const void *key,
    size_t key_length,
    bool *removed);
enum config_kv_status config_kv_count(
    const struct config_kv *kv,
    size_t *count);
const char *config_kv_status_string(enum config_kv_status status);

#endif
