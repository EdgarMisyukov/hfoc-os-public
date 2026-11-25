#ifndef HFOC_ENTROPY_HEALTH_H
#define HFOC_ENTROPY_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#define ENTROPY_HEALTH_VERSION 1U
#define ENTROPY_HEALTH_GENERATION_MIN 1U
#define ENTROPY_HEALTH_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define ENTROPY_HEALTH_BLOCK_LENGTH_MIN 1U
#define ENTROPY_HEALTH_BLOCK_LENGTH_MAX 4096U
#define ENTROPY_HEALTH_STALL_LIMIT_MIN 1U
#define ENTROPY_HEALTH_STALL_LIMIT_MAX UINT32_C(0x7FFFFFFF)

enum entropy_health_state {
    ENTROPY_HEALTH_STATE_UNINITIALIZED = 0,
    ENTROPY_HEALTH_STATE_INITIALIZING = 1,
    ENTROPY_HEALTH_STATE_READY = 2,
    ENTROPY_HEALTH_STATE_FAILED = 3,
};

enum entropy_health_fault {
    ENTROPY_HEALTH_FAULT_NONE = 0,
    ENTROPY_HEALTH_FAULT_REPEATED_BLOCK = 1,
    ENTROPY_HEALTH_FAULT_IMPOSSIBLE_LENGTH = 2,
    ENTROPY_HEALTH_FAULT_STALLED = 3,
    ENTROPY_HEALTH_FAULT_DEVICE_RESET = 4,
};

struct entropy_health_config {
    uint32_t version;
    uint32_t block_length_min;
    uint32_t block_length_max;
    uint32_t max_stall_ticks;
};

struct entropy_health {
    bool initialized;
    uint32_t version;
    uint32_t generation;
    uint32_t block_length_min;
    uint32_t block_length_max;
    uint32_t max_stall_ticks;
    enum entropy_health_state state;
    enum entropy_health_fault fault;
    uint32_t last_accept_tick;
    bool previous_block_valid;
    uint32_t previous_block_length;
    uint8_t previous_block[ENTROPY_HEALTH_BLOCK_LENGTH_MAX];
};

enum entropy_health_status {
    ENTROPY_HEALTH_OK = 0,
    ENTROPY_HEALTH_NULL = 1,
    ENTROPY_HEALTH_BAD_STATE = 2,
    ENTROPY_HEALTH_ALREADY_PUBLISHED = 3,
    ENTROPY_HEALTH_BAD_VERSION = 4,
    ENTROPY_HEALTH_BAD_GENERATION = 5,
    ENTROPY_HEALTH_BAD_LENGTH_BOUNDS = 6,
    ENTROPY_HEALTH_BAD_STALL_LIMIT = 7,
    ENTROPY_HEALTH_REPEATED_BLOCK = 8,
    ENTROPY_HEALTH_IMPOSSIBLE_LENGTH = 9,
    ENTROPY_HEALTH_STALLED = 10,
    ENTROPY_HEALTH_DEVICE_RESET = 11,
};

enum entropy_health_status entropy_health_init(
    struct entropy_health *health,
    const struct entropy_health_config *config,
    uint32_t generation,
    uint32_t initial_tick);
enum entropy_health_status entropy_health_observe(
    struct entropy_health *health,
    uint32_t generation,
    const void *block,
    uint32_t block_length,
    uint32_t now_tick);
enum entropy_health_status entropy_health_progress(
    struct entropy_health *health,
    uint32_t now_tick);
enum entropy_health_state entropy_health_state(
    const struct entropy_health *health);
enum entropy_health_fault entropy_health_fault(
    const struct entropy_health *health);
uint32_t entropy_health_generation(
    const struct entropy_health *health);
bool entropy_health_ready(
    const struct entropy_health *health);
const char *entropy_health_status_string(
    enum entropy_health_status status);
const char *entropy_health_state_string(
    enum entropy_health_state state);
const char *entropy_health_fault_string(
    enum entropy_health_fault fault);

#endif
