#include "link_state.h"
#include "network_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LINK_STATE_HOST_TEST

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

static struct network_device_snapshot make_device_snapshot(
    uint32_t device_id,
    uint32_t capability_mask)
{
    struct network_device_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot.device_id = device_id;
    snapshot.vendor_id = 0x8086U;
    snapshot.product_id = 0x100EU;
    snapshot.bus_address = 0x0010U;
    snapshot.capability_mask = capability_mask;
    snapshot.mac_address[0] = 0x52U;
    snapshot.mac_address[1] = 0x54U;
    snapshot.mac_address[2] = 0x00U;
    snapshot.mac_address[3] = 0x12U;
    snapshot.mac_address[4] = 0x34U;
    snapshot.mac_address[5] = 0x56U;
    return snapshot;
}

static struct network_link_state_record make_link_state(
    uint32_t device_id,
    enum network_link_state state,
    uint32_t generation)
{
    struct network_link_state_record link;

    memset(&link, 0, sizeof(link));
    link.version = NETWORK_LINK_STATE_VERSION;
    link.device_id = device_id;
    link.state = state;
    link.generation = generation;
    return link;
}

static void test_status_strings(void) {
    static const enum network_link_status statuses[] = {
        NETWORK_LINK_OK,
        NETWORK_LINK_NULL,
        NETWORK_LINK_INVALID_VERSION,
        NETWORK_LINK_INVALID_DEVICE_ID,
        NETWORK_LINK_INVALID_STATE,
        NETWORK_LINK_INVALID_GENERATION,
        NETWORK_LINK_ALREADY_PUBLISHED,
        NETWORK_LINK_UNPUBLISHED,
        NETWORK_LINK_INVALID_DEVICE_SNAPSHOT,
        NETWORK_LINK_DEVICE_MISMATCH,
        NETWORK_LINK_GENERATION_MISMATCH,
        NETWORK_LINK_UNSUPPORTED,
        NETWORK_LINK_NOT_UP,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum network_link_status status = statuses[index];
        const char *string = network_link_status_string(status);

        CHECK(string != NULL &&
                  (status == NETWORK_LINK_OK ?
                       strcmp(string, "NETWORK_LINK_OK") == 0 :
                   status == NETWORK_LINK_NULL ?
                       strcmp(string, "NETWORK_LINK_NULL") == 0 :
                   status == NETWORK_LINK_INVALID_VERSION ?
                       strcmp(string,
                              "NETWORK_LINK_INVALID_VERSION") == 0 :
                   status == NETWORK_LINK_INVALID_DEVICE_ID ?
                       strcmp(string,
                              "NETWORK_LINK_INVALID_DEVICE_ID") == 0 :
                   status == NETWORK_LINK_INVALID_STATE ?
                       strcmp(string, "NETWORK_LINK_INVALID_STATE") == 0 :
                   status == NETWORK_LINK_INVALID_GENERATION ?
                       strcmp(string,
                              "NETWORK_LINK_INVALID_GENERATION") == 0 :
                   status == NETWORK_LINK_ALREADY_PUBLISHED ?
                       strcmp(string,
                              "NETWORK_LINK_ALREADY_PUBLISHED") == 0 :
                   status == NETWORK_LINK_UNPUBLISHED ?
                       strcmp(string, "NETWORK_LINK_UNPUBLISHED") == 0 :
                   status == NETWORK_LINK_INVALID_DEVICE_SNAPSHOT ?
                       strcmp(string,
                              "NETWORK_LINK_INVALID_DEVICE_SNAPSHOT") == 0 :
                   status == NETWORK_LINK_DEVICE_MISMATCH ?
                       strcmp(string,
                              "NETWORK_LINK_DEVICE_MISMATCH") == 0 :
                   status == NETWORK_LINK_GENERATION_MISMATCH ?
                       strcmp(string,
                              "NETWORK_LINK_GENERATION_MISMATCH") == 0 :
                   status == NETWORK_LINK_UNSUPPORTED ?
                       strcmp(string, "NETWORK_LINK_UNSUPPORTED") == 0 :
                       strcmp(string, "NETWORK_LINK_NOT_UP") == 0),
              "link status string is exact");
    }
    CHECK(network_link_status_string(
              (enum network_link_status)13U) == NULL,
          "link undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "link status handling performs no allocation");
}

static void test_state_validation(void) {
    struct network_link_state_record link;

    CHECK(network_link_state_validate(NULL) == NETWORK_LINK_NULL,
          "link state validation rejects null state");

    link = make_link_state(1U, NETWORK_LINK_UP, 1U);
    link.version = NETWORK_LINK_STATE_VERSION + 1U;
    CHECK(network_link_state_validate(&link) ==
              NETWORK_LINK_INVALID_VERSION,
          "unknown link state version is rejected");

    link = make_link_state(0U, NETWORK_LINK_UP, 1U);
    CHECK(network_link_state_validate(&link) ==
              NETWORK_LINK_INVALID_DEVICE_ID,
          "zero link device id is rejected");

    link = make_link_state(NETWORK_DEVICE_ID_MAX + 1U,
                           NETWORK_LINK_UP, 1U);
    CHECK(network_link_state_validate(&link) ==
              NETWORK_LINK_INVALID_DEVICE_ID,
          "over-limit link device id is rejected");

    link = make_link_state(1U,
                           (enum network_link_state)
                               NETWORK_LINK_STATE_COUNT,
                           1U);
    CHECK(network_link_state_validate(&link) ==
              NETWORK_LINK_INVALID_STATE,
          "sentinel link state is rejected");

    link = make_link_state(1U, (enum network_link_state)0xFFU, 1U);
    CHECK(network_link_state_validate(&link) ==
              NETWORK_LINK_INVALID_STATE,
          "undefined link state is rejected");

    link = make_link_state(1U, NETWORK_LINK_UP, 0U);
    CHECK(network_link_state_validate(&link) ==
              NETWORK_LINK_INVALID_GENERATION,
          "zero link generation is rejected");

    link = make_link_state(1U, NETWORK_LINK_UP,
                           NETWORK_LINK_GENERATION_MAX + 1U);
    CHECK(network_link_state_validate(&link) ==
              NETWORK_LINK_INVALID_GENERATION,
          "over-limit link generation is rejected");

    link = make_link_state(1U, NETWORK_LINK_DOWN, 1U);
    CHECK(network_link_state_validate(&link) == NETWORK_LINK_OK,
          "down link state is valid");
    link = make_link_state(1U, NETWORK_LINK_INITIALIZING, 1U);
    CHECK(network_link_state_validate(&link) == NETWORK_LINK_OK,
          "initializing link state is valid");
    link = make_link_state(1U, NETWORK_LINK_UP, 1U);
    CHECK(network_link_state_validate(&link) == NETWORK_LINK_OK,
          "up link state is valid");
    link = make_link_state(1U, NETWORK_LINK_FAILED, 1U);
    CHECK(network_link_state_validate(&link) == NETWORK_LINK_OK,
          "failed link state is valid");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "link state validation performs no allocation");
}

static void test_selection_init(void) {
    struct network_link_selection selection;
    struct network_link_selection original;
    struct network_link_state_record link;
    struct network_link_state_record link_copy;

    link = make_link_state(1U, NETWORK_LINK_UP, 5U);
    memset(&selection, 0xA5, sizeof(selection));
    CHECK(network_link_selection_init(NULL, &link, 3U) ==
              NETWORK_LINK_NULL,
          "link selection init rejects null selection");
    CHECK(network_link_selection_init(&selection, NULL, 3U) ==
              NETWORK_LINK_NULL,
          "link selection init rejects null state");

    memset(&selection, 0, sizeof(selection));
    link.version = NETWORK_LINK_STATE_VERSION + 1U;
    CHECK(network_link_selection_init(&selection, &link, 3U) ==
              NETWORK_LINK_INVALID_VERSION &&
              selection.published == false,
          "link selection init rejects invalid state version without mutation");

    memset(&selection, 0, sizeof(selection));
    link = make_link_state(1U, NETWORK_LINK_UP, 0U);
    CHECK(network_link_selection_init(&selection, &link, 3U) ==
              NETWORK_LINK_INVALID_GENERATION &&
              selection.published == false,
          "link selection init rejects invalid link generation without mutation");

    memset(&selection, 0, sizeof(selection));
    link = make_link_state(1U, NETWORK_LINK_UP, 5U);
    CHECK(network_link_selection_init(&selection, &link, 0U) ==
              NETWORK_LINK_INVALID_GENERATION &&
              selection.published == false,
          "link selection init rejects zero device generation without mutation");

    memset(&selection, 0, sizeof(selection));
    CHECK(network_link_selection_init(&selection, &link,
              NETWORK_LINK_GENERATION_MAX + 1U) ==
              NETWORK_LINK_INVALID_GENERATION &&
              selection.published == false,
          "link selection init rejects over-limit device generation without mutation");

    memset(&selection, 0, sizeof(selection));
    CHECK(network_link_selection_init(&selection, &link, 3U) ==
              NETWORK_LINK_OK &&
              network_link_selection_published(&selection) &&
              selection.version == NETWORK_LINK_SELECTION_VERSION &&
              selection.generation == 3U &&
              selection.link.version == NETWORK_LINK_STATE_VERSION &&
              selection.link.device_id == 1U &&
              selection.link.state == NETWORK_LINK_UP &&
              selection.link.generation == 5U,
          "valid link selection publishes exactly one immutable state");
    original = selection;
    link_copy = selection.link;
    link_copy.state = NETWORK_LINK_FAILED;
    CHECK(network_link_selection_init(&selection, &link_copy, 4U) ==
              NETWORK_LINK_ALREADY_PUBLISHED &&
              memcmp(&selection, &original, sizeof(selection)) == 0,
          "link selection init rejects duplicate publication without mutation");

    memset(&selection, 0, sizeof(selection));
    link = make_link_state(1U, NETWORK_LINK_DOWN, 1U);
    CHECK(network_link_selection_init(&selection, &link, 1U) ==
              NETWORK_LINK_OK &&
              selection.link.state == NETWORK_LINK_DOWN,
          "down link state publishes");
    memset(&selection, 0, sizeof(selection));
    link = make_link_state(1U, NETWORK_LINK_INITIALIZING, 1U);
    CHECK(network_link_selection_init(&selection, &link, 1U) ==
              NETWORK_LINK_OK &&
              selection.link.state == NETWORK_LINK_INITIALIZING,
          "initializing link state publishes");
    memset(&selection, 0, sizeof(selection));
    link = make_link_state(1U, NETWORK_LINK_UP, 1U);
    CHECK(network_link_selection_init(&selection, &link, 1U) ==
              NETWORK_LINK_OK &&
              selection.link.state == NETWORK_LINK_UP,
          "up link state publishes");
    memset(&selection, 0, sizeof(selection));
    link = make_link_state(1U, NETWORK_LINK_FAILED, 1U);
    CHECK(network_link_selection_init(&selection, &link, 1U) ==
              NETWORK_LINK_OK &&
              selection.link.state == NETWORK_LINK_FAILED,
          "failed link state publishes");

    memset(&selection, 0, sizeof(selection));
    CHECK(!network_link_selection_published(&selection) &&
              network_link_selection_published(NULL) == false,
          "link selection published reports null and unpublished selections");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "link selection init performs no allocation");
}

static void test_path_admission(void) {
    struct network_link_selection selection;
    struct network_link_selection original;
    struct network_device_snapshot device;
    struct network_link_state_record link;
    enum network_link_status expected;

    device = make_device_snapshot(1U,
                                  NETWORK_DEVICE_CAPABILITY_LINK_STATE);
    link = make_link_state(1U, NETWORK_LINK_UP, 5U);
    memset(&selection, 0, sizeof(selection));

    CHECK(network_link_path_status(NULL, &device, 3U, 5U) ==
              NETWORK_LINK_NULL,
          "link path admission rejects null selection");
    CHECK(network_link_path_status(&selection, NULL, 3U, 5U) ==
              NETWORK_LINK_NULL,
          "link path admission rejects null device snapshot");
    CHECK(network_link_path_status(&selection, &device, 3U, 5U) ==
              NETWORK_LINK_UNPUBLISHED &&
              network_link_path_enabled(&selection, &device, 3U, 5U) ==
                  false,
          "unpublished link selection never enables a packet path");

    CHECK(network_link_selection_init(&selection, &link, 3U) ==
              NETWORK_LINK_OK,
          "link path fixture publishes up selection");
    original = selection;
    selection.version = NETWORK_LINK_SELECTION_VERSION + 1U;
    CHECK(network_link_path_status(&selection, &device, 3U, 5U) ==
              NETWORK_LINK_INVALID_VERSION &&
              network_link_path_enabled(&selection, &device, 3U, 5U) ==
                  false,
          "unknown link selection version disables packet path");
    selection = original;

    device.capability_mask = 0U;
    CHECK(network_link_path_status(&selection, &device, 3U, 5U) ==
              NETWORK_LINK_INVALID_DEVICE_SNAPSHOT,
          "invalid device snapshot disables packet path");
    device = make_device_snapshot(1U,
                                  NETWORK_DEVICE_CAPABILITY_LINK_STATE);

    device.capability_mask = NETWORK_DEVICE_CAPABILITY_RX;
    CHECK(network_link_path_status(&selection, &device, 3U, 5U) ==
              NETWORK_LINK_UNSUPPORTED,
          "device without link-state capability disables packet path");
    device = make_device_snapshot(1U,
                                  NETWORK_DEVICE_CAPABILITY_LINK_STATE);

    device.device_id = 2U;
    CHECK(network_link_path_status(&selection, &device, 3U, 5U) ==
              NETWORK_LINK_DEVICE_MISMATCH,
          "changed device identity disables packet path");
    device = make_device_snapshot(1U,
                                  NETWORK_DEVICE_CAPABILITY_LINK_STATE);

    CHECK(network_link_path_status(&selection, &device, 0U, 5U) ==
              NETWORK_LINK_INVALID_GENERATION,
          "zero observed device generation disables packet path");
    CHECK(network_link_path_status(&selection, &device, 3U, 0U) ==
              NETWORK_LINK_INVALID_GENERATION,
          "zero observed link generation disables packet path");
    CHECK(network_link_path_status(
              &selection, &device,
              NETWORK_LINK_GENERATION_MAX + 1U, 5U) ==
              NETWORK_LINK_INVALID_GENERATION,
          "over-limit observed device generation disables packet path");
    CHECK(network_link_path_status(
              &selection, &device, 3U,
              NETWORK_LINK_GENERATION_MAX + 1U) ==
              NETWORK_LINK_INVALID_GENERATION,
          "over-limit observed link generation disables packet path");

    CHECK(network_link_path_status(&selection, &device, 4U, 5U) ==
              NETWORK_LINK_GENERATION_MISMATCH,
          "changed device generation disables packet path");
    CHECK(network_link_path_status(&selection, &device, 3U, 6U) ==
              NETWORK_LINK_GENERATION_MISMATCH,
          "changed link generation disables packet path");

    CHECK(network_link_path_status(&selection, &device, 3U, 5U) ==
              NETWORK_LINK_OK &&
              network_link_path_enabled(&selection, &device, 3U, 5U) ==
                  true,
          "published up link with stable device enables packet path");
    CHECK(memcmp(&selection, &original, sizeof(selection)) == 0,
          "link path admission leaves selection unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "link path admission performs no allocation");

    link = make_link_state(1U, NETWORK_LINK_DOWN, 5U);
    memset(&selection, 0, sizeof(selection));
    CHECK(network_link_selection_init(&selection, &link, 3U) ==
              NETWORK_LINK_OK,
          "down link path fixture publishes");
    expected = network_link_path_status(&selection, &device, 3U, 5U);
    CHECK(expected == NETWORK_LINK_NOT_UP &&
              network_link_path_enabled(&selection, &device, 3U, 5U) ==
                  false,
          "down link never enables a packet path");

    link = make_link_state(1U, NETWORK_LINK_INITIALIZING, 5U);
    memset(&selection, 0, sizeof(selection));
    CHECK(network_link_selection_init(&selection, &link, 3U) ==
              NETWORK_LINK_OK,
          "initializing link path fixture publishes");
    expected = network_link_path_status(&selection, &device, 3U, 5U);
    CHECK(expected == NETWORK_LINK_NOT_UP &&
              network_link_path_enabled(&selection, &device, 3U, 5U) ==
                  false,
          "initializing link never enables a packet path");

    link = make_link_state(1U, NETWORK_LINK_FAILED, 5U);
    memset(&selection, 0, sizeof(selection));
    CHECK(network_link_selection_init(&selection, &link, 3U) ==
              NETWORK_LINK_OK,
          "failed link path fixture publishes");
    expected = network_link_path_status(&selection, &device, 3U, 5U);
    CHECK(expected == NETWORK_LINK_NOT_UP &&
              network_link_path_enabled(&selection, &device, 3U, 5U) ==
                  false,
          "failed link never enables a packet path");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "link path admission remains allocation-free for every state");
}

int main(void) {
    test_status_strings();
    test_state_validation();
    test_selection_init();
    test_path_admission();

    if (failures != 0) {
        fprintf(stderr, "Link state host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Link state host tests: ALL OK");
    return 0;
}

#else
#error "This host test must be built with HFOC_LINK_STATE_HOST_TEST"
#endif
