#include "ethernet_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_ETHERNET_FRAME_HOST_TEST

#define TEST_COUNT 2U
#define TEST_CAPACITY 2048U
#define TEST_HEADROOM 0U
#define TEST_LENGTH 2048U
#define TEST_STORAGE_SIZE (TEST_COUNT * (TEST_CAPACITY + TEST_HEADROOM))

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct packet_pool_descriptor pool;
static unsigned char storage[TEST_STORAGE_SIZE];

static const uint16_t default_types[] = {
    ETHERNET_FRAME_ETHERTYPE_IPV4,
    ETHERNET_FRAME_ETHERTYPE_ARP,
};
static const uint16_t duplicate_types[] = {
    ETHERNET_FRAME_ETHERTYPE_IPV4,
    ETHERNET_FRAME_ETHERTYPE_IPV4,
};
static const uint16_t vlan_type[] = {
    ETHERNET_FRAME_ETHERTYPE_VLAN,
};
static const uint16_t empty_types[] = {
    ETHERNET_FRAME_ETHERTYPE_IPV4,
};

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static void reset_pool(void)
{
    memset(&pool, 0, sizeof(pool));
    memset(storage, 0xA5, sizeof(storage));
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static bool init_active_slot(
    struct packet_pool_slot_descriptor *descriptor)
{
    reset_pool();
    return packet_pool_init(
               &pool, storage, sizeof(storage),
               TEST_COUNT, TEST_CAPACITY,
               TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_OK &&
           packet_pool_reserve(
               &pool, PACKET_POOL_DIRECTION_RX,
               descriptor) == PACKET_POOL_OK &&
           packet_pool_commit(
               &pool, descriptor) == PACKET_POOL_OK;
}

static bool init_slice(
    struct packet_slice *slice,
    struct packet_pool_slot_descriptor *descriptor,
    uint32_t length)
{
    return init_active_slot(descriptor) &&
           packet_slice_init(
               slice, &pool, descriptor, length) == PACKET_SLICE_OK;
}

static void init_policy(
    struct ethernet_frame_policy *policy,
    const uint16_t *types,
    uint16_t count)
{
    memset(policy, 0, sizeof(*policy));
    policy->version = ETHERNET_FRAME_POLICY_VERSION;
    policy->accept_unicast = true;
    policy->accept_broadcast = true;
    policy->accept_multicast = false;
    policy->allowed_ether_types = types;
    policy->allowed_ether_type_count = count;
}

static void init_view(struct ethernet_frame_view *view)
{
    view->version = UINT32_C(0xDEADBEEF);
    view->destination =
        (enum ethernet_frame_destination)UINT32_C(0xDEAD);
    view->ether_type = UINT16_C(0xDEAD);
    view->payload_offset = UINT32_C(0xDEADBEEF);
    view->payload_length = UINT32_C(0xDEADBEEF);
    view->padding_length = UINT32_C(0xDEADBEEF);
}

static bool view_unchanged(const struct ethernet_frame_view *view)
{
    return view->version == UINT32_C(0xDEADBEEF) &&
           view->destination ==
               (enum ethernet_frame_destination)UINT32_C(0xDEAD) &&
           view->ether_type == UINT16_C(0xDEAD) &&
           view->payload_offset == UINT32_C(0xDEADBEEF) &&
           view->payload_length == UINT32_C(0xDEADBEEF) &&
           view->padding_length == UINT32_C(0xDEADBEEF);
}

static bool view_is(
    const struct ethernet_frame_view *view,
    enum ethernet_frame_destination destination,
    uint16_t ether_type,
    uint32_t payload_length,
    uint32_t padding_length)
{
    return view->version == ETHERNET_FRAME_VERSION &&
           view->destination == destination &&
           view->ether_type == ether_type &&
           view->payload_offset == ETHERNET_FRAME_HEADER_LENGTH &&
           view->payload_length == payload_length &&
           view->padding_length == padding_length;
}

static bool write_bytes(
    struct packet_slice *slice,
    const void *data,
    uint32_t size)
{
    return packet_slice_write(slice, data, size) == PACKET_SLICE_OK;
}

static bool build_frame(
    struct packet_slice *slice,
    const uint8_t destination[6],
    const uint8_t source[6],
    uint16_t ether_type,
    uint32_t payload_length,
    uint32_t padding_length)
{
    struct packet_pool_slot_descriptor descriptor;
    uint32_t frame_length = ETHERNET_FRAME_HEADER_LENGTH +
        payload_length + padding_length;
    uint8_t type_hi = (uint8_t)((ether_type >> 8U) & 0xFFU);
    uint8_t type_lo = (uint8_t)(ether_type & 0xFFU);

    if (!init_slice(slice, &descriptor, frame_length)) {
        return false;
    }
    return write_bytes(slice, destination,
                       ETHERNET_FRAME_DESTINATION_LENGTH) &&
           write_bytes(slice, source,
                       ETHERNET_FRAME_SOURCE_LENGTH) &&
           write_bytes(slice, &type_hi, 1U) &&
           write_bytes(slice, &type_lo, 1U);
}

static bool build_payload_padding(
    struct packet_slice *slice,
    uint32_t payload_length,
    uint32_t padding_length)
{
    static const uint8_t destination[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
    static const uint8_t source[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    uint32_t index;

    if (!build_frame(slice, destination, source,
                     ETHERNET_FRAME_ETHERTYPE_IPV4,
                     payload_length, padding_length)) {
        return false;
    }
    for (index = 0U; index < payload_length; ++index) {
        uint8_t byte = (uint8_t)(index & 0xFFU);
        if (!write_bytes(slice, &byte, 1U)) {
            return false;
        }
    }
    for (index = 0U; index < padding_length; ++index) {
        uint8_t byte = 0U;
        if (!write_bytes(slice, &byte, 1U)) {
            return false;
        }
    }
    return packet_slice_seek(slice, 0U) == PACKET_SLICE_OK;
}

static bool build_raw_default_frame(
    struct packet_slice *slice,
    uint16_t ether_type,
    uint32_t frame_length)
{
    static const uint8_t destination[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
    static const uint8_t source[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    struct packet_pool_slot_descriptor descriptor;
    uint8_t type_hi = (uint8_t)((ether_type >> 8U) & 0xFFU);
    uint8_t type_lo = (uint8_t)(ether_type & 0xFFU);
    uint32_t index;

    if (frame_length < ETHERNET_FRAME_HEADER_LENGTH ||
        !init_slice(slice, &descriptor, frame_length)) {
        return false;
    }
    if (!write_bytes(slice, destination,
                     ETHERNET_FRAME_DESTINATION_LENGTH) ||
        !write_bytes(slice, source,
                     ETHERNET_FRAME_SOURCE_LENGTH) ||
        !write_bytes(slice, &type_hi, 1U) ||
        !write_bytes(slice, &type_lo, 1U)) {
        return false;
    }
    for (index = ETHERNET_FRAME_HEADER_LENGTH;
         index < frame_length;
         ++index) {
        uint8_t byte = 0U;
        if (!write_bytes(slice, &byte, 1U)) {
            return false;
        }
    }
    return packet_slice_seek(slice, 0U) == PACKET_SLICE_OK;
}

static void test_status_strings(void)
{
    static const struct {
        enum ethernet_frame_status status;
        const char *string;
    } statuses[] = {
        {ETHERNET_FRAME_OK, "ETHERNET_FRAME_OK"},
        {ETHERNET_FRAME_NULL, "ETHERNET_FRAME_NULL"},
        {ETHERNET_FRAME_BAD_SLICE, "ETHERNET_FRAME_BAD_SLICE"},
        {ETHERNET_FRAME_BAD_POLICY, "ETHERNET_FRAME_BAD_POLICY"},
        {ETHERNET_FRAME_BAD_VERSION, "ETHERNET_FRAME_BAD_VERSION"},
        {ETHERNET_FRAME_SHORT_FRAME, "ETHERNET_FRAME_SHORT_FRAME"},
        {ETHERNET_FRAME_OVERSIZED_FRAME,
         "ETHERNET_FRAME_OVERSIZED_FRAME"},
        {ETHERNET_FRAME_BAD_DESTINATION,
         "ETHERNET_FRAME_BAD_DESTINATION"},
        {ETHERNET_FRAME_DESTINATION_DENIED,
         "ETHERNET_FRAME_DESTINATION_DENIED"},
        {ETHERNET_FRAME_BAD_ETHERTYPE, "ETHERNET_FRAME_BAD_ETHERTYPE"},
        {ETHERNET_FRAME_ETHERTYPE_DENIED,
         "ETHERNET_FRAME_ETHERTYPE_DENIED"},
        {ETHERNET_FRAME_BAD_PAYLOAD_LENGTH,
         "ETHERNET_FRAME_BAD_PAYLOAD_LENGTH"},
        {ETHERNET_FRAME_BAD_PADDING, "ETHERNET_FRAME_BAD_PADDING"},
    };
    static const struct {
        enum ethernet_frame_destination destination;
        const char *string;
    } destinations[] = {
        {ETHERNET_FRAME_DESTINATION_UNICAST, "unicast"},
        {ETHERNET_FRAME_DESTINATION_BROADCAST, "broadcast"},
        {ETHERNET_FRAME_DESTINATION_MULTICAST, "multicast"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = ethernet_frame_status_string(
            statuses[index].status);
        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "Ethernet frame status string is exact");
    }
    CHECK(ethernet_frame_status_string(
              (enum ethernet_frame_status)99U) == NULL,
          "Ethernet frame undefined status is rejected");
    for (index = 0U;
         index < sizeof(destinations) / sizeof(destinations[0]);
         ++index) {
        const char *string = ethernet_frame_destination_string(
            destinations[index].destination);
        CHECK(string != NULL &&
                  strcmp(string, destinations[index].string) == 0,
              "Ethernet destination string is exact");
    }
    CHECK(ethernet_frame_destination_string(
              (enum ethernet_frame_destination)99U) == NULL,
          "Ethernet undefined destination is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "Ethernet status handling performs no allocation");
}

static void test_null_and_invalid_arguments(void)
{
    struct ethernet_frame_policy policy;
    struct ethernet_frame_policy invalid_policy;
    struct ethernet_frame_view view;
    struct packet_slice invalid_slice;
    struct packet_slice valid_slice;

    reset_pool();
    init_policy(&policy, default_types,
                (uint16_t)(sizeof(default_types) /
                           sizeof(default_types[0])));
    init_view(&view);
    memset(&invalid_slice, 0, sizeof(invalid_slice));
    CHECK(build_raw_default_frame(
              &valid_slice, ETHERNET_FRAME_ETHERTYPE_IPV4, 60U),
          "invalid-policy fixture builds valid frame");

    CHECK(ethernet_frame_validate(
              NULL, &policy, 0U, &view) == ETHERNET_FRAME_NULL,
          "validate rejects null slice");
    CHECK(view_unchanged(&view),
          "null slice leaves view unchanged");
    CHECK(ethernet_frame_validate(
              &invalid_slice, NULL, 0U, &view) ==
              ETHERNET_FRAME_NULL,
          "validate rejects null policy");
    CHECK(ethernet_frame_validate(
              &invalid_slice, &policy, 0U, NULL) ==
              ETHERNET_FRAME_NULL,
          "validate rejects null view");
    CHECK(ethernet_frame_validate(
              &invalid_slice, &policy, 0U, &view) ==
              ETHERNET_FRAME_BAD_SLICE &&
              view_unchanged(&view),
          "validate rejects invalid slice without publication");

    invalid_policy = policy;
    invalid_policy.allowed_ether_types = NULL;
    CHECK(ethernet_frame_validate(
              &valid_slice, &invalid_policy, 0U, &view) ==
              ETHERNET_FRAME_BAD_POLICY,
          "validate rejects null EtherType array");
    invalid_policy = policy;
    invalid_policy.allowed_ether_type_count =
        ETHERNET_FRAME_ALLOWED_TYPES_MAX + 1U;
    CHECK(ethernet_frame_validate(
              &valid_slice, &invalid_policy, 0U, &view) ==
              ETHERNET_FRAME_BAD_POLICY,
          "validate rejects too many allowed EtherTypes");
    invalid_policy = policy;
    invalid_policy.allowed_ether_types = duplicate_types;
    invalid_policy.allowed_ether_type_count =
        (uint16_t)(sizeof(duplicate_types) /
                   sizeof(duplicate_types[0]));
    CHECK(ethernet_frame_validate(
              &valid_slice, &invalid_policy, 0U, &view) ==
              ETHERNET_FRAME_BAD_POLICY,
          "validate rejects duplicate allowed EtherTypes");
    invalid_policy = policy;
    invalid_policy.version = 0U;
    CHECK(ethernet_frame_validate(
              &valid_slice, &invalid_policy, 0U, &view) ==
              ETHERNET_FRAME_BAD_VERSION,
          "validate rejects bad policy version");
    CHECK(view_unchanged(&view),
          "invalid policy leaves view unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "null and invalid argument path performs no allocation");
}

static void test_length_payload_padding(void)
{
    struct ethernet_frame_policy policy;
    struct ethernet_frame_view view;
    struct packet_slice slice;

    init_policy(&policy, default_types,
                (uint16_t)(sizeof(default_types) /
                           sizeof(default_types[0])));

    init_view(&view);
    CHECK(build_payload_padding(&slice, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_OK &&
              view_is(&view, ETHERNET_FRAME_DESTINATION_UNICAST,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U),
          "minimum-length unicast IPv4 frame validates");
    init_view(&view);
    CHECK(build_payload_padding(&slice, 45U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 45U, &view) ==
                  ETHERNET_FRAME_SHORT_FRAME &&
              view_unchanged(&view),
          "frame below minimum length is rejected");
    init_view(&view);
    CHECK(build_payload_padding(&slice, 1500U, 5U) &&
              ethernet_frame_validate(
                  &slice, &policy, 1500U, &view) ==
                  ETHERNET_FRAME_OVERSIZED_FRAME &&
              view_unchanged(&view),
          "frame above maximum length is rejected");
    init_view(&view);
    CHECK(build_payload_padding(&slice, 1500U, 4U) &&
              ethernet_frame_validate(
                  &slice, &policy, 1500U, &view) ==
                  ETHERNET_FRAME_OK &&
              view_is(&view, ETHERNET_FRAME_DESTINATION_UNICAST,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 1500U, 4U),
          "maximum-length frame with bounded padding validates");
    init_view(&view);
    CHECK(build_payload_padding(&slice, 1501U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 1501U, &view) ==
                  ETHERNET_FRAME_BAD_PAYLOAD_LENGTH &&
              view_unchanged(&view),
          "payload above 1500 bytes is rejected");
    init_view(&view);
    CHECK(build_payload_padding(&slice, 0U, 46U) &&
              ethernet_frame_validate(
                  &slice, &policy, 0U, &view) ==
                  ETHERNET_FRAME_OK &&
              view_is(&view, ETHERNET_FRAME_DESTINATION_UNICAST,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 0U, 46U),
          "zero payload with exact padding validates");
    init_view(&view);
    CHECK(build_raw_default_frame(&slice,
                                  ETHERNET_FRAME_ETHERTYPE_IPV4,
                                  61U) &&
              ethernet_frame_validate(
                  &slice, &policy, 0U, &view) ==
                  ETHERNET_FRAME_BAD_PADDING &&
              view_unchanged(&view),
          "padding above 46 bytes is rejected");
    init_view(&view);
    CHECK(build_payload_padding(&slice, 46U, 20U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_OK &&
              view_is(&view, ETHERNET_FRAME_DESTINATION_UNICAST,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 20U),
          "mid-range bounded padding validates");
    init_view(&view);
    CHECK(build_raw_default_frame(&slice,
                                  ETHERNET_FRAME_ETHERTYPE_IPV4,
                                  60U) &&
              ethernet_frame_validate(
                  &slice, &policy, 47U, &view) ==
                  ETHERNET_FRAME_BAD_PADDING &&
              view_unchanged(&view),
          "declared payload beyond frame room is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "length and padding path performs no allocation");
}

static void test_destination_policy(void)
{
    static const uint8_t broadcast[6] =
        {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    static const uint8_t multicast[6] =
        {0x01U, 0x00U, 0x00U, 0x5EU, 0x00U, 0x01U};
    static const uint8_t zero[6] =
        {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    static const uint8_t unicast[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
    static const uint8_t source[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    struct ethernet_frame_policy policy;
    struct ethernet_frame_view view;
    struct packet_slice slice;

    init_policy(&policy, default_types,
                (uint16_t)(sizeof(default_types) /
                           sizeof(default_types[0])));
    init_view(&view);
    CHECK(build_frame(&slice, unicast, source,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_OK &&
              view_is(&view, ETHERNET_FRAME_DESTINATION_UNICAST,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U),
          "unicast destination is accepted");
    init_view(&view);
    CHECK(build_frame(&slice, broadcast, source,
                      ETHERNET_FRAME_ETHERTYPE_ARP, 28U, 18U) &&
              ethernet_frame_validate(
                  &slice, &policy, 28U, &view) ==
                  ETHERNET_FRAME_OK &&
              view_is(&view, ETHERNET_FRAME_DESTINATION_BROADCAST,
                      ETHERNET_FRAME_ETHERTYPE_ARP, 28U, 18U),
          "broadcast destination is accepted");
    init_view(&view);
    CHECK(build_frame(&slice, multicast, source,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_DESTINATION_DENIED &&
              view_unchanged(&view),
          "multicast destination is denied by default");
    init_view(&view);
    CHECK(build_frame(&slice, zero, source,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_BAD_DESTINATION &&
              view_unchanged(&view),
          "all-zero destination is invalid");

    policy.accept_multicast = true;
    init_view(&view);
    CHECK(build_frame(&slice, multicast, source,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_OK,
          "policy can accept multicast explicitly");

    init_policy(&policy, default_types,
                (uint16_t)(sizeof(default_types) /
                           sizeof(default_types[0])));
    policy.accept_unicast = false;
    init_view(&view);
    CHECK(build_frame(&slice, unicast, source,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_DESTINATION_DENIED,
          "disabled unicast policy denies unicast");
    init_policy(&policy, default_types,
                (uint16_t)(sizeof(default_types) /
                           sizeof(default_types[0])));
    policy.accept_broadcast = false;
    init_view(&view);
    CHECK(build_frame(&slice, broadcast, source,
                      ETHERNET_FRAME_ETHERTYPE_ARP, 28U, 18U) &&
              ethernet_frame_validate(
                  &slice, &policy, 28U, &view) ==
                  ETHERNET_FRAME_DESTINATION_DENIED,
          "disabled broadcast policy denies broadcast");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "destination policy path performs no allocation");
}

static void test_ether_type_policy(void)
{
    static const uint8_t destination[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
    static const uint8_t source[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    struct ethernet_frame_policy policy;
    struct ethernet_frame_view view;
    struct packet_slice slice;

    init_policy(&policy, default_types,
                (uint16_t)(sizeof(default_types) /
                           sizeof(default_types[0])));
    init_view(&view);
    CHECK(build_frame(&slice, destination, source,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_OK,
          "IPv4 EtherType is accepted");
    init_view(&view);
    CHECK(build_frame(&slice, destination, source,
                      ETHERNET_FRAME_ETHERTYPE_ARP, 28U, 18U) &&
              ethernet_frame_validate(
                  &slice, &policy, 28U, &view) ==
                  ETHERNET_FRAME_OK,
          "ARP EtherType is accepted");
    init_view(&view);
    CHECK(build_frame(&slice, destination, source,
                      ETHERNET_FRAME_ETHERTYPE_VLAN, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_ETHERTYPE_DENIED &&
              view_unchanged(&view),
          "VLAN EtherType is denied");
    init_view(&view);
    CHECK(build_frame(&slice, destination, source, 0x86DDU, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_ETHERTYPE_DENIED,
          "IPv6 EtherType is denied");
    init_view(&view);
    CHECK(build_frame(&slice, destination, source, 0x005EU, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_BAD_ETHERTYPE,
          "802.3 length field is not an EtherType");
    init_view(&view);
    CHECK(build_frame(&slice, destination, source,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_OK,
          "allowlisted IPv4 remains accepted");

    init_policy(&policy, empty_types,
                (uint16_t)(sizeof(empty_types) /
                           sizeof(empty_types[0])));
    policy.allowed_ether_type_count = 0U;
    policy.allowed_ether_types = NULL;
    init_view(&view);
    CHECK(build_frame(&slice, destination, source,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_ETHERTYPE_DENIED,
          "empty EtherType policy denies every type");

    init_policy(&policy, vlan_type, 1U);
    CHECK(build_frame(&slice, destination, source,
                      ETHERNET_FRAME_ETHERTYPE_IPV4, 46U, 0U) &&
              ethernet_frame_validate(
                  &slice, &policy, 46U, &view) ==
                  ETHERNET_FRAME_BAD_POLICY,
          "policy cannot allow reserved VLAN EtherType");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "EtherType policy path performs no allocation");
}

static void test_stale_slice_and_malformed_isolation(void)
{
    struct ethernet_frame_policy policy;
    struct ethernet_frame_view view;
    struct packet_slice slice;
    struct packet_pool_slot_descriptor descriptor;

    init_policy(&policy, default_types,
                (uint16_t)(sizeof(default_types) /
                           sizeof(default_types[0])));
    CHECK(init_slice(&slice, &descriptor, 60U) == true,
          "stale fixture initializes active slice");
    CHECK(packet_pool_release(
              &pool, &descriptor) == PACKET_POOL_OK,
          "stale fixture releases slot");
    init_view(&view);
    CHECK(ethernet_frame_validate(
              &slice, &policy, 46U, &view) ==
              ETHERNET_FRAME_BAD_SLICE &&
              view_unchanged(&view),
          "stale slice is rejected without publishing a view");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "stale/malformed isolation performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_invalid_arguments();
    test_length_payload_padding();
    test_destination_policy();
    test_ether_type_policy();
    test_stale_slice_and_malformed_isolation();

    if (failures == 0) {
        printf("Ethernet frame host tests: ALL OK\n");
    }
    return failures != 0;
}

#else
#error "This host test must be built with HFOC_ETHERNET_FRAME_HOST_TEST"
#endif
