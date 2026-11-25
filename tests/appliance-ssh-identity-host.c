/*
 * ITEM-601: durable SSH host identity and authorized-key records.
 *
 * The proof uses the real state-block wrapper, the real default layout and a
 * bounded RAM fake of the state partition.  It checks the CERT tail offsets,
 * generate-once and reload, the authorized-key add/reload/remove cycle, and
 * that a corrupt host record is a named refusal rather than a silent
 * regeneration.
 */
#include "appliance_ssh_identity.h"
#include "appliance_state_block_device.h"
#include "appliance_state_layout.h"
#include "block_device.h"

#include "qemu-test-key.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_SSH_IDENTITY_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 512U
#define TEST_STORAGE_BYTES (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define TEST_CERT_OFFSET 192U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct fake_device {
    uint8_t storage[TEST_STORAGE_BYTES];
    bool fail_write;
    unsigned int writes;
    unsigned int flushes;
};

static struct fake_device fake;

static enum block_device_status fake_read(struct block_device *device,
                                          uint64_t offset, void *buffer,
                                          size_t length)
{
    (void)device;
    if (offset + length > sizeof(fake.storage)) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, fake.storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fake_write(struct block_device *device,
                                           uint64_t offset,
                                           const void *buffer,
                                           size_t length)
{
    (void)device;
    ++fake.writes;
    if (fake.fail_write) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (offset + length > sizeof(fake.storage)) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(fake.storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fake_flush(struct block_device *device)
{
    (void)device;
    ++fake.flushes;
    return BLOCK_DEVICE_OK;
}

static uint64_t rng_state = UINT64_C(0x9E3779B97F4A7C15);

static int test_rng(void *context, unsigned char *output, size_t length)
{
    size_t index;

    (void)context;
    for (index = 0U; index < length; ++index) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        output[index] = (unsigned char)(rng_state >> 24);
    }
    return 0;
}

static void init_loader(struct appliance_state_loader *loader)
{
    memset(&fake, 0, sizeof(fake));
    memset(loader, 0, sizeof(*loader));
    loader->initialized = true;
    loader->ready = true;
    loader->status = APPLIANCE_STATE_LOADER_OK;
    CHECK(appliance_state_layout_default(&loader->layout) ==
              APPLIANCE_STATE_LAYOUT_OK,
          "default state layout is available");
    CHECK(appliance_state_block_device_bind_range(
              &(struct block_device){
                  .block_size = TEST_BLOCK_SIZE,
                  .block_count = TEST_BLOCK_COUNT,
                  .context = &fake,
                  .read_fn = fake_read,
                  .write_fn = fake_write,
                  .flush_fn = fake_flush,
              },
              0U, TEST_BLOCK_COUNT, &loader->partition) ==
              APPLIANCE_STATE_BLOCK_DEVICE_OK,
          "the fake state partition binds");
}

static void test_generate_reload_and_authorized_key(void)
{
    struct appliance_state_loader loader;
    struct appliance_ssh_identity first;
    struct appliance_ssh_identity second;
    uint8_t client_scalar[32];
    uint8_t client_blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX];
    uint8_t client_fingerprint[32];
    size_t client_blob_length = 0U;
    size_t host_blob_length = 0U;
    const uint8_t *host_blob;
    uint8_t stored_fingerprint[32];

    init_loader(&loader);
    memset(&first, 0, sizeof(first));
    appliance_ssh_identity_init(&first);
    CHECK(appliance_ssh_identity_prepare(&first, &loader, test_rng, NULL) ==
              APPLIANCE_SSH_IDENTITY_OK,
          "a blank CERT tail generates and persists the host key");
    CHECK(appliance_ssh_identity_host_ready(&first),
          "the generated host key is published");
    CHECK(!appliance_ssh_identity_authorized_ready(&first),
          "a blank authorized record authorizes nobody");
    host_blob = appliance_ssh_identity_host_blob(&first, &host_blob_length);
    CHECK(host_blob != NULL && host_blob_length == 104U,
          "the host key blob is the ECDSA P-256 SSH blob");
    memcpy(stored_fingerprint,
           appliance_ssh_identity_host_fingerprint(&first), 32U);
    CHECK(fake.writes > 0U && fake.flushes > 0U,
          "the generated host key was written and flushed");
    CHECK(memcmp(fake.storage + (TEST_CERT_OFFSET +
                                 APPLIANCE_SSH_CERT_TAIL_BLOCK_OFFSET) *
                                    TEST_BLOCK_SIZE,
                 "HFSSHKEY", 8U) == 0,
          "the host key record is at the free CERT tail block");
    CHECK(fake.storage[0U] == 0U,
          "the TLS certificate record's first block was not touched");

    memset(&second, 0, sizeof(second));
    appliance_ssh_identity_init(&second);
    CHECK(appliance_ssh_identity_prepare(&second, &loader, test_rng, NULL) ==
              APPLIANCE_SSH_IDENTITY_OK &&
              appliance_ssh_identity_host_ready(&second),
          "a second boot reloads the same host identity");
    CHECK(appliance_ssh_identity_host_blob(&second, &host_blob_length) != NULL &&
              host_blob_length == 104U &&
              memcmp(appliance_ssh_identity_host_blob(&second,
                                                      &host_blob_length),
                     host_blob, 104U) == 0 &&
              memcmp(appliance_ssh_identity_host_fingerprint(&second),
                     stored_fingerprint, 32U) == 0,
          "the reloaded host key and fingerprint are byte-identical");

    CHECK(hfoc_ssh_crypto_host_key_create(
              client_scalar, client_blob, &client_blob_length,
              client_fingerprint, test_rng, NULL),
          "a client key blob is available for enrollment");
    CHECK(appliance_ssh_identity_authorized_key_add(
              &second, client_blob, client_blob_length) ==
              APPLIANCE_SSH_IDENTITY_OK &&
              appliance_ssh_identity_authorized_ready(&second),
          "the authorized client key is enrolled and published");

    memset(&first, 0, sizeof(first));
    appliance_ssh_identity_init(&first);
    CHECK(appliance_ssh_identity_prepare(&first, &loader, test_rng, NULL) ==
              APPLIANCE_SSH_IDENTITY_OK &&
              appliance_ssh_identity_authorized_ready(&first),
          "the authorized key survives a fresh load");
    {
        size_t authorized_length = 0U;
        const uint8_t *authorized =
            appliance_ssh_identity_authorized_blob(&first,
                                                   &authorized_length);
        CHECK(authorized != NULL &&
                  authorized_length == client_blob_length &&
                  memcmp(authorized, client_blob, client_blob_length) == 0,
              "the reloaded authorized key is the enrolled blob");
    }
    CHECK(appliance_ssh_identity_authorized_key_remove(&first) ==
              APPLIANCE_SSH_IDENTITY_OK &&
              !appliance_ssh_identity_authorized_ready(&first),
          "removing the authorized key clears it");
    memset(&second, 0, sizeof(second));
    appliance_ssh_identity_init(&second);
    CHECK(appliance_ssh_identity_prepare(&second, &loader, test_rng, NULL) ==
              APPLIANCE_SSH_IDENTITY_OK &&
              !appliance_ssh_identity_authorized_ready(&second),
          "a removed authorized key stays removed after reload");
}

static void test_enrollment_point_text(void)
{
    static const char point_base64[] =
        "BBvtSaBbCvMN+KXyhb3R6mZFrEaz0KUiRusiUUka6JcUgHPG+zEyi4M7yhOJnIoxR6K1Bk3wDJ0J37IxRSyTdaY=";
    uint8_t blob[192];
    uint8_t fingerprint[32];
    size_t blob_length = 0U;
    char text[64];

    memset(blob, 0, sizeof(blob));
    CHECK(appliance_ssh_identity_blob_from_point_base64(
              point_base64, sizeof(point_base64) - 1U, blob,
              &blob_length, fingerprint) &&
              blob_length == sizeof(hfoc_ssh_qemu_authorized_key) &&
              memcmp(blob, hfoc_ssh_qemu_authorized_key, blob_length) == 0,
          "the 88-character point rebuilds the exact authorized-key blob");
    CHECK(appliance_ssh_identity_fingerprint_text(
              fingerprint, text, sizeof(text)) == 50U &&
              memcmp(text, "SHA256:", 7U) == 0,
          "the enrollment fingerprint is the SHA256: base64 form");
    CHECK(!appliance_ssh_identity_blob_from_point_base64(
              "not-base64", 10U, blob, &blob_length, fingerprint),
          "a malformed enrollment point is refused");
    CHECK(!appliance_ssh_identity_blob_from_point_base64(
              point_base64, sizeof(point_base64) - 9U, blob,
              &blob_length, fingerprint),
          "a truncated enrollment point is refused");
}

static void test_corrupt_host_record_refuses(void)
{
    struct appliance_state_loader loader;
    struct appliance_ssh_identity identity;
    uint8_t *host_record;

    init_loader(&loader);
    memset(&identity, 0, sizeof(identity));
    appliance_ssh_identity_init(&identity);
    CHECK(appliance_ssh_identity_prepare(&identity, &loader, test_rng, NULL) ==
              APPLIANCE_SSH_IDENTITY_OK,
          "corrupt-record fixture starts from a generated host key");
    host_record = fake.storage +
                  (TEST_CERT_OFFSET + APPLIANCE_SSH_CERT_TAIL_BLOCK_OFFSET) *
                      TEST_BLOCK_SIZE;
    host_record[16U] ^= 0x01U;
    memset(&identity, 0, sizeof(identity));
    appliance_ssh_identity_init(&identity);
    CHECK(appliance_ssh_identity_prepare(&identity, &loader, test_rng, NULL) ==
              APPLIANCE_SSH_IDENTITY_RECORD_CORRUPT,
          "a corrupt host record is refused, not regenerated");
    CHECK(!appliance_ssh_identity_host_ready(&identity),
          "a corrupt host record publishes no identity");
}

static void test_write_failure_refuses(void)
{
    struct appliance_state_loader loader;
    struct appliance_ssh_identity identity;

    init_loader(&loader);
    fake.fail_write = true;
    memset(&identity, 0, sizeof(identity));
    appliance_ssh_identity_init(&identity);
    CHECK(appliance_ssh_identity_prepare(&identity, &loader, test_rng, NULL) ==
              APPLIANCE_SSH_IDENTITY_STORE_WRITE_FAILED,
          "a failed host-key write is a named refusal");
    CHECK(!appliance_ssh_identity_host_ready(&identity),
          "a failed write publishes no identity");
}

/*
 * ITEM-606: the installer/medium import path writes an authorized key into an
 * existing loader without generating a host key, then reloads it through the
 * installed system's own reader.
 */
static void test_authorized_key_store_from_loader(void)
{
    struct appliance_state_loader loader;
    struct appliance_ssh_identity reloaded;
    uint8_t client_scalar[32];
    uint8_t client_blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX];
    uint8_t client_fingerprint[32];
    size_t client_blob_length = 0U;
    size_t authorized_length = 0U;
    const uint8_t *authorized;

    init_loader(&loader);
    memset(client_scalar, 0, sizeof(client_scalar));
    memset(client_blob, 0, sizeof(client_blob));
    memset(client_fingerprint, 0, sizeof(client_fingerprint));
    CHECK(hfoc_ssh_crypto_host_key_create(
              client_scalar, client_blob, &client_blob_length,
              client_fingerprint, test_rng, NULL),
          "the fixture creates a real P-256 public blob");
    CHECK(appliance_ssh_identity_authorized_key_store(
              &loader, client_blob, client_blob_length) ==
              APPLIANCE_SSH_IDENTITY_OK,
          "the loader import path writes and reads the key back");
    memset(&reloaded, 0, sizeof(reloaded));
    appliance_ssh_identity_init(&reloaded);
    CHECK(appliance_ssh_identity_prepare(&reloaded, &loader, test_rng, NULL) ==
              APPLIANCE_SSH_IDENTITY_OK &&
              appliance_ssh_identity_authorized_ready(&reloaded),
          "the installed reader reloads the imported authorized key");
    authorized = appliance_ssh_identity_authorized_blob(
        &reloaded, &authorized_length);
    CHECK(authorized != NULL &&
              authorized_length == client_blob_length &&
              memcmp(authorized, client_blob, client_blob_length) == 0,
          "the reloaded key is byte-for-byte the imported public key");
}

/*
 * ITEM-607: a failed durable clear cannot promise a cross-reboot tombstone.
 * The live RAM authorization is revoked; a fresh load still sees the old
 * record.  The console reports exactly that split instead of claiming the
 * record was not cleared.
 */
static void test_failed_durable_remove_is_ram_only(void)
{
    struct appliance_state_loader loader;
    struct appliance_ssh_identity first;
    struct appliance_ssh_identity second;
    uint8_t client_scalar[32];
    uint8_t client_blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX];
    uint8_t client_fingerprint[32];
    size_t client_blob_length = 0U;

    init_loader(&loader);
    memset(&first, 0, sizeof(first));
    appliance_ssh_identity_init(&first);
    CHECK(appliance_ssh_identity_prepare(&first, &loader, test_rng, NULL) ==
              APPLIANCE_SSH_IDENTITY_OK &&
              hfoc_ssh_crypto_host_key_create(
                  client_scalar, client_blob, &client_blob_length,
                  client_fingerprint, test_rng, NULL) &&
              appliance_ssh_identity_authorized_key_add(
                  &first, client_blob, client_blob_length) ==
                  APPLIANCE_SSH_IDENTITY_OK &&
              appliance_ssh_identity_authorized_ready(&first),
          "the revoke fixture enrolls a live authorized key");
    fake.fail_write = true;
    CHECK(appliance_ssh_identity_authorized_key_remove(&first) ==
              APPLIANCE_SSH_IDENTITY_STORE_WRITE_FAILED,
          "a failed durable clear is a named write failure");
    CHECK(!appliance_ssh_identity_authorized_ready(&first),
          "the failed clear still revokes the live RAM authorization");
    fake.fail_write = false;
    memset(&second, 0, sizeof(second));
    appliance_ssh_identity_init(&second);
    CHECK(appliance_ssh_identity_prepare(&second, &loader, test_rng, NULL) ==
              APPLIANCE_SSH_IDENTITY_OK &&
              appliance_ssh_identity_authorized_ready(&second),
          "a fresh load proves the old key was not durably tombstoned");
}

int main(void)
{
    test_generate_reload_and_authorized_key();
    test_enrollment_point_text();
    test_corrupt_host_record_refuses();
    test_write_failure_refuses();
    test_authorized_key_store_from_loader();
    test_failed_durable_remove_is_ram_only();
    if (failures != 0) {
        fprintf(stderr, "SSH identity host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("SSH identity host tests: ALL OK\n");
    return 0;
}

#else
typedef int hfoc_appliance_ssh_identity_host_translation_unit_guard;
#endif
