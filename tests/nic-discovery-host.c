/*
 * ITEM-492 host proof for network discovery and driver matching.
 *
 * The item's claim is that the product describes whatever NIC the machine has -
 * not the one vendor it was written against - and that "no driver", "driver
 * refused" and "no device" are three different readings.  This proof drives
 * those decisions, including a PCI walk over a fake configuration space, so the
 * discovery path is exercised without a machine:
 *
 *   * a network controller nobody wrote a driver for is discovered, described
 *     and marked UNSUPPORTED;
 *   * a controller two drivers claim - and the registry refuses that ambiguity;
 *   * a driver that claims a device and then fails leaves it in ERROR, with the
 *     reason, while its neighbour still comes up AVAILABLE;
 *   * a capability scan (no driver callback) never reports AVAILABLE;
 *   * a device whose driver could not read the MAC reports `mac=NONE`, not six
 *     zero bytes;
 *   * non-network PCI devices are not network devices.
 */
#ifdef HFOC_NIC_DISCOVERY_HOST_TEST

#include "nic_discovery.h"

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

/* ------------------------------------------------------------ drivers --- */

static const struct nic_backend nic_virtio = {
    "VIRTIO-NET", UINT16_C(0x1AF4), UINT16_C(0x1000), false
};
static const struct nic_backend nic_realtek = {
    "RTL8168", UINT16_C(0x10EC), UINT16_C(0x8168), false
};

struct bind_log {
    const char *refuse_name;
    const char *reason;
    bool give_mac;
    unsigned int calls;
};

static bool test_bind(void *context,
                      const struct nic_backend *backend,
                      const struct nic_identity *identity,
                      bool *mac_present,
                      uint8_t mac[NIC_MAC_BYTES],
                      const char **reason)
{
    struct bind_log *log = context;

    (void)identity;
    log->calls += 1U;
    if (log->refuse_name != NULL &&
        strcmp(backend->name, log->refuse_name) == 0) {
        *reason = log->reason;
        return false;
    }
    if (log->give_mac) {
        mac[0] = 0x52U;
        mac[1] = 0x54U;
        mac[2] = 0x00U;
        mac[3] = 0x12U;
        mac[4] = 0x34U;
        mac[5] = 0x56U;
        *mac_present = true;
    }
    return true;
}

static struct nic_device device_with(uint16_t vendor, uint16_t product,
                                     uint8_t bus, uint8_t slot, uint8_t func)
{
    struct nic_device device;
    struct nic_identity identity;

    identity.vendor_id = vendor;
    identity.device_id = product;
    identity.bus = bus;
    identity.device = slot;
    identity.function = func;
    nic_device_set_identity(&device, &identity);
    return device;
}

/* --------------------------------------------------------- the tests --- */

static void test_two_devices_one_supported(void)
{
    struct nic_device devices[2U];
    struct bind_log log = { NULL, NULL, true, 0U };
    struct nic_discovery_ops ops = { &log, test_bind };

    devices[0U] = device_with(UINT16_C(0x1AF4), UINT16_C(0x1000), 0U, 3U, 0U);
    devices[1U] = device_with(UINT16_C(0x14E4), UINT16_C(0x165F), 0U, 4U, 0U);
    nic_registry_reset();
    CHECK(nic_registry_register(&nic_virtio) &&
              nic_registry_register(&nic_realtek),
          "the image registers the drivers it carries");
    nic_discovery_classify(devices, 2U, &ops);

    CHECK(devices[0U].state == NIC_STATE_AVAILABLE &&
              strcmp(devices[0U].driver, "VIRTIO-NET") == 0 &&
              devices[0U].mac_present,
          "a claimed device comes up AVAILABLE with the MAC its driver read");
    CHECK(devices[1U].state == NIC_STATE_UNSUPPORTED &&
              devices[1U].driver == NULL && !devices[1U].mac_present,
          "a device no driver claims is UNSUPPORTED, and its neighbour is not");
    CHECK(log.calls == 1U,
          "a device nobody claims is never handed to a driver");
    CHECK(nic_discovery_overall_state(devices, 2U) == NIC_STATE_AVAILABLE,
          "one usable device makes the machine's network available");
}

static void test_a_driver_that_refuses(void)
{
    struct nic_device devices[2U];
    struct bind_log log = { "RTL8168", "RESET_TIMEOUT", false, 0U };
    struct nic_discovery_ops ops = { &log, test_bind };
    char record[NIC_RECORD_BYTES];

    devices[0U] = device_with(UINT16_C(0x10EC), UINT16_C(0x8168), 0U, 5U, 0U);
    devices[1U] = device_with(UINT16_C(0x1AF4), UINT16_C(0x1000), 0U, 6U, 0U);
    nic_registry_reset();
    (void)nic_registry_register(&nic_virtio);
    (void)nic_registry_register(&nic_realtek);
    nic_discovery_classify(devices, 2U, &ops);

    CHECK(devices[0U].state == NIC_STATE_ERROR &&
              strcmp(devices[0U].driver, "RTL8168") == 0 &&
              strcmp(devices[0U].reason, "RESET_TIMEOUT") == 0,
          "a driver that refused its device leaves it in ERROR with the reason");
    CHECK(devices[1U].state == NIC_STATE_AVAILABLE,
          "a refused device does not stop its neighbour from coming up");
    CHECK(nic_device_record(&devices[0U], record, sizeof(record)) > 0U &&
              strstr(record, "state=ERROR") != NULL &&
              strstr(record, "reason=RESET_TIMEOUT") != NULL &&
              strstr(record, " mac=NONE ") != NULL,
          "the description of a refused device names the driver and the reason, "
          "and invents no MAC");
    CHECK(nic_discovery_overall_state(devices, 2U) == NIC_STATE_AVAILABLE,
          "the machine still has a usable network device");
    CHECK(nic_discovery_overall_state(devices + 0, 1U) == NIC_STATE_ERROR,
          "a machine whose only device failed reads ERROR");
}

static void test_a_capability_scan_never_promises(void)
{
    struct nic_device devices[1U];

    devices[0U] = device_with(UINT16_C(0x10EC), UINT16_C(0x8168), 0U, 0x1FU, 0U);
    nic_registry_reset();
    (void)nic_registry_register(&nic_realtek);
    nic_discovery_classify(devices, 1U, NULL);

    CHECK(devices[0U].state == NIC_STATE_SUPPORTED &&
              strcmp(devices[0U].driver, "RTL8168") == 0,
          "without a driver callback a claimed device is SUPPORTED, not AVAILABLE");
    CHECK(!nic_state_is_available(devices[0U].state),
          "a capability reading never reports an initialised device");
    CHECK(nic_discovery_overall_state(devices, 1U) == NIC_STATE_SUPPORTED,
          "the machine reads as supported-but-not-initialised");
    CHECK(nic_discovery_overall_state(NULL, 0U) == NIC_STATE_ABSENT,
          "a machine with no device reads ABSENT");
}

static void test_an_ambiguous_claim_is_refused(void)
{
    const struct nic_backend first = {
        "RTL8168-A", UINT16_C(0x10EC), UINT16_C(0x8168), false
    };
    const struct nic_backend second = {
        "RTL8168-B", UINT16_C(0x10EC), UINT16_C(0x8168), false
    };

    nic_registry_reset();
    CHECK(nic_registry_register(&first) && !nic_registry_register(&second) &&
              nic_registry_count() == 1U,
          "two drivers claiming one part is an ambiguity the registry refuses");
    CHECK(!nic_registry_register(NULL),
          "a null backend registers nothing");
}

/* --------------------------------------------------- the PCI walk --- */

/*
 * A fake configuration space: two network controllers - one Ethernet, one in
 * the "other network controller" subclass - and one storage controller that is
 * not a network device.  The walk must find exactly the two, which is what
 * "enumerate the class, not the vendor" means.
 */
struct fake_pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t base_class;
    uint8_t subclass;
    uint8_t prog_if;
};

static const struct fake_pci_device fake_devices[] = {
    { 0U, 3U, 0U, UINT16_C(0x1AF4), UINT16_C(0x1000), 0x02U, 0x00U, 0x00U },
    { 0U, 4U, 0U, UINT16_C(0x14E4), UINT16_C(0x165F), 0x02U, 0x80U, 0x00U },
    { 0U, 5U, 0U, UINT16_C(0x8086), UINT16_C(0x2829), 0x01U, 0x06U, 0x01U },
};

static bool fake_config_read(void *context,
                             uint8_t bus,
                             uint8_t device,
                             uint8_t function,
                             uint16_t offset,
                             uint32_t *value)
{
    size_t index;

    (void)context;
    if (value == NULL) {
        return false;
    }
    *value = UINT32_C(0xFFFFFFFF);
    for (index = 0U;
         index < sizeof(fake_devices) / sizeof(fake_devices[0U]); ++index) {
        const struct fake_pci_device *entry = &fake_devices[index];

        if (entry->bus != bus || entry->device != device ||
            entry->function != function) {
            continue;
        }
        switch (offset) {
        case 0x00U:
            *value = (uint32_t)entry->vendor_id |
                     ((uint32_t)entry->device_id << 16U);
            return true;
        case 0x08U:
            *value = ((uint32_t)entry->base_class << 24U) |
                     ((uint32_t)entry->subclass << 16U) |
                     ((uint32_t)entry->prog_if << 8U);
            return true;
        case 0x0CU:
            /* A single-function type-0 header. */
            *value = 0U;
            return true;
        default:
            /* No BARs: the decoder reads zero and stops. */
            *value = 0U;
            return true;
        }
    }
    return true;
}

static bool fake_config_write(void *context,
                              uint8_t bus,
                              uint8_t device,
                              uint8_t function,
                              uint16_t offset,
                              uint32_t value)
{
    (void)context;
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    (void)value;
    return true;
}

static void test_the_pci_walk_finds_network_controllers(void)
{
    struct pci_config_io io;
    struct nic_discovery_ops ops;
    struct bind_log log = { NULL, NULL, false, 0U };
    struct nic_device devices[NIC_DEVICES_MAX];
    size_t count = 0U;
    enum nic_discovery_status status;

    io.context = NULL;
    io.read32 = fake_config_read;
    io.write32 = fake_config_write;
    ops.context = &log;
    ops.bind = test_bind;
    nic_registry_reset();
    (void)nic_registry_register(&nic_virtio);

    status = nic_discovery_scan(&io, &ops, devices, NIC_DEVICES_MAX, &count);
    CHECK(status == NIC_DISCOVERY_OK && count == 2U,
          "the walk finds both network controllers and no storage controller");
    CHECK(devices[0U].identity.vendor_id == UINT16_C(0x1AF4) &&
              devices[0U].identity.bus == 0U &&
              devices[0U].identity.device == 3U &&
              devices[1U].identity.vendor_id == UINT16_C(0x14E4) &&
              devices[1U].identity.device == 4U,
          "each device keeps the address and the ids the bus reported");
    CHECK(devices[0U].state == NIC_STATE_AVAILABLE &&
              devices[1U].state == NIC_STATE_UNSUPPORTED,
          "one controller is driven and the other is honestly unsupported");

    CHECK(nic_discovery_scan(&io, &ops, devices, 1U, &count) ==
              NIC_DISCOVERY_CAPACITY &&
              count == 0U,
          "more controllers than the table holds is refused whole");
    CHECK(nic_discovery_scan(NULL, &ops, devices, NIC_DEVICES_MAX, &count) ==
              NIC_DISCOVERY_NULL &&
              nic_discovery_scan(&io, &ops, NULL, NIC_DEVICES_MAX, &count) ==
                  NIC_DISCOVERY_NULL &&
              nic_discovery_scan(&io, &ops, devices, NIC_DEVICES_MAX, NULL) ==
                  NIC_DISCOVERY_NULL,
          "a walk without a bus, a table or a count is refused");
    CHECK(strcmp(nic_discovery_status_string(NIC_DISCOVERY_OK), "OK") == 0 &&
              strcmp(nic_discovery_status_string(NIC_DISCOVERY_CAPACITY),
                     "CAPACITY") == 0,
          "the discovery statuses have one spelling each");
}

int main(void)
{
    test_two_devices_one_supported();
    test_a_driver_that_refuses();
    test_a_capability_scan_never_promises();
    test_an_ambiguous_claim_is_refused();
    test_the_pci_walk_finds_network_controllers();

    if (failures != 0) {
        fprintf(stderr, "NIC discovery host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("NIC discovery host tests: ALL OK");
    return 0;
}

#endif
