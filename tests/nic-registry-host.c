/*
 * ITEM-492 host proof for the NIC registry and the unified device description.
 *
 * The point of the item is that one vendor is no longer the only network device
 * the product can describe.  This proof drives the decisions that make that
 * true, on the cases where getting them wrong hides a machine's real state:
 *
 *   * a device no backend claims is UNSUPPORTED, never AVAILABLE;
 *   * a backend that claims a device and refuses it is ERROR, and the reason
 *     survives into the record;
 *   * two backends claiming one id is an ambiguity the registry refuses;
 *   * a machine with no device reads `devices=0 ... absent=1`, which is not the
 *     same reading as a machine whose device nobody supports;
 *   * the MAC is either the address the driver read or "NONE" - never six zero
 *     bytes pretending to be one.
 */
#include "nic_registry.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct nic_identity identity(uint16_t vendor, uint16_t device,
                                    uint8_t bus, uint8_t slot, uint8_t func)
{
    struct nic_identity value;

    value.vendor_id = vendor;
    value.device_id = device;
    value.bus = bus;
    value.device = slot;
    value.function = func;
    return value;
}

static void test_registry_matching(void)
{
    const struct nic_backend virtio = {
        "VIRTIO-NET", UINT16_C(0x1AF4), UINT16_C(0x1000), false
    };
    const struct nic_backend realtek = {
        "RTL8168", UINT16_C(0x10EC), UINT16_C(0x8168), false
    };
    const struct nic_backend intel_family = {
        "E1000", UINT16_C(0x8086), 0U, true
    };
    const struct nic_backend duplicate = {
        "OTHER", UINT16_C(0x10EC), UINT16_C(0x8168), false
    };
    const struct nic_backend no_name = {
        NULL, UINT16_C(0x1234), UINT16_C(0x5678), false
    };
    const struct nic_backend wildcard_vendor = {
        "ANY", NIC_VENDOR_ANY, 0U, true
    };
    const struct nic_backend wildcard_device = {
        "ANY-DEVICE", UINT16_C(0x1234), NIC_VENDOR_ANY, false
    };

    nic_registry_reset();
    CHECK(nic_registry_register(&virtio) && nic_registry_register(&realtek),
          "two backends register");
    CHECK(nic_registry_register(&intel_family),
          "a vendor-wide claim registers when the backend says so");
    CHECK(!nic_registry_register(&duplicate) && nic_registry_count() == 3U,
          "a second backend claiming one id is refused, not resolved by order");
    CHECK(!nic_registry_register(&no_name) &&
              !nic_registry_register(&wildcard_vendor) &&
              !nic_registry_register(&wildcard_device) &&
              !nic_registry_register(NULL),
          "an unusable claim never enters the registry");

    {
        /* The registry answers with its own copy of the backend, so the match
         * is read by name: the claim is what identifies a driver, not the
         * address the caller happened to register it from. */
        const struct nic_backend *matched =
            nic_registry_match(UINT16_C(0x10EC), UINT16_C(0x8168));

        CHECK(matched != NULL && strcmp(matched->name, "RTL8168") == 0 &&
                  matched->vendor_id == UINT16_C(0x10EC) &&
                  matched->device_id == UINT16_C(0x8168),
              "the Realtek part matches the Realtek backend");
    }
    {
        const struct nic_backend *matched =
            nic_registry_match(UINT16_C(0x1AF4), UINT16_C(0x1000));

        CHECK(matched != NULL && strcmp(matched->name, "VIRTIO-NET") == 0,
              "the virtio part matches the virtio backend");
    }
    {
        const struct nic_backend *matched =
            nic_registry_match(UINT16_C(0x8086), UINT16_C(0x100E));

        CHECK(matched != NULL && strcmp(matched->name, "E1000") == 0,
              "a vendor-wide backend claims a part it did not name");
    }
    CHECK(nic_registry_match(UINT16_C(0x14E4), UINT16_C(0x165F)) == NULL,
          "a device no backend claims matches nothing");
    {
        const struct nic_backend *first = nic_registry_at(0U);
        const struct nic_backend *second = nic_registry_at(1U);

        CHECK(first != NULL && second != NULL &&
                  strcmp(first->name, "VIRTIO-NET") == 0 &&
                  strcmp(second->name, "RTL8168") == 0 &&
                  nic_registry_at(3U) == NULL && nic_registry_count() == 3U,
              "the registry is readable in registration order and bounded by it");
    }
    nic_registry_reset();
    CHECK(nic_registry_count() == 0U &&
              nic_registry_match(UINT16_C(0x10EC), UINT16_C(0x8168)) == NULL,
          "a reset registry claims nothing");
}

static void test_states_of_a_device(void)
{
    const struct nic_backend realtek = {
        "RTL8168", UINT16_C(0x10EC), UINT16_C(0x8168), false
    };
    struct nic_device device;

    nic_device_set_identity(&device, &(struct nic_identity){
        UINT16_C(0x8086), UINT16_C(0x100E), 0U, 3U, 0U});
    CHECK(device.state == NIC_STATE_UNSUPPORTED &&
              device.driver == NULL &&
              strcmp(device.reason, "NO_DRIVER") == 0,
          "a discovered device with no backend is UNSUPPORTED");

    {
        const struct nic_backend *claimed =
            &(const struct nic_backend){"RTL8168", UINT16_C(0x10EC),
                                        UINT16_C(0x8168), false};

        nic_device_set_capability(&device, claimed);
        CHECK(device.state == NIC_STATE_SUPPORTED &&
                  strcmp(device.driver, "RTL8168") == 0 &&
                  strcmp(device.reason, "DRIVER_PRESENT") == 0 &&
                  !nic_state_is_available(device.state),
              "a claimed device is SUPPORTED before any driver initialises it");
        nic_device_set_capability(&device, NULL);
        CHECK(device.state == NIC_STATE_UNSUPPORTED && device.driver == NULL,
              "a device no backend claims is UNSUPPORTED, not merely unbound");
    }

    nic_device_set_state(&device, &realtek, true, NULL);
    CHECK(nic_state_is_available(device.state) &&
              strcmp(device.driver, "RTL8168") == 0 &&
              strcmp(device.reason, "OK") == 0,
          "a device its backend bound is AVAILABLE");
    CHECK(!nic_state_is_available(NIC_STATE_SUPPORTED) &&
              !nic_state_is_available(NIC_STATE_UNSUPPORTED) &&
              !nic_state_is_available(NIC_STATE_ERROR) &&
              !nic_state_is_available(NIC_STATE_ABSENT),
          "only an initialised device counts as available");

    nic_device_set_state(&device, &realtek, false, "LINK_DOWN");
    CHECK(device.state == NIC_STATE_ERROR &&
              strcmp(device.reason, "LINK_DOWN") == 0 &&
              strcmp(device.driver, "RTL8168") == 0,
          "a backend that refused the device is an error that names the reason");

    nic_device_set_state(&device, NULL, false, NULL);
    CHECK(device.state == NIC_STATE_UNSUPPORTED && device.driver == NULL,
          "a device with no claiming backend cannot become an error");

    CHECK(strcmp(nic_state_name(NIC_STATE_SUPPORTED), "SUPPORTED") == 0 &&
              strcmp(nic_state_name(NIC_STATE_AVAILABLE), "AVAILABLE") == 0 &&
              strcmp(nic_state_name(NIC_STATE_UNSUPPORTED), "UNSUPPORTED") == 0 &&
              strcmp(nic_state_name(NIC_STATE_ERROR), "ERROR") == 0 &&
              strcmp(nic_state_name(NIC_STATE_ABSENT), "ABSENT") == 0,
          "the states have one spelling each");
}

static void test_the_device_record(void)
{
    struct nic_device device;
    char record[NIC_RECORD_BYTES];

    nic_device_set_identity(&device, &(struct nic_identity){
        UINT16_C(0x10EC), UINT16_C(0x8168), 0U, 0x1FU, 6U});
    device.mac_present = true;
    device.mac[0] = 0x00U;
    device.mac[1] = 0x11U;
    device.mac[2] = 0x22U;
    device.mac[3] = 0x33U;
    device.mac[4] = 0x44U;
    device.mac[5] = 0x55U;
    nic_device_set_state(&device,
                         &(struct nic_backend){"RTL8168", UINT16_C(0x10EC),
                                               UINT16_C(0x8168), false},
                         true, NULL);
    CHECK(nic_device_record(&device, record, sizeof(record)) > 0U &&
              strcmp(record,
                     "[NIC] bdf=00:1f.06 vendor=10EC device=8168 "
                     "driver=RTL8168 mac=00:11:22:33:44:55 state=AVAILABLE "
                     "reason=OK") == 0,
          "the description names the address, the ids, the driver and the MAC");

    /* ITEM-494: the address a bound driver read lands on the record, and a
     * driver that read none leaves NONE instead of six zero bytes. */
    nic_device_set_mac(&device,
                       (const uint8_t[6]){0x52U, 0x54U, 0x00U, 0x12U, 0x34U,
                                          0x56U});
    CHECK(nic_device_record(&device, record, sizeof(record)) > 0U &&
              strstr(record, " mac=52:54:00:12:34:56 ") != NULL,
          "the address the driver read replaces the record's NONE");
    nic_device_set_mac(&device, NULL);
    CHECK(nic_device_record(&device, record, sizeof(record)) > 0U &&
              strstr(record, " mac=NONE ") != NULL,
          "a driver that read no address leaves NONE, not zeros");
    nic_device_set_mac(NULL, NULL);

    nic_device_set_identity(&device, &(struct nic_identity){
        UINT16_C(0x8086), UINT16_C(0x100E), 0U, 3U, 0U});
    CHECK(nic_device_record(&device, record, sizeof(record)) > 0U &&
              strstr(record, "driver=NONE") != NULL &&
              strstr(record, " mac=NONE ") != NULL &&
              strstr(record, "state=UNSUPPORTED") != NULL &&
              strstr(record, "reason=NO_DRIVER") != NULL,
          "an unsupported device says NONE for its driver and its MAC");

    CHECK(nic_device_record(&device, record, 8U) == 0U &&
              nic_device_record(NULL, record, sizeof(record)) == 0U &&
              nic_device_record(&device, NULL, sizeof(record)) == 0U,
          "a description that cannot fit is refused, never truncated");
}

static void test_the_machine_summary(void)
{
    struct nic_device devices[3U];
    char record[NIC_RECORD_BYTES];

    nic_device_set_identity(&devices[0U], &(struct nic_identity){
        UINT16_C(0x1AF4), UINT16_C(0x1000), 0U, 3U, 0U});
    nic_device_set_state(&devices[0U],
                         &(struct nic_backend){"VIRTIO-NET",
                                               UINT16_C(0x1AF4),
                                               UINT16_C(0x1000), false},
                         true, NULL);
    nic_device_set_identity(&devices[1U], &(struct nic_identity){
        UINT16_C(0x8086), UINT16_C(0x100E), 0U, 4U, 0U});
    nic_device_set_identity(&devices[2U], &(struct nic_identity){
        UINT16_C(0x10EC), UINT16_C(0x8168), 0U, 5U, 0U});
    nic_device_set_state(&devices[2U],
                         &(struct nic_backend){"RTL8168", UINT16_C(0x10EC),
                                               UINT16_C(0x8168), false},
                         false, "RESET_TIMEOUT");

    CHECK(nic_summary_record(devices, 3U, record, sizeof(record)) > 0U &&
              strcmp(record,
                     "[NIC] devices=3 supported=0 available=1 unsupported=1 "
                     "error=1 absent=0") == 0,
          "several devices are counted independently, error included");

    nic_device_set_capability(&devices[1U],
                              &(const struct nic_backend){
                                  "E1000", UINT16_C(0x8086),
                                  UINT16_C(0x100E), false});
    CHECK(nic_summary_record(devices, 3U, record, sizeof(record)) > 0U &&
              strcmp(record,
                     "[NIC] devices=3 supported=1 available=1 unsupported=0 "
                     "error=1 absent=0") == 0,
          "the capability reading counts a claimed device as supported");

    CHECK(nic_summary_record(NULL, 0U, record, sizeof(record)) > 0U &&
              strcmp(record,
                     "[NIC] devices=0 supported=0 available=0 unsupported=0 "
                     "error=0 absent=1") == 0,
          "a machine with no device is a reading, not a failure");
    CHECK(nic_summary_record(NULL, 0U, record, 8U) == 0U,
          "a summary that cannot fit is refused");
}

int main(void)
{
    test_registry_matching();
    test_states_of_a_device();
    test_the_device_record();
    test_the_machine_summary();

    if (failures != 0) {
        fprintf(stderr, "NIC registry host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("NIC registry host tests: ALL OK");
    return 0;
}
