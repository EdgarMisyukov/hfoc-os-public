#ifndef HFOC_NETWORK_IPV4_PROFILE_H
#define HFOC_NETWORK_IPV4_PROFILE_H

#include "network_config_transaction.h"
#include "virtio_net_mac_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_IPV4_PROFILE_VERSION 1U
#define NETWORK_IPV4_PROFILE_GENERATION_MIN 1U
#define NETWORK_IPV4_PROFILE_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define NETWORK_IPV4_PORT_POLICY_VERSION 1U
#define NETWORK_IPV4_PORT_MIN 1U
#define NETWORK_IPV4_PORT_MAX 65535U

#define NETWORK_IPV4_PROFILE_QEMU_ADDRESS_0 10U
#define NETWORK_IPV4_PROFILE_QEMU_ADDRESS_1 0U
#define NETWORK_IPV4_PROFILE_QEMU_ADDRESS_2 2U
#define NETWORK_IPV4_PROFILE_QEMU_ADDRESS_3 15U
#define NETWORK_IPV4_PROFILE_QEMU_PREFIX 24U
#define NETWORK_IPV4_PROFILE_QEMU_GATEWAY_0 10U
#define NETWORK_IPV4_PROFILE_QEMU_GATEWAY_1 0U
#define NETWORK_IPV4_PROFILE_QEMU_GATEWAY_2 2U
#define NETWORK_IPV4_PROFILE_QEMU_GATEWAY_3 2U
#define NETWORK_IPV4_PROFILE_QEMU_DNS_0 10U
#define NETWORK_IPV4_PROFILE_QEMU_DNS_1 0U
#define NETWORK_IPV4_PROFILE_QEMU_DNS_2 2U
#define NETWORK_IPV4_PROFILE_QEMU_DNS_3 3U
#define NETWORK_IPV4_PROFILE_QEMU_PORT 443U

struct network_ipv4_port_policy {
    uint32_t version;
    uint16_t port;
};

struct network_ipv4_profile {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct network_config_transaction config;
    struct virtio_net_mac_identity identity;
    struct network_ipv4_port_policy port_policy;
    bool has_pending;
    struct virtio_net_mac_identity pending_identity;
    struct network_ipv4_port_policy pending_port_policy;
    /*
     * ITEM-416: published, but with no address yet.
     *
     * A machine in DHCP mode has no address until its server answers, and that
     * is a legitimate state to bring an interface up in - it is the state the
     * acquisition cycle starts from.  The product had no way to say it, so the
     * hardware path passed no configuration and the bind filled in the QEMU
     * fixture's address instead (`10.0.2.15/24`), which every bench machine then
     * held and published.  `network_ipv4_profile_active_config` returns NULL
     * while this is set, so a caller can tell "no address" from an address.
     */
    bool unaddressed;
};

enum network_ipv4_profile_status {
    NETWORK_IPV4_PROFILE_OK = 0,
    NETWORK_IPV4_PROFILE_NULL = 1,
    NETWORK_IPV4_PROFILE_BAD_STATE = 2,
    NETWORK_IPV4_PROFILE_BAD_VERSION = 3,
    NETWORK_IPV4_PROFILE_INVALID_GENERATION = 4,
    NETWORK_IPV4_PROFILE_ALREADY_PUBLISHED = 5,
    NETWORK_IPV4_PROFILE_UNPUBLISHED = 6,
    NETWORK_IPV4_PROFILE_INVALID_CONFIG = 7,
    NETWORK_IPV4_PROFILE_INVALID_TRANSACTION = 8,
    NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED = 9,
    NETWORK_IPV4_PROFILE_INVALID_PORT = 10,
    NETWORK_IPV4_PROFILE_NO_PENDING = 11,
    NETWORK_IPV4_PROFILE_PARSE_FAILED = 12,
    NETWORK_IPV4_PROFILE_COMMIT_FAILED = 13,
    NETWORK_IPV4_PROFILE_CHANGED = 14,
};

enum network_ipv4_profile_status network_ipv4_port_policy_validate(
    const struct network_ipv4_port_policy *policy);
enum network_ipv4_profile_status network_ipv4_profile_init(
    struct network_ipv4_profile *profile,
    const struct network_ipv4_config *initial,
    const struct virtio_net_mac_identity *identity,
    const struct network_ipv4_port_policy *port_policy,
    uint32_t generation);
enum network_ipv4_profile_status network_ipv4_profile_stage(
    struct network_ipv4_profile *profile,
    const char *input,
    size_t input_length,
    const struct virtio_net_mac_identity *identity,
    const struct network_ipv4_port_policy *port_policy);
enum network_ipv4_profile_status network_ipv4_profile_commit(
    struct network_ipv4_profile *profile);
enum network_ipv4_profile_status network_ipv4_profile_rollback(
    struct network_ipv4_profile *profile);
enum network_ipv4_profile_status network_ipv4_profile_validate_state(
    const struct network_ipv4_profile *profile,
    const struct network_config_transaction *observed_transaction,
    const struct virtio_net_mac_identity *observed_identity,
    const struct network_ipv4_port_policy *observed_port_policy,
    uint32_t observed_generation);
bool network_ipv4_profile_published(
    const struct network_ipv4_profile *profile);
/*
 * ITEM-416: publish a profile for an interface that is up and unaddressed.
 * The identity and the port policy are what a caller has; the address is what
 * the machine does not have yet, and the profile says so instead of borrowing
 * one.
 */
enum network_ipv4_profile_status network_ipv4_profile_init_unaddressed(
    struct network_ipv4_profile *profile,
    const struct virtio_net_mac_identity *identity,
    const struct network_ipv4_port_policy *port_policy,
    uint32_t generation);

/* True while the published profile has no address. */
bool network_ipv4_profile_unaddressed(
    const struct network_ipv4_profile *profile);

const struct network_ipv4_config *network_ipv4_profile_active_config(
    const struct network_ipv4_profile *profile);
const struct virtio_net_mac_identity *network_ipv4_profile_identity(
    const struct network_ipv4_profile *profile);
const struct network_ipv4_port_policy *network_ipv4_profile_port_policy(
    const struct network_ipv4_profile *profile);
bool network_ipv4_profile_has_pending(
    const struct network_ipv4_profile *profile);
const struct network_ipv4_config *network_ipv4_profile_pending_config(
    const struct network_ipv4_profile *profile);
const struct virtio_net_mac_identity *network_ipv4_profile_pending_identity(
    const struct network_ipv4_profile *profile);
const struct network_ipv4_port_policy *
network_ipv4_profile_pending_port_policy(
    const struct network_ipv4_profile *profile);
enum network_ipv4_profile_status network_ipv4_profile_qemu_fill(
    struct network_ipv4_config *config_out,
    struct network_ipv4_port_policy *port_policy_out);
const char *network_ipv4_profile_status_string(
    enum network_ipv4_profile_status status);

#endif
