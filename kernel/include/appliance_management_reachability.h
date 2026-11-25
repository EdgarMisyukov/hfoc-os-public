#ifndef HFOC_APPLIANCE_MANAGEMENT_REACHABILITY_H
#define HFOC_APPLIANCE_MANAGEMENT_REACHABILITY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-385 management reachability policy.
 *
 * The policy answers one question for every management request: is this peer
 * allowed to reach the management plane at all? Three modes exist:
 *
 *   CONSOLE_ONLY          no network management; the local console still works;
 *   MANAGEMENT_INTERFACE  the declared management interface (and its address,
 *                         or any address when the declared address is zero) is
 *                         admitted; every other interface is denied. A VPN or
 *                         tunnel that terminates on the management interface is
 *                         the recommended remote path and needs no extra mode;
 *   REMOTE_OPT_IN         as above, plus peers listed in the source allowlist
 *                         with, when required, a verified client certificate.
 *
 * The console is evaluated first and is always allowed: it is the break-glass
 * path that must work with no network and even before a policy is published.
 * Nothing here replaces the authorization matrix, the admission policy or the
 * throttling: a reachability verdict only decides whether a request may be
 * evaluated further.
 */

#define APPLIANCE_MANAGEMENT_REACHABILITY_VERSION 1U
#define APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH 4U
#define APPLIANCE_MANAGEMENT_REACHABILITY_ALLOWLIST_MAX 4U
#define APPLIANCE_MANAGEMENT_REACHABILITY_INTERFACE_NONE UINT32_C(0xFFFFFFFF)

/*
 * Declared interface identifiers. The appliance has a single network interface
 * today, so the interface dimension is carried as a declared identifier rather
 * than read from a device registry: zero means "not the management interface"
 * (the outer or an unknown interface) and is never a valid management
 * interface, while INTERFACE_NONE means the operator published a policy that
 * binds no management interface at all, which is valid but DEGRADED.
 *
 * The reachability identifier is deliberately not the interface name string of
 * appliance_network_config/appliance_management_bind: those name the uplink,
 * this identifies the perimeter the management plane is bound to. An empty
 * source allowlist is refused in REMOTE_OPT_IN mode, because
 * appliance_management_bind reads an empty peer list as "any peer on the
 * management interface" while REMOTE_OPT_IN admits peers that are *off* that
 * interface, where only an explicit list is safe.
 */
#define APPLIANCE_MANAGEMENT_REACHABILITY_INTERFACE_OUTER 0U
#define APPLIANCE_MANAGEMENT_REACHABILITY_INTERFACE_MANAGEMENT 1U

enum appliance_management_reachability_mode {
    APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY = 0,
    APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE = 1,
    APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN = 2,
};

enum appliance_management_reachability_readiness {
    APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_UNAVAILABLE = 0,
    APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_DEGRADED = 1,
    APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_READY = 2,
};

enum appliance_management_reachability_verdict {
    APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_CONSOLE_ALLOWED = 0,
    APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_MANAGEMENT_INTERFACE_ALLOWED = 1,
    APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_REMOTE_OPT_IN_ALLOWED = 2,
    APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_UNPUBLISHED = 3,
    APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_CONSOLE_ONLY = 4,
    APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_OUTER_INTERFACE = 5,
    APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NOT_ALLOWLISTED = 6,
    APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NO_CLIENT_CERTIFICATE = 7,
};

enum appliance_management_reachability_status {
    APPLIANCE_MANAGEMENT_REACHABILITY_OK = 0,
    APPLIANCE_MANAGEMENT_REACHABILITY_NULL = 1,
    APPLIANCE_MANAGEMENT_REACHABILITY_BAD_STATE = 2,
    APPLIANCE_MANAGEMENT_REACHABILITY_BAD_VERSION = 3,
    APPLIANCE_MANAGEMENT_REACHABILITY_BAD_MODE = 4,
    APPLIANCE_MANAGEMENT_REACHABILITY_BAD_INTERFACE = 5,
    APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ADDRESS = 6,
    APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST = 7,
    APPLIANCE_MANAGEMENT_REACHABILITY_ALREADY_PUBLISHED = 8,
    APPLIANCE_MANAGEMENT_REACHABILITY_UNPUBLISHED = 9,
};

struct appliance_management_reachability_policy {
    uint32_t version;
    enum appliance_management_reachability_mode mode;
    uint32_t management_interface;
    uint8_t management_address[APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH];
    bool require_client_certificate;
    uint32_t allowlist_count;
    uint8_t allowlist[APPLIANCE_MANAGEMENT_REACHABILITY_ALLOWLIST_MAX]
                     [APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH];
};

struct appliance_management_reachability_selection {
    bool published;
    uint32_t version;
    struct appliance_management_reachability_policy policy;
};

struct appliance_management_reachability_peer {
    bool console;
    bool client_certificate_present;
    uint32_t interface_index;
    uint8_t source_address[APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH];
};

enum appliance_management_reachability_status
appliance_management_reachability_policy_validate(
    const struct appliance_management_reachability_policy *policy);

enum appliance_management_reachability_status
appliance_management_reachability_selection_init(
    struct appliance_management_reachability_selection *selection,
    const struct appliance_management_reachability_policy *policy);

bool appliance_management_reachability_selection_published(
    const struct appliance_management_reachability_selection *selection);

enum appliance_management_reachability_readiness
appliance_management_reachability_readiness(
    const struct appliance_management_reachability_selection *selection);

enum appliance_management_reachability_status
appliance_management_reachability_evaluate(
    const struct appliance_management_reachability_selection *selection,
    const struct appliance_management_reachability_peer *peer,
    enum appliance_management_reachability_verdict *verdict);

bool appliance_management_reachability_verdict_allowed(
    enum appliance_management_reachability_verdict verdict);

const char *appliance_management_reachability_mode_string(
    enum appliance_management_reachability_mode mode);
const char *appliance_management_reachability_verdict_string(
    enum appliance_management_reachability_verdict verdict);
const char *appliance_management_reachability_readiness_string(
    enum appliance_management_reachability_readiness readiness);
const char *appliance_management_reachability_status_string(
    enum appliance_management_reachability_status status);

#endif
