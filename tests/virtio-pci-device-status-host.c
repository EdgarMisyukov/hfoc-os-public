#include "virtio_pci_device_status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_PCI_DEVICE_STATUS_HOST_TEST

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
    uint8_t register_value;
    bool write_fail;
    bool read_fail;
    bool read_override;
    uint8_t read_override_value;
    unsigned int write_count;
    unsigned int read_count;
    uint8_t last_write;
};

static bool test_write(void *context, uint8_t value) {
    struct test_device *device = (struct test_device *)context;

    if (device->write_fail) {
        return false;
    }
    device->register_value = value;
    device->last_write = value;
    ++device->write_count;
    return true;
}

static bool test_read(void *context, uint8_t *value) {
    struct test_device *device = (struct test_device *)context;

    if (device->read_fail) {
        return false;
    }
    *value = device->read_override ?
                 device->read_override_value : device->register_value;
    ++device->read_count;
    return true;
}

static void init_device(struct test_device *device) {
    memset(device, 0, sizeof(*device));
}

static void make_io(
    struct virtio_pci_device_status_io *io,
    struct test_device *device)
{
    io->write = test_write;
    io->read = test_read;
    io->context = device;
}

static void init_controller(
    struct virtio_pci_device_status_controller *controller,
    struct virtio_pci_device_status_io *io,
    struct test_device *device)
{
    memset(controller, 0, sizeof(*controller));
    make_io(io, device);
}

static bool status_string_matches(
    enum virtio_pci_device_status_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_PCI_DEVICE_STATUS_OK:
        return strcmp(string, "VIRTIO_PCI_DEVICE_STATUS_OK") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_NULL:
        return strcmp(string, "VIRTIO_PCI_DEVICE_STATUS_NULL") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_GENERATION:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_INVALID_GENERATION") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_ALREADY_PUBLISHED:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_ALREADY_PUBLISHED") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_STATE:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_INVALID_STATE") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_IO:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_INVALID_IO") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_IO_FAILED:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_IO_FAILED") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_READBACK:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_INVALID_READBACK") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_FAILED:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_FAILED") == 0;
    case VIRTIO_PCI_DEVICE_STATUS_CHANGED:
        return strcmp(
            string,
            "VIRTIO_PCI_DEVICE_STATUS_CHANGED") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void) {
    static const enum virtio_pci_device_status_status statuses[] = {
        VIRTIO_PCI_DEVICE_STATUS_OK,
        VIRTIO_PCI_DEVICE_STATUS_NULL,
        VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION,
        VIRTIO_PCI_DEVICE_STATUS_INVALID_GENERATION,
        VIRTIO_PCI_DEVICE_STATUS_ALREADY_PUBLISHED,
        VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED,
        VIRTIO_PCI_DEVICE_STATUS_INVALID_STATE,
        VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION,
        VIRTIO_PCI_DEVICE_STATUS_INVALID_IO,
        VIRTIO_PCI_DEVICE_STATUS_IO_FAILED,
        VIRTIO_PCI_DEVICE_STATUS_INVALID_READBACK,
        VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET,
        VIRTIO_PCI_DEVICE_STATUS_FAILED,
        VIRTIO_PCI_DEVICE_STATUS_CHANGED,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_pci_device_status_status status = statuses[index];
        const char *string =
            virtio_pci_device_status_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio device status string is exact");
    }
    CHECK(virtio_pci_device_status_status_string(
              (enum virtio_pci_device_status_status)14U) == NULL,
          "virtio device status undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio device status handling performs no allocation");
}

static void test_init_and_null(void) {
    struct virtio_pci_device_status_controller controller;
    struct virtio_pci_device_status_io io;
    struct virtio_pci_device_status_io bad_io;
    struct test_device device;
    uint8_t observed;

    init_device(&device);
    make_io(&io, &device);
    init_controller(&controller, &io, &device);

    CHECK(virtio_pci_device_status_init(NULL, &io, 1U) ==
              VIRTIO_PCI_DEVICE_STATUS_NULL,
          "device status init rejects null controller");
    CHECK(virtio_pci_device_status_init(&controller, NULL, 1U) ==
              VIRTIO_PCI_DEVICE_STATUS_NULL,
          "device status init rejects null I/O");

    bad_io = io;
    bad_io.write = NULL;
    CHECK(virtio_pci_device_status_init(&controller, &bad_io, 1U) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_IO,
          "device status init rejects null write callback");
    bad_io = io;
    bad_io.read = NULL;
    CHECK(virtio_pci_device_status_init(&controller, &bad_io, 1U) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_IO,
          "device status init rejects null read callback");
    bad_io = io;
    bad_io.context = NULL;
    CHECK(virtio_pci_device_status_init(&controller, &bad_io, 1U) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_IO,
          "device status init rejects null I/O context");

    memset(&controller, 0, sizeof(controller));
    controller.version = VIRTIO_PCI_DEVICE_STATUS_VERSION + 1U;
    CHECK(virtio_pci_device_status_init(&controller, &io, 1U) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION,
          "device status init rejects nonzero internal version");
    memset(&controller, 0, sizeof(controller));
    CHECK(virtio_pci_device_status_init(&controller, &io, 0U) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_GENERATION,
          "device status init rejects zero generation");
    CHECK(virtio_pci_device_status_init(
              &controller, &io,
              VIRTIO_PCI_DEVICE_STATUS_GENERATION_MAX + 1U) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_GENERATION,
          "device status init rejects over-limit generation");

    CHECK(virtio_pci_device_status_init(&controller, &io, 7U) ==
              VIRTIO_PCI_DEVICE_STATUS_OK,
          "device status init publishes controller");
    CHECK(controller.published &&
              controller.version == VIRTIO_PCI_DEVICE_STATUS_VERSION &&
              controller.generation == 7U &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_RESET &&
              controller.status_bits == 0U &&
              controller.observed_bits == 0U,
          "device status init publishes exact RESET controller");
    CHECK(device.write_count == 0U && device.read_count == 0U,
          "device status init performs no I/O");
    CHECK(virtio_pci_device_status_init(&controller, &io, 7U) ==
              VIRTIO_PCI_DEVICE_STATUS_ALREADY_PUBLISHED,
          "device status init rejects duplicate publication");

    CHECK(virtio_pci_device_status_reset(NULL) ==
              VIRTIO_PCI_DEVICE_STATUS_NULL,
          "device status reset rejects null controller");
    CHECK(virtio_pci_device_status_acknowledge(NULL) ==
              VIRTIO_PCI_DEVICE_STATUS_NULL,
          "device status acknowledge rejects null controller");
    CHECK(virtio_pci_device_status_driver(NULL) ==
              VIRTIO_PCI_DEVICE_STATUS_NULL,
          "device status driver rejects null controller");
    CHECK(virtio_pci_device_status_features_ok(NULL) ==
              VIRTIO_PCI_DEVICE_STATUS_NULL,
          "device status features-ok rejects null controller");
    CHECK(virtio_pci_device_status_driver_ok(NULL) ==
              VIRTIO_PCI_DEVICE_STATUS_NULL,
          "device status driver-ok rejects null controller");
    CHECK(virtio_pci_device_status_failed(NULL) ==
              VIRTIO_PCI_DEVICE_STATUS_NULL,
          "device status failed rejects null controller");
    CHECK(virtio_pci_device_status_observe(NULL, &observed) ==
              VIRTIO_PCI_DEVICE_STATUS_NULL,
          "device status observe rejects null controller");
    CHECK(virtio_pci_device_status_observe(&controller, NULL) ==
              VIRTIO_PCI_DEVICE_STATUS_NULL,
          "device status observe rejects null output");
    CHECK(!virtio_pci_device_status_live(NULL),
          "device status live rejects null controller");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "device status init and null checks perform no allocation");
}

static void test_unpublished_and_invalid_state(void) {
    struct virtio_pci_device_status_controller controller;
    struct virtio_pci_device_status_io io;
    struct test_device device;
    uint8_t observed;

    init_device(&device);
    make_io(&io, &device);
    memset(&controller, 0, sizeof(controller));
    controller.io = io;

    CHECK(virtio_pci_device_status_reset(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED,
          "unpublished controller reset is rejected");
    CHECK(virtio_pci_device_status_acknowledge(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED,
          "unpublished controller acknowledge is rejected");
    CHECK(virtio_pci_device_status_driver(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED,
          "unpublished controller driver is rejected");
    CHECK(virtio_pci_device_status_features_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED,
          "unpublished controller features-ok is rejected");
    CHECK(virtio_pci_device_status_driver_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED,
          "unpublished controller driver-ok is rejected");
    CHECK(virtio_pci_device_status_failed(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED,
          "unpublished controller failed is rejected");
    CHECK(virtio_pci_device_status_observe(&controller, &observed) ==
              VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED,
          "unpublished controller observe is rejected");
    CHECK(!virtio_pci_device_status_live(&controller),
          "unpublished controller is non-live");
    CHECK(device.write_count == 0U && device.read_count == 0U,
          "unpublished controller performs no I/O");

    controller.published = true;
    controller.version = VIRTIO_PCI_DEVICE_STATUS_VERSION + 1U;
    CHECK(virtio_pci_device_status_reset(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION,
          "invalid controller version is rejected");
    CHECK(virtio_pci_device_status_observe(&controller, &observed) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION,
          "invalid controller version observation is rejected");
    CHECK(!virtio_pci_device_status_live(&controller),
          "invalid controller version is non-live");

    controller.version = VIRTIO_PCI_DEVICE_STATUS_VERSION;
    controller.state = VIRTIO_PCI_DEVICE_STATE_COUNT;
    CHECK(virtio_pci_device_status_reset(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_STATE,
          "invalid internal state is rejected");
    CHECK(virtio_pci_device_status_observe(&controller, &observed) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_STATE,
          "invalid internal state observation is rejected");
    CHECK(!virtio_pci_device_status_live(&controller),
          "invalid internal state is non-live");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid-state checks perform no allocation");
}

static void test_valid_sequence(void) {
    struct virtio_pci_device_status_controller controller;
    struct virtio_pci_device_status_io io;
    struct test_device device;
    uint8_t observed;

    init_device(&device);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_device_status_init(&controller, &io, 3U) ==
              VIRTIO_PCI_DEVICE_STATUS_OK,
          "valid sequence init succeeds");

    CHECK(virtio_pci_device_status_reset(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.write_count == 1U &&
              device.read_count == 1U &&
              device.last_write == 0U,
          "reset writes zero and reads back");
    CHECK(controller.state == VIRTIO_PCI_DEVICE_STATE_RESET &&
              controller.status_bits == 0U &&
              controller.observed_bits == 0U &&
              !virtio_pci_device_status_live(&controller),
          "reset publishes RESET and stays non-live");

    CHECK(virtio_pci_device_status_acknowledge(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write ==
                  VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE,
          "acknowledge writes exact ACKNOWLEDGE bit");
    CHECK(controller.state == VIRTIO_PCI_DEVICE_STATE_ACKNOWLEDGE &&
              controller.status_bits ==
                  VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE &&
              controller.observed_bits ==
                  VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE,
          "acknowledge publishes exact state");

    CHECK(virtio_pci_device_status_driver(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write ==
                  (VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER),
          "driver writes exact ACKNOWLEDGE and DRIVER bits");
    CHECK(controller.state == VIRTIO_PCI_DEVICE_STATE_DRIVER &&
              controller.status_bits ==
                  (VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER),
          "driver publishes exact state");

    CHECK(virtio_pci_device_status_features_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write ==
                  (VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK),
          "features-ok writes exact feature-complete bits");
    CHECK(controller.state == VIRTIO_PCI_DEVICE_STATE_FEATURES_OK,
          "features-ok publishes exact state");

    CHECK(virtio_pci_device_status_driver_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write ==
                  (VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER_OK),
          "driver-ok writes exact ready bits");
    CHECK(controller.state == VIRTIO_PCI_DEVICE_STATE_DRIVER_OK &&
              controller.status_bits ==
                  (VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER_OK) &&
              virtio_pci_device_status_live(&controller),
          "driver-ok publishes live controller");

    CHECK(virtio_pci_device_status_observe(&controller, &observed) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              observed == controller.status_bits,
          "observe reads exact live status");

    CHECK(virtio_pci_device_status_reset(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write == 0U &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_RESET &&
              !virtio_pci_device_status_live(&controller),
          "reset from live controller returns to RESET");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid sequence performs no allocation");
}

static void test_transition_order(void) {
    struct virtio_pci_device_status_controller controller;
    struct virtio_pci_device_status_io io;
    struct test_device device;

    init_device(&device);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_device_status_init(&controller, &io, 3U) ==
              VIRTIO_PCI_DEVICE_STATUS_OK,
          "transition-order init succeeds");

    CHECK(virtio_pci_device_status_driver(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION &&
              device.write_count == 0U && device.read_count == 0U &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_RESET &&
              !virtio_pci_device_status_live(&controller),
          "driver before acknowledge is rejected");

    CHECK(virtio_pci_device_status_acknowledge(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK,
          "transition-order acknowledge succeeds");
    CHECK(virtio_pci_device_status_features_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_ACKNOWLEDGE &&
              !virtio_pci_device_status_live(&controller),
          "features-ok before driver is rejected");

    CHECK(virtio_pci_device_status_driver(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK,
          "transition-order driver succeeds");
    CHECK(virtio_pci_device_status_driver_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_DRIVER &&
              !virtio_pci_device_status_live(&controller),
          "driver-ok before features-ok is rejected");

    CHECK(virtio_pci_device_status_features_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK,
          "transition-order features-ok succeeds");
    CHECK(virtio_pci_device_status_acknowledge(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_FEATURES_OK,
          "acknowledge after features-ok is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "transition-order checks perform no allocation");
}

static void test_io_failure_no_mutation(void) {
    struct virtio_pci_device_status_controller controller;
    struct virtio_pci_device_status_controller original;
    struct virtio_pci_device_status_io io;
    struct test_device device;

    init_device(&device);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_device_status_init(&controller, &io, 3U) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_acknowledge(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK,
          "IO failure fixture reaches acknowledge");
    device.write_fail = true;
    original = controller;
    CHECK(virtio_pci_device_status_driver(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_IO_FAILED &&
              memcmp(&controller, &original, sizeof(controller)) == 0 &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_ACKNOWLEDGE &&
              !virtio_pci_device_status_live(&controller),
          "write failure leaves controller non-live and unchanged");

    device.write_fail = false;
    CHECK(virtio_pci_device_status_driver(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK,
          "IO failure fixture reaches driver");
    device.read_fail = true;
    original = controller;
    CHECK(virtio_pci_device_status_features_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_IO_FAILED &&
              memcmp(&controller, &original, sizeof(controller)) == 0 &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_DRIVER &&
              !virtio_pci_device_status_live(&controller),
          "read failure leaves controller non-live and unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IO failure paths perform no allocation");
}

static void test_readback_failures(void) {
    struct virtio_pci_device_status_controller controller;
    struct virtio_pci_device_status_controller original;
    struct virtio_pci_device_status_io io;
    struct test_device device;
    uint8_t observed;

    init_device(&device);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_device_status_init(&controller, &io, 3U) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_acknowledge(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_driver(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK,
          "readback fixture reaches driver");

    device.read_override = true;
    device.read_override_value = 0x10U;
    original = controller;
    CHECK(virtio_pci_device_status_features_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_READBACK &&
              memcmp(&controller, &original, sizeof(controller)) == 0 &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_DRIVER &&
              !virtio_pci_device_status_live(&controller),
          "unknown readback bit rejects transition");

    device.read_override_value =
        VIRTIO_PCI_DEVICE_STATUS_BIT_DEVICE_NEEDS_RESET |
        VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER;
    CHECK(virtio_pci_device_status_features_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET &&
              memcmp(&controller, &original, sizeof(controller)) == 0 &&
              !virtio_pci_device_status_live(&controller),
          "device needs reset readback rejects transition");

    device.read_override_value =
        VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED |
        VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER;
    CHECK(virtio_pci_device_status_features_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_FAILED &&
              memcmp(&controller, &original, sizeof(controller)) == 0 &&
              !virtio_pci_device_status_live(&controller),
          "failed readback rejects transition");

    device.read_override = false;
    CHECK(virtio_pci_device_status_features_ok(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_driver_ok(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK,
          "readback fixture recovers to live");

    device.read_override = true;
    device.read_override_value =
        VIRTIO_PCI_DEVICE_STATUS_BIT_DEVICE_NEEDS_RESET |
        VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
        VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER_OK;
    CHECK(!virtio_pci_device_status_live(&controller),
          "device needs reset makes live false");
    CHECK(virtio_pci_device_status_observe(&controller, &observed) ==
              VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET &&
              observed == device.read_override_value,
          "observe reports device needs reset");

    device.read_override_value =
        VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE;
    CHECK(virtio_pci_device_status_observe(&controller, &observed) ==
              VIRTIO_PCI_DEVICE_STATUS_CHANGED &&
              observed == VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE,
          "observe reports changed status");

    device.read_override_value =
        VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
        VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER_OK;
    CHECK(virtio_pci_device_status_observe(&controller, &observed) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              observed == device.read_override_value,
          "observe reports exact expected status");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "readback failure paths perform no allocation");
}

static void test_failed_and_recovery(void) {
    struct virtio_pci_device_status_controller controller;
    struct virtio_pci_device_status_io io;
    struct test_device device;

    init_device(&device);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_device_status_init(&controller, &io, 3U) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_acknowledge(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK,
          "failed fixture reaches acknowledge");

    CHECK(virtio_pci_device_status_failed(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write ==
                  (VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED) &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_FAILED &&
              !virtio_pci_device_status_live(&controller),
          "failed transition publishes non-live FAILED state");

    CHECK(virtio_pci_device_status_failed(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_FAILED,
          "repeated failed transition is rejected");

    CHECK(virtio_pci_device_status_reset(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write == 0U &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_RESET &&
              controller.status_bits == 0U &&
              controller.observed_bits == 0U,
          "reset after failed publishes clean RESET state");

    CHECK(virtio_pci_device_status_acknowledge(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_driver(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_features_ok(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_driver_ok(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_live(&controller),
          "reset after failed restores live initialization path");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failed/recovery path performs no allocation");
}

static void test_failed_from_every_non_failed_state(void) {
    struct virtio_pci_device_status_controller controller;
    struct virtio_pci_device_status_io io;
    struct test_device device;

    init_device(&device);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_device_status_init(&controller, &io, 3U) ==
              VIRTIO_PCI_DEVICE_STATUS_OK,
          "failed-from-reset fixture init succeeds");
    CHECK(virtio_pci_device_status_failed(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write == VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_FAILED &&
              !virtio_pci_device_status_live(&controller),
          "failed from RESET writes exact FAILED bits");

    init_device(&device);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_device_status_init(&controller, &io, 3U) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_acknowledge(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_driver(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK,
          "failed-from-driver fixture reaches DRIVER");
    CHECK(virtio_pci_device_status_failed(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write ==
                  (VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED) &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_FAILED,
          "failed from DRIVER writes exact FAILED bits");

    init_device(&device);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_device_status_init(&controller, &io, 3U) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_acknowledge(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_driver(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_features_ok(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK,
          "failed-from-features-ok fixture reaches FEATURES_OK");
    CHECK(virtio_pci_device_status_failed(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write ==
                  (VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED) &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_FAILED,
          "failed from FEATURES_OK writes exact FAILED bits");

    init_device(&device);
    init_controller(&controller, &io, &device);
    CHECK(virtio_pci_device_status_init(&controller, &io, 3U) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_acknowledge(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_driver(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_features_ok(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK &&
              virtio_pci_device_status_driver_ok(&controller) ==
                  VIRTIO_PCI_DEVICE_STATUS_OK,
          "failed-from-driver-ok fixture reaches DRIVER_OK");
    CHECK(virtio_pci_device_status_failed(&controller) ==
              VIRTIO_PCI_DEVICE_STATUS_OK &&
              device.last_write ==
                  (VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER_OK |
                   VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED) &&
              controller.state == VIRTIO_PCI_DEVICE_STATE_FAILED &&
              !virtio_pci_device_status_live(&controller),
          "failed from DRIVER_OK writes exact FAILED bits and becomes non-live");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failed-from-every-state path performs no allocation");
}

int main(void) {
    test_status_strings();
    test_init_and_null();
    test_unpublished_and_invalid_state();
    test_valid_sequence();
    test_transition_order();
    test_io_failure_no_mutation();
    test_readback_failures();
    test_failed_and_recovery();
    test_failed_from_every_non_failed_state();

    if (failures != 0) {
        fprintf(stderr,
                "Virtio device status host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Virtio device status host tests: ALL OK");
    return 0;
}

#else
#error "This host test must be built with HFOC_VIRTIO_PCI_DEVICE_STATUS_HOST_TEST"
#endif
