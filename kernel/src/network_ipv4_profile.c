#include "network_ipv4_profile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool network_ipv4_profile_generation_valid(uint32_t generation)
{
    return generation >= NETWORK_IPV4_PROFILE_GENERATION_MIN &&
           generation <= NETWORK_IPV4_PROFILE_GENERATION_MAX;
}

static void network_ipv4_profile_clear_pending(
    struct network_ipv4_profile *profile)
{
    memset(&profile->pending_identity, 0,
           sizeof(profile->pending_identity));
    memset(&profile->pending_port_policy, 0,
           sizeof(profile->pending_port_policy));
    profile->has_pending = false;
}

static bool network_ipv4_profile_valid(
    const struct network_ipv4_profile *profile)
{
    return profile != NULL &&
           profile->published &&
           profile->version == NETWORK_IPV4_PROFILE_VERSION &&
           network_ipv4_profile_generation_valid(profile->generation) &&
           /*
            * ITEM-416: an unaddressed profile is valid and published - it
            * simply has no configuration to carry, which is what the state
            * means.  Requiring a transaction would make the one state the
            * hardware path binds in unreachable.
            */
           (profile->unaddressed ||
            network_config_transaction_active(&profile->config) != NULL) &&
           virtio_net_mac_identity_published(&profile->identity) &&
           network_ipv4_port_policy_validate(
               &profile->port_policy) == NETWORK_IPV4_PROFILE_OK;
}

enum network_ipv4_profile_status network_ipv4_port_policy_validate(
    const struct network_ipv4_port_policy *policy)
{
    if (policy == NULL) {
        return NETWORK_IPV4_PROFILE_NULL;
    }
    if (policy->version != NETWORK_IPV4_PORT_POLICY_VERSION) {
        return NETWORK_IPV4_PROFILE_BAD_VERSION;
    }
    if (policy->port < NETWORK_IPV4_PORT_MIN) {
        return NETWORK_IPV4_PROFILE_INVALID_PORT;
    }
    return NETWORK_IPV4_PROFILE_OK;
}

enum network_ipv4_profile_status network_ipv4_profile_init(
    struct network_ipv4_profile *profile,
    const struct network_ipv4_config *initial,
    const struct virtio_net_mac_identity *identity,
    const struct network_ipv4_port_policy *port_policy,
    uint32_t generation)
{
    struct network_config_transaction transaction;
    enum network_ipv4_status config_status;
    enum network_ipv4_profile_status policy_status;
    enum network_config_transaction_status transaction_status;

    if (profile == NULL ||
        initial == NULL ||
        identity == NULL ||
        port_policy == NULL) {
        return NETWORK_IPV4_PROFILE_NULL;
    }
    if (profile->published) {
        return NETWORK_IPV4_PROFILE_ALREADY_PUBLISHED;
    }
    if (profile->version != 0U) {
        return NETWORK_IPV4_PROFILE_BAD_VERSION;
    }

    config_status = network_ipv4_config_validate(initial);
    if (config_status != NETWORK_IPV4_OK) {
        return NETWORK_IPV4_PROFILE_INVALID_CONFIG;
    }
    if (!virtio_net_mac_identity_published(identity)) {
        return NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED;
    }
    policy_status = network_ipv4_port_policy_validate(port_policy);
    if (policy_status != NETWORK_IPV4_PROFILE_OK) {
        return policy_status;
    }
    if (!network_ipv4_profile_generation_valid(generation)) {
        return NETWORK_IPV4_PROFILE_INVALID_GENERATION;
    }

    memset(&transaction, 0, sizeof(transaction));
    transaction_status = network_config_transaction_init(
        &transaction, initial);
    if (transaction_status != NETWORK_CONFIG_TRANSACTION_OK) {
        return NETWORK_IPV4_PROFILE_INVALID_TRANSACTION;
    }

    memset(profile, 0, sizeof(*profile));
    profile->version = NETWORK_IPV4_PROFILE_VERSION;
    profile->generation = generation;
    profile->config = transaction;
    profile->identity = *identity;
    profile->port_policy = *port_policy;
    profile->published = true;
    return NETWORK_IPV4_PROFILE_OK;
}

/*
 * ITEM-416: an interface that is up and has no address yet.
 *
 * The identity and the port policy are validated exactly as they are for an
 * addressed profile; the configuration is not invented, because there is none -
 * that is the whole point.  `active_config` returns NULL while this state is
 * published, so the netif applies nothing and the acquisition cycle fills the
 * address in when its server answers.
 */
enum network_ipv4_profile_status network_ipv4_profile_init_unaddressed(
    struct network_ipv4_profile *profile,
    const struct virtio_net_mac_identity *identity,
    const struct network_ipv4_port_policy *port_policy,
    uint32_t generation)
{
    enum network_ipv4_profile_status policy_status;

    if (profile == NULL || identity == NULL || port_policy == NULL) {
        return NETWORK_IPV4_PROFILE_NULL;
    }
    if (profile->published) {
        return NETWORK_IPV4_PROFILE_ALREADY_PUBLISHED;
    }
    if (profile->version != 0U) {
        return NETWORK_IPV4_PROFILE_BAD_VERSION;
    }
    if (!virtio_net_mac_identity_published(identity)) {
        return NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED;
    }
    policy_status = network_ipv4_port_policy_validate(port_policy);
    if (policy_status != NETWORK_IPV4_PROFILE_OK) {
        return policy_status;
    }
    if (!network_ipv4_profile_generation_valid(generation)) {
        return NETWORK_IPV4_PROFILE_INVALID_GENERATION;
    }
    memset(profile, 0, sizeof(*profile));
    profile->version = NETWORK_IPV4_PROFILE_VERSION;
    profile->generation = generation;
    profile->identity = *identity;
    profile->port_policy = *port_policy;
    profile->unaddressed = true;
    profile->published = true;
    return NETWORK_IPV4_PROFILE_OK;
}

bool network_ipv4_profile_unaddressed(
    const struct network_ipv4_profile *profile)
{
    if (!network_ipv4_profile_valid(profile)) {
        return false;
    }
    return profile->unaddressed;
}

enum network_ipv4_profile_status network_ipv4_profile_stage(
    struct network_ipv4_profile *profile,
    const char *input,
    size_t input_length,
    const struct virtio_net_mac_identity *identity,
    const struct network_ipv4_port_policy *port_policy)
{
    enum network_ipv4_profile_status policy_status;
    enum network_config_transaction_status transaction_status;

    if (profile == NULL ||
        input == NULL ||
        identity == NULL ||
        port_policy == NULL) {
        return NETWORK_IPV4_PROFILE_NULL;
    }
    if (!profile->published) {
        return NETWORK_IPV4_PROFILE_UNPUBLISHED;
    }
    if (profile->version != NETWORK_IPV4_PROFILE_VERSION ||
        !network_ipv4_profile_generation_valid(profile->generation)) {
        return NETWORK_IPV4_PROFILE_BAD_STATE;
    }

    network_ipv4_profile_clear_pending(profile);
    if (!virtio_net_mac_identity_published(identity)) {
        return NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED;
    }
    policy_status = network_ipv4_port_policy_validate(port_policy);
    if (policy_status != NETWORK_IPV4_PROFILE_OK) {
        return policy_status;
    }

    transaction_status = network_config_transaction_stage(
        &profile->config, input, input_length);
    if (transaction_status != NETWORK_CONFIG_TRANSACTION_OK) {
        if (transaction_status ==
            NETWORK_CONFIG_TRANSACTION_PARSE_FAILED) {
            return NETWORK_IPV4_PROFILE_PARSE_FAILED;
        }
        return NETWORK_IPV4_PROFILE_INVALID_TRANSACTION;
    }

    profile->pending_identity = *identity;
    profile->pending_port_policy = *port_policy;
    profile->has_pending = true;
    return NETWORK_IPV4_PROFILE_OK;
}

enum network_ipv4_profile_status network_ipv4_profile_commit(
    struct network_ipv4_profile *profile)
{
    enum network_ipv4_profile_status policy_status;
    enum network_config_transaction_status transaction_status;

    if (profile == NULL) {
        return NETWORK_IPV4_PROFILE_NULL;
    }
    if (!profile->published) {
        return NETWORK_IPV4_PROFILE_UNPUBLISHED;
    }
    if (profile->version != NETWORK_IPV4_PROFILE_VERSION ||
        !network_ipv4_profile_generation_valid(profile->generation)) {
        return NETWORK_IPV4_PROFILE_BAD_STATE;
    }
    if (!profile->has_pending ||
        !network_config_transaction_has_pending(&profile->config)) {
        network_ipv4_profile_clear_pending(profile);
        return NETWORK_IPV4_PROFILE_NO_PENDING;
    }
    if (!virtio_net_mac_identity_published(
            &profile->pending_identity)) {
        network_ipv4_profile_clear_pending(profile);
        return NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED;
    }
    policy_status = network_ipv4_port_policy_validate(
        &profile->pending_port_policy);
    if (policy_status != NETWORK_IPV4_PROFILE_OK) {
        network_ipv4_profile_clear_pending(profile);
        return policy_status;
    }

    transaction_status = network_config_transaction_commit(
        &profile->config);
    if (transaction_status != NETWORK_CONFIG_TRANSACTION_OK) {
        network_ipv4_profile_clear_pending(profile);
        return NETWORK_IPV4_PROFILE_COMMIT_FAILED;
    }

    profile->identity = profile->pending_identity;
    profile->port_policy = profile->pending_port_policy;
    network_ipv4_profile_clear_pending(profile);
    return NETWORK_IPV4_PROFILE_OK;
}

enum network_ipv4_profile_status network_ipv4_profile_rollback(
    struct network_ipv4_profile *profile)
{
    enum network_config_transaction_status transaction_status;

    if (profile == NULL) {
        return NETWORK_IPV4_PROFILE_NULL;
    }
    if (!profile->published) {
        return NETWORK_IPV4_PROFILE_UNPUBLISHED;
    }
    if (profile->version != NETWORK_IPV4_PROFILE_VERSION ||
        !network_ipv4_profile_generation_valid(profile->generation)) {
        return NETWORK_IPV4_PROFILE_BAD_STATE;
    }
    if (!profile->has_pending ||
        !network_config_transaction_has_pending(&profile->config)) {
        return NETWORK_IPV4_PROFILE_NO_PENDING;
    }

    transaction_status = network_config_transaction_rollback(
        &profile->config);
    if (transaction_status != NETWORK_CONFIG_TRANSACTION_OK) {
        return NETWORK_IPV4_PROFILE_BAD_STATE;
    }
    network_ipv4_profile_clear_pending(profile);
    return NETWORK_IPV4_PROFILE_OK;
}

enum network_ipv4_profile_status network_ipv4_profile_validate_state(
    const struct network_ipv4_profile *profile,
    const struct network_config_transaction *observed_transaction,
    const struct virtio_net_mac_identity *observed_identity,
    const struct network_ipv4_port_policy *observed_port_policy,
    uint32_t observed_generation)
{
    const struct network_ipv4_config *observed_active;
    enum network_ipv4_profile_status policy_status;

    if (profile == NULL ||
        observed_transaction == NULL ||
        observed_identity == NULL ||
        observed_port_policy == NULL) {
        return NETWORK_IPV4_PROFILE_NULL;
    }
    if (!network_ipv4_profile_valid(profile)) {
        return NETWORK_IPV4_PROFILE_BAD_STATE;
    }
    if (observed_transaction->version !=
            NETWORK_CONFIG_TRANSACTION_VERSION ||
        !observed_transaction->has_active) {
        return NETWORK_IPV4_PROFILE_INVALID_TRANSACTION;
    }
    observed_active = network_config_transaction_active(
        observed_transaction);
    if (observed_active == NULL) {
        return NETWORK_IPV4_PROFILE_INVALID_TRANSACTION;
    }
    if (!virtio_net_mac_identity_published(observed_identity)) {
        return NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED;
    }
    policy_status = network_ipv4_port_policy_validate(
        observed_port_policy);
    if (policy_status != NETWORK_IPV4_PROFILE_OK) {
        return policy_status;
    }
    if (!network_ipv4_profile_generation_valid(
            observed_generation)) {
        return NETWORK_IPV4_PROFILE_INVALID_GENERATION;
    }
    if (observed_generation != profile->generation) {
        return NETWORK_IPV4_PROFILE_CHANGED;
    }
    if (memcmp(observed_active, &profile->config.active,
               sizeof(*observed_active)) != 0 ||
        memcmp(observed_identity, &profile->identity,
               sizeof(*observed_identity)) != 0 ||
        memcmp(observed_port_policy, &profile->port_policy,
               sizeof(*observed_port_policy)) != 0) {
        return NETWORK_IPV4_PROFILE_CHANGED;
    }
    return NETWORK_IPV4_PROFILE_OK;
}

bool network_ipv4_profile_published(
    const struct network_ipv4_profile *profile)
{
    return network_ipv4_profile_valid(profile);
}

const struct network_ipv4_config *network_ipv4_profile_active_config(
    const struct network_ipv4_profile *profile)
{
    if (!network_ipv4_profile_valid(profile) || profile->unaddressed) {
        return NULL;
    }
    return network_config_transaction_active(&profile->config);
}

const struct virtio_net_mac_identity *network_ipv4_profile_identity(
    const struct network_ipv4_profile *profile)
{
    if (!network_ipv4_profile_valid(profile)) {
        return NULL;
    }
    return &profile->identity;
}

const struct network_ipv4_port_policy *network_ipv4_profile_port_policy(
    const struct network_ipv4_profile *profile)
{
    if (!network_ipv4_profile_valid(profile)) {
        return NULL;
    }
    return &profile->port_policy;
}

bool network_ipv4_profile_has_pending(
    const struct network_ipv4_profile *profile)
{
    return network_ipv4_profile_valid(profile) &&
           profile->has_pending &&
           network_config_transaction_has_pending(&profile->config) &&
           virtio_net_mac_identity_published(
               &profile->pending_identity) &&
           network_ipv4_port_policy_validate(
               &profile->pending_port_policy) ==
               NETWORK_IPV4_PROFILE_OK;
}

const struct network_ipv4_config *network_ipv4_profile_pending_config(
    const struct network_ipv4_profile *profile)
{
    if (!network_ipv4_profile_has_pending(profile)) {
        return NULL;
    }
    return &profile->config.pending;
}

const struct virtio_net_mac_identity *
network_ipv4_profile_pending_identity(
    const struct network_ipv4_profile *profile)
{
    if (!network_ipv4_profile_has_pending(profile)) {
        return NULL;
    }
    return &profile->pending_identity;
}

const struct network_ipv4_port_policy *
network_ipv4_profile_pending_port_policy(
    const struct network_ipv4_profile *profile)
{
    if (!network_ipv4_profile_has_pending(profile)) {
        return NULL;
    }
    return &profile->pending_port_policy;
}

enum network_ipv4_profile_status network_ipv4_profile_qemu_fill(
    struct network_ipv4_config *config_out,
    struct network_ipv4_port_policy *port_policy_out)
{
    if (config_out == NULL || port_policy_out == NULL) {
        return NETWORK_IPV4_PROFILE_NULL;
    }

    memset(config_out, 0, sizeof(*config_out));
    config_out->schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config_out->address.octets[0] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_ADDRESS_0;
    config_out->address.octets[1] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_ADDRESS_1;
    config_out->address.octets[2] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_ADDRESS_2;
    config_out->address.octets[3] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_ADDRESS_3;
    config_out->prefix_length =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_PREFIX;
    config_out->gateway.octets[0] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_GATEWAY_0;
    config_out->gateway.octets[1] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_GATEWAY_1;
    config_out->gateway.octets[2] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_GATEWAY_2;
    config_out->gateway.octets[3] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_GATEWAY_3;
    config_out->dns.octets[0] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_DNS_0;
    config_out->dns.octets[1] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_DNS_1;
    config_out->dns.octets[2] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_DNS_2;
    config_out->dns.octets[3] =
        (uint8_t)NETWORK_IPV4_PROFILE_QEMU_DNS_3;

    memset(port_policy_out, 0, sizeof(*port_policy_out));
    port_policy_out->version = NETWORK_IPV4_PORT_POLICY_VERSION;
    port_policy_out->port = NETWORK_IPV4_PROFILE_QEMU_PORT;
    return NETWORK_IPV4_PROFILE_OK;
}

const char *network_ipv4_profile_status_string(
    enum network_ipv4_profile_status status)
{
    switch (status) {
    case NETWORK_IPV4_PROFILE_OK:
        return "NETWORK_IPV4_PROFILE_OK";
    case NETWORK_IPV4_PROFILE_NULL:
        return "NETWORK_IPV4_PROFILE_NULL";
    case NETWORK_IPV4_PROFILE_BAD_STATE:
        return "NETWORK_IPV4_PROFILE_BAD_STATE";
    case NETWORK_IPV4_PROFILE_BAD_VERSION:
        return "NETWORK_IPV4_PROFILE_BAD_VERSION";
    case NETWORK_IPV4_PROFILE_INVALID_GENERATION:
        return "NETWORK_IPV4_PROFILE_INVALID_GENERATION";
    case NETWORK_IPV4_PROFILE_ALREADY_PUBLISHED:
        return "NETWORK_IPV4_PROFILE_ALREADY_PUBLISHED";
    case NETWORK_IPV4_PROFILE_UNPUBLISHED:
        return "NETWORK_IPV4_PROFILE_UNPUBLISHED";
    case NETWORK_IPV4_PROFILE_INVALID_CONFIG:
        return "NETWORK_IPV4_PROFILE_INVALID_CONFIG";
    case NETWORK_IPV4_PROFILE_INVALID_TRANSACTION:
        return "NETWORK_IPV4_PROFILE_INVALID_TRANSACTION";
    case NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED:
        return "NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED";
    case NETWORK_IPV4_PROFILE_INVALID_PORT:
        return "NETWORK_IPV4_PROFILE_INVALID_PORT";
    case NETWORK_IPV4_PROFILE_NO_PENDING:
        return "NETWORK_IPV4_PROFILE_NO_PENDING";
    case NETWORK_IPV4_PROFILE_PARSE_FAILED:
        return "NETWORK_IPV4_PROFILE_PARSE_FAILED";
    case NETWORK_IPV4_PROFILE_COMMIT_FAILED:
        return "NETWORK_IPV4_PROFILE_COMMIT_FAILED";
    case NETWORK_IPV4_PROFILE_CHANGED:
        return "NETWORK_IPV4_PROFILE_CHANGED";
    default:
        return NULL;
    }
}
