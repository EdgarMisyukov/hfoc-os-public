#include "virtio_pci_feature.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_PCI_FEATURE_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

struct test_device {
    uint32_t offered_low;
    uint32_t offered_high;
    uint32_t accepted_low;
    uint32_t accepted_high;
    bool read_offered_fail;
    bool write_accepted_fail;
    bool write_accepted_fail_high;
    bool read_accepted_fail;
    bool read_accepted_override;
    uint32_t read_accepted_override_low;
    uint32_t read_accepted_override_high;
    unsigned int write_log_count;
    uint32_t write_log_value[8];
    uint32_t write_log_select[8];
};

static bool test_read_offered(void *context, uint32_t select, uint32_t *value)
{
    struct test_device *device = (struct test_device *)context;

    if (device->read_offered_fail) {
        return false;
    }
    if (select == VIRTIO_PCI_FEATURE_SELECT_LO) {
        *value = device->offered_low;
    } else {
        *value = device->offered_high;
    }
    return true;
}

static bool test_write_accepted(
    void *context,
    uint32_t select,
    uint32_t value)
{
    struct test_device *device = (struct test_device *)context;

    if (device->write_accepted_fail ||
        (device->write_accepted_fail_high &&
         select == VIRTIO_PCI_FEATURE_SELECT_HI)) {
        return false;
    }
    if (device->write_log_count <
        sizeof(device->write_log_value) / sizeof(device->write_log_value[0])) {
        device->write_log_value[device->write_log_count] = value;
        device->write_log_select[device->write_log_count] = select;
        ++device->write_log_count;
    }
    if (select == VIRTIO_PCI_FEATURE_SELECT_LO) {
        device->accepted_low = value;
    } else {
        device->accepted_high = value;
    }
    return true;
}

static bool test_read_accepted(
    void *context,
    uint32_t select,
    uint32_t *value)
{
    struct test_device *device = (struct test_device *)context;

    if (device->read_accepted_fail) {
        return false;
    }
    if (device->read_accepted_override) {
        if (select == VIRTIO_PCI_FEATURE_SELECT_LO) {
            *value = device->read_accepted_override_low;
        } else {
            *value = device->read_accepted_override_high;
        }
        return true;
    }
    if (select == VIRTIO_PCI_FEATURE_SELECT_LO) {
        *value = device->accepted_low;
    } else {
        *value = device->accepted_high;
    }
    return true;
}

static void init_device(struct test_device *device) {
    memset(device, 0, sizeof(*device));
}

static void make_io(
    struct virtio_pci_feature_io *io,
    struct test_device *device)
{
    io->read_offered = test_read_offered;
    io->write_accepted = test_write_accepted;
    io->read_accepted = test_read_accepted;
    io->context = device;
}

static void init_controller(
    struct virtio_pci_feature_controller *controller,
    struct virtio_pci_feature_io *io,
    struct test_device *device)
{
    memset(controller, 0, sizeof(*controller));
    make_io(io, device);
}

static struct virtio_pci_feature_bitset make_bitset(
    uint32_t low,
    uint32_t high)
{
    struct virtio_pci_feature_bitset bitset;

    bitset.low = low;
    bitset.high = high;
    return bitset;
}

static bool bitset_equal(
    const struct virtio_pci_feature_bitset *left,
    const struct virtio_pci_feature_bitset *right)
{
    return left->low == right->low && left->high == right->high;
}

static bool feature_status_string_matches(
    enum virtio_pci_feature_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_PCI_FEATURE_OK:
        return strcmp(string, "VIRTIO_PCI_FEATURE_OK") == 0;
    case VIRTIO_PCI_FEATURE_NULL:
        return strcmp(string, "VIRTIO_PCI_FEATURE_NULL") == 0;
    case VIRTIO_PCI_FEATURE_INVALID_VERSION:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_INVALID_VERSION") == 0;
    case VIRTIO_PCI_FEATURE_INVALID_GENERATION:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_INVALID_GENERATION") == 0;
    case VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED") == 0;
    case VIRTIO_PCI_FEATURE_UNPUBLISHED:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_UNPUBLISHED") == 0;
    case VIRTIO_PCI_FEATURE_INVALID_IO:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_INVALID_IO") == 0;
    case VIRTIO_PCI_FEATURE_IO_FAILED:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_IO_FAILED") == 0;
    case VIRTIO_PCI_FEATURE_UNKNOWN_REQUIRED:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_UNKNOWN_REQUIRED") == 0;
    case VIRTIO_PCI_FEATURE_UNKNOWN_ACCEPTED:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_UNKNOWN_ACCEPTED") == 0;
    case VIRTIO_PCI_FEATURE_MISSING_REQUIRED:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_MISSING_REQUIRED") == 0;
    case VIRTIO_PCI_FEATURE_REQUIRED_NOT_ACCEPTED:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_REQUIRED_NOT_ACCEPTED") == 0;
    case VIRTIO_PCI_FEATURE_UNOFFERED_ACCEPTED:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_UNOFFERED_ACCEPTED") == 0;
    case VIRTIO_PCI_FEATURE_MISSING_DEPENDENCY:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_MISSING_DEPENDENCY") == 0;
    case VIRTIO_PCI_FEATURE_INVALID_READBACK:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_INVALID_READBACK") == 0;
    case VIRTIO_PCI_FEATURE_CHANGED:
        return strcmp(
            string,
            "VIRTIO_PCI_FEATURE_CHANGED") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void) {
    static const enum virtio_pci_feature_status statuses[] = {
        VIRTIO_PCI_FEATURE_OK,
        VIRTIO_PCI_FEATURE_NULL,
        VIRTIO_PCI_FEATURE_INVALID_VERSION,
        VIRTIO_PCI_FEATURE_INVALID_GENERATION,
        VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED,
        VIRTIO_PCI_FEATURE_UNPUBLISHED,
        VIRTIO_PCI_FEATURE_INVALID_IO,
        VIRTIO_PCI_FEATURE_IO_FAILED,
        VIRTIO_PCI_FEATURE_UNKNOWN_REQUIRED,
        VIRTIO_PCI_FEATURE_UNKNOWN_ACCEPTED,
        VIRTIO_PCI_FEATURE_MISSING_REQUIRED,
        VIRTIO_PCI_FEATURE_REQUIRED_NOT_ACCEPTED,
        VIRTIO_PCI_FEATURE_UNOFFERED_ACCEPTED,
        VIRTIO_PCI_FEATURE_MISSING_DEPENDENCY,
        VIRTIO_PCI_FEATURE_INVALID_READBACK,
        VIRTIO_PCI_FEATURE_CHANGED,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_pci_feature_status status = statuses[index];
        const char *string = virtio_pci_feature_status_string(status);

        CHECK(string != NULL && feature_status_string_matches(status, string),
              "virtio feature status string is exact");
    }
    CHECK(virtio_pci_feature_status_string(
              (enum virtio_pci_feature_status)16U) == NULL,
          "virtio feature undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio feature status handling performs no allocation");
}

static void test_init_and_null(void) {
    struct virtio_pci_feature_controller controller;
    struct virtio_pci_feature_io io;
    struct virtio_pci_feature_io bad_io;
    struct test_device device;
    struct virtio_pci_feature_bitset accepted;
    struct virtio_pci_feature_policy policy;

    init_device(&device);
    make_io(&io, &device);
    init_controller(&controller, &io, &device);

    CHECK(virtio_pci_feature_init(NULL, &io, 1U) ==
              VIRTIO_PCI_FEATURE_NULL,
          "feature init rejects null controller");
    CHECK(virtio_pci_feature_init(&controller, NULL, 1U) ==
              VIRTIO_PCI_FEATURE_NULL,
          "feature init rejects null I/O");

    bad_io = io;
    bad_io.read_offered = NULL;
    CHECK(virtio_pci_feature_init(&controller, &bad_io, 1U) ==
              VIRTIO_PCI_FEATURE_INVALID_IO,
          "feature init rejects null offered-read callback");
    bad_io = io;
    bad_io.write_accepted = NULL;
    CHECK(virtio_pci_feature_init(&controller, &bad_io, 1U) ==
              VIRTIO_PCI_FEATURE_INVALID_IO,
          "feature init rejects null accepted-write callback");
    bad_io = io;
    bad_io.read_accepted = NULL;
    CHECK(virtio_pci_feature_init(&controller, &bad_io, 1U) ==
              VIRTIO_PCI_FEATURE_INVALID_IO,
          "feature init rejects null accepted-read callback");
    bad_io = io;
    bad_io.context = NULL;
    CHECK(virtio_pci_feature_init(&controller, &bad_io, 1U) ==
              VIRTIO_PCI_FEATURE_INVALID_IO,
          "feature init rejects null I/O context");

    memset(&controller, 0, sizeof(controller));
    controller.version = VIRTIO_PCI_FEATURE_VERSION + 1U;
    CHECK(virtio_pci_feature_init(&controller, &io, 1U) ==
              VIRTIO_PCI_FEATURE_INVALID_VERSION,
          "feature init rejects nonzero internal version");
    memset(&controller, 0, sizeof(controller));
    CHECK(virtio_pci_feature_init(&controller, &io, 0U) ==
              VIRTIO_PCI_FEATURE_INVALID_GENERATION,
          "feature init rejects zero generation");
    CHECK(virtio_pci_feature_init(
              &controller, &io,
              VIRTIO_PCI_FEATURE_GENERATION_MAX + 1U) ==
              VIRTIO_PCI_FEATURE_INVALID_GENERATION,
          "feature init rejects over-limit generation");

    CHECK(virtio_pci_feature_init(&controller, &io, 7U) ==
              VIRTIO_PCI_FEATURE_OK,
          "feature init publishes initialized controller");
    CHECK(controller.version == VIRTIO_PCI_FEATURE_VERSION &&
              controller.generation == 7U &&
              controller.offered.low == 0U &&
              controller.offered.high == 0U &&
              controller.accepted.low == 0U &&
              controller.accepted.high == 0U &&
              !controller.published,
          "feature init clears offered/accepted and stays unpublished");
    CHECK(device.write_log_count == 0U,
          "feature init performs no I/O");
    CHECK(virtio_pci_feature_init(&controller, &io, 7U) ==
              VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED,
          "feature init rejects duplicate publication");

    CHECK(virtio_pci_feature_negotiate(NULL, &policy) ==
              VIRTIO_PCI_FEATURE_NULL,
          "feature negotiate rejects null controller");
    CHECK(virtio_pci_feature_negotiate(&controller, NULL) ==
              VIRTIO_PCI_FEATURE_NULL,
          "feature negotiate rejects null policy");
    CHECK(virtio_pci_feature_observe(NULL, &accepted) ==
              VIRTIO_PCI_FEATURE_NULL,
          "feature observe rejects null controller");
    CHECK(virtio_pci_feature_observe(&controller, NULL) ==
              VIRTIO_PCI_FEATURE_NULL,
          "feature observe rejects null output");
    CHECK(!virtio_pci_feature_published(NULL),
          "feature published rejects null controller");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "feature init and null checks perform no allocation");
}

static void test_valid_negotiation(void) {
    struct virtio_pci_feature_controller controller;
    struct virtio_pci_feature_io io;
    struct test_device device;
    struct virtio_pci_feature_policy policy;
    struct virtio_pci_feature_bitset expected_offered;
    struct virtio_pci_feature_bitset expected_accepted;
    struct virtio_pci_feature_bitset observed;

    init_device(&device);
    device.offered_low = VIRTIO_PCI_FEATURE_KNOWN_LO_MASK;
    device.offered_high = UINT32_C(1) <<
        (VIRTIO_PCI_FEATURE_VERSION_1 - 32U);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_feature_init(&controller, &io, 4U) ==
              VIRTIO_PCI_FEATURE_OK,
          "valid negotiation init succeeds");

    expected_offered = make_bitset(
        VIRTIO_PCI_FEATURE_KNOWN_LO_MASK,
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U));
    expected_accepted = make_bitset(
        UINT32_C(1) << VIRTIO_PCI_FEATURE_RING_INDIRECT_DESC,
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U));
    policy.required = make_bitset(
        UINT32_C(1) << VIRTIO_PCI_FEATURE_RING_INDIRECT_DESC,
        0U);
    policy.accepted = expected_accepted;

    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_OK,
          "valid negotiation succeeds");
    CHECK(controller.published &&
              bitset_equal(&controller.offered, &expected_offered) &&
              bitset_equal(&controller.accepted, &expected_accepted),
          "valid negotiation publishes exact offered/accepted sets");
    CHECK(device.write_log_count == 2U &&
              device.write_log_value[0] == expected_accepted.low &&
              device.write_log_select[0] ==
                  VIRTIO_PCI_FEATURE_SELECT_LO &&
              device.write_log_value[1] == expected_accepted.high &&
              device.write_log_select[1] ==
                  VIRTIO_PCI_FEATURE_SELECT_HI,
          "valid negotiation writes both accepted words");
    CHECK(device.accepted_low == expected_accepted.low &&
              device.accepted_high == expected_accepted.high,
          "valid negotiation publishes accepted words to I/O seam");
    CHECK(virtio_pci_feature_published(&controller),
          "published controller is reported published");
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED,
          "valid negotiation rejects duplicate negotiation");
    CHECK(virtio_pci_feature_init(&controller, &io, 4U) ==
              VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED,
          "published controller rejects duplicate init");

    CHECK(virtio_pci_feature_observe(&controller, &observed) ==
              VIRTIO_PCI_FEATURE_OK &&
              bitset_equal(&observed, &expected_accepted),
          "observe reports exact accepted words");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid negotiation performs no allocation");
}

static void test_policy_violations(void) {
    struct virtio_pci_feature_controller controller;
    struct virtio_pci_feature_io io;
    struct test_device device;
    struct virtio_pci_feature_policy policy;
    const uint32_t version_bit =
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U);
    const uint32_t ring_bit =
        UINT32_C(1) << VIRTIO_PCI_FEATURE_RING_INDIRECT_DESC;

    init_device(&device);
    device.offered_low = ring_bit;
    device.offered_high = version_bit |
        (UINT32_C(1) << (VIRTIO_PCI_FEATURE_ACCESS_PLATFORM - 32U));
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_feature_init(&controller, &io, 4U) ==
              VIRTIO_PCI_FEATURE_OK,
          "policy violation fixture init succeeds");

    policy.required = make_bitset(0U, UINT32_C(0x40000000));
    policy.accepted = make_bitset(0U, 0U);
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_UNKNOWN_REQUIRED &&
              !controller.published && device.write_log_count == 0U,
          "unknown required bit fails closed");

    policy.required = make_bitset(0U, 0U);
    policy.accepted = make_bitset(0U, UINT32_C(0x40000000));
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_UNKNOWN_ACCEPTED &&
              !controller.published && device.write_log_count == 0U,
          "unknown accepted bit fails closed");

    policy.required = make_bitset(
        ring_bit |
        (UINT32_C(1) << VIRTIO_PCI_FEATURE_RING_EVENT_IDX),
        0U);
    policy.accepted = make_bitset(0U, 0U);
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_MISSING_REQUIRED &&
              !controller.published && device.write_log_count == 0U,
          "missing mandatory offered bit fails closed");

    policy.required = make_bitset(ring_bit, 0U);
    policy.accepted = make_bitset(0U, version_bit);
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_REQUIRED_NOT_ACCEPTED &&
              !controller.published && device.write_log_count == 0U,
          "required bit not accepted fails closed");

    policy.required = make_bitset(0U, 0U);
    policy.accepted = make_bitset(ring_bit |
        (UINT32_C(1) << VIRTIO_PCI_FEATURE_RING_EVENT_IDX), version_bit);
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_UNOFFERED_ACCEPTED &&
              !controller.published && device.write_log_count == 0U,
          "unoffered accepted bit fails closed");

    policy.required = make_bitset(0U, 0U);
    policy.accepted = make_bitset(
        0U,
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_ACCESS_PLATFORM - 32U));
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_MISSING_DEPENDENCY &&
              !controller.published && device.write_log_count == 0U,
          "missing VERSION_1 dependency fails closed");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "policy violations perform no allocation");
}

static void test_io_failures_and_rollback(void) {
    struct virtio_pci_feature_controller controller;
    struct virtio_pci_feature_io io;
    struct test_device device;
    struct virtio_pci_feature_policy policy;
    const uint32_t version_bit =
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U);
    const uint32_t ring_bit =
        UINT32_C(1) << VIRTIO_PCI_FEATURE_RING_INDIRECT_DESC;

    policy.required = make_bitset(ring_bit, 0U);
    policy.accepted = make_bitset(ring_bit, version_bit);

    init_device(&device);
    device.offered_low = ring_bit;
    device.offered_high = version_bit;
    device.read_offered_fail = true;
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_feature_init(&controller, &io, 4U) ==
              VIRTIO_PCI_FEATURE_OK,
          "offered-read failure fixture init succeeds");
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_IO_FAILED &&
              !controller.published && device.write_log_count == 0U,
          "offered-read failure leaves controller unpublished");

    init_device(&device);
    device.offered_low = ring_bit;
    device.offered_high = version_bit;
    device.write_accepted_fail_high = true;
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_feature_init(&controller, &io, 4U) ==
              VIRTIO_PCI_FEATURE_OK,
          "accepted-write failure fixture init succeeds");
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_IO_FAILED &&
              !controller.published &&
              device.write_log_count == 2U &&
              device.write_log_value[0] == ring_bit &&
              device.write_log_value[1] == 0U &&
              device.accepted_low == 0U,
          "high accepted-write failure rolls back low word");

    init_device(&device);
    device.offered_low = ring_bit;
    device.offered_high = version_bit;
    device.read_accepted_fail = true;
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_feature_init(&controller, &io, 4U) ==
              VIRTIO_PCI_FEATURE_OK,
          "accepted-read failure fixture init succeeds");
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_IO_FAILED &&
              !controller.published &&
              device.write_log_count == 4U &&
              device.write_log_value[2] == 0U &&
              device.write_log_value[3] == 0U &&
              device.accepted_low == 0U &&
              device.accepted_high == 0U,
          "accepted-read failure rolls back both accepted words");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IO failure paths perform no allocation");
}

static void test_readback_mismatch_and_observe(void) {
    struct virtio_pci_feature_controller controller;
    struct virtio_pci_feature_io io;
    struct test_device device;
    struct virtio_pci_feature_policy policy;
    struct virtio_pci_feature_bitset observed;
    const uint32_t version_bit =
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U);
    const uint32_t ring_bit =
        UINT32_C(1) << VIRTIO_PCI_FEATURE_RING_INDIRECT_DESC;

    policy.required = make_bitset(ring_bit, 0U);
    policy.accepted = make_bitset(ring_bit, version_bit);

    init_device(&device);
    device.offered_low = ring_bit;
    device.offered_high = version_bit;
    device.read_accepted_override = true;
    device.read_accepted_override_low = ring_bit ^
        (UINT32_C(1) << VIRTIO_PCI_FEATURE_RING_EVENT_IDX);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_feature_init(&controller, &io, 4U) ==
              VIRTIO_PCI_FEATURE_OK,
          "readback mismatch fixture init succeeds");
    CHECK(virtio_pci_feature_negotiate(&controller, &policy) ==
              VIRTIO_PCI_FEATURE_INVALID_READBACK &&
              !controller.published &&
              device.write_log_count == 4U &&
              device.write_log_value[2] == 0U &&
              device.write_log_value[3] == 0U &&
              device.accepted_low == 0U &&
              device.accepted_high == 0U,
          "readback mismatch fails closed and rolls back accepted words");

    init_device(&device);
    device.offered_low = ring_bit;
    device.offered_high = version_bit;
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_feature_init(&controller, &io, 4U) ==
              VIRTIO_PCI_FEATURE_OK &&
              virtio_pci_feature_negotiate(&controller, &policy) ==
                  VIRTIO_PCI_FEATURE_OK,
          "observe fixture negotiates");
    CHECK(virtio_pci_feature_observe(&controller, &observed) ==
              VIRTIO_PCI_FEATURE_OK &&
              bitset_equal(&observed, &policy.accepted),
          "observe reports unchanged accepted words");
    device.accepted_low = 0U;
    CHECK(virtio_pci_feature_observe(&controller, &observed) ==
              VIRTIO_PCI_FEATURE_CHANGED &&
              observed.low == 0U,
          "observe reports changed accepted words");

    memset(&controller, 0, sizeof(controller));
    controller.version = VIRTIO_PCI_FEATURE_VERSION;
    controller.generation = 1U;
    controller.io = io;
    CHECK(virtio_pci_feature_observe(&controller, &observed) ==
              VIRTIO_PCI_FEATURE_UNPUBLISHED,
          "observe rejects unpublished controller");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "readback and observe paths perform no allocation");
}

int main(void) {
    test_status_strings();
    test_init_and_null();
    test_valid_negotiation();
    test_policy_violations();
    test_io_failures_and_rollback();
    test_readback_mismatch_and_observe();

    if (failures != 0) {
        fprintf(stderr, "Virtio PCI feature host tests: FAIL (%d)\n",
                failures);
        return 1;
    }
    printf("Virtio PCI feature host tests: ALL OK\n");
    return 0;
}

#else
#error "This file must be compiled with HFOC_VIRTIO_PCI_FEATURE_HOST_TEST"
#endif
