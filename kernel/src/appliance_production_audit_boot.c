#include "appliance_production_audit_boot.h"

#ifdef HFOC_PRODUCTION_SERVICES

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-377 ordinary-boot audit composition.  The chain, the export cursor and
 * the published view are single boot-owned instances; ordinary boot calls
 * `init` once, and the host proof re-initializes the same composition.
 */

struct appliance_production_audit_boot {
    bool initialized;
    bool available;
    struct appliance_audit_chain chain;
    struct appliance_audit_export_cursor cursor;
    struct appliance_production_audit_view view;
    enum appliance_audit_chain_status chain_status;
    enum appliance_audit_export_status export_status;
};

static struct appliance_production_audit_boot
    appliance_production_audit_boot_instance;

static bool appliance_production_audit_boot_append(
    struct appliance_production_audit_boot *boot,
    const struct audit_record *record)
{
    struct appliance_audit_chain *chain = &boot->chain;
    enum appliance_audit_chain_status status;

    status = appliance_audit_chain_append(chain, record, NULL);
    if (status == APPLIANCE_AUDIT_CHAIN_FULL) {
        /*
         * Retention is bounded: retire the oldest half and continue the same
         * anchored chain.  The view reports the rotation as DEGRADED.
         */
        if (appliance_audit_chain_rotate(
                chain,
                APPLIANCE_PRODUCTION_AUDIT_BOOT_ROTATE_BATCH,
                NULL) != APPLIANCE_AUDIT_CHAIN_OK) {
            return false;
        }
        boot->view.rotated = true;
        status = appliance_audit_chain_append(chain, record, NULL);
    }
    return status == APPLIANCE_AUDIT_CHAIN_OK;
}

static void appliance_production_audit_boot_ingest_ring(
    struct appliance_production_audit_boot *boot,
    struct event_ring *ring)
{
    struct kernel_event raw[KERNEL_EVENT_RING_CAPACITY];
    uint32_t count = 0U;
    uint32_t index;

    if (ring == NULL || !event_ring_is_published(ring)) {
        return;
    }
    if (event_ring_snapshot(ring, raw, KERNEL_EVENT_RING_CAPACITY,
                            &count) != EVENT_RING_OK) {
        return;
    }
    for (index = 0U; index < count; ++index) {
        struct audit_record record;

        if (raw[index].flags != AUDIT_EVENT_FLAG_STRUCTURED) {
            continue;
        }
        if (audit_record_from_event(&raw[index], &record) != AUDIT_OK) {
            ++boot->view.skipped;
            continue;
        }
        if (!appliance_production_audit_boot_append(boot, &record)) {
            ++boot->view.skipped;
        }
    }
}

static void appliance_production_audit_boot_run_export(
    struct appliance_production_audit_boot *boot)
{
    struct appliance_audit_chain *chain = &boot->chain;
    struct appliance_audit_export_cursor *cursor = &boot->cursor;
    struct audit_record record;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint64_t sequence = 0U;
    enum appliance_audit_export_status status;

    if (appliance_audit_export_cursor_init(cursor) !=
        APPLIANCE_AUDIT_EXPORT_OK) {
        boot->export_status = APPLIANCE_AUDIT_EXPORT_NULL;
        return;
    }
    status = appliance_audit_export_begin(chain, cursor, 0U);
    if (status != APPLIANCE_AUDIT_EXPORT_OK &&
        status != APPLIANCE_AUDIT_EXPORT_EXPIRED) {
        boot->export_status = status;
        return;
    }
    for (;;) {
        status = appliance_audit_export_next(
            chain, cursor, &record, digest, &sequence);
        if (status != APPLIANCE_AUDIT_EXPORT_OK) {
            break;
        }
        status = appliance_audit_export_ack(cursor, sequence);
        if (status != APPLIANCE_AUDIT_EXPORT_OK) {
            break;
        }
        ++boot->view.exported;
    }
    boot->export_status =
        status == APPLIANCE_AUDIT_EXPORT_EMPTY
            ? APPLIANCE_AUDIT_EXPORT_OK
            : status;
}

static void appliance_production_audit_boot_publish_view(
    struct appliance_production_audit_boot *boot,
    struct appliance_audit_chain_verdict *verdict,
    uint64_t generation,
    uint64_t skipped)
{
    struct appliance_production_audit_view *view = &boot->view;

    view->version = APPLIANCE_PRODUCTION_AUDIT_BOOT_VERSION;
    view->published = true;
    view->verify_ok = verdict->chain_ok;
    view->tamper_detected = verdict->tamper_detected;
    view->tamper_sequence = verdict->tamper_sequence;
    view->generation = generation;
    view->records = appliance_audit_chain_count(&boot->chain);
    view->appended = boot->chain.appended_count;
    view->first_sequence = appliance_audit_chain_first_sequence(
        &boot->chain);
    view->next_sequence = appliance_audit_chain_next_sequence(
        &boot->chain);
    view->rotated_count = boot->chain.rotated_count;
    view->skipped = skipped;
    memcpy(view->head_digest, verdict->head_digest,
           SHA256_DIGEST_SIZE);

    if (!verdict->chain_ok) {
        view->state = APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_TAMPERED;
    } else if (view->rotated || view->skipped != 0U ||
               boot->export_status != APPLIANCE_AUDIT_EXPORT_OK) {
        view->state = APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_DEGRADED;
    } else {
        view->state = APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_READY;
    }
}

bool appliance_production_audit_boot_init(
    struct event_ring *ring,
    uint64_t generation,
    uint64_t boot_operation,
    uint64_t boot_detail)
{
    struct appliance_production_audit_boot *boot =
        &appliance_production_audit_boot_instance;
    struct appliance_audit_chain_verdict verdict;
    struct audit_record record;
    uint64_t skipped;

    memset(boot, 0, sizeof(*boot));
    boot->initialized = true;
    boot->chain_status = APPLIANCE_AUDIT_CHAIN_BAD_STATE;
    boot->export_status = APPLIANCE_AUDIT_EXPORT_BAD_STATE;
    boot->view.version = APPLIANCE_PRODUCTION_AUDIT_BOOT_VERSION;
    boot->view.published = true;
    boot->view.state = APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_UNAVAILABLE;

    if (generation == 0U || boot_operation > UINT64_C(0xFF)) {
        return false;
    }
    /*
     * The ring is the audit source.  Without a published ring there is no
     * evidence to chain, so the composition publishes UNAVAILABLE instead of a
     * fabricated record.
     */
    if (ring == NULL || !event_ring_is_published(ring)) {
        return false;
    }
    boot->chain_status = appliance_audit_chain_init(&boot->chain,
                                                    generation);
    if (boot->chain_status != APPLIANCE_AUDIT_CHAIN_OK) {
        return false;
    }

    /*
     * The boot's own record is the first link, so the chain is never empty and
     * every later record is anchored to this boot.
     */
    if (audit_record_init(
            &record,
            AUDIT_RECORD_REBOOT_REASON,
            (uint32_t)boot_operation,
            0U,
            0U,
            0U,
            0U,
            boot_detail) != AUDIT_OK ||
        !appliance_production_audit_boot_append(boot, &record)) {
        return false;
    }
    appliance_production_audit_boot_ingest_ring(boot, ring);
    skipped = boot->view.skipped;
    if (appliance_audit_chain_verify(&boot->chain, &verdict) !=
        APPLIANCE_AUDIT_CHAIN_OK) {
        boot->chain_status = APPLIANCE_AUDIT_CHAIN_BAD_STATE;
        return false;
    }
    boot->chain_status = APPLIANCE_AUDIT_CHAIN_OK;
    if (verdict.chain_ok) {
        appliance_production_audit_boot_run_export(boot);
    }
    appliance_production_audit_boot_publish_view(
        boot, &verdict, generation, skipped);
    boot->available =
        verdict.chain_ok &&
        boot->export_status == APPLIANCE_AUDIT_EXPORT_OK;
    return boot->available;
}

bool appliance_production_audit_boot_available(void)
{
    return appliance_production_audit_boot_instance.initialized &&
           appliance_production_audit_boot_instance.available;
}

enum appliance_production_audit_boot_state
appliance_production_audit_boot_state(void)
{
    return appliance_production_audit_boot_instance.initialized
        ? appliance_production_audit_boot_instance.view.state
        : APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_NONE;
}

const struct appliance_audit_chain *
appliance_production_audit_boot_chain(void)
{
    return appliance_production_audit_boot_instance.initialized
        ? &appliance_production_audit_boot_instance.chain
        : NULL;
}

bool appliance_production_audit_boot_view(
    struct appliance_production_audit_view *view_out)
{
    const struct appliance_production_audit_boot *boot =
        &appliance_production_audit_boot_instance;

    if (!boot->initialized || view_out == NULL ||
        !boot->view.published) {
        return false;
    }
    *view_out = boot->view;
    return true;
}

const struct appliance_audit_export_cursor *
appliance_production_audit_boot_export(void)
{
    const struct appliance_production_audit_boot *boot =
        &appliance_production_audit_boot_instance;

    if (!boot->initialized || !boot->available ||
        boot->export_status != APPLIANCE_AUDIT_EXPORT_OK) {
        return NULL;
    }
    return &boot->cursor;
}

enum appliance_audit_export_status
appliance_production_audit_boot_last_status(void)
{
    return appliance_production_audit_boot_instance.export_status;
}

enum appliance_audit_chain_status
appliance_production_audit_boot_last_chain_status(void)
{
    return appliance_production_audit_boot_instance.chain_status;
}

static bool appliance_production_audit_boot_append_text(
    char *output,
    size_t capacity,
    size_t *offset,
    const char *text)
{
    size_t index = 0U;

    while (text[index] != '\0') {
        if (*offset + 1U > capacity) {
            return false;
        }
        if (*offset < capacity) {
            output[*offset] = text[index];
        }
        ++(*offset);
        ++index;
    }
    return true;
}

static bool appliance_production_audit_boot_append_u64(
    char *output,
    size_t capacity,
    size_t *offset,
    uint64_t value)
{
    char digits[21];
    uint32_t count = 0U;

    if (value == 0U) {
        return appliance_production_audit_boot_append_text(
            output, capacity, offset, "0");
    }
    while (value != 0U && count < 20U) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count != 0U) {
        char single[2];

        --count;
        single[0] = digits[count];
        single[1] = '\0';
        if (!appliance_production_audit_boot_append_text(
                output, capacity, offset, single)) {
            return false;
        }
    }
    return true;
}

bool appliance_production_audit_boot_render(
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_production_audit_boot *boot =
        &appliance_production_audit_boot_instance;
    struct appliance_audit_chain_snapshot snapshot;
    struct appliance_audit_export_snapshot export_snapshot;
    struct appliance_audit_chain_verdict verdict;
    enum appliance_production_audit_boot_state render_state;
    const char *state;
    uint64_t tamper_sequence = boot->view.tamper_sequence;
    bool verify_ok;
    size_t offset = 0U;

    if (output == NULL || output_length == NULL) {
        return false;
    }
    if (!boot->initialized || !boot->view.published) {
        return false;
    }
    /*
     * The published line carries its own fresh verification.  A window that no
     * longer verifies can never render as READY, even if the frozen boot view
     * was published before the rewrite.
     */
    render_state = boot->view.state;
    verify_ok = boot->view.verify_ok;
    if (render_state !=
            APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_UNAVAILABLE &&
        appliance_audit_chain_verify(&boot->chain, &verdict) ==
            APPLIANCE_AUDIT_CHAIN_OK &&
        !verdict.chain_ok) {
        render_state = APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_TAMPERED;
        verify_ok = false;
        tamper_sequence = verdict.tamper_sequence;
    }
    state = appliance_production_audit_boot_state_string(render_state);
    if (state == NULL) {
        return false;
    }
    memset(output, 0, capacity);
    if (!appliance_production_audit_boot_append_text(
            output, capacity, &offset, "audit_chain v=1 state=") ||
        !appliance_production_audit_boot_append_text(
            output, capacity, &offset, state) ||
        !appliance_production_audit_boot_append_text(
            output, capacity, &offset, " verify=") ||
        !appliance_production_audit_boot_append_text(
            output, capacity, &offset,
            render_state ==
                    APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_UNAVAILABLE
                ? "UNAVAILABLE"
                : (verify_ok ? "OK" : "TAMPERED")) ||
        !appliance_production_audit_boot_append_text(
            output, capacity, &offset, " tamper=") ||
        !appliance_production_audit_boot_append_u64(
            output, capacity, &offset, tamper_sequence) ||
        !appliance_production_audit_boot_append_text(
            output, capacity, &offset, " skipped=") ||
        !appliance_production_audit_boot_append_u64(
            output, capacity, &offset, boot->view.skipped)) {
        return false;
    }
    if (render_state ==
        APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_UNAVAILABLE) {
        if (!appliance_production_audit_boot_append_text(
                output, capacity, &offset, " records=0 export=NONE")) {
            return false;
        }
    } else {
        size_t rendered = 0U;

        if (!appliance_production_audit_boot_append_text(
                output, capacity, &offset, " ") ||
            appliance_audit_chain_snapshot(
                &boot->chain, &snapshot) != APPLIANCE_AUDIT_CHAIN_OK ||
            appliance_audit_chain_render_snapshot(
                &snapshot, output + offset, capacity - offset,
                &rendered) != APPLIANCE_AUDIT_CHAIN_OK) {
            return false;
        }
        offset += rendered;
        if (!appliance_production_audit_boot_append_text(
                output, capacity, &offset, " ") ||
            appliance_audit_export_snapshot(
                &boot->cursor, &export_snapshot) !=
                APPLIANCE_AUDIT_EXPORT_OK) {
            return false;
        }
        rendered = 0U;
        if (appliance_audit_export_render(
                &export_snapshot, output + offset, capacity - offset,
                &rendered) != APPLIANCE_AUDIT_EXPORT_OK) {
            return false;
        }
        offset += rendered;
    }
    if (offset + 1U > capacity) {
        return false;
    }
    output[offset] = '\0';
    *output_length = offset;
    return true;
}

const char *appliance_production_audit_boot_state_string(
    enum appliance_production_audit_boot_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_READY:
        return "READY";
    case APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_TAMPERED:
        return "TAMPERED";
    default:
        return NULL;
    }
}

#else

typedef int appliance_production_audit_boot_translation_unit_guard;

#endif
