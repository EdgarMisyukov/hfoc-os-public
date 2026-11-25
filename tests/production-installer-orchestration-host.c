#include "production_installer.h"
#include "production_installer_esp.h"
#include "production_installer_efi.h"
#include "production_installer_state.h"

#include "appliance_gpt_discovery.h"
#include "appliance_persistent_config.h"
#include "appliance_persistent_network_config.h"
#include "appliance_persistent_secret_config.h"
#include "appliance_state_block_device.h"
#include "appliance_state_layout.h"
#include "appliance_state_loader.h"
#include "efi_boot_variables.h"
#include "ipv4_config.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PRODUCTION_INSTALLER_ORCHESTRATION_HOST_TEST

#define FIXTURE_BLOCK_SIZE 512U
#define FIXTURE_BLOCK_COUNT 16384U
#define FIXTURE_VARIABLE_CAPACITY 32U
#define FIXTURE_DATA_MAX 512U

struct fixture_variable {
    bool present;
    struct efi_boot_variable_key key;
    size_t length;
    unsigned char data[FIXTURE_DATA_MAX];
};

struct variable_fixture {
    struct fixture_variable variables[FIXTURE_VARIABLE_CAPACITY];
    size_t variable_count;
    bool fail_read;
    bool fail_next_write;
    size_t write_calls;
};

static const unsigned char test_global_guid[16U] = {
    0x61U, 0xDFU, 0xE4U, 0x8BU, 0xCAU, 0x93U, 0xD2U, 0x11U,
    0xAAU, 0x0DU, 0x00U, 0xE0U, 0x98U, 0x03U, 0x2BU, 0x8CU
};
static const unsigned char test_device_path[] = {
    0x01U, 0x01U, 0x04U, 0x00U,
    0x7FU, 0xFFU, 0x04U, 0x00U
};

static unsigned char fixture_storage[
    FIXTURE_BLOCK_COUNT * FIXTURE_BLOCK_SIZE];
static size_t fixture_write_calls;
static int failures;
static struct variable_fixture variable_store;

static const unsigned char boot_payload[16U] = {
    0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
    0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU
};
static const unsigned char limine_config[] =
    "timeout: 3\n";
static const unsigned char kernel_payload[32U] = {
    0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
    0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU,
    0x20U, 0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U,
    0x28U, 0x29U, 0x2AU, 0x2BU, 0x2CU, 0x2DU, 0x2EU, 0x2FU
};
static const char disk_identity[] = "test-nvme-disk";

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, fixture_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    ++fixture_write_calls;
    if (offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(fixture_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static char fixture_hex_digit(unsigned int value)
{
    static const char digits[] = "0123456789ABCDEF";

    return digits[value & 0x0FU];
}

static void fixture_boot_key(
    uint16_t number,
    struct efi_boot_variable_key *key)
{
    memset(key, 0, sizeof(*key));
    memcpy(key->guid, test_global_guid,
           EFI_BOOT_VARIABLE_GUID_LENGTH);
    key->name[0U] = 'B';
    key->name[1U] = 'o';
    key->name[2U] = 'o';
    key->name[3U] = 't';
    key->name[4U] = fixture_hex_digit(
        (unsigned int)(number >> 12U));
    key->name[5U] = fixture_hex_digit(
        (unsigned int)((number >> 8U) & 0x0FU));
    key->name[6U] = fixture_hex_digit(
        (unsigned int)((number >> 4U) & 0x0FU));
    key->name[7U] = fixture_hex_digit(
        (unsigned int)(number & 0x0FU));
}

static void fixture_order_key(struct efi_boot_variable_key *key)
{
    memset(key, 0, sizeof(*key));
    memcpy(key->guid, test_global_guid,
           EFI_BOOT_VARIABLE_GUID_LENGTH);
    strcpy(key->name, "BootOrder");
}

static bool fixture_key_equal(
    const struct efi_boot_variable_key *left,
    const struct efi_boot_variable_key *right)
{
    return memcmp(left->guid, right->guid,
                  EFI_BOOT_VARIABLE_GUID_LENGTH) == 0 &&
           strcmp(left->name, right->name) == 0;
}

static void fixture_add(
    struct variable_fixture *fixture,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    size_t index;

    for (index = 0U; index < fixture->variable_count; ++index) {
        if (fixture->variables[index].present &&
            fixture_key_equal(
                &fixture->variables[index].key, key)) {
            fixture->variables[index].length = length;
            memcpy(fixture->variables[index].data, data, length);
            return;
        }
    }
    index = fixture->variable_count++;
    fixture->variables[index].present = true;
    fixture->variables[index].key = *key;
    fixture->variables[index].length = length;
    memcpy(fixture->variables[index].data, data, length);
}

static void fixture_add_boot(
    struct variable_fixture *fixture,
    uint16_t number,
    const unsigned char *data,
    size_t length)
{
    struct efi_boot_variable_key key;

    fixture_boot_key(number, &key);
    fixture_add(fixture, &key, data, length);
}

static void fixture_set_order(
    struct variable_fixture *fixture,
    const uint16_t *entries,
    size_t count)
{
    struct efi_boot_variable_key key;
    unsigned char order[EFI_BOOT_VARIABLES_ORDER_CAPACITY];
    size_t index;
    size_t length = 0U;

    for (index = 0U; index < count; ++index) {
        order[length++] =
            (unsigned char)(entries[index] & 0xFFU);
        order[length++] =
            (unsigned char)(entries[index] >> 8U);
    }
    fixture_order_key(&key);
    fixture_add(fixture, &key, order, length);
}

static bool fixture_get_boot(
    const struct variable_fixture *fixture,
    uint16_t number,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    struct efi_boot_variable_key key;
    size_t index;

    fixture_boot_key(number, &key);
    for (index = 0U; index < fixture->variable_count; ++index) {
        const struct fixture_variable *variable =
            &fixture->variables[index];
        if (variable->present &&
            fixture_key_equal(&variable->key, &key)) {
            if (variable->length > capacity) {
                return false;
            }
            memcpy(data, variable->data, variable->length);
            *length = variable->length;
            return true;
        }
    }
    return false;
}

static enum efi_boot_variable_io_status fixture_read_variable(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    struct variable_fixture *fixture =
        (struct variable_fixture *)context;
    size_t index;

    if (fixture == NULL || key == NULL || data == NULL ||
        length == NULL) {
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    if (fixture->fail_read) {
        fixture->fail_read = false;
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    for (index = 0U; index < fixture->variable_count; ++index) {
        const struct fixture_variable *variable =
            &fixture->variables[index];
        if (variable->present &&
            fixture_key_equal(&variable->key, key)) {
            if (variable->length > capacity) {
                return EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL;
            }
            memcpy(data, variable->data, variable->length);
            *length = variable->length;
            return EFI_BOOT_VARIABLE_IO_OK;
        }
    }
    return EFI_BOOT_VARIABLE_IO_NOT_FOUND;
}

static enum efi_boot_variable_io_status fixture_write_variable(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    struct variable_fixture *fixture =
        (struct variable_fixture *)context;

    if (fixture == NULL || key == NULL || data == NULL) {
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    ++fixture->write_calls;
    if (fixture->fail_next_write) {
        fixture->fail_next_write = false;
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    if (length > FIXTURE_DATA_MAX) {
        return EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL;
    }
    fixture_add(fixture, key, data, length);
    return EFI_BOOT_VARIABLE_IO_OK;
}

static struct block_device fixture_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = FIXTURE_BLOCK_SIZE;
    device.block_count = FIXTURE_BLOCK_COUNT;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static void fixture_reset(void)
{
    memset(fixture_storage, 0, sizeof(fixture_storage));
    fixture_write_calls = 0U;
    memset(&variable_store, 0, sizeof(variable_store));
}

static void fixture_report(struct installer_discovery_report *report)
{
    memset(report, 0, sizeof(*report));
    report->version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    report->firmware_mode = INSTALLER_FIRMWARE_UEFI;
    report->read_only = true;
    report->scan_complete = true;
    report->cpu_present = true;
    report->cpu_cores = 2U;
    report->ram_present = true;
    report->ram_bytes = 4294967296ULL;
    report->disks[0].present = true;
    report->disks[0].id = 1U;
    report->disks[0].controller = INSTALLER_DISK_CONTROLLER_NVME;
    report->disks[0].partition_table = INSTALLER_PARTITION_TABLE_GPT;
    report->disks[0].block_size = FIXTURE_BLOCK_SIZE;
    report->disks[0].block_count = FIXTURE_BLOCK_COUNT;
    memcpy(report->disks[0].identity, disk_identity,
           sizeof(disk_identity));
}

#define HARDENING_MEDIA_LENGTH 64U
#define HARDENING_SIGNATURE_LENGTH 32U
#define HARDENING_RELEASE_ID 5U
#define HARDENING_SIGNER_ID 1U

static unsigned char hardening_media[HARDENING_MEDIA_LENGTH];
static unsigned char hardening_envelope[256U];
static size_t hardening_envelope_length;
static struct verification_policy hardening_policy;

static enum installer_media_signature_status hardening_signature_verify(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length)
{
    size_t index;

    (void)context;
    if (algorithm != TRUST_ANCHOR_ALGORITHM_ECDSA_P256 ||
        signer_id != HARDENING_SIGNER_ID ||
        signature_length != HARDENING_SIGNATURE_LENGTH) {
        return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
    }
    for (index = 0U; index < HARDENING_SIGNATURE_LENGTH; ++index) {
        if (signature[index] != (unsigned char)(
                digest[index % SHA256_DIGEST_SIZE] ^ 0x5AU)) {
            return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
        }
    }
    return INSTALLER_MEDIA_SIGNATURE_OK;
}

static bool fixture_bind_hardening(
    struct production_installer *installer,
    const struct installer_discovery_report *report)
{
    struct verification_policy_snapshot snapshot;
    struct installer_media_verifier verifier;
    struct installer_supported_hardware_policy hardware_policy;
    struct installer_media_source media_source;
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char source_digest[SHA256_DIGEST_SIZE];
    unsigned char signature[HARDENING_SIGNATURE_LENGTH];
    uint64_t source_length = 0U;
    size_t index;

    for (index = 0U; index < sizeof(hardening_media); ++index) {
        hardening_media[index] = (unsigned char)(index * 3U + 1U);
    }
    if (sha256_compute(hardening_media, sizeof(hardening_media),
                       digest) != SHA256_OK) {
        return false;
    }
    memset(&media_source, 0, sizeof(media_source));
    media_source.boot_payload = boot_payload;
    media_source.boot_payload_size = sizeof(boot_payload);
    media_source.limine_config = limine_config;
    media_source.limine_config_size = sizeof(limine_config) - 1U;
    media_source.kernel = kernel_payload;
    media_source.kernel_size = sizeof(kernel_payload);
    if (installer_media_source_digest(
            &media_source, source_digest, &source_length) !=
        INSTALLER_MEDIA_OK) {
        return false;
    }
    for (index = 0U; index < sizeof(signature); ++index) {
        signature[index] = (unsigned char)(
            digest[index % SHA256_DIGEST_SIZE] ^ 0x5AU);
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = 1U;
    snapshot.anchor_count = 1U;
    snapshot.anchors[0U].id = HARDENING_SIGNER_ID;
    snapshot.anchors[0U].version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    snapshot.anchors[0U].algorithm_id =
        TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    snapshot.anchors[0U].state = TRUST_ANCHOR_STATE_ACTIVE;
    snapshot.anchors[0U].accepted_purposes =
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    memset(&hardening_policy, 0, sizeof(hardening_policy));
    if (verification_policy_init(&hardening_policy, &snapshot) !=
        VERIFICATION_POLICY_OK) {
        return false;
    }
    hardening_envelope_length = 0U;
    if (installer_media_manifest_seal(
            &hardening_policy, HARDENING_RELEASE_ID,
            HARDENING_SIGNER_ID, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
            digest, sizeof(hardening_media), source_digest,
            source_length, signature, sizeof(signature),
            hardening_envelope, sizeof(hardening_envelope),
            &hardening_envelope_length) != INSTALLER_MEDIA_OK) {
        return false;
    }
    memset(&verifier, 0, sizeof(verifier));
    verifier.verify = hardening_signature_verify;
    if (production_installer_verify_media(
            installer, &hardening_policy, &verifier,
            HARDENING_RELEASE_ID, hardening_envelope,
            hardening_envelope_length, hardening_media,
            sizeof(hardening_media)) != PRODUCTION_INSTALLER_OK) {
        return false;
    }
    if (installer_supported_hardware_policy_default(&hardware_policy) !=
        INSTALLER_SUPPORTED_HARDWARE_OK) {
        return false;
    }
    return production_installer_verify_hardware(
               installer, report, &hardware_policy) ==
           PRODUCTION_INSTALLER_OK;
}

static void fixture_requests(
    struct installer_layout_partition requests[4U])
{
    memset(requests, 0, 4U * sizeof(requests[0U]));
    requests[0U].present = true;
    requests[0U].role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
    requests[0U].length_lba = 2048U;
    requests[1U].present = true;
    requests[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
    requests[1U].length_lba = 2048U;
    requests[2U].present = true;
    requests[2U].role = INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
    requests[2U].length_lba = 2048U;
    requests[3U].present = true;
    requests[3U].role = INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
    requests[3U].length_lba = 2048U;
}

static bool fixture_prepare_controller(
    struct production_installer *installer,
    struct installer_discovery_report *report)
{
    struct production_installer_source source;

    memset(&source, 0, sizeof(source));
    source.version = PRODUCTION_INSTALLER_SOURCE_VERSION;
    source.boot_payload = boot_payload;
    source.boot_payload_size = sizeof(boot_payload);
    source.limine_config = limine_config;
    source.limine_config_size = sizeof(limine_config) - 1U;
    source.kernel = kernel_payload;
    source.kernel_size = sizeof(kernel_payload);
    fixture_report(report);
    return production_installer_init(installer) ==
               PRODUCTION_INSTALLER_OK &&
           production_installer_prepare(
               installer, report,
               PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
               disk_identity, sizeof(disk_identity) - 1U,
               &source, true) ==
               PRODUCTION_INSTALLER_OK &&
           fixture_bind_hardening(installer, report) &&
           production_installer_build_plan(installer) ==
               PRODUCTION_INSTALLER_OK &&
           production_installer_publish_preview(installer) ==
               PRODUCTION_INSTALLER_OK &&
           production_installer_confirm(installer) ==
               PRODUCTION_INSTALLER_OK;
}

static void test_state_initialize_after_layout(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct production_installer_state_erase_evidence erase_evidence;
    struct block_device device;

    fixture_reset();
    fixture_requests(requests);
    CHECK(fixture_prepare_controller(&installer, &report),
          "orchestration fixture prepares controller");
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) ==
              INSTALLER_LAYOUT_OK,
          "orchestration fixture confirms layout");
    device = fixture_device();
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_OK &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_EXECUTING,
          "orchestration fixture executes layout");

    memset(&loader, 0, sizeof(loader));
    memset(&erase_evidence, 0, sizeof(erase_evidence));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_erase(
              &installer, &device, &layout, &plan, &loader,
              &erase_evidence) == PRODUCTION_INSTALLER_OK &&
          erase_evidence.verify_clean &&
          erase_evidence.stores == layout.entry_count,
          "state erase clears the previous run and verifies it empty");
    /*
     * ITEM-504 increment 6: the erase rebuilds eight stores of 64 blocks each
     * (512 blocks) plus the bounded edges of the plan's four roles and the
     * layout writes themselves; 963 writes were measured on this fixture, so
     * the envelope is tight around the accounted work rather than an
     * unbounded "it wrote something" claim.
     */
    CHECK(fixture_write_calls > 400U &&
              fixture_write_calls < 1100U,
          "state erase performs bounded store writes");
    CHECK(production_installer_state_initialize(
              &installer, &device, &layout, &loader) ==
              PRODUCTION_INSTALLER_OK &&
          appliance_state_loader_ready(&loader),
          "state initialization publishes loader-ready partition");
}

/*
 * ITEM-506 increment 4: the operator set the account on the install screen, so
 * the record is committed into the installed system's state and proved by
 * reading it back through the installed system's own reader.  The proof builds
 * the same record the flow builds (a real PBKDF2 credential with a fixed salt)
 * and checks the committed record through that reader, not the candidate the
 * flow passed in.  (The commit's durability is the store's journal write, as
 * on the console path; this proof pins the reader, not a media flush.)
 */
static void test_state_commits_the_operator_account(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct production_installer_state_erase_evidence erase_evidence;
    struct block_device device;
    static const char login[] = "installer1";
    static const char password[] = "install time passphrase";
    static const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH] = {
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U,
        0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU, 0x00U
    };
    struct appliance_admin_credential credential;
    struct appliance_admin_credential readback;

    fixture_reset();
    fixture_requests(requests);
    CHECK(fixture_prepare_controller(&installer, &report),
          "account fixture prepares controller");
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK,
          "account fixture confirms layout");
    device = fixture_device();
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_OK,
          "account fixture executes layout");
    memset(&loader, 0, sizeof(loader));
    memset(&erase_evidence, 0, sizeof(erase_evidence));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_erase(
              &installer, &device, &layout, &plan, &loader,
              &erase_evidence) == PRODUCTION_INSTALLER_OK &&
          production_installer_state_initialize(
              &installer, &device, &layout, &loader) ==
              PRODUCTION_INSTALLER_OK,
          "account fixture reaches initialized state");
    memset(&credential, 0, sizeof(credential));
    CHECK(appliance_admin_credential_create(
              login, strlen(login), password, strlen(password), salt,
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT, &credential) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the account fixture builds a real PBKDF2 record");
    CHECK(production_installer_state_commit_admin(&loader, &credential) ==
              PRODUCTION_INSTALLER_OK,
          "the account record commits into the installed state");
    memset(&readback, 0, sizeof(readback));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &loader.secret_config, &readback) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the installed system's own reader finds the record");
    CHECK(memcmp(readback.login, credential.login,
                 sizeof(readback.login)) == 0 &&
              memcmp(readback.salt, credential.salt,
                     sizeof(readback.salt)) == 0 &&
              memcmp(readback.verifier, credential.verifier,
                     sizeof(readback.verifier)) == 0 &&
              readback.iterations == credential.iterations,
          "the committed record is byte-for-byte the account that was set");
    memset(&readback, 0, sizeof(readback));
    memset(&credential, 0, sizeof(credential));
}

/*
 * ITEM-509 increment 27 (509.7c-2): the language the installation medium
 * declared is committed into the installed system's state and proved by the
 * installed system's own reader.  A freshly initialised state reads the default
 * language from the absence of the record line; the commit writes `ru`, the
 * reader returns `ru`, and the invalid language and the missing loader fail
 * closed instead of leaving a half-written setting.
 */
static void test_state_commits_the_declared_language(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct production_installer_state_erase_evidence erase_evidence;
    struct block_device device;
    enum appliance_language readback = APPLIANCE_LANGUAGE_RU;

    fixture_reset();
    fixture_requests(requests);
    CHECK(fixture_prepare_controller(&installer, &report),
          "language fixture prepares controller");
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK,
          "language fixture confirms layout");
    device = fixture_device();
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_OK,
          "language fixture executes layout");
    memset(&loader, 0, sizeof(loader));
    memset(&erase_evidence, 0, sizeof(erase_evidence));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_erase(
              &installer, &device, &layout, &plan, &loader,
              &erase_evidence) == PRODUCTION_INSTALLER_OK &&
          production_installer_state_initialize(
              &installer, &device, &layout, &loader) ==
              PRODUCTION_INSTALLER_OK,
          "language fixture reaches initialized state");
    CHECK(appliance_persistent_config_language(
              &loader.system_config, &readback) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
          readback == APPLIANCE_LANGUAGE_DEFAULT,
          "a fresh installed state reads the default language from absence");
    CHECK(production_installer_state_commit_language(
              &loader, APPLIANCE_LANGUAGE_RU) == PRODUCTION_INSTALLER_OK,
          "the declared language commits into the installed state");
    readback = APPLIANCE_LANGUAGE_DEFAULT;
    CHECK(appliance_persistent_config_language(
              &loader.system_config, &readback) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
          readback == APPLIANCE_LANGUAGE_RU,
          "the installed system's own reader returns the declared language");
    CHECK(production_installer_state_commit_language(
              &loader, (enum appliance_language)99) ==
              PRODUCTION_INSTALLER_VERIFICATION_FAILED,
          "a language the catalog does not name is refused");
    CHECK(production_installer_state_commit_language(
              NULL, APPLIANCE_LANGUAGE_RU) ==
              PRODUCTION_INSTALLER_VERIFICATION_FAILED,
          "a missing loader refuses the commit");
    CHECK(production_installer_state_commit_language(
              &loader, APPLIANCE_LANGUAGE_EN) == PRODUCTION_INSTALLER_OK,
          "returning to the default language commits");
    readback = APPLIANCE_LANGUAGE_RU;
    CHECK(appliance_persistent_config_language(
              &loader.system_config, &readback) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
          readback == APPLIANCE_LANGUAGE_DEFAULT,
          "the installed reader returns the default after the return");
}

/*
 * ITEM-506 increment 5: the serving gate's state half.  A real installed state
 * is initialized (loader ready), and the installed system's own reader is asked
 * whether an account exists before and after the commit: absent before, the
 * structured record after.  That is the transition the serving gate keys on,
 * with the state-volume term true; the predicate itself (only a structured
 * account counts) is proved by test_first_run_serving_gate in the enrollment
 * host test, and the no-volume behaviour by the untouched network proof.
 */
static void test_installed_state_first_run_gate(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct production_installer_state_erase_evidence erase_evidence;
    struct block_device device;
    static const char login[] = "installer1";
    static const char password[] = "install time passphrase";
    static const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
        0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U
    };
    struct appliance_admin_credential credential;
    struct appliance_admin_credential readback;

    fixture_reset();
    fixture_requests(requests);
    CHECK(fixture_prepare_controller(&installer, &report),
          "first-run fixture prepares controller");
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK,
          "first-run fixture confirms layout");
    device = fixture_device();
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_OK,
          "first-run fixture executes layout");
    memset(&loader, 0, sizeof(loader));
    memset(&erase_evidence, 0, sizeof(erase_evidence));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_erase(
              &installer, &device, &layout, &plan, &loader,
              &erase_evidence) == PRODUCTION_INSTALLER_OK &&
          production_installer_state_initialize(
              &installer, &device, &layout, &loader) ==
              PRODUCTION_INSTALLER_OK &&
          appliance_state_loader_ready(&loader),
          "first-run fixture reaches loader-ready state");
    memset(&readback, 0, sizeof(readback));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &loader.secret_config, &readback) !=
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "an installed unattended state carries no account to serve with");
    memset(&credential, 0, sizeof(credential));
    CHECK(appliance_admin_credential_create(
              login, strlen(login), password, strlen(password), salt,
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT, &credential) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the first-run fixture builds a real PBKDF2 record");
    CHECK(production_installer_state_commit_admin(&loader, &credential) ==
              PRODUCTION_INSTALLER_OK,
          "the first-run account commits into the installed state");
    memset(&readback, 0, sizeof(readback));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &loader.secret_config, &readback) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK &&
          strcmp(readback.login, login) == 0,
          "the installed system's own reader now finds the account");
    memset(&credential, 0, sizeof(credential));
    memset(&readback, 0, sizeof(readback));
}

static void test_state_requires_executing_stage(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct block_device device;

    fixture_reset();
    fixture_prepare_controller(&installer, &report);
    device = fixture_device();
    memset(&loader, 0, sizeof(loader));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_initialize(
              &installer, &device, &layout, &loader) ==
              PRODUCTION_INSTALLER_BAD_STATE,
          "state initialization rejects unexecuted controller");
}

/*
 * ITEM-502: the previous run's address and administrator credential, committed
 * through the real owners into the real stores, are what a reinstall must not
 * resurrect.
 */
static bool fixture_commit_old_state(struct block_device *device)
{
    struct appliance_state_layout layout;
    struct appliance_gpt_hfoc_partition partition;
    struct appliance_state_block_device partition_device;
    struct appliance_state_block_device store;
    struct appliance_persistent_network_config network;
    struct appliance_persistent_secret_config secret;
    struct network_ipv4_config static_config;
    unsigned char fingerprint[SHA256_DIGEST_SIZE];

    if (appliance_state_layout_default(&layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        return false;
    }
    memset(&partition, 0, sizeof(partition));
    if (appliance_gpt_find_hfoc_partition(device, &partition) !=
            APPLIANCE_GPT_DISCOVERY_OK ||
        appliance_state_block_device_bind_range(
            device, partition.first_lba, partition.block_count,
            &partition_device) != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return false;
    }
    /* Layout entry 1 is the network store, entry 4 the secret store. */
    if (appliance_state_block_device_bind(
            &partition_device.device, &layout.entries[1U], &store) !=
        APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return false;
    }
    memset(&network, 0, sizeof(network));
    if (appliance_persistent_network_config_load(
            &network, &store.device) !=
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK ||
        network_ipv4_config_from_parts(
            "192.0.2.33/24", sizeof("192.0.2.33/24") - 1U,
            "192.0.2.1", sizeof("192.0.2.1") - 1U,
            "192.0.2.1", sizeof("192.0.2.1") - 1U,
            &static_config) != NETWORK_IPV4_OK ||
        appliance_persistent_network_config_set_static(
            &network, "eth0", 4U, &static_config) !=
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK) {
        return false;
    }
    if (appliance_state_block_device_bind(
            &partition_device.device, &layout.entries[4U], &store) !=
        APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return false;
    }
    memset(&secret, 0, sizeof(secret));
    memset(fingerprint, 0x5AU, sizeof(fingerprint));
    return appliance_persistent_secret_config_load(
               &secret, &store.device) ==
               APPLIANCE_PERSISTENT_SECRET_CONFIG_OK &&
           appliance_persistent_secret_config_commit_admin(
               &secret, fingerprint) ==
               APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
}

/* Does the machine still read the previous run's address or credential? */
static bool fixture_old_state_present(struct block_device *device)
{
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    const struct appliance_persistent_network_config *network;
    const struct appliance_persistent_secret_config *secret;
    unsigned char fingerprint[SHA256_DIGEST_SIZE];

    if (appliance_state_layout_default(&layout) !=
            APPLIANCE_STATE_LAYOUT_OK) {
        return true;
    }
    memset(&loader, 0, sizeof(loader));
    if (appliance_state_loader_init(&loader) !=
            APPLIANCE_STATE_LOADER_OK ||
        appliance_state_loader_load(&loader, device, &layout) !=
            APPLIANCE_STATE_LOADER_OK) {
        return true;
    }
    network = appliance_state_loader_network_config(&loader);
    secret = appliance_state_loader_secret_config(&loader);
    return network == NULL || secret == NULL ||
           network->source ==
               APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED ||
           appliance_persistent_secret_config_admin(secret, fingerprint);
}

static void test_state_initialize_requires_erase(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct block_device device;

    fixture_reset();
    fixture_requests(requests);
    fixture_prepare_controller(&installer, &report);
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK,
          "erase-gate fixture confirms layout");
    device = fixture_device();
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_OK,
          "erase-gate fixture executes layout");
    memset(&loader, 0, sizeof(loader));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_initialize(
              &installer, &device, &layout, &loader) ==
              PRODUCTION_INSTALLER_BAD_STATE,
          "state initialization refuses a run that did not erase");
    CHECK(production_installer_verify_layout(
              &installer, &device, &plan) ==
              PRODUCTION_INSTALLER_BAD_STATE,
          "layout verification refuses a run that did not erase");
}

static void test_reinstall_erases_the_previous_run(void)
{
    struct production_installer first;
    struct production_installer second;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct production_installer_state_erase_evidence erase_evidence;
    struct block_device device;

    fixture_reset();
    fixture_requests(requests);
    device = fixture_device();
    CHECK(fixture_prepare_controller(&first, &report) &&
              installer_layout_plan_whole_disk(
                  &report, 1U, requests, 4U, &plan) ==
                  INSTALLER_LAYOUT_OK &&
              installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK &&
              production_installer_execute_layout(
                  &first, &device, &plan, &snapshot) ==
                  PRODUCTION_INSTALLER_OK,
          "the first install writes its layout");
    memset(&loader, 0, sizeof(loader));
    memset(&erase_evidence, 0, sizeof(erase_evidence));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_erase(
              &first, &device, &layout, &plan, &loader,
              &erase_evidence) == PRODUCTION_INSTALLER_OK &&
          production_installer_state_initialize(
              &first, &device, &layout, &loader) ==
              PRODUCTION_INSTALLER_OK,
          "the first install erases and initializes its state");
    CHECK(fixture_commit_old_state(&device),
          "the first run commits a static address and a credential");
    CHECK(fixture_old_state_present(&device),
          "the committed state is readable before the reinstall");

    /*
     * The reinstall: a fresh controller writes the same layout again.  The
     * layout writer only touches the GPT, exactly like the install that
     * produced the owner's report.
     */
    CHECK(fixture_prepare_controller(&second, &report) &&
              installer_layout_plan_whole_disk(
                  &report, 1U, requests, 4U, &plan) ==
                  INSTALLER_LAYOUT_OK &&
              installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK &&
              production_installer_execute_layout(
                  &second, &device, &plan, &snapshot) ==
                  PRODUCTION_INSTALLER_OK,
          "the reinstall writes the layout again");
    CHECK(fixture_old_state_present(&device),
          "the layout write alone does not touch the committed state");
    memset(&loader, 0, sizeof(loader));
    memset(&erase_evidence, 0, sizeof(erase_evidence));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_erase(
              &second, &device, &layout, &plan, &loader,
              &erase_evidence) == PRODUCTION_INSTALLER_OK &&
          erase_evidence.verify_clean,
          "the reinstall erases the state and verifies it empty");
    CHECK(!fixture_old_state_present(&device),
          "the previous run's address and credential are gone");
}

static void test_update_preserves_the_previous_run(void)
{
    struct production_installer first;
    struct production_installer update;
    struct production_installer empty;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct production_installer_state_erase_evidence erase_evidence;
    struct production_installer_state_preserve_evidence preserve_evidence;
    struct block_device device;

    fixture_reset();
    fixture_requests(requests);
    device = fixture_device();
    CHECK(fixture_prepare_controller(&first, &report) &&
              installer_layout_plan_whole_disk(
                  &report, 1U, requests, 4U, &plan) ==
                  INSTALLER_LAYOUT_OK &&
              installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK &&
              production_installer_execute_layout(
                  &first, &device, &plan, &snapshot) ==
                  PRODUCTION_INSTALLER_OK,
          "update fixture writes the installed layout");
    memset(&loader, 0, sizeof(loader));
    memset(&erase_evidence, 0, sizeof(erase_evidence));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_erase(
              &first, &device, &layout, &plan, &loader,
              &erase_evidence) == PRODUCTION_INSTALLER_OK &&
          production_installer_state_initialize(
              &first, &device, &layout, &loader) ==
              PRODUCTION_INSTALLER_OK &&
          fixture_commit_old_state(&device),
          "update fixture installs a real configuration and account");

    /*
     * ITEM-586: before the layout write, the update preflight reads the
     * existing persistence partition in exactly the range the new plan will
     * write and loads the installed reader.  The plan is not executed yet, so
     * a refusal here is still before the first write.
     */
    CHECK(fixture_prepare_controller(&update, &report) &&
              installer_layout_plan_whole_disk(
                  &report, 1U, requests, 4U, &plan) ==
                  INSTALLER_LAYOUT_OK &&
              installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK,
          "update fixture builds and confirms the same layout");
    memset(&loader, 0, sizeof(loader));
    memset(&preserve_evidence, 0, sizeof(preserve_evidence));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_preserve(
              &update, &device, &layout, &plan, &loader,
              &preserve_evidence) == PRODUCTION_INSTALLER_OK &&
          preserve_evidence.verify_loaded &&
          preserve_evidence.config_preserved &&
          preserve_evidence.admin_preserved &&
          preserve_evidence.stores == layout.entry_count &&
          update.state_preserved && !update.state_erased,
          "the update preflight proves the existing state is loadable");
    CHECK(production_installer_execute_layout(
              &update, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_OK &&
          fixture_old_state_present(&device),
          "the update writes the layout without touching the state");
    CHECK(production_installer_state_preserve(
              &update, &device, &layout, &plan, &loader,
              &preserve_evidence) == PRODUCTION_INSTALLER_BAD_STATE,
          "an update decides the state once");

    /*
     * A plan that would move persistence is not an update: the old bytes would
     * stay on a partition the new table no longer calls persistence, so the
     * preflight refuses before the layout write.
     */
    {
        struct production_installer moved;
        struct installer_layout_plan moved_plan;
        uint32_t index;

        CHECK(fixture_prepare_controller(&moved, &report) &&
                  installer_layout_plan_whole_disk(
                      &report, 1U, requests, 4U, &moved_plan) ==
                      INSTALLER_LAYOUT_OK &&
                  installer_layout_confirm(&moved_plan) ==
                      INSTALLER_LAYOUT_OK,
              "moved-persistence fixture builds a plan");
        for (index = 0U; index < moved_plan.partition_count; ++index) {
            if (moved_plan.partitions[index].role ==
                INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE) {
                moved_plan.partitions[index].first_lba += 1U;
            }
        }
        memset(&loader, 0, sizeof(loader));
        memset(&preserve_evidence, 0, sizeof(preserve_evidence));
        CHECK(production_installer_state_preserve(
                  &moved, &device, &layout, &moved_plan, &loader,
                  &preserve_evidence) ==
                  PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE,
              "a plan that moves persistence is refused before the write");
    }

    /*
     * ITEM-588: a loadable volume that still holds only the defaults is a
     * valid update target.  This is the owner's Toshiba: the install ran, the
     * account could not be set because the CPU has no entropy source, and the
     * refusal kept the appliance on the old build.
     */
    {
        struct production_installer defaults;
        struct installer_layout_plan defaults_plan;
        struct block_device defaults_device;

        fixture_reset();
        fixture_requests(requests);
        defaults_device = fixture_device();
        CHECK(fixture_prepare_controller(&defaults, &report) &&
                  installer_layout_plan_whole_disk(
                      &report, 1U, requests, 4U, &defaults_plan) ==
                      INSTALLER_LAYOUT_OK &&
                  installer_layout_confirm(&defaults_plan) ==
                      INSTALLER_LAYOUT_OK &&
                  production_installer_execute_layout(
                      &defaults, &defaults_device, &defaults_plan,
                      &snapshot) == PRODUCTION_INSTALLER_OK,
              "default-state fixture writes the installed layout");
        memset(&loader, 0, sizeof(loader));
        memset(&erase_evidence, 0, sizeof(erase_evidence));
        CHECK(appliance_state_layout_default(&layout) ==
                  APPLIANCE_STATE_LAYOUT_OK &&
              production_installer_state_erase(
                  &defaults, &defaults_device, &layout, &defaults_plan,
                  &loader, &erase_evidence) == PRODUCTION_INSTALLER_OK &&
              production_installer_state_initialize(
                  &defaults, &defaults_device, &layout, &loader) ==
                  PRODUCTION_INSTALLER_OK,
              "default-state fixture initialises the stores and nothing else");
        CHECK(fixture_prepare_controller(&defaults, &report) &&
                  installer_layout_plan_whole_disk(
                      &report, 1U, requests, 4U, &defaults_plan) ==
                      INSTALLER_LAYOUT_OK &&
                  installer_layout_confirm(&defaults_plan) ==
                      INSTALLER_LAYOUT_OK,
                  "default-state fixture rebuilds and confirms the plan");
        memset(&loader, 0, sizeof(loader));
        memset(&preserve_evidence, 0, sizeof(preserve_evidence));
        CHECK(appliance_state_layout_default(&layout) ==
                  APPLIANCE_STATE_LAYOUT_OK &&
              production_installer_state_preserve(
                  &defaults, &defaults_device, &layout, &defaults_plan,
                  &loader, &preserve_evidence) ==
                  PRODUCTION_INSTALLER_OK &&
              preserve_evidence.verify_loaded &&
              preserve_evidence.loader_ready &&
              preserve_evidence.current_present &&
              preserve_evidence.current_first_lba ==
                  preserve_evidence.plan_first_lba &&
              preserve_evidence.current_length_lba ==
                  preserve_evidence.plan_length_lba &&
              !preserve_evidence.config_preserved &&
              !preserve_evidence.admin_preserved &&
              preserve_evidence.refusal ==
                  PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_NONE,
              "a loadable default-only volume is updated, not refused");
    }

    /*
     * A plan that moves persistence is refused with the evidence that names
     * the two ranges, so the console and the journal can say which rule
     * stopped the run.
     */
    {
        struct production_installer moved;
        struct installer_layout_plan moved_plan;
        struct block_device moved_device;

        fixture_reset();
        fixture_requests(requests);
        moved_device = fixture_device();
        CHECK(fixture_prepare_controller(&moved, &report) &&
                  installer_layout_plan_whole_disk(
                      &report, 1U, requests, 4U, &plan) ==
                      INSTALLER_LAYOUT_OK &&
                  installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK &&
                  production_installer_execute_layout(
                      &moved, &moved_device, &plan, &snapshot) ==
                      PRODUCTION_INSTALLER_OK,
              "moved-range fixture writes the installed layout");
        memset(&loader, 0, sizeof(loader));
        memset(&erase_evidence, 0, sizeof(erase_evidence));
        CHECK(appliance_state_layout_default(&layout) ==
                  APPLIANCE_STATE_LAYOUT_OK &&
              production_installer_state_erase(
                  &moved, &moved_device, &layout, &plan, &loader,
                  &erase_evidence) == PRODUCTION_INSTALLER_OK &&
              production_installer_state_initialize(
                  &moved, &moved_device, &layout, &loader) ==
                  PRODUCTION_INSTALLER_OK,
              "moved-range fixture initialises the stores");
        CHECK(fixture_prepare_controller(&moved, &report) &&
                  installer_layout_plan_whole_disk(
                      &report, 1U, requests, 4U, &moved_plan) ==
                      INSTALLER_LAYOUT_OK &&
                  installer_layout_confirm(&moved_plan) ==
                      INSTALLER_LAYOUT_OK,
              "moved-range fixture rebuilds and confirms the plan");
        {
            uint32_t moved_index;

            for (moved_index = 0U;
                 moved_index < moved_plan.partition_count;
                 ++moved_index) {
                if (moved_plan.partitions[moved_index].role ==
                    INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE) {
                    moved_plan.partitions[moved_index].first_lba += 1U;
                }
            }
        }
        memset(&loader, 0, sizeof(loader));
        memset(&preserve_evidence, 0, sizeof(preserve_evidence));
        CHECK(production_installer_state_preserve(
                  &moved, &moved_device, &layout, &moved_plan, &loader,
                  &preserve_evidence) ==
                  PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE &&
              preserve_evidence.refusal ==
                  PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_RANGE_MISMATCH &&
              preserve_evidence.current_present &&
              preserve_evidence.current_first_lba !=
                  preserve_evidence.plan_first_lba &&
              preserve_evidence.plan_first_lba ==
                  preserve_evidence.current_first_lba + 1U &&
              !preserve_evidence.verify_loaded,
              "a moved persistence range is refused with both ranges named");
    }

    /*
     * A disk that carries the layout but no initialised stores is refused by
     * the reader, not by an empty configuration.
     */
    fixture_reset();
    fixture_requests(requests);
    CHECK(fixture_prepare_controller(&empty, &report) &&
              installer_layout_plan_whole_disk(
                  &report, 1U, requests, 4U, &plan) ==
                  INSTALLER_LAYOUT_OK &&
              installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK &&
              production_installer_execute_layout(
                  &empty, &device, &plan, &snapshot) ==
                  PRODUCTION_INSTALLER_OK,
          "empty-target fixture writes only the layout");
    memset(&loader, 0, sizeof(loader));
    memset(&preserve_evidence, 0, sizeof(preserve_evidence));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
          production_installer_state_preserve(
              &empty, &device, &layout, &plan, &loader,
              &preserve_evidence) ==
              PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE,
          "an empty target is not an update");
}

static void test_esp_install_after_layout(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    struct production_installer_source source;
    struct block_device device;
    unsigned char readback[sizeof(boot_payload)];
    size_t readback_size = 0U;

    fixture_reset();
    fixture_requests(requests);
    CHECK(fixture_prepare_controller(&installer, &report),
          "ESP fixture prepares controller");
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) ==
              INSTALLER_LAYOUT_OK,
          "ESP fixture confirms layout");
    device = fixture_device();
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_OK,
          "ESP fixture executes layout");

    memset(&source, 0, sizeof(source));
    source.version = PRODUCTION_INSTALLER_SOURCE_VERSION;
    source.boot_payload = boot_payload;
    source.boot_payload_size = sizeof(boot_payload);
    source.limine_config = limine_config;
    source.limine_config_size = sizeof(limine_config) - 1U;
    source.kernel = kernel_payload;
    source.kernel_size = sizeof(kernel_payload);

    memset(&environment, 0, sizeof(environment));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              &variable_store, fixture_read_variable,
              fixture_write_variable) ==
              EFI_BOOT_ENVIRONMENT_OK &&
          efi_boot_environment_set_device_path(
              &environment, test_device_path,
              sizeof(test_device_path)) ==
              EFI_BOOT_ENVIRONMENT_OK,
          "ESP fixture environment initializes");
    CHECK(production_installer_esp_install(
              &installer, &environment,
              plan.partitions[0U].first_lba,
              plan.partitions[0U].length_lba,
              &source, true) ==
              PRODUCTION_INSTALLER_OK,
          "clean ESP installs HFOC payload through controller");
    CHECK(efi_fat32_esp_bind(
              &environment,
              plan.partitions[0U].first_lba,
              plan.partitions[0U].length_lba,
              &esp) == EFI_FAT32_ESP_OK &&
          efi_fat32_esp_read_payload(
              &esp, readback, sizeof(readback),
              &readback_size) == EFI_FAT32_ESP_OK &&
              readback_size == sizeof(boot_payload) &&
              memcmp(readback, boot_payload,
                     sizeof(boot_payload)) == 0,
          "installed ESP payload reads back byte-for-byte");
}

static void test_efi_boot_variables_after_layout(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct efi_boot_environment environment;
    struct efi_boot_variables_snapshot after;
    struct block_device device;
    unsigned char linux_before[64U];
    unsigned char linux_after[64U];
    unsigned char windows_before[80U];
    unsigned char windows_after[80U];
    unsigned char hfoc_data[FIXTURE_DATA_MAX];
    size_t read_length = 0U;
    uint16_t unrelated_order[2U] = { 1U, 2U };
    uint16_t expected_order[3U] = { 0U, 1U, 2U };
    size_t index;

    fixture_reset();
    memset(linux_before, 0x11U, sizeof(linux_before));
    memset(windows_before, 0x22U, sizeof(windows_before));
    fixture_add_boot(
        &variable_store, 1U,
        linux_before, sizeof(linux_before));
    fixture_add_boot(
        &variable_store, 2U,
        windows_before, sizeof(windows_before));
    fixture_set_order(
        &variable_store, unrelated_order, 2U);

    fixture_requests(requests);
    CHECK(fixture_prepare_controller(&installer, &report),
          "boot variable fixture prepares controller");
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) ==
              INSTALLER_LAYOUT_OK,
          "boot variable fixture confirms layout");
    device = fixture_device();
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_OK,
          "boot variable fixture executes layout");

    memset(&environment, 0, sizeof(environment));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              &variable_store, fixture_read_variable,
              fixture_write_variable) ==
              EFI_BOOT_ENVIRONMENT_OK &&
          efi_boot_environment_set_device_path(
              &environment, test_device_path,
              sizeof(test_device_path)) ==
              EFI_BOOT_ENVIRONMENT_OK &&
          production_installer_efi_apply_boot(
              &installer, &environment) ==
              PRODUCTION_INSTALLER_OK,
          "boot variable seam applies and verifies HFOC boot entry");

    memset(&after, 0, sizeof(after));
    CHECK(efi_boot_variables_snapshot(
              &environment, &after) ==
              EFI_BOOT_VARIABLES_OK &&
              after.boot_order_present &&
              after.boot_order_length == 6U &&
              after.entry_count == 3U,
          "snapshot shows HFOC plus both unrelated entries");
    for (index = 0U; index < 3U; ++index) {
        CHECK((uint16_t)(
                  after.boot_order[index * 2U] |
                  ((uint16_t)after.boot_order[index * 2U + 1U] <<
                   8U)) == expected_order[index],
              "boot order preserves unrelated relative order");
    }
    read_length = 0U;
    CHECK(fixture_get_boot(
              &variable_store, 0U, hfoc_data,
              sizeof(hfoc_data), &read_length) &&
              read_length > 0U,
          "HFOC Boot0000 entry was created");
    read_length = 0U;
    CHECK(fixture_get_boot(
              &variable_store, 1U, linux_after,
              sizeof(linux_after), &read_length) &&
              read_length == sizeof(linux_before) &&
              memcmp(linux_after, linux_before,
                     sizeof(linux_before)) == 0,
          "unrelated Linux Boot0001 entry is preserved");
    read_length = 0U;
    CHECK(fixture_get_boot(
              &variable_store, 2U, windows_after,
              sizeof(windows_after), &read_length) &&
              read_length == sizeof(windows_before) &&
              memcmp(windows_after, windows_before,
                     sizeof(windows_before)) == 0,
          "unrelated Windows Boot0002 entry is preserved");
}

int main(void)
{
    test_state_requires_executing_stage();
    test_state_initialize_after_layout();
    test_state_commits_the_operator_account();
    test_state_commits_the_declared_language();
    test_installed_state_first_run_gate();
    test_state_initialize_requires_erase();
    test_reinstall_erases_the_previous_run();
    test_update_preserves_the_previous_run();
    test_esp_install_after_layout();
    test_efi_boot_variables_after_layout();

    if (failures != 0) {
        fprintf(stderr,
                "Production installer orchestration host tests: "
                "%d FAILURES\n",
                failures);
        return 1;
    }
    puts(
        "Production installer orchestration host tests: ALL OK");
    return 0;
}

#endif
