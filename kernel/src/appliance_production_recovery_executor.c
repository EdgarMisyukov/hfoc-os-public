#include "appliance_production_recovery_executor.h"

#include "appliance_state_block_device.h"
#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const unsigned char
appliance_production_recovery_executor_data_magic[DATA_REGION_MAGIC_SIZE] =
    DATA_REGION_MAGIC_BYTES;

static bool appliance_production_recovery_executor_state_valid(
    enum appliance_production_recovery_executor_state state)
{
    return state >= APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_NONE &&
           state <= APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_FAILED;
}

static bool appliance_production_recovery_executor_status_valid(
    enum appliance_production_recovery_executor_status status)
{
    return status >= APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK &&
           status <= APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BUFFER_TOO_SMALL;
}

static bool appliance_production_recovery_executor_operation_valid(
    enum appliance_production_recovery_executor_operation operation)
{
    return operation >= APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE &&
           operation <=
               APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_FACTORY_RESET;
}

static bool appliance_production_recovery_executor_initialized(
    const struct appliance_production_recovery_executor *executor)
{
    return executor != NULL &&
           executor->initialized &&
           executor->version ==
               APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_VERSION &&
           appliance_production_recovery_executor_state_valid(
               executor->state) &&
           appliance_production_recovery_executor_status_valid(
               executor->last_status) &&
           appliance_production_recovery_executor_operation_valid(
               executor->pending_operation) &&
           executor->auth != NULL &&
           executor->management != NULL &&
           executor->reset_gate != NULL &&
           executor->recovery_gate != NULL &&
           executor->storage.partition != NULL &&
           executor->storage.layout != NULL;
}

static bool appliance_production_recovery_executor_available(
    const struct appliance_production_recovery_executor *executor)
{
    return appliance_production_auth_available(executor->auth) &&
           appliance_production_management_plane_ready(executor->management);
}

/*
 * A plan can only be derived when the boot foundation, the state storage and
 * the trusted recovery boundary are all published. A factory reset widens
 * that requirement by the installer/recovery source readiness.
 */
static bool appliance_production_recovery_executor_plan_capable(
    const struct appliance_production_recovery_executor *executor)
{
    return executor->evidence_published &&
           executor->evidence.boot_foundation_verified &&
           executor->evidence.storage_loader_ready &&
           !executor->evidence.recovery_trust_active;
}

static bool appliance_production_recovery_executor_factory_capable(
    const struct appliance_production_recovery_executor *executor)
{
    return appliance_production_recovery_executor_plan_capable(executor) &&
           executor->evidence.installer_recovery_ready;
}

static enum appliance_production_recovery_executor_state
appliance_production_recovery_executor_derive_state(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_available(executor)) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_UNAVAILABLE;
    }
    if (executor->pending_operation !=
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_PENDING_CONFIRMATION;
    }
    if (!appliance_production_recovery_executor_factory_capable(executor)) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_DEGRADED;
    }
    if (appliance_production_management_plane_state(executor->management) ==
        APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_DEGRADED) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_DEGRADED;
    }
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_READY;
}

static void appliance_production_recovery_executor_audit(
    struct appliance_production_recovery_executor *executor,
    enum appliance_production_recovery_executor_audit_outcome outcome,
    enum appliance_production_recovery_executor_operation operation,
    uint64_t token,
    uint64_t tick,
    const struct appliance_admin_auth_decision *decision)
{
    struct appliance_production_recovery_executor_audit_entry *entry;

    entry = &executor->audit[executor->audit_head];
    memset(entry, 0, sizeof(*entry));
    entry->sequence = executor->audit_next_sequence;
    entry->tick = tick;
    entry->token = token;
    entry->operation = (uint32_t)operation;
    entry->outcome = (uint32_t)outcome;
    if (decision != NULL) {
        entry->session_id = decision->session_id;
        entry->principal_id = decision->principal_id;
    }
    executor->audit_next_sequence++;
    executor->audit_head =
        (executor->audit_head + 1U) %
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CAPACITY;
    executor->audit_count++;
    executor->audit_published = true;
}

static void appliance_production_recovery_executor_clear_pending(
    struct appliance_production_recovery_executor *executor)
{
    executor->pending_operation =
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE;
    executor->pending_token = 0U;
    executor->pending_tick = 0U;
    executor->pending_recovery_action = 0U;
    executor->pending_stores = 0U;
    executor->pending_blocks = 0U;
    memset(&executor->pending_plan, 0, sizeof(executor->pending_plan));
}

static uint64_t appliance_production_recovery_executor_issue_token(
    struct appliance_production_recovery_executor *executor,
    uint64_t now_tick)
{
    executor->confirmation_sequence++;
    return ((now_tick + 1U) << 16) ^ (executor->confirmation_sequence + 1U);
}

static bool appliance_production_recovery_executor_append(
    char *rendered,
    size_t *offset,
    const char *text,
    size_t capacity)
{
    size_t length = strlen(text);
    size_t index;

    if (*offset + length >= capacity) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        rendered[*offset + index] = text[index];
    }
    *offset += length;
    rendered[*offset] = '\0';
    return true;
}

static bool appliance_production_recovery_executor_append_u64(
    char *rendered,
    size_t *offset,
    uint64_t value,
    size_t capacity)
{
    char digits[21];
    size_t length = 0U;
    size_t index;

    do {
        digits[length] = (char)('0' + (value % 10U));
        value /= 10U;
        ++length;
    } while (value != 0U);
    if (*offset + length >= capacity) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        rendered[*offset + index] = digits[length - 1U - index];
    }
    *offset += length;
    rendered[*offset] = '\0';
    return true;
}

static const char *appliance_production_recovery_executor_action_name(
    uint32_t action)
{
    switch (action) {
    case APPLIANCE_RECOVERY_MODE_ACTION_KEEP_LAST_KNOWN_GOOD:
        return "KEEP_LAST_KNOWN_GOOD";
    case APPLIANCE_RECOVERY_MODE_ACTION_SAFE_DEFAULT:
        return "SAFE_DEFAULT";
    case APPLIANCE_RECOVERY_MODE_ACTION_AUTOMATIC_ROLLBACK:
        return "AUTOMATIC_ROLLBACK";
    case APPLIANCE_RECOVERY_MODE_ACTION_SUPERVISED_RESTART:
        return "SUPERVISED_RESTART";
    case APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY:
        return "READ_ONLY";
    default:
        return "NONE";
    }
}

static enum appliance_production_recovery_executor_operation
appliance_production_recovery_executor_operation_for_mutation(
    enum appliance_management_authorization_mutation mutation)
{
    switch (mutation) {
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_RECOVERY_ACTION:
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_RECOVERY;
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_FACTORY_RESET:
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_FACTORY_RESET;
    default:
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE;
    }
}

static enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_finish(
    struct appliance_production_recovery_executor *executor,
    enum appliance_production_recovery_executor_status status,
    enum appliance_production_recovery_executor_operation operation,
    uint32_t action,
    uint32_t stores,
    uint64_t blocks,
    uint32_t preserved,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t offset = 0U;

    executor->last_status = status;
    executor->state = appliance_production_recovery_executor_derive_state(
        executor);
    *output_length = 0U;
    if (output == NULL || capacity == 0U) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BUFFER_TOO_SMALL;
    }
    if (!appliance_production_recovery_executor_append(
            output, &offset, "recovery=", capacity) ||
        !appliance_production_recovery_executor_append(
            output,
            &offset,
            appliance_production_recovery_executor_operation_string(
                operation),
            capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " action=", capacity) ||
        !appliance_production_recovery_executor_append(
            output,
            &offset,
            appliance_production_recovery_executor_action_name(action),
            capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " state=", capacity) ||
        !appliance_production_recovery_executor_append(
            output,
            &offset,
            appliance_production_recovery_executor_state_string(
                executor->state),
            capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " result=", capacity) ||
        !appliance_production_recovery_executor_append(
            output,
            &offset,
            appliance_production_recovery_executor_status_string(status),
            capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " stores=", capacity) ||
        !appliance_production_recovery_executor_append_u64(
            output, &offset, stores, capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " blocks=", capacity) ||
        !appliance_production_recovery_executor_append_u64(
            output, &offset, blocks, capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " preserved=", capacity) ||
        !appliance_production_recovery_executor_append_u64(
            output, &offset, preserved, capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " physical=NOT_TESTED", capacity)) {
        *output_length = 0U;
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BUFFER_TOO_SMALL;
    }
    *output_length = offset;
    return status;
}

/*
 * Measures a plan against the published state layout. In strict mode a
 * missing store entry, an out-of-partition range or a transaction wider than
 * the configured block bound refuses the plan before the first write; the
 * counting mode is used only to render an already validated plan.
 */
static enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_measure_plan(
    const struct appliance_production_recovery_executor *executor,
    const struct appliance_factory_reset_plan *plan,
    bool strict,
    uint32_t *stores_out,
    uint64_t *blocks_out,
    uint32_t *preserved_out)
{
    uint32_t stores = 0U;
    uint32_t preserved = 0U;
    uint64_t blocks = 0U;
    uint32_t index;

    if (plan == NULL) {
        *stores_out = 0U;
        *blocks_out = 0U;
        *preserved_out = 0U;
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
    }
    if (plan->version != APPLIANCE_FACTORY_RESET_VERSION ||
        plan->namespace_count > APPLIANCE_FACTORY_RESET_NAMESPACE_COUNT) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE;
    }
    for (index = 0U; index < plan->namespace_count; ++index) {
        uint32_t action = plan->namespaces[index].action;
        bool matched = false;
        uint32_t entry_index;

        if (action == APPLIANCE_FACTORY_RESET_ACTION_PRESERVE_FOUNDATION) {
            ++preserved;
            continue;
        }
        if (action != APPLIANCE_FACTORY_RESET_ACTION_CLEAR &&
            action !=
                APPLIANCE_FACTORY_RESET_ACTION_REWRITE_FACTORY_DEFAULT) {
            return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE;
        }
        for (entry_index = 0U;
             entry_index < executor->storage.layout->entry_count;
             ++entry_index) {
            const struct appliance_state_layout_entry *entry =
                &executor->storage.layout->entries[entry_index];

            if (entry->namespace_id != plan->namespaces[index].id) {
                continue;
            }
            matched = true;
            if (entry->block_count == 0U ||
                entry->block_offset >
                    executor->storage.partition->block_count ||
                entry->block_count >
                    executor->storage.partition->block_count -
                        entry->block_offset) {
                return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OUT_OF_BOUNDS;
            }
            ++stores;
            blocks += entry->block_count;
        }
        if (!matched && strict) {
            return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OUT_OF_BOUNDS;
        }
    }
    if (blocks > executor->storage.max_blocks) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OUT_OF_BOUNDS;
    }
    *stores_out = stores;
    *blocks_out = blocks;
    *preserved_out = preserved;
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
}

static enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_write_block(
    struct appliance_production_recovery_executor *executor,
    uint64_t block,
    const unsigned char *data)
{
    enum block_device_status status = block_device_write(
        executor->storage.partition,
        block * APPLIANCE_STATE_BLOCK_SIZE,
        data,
        APPLIANCE_STATE_BLOCK_SIZE);

    return status == BLOCK_DEVICE_OK
        ? APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK
        : APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STORAGE_ERROR;
}

static enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_read_block(
    struct appliance_production_recovery_executor *executor,
    uint64_t block,
    unsigned char *buffer)
{
    enum block_device_status status = block_device_read(
        executor->storage.partition,
        block * APPLIANCE_STATE_BLOCK_SIZE,
        buffer,
        APPLIANCE_STATE_BLOCK_SIZE);

    return status == BLOCK_DEVICE_OK
        ? APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK
        : APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STORAGE_ERROR;
}

static bool appliance_production_recovery_executor_is_zero(
    const unsigned char *buffer,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (buffer[index] != 0U) {
            return false;
        }
    }
    return true;
}

static enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_clear_entry(
    struct appliance_production_recovery_executor *executor,
    const struct appliance_state_layout_entry *entry,
    uint64_t *written_blocks)
{
    unsigned char zero[APPLIANCE_STATE_BLOCK_SIZE];
    enum appliance_production_recovery_executor_status status;
    uint64_t index;

    memset(zero, 0, sizeof(zero));
    for (index = 0U; index < entry->block_count; ++index) {
        status = appliance_production_recovery_executor_write_block(
            executor, entry->block_offset + index, zero);
        if (status != APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
            return status;
        }
        ++*written_blocks;
    }
    if (block_device_flush(executor->storage.partition) != BLOCK_DEVICE_OK) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STORAGE_ERROR;
    }
    for (index = 0U; index < entry->block_count; ++index) {
        unsigned char buffer[APPLIANCE_STATE_BLOCK_SIZE];

        status = appliance_production_recovery_executor_read_block(
            executor, entry->block_offset + index, buffer);
        if (status != APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
            return status;
        }
        if (!appliance_production_recovery_executor_is_zero(
                buffer, sizeof(buffer))) {
            return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLY_FAILED;
        }
    }
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
}

static void appliance_production_recovery_executor_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

/*
 * REWRITE_FACTORY_DEFAULT rebuilds the store as an empty data region: the
 * journal body is cleared and the block-0 region header is rewritten, so the
 * existing store loader publishes its validated safe default on the next
 * load. CLEAR removes the whole store, including the region header.
 */
static enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_rewrite_default_entry(
    struct appliance_production_recovery_executor *executor,
    const struct appliance_state_layout_entry *entry,
    uint64_t *written_blocks)
{
    unsigned char header[APPLIANCE_STATE_BLOCK_SIZE];
    unsigned char zero[APPLIANCE_STATE_BLOCK_SIZE];
    unsigned char buffer[APPLIANCE_STATE_BLOCK_SIZE];
    enum appliance_production_recovery_executor_status status;
    uint64_t index;

    memset(zero, 0, sizeof(zero));
    for (index = 1U; index < entry->block_count; ++index) {
        status = appliance_production_recovery_executor_write_block(
            executor, entry->block_offset + index, zero);
        if (status != APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
            return status;
        }
        ++*written_blocks;
    }
    memset(header, 0, sizeof(header));
    memcpy(header,
           appliance_production_recovery_executor_data_magic,
           sizeof(appliance_production_recovery_executor_data_magic));
    header[8U] = DATA_REGION_FORMAT_VERSION;
    appliance_production_recovery_executor_store_le64(header + 16U, 1U);
    appliance_production_recovery_executor_store_le64(
        header + 24U, entry->block_count - 1U);
    status = appliance_production_recovery_executor_write_block(
        executor, entry->block_offset, header);
    if (status != APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
        return status;
    }
    ++*written_blocks;
    if (block_device_flush(executor->storage.partition) != BLOCK_DEVICE_OK) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STORAGE_ERROR;
    }
    status = appliance_production_recovery_executor_read_block(
        executor, entry->block_offset, buffer);
    if (status != APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
        return status;
    }
    if (memcmp(buffer, header, sizeof(header)) != 0) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLY_FAILED;
    }
    for (index = 1U; index < entry->block_count; ++index) {
        status = appliance_production_recovery_executor_read_block(
            executor, entry->block_offset + index, buffer);
        if (status != APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
            return status;
        }
        if (!appliance_production_recovery_executor_is_zero(
                buffer, sizeof(buffer))) {
            return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLY_FAILED;
        }
    }
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
}

static enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_execute_plan(
    struct appliance_production_recovery_executor *executor,
    const struct appliance_factory_reset_plan *plan,
    uint64_t *written_out,
    uint32_t *preserved_out)
{
    uint64_t written = 0U;
    uint32_t preserved = 0U;
    uint32_t index;

    for (index = 0U; index < plan->namespace_count; ++index) {
        uint32_t action = plan->namespaces[index].action;
        uint32_t entry_index;

        if (action == APPLIANCE_FACTORY_RESET_ACTION_PRESERVE_FOUNDATION) {
            ++preserved;
            continue;
        }
        for (entry_index = 0U;
             entry_index < executor->storage.layout->entry_count;
             ++entry_index) {
            const struct appliance_state_layout_entry *entry =
                &executor->storage.layout->entries[entry_index];
            enum appliance_production_recovery_executor_status status;

            if (entry->namespace_id != plan->namespaces[index].id) {
                continue;
            }
            if (action == APPLIANCE_FACTORY_RESET_ACTION_CLEAR) {
                status = appliance_production_recovery_executor_clear_entry(
                    executor, entry, &written);
            } else {
                status =
                    appliance_production_recovery_executor_rewrite_default_entry(
                        executor, entry, &written);
            }
            if (status != APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
                *written_out = written;
                *preserved_out = preserved;
                return status;
            }
        }
    }
    *written_out = written;
    *preserved_out = preserved;
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
}

static enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_build_recovery_plan(
    struct appliance_production_recovery_executor *executor,
    uint32_t *action_out,
    struct appliance_factory_reset_plan *plan_out)
{
    struct appliance_recovery_mode_decision decision;
    uint32_t index;

    memset(&decision, 0, sizeof(decision));
    memset(executor->recovery_gate, 0, sizeof(*executor->recovery_gate));
    if (appliance_recovery_mode_init(executor->recovery_gate) !=
        APPLIANCE_RECOVERY_MODE_OK) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE;
    }
    if (appliance_recovery_mode_evaluate(
            executor->recovery_gate,
            &executor->evidence.recovery,
            &decision) != APPLIANCE_RECOVERY_MODE_OK) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE;
    }
    *action_out = decision.action;
    memset(plan_out, 0, sizeof(*plan_out));
    plan_out->version = APPLIANCE_FACTORY_RESET_VERSION;
    plan_out->namespace_count = APPLIANCE_FACTORY_RESET_NAMESPACE_COUNT;
    plan_out->local_only = true;
    plan_out->preserves_boot_foundation = true;
    plan_out->preserves_recovery_foundation = true;
    for (index = 0U; index < APPLIANCE_FACTORY_RESET_NAMESPACE_COUNT;
         ++index) {
        plan_out->namespaces[index].id =
            (enum appliance_persistent_storage_namespace_id)(index + 1U);
        plan_out->namespaces[index].action =
            APPLIANCE_FACTORY_RESET_ACTION_PRESERVE_FOUNDATION;
    }
    if (decision.action == APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
    }
    if (decision.action != APPLIANCE_RECOVERY_MODE_ACTION_SAFE_DEFAULT) {
        /*
         * Last-known-good, automatic rollback and supervised restart are
         * owned by their own boundaries (update slots, service supervisor).
         * This executor refuses them instead of reporting a reset it cannot
         * perform.
         */
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNSUPPORTED_TARGET;
    }
    plan_out->namespaces[0].action =
        APPLIANCE_FACTORY_RESET_ACTION_REWRITE_FACTORY_DEFAULT;
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
}

static enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_build_factory_reset_plan(
    struct appliance_production_recovery_executor *executor,
    struct appliance_factory_reset_plan *plan_out)
{
    struct appliance_factory_reset_evidence evidence;
    enum appliance_factory_reset_status status;

    memset(&evidence, 0, sizeof(evidence));
    evidence.version = APPLIANCE_FACTORY_RESET_VERSION;
    evidence.authorization_verified = true;
    evidence.boot_foundation_verified =
        executor->evidence.boot_foundation_verified;
    evidence.storage_loader_ready = executor->evidence.storage_loader_ready;
    evidence.recovery_trust_active =
        executor->evidence.recovery_trust_active;
    memset(executor->reset_gate, 0, sizeof(*executor->reset_gate));
    if (appliance_factory_reset_init(executor->reset_gate) !=
        APPLIANCE_FACTORY_RESET_OK) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE;
    }
    status = appliance_factory_reset_evaluate(
        executor->reset_gate, &evidence, plan_out);
    if (status == APPLIANCE_FACTORY_RESET_OK) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
    }
    if (status == APPLIANCE_FACTORY_RESET_BAD_STATE ||
        status == APPLIANCE_FACTORY_RESET_BAD_REQUEST ||
        status == APPLIANCE_FACTORY_RESET_NULL) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE;
    }
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE;
}

enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_init(
    struct appliance_production_recovery_executor *executor,
    struct appliance_production_auth *auth,
    struct appliance_production_management_plane *management,
    struct appliance_factory_reset_gate *reset_gate,
    struct appliance_recovery_mode_gate *recovery_gate,
    const struct appliance_production_recovery_executor_storage *storage)
{
    uint64_t max_blocks;

    if (executor == NULL || auth == NULL || management == NULL ||
        reset_gate == NULL || recovery_gate == NULL || storage == NULL) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL;
    }
    if (storage->partition == NULL || storage->layout == NULL) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL;
    }
    if (block_device_validate(storage->partition) != BLOCK_DEVICE_OK ||
        storage->partition->block_size != APPLIANCE_STATE_BLOCK_SIZE) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE;
    }
    if (appliance_state_layout_validate(storage->layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE;
    }
    max_blocks = storage->max_blocks;
    if (max_blocks == 0U) {
        max_blocks =
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DEFAULT_MAX_BLOCKS;
    }
    memset(executor, 0, sizeof(*executor));
    executor->initialized = true;
    executor->version = APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_VERSION;
    executor->state = APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_PREPARED;
    executor->last_status = APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
    executor->auth = auth;
    executor->management = management;
    executor->reset_gate = reset_gate;
    executor->recovery_gate = recovery_gate;
    executor->storage = *storage;
    executor->storage.max_blocks = max_blocks;
    executor->confirmation_window =
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_CONFIRMATION_TICKS;
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
}

enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_publish_evidence(
    struct appliance_production_recovery_executor *executor,
    const struct appliance_production_recovery_executor_evidence *evidence)
{
    if (!appliance_production_recovery_executor_initialized(executor) ||
        evidence == NULL) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL;
    }
    if (evidence->version !=
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_EVIDENCE_VERSION) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_VERSION;
    }
    executor->evidence = *evidence;
    executor->evidence_published = true;
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
}

enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_refresh(
    struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL;
    }
    if (executor->state ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_STOPPED ||
        executor->state ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_FAILED) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE;
    }
    executor->state =
        appliance_production_recovery_executor_derive_state(executor);
    if (executor->state == APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_READY ||
        executor->state ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_DEGRADED ||
        executor->state ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_PENDING_CONFIRMATION) {
        executor->last_status = APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
    }
    executor->last_status = APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE;
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE;
}

bool appliance_production_recovery_executor_ready(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return false;
    }
    if (executor->state ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_STOPPED ||
        executor->state ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_FAILED) {
        return false;
    }
    return appliance_production_recovery_executor_available(executor);
}

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
    size_t *output_length)
{
    struct appliance_admin_auth_decision decision;
    enum appliance_management_authorization_mutation mutation =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    enum appliance_production_recovery_executor_operation operation =
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE;
    enum appliance_production_auth_status auth_status;
    struct appliance_factory_reset_plan plan;
    enum appliance_production_recovery_executor_status plan_status;
    uint32_t action = 0U;
    uint32_t stores = 0U;
    uint32_t preserved = 0U;
    uint64_t blocks = 0U;
    uint64_t written = 0U;

    if (!appliance_production_recovery_executor_initialized(executor) ||
        output == NULL || output_length == NULL || target == NULL) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL;
    }
    *output_length = 0U;
    if (executor->state ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_STOPPED ||
        executor->state ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_FAILED) {
        executor->last_status =
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE;
        return executor->last_status;
    }

    if (executor->pending_operation !=
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE &&
        now_tick > executor->pending_tick + executor->confirmation_window) {
        enum appliance_production_recovery_executor_operation expired =
            executor->pending_operation;
        uint64_t expired_token = executor->pending_token;

        appliance_production_recovery_executor_clear_pending(executor);
        appliance_production_recovery_executor_audit(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_EXPIRED,
            expired,
            expired_token,
            now_tick,
            NULL);
        ++executor->expired_count;
        return appliance_production_recovery_executor_finish(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_EXPIRED,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE,
            0U,
            0U,
            0U,
            0U,
            output,
            capacity,
            output_length);
    }

    if (!appliance_production_recovery_executor_available(executor)) {
        ++executor->unavailable_count;
        appliance_production_recovery_executor_audit(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_UNAVAILABLE,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE,
            0U,
            now_tick,
            NULL);
        return appliance_production_recovery_executor_finish(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE,
            0U,
            0U,
            0U,
            0U,
            output,
            capacity,
            output_length);
    }

    memset(&decision, 0, sizeof(decision));
    auth_status = appliance_production_auth_authorize_mutation(
        executor->auth,
        authorization_header,
        authorization_header_length,
        method,
        target,
        target_length,
        now_tick,
        &decision,
        &mutation);
    if (auth_status != APPLIANCE_PRODUCTION_AUTH_ALLOWED) {
        if (auth_status == APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE) {
            ++executor->unavailable_count;
            appliance_production_recovery_executor_audit(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_UNAVAILABLE,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE,
                0U,
                now_tick,
                &decision);
            return appliance_production_recovery_executor_finish(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE,
                0U,
                0U,
                0U,
                0U,
                output,
                capacity,
                output_length);
        }
        ++executor->denied_count;
        appliance_production_recovery_executor_audit(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DENIED,
            operation,
            confirmation_token,
            now_tick,
            &decision);
        return appliance_production_recovery_executor_finish(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DENIED,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE,
            0U,
            0U,
            0U,
            0U,
            output,
            capacity,
            output_length);
    }

    operation =
        appliance_production_recovery_executor_operation_for_mutation(
            mutation);

    if (cancel) {
        if (executor->pending_operation ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE) {
            return appliance_production_recovery_executor_finish(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NO_PENDING,
                operation,
                0U,
                0U,
                0U,
                0U,
                output,
                capacity,
                output_length);
        }
        if (confirmation_token != executor->pending_token) {
            /*
             * A mis-typed confirmation is an audited refusal, not an
             * authorization denial: `denied_count` counts requests the
             * authorization gate rejected.
             */
            appliance_production_recovery_executor_audit(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DENIED,
                executor->pending_operation,
                confirmation_token,
                now_tick,
                &decision);
            return appliance_production_recovery_executor_finish(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_CONFIRMATION,
                executor->pending_operation,
                executor->pending_recovery_action,
                0U,
                0U,
                0U,
                output,
                capacity,
                output_length);
        }
        appliance_production_recovery_executor_audit(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CANCELLED,
            executor->pending_operation,
            executor->pending_token,
            now_tick,
            &decision);
        appliance_production_recovery_executor_clear_pending(executor);
        ++executor->cancelled_count;
        return appliance_production_recovery_executor_finish(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_CANCELLED,
            operation,
            0U,
            0U,
            0U,
            0U,
            output,
            capacity,
            output_length);
    }

    if (confirmation_token != 0U) {
        enum appliance_production_recovery_executor_operation pending;
        uint32_t pending_action;
        uint32_t pending_stores;
        uint64_t pending_blocks;
        uint32_t pending_preserved;

        if (executor->pending_operation ==
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE ||
            confirmation_token != executor->pending_token) {
            appliance_production_recovery_executor_audit(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DENIED,
                executor->pending_operation,
                confirmation_token,
                now_tick,
                &decision);
            return appliance_production_recovery_executor_finish(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_CONFIRMATION,
                executor->pending_operation,
                executor->pending_recovery_action,
                0U,
                0U,
                0U,
                output,
                capacity,
                output_length);
        }
        if (operation != executor->pending_operation ||
            (operation ==
                 APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE)) {
            appliance_production_recovery_executor_clear_pending(executor);
            appliance_production_recovery_executor_audit(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DENIED,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE,
                confirmation_token,
                now_tick,
                &decision);
            return appliance_production_recovery_executor_finish(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNSUPPORTED_TARGET,
                operation,
                0U,
                0U,
                0U,
                0U,
                output,
                capacity,
                output_length);
        }
        if ((executor->pending_operation ==
                 APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_FACTORY_RESET &&
             !appliance_production_recovery_executor_factory_capable(
                 executor)) ||
            (executor->pending_operation ==
                 APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_RECOVERY &&
             !appliance_production_recovery_executor_plan_capable(
                 executor))) {
            ++executor->unavailable_count;
            appliance_production_recovery_executor_clear_pending(executor);
            appliance_production_recovery_executor_audit(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_UNAVAILABLE,
                operation,
                confirmation_token,
                now_tick,
                &decision);
            return appliance_production_recovery_executor_finish(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE,
                operation,
                0U,
                0U,
                0U,
                0U,
                output,
                capacity,
                output_length);
        }

        pending = executor->pending_operation;
        pending_action = executor->pending_recovery_action;
        pending_stores = executor->pending_stores;
        pending_blocks = executor->pending_blocks;
        pending_preserved = 0U;
        (void)appliance_production_recovery_executor_measure_plan(
            executor,
            &executor->pending_plan,
            false,
            &pending_stores,
            &pending_blocks,
            &pending_preserved);
        plan_status = appliance_production_recovery_executor_execute_plan(
            executor,
            &executor->pending_plan,
            &written,
            &pending_preserved);
        if (plan_status != APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
            executor->last_written_blocks = written;
            appliance_production_recovery_executor_clear_pending(executor);
            ++executor->failed_count;
            appliance_production_recovery_executor_audit(
                executor,
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_APPLY_FAILED,
                pending,
                confirmation_token,
                now_tick,
                &decision);
            return appliance_production_recovery_executor_finish(
                executor,
                plan_status,
                pending,
                pending_action,
                pending_stores,
                written,
                pending_preserved,
                output,
                capacity,
                output_length);
        }
        executor->last_stores = pending_stores;
        executor->last_written_blocks = written;
        executor->last_preserved = pending_preserved;
        appliance_production_recovery_executor_clear_pending(executor);
        ++executor->applied_count;
        appliance_production_recovery_executor_audit(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_ALLOWED,
            pending,
            confirmation_token,
            now_tick,
            &decision);
        return appliance_production_recovery_executor_finish(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLIED,
            pending,
            pending_action,
            pending_stores,
            written,
            pending_preserved,
            output,
            capacity,
            output_length);
    }

    if (operation == APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE) {
        appliance_production_recovery_executor_audit(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DENIED,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE,
            0U,
            now_tick,
            &decision);
        return appliance_production_recovery_executor_finish(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNSUPPORTED_TARGET,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE,
            0U,
            0U,
            0U,
            0U,
            output,
            capacity,
            output_length);
    }
    if (operation ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_FACTORY_RESET &&
        !appliance_production_recovery_executor_factory_capable(executor)) {
        ++executor->unavailable_count;
        appliance_production_recovery_executor_audit(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_UNAVAILABLE,
            operation,
            0U,
            now_tick,
            &decision);
        return appliance_production_recovery_executor_finish(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE,
            operation,
            0U,
            0U,
            0U,
            0U,
            output,
            capacity,
            output_length);
    }
    if (operation == APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_RECOVERY &&
        !appliance_production_recovery_executor_plan_capable(executor)) {
        ++executor->unavailable_count;
        appliance_production_recovery_executor_audit(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_UNAVAILABLE,
            operation,
            0U,
            now_tick,
            &decision);
        return appliance_production_recovery_executor_finish(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE,
            operation,
            0U,
            0U,
            0U,
            0U,
            output,
            capacity,
            output_length);
    }

    memset(&plan, 0, sizeof(plan));
    if (operation ==
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_FACTORY_RESET) {
        plan_status =
            appliance_production_recovery_executor_build_factory_reset_plan(
                executor, &plan);
    } else {
        plan_status =
            appliance_production_recovery_executor_build_recovery_plan(
                executor, &action, &plan);
    }
    if (plan_status != APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
        appliance_production_recovery_executor_audit(
            executor,
            plan_status ==
                    APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNSUPPORTED_TARGET
                ? APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DENIED
                : APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_UNAVAILABLE,
            operation,
            0U,
            now_tick,
            &decision);
        if (plan_status ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE) {
            ++executor->unavailable_count;
        }
        return appliance_production_recovery_executor_finish(
            executor,
            plan_status,
            operation,
            action,
            0U,
            0U,
            0U,
            output,
            capacity,
            output_length);
    }
    plan_status = appliance_production_recovery_executor_measure_plan(
        executor, &plan, true, &stores, &blocks, &preserved);
    if (plan_status != APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
        ++executor->failed_count;
        appliance_production_recovery_executor_audit(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_APPLY_FAILED,
            operation,
            0U,
            now_tick,
            &decision);
        return appliance_production_recovery_executor_finish(
            executor,
            plan_status,
            operation,
            action,
            0U,
            0U,
            0U,
            output,
            capacity,
            output_length);
    }
    if (dry_run) {
        ++executor->dry_run_count;
        appliance_production_recovery_executor_audit(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DRY_RUN,
            operation,
            0U,
            now_tick,
            &decision);
        return appliance_production_recovery_executor_finish(
            executor,
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DRY_RUN,
            operation,
            action,
            stores,
            blocks,
            preserved,
            output,
            capacity,
            output_length);
    }

    executor->pending_operation = operation;
    executor->pending_recovery_action = action;
    executor->pending_plan = plan;
    executor->pending_stores = stores;
    executor->pending_blocks = blocks;
    executor->pending_token =
        appliance_production_recovery_executor_issue_token(executor, now_tick);
    executor->pending_tick = now_tick;
    return appliance_production_recovery_executor_finish(
        executor,
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION,
        operation,
        action,
        stores,
        blocks,
        preserved,
        output,
        capacity,
        output_length);
}

enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_stop(
    struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL;
    }
    if (executor->pending_operation !=
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE) {
        appliance_production_recovery_executor_clear_pending(executor);
    }
    executor->state = APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_STOPPED;
    executor->last_status = APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
}

enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_fail_closed(
    struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL;
    }
    if (executor->pending_operation !=
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE) {
        appliance_production_recovery_executor_clear_pending(executor);
    }
    executor->state = APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_FAILED;
    executor->last_status = APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE;
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE;
}

enum appliance_production_recovery_executor_status
appliance_production_recovery_executor_render(
    const struct appliance_production_recovery_executor *executor,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t offset = 0U;

    if (!appliance_production_recovery_executor_initialized(executor) ||
        output == NULL || output_length == NULL) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL;
    }
    *output_length = 0U;
    if (capacity == 0U) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BUFFER_TOO_SMALL;
    }
    if (!appliance_production_recovery_executor_append(
            output, &offset, "production_recovery v=1 state=", capacity) ||
        !appliance_production_recovery_executor_append(
            output,
            &offset,
            appliance_production_recovery_executor_state_string(
                appliance_production_recovery_executor_state(executor)),
            capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " operation=", capacity) ||
        !appliance_production_recovery_executor_append(
            output,
            &offset,
            appliance_production_recovery_executor_operation_string(
                executor->pending_operation),
            capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " result=", capacity) ||
        !appliance_production_recovery_executor_append(
            output,
            &offset,
            appliance_production_recovery_executor_status_string(
                executor->last_status),
            capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " stores=", capacity) ||
        !appliance_production_recovery_executor_append_u64(
            output, &offset, executor->last_stores, capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " blocks=", capacity) ||
        !appliance_production_recovery_executor_append_u64(
            output, &offset, executor->last_written_blocks, capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " preserved=", capacity) ||
        !appliance_production_recovery_executor_append_u64(
            output, &offset, executor->last_preserved, capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " applied=", capacity) ||
        !appliance_production_recovery_executor_append_u64(
            output, &offset, executor->applied_count, capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " denied=", capacity) ||
        !appliance_production_recovery_executor_append_u64(
            output, &offset, executor->denied_count, capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " failed=", capacity) ||
        !appliance_production_recovery_executor_append_u64(
            output, &offset, executor->failed_count, capacity) ||
        !appliance_production_recovery_executor_append(
            output, &offset, " physical=NOT_TESTED", capacity)) {
        *output_length = 0U;
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BUFFER_TOO_SMALL;
    }
    *output_length = offset;
    return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK;
}

enum appliance_production_recovery_executor_state
appliance_production_recovery_executor_state(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_UNINITIALIZED;
    }
    if (executor->state ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_STOPPED ||
        executor->state ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_FAILED) {
        return executor->state;
    }
    if (executor->state ==
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_PENDING_CONFIRMATION) {
        return executor->state;
    }
    return appliance_production_recovery_executor_derive_state(executor);
}

bool appliance_production_recovery_executor_has_pending(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return false;
    }
    return executor->pending_operation !=
           APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE;
}

enum appliance_production_recovery_executor_operation
appliance_production_recovery_executor_pending_operation(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_has_pending(executor)) {
        return APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE;
    }
    return executor->pending_operation;
}

uint64_t appliance_production_recovery_executor_pending_token(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_has_pending(executor)) {
        return 0U;
    }
    return executor->pending_token;
}

uint64_t appliance_production_recovery_executor_pending_blocks(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->pending_blocks;
}

uint32_t appliance_production_recovery_executor_pending_stores(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->pending_stores;
}

uint32_t appliance_production_recovery_executor_last_stores(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->last_stores;
}

uint32_t appliance_production_recovery_executor_last_preserved(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->last_preserved;
}

uint64_t appliance_production_recovery_executor_last_written_blocks(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->last_written_blocks;
}

uint32_t appliance_production_recovery_executor_dry_run_count(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->dry_run_count;
}

uint32_t appliance_production_recovery_executor_applied_count(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->applied_count;
}

uint32_t appliance_production_recovery_executor_denied_count(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->denied_count;
}

uint32_t appliance_production_recovery_executor_cancelled_count(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->cancelled_count;
}

uint32_t appliance_production_recovery_executor_expired_count(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->expired_count;
}

uint32_t appliance_production_recovery_executor_unavailable_count(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->unavailable_count;
}

uint32_t appliance_production_recovery_executor_failed_count(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->failed_count;
}

uint32_t appliance_production_recovery_executor_audit_count(
    const struct appliance_production_recovery_executor *executor)
{
    if (!appliance_production_recovery_executor_initialized(executor)) {
        return 0U;
    }
    return executor->audit_count;
}

bool appliance_production_recovery_executor_audit_entry(
    const struct appliance_production_recovery_executor *executor,
    uint32_t index,
    struct appliance_production_recovery_executor_audit_entry *entry_out)
{
    uint32_t retained;
    uint32_t start;
    uint32_t slot;

    if (!appliance_production_recovery_executor_initialized(executor) ||
        entry_out == NULL) {
        return false;
    }
    retained = executor->audit_count <
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CAPACITY
        ? executor->audit_count
        : APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CAPACITY;
    if (index >= retained) {
        return false;
    }
    start = (executor->audit_head +
             APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CAPACITY -
             retained) %
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CAPACITY;
    slot = (start + index) %
           APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CAPACITY;
    *entry_out = executor->audit[slot];
    return true;
}

const char *appliance_production_recovery_executor_state_string(
    enum appliance_production_recovery_executor_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_PREPARED:
        return "PREPARED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_READY:
        return "READY";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_PENDING_CONFIRMATION:
        return "PENDING_CONFIRMATION";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_production_recovery_executor_status_string(
    enum appliance_production_recovery_executor_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DENIED:
        return "DENIED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION:
        return "PENDING_CONFIRMATION";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DRY_RUN:
        return "DRY_RUN";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLIED:
        return "APPLIED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_CANCELLED:
        return "CANCELLED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NO_PENDING:
        return "NO_PENDING";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_CONFIRMATION:
        return "BAD_CONFIRMATION";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNSUPPORTED_TARGET:
        return "UNSUPPORTED_TARGET";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OUT_OF_BOUNDS:
        return "OUT_OF_BOUNDS";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STORAGE_ERROR:
        return "STORAGE_ERROR";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLY_FAILED:
        return "APPLY_FAILED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}

const char *appliance_production_recovery_executor_operation_string(
    enum appliance_production_recovery_executor_operation operation)
{
    switch (operation) {
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_RECOVERY:
        return "RECOVERY";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_FACTORY_RESET:
        return "FACTORY_RESET";
    default:
        return NULL;
    }
}

const char *appliance_production_recovery_executor_audit_outcome_string(
    enum appliance_production_recovery_executor_audit_outcome outcome)
{
    switch (outcome) {
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_ALLOWED:
        return "ALLOWED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DENIED:
        return "DENIED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CANCELLED:
        return "CANCELLED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_DRY_RUN:
        return "DRY_RUN";
    case APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_APPLY_FAILED:
        return "APPLY_FAILED";
    default:
        return NULL;
    }
}
