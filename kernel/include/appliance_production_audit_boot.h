#ifndef HFOC_APPLIANCE_PRODUCTION_AUDIT_BOOT_H
#define HFOC_APPLIANCE_PRODUCTION_AUDIT_BOOT_H

#include "appliance_audit_chain.h"
#include "appliance_audit_export.h"
#include "audit.h"
#include "event_ring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-377 ordinary-boot audit integrity composition.
 *
 * Ordinary boot owns the audit chain: this module builds the chain from the
 * structured audit records the kernel event ring already holds, prepends the
 * reboot-reason record for the boot itself, verifies the whole retained window,
 * and then exercises the external export path once with a cursor (begin, batch,
 * acknowledge, render) so an export consumer is proven to receive exactly the
 * chained records in order.
 *
 * Honest limits of the frozen baseline, all published rather than hidden:
 *
 *   - the chain and the export cursor live in RAM: there is no persistent audit
 *     store in this wave, so a reboot starts a new chain generation and
 *     physical tamper of stored bytes stays NOT TESTED;
 *   - retention is bounded: once the window is full the oldest half is retired,
 *     which is reported as DEGRADED because the appliance no longer holds the
 *     complete session history;
 *   - a structured audit event that fails to decode is counted and published as
 *     `skipped`, and it degrades the view instead of being silently dropped.
 */

#define APPLIANCE_PRODUCTION_AUDIT_BOOT_VERSION 1U
#define APPLIANCE_PRODUCTION_AUDIT_BOOT_RENDER_MAX 640U

#define APPLIANCE_PRODUCTION_AUDIT_BOOT_ROTATE_BATCH \
    (APPLIANCE_AUDIT_CHAIN_CAPACITY / 2U)

/*
 * The frozen baseline has no persistent boot counter, so ordinary boot opens
 * the chain at generation 1.  The generation still binds an export cursor to
 * one chain instance: a cursor from another generation is refused instead of
 * being silently replayed against a fresh window.
 */
#define APPLIANCE_PRODUCTION_AUDIT_BOOT_GENERATION_FIRST 1U

enum appliance_production_audit_boot_state {
    APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_UNAVAILABLE = 1,
    APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_READY = 2,
    APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_DEGRADED = 3,
    APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_TAMPERED = 4,
};

struct appliance_production_audit_view {
    uint32_t version;
    bool published;
    enum appliance_production_audit_boot_state state;
    bool verify_ok;
    bool tamper_detected;
    bool rotated;
    uint64_t generation;
    uint64_t records;
    uint64_t appended;
    uint64_t first_sequence;
    uint64_t next_sequence;
    uint64_t rotated_count;
    uint64_t skipped;
    uint64_t exported;
    uint64_t tamper_sequence;
    uint8_t head_digest[SHA256_DIGEST_SIZE];
};

/*
 * Build (or rebuild) the boot chain: the boot record first, then every
 * structured audit record in `ring` in ring order.  `boot_operation` and
 * `boot_detail` describe the boot record the module appends for this boot.
 * Returns true only when the chain verified and the boot export completed; a
 * window that does not verify is never exported and never publishes a cursor.
 */
bool appliance_production_audit_boot_init(
    struct event_ring *ring,
    uint64_t generation,
    uint64_t boot_operation,
    uint64_t boot_detail);
bool appliance_production_audit_boot_available(void);
enum appliance_production_audit_boot_state
appliance_production_audit_boot_state(void);
const struct appliance_audit_chain *
appliance_production_audit_boot_chain(void);
bool appliance_production_audit_boot_view(
    struct appliance_production_audit_view *view_out);
/*
 * The boot export cursor after the one bounded export run, or NULL when the
 * export never completed.
 */
const struct appliance_audit_export_cursor *
appliance_production_audit_boot_export(void);
enum appliance_audit_export_status
appliance_production_audit_boot_last_status(void);
enum appliance_audit_chain_status
appliance_production_audit_boot_last_chain_status(void);
/* Canonical single-line view published on ordinary boot. */
bool appliance_production_audit_boot_render(
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_production_audit_boot_state_string(
    enum appliance_production_audit_boot_state state);

#endif
