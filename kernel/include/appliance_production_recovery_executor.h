#ifndef HFOC_APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_H
#define HFOC_APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_H

#include "appliance_config_integrity.h"
#include "appliance_factory_reset.h"
#include "appliance_persistent_storage_namespace.h"
#include "appliance_production_auth.h"
#include "appliance_production_management_plane.h"
#include "appliance_recovery_mode.h"
#include "appliance_state_layout.h"
#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-368 production recovery and factory-reset executor.
 *
 * This caller-owned, allocation-free composition binds the ITEM-365
 * production request gate (`appliance_production_auth`), the ITEM-366
 * read-only management plane (`appliance_production_management_plane`), the
 * existing recovery decision gate (`appliance_recovery_mode`), the existing
 * factory-reset plan gate (`appliance_factory_reset`) and the ITEM-360
 * production state partition (`block_device` + `appliance_state_layout`) so an
 * authorized administrator can plan and execute a bounded recovery or
 * factory-reset storage transaction without introducing a second device
 * registry, route table, state store, or trust store.
 *
 * Every operation is two-phase and bounded:
 *   1. an authorized request publishes a bounded plan and, when it is not a
 *      dry run, stages exactly that plan and issues a single-use confirmation
 *      token with a bounded lifetime;
 *   2. only a second authorized request carrying that exact token executes the
 *      plan, while an explicit cancel or an expired window discards it and
 *      leaves the persistent state untouched.
 *
 * The storage transaction never invents success: the whole plan is validated
 * against the published state layout and the configured block bound before the
 * first write, every written range is read back and verified, and a failure
 * reports APPLY_FAILED (or STORAGE_ERROR) with the number of blocks already
 * written instead of a fabricated APPLIED. `RECOVERY_METADATA` is preserved
 * by the factory-reset plan and is never written by this executor.
 *
 * Readiness is honest: READY only when the request gate and the management
 * plane are available and the boot/storage/installer evidence is published;
 * DEGRADED when only dry runs are possible; UNAVAILABLE otherwise. Physical
 * reset is not exercised by QEMU and is always rendered as `physical=NOT_TESTED`.
 */

#define APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_VERSION 1U
#define APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_EVIDENCE_VERSION 1U
#define APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OUTPUT_MAX 256U
#define APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CAPACITY 16U
#define APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_CONFIRMATION_TICKS 300U
#define APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DEFAULT_MAX_BLOCKS 512U

enum appliance_production_recovery_executor_state {
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_UNINITIALIZED = 1,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_PREPARED = 2,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_UNAVAILABLE = 3,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_READY = 4,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_DEGRADED = 5,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_PENDING_CONFIRMATION = 6,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_STOPPED = 7,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_FAILED = 8,
};

enum appliance_production_recovery_executor_status {
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK = 0,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL = 1,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE = 4,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DENIED = 5,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION = 6,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DRY_RUN = 7,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLIED = 8,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_CANCELLED = 9,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_EXPIRED = 10,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NO_PENDING = 11,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_CONFIRMATION = 12,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNSUPPORTED_TARGET = 13,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OUT_OF_BOUNDS = 14,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STORAGE_ERROR = 15,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLY_FAILED = 16,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BUFFER_TOO_SMALL = 17,
};

enum appliance_production_recovery_executor_operation {
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE = 0,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_RECOVERY = 1,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_FACTORY_RESET = 2,
};

enum appliance_production_recovery_executor_audit_outcome {
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_NONE = 0,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_ALLOWED = 1,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DENIED = 2,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_UNAVAILABLE = 3,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CANCELLED = 4,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_EXPIRED = 5,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DRY_RUN = 6,
    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_APPLY_FAILED = 7,
};

struct appliance_production_recovery_executor_audit_entry {
    uint64_t sequence;
    uint64_t tick;
    uint64_t token;
    uint32_t operation;
    uint32_t outcome;
    uint32_t session_id;
    uint32_t principal_id;
};

/*
 * Boot, storage and installer evidence published by the ITEM-360 boot
 * coordinator and the ITEM-361 pre-kernel installer helper owners. A request
 * that cannot be backed by this evidence fails closed.
 */
struct appliance_production_recovery_executor_evidence {
    uint32_t version;
    bool boot_foundation_verified;
    bool storage_loader_ready;
    bool installer_recovery_ready;
    bool recovery_trust_active;
    struct appliance_recovery_mode_evidence recovery;
};

/*
 * Bounded storage binding. `partition` is the production state-partition view
 * returned by the ITEM-360 boot coordinator; the layout entries are relative
 * to that partition. `max_blocks` bounds the complete transaction; 0 selects
 * APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DEFAULT_MAX_BLOCKS.
 */
struct appliance_production_recovery_executor_storage {
    struct block_device *partition;
    const struct appliance_state_layout *layout;
    uint64_t max_blocks;
};

struct appliance_production_recovery_executor {
    bool initialized;
    uint32_t version;
    enum appliance_production_recovery_executor_state state;
    enum appliance_production_recovery_executor_status last_status;
    struct appliance_production_auth *auth;
    struct appliance_production_management_plane *management;
    struct appliance_factory_reset_gate *reset_gate;
    struct appliance_recovery_mode_gate *recovery_gate;
    struct appliance_production_recovery_executor_storage storage;
    struct appliance_production_recovery_executor_evidence evidence;
    bool evidence_published;
    enum appliance_production_recovery_executor_operation pending_operation;
    uint64_t confirmation_sequence;
    uint64_t pending_token;
    uint64_t pending_tick;
    uint64_t confirmation_window;
    struct appliance_factory_reset_plan pending_plan;
    uint32_t pending_recovery_action;
    uint32_t pending_stores;
    uint64_t pending_blocks;
    uint32_t last_stores;
    uint32_t last_preserved;
    uint64_t last_written_blocks;
    uint32_t dry_run_count;
    uint32_t applied_count;
    uint32_t denied_count;
    uint32_t cancelled_count;
    uint32_t expired_count;
    uint32_t unavailable_count;
    uint32_t failed_count;
    bool audit_published;
    uint64_t audit_next_sequence;
    uint32_t audit_count;
    uint32_t audit_head;
    struct appliance_production_recovery_executor_audit_entry audit[
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CAPACITY];
};

enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_init(
    struct appliance_production_recovery_executor *executor,
    struct appliance_production_auth *auth,
    struct appliance_production_management_plane *management,
    struct appliance_factory_reset_gate *reset_gate,
    struct appliance_recovery_mode_gate *recovery_gate,
    const struct appliance_production_recovery_executor_storage *storage);
enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_publish_evidence(
    struct appliance_production_recovery_executor *executor,
    const struct appliance_production_recovery_executor_evidence *evidence);
enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_refresh(
    struct appliance_production_recovery_executor *executor);
bool appliance_production_recovery_executor_ready(
    const struct appliance_production_recovery_executor *executor);
enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_request(
    struct appliance_production_recovery_executor *executor,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    enum appliance_management_authorization_method method,
    const uint8_t *target,
    uint32_t target_length,
    bool dry_run,
    uint64_t confirmation_token,
    bool cancel,
    uint64_t now_tick,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_stop(
    struct appliance_production_recovery_executor *executor);
enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_fail_closed(
    struct appliance_production_recovery_executor *executor);
enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_render(
    const struct appliance_production_recovery_executor *executor,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_production_recovery_executor_state
appliance_production_recovery_executor_state(
    const struct appliance_production_recovery_executor *executor);
bool appliance_production_recovery_executor_has_pending(
    const struct appliance_production_recovery_executor *executor);
enum appliance_production_recovery_executor_operation
appliance_production_recovery_executor_pending_operation(
    const struct appliance_production_recovery_executor *executor);
uint64_t appliance_production_recovery_executor_pending_token(
    const struct appliance_production_recovery_executor *executor);
uint64_t appliance_production_recovery_executor_pending_blocks(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_pending_stores(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_last_stores(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_last_preserved(
    const struct appliance_production_recovery_executor *executor);
uint64_t appliance_production_recovery_executor_last_written_blocks(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_dry_run_count(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_applied_count(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_denied_count(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_cancelled_count(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_expired_count(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_unavailable_count(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_failed_count(
    const struct appliance_production_recovery_executor *executor);
uint32_t appliance_production_recovery_executor_audit_count(
    const struct appliance_production_recovery_executor *executor);
bool appliance_production_recovery_executor_audit_entry(
    const struct appliance_production_recovery_executor *executor,
    uint32_t index,
    struct appliance_production_recovery_executor_audit_entry *entry_out);
const char *appliance_production_recovery_executor_state_string(
    enum appliance_production_recovery_executor_state state);
const char *appliance_production_recovery_executor_status_string(
    enum appliance_production_recovery_executor_status status);
const char *appliance_production_recovery_executor_operation_string(
    enum appliance_production_recovery_executor_operation operation);
const char *appliance_production_recovery_executor_audit_outcome_string(
    enum appliance_production_recovery_executor_audit_outcome outcome);

#endif
