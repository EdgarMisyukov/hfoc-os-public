#include "efi_boot_environment.h"
#include "efi_boot_variables.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_EFI_BOOT_VARIABLES_HOST_TEST

#define FIXTURE_VARIABLE_CAPACITY 64U
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
static struct block_device environment_block_device;

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

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

static void fixture_init(struct variable_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
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

static enum block_device_status fixture_block_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    (void)offset;
    (void)buffer;
    (void)length;
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_block_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    (void)offset;
    (void)buffer;
    (void)length;
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_block_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct block_device make_block_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = 512U;
    device.block_count = 1U;
    device.read_fn = fixture_block_read;
    device.write_fn = fixture_block_write;
    device.flush_fn = fixture_block_flush;
    return device;
}

static bool fixture_environment(
    struct variable_fixture *fixture,
    struct efi_boot_environment *environment)
{
    memset(environment, 0, sizeof(*environment));
    environment_block_device = make_block_device();
    return efi_boot_environment_init(
        environment, EFI_BOOT_FIRMWARE_UEFI,
        &environment_block_device,
        fixture, fixture_read_variable,
        fixture_write_variable) == EFI_BOOT_ENVIRONMENT_OK &&
        efi_boot_environment_set_device_path(
            environment, test_device_path,
            sizeof(test_device_path)) ==
            EFI_BOOT_ENVIRONMENT_OK;
}

static void fixture_install_unrelated(
    struct variable_fixture *fixture)
{
    unsigned char linux_data[64U];
    unsigned char windows_data[80U];
    uint16_t order[2U] = { 1U, 2U };

    memset(linux_data, 0x11U, sizeof(linux_data));
    memset(windows_data, 0x22U, sizeof(windows_data));
    fixture_add_boot(fixture, 1U, linux_data, sizeof(linux_data));
    fixture_add_boot(
        fixture, 2U, windows_data, sizeof(windows_data));
    fixture_set_order(fixture, order, 2U);
}

static void test_empty_and_preservation_lifecycle(void)
{
    struct variable_fixture fixture;
    struct efi_boot_environment environment;
    struct efi_boot_variables_snapshot snapshot;
    struct efi_boot_variables_snapshot after;
    struct efi_boot_variables_plan plan;
    unsigned char linux_before[64U];
    unsigned char linux_after[64U];
    unsigned char windows_before[80U];
    unsigned char windows_after[80U];
    unsigned char read_data[512U];
    size_t read_length = 0U;
    uint16_t expected_order[3U] = { 0U, 1U, 2U };
    size_t index;

    fixture_init(&fixture);
    CHECK(fixture_environment(&fixture, &environment),
          "boot variable fixture environment initializes");
    CHECK(efi_boot_variables_snapshot(
              &environment, &snapshot) ==
              EFI_BOOT_VARIABLES_OK &&
              !snapshot.boot_order_present &&
              snapshot.entry_count == 0U,
          "empty fixture snapshots no boot variables");
    memset(&plan, 0, sizeof(plan));
    CHECK(efi_boot_variables_plan(
              &environment, &plan) ==
              EFI_BOOT_VARIABLES_OK &&
              plan.version == EFI_BOOT_VARIABLES_VERSION &&
              !plan.hfoc_present &&
              plan.hfoc_number == 0U &&
              plan.hfoc_data_length > 0U,
          "empty plan selects deterministic HFOC boot number");
    CHECK(memcmp(
              plan.hfoc_data + 22U, test_device_path,
              sizeof(test_device_path) - 4U) == 0,
          "plan includes the caller-owned boot device path");
    CHECK(efi_boot_variables_apply(
              &environment, &snapshot, &plan) ==
              EFI_BOOT_VARIABLES_NOT_CONFIRMED &&
              fixture.variable_count == 0U,
          "unconfirmed plan does not mutate variables");
    CHECK(efi_boot_variables_confirm(&plan) ==
              EFI_BOOT_VARIABLES_OK,
          "plan confirms");
    CHECK(efi_boot_variables_apply(
              &environment, &snapshot, &plan) ==
              EFI_BOOT_VARIABLES_OK,
          "confirmed empty plan applies");
    CHECK(efi_boot_variables_verify(
              &environment, &plan) ==
              EFI_BOOT_VARIABLES_OK,
          "empty plan verifies HFOC BootOrder entry");

    fixture_init(&fixture);
    fixture_install_unrelated(&fixture);
    fixture_get_boot(
        &fixture, 1U, linux_before, sizeof(linux_before),
        &read_length);
    fixture_get_boot(
        &fixture, 2U, windows_before, sizeof(windows_before),
        &read_length);
    CHECK(fixture_environment(&fixture, &environment) &&
          efi_boot_variables_snapshot(
              &environment, &snapshot) ==
              EFI_BOOT_VARIABLES_OK &&
              snapshot.boot_order_length == 4U &&
              snapshot.entry_count == 2U,
          "snapshot captures unrelated BootOrder and entries");
    memset(&plan, 0, sizeof(plan));
    CHECK(efi_boot_variables_plan(
              &environment, &plan) ==
              EFI_BOOT_VARIABLES_OK &&
              plan.hfoc_number == 0U &&
              plan.boot_order_change_required &&
              plan.hfoc_entry_change_required,
          "preservation plan adds HFOC at front");
    CHECK(efi_boot_variables_confirm(&plan) ==
              EFI_BOOT_VARIABLES_OK &&
          efi_boot_variables_apply(
              &environment, &snapshot, &plan) ==
              EFI_BOOT_VARIABLES_OK &&
          efi_boot_variables_verify(
              &environment, &plan) ==
              EFI_BOOT_VARIABLES_OK,
          "preservation apply/verify succeeds");
    CHECK(efi_boot_variables_snapshot(
              &environment, &after) ==
              EFI_BOOT_VARIABLES_OK &&
              after.boot_order_length == 6U,
          "after snapshot contains three BootOrder entries");
    for (index = 0U; index < 3U; ++index) {
        CHECK((uint16_t)(
                  after.boot_order[index * 2U] |
                  ((uint16_t)after.boot_order[index * 2U + 1U] <<
                   8U)) == expected_order[index],
              "BootOrder preserves unrelated entries after HFOC");
    }
    read_length = 0U;
    CHECK(fixture_get_boot(
              &fixture, 0U, read_data, sizeof(read_data),
              &read_length) &&
              read_length == plan.hfoc_data_length &&
              memcmp(read_data, plan.hfoc_data,
                     read_length) == 0,
          "HFOC load option is written exactly");
    read_length = 0U;
    CHECK(fixture_get_boot(
              &fixture, 1U, linux_after, sizeof(linux_after),
              &read_length) &&
              read_length == sizeof(linux_before) &&
              memcmp(linux_after, linux_before,
                     sizeof(linux_before)) == 0,
          "Linux Boot0001 data is preserved byte-for-byte");
    read_length = 0U;
    CHECK(fixture_get_boot(
              &fixture, 2U, windows_after, sizeof(windows_after),
              &read_length) &&
              read_length == sizeof(windows_before) &&
              memcmp(windows_after, windows_before,
                     sizeof(windows_before)) == 0,
          "Windows Boot0002 data is preserved byte-for-byte");
}

static void test_existing_hfoc_and_outside_order(void)
{
    struct variable_fixture fixture;
    struct efi_boot_environment environment;
    struct efi_boot_variables_snapshot snapshot;
    struct efi_boot_variables_snapshot after;
    struct efi_boot_variables_plan plan;
    unsigned char canonical[512U];
    size_t canonical_length = 0U;
    uint16_t linux_windows[2U] = { 1U, 2U };
    uint16_t in_order[3U] = { 1U, 0U, 2U };
    size_t index;

    fixture_init(&fixture);
    CHECK(fixture_environment(&fixture, &environment) &&
          efi_boot_variables_plan(
              &environment, &plan) ==
              EFI_BOOT_VARIABLES_OK,
          "empty fixture produces canonical plan");
    canonical_length = plan.hfoc_data_length;
    memcpy(canonical, plan.hfoc_data, canonical_length);

    fixture_init(&fixture);
    fixture_install_unrelated(&fixture);
    fixture_add_boot(
        &fixture, 0U, canonical, canonical_length);
    fixture_set_order(&fixture, linux_windows, 2U);
    CHECK(fixture_environment(&fixture, &environment) &&
          efi_boot_variables_snapshot(
              &environment, &snapshot) ==
              EFI_BOOT_VARIABLES_OK &&
          efi_boot_variables_plan(
              &environment, &plan) ==
              EFI_BOOT_VARIABLES_OK &&
              plan.hfoc_present &&
              plan.hfoc_number == 0U &&
              !plan.hfoc_in_boot_order &&
              plan.boot_order_change_required,
          "existing HFOC outside BootOrder is detected and queued");
    CHECK(efi_boot_variables_confirm(&plan) ==
              EFI_BOOT_VARIABLES_OK &&
          efi_boot_variables_apply(
              &environment, &snapshot, &plan) ==
              EFI_BOOT_VARIABLES_OK &&
          efi_boot_variables_verify(
              &environment, &plan) ==
              EFI_BOOT_VARIABLES_OK,
          "outside-order HFOC is inserted at front and verified");
    CHECK(efi_boot_variables_snapshot(
              &environment, &after) ==
              EFI_BOOT_VARIABLES_OK &&
              after.boot_order_length == 6U &&
              (uint16_t)(
                  after.boot_order[0U] |
                  ((uint16_t)after.boot_order[1U] << 8U)) == 0U,
          "outside-order update places HFOC before unrelated entries");

    fixture_init(&fixture);
    fixture_install_unrelated(&fixture);
    fixture_add_boot(
        &fixture, 0U, canonical, canonical_length);
    fixture_set_order(&fixture, in_order, 3U);
    CHECK(fixture_environment(&fixture, &environment) &&
          efi_boot_variables_snapshot(
              &environment, &snapshot) ==
              EFI_BOOT_VARIABLES_OK &&
          efi_boot_variables_plan(
              &environment, &plan) ==
              EFI_BOOT_VARIABLES_OK &&
              plan.hfoc_present &&
              plan.hfoc_in_boot_order &&
              !plan.boot_order_change_required &&
              !plan.hfoc_entry_change_required,
          "existing in-order HFOC requires no mutation");
    CHECK(efi_boot_variables_confirm(&plan) ==
              EFI_BOOT_VARIABLES_OK &&
          efi_boot_variables_apply(
              &environment, &snapshot, &plan) ==
              EFI_BOOT_VARIABLES_OK &&
          efi_boot_variables_verify(
              &environment, &plan) ==
              EFI_BOOT_VARIABLES_OK,
          "in-order HFOC apply/verify succeeds without reorder");
    for (index = 0U; index < 3U; ++index) {
        CHECK((uint16_t)(
                  plan.before_order[index * 2U] |
                  ((uint16_t)plan.before_order[index * 2U + 1U] <<
                   8U)) == in_order[index],
              "in-order HFOC leaves unrelated BootOrder order stable");
    }
}

static void test_failure_semantics(void)
{
    struct variable_fixture fixture;
    struct efi_boot_environment environment;
    struct efi_boot_variables_snapshot snapshot;
    struct efi_boot_variables_plan plan;
    uint16_t order[2U] = { 0U, 1U };
    unsigned char dummy[16U];

    memset(dummy, 0x33U, sizeof(dummy));
    fixture_init(&fixture);
    fixture_add_boot(&fixture, 0U, dummy, sizeof(dummy));
    fixture_set_order(&fixture, order, 2U);
    CHECK(fixture_environment(&fixture, &environment) &&
          efi_boot_variables_snapshot(
              &environment, &snapshot) ==
              EFI_BOOT_VARIABLES_VARIABLE_UNAVAILABLE,
          "snapshot fails closed when BootOrder misses Boot0001");

    fixture_init(&fixture);
    fixture_install_unrelated(&fixture);
    CHECK(fixture_environment(&fixture, &environment) &&
          efi_boot_variables_snapshot(
              &environment, &snapshot) ==
              EFI_BOOT_VARIABLES_OK &&
          efi_boot_variables_plan(
              &environment, &plan) ==
              EFI_BOOT_VARIABLES_OK &&
          efi_boot_variables_confirm(&plan) ==
              EFI_BOOT_VARIABLES_OK,
          "write-failure fixture builds confirmed plan");
    fixture.fail_next_write = true;
    CHECK(efi_boot_variables_apply(
              &environment, &snapshot, &plan) ==
              EFI_BOOT_VARIABLES_MUTATION_FAILED,
          "variable write failure is reported");
    CHECK(!fixture_get_boot(
              &fixture, 0U, dummy, sizeof(dummy),
              &(size_t){ 0U }),
          "failed Boot#### write does not create HFOC entry");

    fixture.fail_read = true;
    CHECK(efi_boot_variables_snapshot(
              &environment, &snapshot) ==
              EFI_BOOT_VARIABLES_VARIABLE_IO_ERROR,
          "variable read failure is reported");
}

static void test_status_strings(void)
{
    CHECK(strcmp(efi_boot_variables_status_string(
                     EFI_BOOT_VARIABLES_OK),
                 "EFI_BOOT_VARIABLES_OK") == 0,
          "boot variables OK status string is exact");
    CHECK(strcmp(efi_boot_variables_status_string(
                     EFI_BOOT_VARIABLES_MUTATION_FAILED),
                 "EFI_BOOT_VARIABLES_MUTATION_FAILED") == 0,
          "boot variables mutation-failed string is exact");
    CHECK(efi_boot_variables_status_string(
              (enum efi_boot_variables_status)99U) == NULL,
          "undefined boot variables status string is rejected");
}

int main(void)
{
    test_empty_and_preservation_lifecycle();
    test_existing_hfoc_and_outside_order();
    test_failure_semantics();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr,
                "EFI boot variables host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("EFI boot variables host tests: ALL OK");
    return 0;
}

#endif
