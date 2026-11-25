#ifndef HFOC_ENTROPY_DIAGNOSTICS_H
#define HFOC_ENTROPY_DIAGNOSTICS_H

#include "entropy_accumulator.h"
#include "entropy_health.h"
#include "entropy_seed_fill.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ENTROPY_DIAGNOSTICS_VERSION 1U
#define ENTROPY_DIAGNOSTICS_POLICY_ID "HFOC-ENTROPY-DIAG-1"
#define ENTROPY_DIAGNOSTICS_POLICY_ID_LENGTH 19U
#define ENTROPY_DIAGNOSTICS_SERIAL_MAX 384U
#define ENTROPY_DIAGNOSTICS_SUPPORT_SIZE 64U

enum entropy_diagnostics_state {
    ENTROPY_DIAGNOSTICS_STATE_UNINITIALIZED = 0,
    ENTROPY_DIAGNOSTICS_STATE_UNAVAILABLE = 1,
    ENTROPY_DIAGNOSTICS_STATE_DEGRADED = 2,
    ENTROPY_DIAGNOSTICS_STATE_READY = 3,
    ENTROPY_DIAGNOSTICS_STATE_FAILED = 4,
};

enum entropy_diagnostics_fault {
    ENTROPY_DIAGNOSTICS_FAULT_NONE = 0,
    ENTROPY_DIAGNOSTICS_FAULT_HEALTH_REPEATED_BLOCK = 1,
    ENTROPY_DIAGNOSTICS_FAULT_HEALTH_IMPOSSIBLE_LENGTH = 2,
    ENTROPY_DIAGNOSTICS_FAULT_HEALTH_STALLED = 3,
    ENTROPY_DIAGNOSTICS_FAULT_HEALTH_DEVICE_RESET = 4,
    ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE = 5,
    ENTROPY_DIAGNOSTICS_FAULT_GENERATION_MISMATCH = 6,
    ENTROPY_DIAGNOSTICS_FAULT_SOURCE_MISMATCH = 7,
    ENTROPY_DIAGNOSTICS_FAULT_CREDITED_BYTES = 8,
    ENTROPY_DIAGNOSTICS_FAULT_OUTPUT_OVERLAP = 9,
    ENTROPY_DIAGNOSTICS_FAULT_LENGTH_OVERFLOW = 10,
    ENTROPY_DIAGNOSTICS_FAULT_FILL_EXHAUSTED = 11,
};

struct entropy_diagnostics_snapshot {
    uint32_t version;
    uint32_t generation;
    uint32_t source_id;
    enum entropy_diagnostics_state state;
    enum entropy_diagnostics_fault fault;
    uint64_t credited_bytes;
    uint32_t credited_blocks;
    uint32_t last_accept_tick;
    bool seed_published;
    uint32_t seed_material_bytes;
    uint32_t fill_count;
};

struct entropy_diagnostics_audit {
    uint32_t version;
    uint32_t state;
    uint32_t fault;
    uint32_t generation;
    uint32_t source_id;
    uint64_t credited_bytes;
    uint32_t credited_blocks;
    uint32_t seed_material_bytes;
    uint32_t fill_count;
    uint32_t last_accept_tick;
};

enum entropy_diagnostics_status {
    ENTROPY_DIAGNOSTICS_OK = 0,
    ENTROPY_DIAGNOSTICS_NULL = 1,
    ENTROPY_DIAGNOSTICS_BAD_SNAPSHOT = 2,
    ENTROPY_DIAGNOSTICS_BAD_STATE = 3,
    ENTROPY_DIAGNOSTICS_BAD_VERSION = 4,
    ENTROPY_DIAGNOSTICS_BAD_GENERATION = 5,
    ENTROPY_DIAGNOSTICS_BAD_SOURCE_ID = 6,
    ENTROPY_DIAGNOSTICS_TOO_SMALL = 7,
};

enum entropy_diagnostics_status entropy_diagnostics_snapshot(
    const struct entropy_health *health,
    const struct entropy_accumulator *accumulator,
    const struct entropy_seed_fill *controller,
    struct entropy_diagnostics_snapshot *snapshot_out);
enum entropy_diagnostics_status entropy_diagnostics_serial(
    const struct entropy_diagnostics_snapshot *snapshot,
    void *buffer,
    size_t capacity,
    size_t *length_out);
enum entropy_diagnostics_status entropy_diagnostics_audit(
    const struct entropy_diagnostics_snapshot *snapshot,
    struct entropy_diagnostics_audit *audit_out);
enum entropy_diagnostics_status entropy_diagnostics_support(
    const struct entropy_diagnostics_snapshot *snapshot,
    void *buffer,
    size_t capacity,
    size_t *length_out);
const char *entropy_diagnostics_state_string(
    enum entropy_diagnostics_state state);
const char *entropy_diagnostics_fault_string(
    enum entropy_diagnostics_fault fault);
const char *entropy_diagnostics_status_string(
    enum entropy_diagnostics_status status);
uint32_t entropy_diagnostics_version(void);
const char *entropy_diagnostics_policy_id(void);

#endif
