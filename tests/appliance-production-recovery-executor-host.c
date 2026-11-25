#include "appliance_production_recovery_executor.h"

#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_PARTITION_BLOCKS APPLIANCE_STATE_LAYOUT_DEFAULT_CAPACITY_BLOCKS
#define TEST_DISK_BLOCKS 512U
#define TEST_DISK_CAPACITY (TEST_BLOCK_SIZE * TEST_DISK_BLOCKS)
#define TEST_OUTSIDE_FILL 0xA5U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    return false;
}

struct fixture_device {
    unsigned char storage[TEST_DISK_CAPACITY];
    bool fail_write;
    size_t read_calls;
    size_t write_calls;
    size_t flush_calls;
};

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_device *fixture = device->context;

    if (offset + length > sizeof(fixture->storage)) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    ++fixture->read_calls;
    memcpy(buffer, fixture->storage + offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_device *fixture = device->context;

    if (offset + length > sizeof(fixture->storage)) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    if (fixture->fail_write) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    ++fixture->write_calls;
    memcpy(fixture->storage + offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(struct block_device *device)
{
    struct fixture_device *fixture = device->context;

    ++fixture->flush_calls;
    return BLOCK_DEVICE_OK;
}

static struct block_device make_device(struct fixture_device *fixture)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_DISK_BLOCKS;
    device.context = fixture;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

struct test_env {
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_state_layout layout;
    struct appliance_production_recovery_executor_storage storage;
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct appliance_factory_reset_gate reset_gate;
    struct appliance_recovery_mode_gate recovery_gate;
    struct appliance_production_recovery_executor executor;
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
};

static struct test_env env;
static char output[APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OUTPUT_MAX + 1U];
static size_t output_length;

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint64_t load_le64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

/*
 * Marks every state store with a data-region header and a distinctive record
 * body so a clear or a rewrite is observable, and fills the area outside the
 * HFOC partition with a sentinel that must never change.
 */
static void init_fixture(void)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;
    uint32_t index;

    memset(&env, 0, sizeof(env));
    memset(env.fixture.storage, TEST_OUTSIDE_FILL,
           sizeof(env.fixture.storage));
    for (index = 0U; index < TEST_PARTITION_BLOCKS; ++index) {
        unsigned char *block =
            env.fixture.storage + (size_t)index * TEST_BLOCK_SIZE;

        memset(block, 0, TEST_BLOCK_SIZE);
        if (index % APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT == 0U) {
            memcpy(block, magic, sizeof(magic));
            block[8U] = DATA_REGION_FORMAT_VERSION;
            store_le64(block + 16U, 1U);
            store_le64(
                block + 24U,
                APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT - 1U);
        } else {
            memset(block, (int)(0x10U + index % 0x40U),
                   TEST_BLOCK_SIZE);
        }
    }
    env.device = make_device(&env.fixture);
    if (appliance_state_layout_default(&env.layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        fprintf(stderr, "FAIL: state layout bootstrap\n");
        ++failures;
    }
    env.storage.partition = &env.device;
    env.storage.layout = &env.layout;
    env.storage.max_blocks = 0U;
}

static void make_credential(
    struct appliance_admin_auth_credential *credential,
    uint8_t seed)
{
    size_t index;

    memset(credential, 0, sizeof(*credential));
    credential->version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    for (index = 0U; index < sizeof(credential->verifier); ++index) {
        credential->verifier[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static void make_token(
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint8_t seed)
{
    size_t index;

    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH; ++index) {
        token[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static void make_proof(
    struct appliance_admin_auth_proof *proof,
    uint8_t seed)
{
    size_t index;

    memset(proof, 0, sizeof(*proof));
    proof->version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    for (index = 0U; index < sizeof(proof->proof); ++index) {
        proof->proof[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static void make_management_evidence(
    struct appliance_production_management_evidence *evidence,
    bool degraded)
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
    evidence->degraded_requested = degraded;
}

static void make_recovery_evidence(
    struct appliance_recovery_mode_evidence *evidence,
    uint32_t domain,
    enum appliance_config_integrity_recovery config_recovery)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->version = APPLIANCE_RECOVERY_MODE_VERSION;
    evidence->domain = domain;
    evidence->config_recovery = config_recovery;
}

static void make_executor_evidence(
    struct appliance_production_recovery_executor_evidence *evidence,
    bool boot_foundation_verified,
    bool storage_loader_ready,
    bool installer_recovery_ready,
    bool recovery_trust_active,
    uint32_t domain,
    enum appliance_config_integrity_recovery config_recovery)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->version =
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_EVIDENCE_VERSION;
    evidence->boot_foundation_verified = boot_foundation_verified;
    evidence->storage_loader_ready = storage_loader_ready;
    evidence->installer_recovery_ready = installer_recovery_ready;
    evidence->recovery_trust_active = recovery_trust_active;
    make_recovery_evidence(
        &evidence->recovery, domain, config_recovery);
}

static bool initialize_ready_executor(void)
{
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence management_evidence;
    struct appliance_production_recovery_executor_evidence evidence;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;

    if (appliance_production_auth_init(&env.auth) !=
            APPLIANCE_PRODUCTION_AUTH_OK ||
        appliance_production_management_init(&env.management) !=
            APPLIANCE_PRODUCTION_MANAGEMENT_OK ||
        appliance_production_management_plane_init(
            &env.plane, &env.management, &env.auth) !=
            APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK ||
        appliance_factory_reset_init(&env.reset_gate) !=
            APPLIANCE_FACTORY_RESET_OK ||
        appliance_recovery_mode_init(&env.recovery_gate) !=
            APPLIANCE_RECOVERY_MODE_OK ||
        appliance_production_recovery_executor_init(
            &env.executor, &env.auth, &env.plane, &env.reset_gate,
            &env.recovery_gate, &env.storage) !=
            APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
        return false;
    }
    make_credential(&credential, 0x40U);
    if (appliance_production_auth_publish_credential(
            &env.auth, &credential) != APPLIANCE_PRODUCTION_AUTH_OK) {
        return false;
    }
    make_proof(&proof, 0x40U);
    make_token(token, 0x90U);
    if (appliance_admin_auth_authenticate_local(
            &env.auth.admin, &proof, token, 10U, 100U, &session_id) !=
            APPLIANCE_ADMIN_AUTH_OK ||
        session_id != 1U) {
        return false;
    }
    memcpy(env.header, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
    memcpy(env.header + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
           token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
    make_management_evidence(&management_evidence, false);
    if (appliance_production_management_publish(
            &env.management, &management_evidence) !=
        APPLIANCE_PRODUCTION_MANAGEMENT_OK) {
        return false;
    }
    make_executor_evidence(
        &evidence, true, true, true, false,
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE);
    if (appliance_production_recovery_executor_publish_evidence(
            &env.executor, &evidence) !=
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
        return false;
    }
    if (appliance_production_recovery_executor_refresh(&env.executor) !=
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK) {
        return false;
    }
    return true;
}

static enum appliance_production_recovery_executor_status request(
    bool dry_run,
    uint64_t confirmation_token,
    bool cancel,
    uint64_t now_tick,
    const char *target)
{
    output_length = 0U;
    memset(output, 0, sizeof(output));
    return appliance_production_recovery_executor_request(
        &env.executor, env.header, sizeof(env.header),
        APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
        (const uint8_t *)target, (uint32_t)strlen(target),
        dry_run, confirmation_token, cancel, now_tick,
        output, sizeof(output), &output_length);
}

static bool store_is_cleared(uint32_t store_index)
{
    uint64_t first =
        (uint64_t)store_index * APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT;
    uint64_t count = APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT;
    uint64_t block;

    for (block = first; block < first + count; ++block) {
        const unsigned char *bytes =
            env.fixture.storage + (size_t)block * TEST_BLOCK_SIZE;
        size_t index;

        for (index = 0U; index < TEST_BLOCK_SIZE; ++index) {
            if (bytes[index] != 0U) {
                return false;
            }
        }
    }
    return true;
}

static bool store_has_region_header(uint32_t store_index)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;
    const unsigned char *block =
        env.fixture.storage +
        (size_t)store_index * APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT *
            TEST_BLOCK_SIZE;

    return memcmp(block, magic, sizeof(magic)) == 0 &&
           block[8U] == DATA_REGION_FORMAT_VERSION &&
           load_le64(block + 16U) == 1U &&
           load_le64(block + 24U) ==
               APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT - 1U;
}

static bool store_body_is_zero(uint32_t store_index)
{
    uint64_t first =
        (uint64_t)store_index * APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT + 1U;
    uint64_t end =
        (uint64_t)(store_index + 1U) *
        APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT;
    uint64_t block;

    for (block = first; block < end; ++block) {
        const unsigned char *bytes =
            env.fixture.storage + (size_t)block * TEST_BLOCK_SIZE;
        size_t index;

        for (index = 0U; index < TEST_BLOCK_SIZE; ++index) {
            if (bytes[index] != 0U) {
                return false;
            }
        }
    }
    return true;
}

static bool outside_partition_untouched(void)
{
    const unsigned char *bytes =
        env.fixture.storage +
        (size_t)TEST_PARTITION_BLOCKS * TEST_BLOCK_SIZE;
    size_t length =
        ((size_t)TEST_DISK_BLOCKS - TEST_PARTITION_BLOCKS) * TEST_BLOCK_SIZE;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != TEST_OUTSIDE_FILL) {
            return false;
        }
    }
    return true;
}

static bool recovery_metadata_untouched(void)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;
    const unsigned char *store =
        env.fixture.storage +
        (size_t)6U * APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT *
            TEST_BLOCK_SIZE;

    return memcmp(store, magic, sizeof(magic)) == 0 &&
           store[TEST_BLOCK_SIZE] != 0U;
}

static void test_init_and_fail_closed(void)
{
    struct appliance_production_recovery_executor_storage bad_storage;

    init_fixture();

    CHECK(appliance_production_recovery_executor_init(
              NULL, &env.auth, &env.plane, &env.reset_gate,
              &env.recovery_gate, &env.storage) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL,
          "executor init rejects a null executor");
    CHECK(appliance_production_recovery_executor_init(
              &env.executor, NULL, &env.plane, &env.reset_gate,
              &env.recovery_gate, &env.storage) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL,
          "executor init rejects a null auth gate");
    CHECK(appliance_production_recovery_executor_init(
              &env.executor, &env.auth, NULL, &env.reset_gate,
              &env.recovery_gate, &env.storage) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL,
          "executor init rejects a null management plane");
    CHECK(appliance_production_recovery_executor_init(
              &env.executor, &env.auth, &env.plane, NULL,
              &env.recovery_gate, &env.storage) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL,
          "executor init rejects a null factory-reset gate");
    CHECK(appliance_production_recovery_executor_init(
              &env.executor, &env.auth, &env.plane, &env.reset_gate,
              NULL, &env.storage) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL,
          "executor init rejects a null recovery gate");
    CHECK(appliance_production_recovery_executor_init(
              &env.executor, &env.auth, &env.plane, &env.reset_gate,
              &env.recovery_gate, NULL) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL,
          "executor init rejects a null storage binding");
    bad_storage = env.storage;
    bad_storage.partition = NULL;
    CHECK(appliance_production_recovery_executor_init(
              &env.executor, &env.auth, &env.plane, &env.reset_gate,
              &env.recovery_gate, &bad_storage) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL,
          "executor init rejects a null partition");
    bad_storage = env.storage;
    bad_storage.layout = NULL;
    CHECK(appliance_production_recovery_executor_init(
              &env.executor, &env.auth, &env.plane, &env.reset_gate,
              &env.recovery_gate, &bad_storage) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL,
          "executor init rejects a null layout");
    bad_storage = env.storage;
    bad_storage.max_blocks = 0U;
    CHECK(appliance_production_recovery_executor_init(
              &env.executor, &env.auth, &env.plane, &env.reset_gate,
              &env.recovery_gate, &bad_storage) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK &&
              env.executor.storage.max_blocks ==
                  APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DEFAULT_MAX_BLOCKS,
          "a zero block bound selects the default bound");

    init_fixture();
    CHECK(appliance_production_recovery_executor_init(
              &env.executor, &env.auth, &env.plane, &env.reset_gate,
              &env.recovery_gate, &env.storage) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK,
          "executor binds auth, management, gates and storage");
    CHECK(appliance_production_recovery_executor_fail_closed(NULL) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NULL,
          "fail-closed rejects a null executor");
    CHECK(appliance_production_recovery_executor_ready(&env.executor) ==
              false,
          "executor is not ready before evidence and credential");
    CHECK(request(false, 0U, false, 1U, "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE,
          "unpublished credential fails the request closed");
    CHECK(appliance_production_recovery_executor_has_pending(
              &env.executor) == false,
          "unavailable request stages no plan");
}

static void test_dry_run_and_bounds(void)
{
    uint32_t writes_before;

    init_fixture();
    env.storage.max_blocks = 100U;
    CHECK(initialize_ready_executor(),
          "executor reaches an honest READY state with a tight bound");
    CHECK(appliance_production_recovery_executor_state(&env.executor) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_READY,
          "ready executor publishes READY");
    CHECK(appliance_production_recovery_executor_ready(&env.executor) ==
              true,
          "ready executor admits authorized requests");

    writes_before = (uint32_t)env.fixture.write_calls;
    CHECK(request(true, 0U, false, 21U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OUT_OF_BOUNDS,
          "dry run refuses a plan wider than the transaction bound");
    CHECK(request(false, 0U, false, 22U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OUT_OF_BOUNDS,
          "staging refuses a plan wider than the transaction bound");
    CHECK(env.fixture.write_calls == writes_before,
          "an out-of-bounds plan writes nothing");
    CHECK(appliance_production_recovery_executor_has_pending(&env.executor) ==
              false,
          "an out-of-bounds plan stages nothing");

    init_fixture();
    CHECK(initialize_ready_executor(), "executor reaches READY");
    writes_before = (uint32_t)env.fixture.write_calls;
    output_length = 0U;
    CHECK(request(true, 0U, false, 20U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DRY_RUN,
          "authorized factory-reset dry run returns DRY_RUN");
    CHECK(env.fixture.write_calls == writes_before,
          "dry run performs no storage write");
    CHECK(appliance_production_recovery_executor_has_pending(&env.executor) ==
              false,
          "dry run stages no pending plan");
    CHECK(appliance_production_recovery_executor_dry_run_count(&env.executor) ==
              1U,
          "dry run is counted");
    CHECK(strstr(output, "recovery=FACTORY_RESET") != NULL &&
              strstr(output, "result=DRY_RUN") != NULL &&
              strstr(output, "stores=7") != NULL &&
              strstr(output, "blocks=448") != NULL &&
              strstr(output, "preserved=1") != NULL &&
              strstr(output, "physical=NOT_TESTED") != NULL,
          "dry run publishes the bounded plan and the physical honesty line");
    CHECK(appliance_production_recovery_executor_last_stores(&env.executor) ==
              0U &&
              appliance_production_recovery_executor_pending_blocks(
                  &env.executor) == 0U,
          "dry run does not claim an applied transaction");
}

static void test_factory_reset_two_phase(void)
{
    uint64_t token;

    init_fixture();
    CHECK(initialize_ready_executor(), "executor reaches READY");

    CHECK(request(false, 0U, false, 30U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION,
          "authorized factory reset stages a bounded plan");
    CHECK(appliance_production_recovery_executor_has_pending(&env.executor) ==
              true,
          "staged factory reset is pending");
    CHECK(appliance_production_recovery_executor_pending_operation(
              &env.executor) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_FACTORY_RESET,
          "pending operation is the factory reset");
    CHECK(appliance_production_recovery_executor_pending_stores(
              &env.executor) == 7U &&
              appliance_production_recovery_executor_pending_blocks(
                  &env.executor) == 448U,
          "pending plan is bounded to seven stores and 448 blocks");
    token = appliance_production_recovery_executor_pending_token(&env.executor);
    CHECK(token != 0U, "staged plan issues a non-zero confirmation token");
    CHECK(appliance_production_recovery_executor_state(&env.executor) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_PENDING_CONFIRMATION,
          "pending plan publishes PENDING_CONFIRMATION");
    CHECK(env.fixture.write_calls == 0U,
          "staging performs no storage write");

    CHECK(request(false, token + 1U, false, 31U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_CONFIRMATION,
          "a wrong confirmation token is rejected");
    CHECK(env.fixture.write_calls == 0U,
          "a wrong confirmation token writes nothing");
    CHECK(appliance_production_recovery_executor_has_pending(&env.executor) ==
              true,
          "a wrong token keeps the pending plan");

    CHECK(request(false, token, true, 32U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_CANCELLED,
          "an authorized cancel discards the pending plan");
    CHECK(appliance_production_recovery_executor_has_pending(&env.executor) ==
              false,
          "cancelled plan is gone");
    CHECK(env.fixture.write_calls == 0U,
          "cancel writes nothing");
    CHECK(appliance_production_recovery_executor_cancelled_count(
              &env.executor) == 1U,
          "cancel is counted");

    CHECK(request(false, 0U, false, 40U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION,
          "factory reset stages again after cancel");
    token = appliance_production_recovery_executor_pending_token(&env.executor);
    CHECK(request(false, token, false,
                  40U + APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_CONFIRMATION_TICKS +
                      1U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_EXPIRED,
          "an elapsed confirmation window expires the plan");
    CHECK(appliance_production_recovery_executor_has_pending(&env.executor) ==
              false,
          "expired plan is discarded");
    CHECK(env.fixture.write_calls == 0U,
          "an expired plan writes nothing");
    CHECK(appliance_production_recovery_executor_expired_count(&env.executor) ==
              1U,
          "expiry is counted");

    CHECK(request(false, 0U, false, 50U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION,
          "factory reset stages after expiry");
    token = appliance_production_recovery_executor_pending_token(&env.executor);
    CHECK(request(false, token, false, 51U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLIED,
          "the exact confirmation token executes the plan");
    CHECK(appliance_production_recovery_executor_has_pending(&env.executor) ==
              false,
          "applied plan is cleared");
    CHECK(appliance_production_recovery_executor_applied_count(&env.executor) ==
              1U,
          "applied transaction is counted");
    CHECK(appliance_production_recovery_executor_last_stores(&env.executor) ==
              7U &&
              appliance_production_recovery_executor_last_written_blocks(
                  &env.executor) == 448U &&
              appliance_production_recovery_executor_last_preserved(
                  &env.executor) == 1U,
          "applied transaction publishes exact stores, blocks and preservation");

    CHECK(store_has_region_header(0U) && store_body_is_zero(0U),
          "system hostname store is rewritten to factory default");
    CHECK(store_has_region_header(1U) && store_body_is_zero(1U),
          "system network store is rewritten to factory default");
    CHECK(store_has_region_header(7U) && store_body_is_zero(7U),
          "ITEM-504: the route store is rewritten to factory default so a "
          "reset carries no operator route");
    CHECK(store_is_cleared(2U) && store_is_cleared(3U) &&
              store_is_cleared(4U) && store_is_cleared(5U),
          "service, certificate, secret and diagnostic stores are cleared");
    CHECK(recovery_metadata_untouched(),
          "recovery metadata foundation is preserved");
    CHECK(outside_partition_untouched(),
          "blocks outside the state partition are untouched");

    CHECK(request(false, 0U, false, 52U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION,
          "the executor remains usable after an applied transaction");
    CHECK(request(false,
                  appliance_production_recovery_executor_pending_token(
                      &env.executor),
                  true, 53U, "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_CANCELLED,
          "the follow-up plan cancels cleanly");

    CHECK(appliance_production_recovery_executor_audit_count(&env.executor) >=
              5U,
          "every attempt is audited");
    {
        struct appliance_production_recovery_executor_audit_entry entry;
        bool saw_applied = false;
        bool saw_cancelled = false;
        bool saw_expired = false;
        uint32_t index;

        for (index = 0U;
             index <
             appliance_production_recovery_executor_audit_count(&env.executor);
             ++index) {
            if (!appliance_production_recovery_executor_audit_entry(
                    &env.executor, index, &entry)) {
                continue;
            }
            if (entry.outcome ==
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_ALLOWED) {
                saw_applied = true;
            }
            if (entry.outcome ==
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_CANCELLED) {
                saw_cancelled = true;
            }
            if (entry.outcome ==
                APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_EXPIRED) {
                saw_expired = true;
            }
        }
        CHECK(saw_applied && saw_cancelled && saw_expired,
              "audit records the applied, cancelled and expired outcomes");
    }
}

static void test_storage_failure_is_honest(void)
{
    uint64_t token;

    init_fixture();
    CHECK(initialize_ready_executor(), "executor reaches READY");
    CHECK(request(false, 0U, false, 60U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION,
          "factory reset stages before a storage fault");
    token = appliance_production_recovery_executor_pending_token(&env.executor);
    env.fixture.fail_write = true;
    CHECK(request(false, token, false, 61U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STORAGE_ERROR,
          "a storage fault reports STORAGE_ERROR instead of success");
    CHECK(appliance_production_recovery_executor_failed_count(&env.executor) ==
              1U,
          "failed transaction is counted");
    CHECK(appliance_production_recovery_executor_last_written_blocks(
              &env.executor) == 0U,
          "a fault before the first block reports zero written blocks");
    CHECK(appliance_production_recovery_executor_has_pending(&env.executor) ==
              false,
          "a failed transaction leaves no pending plan");
    env.fixture.fail_write = false;
    CHECK(appliance_production_recovery_executor_ready(&env.executor) == true,
          "the executor stays bound after a storage fault");
}

static void test_recovery_operation(void)
{
    struct appliance_production_recovery_executor_evidence evidence;

    init_fixture();
    CHECK(initialize_ready_executor(), "executor reaches READY");

    make_executor_evidence(
        &evidence, true, true, true, false,
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_MISSING);
    CHECK(appliance_production_recovery_executor_publish_evidence(
              &env.executor, &evidence) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK,
          "safe-default recovery evidence publishes");
    CHECK(request(true, 0U, false, 70U,
                  "/api/v1/system/recovery") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DRY_RUN,
          "safe-default recovery dry run publishes a bounded plan");
    CHECK(strstr(output, "recovery=RECOVERY") != NULL &&
              strstr(output, "action=SAFE_DEFAULT") != NULL &&
              strstr(output, "stores=3") != NULL &&
              strstr(output, "blocks=192") != NULL &&
              strstr(output, "preserved=5") != NULL,
          "safe-default recovery plan rewrites only the system configuration");
    CHECK(request(false, 0U, false, 71U,
                  "/api/v1/system/recovery") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION,
          "safe-default recovery stages");
    CHECK(request(false,
                  appliance_production_recovery_executor_pending_token(
                      &env.executor),
                  false, 72U, "/api/v1/system/recovery") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLIED,
          "safe-default recovery applies its bounded transaction");
    CHECK(store_has_region_header(0U) && store_body_is_zero(0U) &&
              store_has_region_header(1U) && store_body_is_zero(1U),
          "safe-default recovery rewrites both system configuration stores");
    CHECK(store_is_cleared(2U) == false,
          "safe-default recovery leaves unrelated stores untouched");

    init_fixture();
    CHECK(initialize_ready_executor(), "executor reaches READY again");
    make_executor_evidence(
        &evidence, true, true, true, false,
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR);
    CHECK(appliance_production_recovery_executor_publish_evidence(
              &env.executor, &evidence) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK,
          "read-only recovery evidence publishes");
    CHECK(request(true, 0U, false, 80U,
                  "/api/v1/system/recovery") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DRY_RUN,
          "read-only recovery dry run publishes a plan");
    CHECK(strstr(output, "action=READ_ONLY") != NULL &&
              strstr(output, "stores=0") != NULL &&
              strstr(output, "blocks=0") != NULL &&
              strstr(output, "preserved=6") != NULL,
          "read-only recovery plans no storage write");
    CHECK(request(false, 0U, false, 81U,
                  "/api/v1/system/recovery") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_PENDING_CONFIRMATION,
          "read-only recovery stages an empty plan");
    CHECK(env.fixture.write_calls == 0U,
          "read-only recovery never writes at staging time");
    CHECK(request(false,
                  appliance_production_recovery_executor_pending_token(
                      &env.executor),
                  false, 82U, "/api/v1/system/recovery") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLIED,
          "read-only recovery applies without a storage write");
    CHECK(env.fixture.write_calls == 0U,
          "read-only recovery performs no storage write at all");
    CHECK(appliance_production_recovery_executor_last_written_blocks(
              &env.executor) == 0U &&
              appliance_production_recovery_executor_last_preserved(
                  &env.executor) == 6U,
          "read-only recovery preserves every store");

    init_fixture();
    CHECK(initialize_ready_executor(), "executor reaches READY a third time");
    make_executor_evidence(
        &evidence, true, true, true, false,
        APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE);
    evidence.recovery.update_verified = true;
    evidence.recovery.pending_update = true;
    evidence.recovery.first_boot_health_failed = true;
    CHECK(appliance_production_recovery_executor_publish_evidence(
              &env.executor, &evidence) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK,
          "automatic rollback evidence publishes");
    CHECK(request(true, 0U, false, 90U,
                  "/api/v1/system/recovery") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNSUPPORTED_TARGET,
          "an automatic rollback is refused instead of faked");
    CHECK(env.fixture.write_calls == 0U,
          "an unsupported recovery action writes nothing");
}

static void test_evidence_and_denials(void)
{
    struct appliance_production_recovery_executor_evidence evidence;
    struct appliance_production_recovery_executor_evidence bad_evidence;
    uint8_t anonymous_target[] = "/api/v1/system/factory-reset";

    init_fixture();
    CHECK(initialize_ready_executor(), "executor reaches READY");

    make_executor_evidence(
        &evidence, true, true, false, false,
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE);
    CHECK(appliance_production_recovery_executor_publish_evidence(
              &env.executor, &evidence) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK,
          "evidence without installer recovery publishes");
    CHECK(appliance_production_recovery_executor_state(&env.executor) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_DEGRADED,
          "missing installer recovery degrades readiness honestly");
    CHECK(request(false, 0U, false, 100U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE,
          "factory reset fails closed without installer recovery evidence");

    make_executor_evidence(
        &evidence, true, true, true, false,
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE);
    CHECK(appliance_production_recovery_executor_publish_evidence(
              &env.executor, &evidence) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK,
          "complete evidence republishes");
    CHECK(appliance_production_recovery_executor_refresh(&env.executor) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK &&
              appliance_production_recovery_executor_state(&env.executor) ==
                  APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_READY,
          "readiness returns to READY");

    output_length = 0U;
    memset(output, 0, sizeof(output));
    CHECK(appliance_production_recovery_executor_request(
              &env.executor, NULL, 0U,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              anonymous_target, sizeof(anonymous_target) - 1U,
              false, 0U, false, 101U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_DENIED,
          "an anonymous recovery request is denied");
    CHECK(appliance_production_recovery_executor_denied_count(&env.executor) ==
              1U,
          "anonymous denial is counted");
    CHECK(env.fixture.write_calls == 0U,
          "a denied request writes nothing");

    CHECK(request(false, 0U, false, 102U, "/api/v1/network") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNSUPPORTED_TARGET,
          "a non-recovery mutation target is refused");
    CHECK(request(false, 0U, false, 103U, "/api/v1/system/update") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNSUPPORTED_TARGET,
          "an update mutation target is refused");
    CHECK(request(false, 0U, true, 104U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_NO_PENDING,
          "cancel without a pending plan is refused");

    env.fixture.fail_write = false;
    bad_evidence = evidence;
    bad_evidence.version =
        APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_EVIDENCE_VERSION + 1U;
    CHECK(appliance_production_recovery_executor_publish_evidence(
              &env.executor, &bad_evidence) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_VERSION,
          "evidence with an unknown version is rejected");

    make_executor_evidence(
        &evidence, true, true, true, true,
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE);
    CHECK(appliance_production_recovery_executor_publish_evidence(
              &env.executor, &evidence) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK,
          "trust-active evidence publishes");
    CHECK(appliance_production_recovery_executor_state(&env.executor) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_DEGRADED,
          "active recovery trust blocks execution readiness");
    CHECK(request(false, 0U, false, 105U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE,
          "active recovery trust fails the reset closed");
}

static void test_render_and_stop(void)
{
    init_fixture();
    CHECK(initialize_ready_executor(), "executor reaches READY");

    output_length = 0U;
    memset(output, 0, sizeof(output));
    CHECK(appliance_production_recovery_executor_render(
              &env.executor, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK,
          "executor renders a canonical snapshot");
    CHECK(strcmp(output,
                 "production_recovery v=1 state=READY operation=NONE "
                 "result=OK stores=0 blocks=0 preserved=0 applied=0 "
                 "denied=0 failed=0 physical=NOT_TESTED") == 0,
          "canonical snapshot is exact");

    CHECK(strcmp(appliance_production_recovery_executor_state_string(
                     APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_READY),
                 "READY") == 0 &&
              appliance_production_recovery_executor_state_string(
                  (enum appliance_production_recovery_executor_state)99) ==
                  NULL,
          "state strings are exact");
    CHECK(strcmp(appliance_production_recovery_executor_status_string(
                     APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_APPLIED),
                 "APPLIED") == 0 &&
              strcmp(appliance_production_recovery_executor_operation_string(
                         APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OPERATION_FACTORY_RESET),
                     "FACTORY_RESET") == 0 &&
              strcmp(appliance_production_recovery_executor_audit_outcome_string(
                         APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_AUDIT_EXPIRED),
                     "EXPIRED") == 0,
          "status, operation and audit strings are exact");

    CHECK(appliance_production_recovery_executor_stop(&env.executor) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_OK,
          "executor stops");
    CHECK(request(false, 0U, false, 110U,
                  "/api/v1/system/factory-reset") ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_BAD_STATE,
          "a stopped executor rejects further requests");
    CHECK(appliance_production_recovery_executor_fail_closed(&env.executor) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_UNAVAILABLE,
          "fail-closed reports UNAVAILABLE");
    CHECK(appliance_production_recovery_executor_state(&env.executor) ==
              APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_STATE_FAILED,
          "fail-closed publishes FAILED");
}

int main(void)
{
    test_init_and_fail_closed();
    test_dry_run_and_bounds();
    test_factory_reset_two_phase();
    test_storage_failure_is_honest();
    test_recovery_operation();
    test_evidence_and_denials();
    test_render_and_stop();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance production recovery executor host tests: "
                "%d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance production recovery executor host tests: ALL OK\n");
    return 0;
}

#endif
