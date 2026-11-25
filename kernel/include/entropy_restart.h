#ifndef HFOC_ENTROPY_RESTART_H
#define HFOC_ENTROPY_RESTART_H

#include "entropy_accumulator.h"
#include "entropy_health.h"
#include "entropy_seed_fill.h"

#include <stdbool.h>
#include <stdint.h>

#define ENTROPY_RESTART_VERSION 1U
#define ENTROPY_RESTART_GENERATION_MIN 1U
#define ENTROPY_RESTART_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define ENTROPY_RESTART_SOURCE_ID_MIN 1U
#define ENTROPY_RESTART_SOURCE_ID_MAX UINT32_C(0xFFFFFFFE)
#define ENTROPY_RESTART_RESTART_COUNT_MAX 255U
#define ENTROPY_RESTART_INTERVAL_MIN_TICKS 1U
#define ENTROPY_RESTART_INTERVAL_MAX_TICKS UINT32_C(0x7FFFFFFF)

enum entropy_restart_state {
    ENTROPY_RESTART_STATE_UNINITIALIZED = 0,
    ENTROPY_RESTART_STATE_UNAVAILABLE = 1,
    ENTROPY_RESTART_STATE_READY = 2,
    ENTROPY_RESTART_STATE_FAILED = 3,
    ENTROPY_RESTART_STATE_STOPPED = 4,
};

enum entropy_restart_fault {
    ENTROPY_RESTART_FAULT_NONE = 0,
    ENTROPY_RESTART_FAULT_MISSING_DEVICE = 1,
    ENTROPY_RESTART_FAULT_STALLED = 2,
    ENTROPY_RESTART_FAULT_REPEATED_OUTPUT = 3,
    ENTROPY_RESTART_FAULT_DEVICE_RESET = 4,
    ENTROPY_RESTART_FAULT_UNHEALTHY = 5,
    ENTROPY_RESTART_FAULT_SHUTDOWN = 6,
};

struct entropy_restart_config {
    uint32_t version;
    uint32_t restart_count_max;
    uint32_t min_restart_interval_ticks;
};

struct entropy_restart {
    bool initialized;
    uint32_t version;
    uint32_t generation;
    uint32_t source_id;
    uint32_t last_generation;
    uint32_t restart_count;
    uint32_t restart_count_max;
    uint32_t min_restart_interval_ticks;
    uint32_t last_restart_tick;
    enum entropy_restart_state state;
    enum entropy_restart_fault fault;
    struct entropy_health *health;
    struct entropy_accumulator *accumulator;
    struct entropy_seed_fill *seed_fill;
};

enum entropy_restart_status {
    ENTROPY_RESTART_OK = 0,
    ENTROPY_RESTART_NULL = 1,
    ENTROPY_RESTART_BAD_STATE = 2,
    ENTROPY_RESTART_ALREADY_PUBLISHED = 3,
    ENTROPY_RESTART_BAD_VERSION = 4,
    ENTROPY_RESTART_BAD_RESTART_COUNT = 5,
    ENTROPY_RESTART_BAD_INTERVAL = 6,
    ENTROPY_RESTART_BAD_GENERATION = 7,
    ENTROPY_RESTART_BAD_SOURCE_ID = 8,
    ENTROPY_RESTART_MISSING_DEVICE = 9,
    ENTROPY_RESTART_STALLED = 10,
    ENTROPY_RESTART_REPEATED_OUTPUT = 11,
    ENTROPY_RESTART_DEVICE_RESET = 12,
    ENTROPY_RESTART_UNHEALTHY = 13,
    ENTROPY_RESTART_NOT_READY = 14,
    ENTROPY_RESTART_STALE_GENERATION = 15,
    ENTROPY_RESTART_INTERVAL = 16,
    ENTROPY_RESTART_EXHAUSTED = 17,
};

enum entropy_restart_status entropy_restart_init(
    struct entropy_restart *restart,
    const struct entropy_restart_config *config);
enum entropy_restart_status entropy_restart_activate(
    struct entropy_restart *restart,
    struct entropy_health *health,
    struct entropy_accumulator *accumulator,
    struct entropy_seed_fill *seed_fill,
    uint32_t generation,
    uint32_t source_id,
    uint32_t now_tick);
enum entropy_restart_status entropy_restart_check(
    struct entropy_restart *restart,
    uint32_t now_tick);
enum entropy_restart_status entropy_restart_stop(
    struct entropy_restart *restart,
    uint32_t now_tick);
enum entropy_restart_status entropy_restart_restart(
    struct entropy_restart *restart,
    uint32_t now_tick);
bool entropy_restart_initialized(
    const struct entropy_restart *restart);
bool entropy_restart_ready(
    const struct entropy_restart *restart);
bool entropy_restart_stopped(
    const struct entropy_restart *restart);
enum entropy_restart_state entropy_restart_state(
    const struct entropy_restart *restart);
enum entropy_restart_fault entropy_restart_fault(
    const struct entropy_restart *restart);
uint32_t entropy_restart_generation(
    const struct entropy_restart *restart);
uint32_t entropy_restart_last_generation(
    const struct entropy_restart *restart);
uint32_t entropy_restart_restart_count(
    const struct entropy_restart *restart);
uint32_t entropy_restart_last_restart_tick(
    const struct entropy_restart *restart);
const char *entropy_restart_status_string(
    enum entropy_restart_status status);
const char *entropy_restart_state_string(
    enum entropy_restart_state state);
const char *entropy_restart_fault_string(
    enum entropy_restart_fault fault);

#endif
