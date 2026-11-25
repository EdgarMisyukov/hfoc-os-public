#ifndef HFOC_ENTROPY_ACCUMULATOR_H
#define HFOC_ENTROPY_ACCUMULATOR_H

#include <stdbool.h>
#include <stdint.h>

#define ENTROPY_ACCUMULATOR_VERSION 1U
#define ENTROPY_ACCUMULATOR_CAPACITY 4096U
#define ENTROPY_ACCUMULATOR_SCRATCH_SIZE 64U
#define ENTROPY_ACCUMULATOR_GENERATION_MIN 1U
#define ENTROPY_ACCUMULATOR_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define ENTROPY_ACCUMULATOR_SOURCE_ID_MIN 1U
#define ENTROPY_ACCUMULATOR_SOURCE_ID_MAX UINT32_C(0xFFFFFFFE)
#define ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MIN 1U
#define ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX 4096U
#define ENTROPY_ACCUMULATOR_STALL_LIMIT_MIN 1U
#define ENTROPY_ACCUMULATOR_STALL_LIMIT_MAX UINT32_C(0x7FFFFFFF)

enum entropy_accumulator_state {
    ENTROPY_ACCUMULATOR_STATE_UNINITIALIZED = 0,
    ENTROPY_ACCUMULATOR_STATE_INITIALIZING = 1,
    ENTROPY_ACCUMULATOR_STATE_ACCUMULATING = 2,
    ENTROPY_ACCUMULATOR_STATE_FAILED = 3,
};

enum entropy_accumulator_fault {
    ENTROPY_ACCUMULATOR_FAULT_NONE = 0,
    ENTROPY_ACCUMULATOR_FAULT_GENERATION_MISMATCH = 1,
    ENTROPY_ACCUMULATOR_FAULT_SOURCE_MISMATCH = 2,
    ENTROPY_ACCUMULATOR_FAULT_IMPOSSIBLE_LENGTH = 3,
    ENTROPY_ACCUMULATOR_FAULT_OVERLAPPING_BLOCK = 4,
    ENTROPY_ACCUMULATOR_FAULT_CREDIT_OVERFLOW = 5,
    ENTROPY_ACCUMULATOR_FAULT_STALLED = 6,
};

struct entropy_accumulator_config {
    uint32_t version;
    uint32_t generation;
    uint32_t source_id;
    uint32_t block_length_min;
    uint32_t block_length_max;
    uint32_t max_stall_ticks;
};

struct entropy_accumulator_source_state {
    bool active;
    uint32_t generation;
    uint32_t source_id;
    uint64_t credited_bytes;
    uint32_t credited_blocks;
    uint32_t last_accept_tick;
};

struct entropy_accumulator {
    bool initialized;
    uint32_t version;
    uint32_t generation;
    uint32_t source_id;
    uint32_t block_length_min;
    uint32_t block_length_max;
    uint32_t max_stall_ticks;
    enum entropy_accumulator_state state;
    enum entropy_accumulator_fault fault;
    struct entropy_accumulator_source_state source;
    uint8_t bytes[ENTROPY_ACCUMULATOR_CAPACITY];
};

enum entropy_accumulator_status {
    ENTROPY_ACCUMULATOR_OK = 0,
    ENTROPY_ACCUMULATOR_NULL = 1,
    ENTROPY_ACCUMULATOR_BAD_STATE = 2,
    ENTROPY_ACCUMULATOR_ALREADY_PUBLISHED = 3,
    ENTROPY_ACCUMULATOR_BAD_VERSION = 4,
    ENTROPY_ACCUMULATOR_BAD_GENERATION = 5,
    ENTROPY_ACCUMULATOR_BAD_SOURCE_ID = 6,
    ENTROPY_ACCUMULATOR_BAD_LENGTH_BOUNDS = 7,
    ENTROPY_ACCUMULATOR_BAD_STALL_LIMIT = 8,
    ENTROPY_ACCUMULATOR_GENERATION_MISMATCH = 9,
    ENTROPY_ACCUMULATOR_SOURCE_MISMATCH = 10,
    ENTROPY_ACCUMULATOR_IMPOSSIBLE_LENGTH = 11,
    ENTROPY_ACCUMULATOR_OVERLAPPING_BLOCK = 12,
    ENTROPY_ACCUMULATOR_CREDIT_OVERFLOW = 13,
    ENTROPY_ACCUMULATOR_STALLED = 14,
};

enum entropy_accumulator_status entropy_accumulator_init(
    struct entropy_accumulator *accumulator,
    const struct entropy_accumulator_config *config,
    uint32_t initial_tick);
enum entropy_accumulator_status entropy_accumulator_mix(
    struct entropy_accumulator *accumulator,
    uint32_t generation,
    uint32_t source_id,
    const void *block,
    uint32_t block_length,
    uint32_t now_tick);
enum entropy_accumulator_status entropy_accumulator_progress(
    struct entropy_accumulator *accumulator,
    uint32_t now_tick);
enum entropy_accumulator_state entropy_accumulator_state(
    const struct entropy_accumulator *accumulator);
enum entropy_accumulator_fault entropy_accumulator_fault(
    const struct entropy_accumulator *accumulator);
bool entropy_accumulator_initialized(
    const struct entropy_accumulator *accumulator);
bool entropy_accumulator_accumulating(
    const struct entropy_accumulator *accumulator);
uint32_t entropy_accumulator_generation(
    const struct entropy_accumulator *accumulator);
uint32_t entropy_accumulator_source_id(
    const struct entropy_accumulator *accumulator);
uint64_t entropy_accumulator_credited_bytes(
    const struct entropy_accumulator *accumulator);
uint32_t entropy_accumulator_credited_blocks(
    const struct entropy_accumulator *accumulator);
uint32_t entropy_accumulator_last_accept_tick(
    const struct entropy_accumulator *accumulator);
const char *entropy_accumulator_status_string(
    enum entropy_accumulator_status status);
const char *entropy_accumulator_state_string(
    enum entropy_accumulator_state state);
const char *entropy_accumulator_fault_string(
    enum entropy_accumulator_fault fault);

#endif
