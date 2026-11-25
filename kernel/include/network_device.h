#ifndef HFOC_NETWORK_DEVICE_H
#define HFOC_NETWORK_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#define NETWORK_DEVICE_SNAPSHOT_VERSION 1U
#define NETWORK_DEVICE_SELECTION_VERSION 1U
#define NETWORK_DEVICE_ID_MIN 1U
#define NETWORK_DEVICE_ID_MAX 255U
#define NETWORK_DEVICE_VENDOR_ID_MIN 1U
#define NETWORK_DEVICE_VENDOR_ID_MAX 0xFFFFU
#define NETWORK_DEVICE_PRODUCT_ID_MIN 1U
#define NETWORK_DEVICE_PRODUCT_ID_MAX 0xFFFFU
#define NETWORK_DEVICE_BUS_ADDRESS_MAX 0xFFFFU
#define NETWORK_DEVICE_MAC_LENGTH 6U
#define NETWORK_DEVICE_GENERATION_MIN 1U
#define NETWORK_DEVICE_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define NETWORK_DEVICE_CAPABILITY_RX         (1U << 0U)
#define NETWORK_DEVICE_CAPABILITY_TX         (1U << 1U)
#define NETWORK_DEVICE_CAPABILITY_LINK_STATE (1U << 2U)
#define NETWORK_DEVICE_CAPABILITY_MASK \
    (NETWORK_DEVICE_CAPABILITY_RX | \
     NETWORK_DEVICE_CAPABILITY_TX | \
     NETWORK_DEVICE_CAPABILITY_LINK_STATE)

enum network_device_status {
    NETWORK_DEVICE_OK = 0,
    NETWORK_DEVICE_NULL = 1,
    NETWORK_DEVICE_INVALID_VERSION = 2,
    NETWORK_DEVICE_INVALID_ID = 3,
    NETWORK_DEVICE_INVALID_VENDOR_ID = 4,
    NETWORK_DEVICE_INVALID_PRODUCT_ID = 5,
    NETWORK_DEVICE_INVALID_BUS_ADDRESS = 6,
    NETWORK_DEVICE_INVALID_MAC_ADDRESS = 7,
    NETWORK_DEVICE_UNSUPPORTED_CAPABILITY = 8,
    NETWORK_DEVICE_BAD_GENERATION = 9,
    NETWORK_DEVICE_ALREADY_PUBLISHED = 10,
    NETWORK_DEVICE_UNPUBLISHED = 11,
    NETWORK_DEVICE_CHANGED = 12,
};

struct network_device_snapshot {
    uint32_t version;
    uint32_t device_id;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t bus_address;
    uint8_t mac_address[NETWORK_DEVICE_MAC_LENGTH];
    uint32_t capability_mask;
};

struct network_device_selection {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct network_device_snapshot device;
};

enum network_device_status network_device_snapshot_validate(
    const struct network_device_snapshot *snapshot);
enum network_device_status network_device_snapshot_publish(
    struct network_device_snapshot *published,
    const struct network_device_snapshot *snapshot);
enum network_device_status network_device_selection_init(
    struct network_device_selection *selection,
    const struct network_device_snapshot *snapshot,
    uint32_t generation);
enum network_device_status network_device_selection_validate_state(
    const struct network_device_selection *selection,
    const struct network_device_snapshot *observed_snapshot,
    uint32_t observed_generation);
bool network_device_selection_published(
    const struct network_device_selection *selection);
const char *network_device_status_string(
    enum network_device_status status);

#endif
