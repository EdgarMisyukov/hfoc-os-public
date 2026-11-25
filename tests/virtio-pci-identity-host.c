#include "virtio_pci_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_PCI_IDENTITY_HOST_TEST

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

static struct virtio_pci_identity make_identity(
    uint16_t device_id,
    uint8_t modern,
    uint8_t bus,
    uint8_t pci_device,
    uint8_t function)
{
    struct virtio_pci_identity identity;

    memset(&identity, 0, sizeof(identity));
    identity.version = VIRTIO_PCI_IDENTITY_VERSION;
    identity.vendor_id = VIRTIO_PCI_VENDOR_ID;
    identity.device_id = device_id;
    identity.bdf.bus = bus;
    identity.bdf.device = pci_device;
    identity.bdf.function = function;
    identity.modern = modern;
    return identity;
}

static struct virtio_pci_capability_snapshot make_capability(
    uint32_t capability_mask)
{
    struct virtio_pci_capability_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VIRTIO_PCI_CAPABILITY_SNAPSHOT_VERSION;
    snapshot.capability_mask = capability_mask;
    return snapshot;
}

static bool status_string_matches(
    enum virtio_pci_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_PCI_OK:
        return strcmp(string, "VIRTIO_PCI_OK") == 0;
    case VIRTIO_PCI_NULL:
        return strcmp(string, "VIRTIO_PCI_NULL") == 0;
    case VIRTIO_PCI_INVALID_IDENTITY_VERSION:
        return strcmp(string, "VIRTIO_PCI_INVALID_IDENTITY_VERSION") == 0;
    case VIRTIO_PCI_INVALID_VENDOR_ID:
        return strcmp(string, "VIRTIO_PCI_INVALID_VENDOR_ID") == 0;
    case VIRTIO_PCI_UNSUPPORTED_DEVICE_ID:
        return strcmp(string, "VIRTIO_PCI_UNSUPPORTED_DEVICE_ID") == 0;
    case VIRTIO_PCI_INVALID_BDF:
        return strcmp(string, "VIRTIO_PCI_INVALID_BDF") == 0;
    case VIRTIO_PCI_INVALID_TRANSPORT:
        return strcmp(string, "VIRTIO_PCI_INVALID_TRANSPORT") == 0;
    case VIRTIO_PCI_INVALID_CAPABILITY_VERSION:
        return strcmp(string, "VIRTIO_PCI_INVALID_CAPABILITY_VERSION") == 0;
    case VIRTIO_PCI_UNSUPPORTED_CAPABILITY:
        return strcmp(string, "VIRTIO_PCI_UNSUPPORTED_CAPABILITY") == 0;
    case VIRTIO_PCI_BAD_GENERATION:
        return strcmp(string, "VIRTIO_PCI_BAD_GENERATION") == 0;
    case VIRTIO_PCI_ALREADY_PUBLISHED:
        return strcmp(string, "VIRTIO_PCI_ALREADY_PUBLISHED") == 0;
    case VIRTIO_PCI_UNPUBLISHED:
        return strcmp(string, "VIRTIO_PCI_UNPUBLISHED") == 0;
    case VIRTIO_PCI_CHANGED:
        return strcmp(string, "VIRTIO_PCI_CHANGED") == 0;
    case VIRTIO_PCI_INVALID_SELECTION_VERSION:
        return strcmp(string, "VIRTIO_PCI_INVALID_SELECTION_VERSION") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void) {
    static const enum virtio_pci_status statuses[] = {
        VIRTIO_PCI_OK,
        VIRTIO_PCI_NULL,
        VIRTIO_PCI_INVALID_IDENTITY_VERSION,
        VIRTIO_PCI_INVALID_VENDOR_ID,
        VIRTIO_PCI_UNSUPPORTED_DEVICE_ID,
        VIRTIO_PCI_INVALID_BDF,
        VIRTIO_PCI_INVALID_TRANSPORT,
        VIRTIO_PCI_INVALID_CAPABILITY_VERSION,
        VIRTIO_PCI_UNSUPPORTED_CAPABILITY,
        VIRTIO_PCI_BAD_GENERATION,
        VIRTIO_PCI_ALREADY_PUBLISHED,
        VIRTIO_PCI_UNPUBLISHED,
        VIRTIO_PCI_CHANGED,
        VIRTIO_PCI_INVALID_SELECTION_VERSION,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_pci_status status = statuses[index];
        const char *string = virtio_pci_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio PCI status string is exact");
    }
    CHECK(virtio_pci_status_string(
              (enum virtio_pci_status)14U) == NULL,
          "virtio PCI undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio PCI status handling performs no allocation");
}

static void test_identity_validation(void) {
    struct virtio_pci_identity identity;

    CHECK(virtio_pci_identity_validate(NULL) == VIRTIO_PCI_NULL,
          "virtio PCI identity validation rejects null identity");

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U, 0U, 0U, 0U);
    identity.version = VIRTIO_PCI_IDENTITY_VERSION + 1U;
    CHECK(virtio_pci_identity_validate(&identity) ==
              VIRTIO_PCI_INVALID_IDENTITY_VERSION,
          "unknown identity version is rejected");

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U, 0U, 0U, 0U);
    identity.vendor_id = 0x1234U;
    CHECK(virtio_pci_identity_validate(&identity) ==
              VIRTIO_PCI_INVALID_VENDOR_ID,
          "non-virtio vendor is rejected");

    identity = make_identity(0x0001U, 1U, 0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) ==
              VIRTIO_PCI_UNSUPPORTED_DEVICE_ID,
          "unsupported virtio device id is rejected");
    identity = make_identity(0x1001U, 1U, 0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) ==
              VIRTIO_PCI_UNSUPPORTED_DEVICE_ID,
          "transitional block device id is rejected");
    identity = make_identity(0x1042U, 1U, 0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) ==
              VIRTIO_PCI_UNSUPPORTED_DEVICE_ID,
          "modern block device id is rejected");
    identity = make_identity(0xFFFFU, 1U, 0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) ==
              VIRTIO_PCI_UNSUPPORTED_DEVICE_ID,
          "reserved device id is rejected");

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U,
                             0U, 32U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) == VIRTIO_PCI_INVALID_BDF,
          "over-limit BDF device is rejected");
    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U,
                             0U, 0U, 8U);
    CHECK(virtio_pci_identity_validate(&identity) == VIRTIO_PCI_INVALID_BDF,
          "over-limit BDF function is rejected");

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_TRANSITIONAL, 1U,
                             0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) ==
              VIRTIO_PCI_INVALID_TRANSPORT,
          "transitional device with modern flag is rejected");
    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 0U,
                             0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) ==
              VIRTIO_PCI_INVALID_TRANSPORT,
          "modern device with transitional flag is rejected");
    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_TRANSITIONAL, 1U,
                             0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) ==
              VIRTIO_PCI_INVALID_TRANSPORT,
          "transitional rng with modern flag is rejected");
    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_MODERN, 0U,
                             0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) ==
              VIRTIO_PCI_INVALID_TRANSPORT,
          "modern rng with transitional flag is rejected");

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_TRANSITIONAL, 0U,
                             0xFFU, 31U, 7U);
    CHECK(virtio_pci_identity_validate(&identity) == VIRTIO_PCI_OK,
          "transitional net boundary identity validates");
    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_TRANSITIONAL, 0U,
                             0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) == VIRTIO_PCI_OK,
          "transitional rng identity validates");
    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U,
                             0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) == VIRTIO_PCI_OK,
          "modern net identity validates");
    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_MODERN, 1U,
                             0U, 0U, 0U);
    CHECK(virtio_pci_identity_validate(&identity) == VIRTIO_PCI_OK,
          "modern rng identity validates");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio PCI identity validation performs no allocation");
}

static void test_capability_validation(void) {
    struct virtio_pci_capability_snapshot snapshot;

    CHECK(virtio_pci_capability_snapshot_validate(NULL) == VIRTIO_PCI_NULL,
          "virtio PCI capability validation rejects null snapshot");

    snapshot = make_capability(VIRTIO_PCI_CAPABILITY_MASK);
    snapshot.version = VIRTIO_PCI_CAPABILITY_SNAPSHOT_VERSION + 1U;
    CHECK(virtio_pci_capability_snapshot_validate(&snapshot) ==
              VIRTIO_PCI_INVALID_CAPABILITY_VERSION,
          "unknown capability snapshot version is rejected");

    snapshot = make_capability(0U);
    CHECK(virtio_pci_capability_snapshot_validate(&snapshot) ==
              VIRTIO_PCI_UNSUPPORTED_CAPABILITY,
          "zero capability mask is rejected");
    snapshot = make_capability(1U << 4U);
    CHECK(virtio_pci_capability_snapshot_validate(&snapshot) ==
              VIRTIO_PCI_UNSUPPORTED_CAPABILITY,
          "unknown capability bit is rejected");
    snapshot = make_capability(0xFFFFFFFFU);
    CHECK(virtio_pci_capability_snapshot_validate(&snapshot) ==
              VIRTIO_PCI_UNSUPPORTED_CAPABILITY,
          "all-bits capability mask is rejected");

    snapshot = make_capability(VIRTIO_PCI_CAPABILITY_COMMON);
    CHECK(virtio_pci_capability_snapshot_validate(&snapshot) ==
              VIRTIO_PCI_OK,
          "common capability snapshot validates");
    snapshot = make_capability(VIRTIO_PCI_CAPABILITY_MASK);
    CHECK(virtio_pci_capability_snapshot_validate(&snapshot) ==
              VIRTIO_PCI_OK,
          "full capability snapshot validates");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio PCI capability validation performs no allocation");
}

static void test_selection_init(void) {
    struct virtio_pci_selection selection;
    struct virtio_pci_selection original;
    struct virtio_pci_identity identity;
    struct virtio_pci_capability_snapshot capability;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U, 0U, 0U, 0U);
    capability = make_capability(VIRTIO_PCI_CAPABILITY_MASK);
    memset(&selection, 0xA5, sizeof(selection));
    CHECK(virtio_pci_selection_init(NULL, &identity, &capability, 1U) ==
              VIRTIO_PCI_NULL,
          "selection init rejects null selection");
    CHECK(virtio_pci_selection_init(&selection, NULL, &capability, 1U) ==
              VIRTIO_PCI_NULL,
          "selection init rejects null identity");
    CHECK(virtio_pci_selection_init(&selection, &identity, NULL, 1U) ==
              VIRTIO_PCI_NULL,
          "selection init rejects null capability snapshot");

    memset(&selection, 0, sizeof(selection));
    identity.version = VIRTIO_PCI_IDENTITY_VERSION + 1U;
    CHECK(virtio_pci_selection_init(&selection, &identity, &capability, 1U) ==
              VIRTIO_PCI_INVALID_IDENTITY_VERSION &&
              selection.published == false,
          "selection init rejects invalid identity without mutation");
    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U, 0U, 0U, 0U);

    memset(&selection, 0, sizeof(selection));
    capability.version = VIRTIO_PCI_CAPABILITY_SNAPSHOT_VERSION + 1U;
    CHECK(virtio_pci_selection_init(&selection, &identity, &capability, 1U) ==
              VIRTIO_PCI_INVALID_CAPABILITY_VERSION &&
              selection.published == false,
          "selection init rejects invalid capability without mutation");
    capability = make_capability(VIRTIO_PCI_CAPABILITY_MASK);

    memset(&selection, 0, sizeof(selection));
    CHECK(virtio_pci_selection_init(&selection, &identity, &capability, 0U) ==
              VIRTIO_PCI_BAD_GENERATION &&
              selection.published == false,
          "selection init rejects zero generation without mutation");
    memset(&selection, 0, sizeof(selection));
    CHECK(virtio_pci_selection_init(&selection, &identity, &capability,
              VIRTIO_PCI_GENERATION_MAX + 1U) ==
              VIRTIO_PCI_BAD_GENERATION &&
              selection.published == false,
          "selection init rejects over-limit generation without mutation");

    memset(&selection, 0, sizeof(selection));
    CHECK(virtio_pci_selection_init(&selection, &identity, &capability, 7U) ==
              VIRTIO_PCI_OK &&
              virtio_pci_selection_published(&selection) &&
              selection.version == VIRTIO_PCI_SELECTION_VERSION &&
              selection.generation == 7U &&
              memcmp(&selection.identity, &identity,
                     sizeof(selection.identity)) == 0 &&
              memcmp(&selection.capability_snapshot, &capability,
                     sizeof(selection.capability_snapshot)) == 0,
          "valid selection publishes exactly one immutable virtio PCI device");
    original = selection;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_RNG_MODERN, 1U, 1U, 1U, 0U);
    capability = make_capability(VIRTIO_PCI_CAPABILITY_COMMON);
    CHECK(virtio_pci_selection_init(&selection, &identity, &capability, 8U) ==
              VIRTIO_PCI_ALREADY_PUBLISHED &&
              memcmp(&selection, &original, sizeof(selection)) == 0,
          "duplicate selection publication is rejected without mutation");

    memset(&selection, 0, sizeof(selection));
    CHECK(!virtio_pci_selection_published(&selection) &&
              virtio_pci_selection_published(NULL) == false,
          "selection published reports null and unpublished selections");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio PCI selection init performs no allocation");
}

static void test_observed_state_validation(void) {
    struct virtio_pci_selection selection;
    struct virtio_pci_selection original;
    struct virtio_pci_identity identity;
    struct virtio_pci_identity observed;
    struct virtio_pci_capability_snapshot capability;
    struct virtio_pci_capability_snapshot observed_capability;

    identity = make_identity(VIRTIO_PCI_DEVICE_ID_NET_MODERN, 1U, 0U, 0U, 0U);
    capability = make_capability(VIRTIO_PCI_CAPABILITY_MASK);
    memset(&selection, 0, sizeof(selection));

    CHECK(virtio_pci_selection_validate_state(
              NULL, &identity, &capability, 1U) == VIRTIO_PCI_NULL,
          "observed-state validation rejects null selection");
    CHECK(virtio_pci_selection_validate_state(
              &selection, NULL, &capability, 1U) == VIRTIO_PCI_NULL,
          "observed-state validation rejects null identity");
    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, NULL, 1U) == VIRTIO_PCI_NULL,
          "observed-state validation rejects null capability snapshot");
    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &capability, 1U) ==
              VIRTIO_PCI_UNPUBLISHED,
          "observed-state validation rejects unpublished selection");

    memset(&selection, 0, sizeof(selection));
    CHECK(virtio_pci_selection_init(&selection, &identity, &capability, 1U) ==
              VIRTIO_PCI_OK,
          "observed-state fixture selection publishes");

    original = selection;
    selection.version = VIRTIO_PCI_SELECTION_VERSION + 1U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &capability, 1U) ==
              VIRTIO_PCI_INVALID_SELECTION_VERSION,
          "invalid stored selection version is rejected");
    selection = original;

    selection.identity.version = VIRTIO_PCI_IDENTITY_VERSION + 1U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &capability, 1U) ==
              VIRTIO_PCI_INVALID_IDENTITY_VERSION,
          "invalid stored identity version is rejected");
    selection = original;

    selection.capability_snapshot.capability_mask = 1U << 4U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &capability, 1U) ==
              VIRTIO_PCI_UNSUPPORTED_CAPABILITY,
          "invalid stored capability is rejected");
    selection = original;

    observed = identity;
    observed.version = VIRTIO_PCI_IDENTITY_VERSION + 1U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &observed, &capability, 1U) ==
              VIRTIO_PCI_INVALID_IDENTITY_VERSION,
          "invalid observed identity version is rejected");

    observed = identity;
    observed.vendor_id = 0x1234U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &observed, &capability, 1U) ==
              VIRTIO_PCI_INVALID_VENDOR_ID,
          "invalid observed vendor is rejected");

    observed = identity;
    observed.device_id = 0x1042U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &observed, &capability, 1U) ==
              VIRTIO_PCI_UNSUPPORTED_DEVICE_ID,
          "invalid observed device id is rejected");

    observed = identity;
    observed.bdf.device = 32U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &observed, &capability, 1U) ==
              VIRTIO_PCI_INVALID_BDF,
          "invalid observed BDF is rejected");

    observed = identity;
    observed.modern = 0U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &observed, &capability, 1U) ==
              VIRTIO_PCI_INVALID_TRANSPORT,
          "invalid observed transport policy is rejected");

    observed_capability = capability;
    observed_capability.version = VIRTIO_PCI_CAPABILITY_SNAPSHOT_VERSION + 1U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &observed_capability, 1U) ==
              VIRTIO_PCI_INVALID_CAPABILITY_VERSION,
          "invalid observed capability version is rejected");

    observed_capability = capability;
    observed_capability.capability_mask = 0U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &observed_capability, 1U) ==
              VIRTIO_PCI_UNSUPPORTED_CAPABILITY,
          "invalid observed capability mask is rejected");

    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &capability, 0U) ==
              VIRTIO_PCI_BAD_GENERATION,
          "observed-state validation rejects zero observed generation");
    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &capability,
              VIRTIO_PCI_GENERATION_MAX + 1U) ==
              VIRTIO_PCI_BAD_GENERATION,
          "observed-state validation rejects over-limit generation");

    observed = identity;
    observed.device_id = VIRTIO_PCI_DEVICE_ID_RNG_MODERN;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &observed, &capability, 1U) ==
              VIRTIO_PCI_CHANGED,
          "changed device id fails closed");

    observed = identity;
    observed.bdf.bus = 1U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &observed, &capability, 1U) ==
              VIRTIO_PCI_CHANGED,
          "changed BDF bus fails closed");

    observed = identity;
    observed.bdf.device = 1U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &observed, &capability, 1U) ==
              VIRTIO_PCI_CHANGED,
          "changed BDF device fails closed");

    observed = identity;
    observed.bdf.function = 1U;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &observed, &capability, 1U) ==
              VIRTIO_PCI_CHANGED,
          "changed BDF function fails closed");

    observed_capability = capability;
    observed_capability.capability_mask = VIRTIO_PCI_CAPABILITY_COMMON;
    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &observed_capability, 1U) ==
              VIRTIO_PCI_CHANGED,
          "changed capability mask fails closed");

    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &capability, 2U) ==
              VIRTIO_PCI_CHANGED,
          "changed generation fails closed");

    CHECK(virtio_pci_selection_validate_state(
              &selection, &identity, &capability, 1U) == VIRTIO_PCI_OK &&
              memcmp(&selection, &original, sizeof(selection)) == 0,
          "unchanged observed state validates without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "observed-state validation performs no allocation");
}

int main(void) {
    test_status_strings();
    test_identity_validation();
    test_capability_validation();
    test_selection_init();
    test_observed_state_validation();

    if (failures != 0) {
        fprintf(stderr, "Virtio PCI identity host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Virtio PCI identity host tests: ALL OK");
    return 0;
}

#else
#error "This host test must be built with HFOC_VIRTIO_PCI_IDENTITY_HOST_TEST"
#endif
