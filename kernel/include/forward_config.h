#ifndef HFOC_FORWARD_CONFIG_H
#define HFOC_FORWARD_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-449: the one forward declaration the appliance understands.
 *
 * The operator declares one TCP forward: a literal IPv4 target and port, the
 * local port the appliance listens on, and the local-network clients that are
 * allowed to use it.  The declaration is bounded on purpose - four clients,
 * exact addresses, tcp only - because it is the whole access rule: a client
 * that is not in the list never reaches the outbound connection, and a
 * declaration that is absent, disabled or malformed opens no port at all.
 *
 * The module owns the grammar, the validation and the admission decision.  It
 * owns no device, no socket and no record: the installer medium, the boot
 * command line and (later) the machine's own menu all produce this value, and
 * the one relay owner consumes it.
 */
#define FORWARD_CONFIG_VERSION 1U
#define FORWARD_CONFIG_CLIENT_MAX 4U
#define FORWARD_CONFIG_ADDRESS_LENGTH 4U
/*
 * The listen port the management serving owner already holds
 * (APPLIANCE_PRODUCTION_SERVING_PORT).  A forward declaration may not claim
 * it: two listeners on one port is not a configuration the machine could
 * honour, so the declaration is refused instead of being accepted and losing
 * a race at bind time.
 */
#define FORWARD_CONFIG_RESERVED_LISTEN_PORT 443U
#define FORWARD_CONFIG_ADDRESS_TEXT_MAX 16U
#define FORWARD_CONFIG_CLIENTS_TEXT_MAX 64U

enum forward_config_status {
    FORWARD_CONFIG_OK = 0,
    FORWARD_CONFIG_NULL = 1,
    FORWARD_CONFIG_BAD_TEXT = 2,
    FORWARD_CONFIG_MISSING_FIELD = 3,
    FORWARD_CONFIG_INVALID_ADDRESS = 4,
    FORWARD_CONFIG_INVALID_PORT = 5,
    FORWARD_CONFIG_TOO_MANY_CLIENTS = 6,
    FORWARD_CONFIG_DUPLICATE_CLIENT = 7,
    FORWARD_CONFIG_RESERVED_PORT = 8,
    FORWARD_CONFIG_VALUES_WITHOUT_DECLARATION = 9,
    FORWARD_CONFIG_VALUES_WITH_DISABLED = 10,
    FORWARD_CONFIG_BUFFER_TOO_SMALL = 11,
    /* The admission answers, so one publish path states one of them. */
    FORWARD_CONFIG_ADMITTED = 12,
    FORWARD_CONFIG_DENIED_NO_DECLARATION = 13,
    FORWARD_CONFIG_DENIED_DISABLED = 14,
    FORWARD_CONFIG_DENIED_NOT_ALLOWED = 15,
};

struct forward_config {
    uint32_t version;
    bool declared;
    bool enabled;
    uint8_t target[FORWARD_CONFIG_ADDRESS_LENGTH];
    uint16_t target_port;
    uint16_t listen_port;
    uint32_t client_count;
    uint8_t clients[FORWARD_CONFIG_CLIENT_MAX]
                   [FORWARD_CONFIG_ADDRESS_LENGTH];
};

/*
 * Compose one declaration from its parts, exactly as the directives carry
 * them.  `declared` is false when nothing declared anything, and then every
 * part has to be empty: a value without a declaration is refused rather than
 * ignored.  `enabled` false is the explicit `off`, which accepts no values at
 * all.  An enabled declaration requires the target, both ports and at least
 * one client, and each part is validated here - the caller never validates a
 * second time.
 */
enum forward_config_status forward_config_from_parts(
    bool declared,
    bool enabled,
    const char *target,
    size_t target_length,
    const char *target_port,
    size_t target_port_length,
    const char *listen_port,
    size_t listen_port_length,
    const char *clients,
    size_t clients_length,
    struct forward_config *config_out);
enum forward_config_status forward_config_validate(
    const struct forward_config *config);
/*
 * The access rule.  A source address is admitted only by name, only when the
 * declaration is present and enabled, and only when it is one of the declared
 * clients; everything else is denied with the reason the record states.
 */
enum forward_config_status forward_config_admit(
    const struct forward_config *config,
    const uint8_t source[FORWARD_CONFIG_ADDRESS_LENGTH]);
/*
 * Render the one-line rule the records quote:
 * `target=A.B.C.D:port listen=port clients=N`.  A declaration that is absent
 * or disabled renders `target=none listen=none clients=0` instead of a rule
 * the machine does not have.
 */
enum forward_config_status forward_config_render(
    const struct forward_config *config,
    char *buffer,
    size_t capacity,
    size_t *length_out);
bool forward_config_enabled(const struct forward_config *config);
const char *forward_config_status_string(enum forward_config_status status);

#endif
