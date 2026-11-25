#ifndef HFOC_ENTROPY_SEED_FILL_H
#define HFOC_ENTROPY_SEED_FILL_H

#include <stdbool.h>
#include <stdint.h>

#include "entropy_accumulator.h"

#define ENTROPY_SEED_FILL_VERSION 1U
#define ENTROPY_SEED_FILL_GENERATION_MIN 1U
#define ENTROPY_SEED_FILL_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define ENTROPY_SEED_FILL_SOURCE_ID_MIN 1U
#define ENTROPY_SEED_FILL_SOURCE_ID_MAX UINT32_C(0xFFFFFFFE)
#define ENTROPY_SEED_FILL_REQUIRED_BYTES_MIN 1U
#define ENTROPY_SEED_FILL_REQUIRED_BYTES_MAX ENTROPY_ACCUMULATOR_CAPACITY
#define ENTROPY_SEED_FILL_SEED_LENGTH_MIN 1U
#define ENTROPY_SEED_FILL_SEED_LENGTH_MAX 64U
#define ENTROPY_SEED_FILL_FILL_LENGTH_MAX 64U
#define ENTROPY_SEED_FILL_FILL_COUNT_MAX 64U
#define ENTROPY_SEED_FILL_TOTAL_MATERIAL_MAX ENTROPY_ACCUMULATOR_CAPACITY

enum entropy_seed_fill_state {
    ENTROPY_SEED_FILL_STATE_UNINITIALIZED = 0,
    ENTROPY_SEED_FILL_STATE_UNAVAILABLE = 1,
    ENTROPY_SEED_FILL_STATE_DEGRADED = 2,
    ENTROPY_SEED_FILL_STATE_READY = 3,
    ENTROPY_SEED_FILL_STATE_FAILED = 4,
};

enum entropy_seed_fill_fault {
    ENTROPY_SEED_FILL_FAULT_NONE = 0,
    ENTROPY_SEED_FILL_FAULT_ACCUMULATOR_STATE = 1,
    ENTROPY_SEED_FILL_FAULT_GENERATION_MISMATCH = 2,
    ENTROPY_SEED_FILL_FAULT_SOURCE_MISMATCH = 3,
    ENTROPY_SEED_FILL_FAULT_CREDITED_BYTES = 4,
    ENTROPY_SEED_FILL_FAULT_OUTPUT_OVERLAP = 5,
    ENTROPY_SEED_FILL_FAULT_LENGTH_OVERFLOW = 6,
    ENTROPY_SEED_FILL_FAULT_FILL_EXHAUSTED = 7,
};

struct entropy_seed_fill_config {
    uint32_t version;
    uint32_t generation;
    uint32_t source_id;
    uint32_t required_credited_bytes;
    uint32_t seed_length;
    uint32_t fill_length_max;
    uint32_t fill_count_max;
};

struct entropy_seed_fill {
    bool initialized;
    uint32_t version;
    uint32_t generation;
    uint32_t source_id;
    uint32_t required_credited_bytes;
    uint32_t seed_length;
    uint32_t fill_length_max;
    uint32_t fill_count_max;
    enum entropy_seed_fill_state state;
    enum entropy_seed_fill_fault fault;
    bool seed_published;
    uint32_t seed_material_bytes;
    uint32_t fill_count;
    uint32_t material_offset;
    uint32_t last_fill_length;
};

enum entropy_seed_fill_status {
    ENTROPY_SEED_FILL_OK = 0,
    ENTROPY_SEED_FILL_NULL = 1,
    ENTROPY_SEED_FILL_BAD_STATE = 2,
    ENTROPY_SEED_FILL_ALREADY_PUBLISHED = 3,
    ENTROPY_SEED_FILL_BAD_VERSION = 4,
    ENTROPY_SEED_FILL_BAD_GENERATION = 5,
    ENTROPY_SEED_FILL_BAD_SOURCE_ID = 6,
    ENTROPY_SEED_FILL_BAD_REQUIRED_BYTES = 7,
    ENTROPY_SEED_FILL_BAD_SEED_LENGTH = 8,
    ENTROPY_SEED_FILL_BAD_FILL_LENGTH = 9,
    ENTROPY_SEED_FILL_BAD_FILL_COUNT = 10,
    ENTROPY_SEED_FILL_ACCUMULATOR_NOT_READY = 11,
    ENTROPY_SEED_FILL_GENERATION_MISMATCH = 12,
    ENTROPY_SEED_FILL_SOURCE_MISMATCH = 13,
    ENTROPY_SEED_FILL_INSUFFICIENT_CREDITED_BYTES = 14,
    ENTROPY_SEED_FILL_OUTPUT_OVERLAP = 15,
    ENTROPY_SEED_FILL_OUTPUT_TOO_SMALL = 16,
    ENTROPY_SEED_FILL_SEED_NOT_PUBLISHED = 17,
    ENTROPY_SEED_FILL_SEED_ALREADY_PUBLISHED = 18,
    ENTROPY_SEED_FILL_FILL_EXHAUSTED = 19,
    ENTROPY_SEED_FILL_LENGTH_OVERFLOW = 20,
};

enum entropy_seed_fill_status entropy_seed_fill_init(
    struct entropy_seed_fill *controller,
    const struct entropy_seed_fill_config *config,
    const struct entropy_accumulator *accumulator);
enum entropy_seed_fill_status entropy_seed_fill_seed(
    struct entropy_seed_fill *controller,
    const struct entropy_accumulator *accumulator,
    void *output,
    uint32_t output_capacity,
    uint32_t *bytes_written);
enum entropy_seed_fill_status entropy_seed_fill_fill(
    struct entropy_seed_fill *controller,
    const struct entropy_accumulator *accumulator,
    void *output,
    uint32_t output_capacity,
    uint32_t requested_length,
    uint32_t *bytes_written);
enum entropy_seed_fill_state entropy_seed_fill_state(
    const struct entropy_seed_fill *controller);
enum entropy_seed_fill_fault entropy_seed_fill_fault(
    const struct entropy_seed_fill *controller);
bool entropy_seed_fill_initialized(
    const struct entropy_seed_fill *controller);
bool entropy_seed_fill_ready(
    const struct entropy_seed_fill *controller);
bool entropy_seed_fill_seed_published(
    const struct entropy_seed_fill *controller);
uint32_t entropy_seed_fill_generation(
    const struct entropy_seed_fill *controller);
uint32_t entropy_seed_fill_source_id(
    const struct entropy_seed_fill *controller);
uint32_t entropy_seed_fill_seed_material_bytes(
    const struct entropy_seed_fill *controller);
uint32_t entropy_seed_fill_fill_count(
    const struct entropy_seed_fill *controller);
const char *entropy_seed_fill_status_string(
    enum entropy_seed_fill_status status);
const char *entropy_seed_fill_state_string(
    enum entropy_seed_fill_state state);
const char *entropy_seed_fill_fault_string(
    enum entropy_seed_fill_fault fault);

#endif
