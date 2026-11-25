#ifdef HFOC_APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_QEMU_TEST

#include "appliance_production_recovery_executor.h"

#include "appliance_production_boot.h"
#include "appliance_state_loader.h"
#include "data_region.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RECOVERY_EXECUTOR_QEMU_BLOCKS \
    APPLIANCE_STATE_LAYOUT_DEFAULT_CAPACITY_BLOCKS
#define RECOVERY_EXECUTOR_QEMU_BUFFER_SIZE \
    (RECOVERY_EXECUTOR_QEMU_BLOCKS * APPLIANCE_STATE_BLOCK_SIZE)

static const unsigned char recovery_executor_qemu_magic[
    DATA_REGION_MAGIC_SIZE] = DATA_REGION_MAGIC_BYTES;

static unsigned char recovery_executor_qemu_before[
    RECOVERY_EXECUTOR_QEMU_BUFFER_SIZE];
static unsigned char recovery_executor_qemu_after[
    RECOVERY_EXECUTOR_QEMU_BUFFER_SIZE];
static struct block_device recovery_executor_qemu_partition;
static struct appliance_production_auth recovery_executor_qemu_auth;
static struct appliance_production_management recovery_executor_qemu_management;
static struct appliance_production_management_plane
    recovery_executor_qemu_management_plane;
static struct appliance_factory_reset_gate recovery_executor_qemu_reset_gate;
static struct appliance_recovery_mode_gate recovery_executor_qemu_recovery_gate;
static struct appliance_production_recovery_executor recovery_executor_qemu;

static void recovery_executor_qemu_failure(const char *message)
    __attribute__((noreturn));

static void recovery_executor_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write("[PRODUCTION-RECOVERY] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void recovery_executor_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        recovery_executor_qemu_failure(message);
    }
}

static void recovery_executor_qemu_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint64_t recovery_executor_qemu_load_le64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static bool recovery_executor_qemu_is_zero(
    const unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void recovery_executor_qemu_read_all(unsigned char *buffer)
{
    uint64_t index;

    for (index = 0U; index < recovery_executor_qemu_partition.block_count;
         ++index) {
        if (block_device_read(
                &recovery_executor_qemu_partition,
                index * APPLIANCE_STATE_BLOCK_SIZE,
                buffer + (size_t)index * APPLIANCE_STATE_BLOCK_SIZE,
                APPLIANCE_STATE_BLOCK_SIZE) != BLOCK_DEVICE_OK) {
            recovery_executor_qemu_failure("state partition readback");
        }
    }
}

static const unsigned char *recovery_executor_qemu_block(uint64_t block)
{
    return recovery_executor_qemu_after +
           (size_t)block * APPLIANCE_STATE_BLOCK_SIZE;
}

static bool recovery_executor_qemu_store_is_zero(uint64_t first, uint64_t count)
{
    uint64_t block;

    for (block = first; block < first + count; ++block) {
        if (!recovery_executor_qemu_is_zero(
                recovery_executor_qemu_block(block),
                APPLIANCE_STATE_BLOCK_SIZE)) {
            return false;
        }
    }
    return true;
}

static bool recovery_executor_qemu_has_region_header(
    uint64_t block,
    uint64_t store_blocks)
{
    const unsigned char *bytes = recovery_executor_qemu_block(block);

    return memcmp(bytes,
                  recovery_executor_qemu_magic,
                  sizeof(recovery_executor_qemu_magic)) == 0 &&
           bytes[8U] == DATA_REGION_FORMAT_VERSION &&
           recovery_executor_qemu_load_le64(bytes + 16U) == 1U &&
           recovery_executor_qemu_load_le64(bytes + 24U) == store_blocks - 1U;
}

static bool recovery_executor_qemu_body_is_zero(
    uint64_t first,
    uint64_t count)
{
    return recovery_executor_qemu_store_is_zero(first + 1U, count - 1U);
}

static void recovery_executor_qemu_make_credential(
    struct appliance_admin_auth_credential *credential)
{
    size_t index;

    memset(credential, 0, sizeof(*credential));
    credential->version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    for (index = 0U; index < sizeof(credential->verifier); ++index) {
        credential->verifier[index] = (uint8_t)(0x40U + (uint8_t)index);
    }
}

static void recovery_executor_qemu_make_proof(
    struct appliance_admin_auth_proof *proof)
{
    size_t index;

    memset(proof, 0, sizeof(*proof));
    proof->version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    for (index = 0U; index < sizeof(proof->proof); ++index) {
        proof->proof[index] = (uint8_t)(0x40U + (uint8_t)index);
    }
}

static void recovery_executor_qemu_make_management_evidence(
    struct appliance_production_management_evidence *evidence)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->version = APPLIANCE_PRODUCTION_MANAGEMENT_EVIDENCE_VERSION;
    evidence->published = true;
    evidence->source_generation = 1U;
    evidence->system_identity_present = true;
    evidence->configuration_ready = true;
    evidence->network_required = true;
    evidence->network_active = true;
    evidence->network_health_ok = true;
    evidence->production_services_ready = true;
    evidence->https_management_ready = true;
    evidence->authorization_available = true;
}

static void recovery_executor_qemu_make_executor_evidence(
    struct appliance_production_recovery_executor_evidence *evidence)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->version =
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_EVIDENCE_VERSION;
    evidence->boot_foundation_verified = true;
    evidence->storage_loader_ready = true;
    evidence->installer_recovery_ready = true;
    evidence->recovery_trust_active = false;
    evidence->recovery.version = APPLIANCE_RECOVERY_MODE_VERSION;
    evidence->recovery.domain =
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION;
    evidence->recovery.config_recovery =
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE;
}

static bool recovery_executor_qemu_verify_plan(
    const struct appliance_state_layout *layout)
{
    uint32_t index;

    for (index = 0U; index < layout->entry_count; ++index) {
        const struct appliance_state_layout_entry *entry =
            &layout->entries[index];

        switch (entry->namespace_id) {
        case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG:
            if (!recovery_executor_qemu_has_region_header(
                    entry->block_offset, entry->block_count) ||
                !recovery_executor_qemu_body_is_zero(
                    entry->block_offset, entry->block_count)) {
                return false;
            }
            break;
        case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA:
            if (memcmp(recovery_executor_qemu_before +
                           (size_t)entry->block_offset *
                               APPLIANCE_STATE_BLOCK_SIZE,
                       recovery_executor_qemu_after +
                           (size_t)entry->block_offset *
                               APPLIANCE_STATE_BLOCK_SIZE,
                       (size_t)entry->block_count *
                           APPLIANCE_STATE_BLOCK_SIZE) != 0) {
                return false;
            }
            break;
        default:
            if (!recovery_executor_qemu_store_is_zero(
                    entry->block_offset, entry->block_count)) {
                return false;
            }
            break;
        }
    }
    return true;
}

void appliance_production_recovery_executor_qemu_test_init(
    const struct appliance_state_loader *loader,
    enum appliance_production_boot_readiness readiness)
{
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence management_evidence;
    struct appliance_production_recovery_executor_evidence evidence;
    struct appliance_production_recovery_executor_storage storage;
    const struct block_device *partition_view;
    const struct appliance_state_layout *layout;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint64_t confirmation_token;
    char output[
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    serial_write("[PRODUCTION-RECOVERY] proof init\n");

    if (loader == NULL ||
        readiness != APPLIANCE_PRODUCTION_BOOT_READINESS_READY ||
        !appliance_state_loader_ready(loader)) {
        /*
         * Honest proof: without a bound state partition the executor cannot
         * claim a storage transaction. The QEMU script attaches one.
         */
        serial_write("[PRODUCTION-RECOVERY] storage=UNAVAILABLE\n");
        return;
    }
    partition_view = appliance_state_loader_partition_device(loader);
    layout = appliance_state_loader_layout(loader);
    recovery_executor_qemu_require(
        partition_view != NULL && layout != NULL &&
            partition_view->block_count == RECOVERY_EXECUTOR_QEMU_BLOCKS,
        "state partition binding");

    recovery_executor_qemu_partition = *partition_view;
    memset(&storage, 0, sizeof(storage));
    storage.partition = &recovery_executor_qemu_partition;
    storage.layout = layout;
    storage.max_blocks = 0U;

    recovery_executor_qemu_require(
        appliance_production_auth_init(&recovery_executor_qemu_auth) ==
                APPLIANCE_PRODUCTION_AUTH_OK &&
            appliance_production_management_init(
                &recovery_executor_qemu_management) ==
                APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
            appliance_production_management_plane_init(
                &recovery_executor_qemu_management_plane,
                &recovery_executor_qemu_management,
                &recovery_executor_qemu_auth) ==
                APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK &&
            appliance_factory_reset_init(
                &recovery_executor_qemu_reset_gate) ==
                APPLIANCE_FACTORY_RESET_OK &&
            appliance_recovery_mode_init(
                &recovery_executor_qemu_recovery_gate) ==
                APPLIANCE_RECOVERY_MODE_OK &&
            appliance_production_recovery_executor_init(
                &recovery_executor_qemu,
                &recovery_executor_qemu_auth,
                &recovery_executor_qemu_management_plane,
                &recovery_executor_qemu_reset_gate,
                &recovery_executor_qemu_recovery_gate,
                &storage) == APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK,
        "executor composition");

    recovery_executor_qemu_make_credential(&credential);
    recovery_executor_qemu_require(
        appliance_production_auth_publish_credential(
            &recovery_executor_qemu_auth, &credential) ==
            APPLIANCE_PRODUCTION_AUTH_OK,
        "administrator credential");
    recovery_executor_qemu_make_proof(&proof);
    memset(token, 0x90, sizeof(token));
    recovery_executor_qemu_require(
        appliance_admin_auth_authenticate_local(
            &recovery_executor_qemu_auth.admin,
            &proof,
            token,
            10U,
            100U,
            &session_id) == APPLIANCE_ADMIN_AUTH_OK &&
            session_id == 1U,
        "administrator session");
    memcpy(header,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
    memcpy(header + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
           token,
           APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);

    recovery_executor_qemu_make_management_evidence(&management_evidence);
    recovery_executor_qemu_require(
        appliance_production_management_publish(
            &recovery_executor_qemu_management,
            &management_evidence) == APPLIANCE_PRODUCTION_MANAGEMENT_OK,
        "management evidence");
    recovery_executor_qemu_make_executor_evidence(&evidence);
    recovery_executor_qemu_require(
        appliance_production_recovery_executor_publish_evidence(
            &recovery_executor_qemu, &evidence) ==
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK &&
            appliance_production_recovery_executor_refresh(
                &recovery_executor_qemu) ==
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK &&
            appliance_production_recovery_executor_state(
                &recovery_executor_qemu) ==
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_READY &&
            appliance_production_recovery_executor_ready(
                &recovery_executor_qemu),
        "executor ready");
    serial_write("[PRODUCTION-RECOVERY] executor=READY\n");

    recovery_executor_qemu_read_all(recovery_executor_qemu_before);

    output_length = 0U;
    recovery_executor_qemu_require(
        appliance_production_recovery_executor_request(
            &recovery_executor_qemu,
            header,
            sizeof(header),
            APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
            (const uint8_t *)"/api/v1/system/factory-reset",
            (uint32_t)strlen("/api/v1/system/factory-reset"),
            true,
            0U,
            false,
            20U,
            output,
            sizeof(output),
            &output_length) ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DRY_RUN,
        "factory reset dry run");
    recovery_executor_qemu_read_all(recovery_executor_qemu_after);
    recovery_executor_qemu_require(
        memcmp(recovery_executor_qemu_before,
               recovery_executor_qemu_after,
               sizeof(recovery_executor_qemu_before)) == 0 &&
            !appliance_production_recovery_executor_has_pending(
                &recovery_executor_qemu),
        "dry run wrote nothing");
    serial_write("[PRODUCTION-RECOVERY] dry_run=OK writes=0\n");

    output_length = 0U;
    recovery_executor_qemu_require(
        appliance_production_recovery_executor_request(
            &recovery_executor_qemu,
            header,
            sizeof(header),
            APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
            (const uint8_t *)"/api/v1/system/factory-reset",
            (uint32_t)strlen("/api/v1/system/factory-reset"),
            false,
            0U,
            false,
            21U,
            output,
            sizeof(output),
            &output_length) ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION,
        "factory reset staging");
    confirmation_token =
        appliance_production_recovery_executor_pending_token(
            &recovery_executor_qemu);
    recovery_executor_qemu_require(
        confirmation_token != 0U &&
            appliance_production_recovery_executor_pending_stores(
                &recovery_executor_qemu) == 7U &&
            appliance_production_recovery_executor_pending_blocks(
                &recovery_executor_qemu) == 448U,
        "staged bounded plan");

    output_length = 0U;
    recovery_executor_qemu_require(
        appliance_production_recovery_executor_request(
            &recovery_executor_qemu,
            header,
            sizeof(header),
            APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
            (const uint8_t *)"/api/v1/system/factory-reset",
            (uint32_t)strlen("/api/v1/system/factory-reset"),
            false,
            confirmation_token + 1U,
            false,
            22U,
            output,
            sizeof(output),
            &output_length) ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_CONFIRMATION,
        "wrong confirmation token");
    recovery_executor_qemu_read_all(recovery_executor_qemu_after);
    recovery_executor_qemu_require(
        memcmp(recovery_executor_qemu_before,
               recovery_executor_qemu_after,
               sizeof(recovery_executor_qemu_before)) == 0,
        "wrong token wrote nothing");
    serial_write("[PRODUCTION-RECOVERY] wrong_token=REJECTED\n");

    output_length = 0U;
    recovery_executor_qemu_require(
        appliance_production_recovery_executor_request(
            &recovery_executor_qemu,
            header,
            sizeof(header),
            APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
            (const uint8_t *)"/api/v1/system/factory-reset",
            (uint32_t)strlen("/api/v1/system/factory-reset"),
            false,
            confirmation_token,
            false,
            23U,
            output,
            sizeof(output),
            &output_length) ==
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLIED &&
            appliance_production_recovery_executor_last_stores(
                &recovery_executor_qemu) == 7U &&
            appliance_production_recovery_executor_last_written_blocks(
                &recovery_executor_qemu) == 448U &&
            appliance_production_recovery_executor_last_preserved(
                &recovery_executor_qemu) == 1U,
        "factory reset apply");
    serial_write("[PRODUCTION-RECOVERY] apply=APPLIED stores=7 blocks=448 preserved=1\n");

    recovery_executor_qemu_read_all(recovery_executor_qemu_after);
    recovery_executor_qemu_require(
        recovery_executor_qemu_verify_plan(layout),
        "factory reset store map");
    serial_write("[PRODUCTION-RECOVERY] recovery_metadata=PRESERVED\n");
    serial_write("[PRODUCTION-RECOVERY] store_map=VERIFIED\n");
    serial_write("[PRODUCTION-RECOVERY] proof OK\n");
}

#else
typedef int appliance_production_recovery_executor_qemu_translation_unit_guard;
#endif
