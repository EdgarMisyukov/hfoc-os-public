#include "network_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETWORK_DEVICE_HOST_TEST

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

static struct network_device_snapshot make_snapshot(
    uint32_t device_id,
    uint16_t vendor_id,
    uint16_t product_id,
    uint32_t bus_address,
    uint32_t capability_mask)
{
    struct network_device_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot.device_id = device_id;
    snapshot.vendor_id = vendor_id;
    snapshot.product_id = product_id;
    snapshot.bus_address = bus_address;
    snapshot.capability_mask = capability_mask;
    snapshot.mac_address[0] = 0x52U;
    snapshot.mac_address[1] = 0x54U;
    snapshot.mac_address[2] = 0x00U;
    snapshot.mac_address[3] = 0x12U;
    snapshot.mac_address[4] = 0x34U;
    snapshot.mac_address[5] = 0x56U;
    return snapshot;
}

static void check_snapshot_failure(
    const struct network_device_snapshot *snapshot,
    enum network_device_status expected,
    const char *message)
{
    struct network_device_snapshot published;
    unsigned char original[sizeof(published)];

    memset(&published, 0xA5, sizeof(published));
    memcpy(original, &published, sizeof(original));
    CHECK(network_device_snapshot_validate(snapshot) == expected &&
              network_device_snapshot_publish(&published, snapshot) ==
                  expected &&
              memcmp(&published, original, sizeof(original)) == 0,
          message);
}

static void test_status_strings(void) {
    static const enum network_device_status statuses[] = {
        NETWORK_DEVICE_OK,
        NETWORK_DEVICE_NULL,
        NETWORK_DEVICE_INVALID_VERSION,
        NETWORK_DEVICE_INVALID_ID,
        NETWORK_DEVICE_INVALID_VENDOR_ID,
        NETWORK_DEVICE_INVALID_PRODUCT_ID,
        NETWORK_DEVICE_INVALID_BUS_ADDRESS,
        NETWORK_DEVICE_INVALID_MAC_ADDRESS,
        NETWORK_DEVICE_UNSUPPORTED_CAPABILITY,
        NETWORK_DEVICE_BAD_GENERATION,
        NETWORK_DEVICE_ALREADY_PUBLISHED,
        NETWORK_DEVICE_UNPUBLISHED,
        NETWORK_DEVICE_CHANGED,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum network_device_status status = statuses[index];
        const char *string = network_device_status_string(status);

        CHECK(string != NULL &&
                  (status == NETWORK_DEVICE_OK ?
                       strcmp(string, "NETWORK_DEVICE_OK") == 0 :
                   status == NETWORK_DEVICE_NULL ?
                       strcmp(string, "NETWORK_DEVICE_NULL") == 0 :
                   status == NETWORK_DEVICE_INVALID_VERSION ?
                       strcmp(string,
                              "NETWORK_DEVICE_INVALID_VERSION") == 0 :
                   status == NETWORK_DEVICE_INVALID_ID ?
                       strcmp(string, "NETWORK_DEVICE_INVALID_ID") == 0 :
                   status == NETWORK_DEVICE_INVALID_VENDOR_ID ?
                       strcmp(string,
                              "NETWORK_DEVICE_INVALID_VENDOR_ID") == 0 :
                   status == NETWORK_DEVICE_INVALID_PRODUCT_ID ?
                       strcmp(string,
                              "NETWORK_DEVICE_INVALID_PRODUCT_ID") == 0 :
                   status == NETWORK_DEVICE_INVALID_BUS_ADDRESS ?
                       strcmp(string,
                              "NETWORK_DEVICE_INVALID_BUS_ADDRESS") == 0 :
                   status == NETWORK_DEVICE_INVALID_MAC_ADDRESS ?
                       strcmp(string,
                              "NETWORK_DEVICE_INVALID_MAC_ADDRESS") == 0 :
                   status == NETWORK_DEVICE_UNSUPPORTED_CAPABILITY ?
                       strcmp(string,
                              "NETWORK_DEVICE_UNSUPPORTED_CAPABILITY") == 0 :
                   status == NETWORK_DEVICE_BAD_GENERATION ?
                       strcmp(string,
                              "NETWORK_DEVICE_BAD_GENERATION") == 0 :
                   status == NETWORK_DEVICE_ALREADY_PUBLISHED ?
                       strcmp(string,
                              "NETWORK_DEVICE_ALREADY_PUBLISHED") == 0 :
                   status == NETWORK_DEVICE_UNPUBLISHED ?
                       strcmp(string, "NETWORK_DEVICE_UNPUBLISHED") == 0 :
                       strcmp(string, "NETWORK_DEVICE_CHANGED") == 0),
              "network device status string is exact");
    }
    CHECK(network_device_status_string(
              (enum network_device_status)13U) == NULL,
          "network device undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network device status handling performs no allocation");
}

static void test_snapshot_validation_and_publication(void) {
    struct network_device_snapshot snapshot;
    struct network_device_snapshot published;
    struct network_device_snapshot expected;

    CHECK(network_device_snapshot_validate(NULL) == NETWORK_DEVICE_NULL,
          "network device snapshot validation rejects null snapshot");
    CHECK(network_device_snapshot_publish(NULL, NULL) ==
              NETWORK_DEVICE_NULL,
          "network device snapshot publication rejects null output");
    CHECK(network_device_snapshot_publish(NULL, &snapshot) ==
              NETWORK_DEVICE_NULL,
          "network device snapshot publication rejects null output with input");

    snapshot = make_snapshot(1U, 0x8086U, 0x100EU, 0x0010U,
                             NETWORK_DEVICE_CAPABILITY_MASK);
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION + 1U;
    check_snapshot_failure(&snapshot, NETWORK_DEVICE_INVALID_VERSION,
                           "unknown snapshot version is rejected without write");

    snapshot = make_snapshot(0U, 0x8086U, 0x100EU, 0x0010U,
                             NETWORK_DEVICE_CAPABILITY_MASK);
    check_snapshot_failure(&snapshot, NETWORK_DEVICE_INVALID_ID,
                           "zero device id is rejected without write");

    snapshot = make_snapshot(NETWORK_DEVICE_ID_MAX + 1U, 0x8086U, 0x100EU,
                             0x0010U, NETWORK_DEVICE_CAPABILITY_MASK);
    check_snapshot_failure(&snapshot, NETWORK_DEVICE_INVALID_ID,
                           "over-limit device id is rejected without write");

    snapshot = make_snapshot(1U, 0U, 0x100EU, 0x0010U,
                             NETWORK_DEVICE_CAPABILITY_MASK);
    check_snapshot_failure(&snapshot, NETWORK_DEVICE_INVALID_VENDOR_ID,
                           "zero vendor id is rejected without write");

    snapshot = make_snapshot(1U, 0x8086U, 0U, 0x0010U,
                             NETWORK_DEVICE_CAPABILITY_MASK);
    check_snapshot_failure(&snapshot, NETWORK_DEVICE_INVALID_PRODUCT_ID,
                           "zero product id is rejected without write");

    snapshot = make_snapshot(1U, 0x8086U, 0x100EU,
                             NETWORK_DEVICE_BUS_ADDRESS_MAX + 1U,
                             NETWORK_DEVICE_CAPABILITY_MASK);
    check_snapshot_failure(&snapshot, NETWORK_DEVICE_INVALID_BUS_ADDRESS,
                           "over-limit bus address is rejected without write");

    snapshot = make_snapshot(1U, 0x8086U, 0x100EU, 0x0010U,
                             NETWORK_DEVICE_CAPABILITY_MASK);
    memset(snapshot.mac_address, 0, NETWORK_DEVICE_MAC_LENGTH);
    check_snapshot_failure(&snapshot, NETWORK_DEVICE_INVALID_MAC_ADDRESS,
                           "zero mac address is rejected without write");

    snapshot = make_snapshot(1U, 0x8086U, 0x100EU, 0x0010U,
                             (1U << 3U));
    check_snapshot_failure(&snapshot, NETWORK_DEVICE_UNSUPPORTED_CAPABILITY,
                           "unknown capability bit is rejected without write");

    snapshot = make_snapshot(1U, 0x8086U, 0x100EU, 0x0010U, 0U);
    check_snapshot_failure(&snapshot, NETWORK_DEVICE_UNSUPPORTED_CAPABILITY,
                           "zero capability mask is rejected without write");

    snapshot = make_snapshot(1U, 0x8086U, 0x100EU, 0x0010U,
                             NETWORK_DEVICE_CAPABILITY_MASK);
    CHECK(network_device_snapshot_validate(&snapshot) ==
              NETWORK_DEVICE_OK &&
              network_device_snapshot_publish(&published, &snapshot) ==
                  NETWORK_DEVICE_OK &&
              memcmp(&published, &snapshot, sizeof(published)) == 0,
          "valid snapshot publishes an exact copy");
    expected = published;
    snapshot.device_id = 99U;
    snapshot.vendor_id = 0x1234U;
    snapshot.capability_mask = NETWORK_DEVICE_CAPABILITY_RX;
    CHECK(memcmp(&published, &expected, sizeof(published)) == 0,
          "published snapshot remains immutable after source mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network device snapshot handling performs no allocation");
}

static void test_selection_init(void) {
    struct network_device_selection selection;
    struct network_device_snapshot snapshot;
    struct network_device_selection original;
    struct network_device_selection published_selection;

    snapshot = make_snapshot(1U, 0x8086U, 0x100EU, 0x0010U,
                             NETWORK_DEVICE_CAPABILITY_MASK);
    memset(&selection, 0, sizeof(selection));
    CHECK(network_device_selection_init(NULL, &snapshot, 1U) ==
              NETWORK_DEVICE_NULL,
          "selection init rejects null selection");
    CHECK(network_device_selection_init(&selection, NULL, 1U) ==
              NETWORK_DEVICE_NULL,
          "selection init rejects null snapshot");

    memset(&selection, 0, sizeof(selection));
    CHECK(network_device_selection_init(&selection, &snapshot, 0U) ==
              NETWORK_DEVICE_BAD_GENERATION &&
              selection.published == false,
          "selection init rejects zero generation without mutation");

    memset(&selection, 0, sizeof(selection));
    CHECK(network_device_selection_init(&selection, &snapshot,
              NETWORK_DEVICE_GENERATION_MAX + 1U) ==
              NETWORK_DEVICE_BAD_GENERATION &&
              selection.published == false,
          "selection init rejects over-limit generation without mutation");

    memset(&selection, 0, sizeof(selection));
    snapshot.capability_mask = (1U << 3U);
    CHECK(network_device_selection_init(&selection, &snapshot, 1U) ==
              NETWORK_DEVICE_UNSUPPORTED_CAPABILITY &&
              selection.published == false,
          "selection init rejects unsupported capability without mutation");
    snapshot = make_snapshot(1U, 0x8086U, 0x100EU, 0x0010U,
                             NETWORK_DEVICE_CAPABILITY_MASK);

    memset(&selection, 0, sizeof(selection));
    CHECK(network_device_selection_init(&selection, &snapshot, 7U) ==
              NETWORK_DEVICE_OK &&
              network_device_selection_published(&selection) &&
              selection.version == NETWORK_DEVICE_SELECTION_VERSION &&
              selection.generation == 7U &&
              memcmp(&selection.device, &snapshot,
                     sizeof(selection.device)) == 0,
          "valid selection publishes exactly one immutable device");
    original = selection;
    snapshot.device_id = 2U;
    CHECK(network_device_selection_init(&selection, &snapshot, 8U) ==
              NETWORK_DEVICE_ALREADY_PUBLISHED &&
              memcmp(&selection, &original, sizeof(selection)) == 0,
          "selection init rejects duplicate publication without mutation");

    memset(&published_selection, 0, sizeof(published_selection));
    CHECK(!network_device_selection_published(&published_selection) &&
              network_device_selection_published(NULL) == false,
          "selection published reports null and unpublished selections");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network device selection init performs no allocation");
}

static void test_observed_state_validation(void) {
    struct network_device_selection selection;
    struct network_device_snapshot snapshot;
    struct network_device_snapshot observed;

    snapshot = make_snapshot(1U, 0x8086U, 0x100EU, 0x0010U,
                             NETWORK_DEVICE_CAPABILITY_MASK);
    memset(&selection, 0, sizeof(selection));
    CHECK(network_device_selection_validate_state(NULL, &snapshot, 1U) ==
              NETWORK_DEVICE_NULL,
          "observed-state validation rejects null selection");
    CHECK(network_device_selection_validate_state(&selection, NULL, 1U) ==
              NETWORK_DEVICE_NULL,
          "observed-state validation rejects null observed snapshot");
    CHECK(network_device_selection_validate_state(&selection, &snapshot, 1U) ==
              NETWORK_DEVICE_UNPUBLISHED,
          "observed-state validation rejects unpublished selection");

    memset(&selection, 0, sizeof(selection));
    CHECK(network_device_selection_init(&selection, &snapshot, 1U) ==
              NETWORK_DEVICE_OK,
          "observed-state fixture selection publishes");

    observed = snapshot;
    observed.version = NETWORK_DEVICE_SNAPSHOT_VERSION + 1U;
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 1U) == NETWORK_DEVICE_INVALID_VERSION,
          "observed-state validation rejects changed snapshot version");

    observed = snapshot;
    observed.capability_mask = (1U << 3U);
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 1U) ==
              NETWORK_DEVICE_UNSUPPORTED_CAPABILITY,
          "observed-state validation rejects unsupported capability");

    observed = snapshot;
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 0U) == NETWORK_DEVICE_BAD_GENERATION,
          "observed-state validation rejects zero observed generation");
    CHECK(network_device_selection_validate_state(
              &selection, &observed,
              NETWORK_DEVICE_GENERATION_MAX + 1U) ==
              NETWORK_DEVICE_BAD_GENERATION,
          "observed-state validation rejects over-limit generation");

    observed = snapshot;
    observed.device_id = 2U;
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 1U) == NETWORK_DEVICE_CHANGED,
          "changed device id fails closed");

    observed = snapshot;
    observed.vendor_id = 0x1234U;
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 1U) == NETWORK_DEVICE_CHANGED,
          "changed vendor id fails closed");

    observed = snapshot;
    observed.product_id = 0x1234U;
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 1U) == NETWORK_DEVICE_CHANGED,
          "changed product id fails closed");

    observed = snapshot;
    observed.bus_address = 0x0020U;
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 1U) == NETWORK_DEVICE_CHANGED,
          "changed bus address fails closed");

    observed = snapshot;
    observed.mac_address[5] = 0x99U;
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 1U) == NETWORK_DEVICE_CHANGED,
          "changed mac address fails closed");

    observed = snapshot;
    observed.capability_mask = NETWORK_DEVICE_CAPABILITY_RX;
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 1U) == NETWORK_DEVICE_CHANGED,
          "changed capability snapshot fails closed");

    observed = snapshot;
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 2U) == NETWORK_DEVICE_CHANGED,
          "changed generation fails closed");

    observed = snapshot;
    CHECK(network_device_selection_validate_state(
              &selection, &observed, 1U) == NETWORK_DEVICE_OK,
          "unchanged observed state validates");
    CHECK(memcmp(&selection.device, &snapshot,
                 sizeof(selection.device)) == 0 &&
              selection.generation == 1U,
          "observed-state validation leaves selection unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "observed-state validation performs no allocation");
}

int main(void) {
    test_status_strings();
    test_snapshot_validation_and_publication();
    test_selection_init();
    test_observed_state_validation();

    if (failures != 0) {
        fprintf(stderr, "Network device host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Network device host tests: ALL OK");
    return 0;
}

#else
#error "This host test must be built with HFOC_NETWORK_DEVICE_HOST_TEST"
#endif
