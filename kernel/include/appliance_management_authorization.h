#ifndef HFOC_APPLIANCE_MANAGEMENT_AUTHORIZATION_H
#define HFOC_APPLIANCE_MANAGEMENT_AUTHORIZATION_H

#include "appliance_admin_auth.h"
#include "authorization.h"
#include "http_parser.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_MANAGEMENT_AUTHORIZATION_VERSION 1U
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_DISCOVERY_PATH "/api/v1"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH "/api/v1/system"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH "/api/v1/network"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_STATUS_PATH \
    "/api/v1/network/status"
/*
 * ITEM-505: the network configuration is a configuring section.  The panel
 * form posts to `/panel/network`; the same mutation is reachable at
 * `/api/v1/network` for a script.  Both targets resolve to `NETWORK_CONFIG`
 * when the method is POST, exactly as the certificate and the routes do.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_NETWORK_PATH \
    "/panel/network"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_SERVICE_PATH "/api/v1/service"
/*
 * ITEM-401: the aggregate read-only health target the ITEM-400 owner serves.
 * It carries the same status-read capability as the system status target; it
 * is a read-only alias, not a new capability and never an anonymous target.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_HEALTH_PATH "/api/v1/health"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_UPDATE_PATH "/api/v1/system/update"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_RECOVERY_PATH \
    "/api/v1/system/recovery"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_FACTORY_RESET_PATH \
    "/api/v1/system/factory-reset"
/*
 * ITEM-503: the operator's BYO certificate.  The panel form posts to
 * `/panel/security`; the same executor is reachable at the API target so a
 * script can import a certificate through the same two-phase mutation the
 * panel uses.  Both targets resolve to `CERTIFICATE_IMPORT`.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_CERTIFICATE_PATH \
    "/api/v1/system/certificate"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SECURITY_PATH \
    "/panel/security"
/*
 * ITEM-504: the routing configuration.  The panel form posts to
 * `/panel/routing`; the same mutation is reachable at
 * `/api/v1/system/routes` for a script.  Both targets resolve to
 * `ROUTING_CONFIG`.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTES_PATH \
    "/api/v1/system/routes"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ROUTING_PATH \
    "/panel/routing"
/*
 * ITEM-505 increment 3: the system hostname.  The panel form posts to
 * `/panel/system`; the same mutation is reachable at `/api/v1/system` for a
 * script, whose GET body keeps its read-only meaning.  Both targets resolve to
 * `SYSTEM_HOSTNAME` when the method is POST, exactly as the certificate and
 * the routes do; the read path of `/api/v1/system` keeps the system-status
 * route it has always used.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SYSTEM_PATH "/panel/system"
/*
 * ITEM-506 increment 6: the one account.  The panel form posts to
 * `/panel/account`; the same mutation is reachable at
 * `/api/v1/system/account` for a script.  Both targets resolve to
 * `ACCOUNT_CREDENTIAL` when the method is POST; the panel target's GET is the
 * rendered Account page, and the API target has no read meaning at all.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_ACCOUNT_PATH \
    "/api/v1/system/account"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH \
    "/panel/account"
/*
 * ITEM-507 increment 4: the name -> certificate/key table.  The script target
 * is `/api/v1/system/certificate/name`; increment 5 adds the panel page target
 * `/panel/certificates`.  Both resolve to `CERTIFICATE_NAME` when the method is
 * POST; the panel target's GET is the rendered page, and the API target has no
 * read meaning.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_CERTIFICATE_NAME_PATH \
    "/api/v1/system/certificate/name"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_CERTIFICATES_PATH \
    "/panel/certificates"
/*
 * ITEM-508 increment 34: the durable ACME control.  The panel's ACME control
 * page posts to `/panel/acme`; the same mutation is reachable at
 * `/api/v1/system/acme` for a script.  Both targets resolve to `ACME_CONTROL`
 * when the method is POST; the panel target's GET is the rendered control
 * page, and the API target has no read meaning.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_ACME_PATH "/api/v1/system/acme"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACME_PATH "/panel/acme"
/*
 * ITEM-516: the management gate's after-boot policy.  The panel page posts to
 * `/panel/gate`; the same two-phase mutation is reachable at
 * `/api/v1/system/gate` for a script.  Both targets resolve to `PANEL_GATE`
 * when the method is POST; the panel target's GET is the rendered page, and
 * the API target has no read meaning.  The panel direction can only ever close
 * the gate - opening is the local console's - and an attempt to open is
 * refused by name.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_GATE_PATH "/api/v1/system/gate"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH "/panel/gate"
/*
 * ITEM-509 increment 8: the help index `/panel/help` and its per-topic pages
 * `/panel/help/<id>`.  A topic page reads the same built-in catalog and
 * carries no capability the index does not; the panel resolves an unknown id
 * to a named refusal on the same page, never to a different route.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_HELP_PATH "/panel/help"
/*
 * ITEM-513: the nav's one-click language switch.  It changes only the
 * browser's own cookie, so it needs no capability the section pages do not
 * already have; it is a panel target like them and is authorized through the
 * same `PANEL` route.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_LANGUAGE_PATH \
    "/panel/language"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_PATH "/panel"
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_PATH_LENGTH 6U
/* ITEM-382 about/licenses panel page. */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_LICENSES_PATH \
    "/panel/about/licenses"

/*
 * Mutation command ids. They are deliberately distinct from the read route
 * ids because an authorization entry demands every capability it lists.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_NETWORK_MUTATION 6U
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_SERVICE_MUTATION 7U
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_UPDATE_MUTATION 8U
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_RECOVERY_MUTATION 9U
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_FACTORY_RESET_MUTATION 10U
/*
 * ITEM-503: 12, because 11 is already the read-only panel route id and a
 * duplicated command id makes the matrix reject the whole policy at init.
 */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_CERTIFICATE_MUTATION 12U
/* ITEM-504: 13, the next free mutation command id after ITEM-503's 12. */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ROUTING_MUTATION 13U
/* ITEM-505: 14, the next free mutation command id after ITEM-504's 13. */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_SYSTEM_MUTATION 14U
/* ITEM-506 increment 6: 15, the next free mutation command id after 14. */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ACCOUNT_MUTATION 15U
/* ITEM-507 increment 4: 16, the next free mutation command id after 15. */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_CERTIFICATE_NAME_MUTATION 16U
/* ITEM-508 increment 34: 17, the next free mutation command id after 16. */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ACME_MUTATION 17U
/* ITEM-516: 18, the next free mutation command id after 17. */
#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_GATE_MUTATION 18U

enum appliance_management_authorization_route {
    APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE = 0,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY = 1,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS = 2,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_CONFIG = 3,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_STATUS = 4,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS = 5,
    /*
     * ITEM-384 rendered panel. The id is deliberately distinct from every
     * mutation command id (6..10 and ITEM-503's 12): an authorization entry
     * demands every capability it lists, so the read-only panel entry carries
     * the status-read capability and shares nothing with a mutation entry.
     */
    APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL = 11,
};

/*
 * ITEM-367A mutating request surface.
 *
 * The read path stays strictly GET/HEAD with the status-read capability.
 * Mutations are a separate, narrower surface: only POST/PUT on a known
 * mutation target, only for an authenticated administrator session that
 * carries the configuration-write capability. These enumerations are
 * deliberately independent of `enum http_method_kind`, which the wire policy
 * layer resolves for read-only requests; the mutating composition maps its
 * parsed method into this enumeration explicitly.
 */
enum appliance_management_authorization_method {
    APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NONE = 0,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST = 1,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT = 2,
};

enum appliance_management_authorization_mutation {
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE = 0,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG = 1,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SERVICE_ACTION = 2,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_UPDATE_ACCEPT = 3,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_RECOVERY_ACTION = 4,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_FACTORY_RESET = 5,
    /*
     * ITEM-503: importing an operator certificate and its key (BYO).  It is a
     * distinct mutation because it writes the CERTIFICATE store rather than
     * the network configuration, and it is the only mutation whose body
     * carries a private key.
     */
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT = 6,
    /*
     * ITEM-504: the facade's route table.  It is a distinct mutation because
     * it names the public names the facade serves and the backends behind
     * them, not the network configuration of the machine itself.
     */
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG = 7,
    /*
     * ITEM-505 increment 3: the machine's own hostname.  It is a distinct
     * mutation because it writes the system configuration record whose value
     * the next boot serves as the identity and the certificate name, not the
     * network configuration and not the facade.
     */
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME = 8,
    /*
     * ITEM-506 increment 6: the one account's login and password.  It is a
     * distinct mutation because it writes the administrator credential record
     * and, on success, the owner republishes the verifier and closes every
     * issued session - including the session that confirmed the change.
     */
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL = 9,
    /*
     * ITEM-507 increment 4: the name -> certificate/key table.  It is a
     * distinct mutation because it rewrites the durable record SNI selects
     * from, next to - and without touching - the appliance's own identity
     * certificate.
     */
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME = 10,
    /*
     * ITEM-508 increment 34: the durable ACME control: enable, disable and one
     * pending issuance request.  It is a distinct mutation because it writes
     * the ACME control record the next boot reads - not the certificate, not
     * the name table and not the network configuration - and because "start an
     * issuance" is an operator decision that has to be audited as its own act.
     */
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACME_CONTROL = 11,
    /*
     * ITEM-516: the management gate's after-boot policy.  It is a distinct
     * mutation because it writes the system record the next boot reads as the
     * gate decision - not the network, not the facade and not the identity -
     * and because the panel may only ever close it while the local console
     * owns opening it.
     */
    APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE = 12,
};

enum appliance_management_authorization_status {
    APPLIANCE_MANAGEMENT_AUTHORIZATION_OK = 0,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL = 1,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE = 2,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_VERSION = 3,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_NOT_MANAGEMENT = 4,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE = 5,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED = 6,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED = 7,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_ROLE = 8,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_CAPABILITY = 9,
    APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_AUTHORIZATION = 10,
};

struct appliance_management_authorization_policy {
    uint32_t version;
    bool initialized;
    struct authorization_matrix matrix;
};

enum appliance_management_authorization_status
appliance_management_authorization_policy_init(
    struct appliance_management_authorization_policy *policy);
enum appliance_management_authorization_status
appliance_management_authorization_check_request(
    const struct appliance_management_authorization_policy *policy,
    const struct appliance_admin_auth_decision *decision,
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    enum appliance_management_authorization_route *route_out);
enum appliance_management_authorization_status
appliance_management_authorization_check_mutation_request(
    const struct appliance_management_authorization_policy *policy,
    const struct appliance_admin_auth_decision *decision,
    enum appliance_management_authorization_method method,
    const uint8_t *target,
    uint32_t target_length,
    enum appliance_management_authorization_mutation *mutation_out);
const char *appliance_management_authorization_status_string(
    enum appliance_management_authorization_status status);
const char *appliance_management_authorization_route_string(
    enum appliance_management_authorization_route route);
const char *appliance_management_authorization_mutation_string(
    enum appliance_management_authorization_mutation mutation);
/*
 * True when the mutation has a panel page that renders its two-phase
 * confirmation token.  The serving path uses this one list to decide whether
 * a pending candidate is shown to the operator, so a mutation whose page
 * exists cannot silently lose its confirmation step.
 */
bool appliance_management_authorization_mutation_has_panel_confirmation(
    enum appliance_management_authorization_mutation mutation);

#endif
