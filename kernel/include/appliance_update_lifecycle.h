#ifndef HFOC_APPLIANCE_UPDATE_LIFECYCLE_H
#define HFOC_APPLIANCE_UPDATE_LIFECYCLE_H

#include "appliance_update_record.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-376 boot-side update lifecycle.
 *
 * The durable record says what was intended; this module is the decision a
 * boot actually makes from it, and the settlement after the new slot has
 * either confirmed itself or failed:
 *
 *   resolve  - the pending target's bytes are still intact and its attempt
 *              budget is not exhausted: count this boot durably and hand the
 *              target over.  A damaged slot, or an exhausted budget, rolls
 *              back to the known-good slot instead, so an interrupted or
 *              repeatedly failing update still boots;
 *   accept   - the target confirmed: promote it, advance the downgrade floor
 *              and clear the pending state durably;
 *   fail     - the target did not confirm: drop it and keep the known-good
 *              slot, recording the rollback as evidence.
 *
 * A settle that cannot be made durable is never reported as settled: the
 * module publishes WRITE_FAULT and leaves the record pending, so the next
 * boot retries instead of trusting a decision that was never written.
 *
 * Everything is caller-owned and allocation-free.  The slot readback is an
 * injected callback so the module never touches a block device directly and
 * stays host-testable.
 */

#define APPLIANCE_UPDATE_LIFECYCLE_VERSION 1U
#define APPLIANCE_UPDATE_LIFECYCLE_RENDER_MAX 256U

enum appliance_update_lifecycle_state {
    APPLIANCE_UPDATE_LIFECYCLE_STATE_NONE = 0,
    APPLIANCE_UPDATE_LIFECYCLE_STATE_IDLE = 1,
    APPLIANCE_UPDATE_LIFECYCLE_STATE_PENDING = 2,
    APPLIANCE_UPDATE_LIFECYCLE_STATE_ACCEPTED = 3,
    APPLIANCE_UPDATE_LIFECYCLE_STATE_ROLLED_BACK = 4,
    APPLIANCE_UPDATE_LIFECYCLE_STATE_FAILED = 5,
};

enum appliance_update_lifecycle_status {
    APPLIANCE_UPDATE_LIFECYCLE_OK = 0,
    APPLIANCE_UPDATE_LIFECYCLE_NULL = 1,
    APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE = 2,
    APPLIANCE_UPDATE_LIFECYCLE_BAD_VERSION = 3,
    APPLIANCE_UPDATE_LIFECYCLE_RECORD_FAILED = 4,
    APPLIANCE_UPDATE_LIFECYCLE_NO_PENDING = 5,
    APPLIANCE_UPDATE_LIFECYCLE_NO_ROLLBACK_SOURCE = 6,
    APPLIANCE_UPDATE_LIFECYCLE_DOWNGRADE_REFUSED = 7,
    APPLIANCE_UPDATE_LIFECYCLE_SOURCE_MISMATCH = 8,
    APPLIANCE_UPDATE_LIFECYCLE_BUFFER_TOO_SMALL = 9,
};

/* Does the pending target's slot still hold the recorded bytes? */
typedef bool (*appliance_update_lifecycle_slot_fn)(
    void *context,
    const struct appliance_update_record_entry *entry);

struct appliance_update_lifecycle_io {
    void *context;
    struct appliance_update_record_io record;
    appliance_update_lifecycle_slot_fn slot_intact;
};

struct appliance_update_lifecycle {
    bool initialized;
    uint32_t version;
    enum appliance_update_lifecycle_state state;
    enum appliance_update_lifecycle_status last_status;
    struct appliance_update_record record;
    struct appliance_update_record_entry active;
    bool rollback_selected;
    bool slot_verified;
    uint32_t resolve_count;
    uint32_t accept_count;
    uint32_t rollback_count;
    uint32_t reject_count;
};

struct appliance_update_lifecycle_snapshot {
    bool published;
    uint32_t version;
    enum appliance_update_lifecycle_state state;
    enum appliance_update_lifecycle_status last_status;
    enum appliance_update_record_outcome outcome;
    uint32_t installed_image_id;
    uint32_t attempts;
    uint32_t active_slot_id;
    bool pending;
    bool rollback_selected;
    uint32_t resolve_count;
    uint32_t accept_count;
    uint32_t rollback_count;
    uint32_t reject_count;
};

/* Load the durable record, publishing the first-boot one when absent. */
enum appliance_update_lifecycle_status appliance_update_lifecycle_init(
    struct appliance_update_lifecycle *lifecycle,
    uint32_t installed_image_id,
    const struct appliance_update_record_entry *known_good,
    const struct appliance_update_lifecycle_io *io);
/* The boot decision: hand over the pending target or fall back. */
enum appliance_update_lifecycle_status appliance_update_lifecycle_resolve(
    struct appliance_update_lifecycle *lifecycle,
    const struct appliance_update_lifecycle_io *io);
/* The target confirmed; promote it and advance the downgrade floor. */
enum appliance_update_lifecycle_status appliance_update_lifecycle_accept(
    struct appliance_update_lifecycle *lifecycle,
    uint32_t accepted_image_id,
    const struct appliance_update_lifecycle_io *io);
/* The target did not confirm; roll back and keep the known good. */
enum appliance_update_lifecycle_status appliance_update_lifecycle_fail(
    struct appliance_update_lifecycle *lifecycle,
    const struct appliance_update_lifecycle_io *io);
/* Ordered admission of a new bundle against the durable record. */
enum appliance_update_lifecycle_status appliance_update_lifecycle_admit(
    const struct appliance_update_lifecycle *lifecycle,
    uint32_t target_image_id,
    uint32_t source_image_id);
const struct appliance_update_record_entry *
appliance_update_lifecycle_active(
    const struct appliance_update_lifecycle *lifecycle);
enum appliance_update_lifecycle_status appliance_update_lifecycle_snapshot(
    const struct appliance_update_lifecycle *lifecycle,
    struct appliance_update_lifecycle_snapshot *snapshot_out);
enum appliance_update_lifecycle_status appliance_update_lifecycle_render(
    const struct appliance_update_lifecycle_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_update_lifecycle_state_string(
    enum appliance_update_lifecycle_state state);
const char *appliance_update_lifecycle_status_string(
    enum appliance_update_lifecycle_status status);

#endif
